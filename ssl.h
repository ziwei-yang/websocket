#ifndef SSL_H
#define SSL_H

#include <stdint.h>
#include <stddef.h>

typedef struct ssl_context ssl_context_t;

// Initialize SSL context with hostname and port
ssl_context_t *ssl_init(const char *hostname, int port);

// Free SSL context
void ssl_free(ssl_context_t *ctx);

// Perform SSL handshake
int ssl_handshake(ssl_context_t *ctx);

// Encrypt and send data
int ssl_send(ssl_context_t *ctx, const uint8_t *data, size_t len);

// Receive and decrypt data
int ssl_recv(ssl_context_t *ctx, uint8_t *data, size_t len);

// Get socket file descriptor
int ssl_get_fd(ssl_context_t *ctx);

// Set socket file descriptor (for non-blocking mode)
int ssl_set_fd(ssl_context_t *ctx, int fd);

// Check if there's pending data to read
int ssl_pending(ssl_context_t *ctx);

// Get SSL error code
int ssl_get_error_code(ssl_context_t *ctx, int ret);

// Read directly into buffer (for ring buffer zero-copy writes)
int ssl_read_into(ssl_context_t *ctx, uint8_t *buf, size_t len);

// Get SSL handle (for direct access to SSL_read)
void *ssl_get_handle(ssl_context_t *ctx);

// Check if hardware timestamping is enabled (Linux only, returns 0 on other platforms)
int ssl_hw_timestamping_enabled(ssl_context_t *ctx);

// Get latest hardware NIC timestamp if available (Linux only, returns 0 if not available)
// Returns timestamp in nanoseconds
uint64_t ssl_get_hw_timestamp(ssl_context_t *ctx);

// Check if Kernel TLS (kTLS) is enabled (Linux only, returns 0 on other platforms or if not enabled)
// When kTLS is enabled, encryption/decryption is offloaded to the kernel for better performance
int ssl_ktls_enabled(ssl_context_t *ctx);

// Get TLS processing mode (kernel or userspace)
// Returns "kTLS (Kernel)" if kernel offload is active, "OpenSSL (Userspace)" otherwise
const char* ssl_get_tls_mode(ssl_context_t *ctx);

// Get negotiated cipher suite name
// Returns cipher name string (e.g., "ECDHE-RSA-AES128-GCM-SHA256"), or NULL if not connected
const char* ssl_get_cipher_name(ssl_context_t *ctx);

// Check if hardware cryptography acceleration is available
// Returns 1 if AES-NI (x86) or ARM Crypto Extensions are available, 0 otherwise
int ssl_has_hw_crypto(void);

// Get SSL backend version string
// Returns version string (e.g., "LibreSSL 3.8.2", "BoringSSL", "OpenSSL 3.0.0")
const char* ssl_get_backend_version(void);

#endif // SSL_H

