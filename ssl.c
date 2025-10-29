#include "ssl.h"
#include <openssl/ssl.h>
#include <openssl/err.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <netdb.h>
#include <unistd.h>
#include <fcntl.h>
#include <string.h>
#include <stdlib.h>

// Linux hardware timestamping support
#ifdef __linux__
#include <linux/net_tstamp.h>
#include <linux/errqueue.h>
#define HW_TIMESTAMPING_SUPPORTED 1
#endif

// Static global context for thread-unsafe but fast operations
static SSL_CTX *global_ctx = NULL;
static int ssl_initialized = 0;

// Initialize OpenSSL library only once (called on first connection)
static void ssl_init_once(void) {
    if (ssl_initialized) return;
    
    // Initialize OpenSSL only once
    SSL_library_init();
    OpenSSL_add_all_algorithms();
    // Skip SSL_load_error_strings() - we don't need error messages for zero latency
    
    // Create shared SSL_CTX for all connections
    global_ctx = SSL_CTX_new(TLS_client_method());
    if (!global_ctx) return;
    
    // Disable certificate verification for minimal latency
    SSL_CTX_set_verify(global_ctx, SSL_VERIFY_NONE, NULL);
    SSL_CTX_set_verify_depth(global_ctx, 0);
    
    // Disable session caching for deterministic performance
    SSL_CTX_set_session_cache_mode(global_ctx, SSL_SESS_CACHE_OFF);
    
    // Disable session tickets for slightly faster handshake
    SSL_CTX_set_options(global_ctx, SSL_OP_NO_TICKET);
    
    // Skip renegotiation time limit
    SSL_CTX_set_options(global_ctx, SSL_OP_ALLOW_UNSAFE_LEGACY_RENEGOTIATION);
    
    ssl_initialized = 1;
}

struct ssl_context {
    SSL *ssl;
    int sockfd;
    int port;
    char *hostname;
    // Sentinel for double-free detection (simple check, no overhead)
    uint32_t magic;
    // Hardware timestamping enabled flag (Linux only)
    int hw_timestamping_enabled;
};

#define SSL_CONTEXT_MAGIC 0x53534C00  // "SSL\0" in little-endian

ssl_context_t *ssl_init(const char *hostname, int port) {
    // Initialize OpenSSL library if not already done
    ssl_init_once();
    if (!global_ctx) return NULL;
    
    // Check for NULL parameters
    if (!hostname || port < 0 || port > 65535) return NULL;
    
    ssl_context_t *sctx = (ssl_context_t *)malloc(sizeof(ssl_context_t));
    if (!sctx) return NULL;
    
    sctx->hostname = strdup(hostname);
    sctx->port = port;
    sctx->ssl = NULL;
    sctx->sockfd = -1;
    sctx->magic = SSL_CONTEXT_MAGIC;  // Set magic value
    sctx->hw_timestamping_enabled = 0;  // Will be set if successful

    // Create socket
    struct hostent *server = gethostbyname(hostname);
    if (!server) {
        free(sctx->hostname);
        free(sctx);
        return NULL;
    }

    sctx->sockfd = socket(AF_INET, SOCK_STREAM, 0);
    if (sctx->sockfd < 0) {
        free(sctx->hostname);
        free(sctx);
        return NULL;
    }

    // Set non-blocking mode
    int flags = fcntl(sctx->sockfd, F_GETFL, 0);
    fcntl(sctx->sockfd, F_SETFL, flags | O_NONBLOCK);

#ifdef HW_TIMESTAMPING_SUPPORTED
    // Enable hardware timestamping on Linux
    // Request: hardware RX timestamps, software fallback, raw hardware timestamps
    int timestamping_flags = SOF_TIMESTAMPING_RX_HARDWARE |
                             SOF_TIMESTAMPING_RX_SOFTWARE |
                             SOF_TIMESTAMPING_SOFTWARE |
                             SOF_TIMESTAMPING_RAW_HARDWARE;

    if (setsockopt(sctx->sockfd, SOL_SOCKET, SO_TIMESTAMPING,
                   &timestamping_flags, sizeof(timestamping_flags)) == 0) {
        sctx->hw_timestamping_enabled = 1;
    }
    // If setsockopt fails, we continue without timestamping (graceful degradation)
#endif
    
    struct sockaddr_in serv_addr;
    memset(&serv_addr, 0, sizeof(serv_addr));
    serv_addr.sin_family = AF_INET;
    memcpy(&serv_addr.sin_addr.s_addr, server->h_addr, server->h_length);
    serv_addr.sin_port = htons(port);
    
    if (connect(sctx->sockfd, (struct sockaddr *)&serv_addr, sizeof(serv_addr)) < 0) {
        // Connection may be pending (non-blocking)
    }
    
    return sctx;
}

void ssl_free(ssl_context_t *sctx) {
    if (!sctx) return;
    
    // Check for double-free or invalid pointer
    if (sctx->magic != SSL_CONTEXT_MAGIC) return;
    
    // Clear magic to prevent reuse
    sctx->magic = 0;
    
    if (sctx->ssl) {
        SSL_shutdown(sctx->ssl);
        SSL_free(sctx->ssl);
    }
    
    if (sctx->sockfd >= 0) {
        close(sctx->sockfd);
    }
    
    if (sctx->hostname) {
        free(sctx->hostname);
    }
    
    free(sctx);
    // Note: We don't free global_ctx as it's shared across all connections
}

int ssl_handshake(ssl_context_t *sctx) {
    if (!sctx) return -1;
    
    if (sctx->ssl) return 1;  // Already connected
    
    sctx->ssl = SSL_new(global_ctx);
    if (!sctx->ssl) return -1;
    
    SSL_set_fd(sctx->ssl, sctx->sockfd);
    
    // Check if socket is connected
    int optval;
    socklen_t optlen = sizeof(optval);
    if (getsockopt(sctx->sockfd, SOL_SOCKET, SO_ERROR, &optval, &optlen) == 0) {
        if (optval != 0) return -1;  // Connection failed
    }
    
    int ret = SSL_connect(sctx->ssl);
    if (ret <= 0) {
        int err = SSL_get_error(sctx->ssl, ret);
        if (err == SSL_ERROR_WANT_READ || err == SSL_ERROR_WANT_WRITE) {
            return 0;  // Handshake in progress
        }
        return -1;  // Handshake failed
    }
    
    return 1;  // Handshake completed successfully
}

int ssl_send(ssl_context_t *sctx, const uint8_t *data, size_t len) {
    if (!sctx || !sctx->ssl) return -1;
    
    int result = SSL_write(sctx->ssl, data, len);
    if (result <= 0) {
        int err = SSL_get_error(sctx->ssl, result);
        if (err == SSL_ERROR_WANT_WRITE || err == SSL_ERROR_WANT_READ) {
            return 0; // Would block, not an error
        }
        return -1;
    }
    
    return result;
}

int ssl_recv(ssl_context_t *sctx, uint8_t *data, size_t len) {
    if (!sctx || !sctx->ssl) return -1;
    
    return SSL_read(sctx->ssl, data, len);
}

int ssl_get_fd(ssl_context_t *sctx) {
    if (!sctx) return -1;
    return sctx->sockfd;
}

int ssl_set_fd(ssl_context_t *sctx, int fd) {
    if (!sctx) return -1;
    sctx->sockfd = fd;
    return 0;
}

int ssl_pending(ssl_context_t *sctx) {
    if (!sctx || !sctx->ssl) return 0;
    return SSL_pending(sctx->ssl);
}

int ssl_get_error_code(ssl_context_t *sctx, int ret) {
    if (!sctx || !sctx->ssl) return 0;
    return SSL_get_error(sctx->ssl, ret);
}

int ssl_read_into(ssl_context_t *sctx, uint8_t *buf, size_t len) {
    if (!sctx || !sctx->ssl) return -1;
    return SSL_read(sctx->ssl, buf, len);
}

void *ssl_get_handle(ssl_context_t *sctx) {
    if (!sctx) return NULL;
    return (void *)sctx->ssl;
}

int ssl_hw_timestamping_enabled(ssl_context_t *sctx) {
    if (!sctx) return 0;
    return sctx->hw_timestamping_enabled;
}

#ifdef HW_TIMESTAMPING_SUPPORTED
// Try to extract hardware timestamp from socket error queue (Linux only)
// Returns timestamp in nanoseconds, or 0 if not available
static uint64_t try_get_hw_timestamp(int sockfd) {
    char control[512];
    struct msghdr msg;
    struct iovec iov;
    char data[1];

    // Setup message structure for recvmsg
    memset(&msg, 0, sizeof(msg));
    iov.iov_base = data;
    iov.iov_len = sizeof(data);
    msg.msg_iov = &iov;
    msg.msg_iovlen = 1;
    msg.msg_control = control;
    msg.msg_controllen = sizeof(control);

    // Try to read from error queue (non-blocking)
    if (recvmsg(sockfd, &msg, MSG_ERRQUEUE | MSG_DONTWAIT) < 0) {
        return 0;  // No timestamp available
    }

    // Parse control messages for timestamps
    struct cmsghdr *cmsg;
    for (cmsg = CMSG_FIRSTHDR(&msg); cmsg; cmsg = CMSG_NXTHDR(&msg, cmsg)) {
        if (cmsg->cmsg_level == SOL_SOCKET && cmsg->cmsg_type == SO_TIMESTAMPING) {
            struct timespec *ts = (struct timespec *)CMSG_DATA(cmsg);
            // ts[0] = software timestamp
            // ts[1] = (deprecated)
            // ts[2] = hardware timestamp (if available)

            // Try hardware timestamp first (ts[2])
            if (ts[2].tv_sec != 0 || ts[2].tv_nsec != 0) {
                return (uint64_t)ts[2].tv_sec * 1000000000ULL + ts[2].tv_nsec;
            }
            // Fall back to software timestamp (ts[0])
            if (ts[0].tv_sec != 0 || ts[0].tv_nsec != 0) {
                return (uint64_t)ts[0].tv_sec * 1000000000ULL + ts[0].tv_nsec;
            }
        }
    }

    return 0;
}
#endif

// Get latest hardware timestamp if available (Linux only)
uint64_t ssl_get_hw_timestamp(ssl_context_t *sctx) {
    if (!sctx || !sctx->hw_timestamping_enabled) return 0;

#ifdef HW_TIMESTAMPING_SUPPORTED
    return try_get_hw_timestamp(sctx->sockfd);
#else
    return 0;
#endif
}
