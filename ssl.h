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

#endif // SSL_H

