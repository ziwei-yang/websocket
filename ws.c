#include "ws.h"
#include "ssl.h"
#include "ringbuffer.h"
#include "os.h"
#include "ws_notifier.h"
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <stdarg.h>
#include <unistd.h>
#include <errno.h>
#include <time.h>
#include <fcntl.h>

// Platform-specific random number generation
#ifdef __linux__
#include <sys/syscall.h>
#include <sys/random.h>
#endif

// #define WS_DEBUG 1
#define WS_HTTP_BUFFER_SIZE 4096

// Helper: Safe environment variable parsing (returns 1 if valid "1", 0 otherwise)
static inline int env_is_enabled(const char *value) {
    if (!value) return 0;
    char *endptr;
    errno = 0;  // Clear errno before strtol
    long val = strtol(value, &endptr, 10);
    // Reject on overflow, parse error, or value != 1
    if (errno == ERANGE || *endptr != '\0') return 0;
    return (val == 1);
}

typedef enum {
    WS_FRAME_CONTINUATION = 0x0,
    WS_FRAME_TEXT = 0x1,
    WS_FRAME_BINARY = 0x2,
    WS_FRAME_CLOSE = 0x8,
    WS_FRAME_PING = 0x9,
    WS_FRAME_PONG = 0xA
} ws_frame_opcode_t;

// Fast userspace PRNG for masking key generation (xoshiro128+ variant)
typedef struct {
    uint32_t s[4];  // 128-bit state
} ws_prng_t;

// xoshiro128+ fast PRNG - suitable for non-crypto random (masking keys)
static inline uint32_t ws_prng_next(ws_prng_t *prng) {
    const uint32_t result = prng->s[0] + prng->s[3];
    const uint32_t t = prng->s[1] << 9;

    prng->s[2] ^= prng->s[0];
    prng->s[3] ^= prng->s[1];
    prng->s[1] ^= prng->s[2];
    prng->s[0] ^= prng->s[3];

    prng->s[2] ^= t;
    prng->s[3] = (prng->s[3] << 11) | (prng->s[3] >> 21);  // rotl(s[3], 11)

    return result;
}

struct websocket_context {
    ssl_context_t *ssl;
    ringbuffer_t rx_buffer;
    ringbuffer_t tx_buffer;
    ws_on_msg_t on_msg;          // Zero-copy callback
    ws_prng_t prng;              // Fast PRNG for masking keys (seeded once)
    int prng_seeded;             // Flag: 1 if PRNG has been seeded
    ws_on_status_t on_status;    // Connection status callback (called once on connect)
    ws_notifier_t *notifier;     // Optional event loop notifier (for auto WRITE event management)

    // Connection state: 0 = connecting, 1 = connected
    int connected;
    int closed;  // Set to 1 when ws_close() is called

    // URL parsing
    char *hostname;
    int port;
    char *path;

    // HTTP state (used only during initial handshake)
    uint8_t http_buffer[WS_HTTP_BUFFER_SIZE];
    size_t http_len;
    int handshake_sent;

    // Latency measurement timestamps
    uint64_t event_timestamp;              // When event loop received data (TSC cycles)
    uint64_t ssl_read_complete_timestamp;  // When SSL_read completed (TSC cycles)

#ifdef __linux__
    uint64_t hw_timestamp_ns;              // Hardware NIC timestamp in nanoseconds (0 if unavailable)
    int hw_timestamping_available;
#endif

    // Optimization #8: Flag to avoid checking tx_buffer when empty (receive-only workload)
    uint8_t has_pending_tx;
};

// Generate masking key using PRNG (seeds on first call)
// RFC 6455 requires unpredictable masking for all client-to-server frames
static inline uint32_t get_masking_key(websocket_context_t *ws) {
    // Seed PRNG on first call using strong entropy source
    if (__builtin_expect(!ws->prng_seeded, 0)) {
        uint32_t seed[4];
        int seed_success = 0;

#ifdef __APPLE__
        // macOS/BSD: arc4random_buf() provides strong entropy
        arc4random_buf(seed, sizeof(seed));
        seed_success = 1;
#elif defined(__linux__)
        // Linux: getrandom() provides kernel entropy
        #ifdef SYS_getrandom
        if (getrandom(seed, sizeof(seed), 0) == sizeof(seed)) {
            seed_success = 1;
        }
        #endif
#endif

        // Fallback: /dev/urandom for seed (one-time cost)
        if (!seed_success) {
            int fd = open("/dev/urandom", O_RDONLY);
            if (fd >= 0) {
                ssize_t bytes_read = read(fd, seed, sizeof(seed));
                close(fd);
                if (bytes_read == sizeof(seed)) {
                    seed_success = 1;
                }
            }
        }

        // Last resort: time-based seed (weak but better than nothing)
        if (!seed_success) {
            seed[0] = (uint32_t)time(NULL);
            seed[1] = (uint32_t)getpid();
            seed[2] = (uint32_t)os_get_cpu_cycle();
            seed[3] = (uint32_t)(os_get_cpu_cycle() >> 32);
        }

        // Initialize PRNG state
        ws->prng.s[0] = seed[0];
        ws->prng.s[1] = seed[1];
        ws->prng.s[2] = seed[2];
        ws->prng.s[3] = seed[3];
        ws->prng_seeded = 1;
    }

    // Generate masking key from fast PRNG (no syscalls!)
    return ws_prng_next(&ws->prng);
}

uint64_t ws_get_event_timestamp(websocket_context_t *ws) {
    if (!ws) return 0;
    return ws->event_timestamp;
}

uint64_t ws_get_ssl_read_timestamp(websocket_context_t *ws) {
    if (!ws) return 0;
    return ws->ssl_read_complete_timestamp;
}

uint64_t ws_get_hw_timestamp(websocket_context_t *ws) {
    if (!ws) return 0;
#ifdef __linux__
    return ws->hw_timestamp_ns;
#else
    return 0;
#endif
}

int ws_has_hw_timestamping(websocket_context_t *ws) {
    if (!ws) return 0;
#ifdef __linux__
    return ws->hw_timestamping_available;
#else
    return 0;
#endif
}

int ws_get_fd(websocket_context_t *ws) {
    if (!ws || !ws->ssl) return -1;
    return ssl_get_fd(ws->ssl);
}

const char* ws_get_cipher_name(websocket_context_t *ws) {
    if (!ws || !ws->ssl) return NULL;
    return ssl_get_cipher_name(ws->ssl);
}

const char* ws_get_tls_mode(websocket_context_t *ws) {
    if (!ws || !ws->ssl) return "Unknown";
    return ssl_get_tls_mode(ws->ssl);
}

int ws_get_rx_buffer_is_mirrored(websocket_context_t *ws) {
    if (!ws) return 0;
    return ringbuffer_is_mirrored(&ws->rx_buffer);
}

int ws_get_rx_buffer_is_mmap(websocket_context_t *ws) {
    if (!ws) return 0;
    return ringbuffer_is_mmap(&ws->rx_buffer);
}

int ws_get_tx_buffer_is_mirrored(websocket_context_t *ws) {
    if (!ws) return 0;
    return ringbuffer_is_mirrored(&ws->tx_buffer);
}

int ws_get_tx_buffer_is_mmap(websocket_context_t *ws) {
    if (!ws) return 0;
    return ringbuffer_is_mmap(&ws->tx_buffer);
}

// Base64 encoding size calculation: input N bytes → (N+2)/3*4 + 1 bytes output
#define BASE64_ENCODE_SIZE(n) (((n) + 2) / 3 * 4 + 1)

// Base64 encoding for WebSocket key (simplified)
// IMPORTANT: output buffer must be at least BASE64_ENCODE_SIZE(len) bytes
static void base64_encode(const uint8_t *input, char *output, size_t len) {
    const char base64_chars[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    size_t out_len = 0;
    
    for (size_t i = 0; i < len; i += 3) {
        uint32_t b = (input[i] << 16);
        if (i + 1 < len) b |= (input[i + 1] << 8);
        if (i + 2 < len) b |= input[i + 2];
        
        output[out_len++] = base64_chars[(b >> 18) & 0x3F];
        output[out_len++] = base64_chars[(b >> 12) & 0x3F];
        output[out_len++] = (i + 1 < len) ? base64_chars[(b >> 6) & 0x3F] : '=';
        output[out_len++] = (i + 2 < len) ? base64_chars[b & 0x3F] : '=';
    }
    output[out_len] = '\0';
}

// Generate cryptographically secure WebSocket key (RFC 6455 Section 10.3)
static void generate_ws_key(char *key) {
    uint8_t random_bytes[16];
    int success = 0;

    // Try /dev/urandom first (cryptographically secure on Unix-like systems)
    int fd = open("/dev/urandom", O_RDONLY);
    if (fd >= 0) {
        ssize_t bytes_read = read(fd, random_bytes, 16);
        close(fd);
        if (bytes_read == 16) {
            success = 1;
        }
    }

    // Fallback: Use rand() with proper seeding (less secure but better than nothing)
    if (!success) {
        static int rand_initialized = 0;
        if (!rand_initialized) {
            srand((unsigned int)(time(NULL) ^ getpid()));
            rand_initialized = 1;
        }
        for (int i = 0; i < 16; i++) {
            random_bytes[i] = rand() & 0xFF;
        }
    }

    base64_encode(random_bytes, key, 16);
}

// Parse URL
static int parse_url(const char *url, char **hostname, int *port, char **path) {
    // Initialize outputs to NULL for cleanup on failure
    *hostname = NULL;
    *path = NULL;
    *port = 0;

    // Simple URL parser for wss:// and ws://
    int default_port;

    if (strncmp(url, "wss://", 6) == 0) {
        url += 6;
        default_port = 443;
    } else if (strncmp(url, "ws://", 5) == 0) {
        url += 5;
        default_port = 80;
    } else {
        return -1;
    }

    const char *colon = strchr(url, ':');
    const char *slash = strchr(url, '/');

    if (colon && (!slash || colon < slash)) {
        size_t hostname_len = colon - url;
        *hostname = malloc(hostname_len + 1);
        if (!*hostname) {
            return -1;
        }
        memcpy(*hostname, url, hostname_len);
        (*hostname)[hostname_len] = '\0';

        // Validate port using strtol (safer than atoi)
        char *endptr;
        long port_val = strtol(colon + 1, &endptr, 10);
        if (endptr == colon + 1 || (*endptr != '\0' && *endptr != '/') ||
            port_val < 1 || port_val > 65535) {
            free(*hostname);
            *hostname = NULL;
            return -1;
        }
        *port = (int)port_val;

        if (slash) {
            *path = strdup(slash);
        } else {
            *path = strdup("/");
        }
        if (!*path) {
            free(*hostname);
            *hostname = NULL;
            return -1;
        }
    } else if (slash) {
        size_t hostname_len = slash - url;
        *hostname = malloc(hostname_len + 1);
        if (!*hostname) {
            return -1;
        }
        memcpy(*hostname, url, hostname_len);
        (*hostname)[hostname_len] = '\0';

        *port = default_port;  // Use scheme-specific default
        *path = strdup(slash);
        if (!*path) {
            free(*hostname);
            *hostname = NULL;
            return -1;
        }
    } else {
        *hostname = strdup(url);
        if (!*hostname) {
            return -1;
        }
        *port = default_port;  // Use scheme-specific default
        *path = strdup("/");
        if (!*path) {
            free(*hostname);
            *hostname = NULL;
            return -1;
        }
    }

    return 0;
}

websocket_context_t *ws_init(const char *url) {
    // Allocate with cache-line alignment for optimal performance
    websocket_context_t *ws = NULL;
    // Check both return value and pointer
    if (posix_memalign((void**)&ws, CACHE_LINE_SIZE, sizeof(websocket_context_t)) != 0 || ws == NULL) {
        return NULL;
    }

    memset(ws, 0, sizeof(websocket_context_t));

    // Parse URL
    // parse_url handles cleanup internally on failure
    if (parse_url(url, &ws->hostname, &ws->port, &ws->path) < 0) {
        free(ws);
        return NULL;
    }
    
    // Initialize buffers
    if (ringbuffer_init(&ws->rx_buffer) < 0) {
        free(ws->hostname);
        free(ws->path);
        free(ws);
        return NULL;
    }
    
    if (ringbuffer_init(&ws->tx_buffer) < 0) {
        ringbuffer_free(&ws->rx_buffer);
        free(ws->hostname);
        free(ws->path);
        free(ws);
        return NULL;
    }
    
    ws->ssl = ssl_init(ws->hostname, ws->port);
    if (!ws->ssl) {
        ringbuffer_free(&ws->rx_buffer);
        ringbuffer_free(&ws->tx_buffer);
        free(ws->hostname);
        free(ws->path);
        free(ws);
        return NULL;
    }

    ws->connected = 0;
    ws->handshake_sent = 0;

#ifdef __linux__
    ws->hw_timestamping_available = ssl_hw_timestamping_enabled(ws->ssl);
    ws->hw_timestamp_ns = 0;
#endif

    return ws;
}

void ws_free(websocket_context_t *ws) {
    if (!ws) return;

    // Defense-in-depth: Zero sensitive data before free
    // PRNG state contains entropy that could be used to predict masking keys
    // Use volatile to prevent compiler optimization from removing the memset
    volatile uint8_t *prng_ptr = (volatile uint8_t *)&ws->prng;
    for (size_t i = 0; i < sizeof(ws->prng); i++) {
        prng_ptr[i] = 0;
    }

    ssl_free(ws->ssl);
    ringbuffer_free(&ws->rx_buffer);
    ringbuffer_free(&ws->tx_buffer);

    if (ws->hostname) free(ws->hostname);
    if (ws->path) free(ws->path);

    free(ws);
}

void ws_set_on_msg(websocket_context_t *ws, ws_on_msg_t callback) {
    if (ws) ws->on_msg = callback;
}

void ws_set_on_status(websocket_context_t *ws, ws_on_status_t callback) {
    if (ws) ws->on_status = callback;
}

// Send HTTP handshake
static int send_handshake(websocket_context_t *ws) {
    // Key buffer must be >= BASE64_ENCODE_SIZE(16) = 25 bytes
    // Using 32 bytes for safety margin
    char key[32];
    _Static_assert(sizeof(key) >= BASE64_ENCODE_SIZE(16),
                   "key buffer too small for base64-encoded 16-byte random");
    generate_ws_key(key);

    // Build Host header with port if non-standard (RFC 6455 requires port for non-443)
    char host_header[256];
    if (ws->port != 443) {
        snprintf(host_header, sizeof(host_header), "%s:%d", ws->hostname, ws->port);
    } else {
        snprintf(host_header, sizeof(host_header), "%s", ws->hostname);
    }

    char handshake[1024];
    // Check snprintf return value for truncation
    int len = snprintf(handshake, sizeof(handshake),
        "GET %s HTTP/1.1\r\n"
        "Host: %s\r\n"
        "Upgrade: websocket\r\n"
        "Connection: Upgrade\r\n"
        "Sec-WebSocket-Key: %s\r\n"
        "Sec-WebSocket-Version: 13\r\n"
        "\r\n",
        ws->path, host_header, key);

    if (len < 0 || len >= (int)sizeof(handshake)) {
        // Handshake too large or encoding error
        return -1;
    }

    return ssl_send(ws->ssl, (const uint8_t *)handshake, len);
}

// Parse HTTP response
static int parse_http_response(websocket_context_t *ws) {
    // Look for "HTTP/1.1 200 OK" or similar
    if (ws->http_len < 12) return 0;  // Not enough data

    // Simple check for "200" status code
    if (strncmp((char *)ws->http_buffer, "HTTP", 4) != 0) {
        return -1;
    }

    char *status = strstr((char *)ws->http_buffer, " 200 ");
    if (!status) {
        // Check for 101 Switching Protocols
        status = strstr((char *)ws->http_buffer, " 101 ");
        if (!status) return -1;
    }

    // Case-insensitive header parsing (HTTP headers are case-insensitive per RFC 7230)
    // Check common capitalizations for portability (strcasestr is GNU extension)
    if (strstr((char *)ws->http_buffer, "upgrade: websocket") == NULL &&
        strstr((char *)ws->http_buffer, "Upgrade: websocket") == NULL &&
        strstr((char *)ws->http_buffer, "Upgrade: WebSocket") == NULL) {
        return -1;
    }

    return 1;  // Handshake complete
}

// Zero-copy WebSocket frame parser - HFT simplified version
// Assumes: well-formed frames, FIN=1, no masking, complete frames in buffer
// Optimization #7: Accept cached available to avoid redundant ringbuffer_available_read()
static int parse_ws_frame_zero_copy(websocket_context_t *ws, size_t available, uint8_t **payload_ptr, size_t *payload_len, uint8_t *opcode) {
    if (__builtin_expect(available < 2, 0)) return 0;  // Expect enough data available

    // Get direct pointer to ringbuffer data (zero-copy)
    uint8_t *data_ptr = NULL;
    size_t data_len = 0;
    ringbuffer_peek_read(&ws->rx_buffer, &data_ptr, &data_len);
    if (__builtin_expect(data_len < 2, 0)) return 0;  // Expect valid peek

    // Parse frame header - assume FIN=1, well-formed
    *opcode = data_ptr[0] & 0x0F;

    // RFC 6455 Section 5.1: Server MUST NOT mask frames sent to client
    // Bit 7 of byte 1 is the MASK bit - must be 0 for server-to-client frames
    uint8_t mask_bit = data_ptr[1] & 0x80;
    if (__builtin_expect(mask_bit != 0, 0)) {
        return -1;  // Protocol violation: server frames must not be masked
    }

    uint64_t payload_len_raw = data_ptr[1] & 0x7F;
    size_t header_len = 2;

    // Parse extended payload length
    // Most market data messages are small (< 125 bytes), so expect normal length
    if (__builtin_expect(payload_len_raw == 126, 0)) {
        if (__builtin_expect(data_len < 4, 0)) return 0;
        payload_len_raw = (data_ptr[2] << 8) | data_ptr[3];
        header_len = 4;

        // RFC 6455 Section 5.2: Extended length MUST NOT be used for <= 125 bytes
        if (__builtin_expect(payload_len_raw <= 125, 0)) {
            return -1;  // Protocol violation
        }
    } else if (__builtin_expect(payload_len_raw == 127, 0)) {
        if (__builtin_expect(data_len < 10, 0)) return 0;
        payload_len_raw = ((uint64_t)data_ptr[2] << 56) | ((uint64_t)data_ptr[3] << 48) |
                          ((uint64_t)data_ptr[4] << 40) | ((uint64_t)data_ptr[5] << 32) |
                          ((uint64_t)data_ptr[6] << 24) | ((uint64_t)data_ptr[7] << 16) |
                          ((uint64_t)data_ptr[8] << 8) | data_ptr[9];
        header_len = 10;

        // RFC 6455 Section 5.2: 64-bit length MUST NOT be used for <= 65535 bytes
        if (__builtin_expect(payload_len_raw <= 65535, 0)) {
            return -1;  // Protocol violation
        }
    }

    // Check for integer overflow in frame size calculation
    size_t total_frame_size;
    if (__builtin_expect(__builtin_add_overflow(header_len, payload_len_raw, &total_frame_size), 0)) {
        // Overflow: frame size exceeds SIZE_MAX - protocol violation, close connection
        ws->closed = 1;
        return -1;
    }

    // Check if complete frame is available - expect complete frame ready
    if (__builtin_expect(data_len < total_frame_size, 0)) return 0;

    // Return direct pointer to payload (zero-copy)
    *payload_ptr = data_ptr + header_len;
    *payload_len = payload_len_raw;

    // Prefetch payload data for large messages
    // Market data often exceeds cache line size, so expect prefetch needed
    if (__builtin_expect(payload_len_raw > CACHE_LINE_SIZE, 1)) {
        __builtin_prefetch(*payload_ptr + CACHE_LINE_SIZE, 0, 2);
        if (__builtin_expect(payload_len_raw > 512, 0)) {
            __builtin_prefetch(*payload_ptr + 256, 0, 1);
            __builtin_prefetch(*payload_ptr + 512, 0, 0);
        }
    }

    // Advance ringbuffer past the frame
    ringbuffer_advance_read(&ws->rx_buffer, total_frame_size);

    // Return actual bytes consumed - check INT_MAX for safe cast
    if (__builtin_expect(total_frame_size > INT_MAX, 0)) {
        // Frame exceeds 2GB - very rare, but return INT_MAX to prevent truncation
        return INT_MAX;
    }
    return (int)total_frame_size;
}

// Process incoming data - zero-copy from SSL to ring buffer
// HFT simplified: drains SSL, no error handling (fail-fast)
static inline int process_recv(websocket_context_t *ws) {
    // Capture event loop timestamp (start of recv processing)
    ws->event_timestamp = os_get_cpu_cycle();

    uint8_t *write_ptr = NULL;
    size_t write_len = 0;
    int total_read = 0;
    int first_read = 1;  // Track first successful read to capture SSL completion time

    // Optimization: Use do-while to save one ssl_pending() call
    // Since we're called when event notifier reports data available,
    // we know there's data to read, so do at least one attempt
    do {
        ringbuffer_get_write_ptr(&ws->rx_buffer, &write_ptr, &write_len);
        if (__builtin_expect(write_len == 0, 0)) break;  // Expect buffer space available

        int ret = ssl_read_into(ws->ssl, write_ptr, write_len);
        if (__builtin_expect(ret > 0, 1)) {  // Expect successful read
            // Capture timestamp after first successful SSL_read (data decrypted and in userspace)
            if (__builtin_expect(first_read, 1)) {  // Expect first read
                ws->ssl_read_complete_timestamp = os_get_cpu_cycle();

#ifdef __linux__
                // Capture hardware NIC timestamp from BIO storage (if available)
                if (ws->hw_timestamping_available) {
                    bio_timestamp_t *bio_ts = ssl_get_timestamp_storage(ws->ssl);
                    if (bio_ts && bio_ts->hw_timestamp_ns != 0) {
                        ws->hw_timestamp_ns = bio_ts->hw_timestamp_ns;
                    }
                }
#endif
                first_read = 0;
            }
            ringbuffer_commit_write(&ws->rx_buffer, ret);
            total_read += ret;
        } else {
            break;  // No more data available or error
        }
    } while (ssl_pending(ws->ssl) > 0);  // Continue if SSL has buffered data

    return total_read;
}

// Handle HTTP handshake - simplified
static inline void handle_http_stage(websocket_context_t *ws) {
    // Reserve 1 byte for null terminator
    size_t space_available = sizeof(ws->http_buffer) - ws->http_len - 1;
    if (space_available == 0) return;  // Buffer full (accounting for null terminator)

    int ret = ssl_recv(ws->ssl, ws->http_buffer + ws->http_len, space_available);
    if (ret > 0) {
        ws->http_len += ret;
        // Always null-terminate for safe string operations (strstr, etc.)
        ws->http_buffer[ws->http_len] = '\0';
        int parse_result = parse_http_response(ws);
        if (parse_result == 1) {
            ws->connected = 1;
            if (ws->on_status) ws->on_status(ws, 0);
        } else if (parse_result == -1) {
            // HTTP handshake failed (non-101 response)
            // Print the response for debugging
            const char *debug_env = getenv("WS_DEBUG");
            if (env_is_enabled(debug_env) && ws->http_len > 0) {
                fprintf(stderr, "WebSocket handshake failed. HTTP response:\n%.*s\n",
                        (int)ws->http_len, ws->http_buffer);
            }
            ws->closed = 1;  // Mark as closed to indicate error
            if (ws->on_status) ws->on_status(ws, -1);
        }
    }
}

// Send PONG frame in response to PING (RFC 6455 Section 5.5.2)
static inline void send_pong_frame(websocket_context_t *ws, const uint8_t *ping_payload, size_t ping_len) {
    // Don't send PONG if connection is already closed (after CLOSE handshake)
    if (ws->closed) return;

    // RFC 6455 Section 5.5: Control frames MUST have payload <= 125 bytes
    // Receiving oversized PING is a protocol violation - close connection
    if (__builtin_expect(ping_len > 125, 0)) {
        ws->closed = 1;
        return;
    }

    // PONG frame: FIN=1, opcode=0xA (PONG), MASKED (client-to-server)
    // RFC 6455 Section 5.1: All client-to-server frames MUST be masked
    uint8_t frame[6 + 125];  // 2-byte header + 4-byte mask + max control payload
    size_t frame_len = 2;

    frame[0] = 0x8A;  // FIN + PONG opcode
    frame[1] = 0x80 | (ping_len & 0x7F);  // MASK=1 + payload length

    // Generate unpredictable 4-byte masking key (RFC 6455 requirement)
    uint32_t mask_word = get_masking_key(ws);
    uint8_t mask[4];
    mask[0] = (mask_word >> 0) & 0xFF;
    mask[1] = (mask_word >> 8) & 0xFF;
    mask[2] = (mask_word >> 16) & 0xFF;
    mask[3] = (mask_word >> 24) & 0xFF;

    memcpy(frame + 2, mask, 4);
    frame_len += 4;

    size_t total_size = frame_len + ping_len;

    // Write to TX buffer
    uint8_t *write_ptr = NULL;
    size_t available = 0;
    ringbuffer_get_write_ptr(&ws->tx_buffer, &write_ptr, &available);

    if (available >= total_size) {
        // Write frame header (includes masking key)
        memcpy(write_ptr, frame, frame_len);

        // Write masked payload (RFC 6455 Section 5.3: XOR with mask)
        if (ping_len > 0) {
            for (size_t i = 0; i < ping_len; i++) {
                write_ptr[frame_len + i] = ping_payload[i] ^ mask[i & 3];
            }
        }

        ringbuffer_commit_write(&ws->tx_buffer, total_size);
        ws->has_pending_tx = 1;

        // Auto-register WRITE event if notifier is set (Option 3)
        if (ws->notifier) {
            int fd = ws_get_fd(ws);
            if (fd >= 0) {
                ws_notifier_mod(ws->notifier, fd, WS_EVENT_READ | WS_EVENT_WRITE);
            }
        }
    }
    // If no space, silently drop (control frames are best-effort in tight loops)
}

// Send CLOSE frame in response to server-initiated CLOSE (RFC 6455 Section 5.5.1)
static inline void send_close_response(websocket_context_t *ws, const uint8_t *close_payload, size_t close_len) {
    // Don't send multiple CLOSE responses (RFC 6455: only respond once)
    if (ws->closed) return;

    // RFC 6455 Section 5.5: Control frames MUST have payload <= 125 bytes
    if (__builtin_expect(close_len > 125, 0)) {
        ws->closed = 1;
        return;
    }

    // RFC 6455 Section 5.5.1: CLOSE status code must be 0 or 2+ bytes (not 1)
    // 1-byte payload is invalid (status code is always 2 bytes)
    if (__builtin_expect(close_len == 1, 0)) {
        ws->closed = 1;
        return;
    }

    // CLOSE frame: FIN=1, opcode=0x8 (CLOSE), masked
    // Frame format: 2-byte header + 4-byte mask + payload (status code if present)
    uint8_t frame[6 + 125];  // Header + mask + max control payload
    size_t frame_len = 2;

    frame[0] = 0x88;  // FIN + CLOSE opcode

    // Determine payload length (echo back status code if provided)
    size_t response_len = (close_len >= 2) ? 2 : 0;  // Echo status code only, no reason text
    frame[1] = 0x80 | (response_len & 0x7F);  // MASK=1 + length

    // Generate unpredictable 4-byte masking key (RFC 6455 requirement)
    uint32_t mask_word = get_masking_key(ws);
    uint8_t mask[4];
    mask[0] = (mask_word >> 0) & 0xFF;
    mask[1] = (mask_word >> 8) & 0xFF;
    mask[2] = (mask_word >> 16) & 0xFF;
    mask[3] = (mask_word >> 24) & 0xFF;

    memcpy(frame + 2, mask, 4);
    frame_len += 4;

    // Copy and mask status code if present
    if (response_len >= 2) {
        frame[6] = close_payload[0] ^ mask[0];
        frame[7] = close_payload[1] ^ mask[1];
        frame_len += 2;
    }

    size_t total_size = frame_len;

    // Write to TX buffer
    uint8_t *write_ptr = NULL;
    size_t available = 0;
    ringbuffer_get_write_ptr(&ws->tx_buffer, &write_ptr, &available);

    if (available >= total_size) {
        memcpy(write_ptr, frame, total_size);
        ringbuffer_commit_write(&ws->tx_buffer, total_size);
        ws->has_pending_tx = 1;

        // Auto-register WRITE event if notifier is set (Option 3)
        if (ws->notifier) {
            int fd = ws_get_fd(ws);
            if (fd >= 0) {
                ws_notifier_mod(ws->notifier, fd, WS_EVENT_READ | WS_EVENT_WRITE);
            }
        }
    }

    // Mark connection as closed per RFC 6455 closing handshake
    ws->connected = 0;
    ws->closed = 1;
}

// Handle WebSocket data stage - HFT hot path (assume always connected)
static inline void handle_ws_stage(websocket_context_t *ws) {
    // Optimization #7: Cache available to avoid redundant ringbuffer_available_read() calls
    size_t available = ringbuffer_available_read(&ws->rx_buffer);

    // Parse frames zero-copy
    while (available >= 2) {
        uint8_t *payload_ptr = NULL;
        size_t payload_len = 0;
        uint8_t opcode = 0;

        int consumed = parse_ws_frame_zero_copy(ws, available, &payload_ptr, &payload_len, &opcode);
        if (__builtin_expect(consumed > 0, 1)) {  // Expect successful parse
            // Handle PING frames automatically (RFC 6455: MUST respond with PONG)
            // Expect TEXT/BINARY data frames, not control frames (rare)
            if (__builtin_expect(opcode == WS_FRAME_PING, 0)) {
                send_pong_frame(ws, payload_ptr, payload_len);
            }

            // Handle CLOSE frames automatically (RFC 6455 Section 5.5.1: MUST respond with CLOSE)
            if (__builtin_expect(opcode == WS_FRAME_CLOSE, 0)) {
                send_close_response(ws, payload_ptr, payload_len);
            }

            // Pass all frames to callback (application can see PINGs/PONGs/CLOSEs for monitoring)
            if (__builtin_expect(ws->on_msg != NULL, 1)) {  // Expect callback set
                ws->on_msg(ws, payload_ptr, payload_len, opcode);
            }

            // Update cached available by subtracting consumed bytes
            available -= consumed;
        } else if (consumed < 0) {
            // Protocol violation detected - close connection immediately
            // This prevents infinite re-parse loop of malformed frames
            ws->connected = 0;
            ws->closed = 1;
            if (ws->on_status) ws->on_status(ws, -1);
            break;
        } else {
            break;  // consumed == 0: Incomplete frame, wait for more data
        }
    }
}

// HFT simplified ws_update: minimal state machine
int ws_update(websocket_context_t *ws) {
    if (!ws) return -1;

    if (__builtin_expect(!ws->connected, 0)) { // Once per connection
        // Connection phase - do SSL handshake and WebSocket handshake
        int ssl_status = ssl_handshake(ws->ssl);
        if (ssl_status == 1) {
            // SSL handshake complete, send WebSocket handshake
            if (!ws->handshake_sent) {
                if (send_handshake(ws) > 0) {
                    ws->handshake_sent = 1;
                }
            }
            if (ws->handshake_sent) {
                handle_http_stage(ws);
            }
        } else if (ssl_status == -1) {
            // SSL handshake failed - mark as closed and notify application
            ws->closed = 1;
            if (ws->on_status) ws->on_status(ws, -1);
            return -1;
        }
        return 0;
    }

    // Hot path
    // Drain SSL
    process_recv(ws);
    handle_ws_stage(ws);

    // Optimization #8: Only check TX buffer if flag indicates pending data
    if (__builtin_expect(ws->has_pending_tx, 0)) {
        size_t tx_available = ringbuffer_available_read(&ws->tx_buffer);
        if (tx_available > 0) {
            uint8_t *read_ptr = NULL;
            size_t read_len = 0;
            ringbuffer_next_read(&ws->tx_buffer, &read_ptr, &read_len);
            if (read_len > 0) {
                if (read_len > 4096) read_len = 4096;
                int sent = ssl_send(ws->ssl, read_ptr, read_len);
                if (sent > 0) {
                    ringbuffer_advance_read(&ws->tx_buffer, sent);
                }
            }
        }
        // Clear flag if TX buffer is now empty
        if (ringbuffer_available_read(&ws->tx_buffer) == 0) {
            ws->has_pending_tx = 0;

            // Auto-unregister WRITE event if notifier is set (Option 3)
            if (ws->notifier) {
                int fd = ws_get_fd(ws);
                if (fd >= 0) {
                    // Unregister WRITE, keep only READ event
                    ws_notifier_mod(ws->notifier, fd, WS_EVENT_READ);
                }
            }
        }
    }

    return 0;
}

int ws_send(websocket_context_t *ws, const uint8_t *data, size_t len) {
    if (__builtin_expect(!ws || !ws->connected, 0)) return -1;  // Unlikely: validation

    // Create WebSocket frame header with masking (RFC 6455 Section 5.1)
    // Client-to-server frames MUST be masked
    uint8_t frame[14];
    size_t frame_len = 2;

    frame[0] = 0x81;  // FIN + TEXT frame

    // Set length field with MASK bit (bit 7)
    if (len <= 125) {
        frame[1] = 0x80 | (len & 0x7F);  // MASK=1 | length
        frame_len = 2;
    } else if (len <= 65535) {
        frame[1] = 0x80 | 126;  // MASK=1 | 126 (extended payload)
        frame[2] = (len >> 8) & 0xFF;
        frame[3] = len & 0xFF;
        frame_len = 4;
    } else {
        frame[1] = 0x80 | 127;  // MASK=1 | 127 (64-bit length)
        // Write 64-bit length (big-endian)
        frame[2] = 0; frame[3] = 0; frame[4] = 0; frame[5] = 0;
        frame[6] = (len >> 24) & 0xFF;
        frame[7] = (len >> 16) & 0xFF;
        frame[8] = (len >> 8) & 0xFF;
        frame[9] = len & 0xFF;
        frame_len = 10;
    }

    // Generate unpredictable masking key (RFC 6455 Section 5.3)
    // Uses fast userspace PRNG seeded with strong entropy
    uint32_t mask_word = get_masking_key(ws);
    uint8_t mask[4];
    mask[0] = (mask_word >> 0) & 0xFF;
    mask[1] = (mask_word >> 8) & 0xFF;
    mask[2] = (mask_word >> 16) & 0xFF;
    mask[3] = (mask_word >> 24) & 0xFF;

    memcpy(frame + frame_len, mask, 4);
    frame_len += 4;

    // Check for integer overflow in total size calculation
    size_t total_size;
    if (__builtin_expect(__builtin_add_overflow(frame_len, len, &total_size), 0)) {
        // Overflow: message size exceeds SIZE_MAX - invalid length
        return -1;
    }

    // Zero-copy write: get direct pointer and write in one shot
    uint8_t *write_ptr = NULL;
    size_t available = 0;

    ringbuffer_get_write_ptr(&ws->tx_buffer, &write_ptr, &available);

    // Unlikely: ringbuffer should have sufficient contiguous space
    if (__builtin_expect(available < total_size, 0)) {
        // Not enough contiguous space - need to handle wraparound or partial writes
        // For simplicity, write in two parts if needed
        // Likely: at least have space for frame header
        if (__builtin_expect(available >= frame_len, 1)) {
            // Write frame header (includes masking key)
            memcpy(write_ptr, frame, frame_len);
            ringbuffer_commit_write(&ws->tx_buffer, frame_len);

            // Write masked payload
            ringbuffer_get_write_ptr(&ws->tx_buffer, &write_ptr, &available);
            // Unlikely: should have space for payload after wraparound
            if (__builtin_expect(available < len, 0)) {
                return -1;  // Not enough space for payload
            }
            // Apply masking: masked_data[i] = data[i] XOR mask[i & 3]
            // Use bitwise AND instead of modulo for performance (i % 4 == i & 3)
            for (size_t i = 0; i < len; i++) {
                write_ptr[i] = data[i] ^ mask[i & 3];
            }
            ringbuffer_commit_write(&ws->tx_buffer, len);
        } else {
            return -1;  // Not enough space
        }
    } else {
        // Likely: enough contiguous space - write both frame and data in one go
        memcpy(write_ptr, frame, frame_len);
        // Apply masking to payload: masked_data[i] = data[i] XOR mask[i & 3]
        // Use bitwise AND instead of modulo for performance (i % 4 == i & 3)
        for (size_t i = 0; i < len; i++) {
            write_ptr[frame_len + i] = data[i] ^ mask[i & 3];
        }
        ringbuffer_commit_write(&ws->tx_buffer, total_size);
    }

    // Optimization #8: Mark that we have pending TX data
    ws->has_pending_tx = 1;

    // Auto-register WRITE event if notifier is set (Option 3)
    if (ws->notifier) {
        int fd = ws_get_fd(ws);
        if (fd >= 0) {
            // Register both READ and WRITE events
            ws_notifier_mod(ws->notifier, fd, WS_EVENT_READ | WS_EVENT_WRITE);
        }
    }

    return len;
}

// Connect websocket context to event loop notifier for automatic WRITE event management
void ws_set_notifier(websocket_context_t *ws, ws_notifier_t *notifier) {
    if (!ws) return;
    ws->notifier = notifier;
}

// Query if TX buffer has pending data (for manual event management)
int ws_wants_write(websocket_context_t *ws) {
    if (!ws) return 0;
    return ws->has_pending_tx;
}

// Flush TX buffer immediately without waiting for event loop (Option 3)
// Returns 0 on success, -1 on error
int ws_flush_tx(websocket_context_t *ws) {
    if (!ws || !ws->connected) return -1;

    // Only flush if we have pending data
    if (!ws->has_pending_tx) return 0;

    size_t tx_available = ringbuffer_available_read(&ws->tx_buffer);
    if (tx_available > 0) {
        uint8_t *read_ptr = NULL;
        size_t read_len = 0;
        ringbuffer_next_read(&ws->tx_buffer, &read_ptr, &read_len);
        if (read_len > 0) {
            if (read_len > 4096) read_len = 4096;
            int sent = ssl_send(ws->ssl, read_ptr, read_len);
            if (sent > 0) {
                ringbuffer_advance_read(&ws->tx_buffer, sent);
            } else if (sent < 0) {
                return -1;  // Error occurred
            }
        }
    }

    // Clear flag if TX buffer is now empty
    if (ringbuffer_available_read(&ws->tx_buffer) == 0) {
        ws->has_pending_tx = 0;

        // Auto-unregister WRITE event if notifier is set (Option 3)
        if (ws->notifier) {
            int fd = ws_get_fd(ws);
            if (fd >= 0) {
                ws_notifier_mod(ws->notifier, fd, WS_EVENT_READ);
            }
        }
    }

    return 0;
}

void ws_close(websocket_context_t *ws) {
    if (!ws || ws->closed) return;

    // Send WebSocket CLOSE frame (RFC 6455 Section 5.5.1)
    // This ensures proper closing handshake and avoids exchange penalties
    uint8_t frame[8];  // 2-byte header + 4-byte mask + 2-byte status code
    frame[0] = 0x88;   // FIN + CLOSE opcode
    frame[1] = 0x80 | 2;  // MASK=1, length=2 (status code only)

    // Generate unpredictable 4-byte masking key (RFC 6455 requirement)
    uint32_t mask_word = get_masking_key(ws);
    uint8_t mask[4];
    mask[0] = (mask_word >> 0) & 0xFF;
    mask[1] = (mask_word >> 8) & 0xFF;
    mask[2] = (mask_word >> 16) & 0xFF;
    mask[3] = (mask_word >> 24) & 0xFF;

    memcpy(frame + 2, mask, 4);

    // Status code 1000 = Normal Closure (big-endian), then apply mask
    uint8_t status_bytes[2];
    status_bytes[0] = (1000 >> 8) & 0xFF;
    status_bytes[1] = 1000 & 0xFF;
    frame[6] = status_bytes[0] ^ mask[0];
    frame[7] = status_bytes[1] ^ mask[1];

    // Write to TX buffer (best-effort, silent drop if full)
    uint8_t *write_ptr = NULL;
    size_t available = 0;
    ringbuffer_get_write_ptr(&ws->tx_buffer, &write_ptr, &available);

    if (available >= sizeof(frame)) {
        memcpy(write_ptr, frame, sizeof(frame));
        ringbuffer_commit_write(&ws->tx_buffer, sizeof(frame));
        ws->has_pending_tx = 1;

        // Auto-register WRITE event if notifier is set
        if (ws->notifier) {
            int fd = ws_get_fd(ws);
            if (fd >= 0) {
                ws_notifier_mod(ws->notifier, fd, WS_EVENT_READ | WS_EVENT_WRITE);
            }
        }
    }

    ws->connected = 0;
    ws->closed = 1;
    // Note: Socket fd is not closed here - it will be closed in ws_free()
    // This allows buffered CLOSE frame to be transmitted before cleanup
}

ws_state_t ws_get_state(websocket_context_t *ws) {
    if (!ws) return WS_STATE_ERROR;
    if (ws->closed) return WS_STATE_CLOSED;
    if (!ws->connected) return WS_STATE_CONNECTING;
    return WS_STATE_CONNECTED;
}

