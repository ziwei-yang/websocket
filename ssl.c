#include "ssl.h"
#include "ssl_backend.h"
#include <openssl/ssl.h>
#include <openssl/err.h>
#include <sys/socket.h>
#include <sys/select.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <arpa/inet.h>
#include <netdb.h>
#include <unistd.h>
#include <fcntl.h>
#include <string.h>
#include <stdlib.h>
#include <errno.h>

// Linux hardware timestamping support
#ifdef __linux__
#include <linux/net_tstamp.h>
#include <linux/errqueue.h>
#define HW_TIMESTAMPING_SUPPORTED 1
// Linux kTLS support (requires kernel 4.13+ for RX, 4.17+ for TX)
#include <linux/tls.h>
#ifndef TCP_ULP
#define TCP_ULP 31
#endif
#define KTLS_SUPPORTED 1
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
    // kTLS enabled flag (Linux only, offloads crypto to kernel)
    int ktls_enabled;
    int ktls_checked;  // Flag to track if kTLS has been checked
};

#define SSL_CONTEXT_MAGIC 0x53534C00  // "SSL\0" in little-endian

ssl_context_t *ssl_init(const char *hostname, int port) {
    // Initialize OpenSSL library if not already done
    ssl_init_once();
    if (!global_ctx) return NULL;

    // Check for NULL parameters and valid port range
    if (!hostname || port <= 0 || port > 65535) return NULL;

    // Use getaddrinfo() instead of deprecated gethostbyname()
    // Resolve hostname BEFORE allocating context to avoid leaks on failure
    struct addrinfo hints, *result = NULL;
    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;

    char port_str[8];
    snprintf(port_str, sizeof(port_str), "%d", port);

    if (getaddrinfo(hostname, port_str, &hints, &result) != 0) {
        return NULL;
    }

    // Allocate context AFTER successful hostname resolution
    ssl_context_t *sctx = (ssl_context_t *)malloc(sizeof(ssl_context_t));
    if (!sctx) {
        freeaddrinfo(result);
        return NULL;
    }

    // Check strdup() return value
    sctx->hostname = strdup(hostname);
    if (!sctx->hostname) {
        free(sctx);
        freeaddrinfo(result);
        return NULL;
    }

    sctx->port = port;
    sctx->ssl = NULL;
    sctx->sockfd = -1;
    sctx->magic = SSL_CONTEXT_MAGIC;  // Set magic value
    sctx->hw_timestamping_enabled = 0;  // Will be set if successful
    sctx->ktls_enabled = 0;  // Will be set if kTLS is enabled
    sctx->ktls_checked = 0;  // Will be set to 1 after kTLS detection completes

    // Create socket
    sctx->sockfd = socket(AF_INET, SOCK_STREAM, 0);
    if (sctx->sockfd < 0) {
        free(sctx->hostname);
        free(sctx);
        freeaddrinfo(result);
        return NULL;
    }

    // NOTE: Keep socket in blocking mode during TLS handshake for kTLS activation
    // We'll switch to non-blocking mode after handshake completes successfully
    // (OpenSSL's kTLS has issues activating on non-blocking sockets)

    // Optimize socket buffer sizes for low latency
    // Larger buffers reduce the chance of drops but may increase latency
    // For HFT, we prefer smaller buffers with faster processing
    int rcvbuf_size = 256 * 1024;  // 256KB receive buffer
    int sndbuf_size = 256 * 1024;  // 256KB send buffer

    // Note: Buffer size failures are non-critical (kernel may choose different sizes)
    if (setsockopt(sctx->sockfd, SOL_SOCKET, SO_RCVBUF, &rcvbuf_size, sizeof(rcvbuf_size)) != 0) {
        fprintf(stderr, "Warning: Failed to set SO_RCVBUF: %s\n", strerror(errno));
    }
    if (setsockopt(sctx->sockfd, SOL_SOCKET, SO_SNDBUF, &sndbuf_size, sizeof(sndbuf_size)) != 0) {
        fprintf(stderr, "Warning: Failed to set SO_SNDBUF: %s\n", strerror(errno));
    }

    // Check return values from critical setsockopt() calls
    // Enable TCP_NODELAY to disable Nagle's algorithm (reduce latency)
    int nodelay = 1;
    if (setsockopt(sctx->sockfd, IPPROTO_TCP, TCP_NODELAY, &nodelay, sizeof(nodelay)) != 0) {
        fprintf(stderr, "Warning: Failed to set TCP_NODELAY: %s\n", strerror(errno));
    }

    // Enable SO_KEEPALIVE to detect dead connections
    int keepalive = 1;
    if (setsockopt(sctx->sockfd, SOL_SOCKET, SO_KEEPALIVE, &keepalive, sizeof(keepalive)) != 0) {
        fprintf(stderr, "Warning: Failed to set SO_KEEPALIVE: %s\n", strerror(errno));
    }

#ifdef __APPLE__
    // macOS-specific socket optimizations
    // Set TCP_NOOPT to disable TCP options processing for lower latency
    int tcp_noopt = 1;
    if (setsockopt(sctx->sockfd, IPPROTO_TCP, TCP_NOOPT, &tcp_noopt, sizeof(tcp_noopt)) != 0) {
        fprintf(stderr, "Warning: Failed to set TCP_NOOPT: %s\n", strerror(errno));
    }

    // Set SO_NOSIGPIPE to prevent SIGPIPE on broken connections
    int nosigpipe = 1;
    if (setsockopt(sctx->sockfd, SOL_SOCKET, SO_NOSIGPIPE, &nosigpipe, sizeof(nosigpipe)) != 0) {
        fprintf(stderr, "Warning: Failed to set SO_NOSIGPIPE: %s\n", strerror(errno));
    }
#endif

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

    // Use non-blocking connect with timeout to avoid stalling application
    // (kTLS requires blocking mode, but we can temporarily switch for connect)

    // Set socket to non-blocking for connect with timeout
    int flags = fcntl(sctx->sockfd, F_GETFL, 0);
    if (flags < 0 || fcntl(sctx->sockfd, F_SETFL, flags | O_NONBLOCK) < 0) {
        close(sctx->sockfd);
        free(sctx->hostname);
        free(sctx);
        freeaddrinfo(result);
        return NULL;
    }

    // Attempt non-blocking connect
    int connect_result = connect(sctx->sockfd, result->ai_addr, result->ai_addrlen);

    if (connect_result < 0) {
        if (errno == EINPROGRESS) {
            // Connection in progress - wait with timeout (5 seconds for HFT)
            fd_set write_fds;
            struct timeval timeout;
            timeout.tv_sec = 5;
            timeout.tv_usec = 0;

            FD_ZERO(&write_fds);
            FD_SET(sctx->sockfd, &write_fds);

            int select_result = select(sctx->sockfd + 1, NULL, &write_fds, NULL, &timeout);

            if (select_result <= 0) {
                // Timeout or error
                close(sctx->sockfd);
                free(sctx->hostname);
                free(sctx);
                freeaddrinfo(result);
                return NULL;
            }

            // Check if connection succeeded
            int so_error;
            socklen_t len = sizeof(so_error);
            if (getsockopt(sctx->sockfd, SOL_SOCKET, SO_ERROR, &so_error, &len) < 0 || so_error != 0) {
                // Connection failed
                close(sctx->sockfd);
                free(sctx->hostname);
                free(sctx);
                freeaddrinfo(result);
                return NULL;
            }
        } else {
            // Immediate connection failure
            close(sctx->sockfd);
            free(sctx->hostname);
            free(sctx);
            freeaddrinfo(result);
            return NULL;
        }
    }

    // Restore blocking mode for kTLS activation during TLS handshake
    // (OpenSSL's kTLS has issues activating on non-blocking sockets)
    if (fcntl(sctx->sockfd, F_SETFL, flags) < 0) {
        close(sctx->sockfd);
        free(sctx->hostname);
        free(sctx);
        freeaddrinfo(result);
        return NULL;
    }

    freeaddrinfo(result);
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

    // If handshake already complete and kTLS already checked, return success
    if (sctx->ssl && sctx->ktls_checked) {
        return 1;  // Already connected and kTLS checked
    }

    // If SSL object doesn't exist yet, create it
    if (!sctx->ssl) {
        sctx->ssl = SSL_new(global_ctx);
        if (!sctx->ssl) return -1;

        SSL_set_fd(sctx->ssl, sctx->sockfd);

        // Set SNI (Server Name Indication) - critical for CDN-backed endpoints
        if (SSL_set_tlsext_host_name(sctx->ssl, sctx->hostname) != 1) {
            fprintf(stderr, "Warning: failed to set SNI for %s\n", sctx->hostname);
        }

        #if defined(SSL_BACKEND_KTLS) && defined(KTLS_SUPPORTED)
        // For kTLS: Default to TLS 1.2 (production-ready kTLS)
        // Set WS_FORCE_TLS13=1 to override and use TLS 1.3 (disables kTLS)
        const char *force_tls13 = getenv("WS_FORCE_TLS13");
        if (force_tls13 && atoi(force_tls13) == 1) {
            // Override: Force TLS 1.3 only (disables kTLS, uses userspace OpenSSL)
            SSL_set_min_proto_version(sctx->ssl, TLS1_3_VERSION);
            SSL_set_max_proto_version(sctx->ssl, TLS1_3_VERSION);
        } else {
            // Default: Force TLS 1.2 for kTLS support (kernel offload, best HFT performance)
            SSL_set_min_proto_version(sctx->ssl, TLS1_2_VERSION);
            SSL_set_max_proto_version(sctx->ssl, TLS1_2_VERSION);
        }

        // Set TLS 1.3 cipher suites (kTLS-compatible: AES-GCM and ChaCha20-Poly1305)
        const char *tls13_ciphersuites = getenv("WS_TLS13_CIPHERSUITES");
        if (!tls13_ciphersuites) {
            // Default: Prioritize AES-GCM (best kTLS support)
            tls13_ciphersuites = "TLS_AES_128_GCM_SHA256:"
                                 "TLS_AES_256_GCM_SHA384:"
                                 "TLS_CHACHA20_POLY1305_SHA256";
        }
        if (SSL_set_ciphersuites(sctx->ssl, tls13_ciphersuites) != 1) {
            fprintf(stderr, "Warning: Failed to set TLS 1.3 ciphersuites: %s\n", tls13_ciphersuites);
        }
        #endif

        // Force AES-GCM cipher suites for TLS 1.2 (configurable via WS_CIPHER_LIST)
        const char *cipher_list = getenv("WS_CIPHER_LIST");
        if (!cipher_list) {
            // Default: Prioritize AES-GCM (hardware accelerated) over ChaCha20-Poly1305
            cipher_list = "ECDHE-RSA-AES128-GCM-SHA256:"
                          "ECDHE-RSA-AES256-GCM-SHA384:"
                          "ECDHE-RSA-CHACHA20-POLY1305:"
                          "AES128-GCM-SHA256:"
                          "AES256-GCM-SHA384";
        }
        if (SSL_set_cipher_list(sctx->ssl, cipher_list) != 1) {
            fprintf(stderr, "Warning: Failed to set cipher list: %s\n", cipher_list);
        }

        #if defined(SSL_BACKEND_KTLS) && defined(KTLS_SUPPORTED)
        // Enable kTLS before handshake (OpenSSL will set it up automatically if supported)
        #ifdef SSL_OP_ENABLE_KTLS
        SSL_set_options(sctx->ssl, SSL_OP_ENABLE_KTLS);
        #endif
        #endif

        // Check if socket is connected (only check on first SSL creation)
        int optval;
        socklen_t optlen = sizeof(optval);
        if (getsockopt(sctx->sockfd, SOL_SOCKET, SO_ERROR, &optval, &optlen) == 0) {
            if (optval != 0) return -1;  // Connection failed
        }
    }  // End of if (!sctx->ssl) block
    
    int ret = SSL_connect(sctx->ssl);
    if (ret <= 0) {
        int err = SSL_get_error(sctx->ssl, ret);
        if (err == SSL_ERROR_WANT_READ || err == SSL_ERROR_WANT_WRITE) {
            return 0;  // Handshake in progress
        }
        return -1;  // Handshake failed
    }

#if defined(SSL_BACKEND_KTLS) && defined(KTLS_SUPPORTED)
    // Enable Kernel TLS after successful handshake (Linux only)
    // OpenSSL 3.0+ and 1.1.1+ have built-in kTLS support
    // Check if kTLS was successfully enabled by OpenSSL

    #if OPENSSL_VERSION_NUMBER >= 0x30000000L
    // OpenSSL 3.0+ API
    BIO *wbio = SSL_get_wbio(sctx->ssl);
    BIO *rbio = SSL_get_rbio(sctx->ssl);
    int send_ktls = BIO_get_ktls_send(wbio);
    int recv_ktls = BIO_get_ktls_recv(rbio);

    const char *debug_ktls = getenv("WS_DEBUG_KTLS");
    if (debug_ktls && atoi(debug_ktls) == 1) {
        fprintf(stderr, "[kTLS Debug] send_ktls=%d, recv_ktls=%d\n", send_ktls, recv_ktls);
    }

    if (send_ktls && recv_ktls) {
        sctx->ktls_enabled = 1;
    }
    #elif OPENSSL_VERSION_NUMBER >= 0x10101000L
    // OpenSSL 1.1.1+ API (BIO_get_ktls_send/recv may not be available)
    // Try to check if kTLS is enabled via socket options
    int ulp_enabled = 0;
    socklen_t optlen = sizeof(ulp_enabled);
    if (getsockopt(sctx->sockfd, SOL_TCP, TCP_ULP, &ulp_enabled, &optlen) == 0) {
        sctx->ktls_enabled = 1;
    }
    #endif

    // If kTLS is not enabled, we continue with userspace OpenSSL (graceful fallback)
#endif

    // Mark that handshake is complete (prevents redundant SSL_connect calls)
    // MUST be outside #if block to work for all SSL backends
    sctx->ktls_checked = 1;

    // NOW switch to non-blocking mode after successful handshake
    // (kTLS needs blocking mode during handshake to activate properly)
    int flags = fcntl(sctx->sockfd, F_GETFL, 0);
    if (flags >= 0) {
        if (fcntl(sctx->sockfd, F_SETFL, flags | O_NONBLOCK) < 0) {
            fprintf(stderr, "Warning: Failed to set O_NONBLOCK: %s\n", strerror(errno));
        }
    }

    return 1;  // Handshake completed successfully
}

int ssl_send(ssl_context_t *sctx, const uint8_t *data, size_t len) {
    if (__builtin_expect(!sctx || !sctx->ssl, 0)) return -1;  // Unlikely: validation

    // SSL_write takes int, check for overflow
    if (len > INT_MAX) {
        len = INT_MAX;
    }

    int result = SSL_write(sctx->ssl, data, (int)len);
    if (__builtin_expect(result <= 0, 0)) {  // Unlikely: error path
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

    // SSL_read takes int, check for overflow (matches ssl_send)
    if (len > INT_MAX) {
        len = INT_MAX;
    }

    return SSL_read(sctx->ssl, data, (int)len);
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

// Get backend name at runtime
const char *ssl_get_backend_name(void) {
    return SSL_BACKEND_NAME;
}

// Check if Kernel TLS is enabled
int ssl_ktls_enabled(ssl_context_t *sctx) {
    if (!sctx) return 0;
    return sctx->ktls_enabled;
}

// Get TLS processing mode (kernel or userspace)
// Returns descriptive string indicating whether kTLS is active
const char* ssl_get_tls_mode(ssl_context_t *sctx) {
    if (!sctx) return "Unknown";

#ifdef KTLS_SUPPORTED
    if (sctx->ktls_enabled) {
        return "kTLS (Kernel)";
    }
#endif
    return "OpenSSL (Userspace)";
}

// Get negotiated cipher suite name
const char* ssl_get_cipher_name(ssl_context_t *sctx) {
    if (!sctx || !sctx->ssl) return NULL;

    const SSL_CIPHER *cipher = SSL_get_current_cipher(sctx->ssl);
    if (!cipher) return NULL;

    return SSL_CIPHER_get_name(cipher);
}

// Check if hardware cryptography acceleration is available
int ssl_has_hw_crypto(void) {
#if defined(__x86_64__) || defined(__i386__)
    // x86/x64: Check for AES-NI via CPUID
    #if defined(__GNUC__) || defined(__clang__)
    unsigned int eax, ebx, ecx, edx;

    // CPUID function 1: Processor Info and Feature Bits
    __asm__ __volatile__(
        "cpuid"
        : "=a" (eax), "=b" (ebx), "=c" (ecx), "=d" (edx)
        : "a" (1), "c" (0)
    );

    // Check ECX bit 25 for AES-NI support
    return (ecx & (1 << 25)) != 0;
    #else
    return 0;  // Compiler doesn't support inline assembly
    #endif

#elif defined(__aarch64__) || defined(__arm64__)
    // ARM64: Check for ARM Crypto Extensions at compile time
    #ifdef __ARM_FEATURE_CRYPTO
    return 1;  // ARM Crypto Extensions available
    #else
    return 0;  // No ARM Crypto Extensions
    #endif

#else
    // Other architectures: assume no hardware crypto
    return 0;
#endif
}

// Get SSL backend version string
const char* ssl_get_backend_version(void) {
#if defined(SSL_BACKEND_LIBRESSL)
    return "LibreSSL " LIBRESSL_VERSION_TEXT;
#elif defined(SSL_BACKEND_BORINGSSL)
    return "BoringSSL";
#elif defined(SSL_BACKEND_OPENSSL)
    return "OpenSSL " OPENSSL_VERSION_TEXT;
#else
    return "Unknown SSL Backend";
#endif
}
