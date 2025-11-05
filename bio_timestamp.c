#include "bio_timestamp.h"
#include <string.h>
#include <errno.h>
#include <unistd.h>

#ifdef __linux__
#include <sys/socket.h>
#include <linux/net_tstamp.h>
#include <linux/errqueue.h>
#include <linux/tls.h>
#include <netinet/tcp.h>

#ifndef TCP_ULP
#define TCP_ULP 31
#endif

// Helper: Safe environment variable parsing (returns 1 if valid "1", 0 otherwise)
static inline int env_is_enabled(const char *value) {
    if (!value) return 0;
    char *endptr;
    long val = strtol(value, &endptr, 10);
    return (*endptr == 0 && val == 1);
}

// Internal structure to store BIO state
typedef struct {
    int fd;                        // Socket file descriptor
    bio_timestamp_t *ts_storage;   // Pointer to shared timestamp storage
    int ktls_tx_enabled;           // kTLS TX enabled flag
    int ktls_rx_enabled;           // kTLS RX enabled flag
} bio_ts_data_t;

// Forward declarations
static int bio_ts_read(BIO *bio, char *buf, int len);
static int bio_ts_write(BIO *bio, const char *buf, int len);
static long bio_ts_ctrl(BIO *bio, int cmd, long num, void *ptr);
static int bio_ts_create(BIO *bio);
static int bio_ts_destroy(BIO *bio);
static void bio_ts_check_ktls(bio_ts_data_t *data);

// BIO method structure (OpenSSL 1.1.0+ API)
#if OPENSSL_VERSION_NUMBER >= 0x10100000L
static BIO_METHOD *bio_ts_method = NULL;

static void bio_ts_init_method(void) {
    if (bio_ts_method != NULL) return;

    bio_ts_method = BIO_meth_new(BIO_TYPE_SOCKET, "timestamp_socket");
    if (bio_ts_method == NULL) return;

    BIO_meth_set_write(bio_ts_method, bio_ts_write);
    BIO_meth_set_read(bio_ts_method, bio_ts_read);
    BIO_meth_set_ctrl(bio_ts_method, bio_ts_ctrl);
    BIO_meth_set_create(bio_ts_method, bio_ts_create);
    BIO_meth_set_destroy(bio_ts_method, bio_ts_destroy);
}
#else
// OpenSSL 1.0.x API (static BIO_METHOD)
static BIO_METHOD bio_ts_method_v1 = {
    BIO_TYPE_SOCKET,
    "timestamp_socket",
    bio_ts_write,
    bio_ts_read,
    NULL,  // puts
    NULL,  // gets
    bio_ts_ctrl,
    bio_ts_create,
    bio_ts_destroy,
    NULL   // callback_ctrl
};
#endif

// Custom BIO read function with hardware timestamp capture
static int bio_ts_read(BIO *bio, char *buf, int len) {
    if (buf == NULL || len <= 0) return 0;

    // Get internal data
    bio_ts_data_t *data = (bio_ts_data_t *)BIO_get_data(bio);
    if (data == NULL || data->fd < 0) {
        return -1;
    }

    // Setup for recvmsg with timestamping
    struct iovec iov;
    iov.iov_base = buf;
    iov.iov_len = len;

    char control[512];  // Buffer for control messages
    struct msghdr msg;
    memset(&msg, 0, sizeof(msg));
    msg.msg_iov = &iov;
    msg.msg_iovlen = 1;
    msg.msg_control = control;
    msg.msg_controllen = sizeof(control);

    // Perform recvmsg to capture timestamp
    // Note: Don't use MSG_DONTWAIT - respect the socket's blocking mode
    // The socket is blocking during handshake and non-blocking during data transfer
    ssize_t bytes_read = recvmsg(data->fd, &msg, 0);

    if (bytes_read < 0) {
        // Handle errors
        if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR) {
            BIO_set_retry_read(bio);
            return -1;
        }
        return -1;  // Real error
    }

    if (bytes_read == 0) {
        return 0;  // Connection closed
    }

    // Parse control messages for hardware timestamp
    if (data->ts_storage != NULL) {
        struct cmsghdr *cmsg;
        for (cmsg = CMSG_FIRSTHDR(&msg); cmsg != NULL; cmsg = CMSG_NXTHDR(&msg, cmsg)) {
            if (cmsg->cmsg_level == SOL_SOCKET && cmsg->cmsg_type == SO_TIMESTAMPING) {
                struct timespec *ts = (struct timespec *)CMSG_DATA(cmsg);

                // Validate control message has space for timestamp array
                size_t cmsg_data_len = cmsg->cmsg_len - CMSG_LEN(0);
                size_t num_timestamps = cmsg_data_len / sizeof(struct timespec);

                // ts[0] = software timestamp
                // ts[1] = (deprecated)
                // ts[2] = hardware timestamp (if available)

                // Prefer hardware timestamp (ts[2])
                if (num_timestamps >= 3 && (ts[2].tv_sec != 0 || ts[2].tv_nsec != 0)) {
                    // Check for overflow (year 2262+)
                    if ((uint64_t)ts[2].tv_sec > (UINT64_MAX / 1000000000ULL)) {
                        data->ts_storage->hw_timestamp_ns = UINT64_MAX;  // Saturate
                    } else {
                        data->ts_storage->hw_timestamp_ns = (uint64_t)ts[2].tv_sec * 1000000000ULL + ts[2].tv_nsec;
                    }
                    data->ts_storage->hw_available = 1;
                } else if (num_timestamps >= 1 && (ts[0].tv_sec != 0 || ts[0].tv_nsec != 0)) {
                    // Fallback to software timestamp
                    if ((uint64_t)ts[0].tv_sec > (UINT64_MAX / 1000000000ULL)) {
                        data->ts_storage->hw_timestamp_ns = UINT64_MAX;  // Saturate
                    } else {
                        data->ts_storage->hw_timestamp_ns = (uint64_t)ts[0].tv_sec * 1000000000ULL + ts[0].tv_nsec;
                    }
                    data->ts_storage->hw_available = 0;
                }
            }
        }
    }

    return (int)bytes_read;
}

// Custom BIO write function (standard socket write)
static int bio_ts_write(BIO *bio, const char *buf, int len) {
    if (buf == NULL || len <= 0) return 0;

    bio_ts_data_t *data = (bio_ts_data_t *)BIO_get_data(bio);
    if (data == NULL || data->fd < 0) {
        return -1;
    }

    ssize_t bytes_written = write(data->fd, buf, len);

    if (bytes_written < 0) {
        if (errno == EAGAIN || errno == EWOULDBLOCK) {
            BIO_set_retry_write(bio);
            return -1;
        }
        return -1;
    }

    return (int)bytes_written;
}

// Check if kTLS is enabled on the socket
static void bio_ts_check_ktls(bio_ts_data_t *data) {
    if (data == NULL || data->fd < 0) return;

    // Reset flags before checking
    data->ktls_tx_enabled = 0;
    data->ktls_rx_enabled = 0;

    // Check if TCP ULP (Upper Layer Protocol) is set to "tls"
    // This indicates kTLS is enabled
    char ulp_name[16] = {0};
    socklen_t optlen = sizeof(ulp_name);

    // Try to get the ULP name - if it returns "tls", kTLS is enabled
    if (getsockopt(data->fd, SOL_TCP, TCP_ULP, ulp_name, &optlen) == 0) {
        const char *debug = getenv("WS_DEBUG_KTLS");
        if (env_is_enabled(debug)) {
            fprintf(stderr, "[BIO kTLS Debug] ULP name: '%s'\n", ulp_name);
        }

        if (strcmp(ulp_name, "tls") == 0) {
            // kTLS is enabled - check both TX and RX
            // If we can read the TLS_TX/RX crypto info, it's enabled
            struct tls12_crypto_info_aes_gcm_128 crypto_info;
            socklen_t crypto_len = sizeof(crypto_info);

            // Check TX
            if (getsockopt(data->fd, SOL_TLS, TLS_TX, &crypto_info, &crypto_len) == 0) {
                data->ktls_tx_enabled = 1;
                if (env_is_enabled(debug)) {
                    fprintf(stderr, "[BIO kTLS Debug] TX enabled\n");
                }
            }

            // Check RX
            crypto_len = sizeof(crypto_info);
            if (getsockopt(data->fd, SOL_TLS, TLS_RX, &crypto_info, &crypto_len) == 0) {
                data->ktls_rx_enabled = 1;
                if (env_is_enabled(debug)) {
                    fprintf(stderr, "[BIO kTLS Debug] RX enabled\n");
                }
            }
        }
    } else {
        const char *debug = getenv("WS_DEBUG_KTLS");
        if (env_is_enabled(debug)) {
            fprintf(stderr, "[BIO kTLS Debug] getsockopt TCP_ULP failed: %s\n", strerror(errno));
        }
    }
}

// BIO control function
static long bio_ts_ctrl(BIO *bio, int cmd, long num, void *ptr) {
    bio_ts_data_t *data = (bio_ts_data_t *)BIO_get_data(bio);
    long ret = 1;

    switch (cmd) {
        case BIO_CTRL_RESET:
            ret = 0;
            break;
        case BIO_CTRL_EOF:
            ret = 0;
            break;
        case BIO_CTRL_GET_CLOSE:
            ret = BIO_get_shutdown(bio);
            break;
        case BIO_CTRL_SET_CLOSE:
            BIO_set_shutdown(bio, (int)num);
            ret = 1;
            break;
        case BIO_CTRL_PENDING:
        case BIO_CTRL_WPENDING:
            ret = 0;
            break;
        case BIO_CTRL_DUP:
        case BIO_CTRL_FLUSH:
            ret = 1;
            break;
        case BIO_C_SET_FD:
            if (data != NULL) {
                data->fd = *((int *)ptr);
                BIO_set_init(bio, 1);
                ret = 1;
            }
            break;
        case BIO_C_GET_FD:
            if (data != NULL && data->fd >= 0) {
                if (ptr != NULL) {
                    *((int *)ptr) = data->fd;
                }
                ret = data->fd;
            } else {
                ret = -1;
            }
            break;
#if OPENSSL_VERSION_NUMBER >= 0x30000000L
        case BIO_CTRL_GET_KTLS_SEND:
            // Check kTLS status and return current state
            if (data != NULL) {
                bio_ts_check_ktls(data);
                ret = data->ktls_tx_enabled;
            } else {
                ret = 0;
            }
            break;
        case BIO_CTRL_GET_KTLS_RECV:
            // Check kTLS status and return current state
            if (data != NULL) {
                bio_ts_check_ktls(data);
                ret = data->ktls_rx_enabled;
            } else {
                ret = 0;
            }
            break;
#endif
        default:
            ret = 0;
            break;
    }

    return ret;
}

// BIO create function
static int bio_ts_create(BIO *bio) {
    BIO_set_init(bio, 0);
    BIO_set_data(bio, NULL);
    BIO_set_flags(bio, 0);
    return 1;
}

// BIO destroy function
static int bio_ts_destroy(BIO *bio) {
    if (bio == NULL) return 0;

    bio_ts_data_t *data = (bio_ts_data_t *)BIO_get_data(bio);
    if (data != NULL) {
        // Close socket if BIO owns it
        if (BIO_get_shutdown(bio) && data->fd >= 0) {
            close(data->fd);
        }
        free(data);
        BIO_set_data(bio, NULL);
    }

    return 1;
}

// Create a new BIO for timestamp-aware socket
BIO* BIO_new_timestamp_socket(int fd, bio_timestamp_t *ts_storage) {
    if (fd < 0) return NULL;

#if OPENSSL_VERSION_NUMBER >= 0x10100000L
    // OpenSSL 1.1.0+ API
    bio_ts_init_method();
    if (bio_ts_method == NULL) return NULL;

    BIO *bio = BIO_new(bio_ts_method);
#else
    // OpenSSL 1.0.x API
    BIO *bio = BIO_new(&bio_ts_method_v1);
#endif

    if (bio == NULL) return NULL;

    // Allocate internal data structure
    bio_ts_data_t *data = (bio_ts_data_t *)malloc(sizeof(bio_ts_data_t));
    if (data == NULL) {
        BIO_free(bio);
        return NULL;
    }

    data->fd = fd;
    data->ts_storage = ts_storage;
    data->ktls_tx_enabled = 0;  // Will be set by bio_ts_check_ktls()
    data->ktls_rx_enabled = 0;  // Will be set by bio_ts_check_ktls()

    BIO_set_data(bio, data);
    BIO_set_init(bio, 1);
    BIO_set_shutdown(bio, 0);  // Don't close FD when BIO is freed (caller owns it)

    return bio;
}

#endif // __linux__
