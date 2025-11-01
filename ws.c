#include "ws.h"
#include "ssl.h"
#include "ringbuffer.h"
#include "os.h"
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <stdarg.h>
#include <unistd.h>
#include <errno.h>
#include <time.h>
#include <fcntl.h>

// #define WS_DEBUG 1
#define WS_HTTP_BUFFER_SIZE 4096

typedef enum {
    WS_FRAME_CONTINUATION = 0x0,
    WS_FRAME_TEXT = 0x1,
    WS_FRAME_BINARY = 0x2,
    WS_FRAME_CLOSE = 0x8,
    WS_FRAME_PING = 0x9,
    WS_FRAME_PONG = 0xA
} ws_frame_opcode_t;

struct websocket_context {
    ssl_context_t *ssl;
    ringbuffer_t rx_buffer;
    ringbuffer_t tx_buffer;
    ws_on_msg_t on_msg;          // Zero-copy callback
    ws_on_status_t on_status;    // Connection status callback (called once on connect)

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

    // Latency measurement - timestamp when data last received from socket (NIC arrival)
    uint64_t last_recv_timestamp;

    // Timestamp when SSL_read completed (data decrypted and in userspace)
    uint64_t ssl_read_complete_timestamp;

    // Optimization #8: Flag to avoid checking tx_buffer when empty (receive-only workload)
    uint8_t has_pending_tx;

    // Hardware NIC timestamp (Linux only, in nanoseconds)
    uint64_t last_nic_timestamp;
    int hw_timestamping_available;
};

uint64_t ws_get_last_recv_timestamp(websocket_context_t *ws) {
    if (!ws) return 0;
    return ws->last_recv_timestamp;
}

uint64_t ws_get_ssl_read_timestamp(websocket_context_t *ws) {
    if (!ws) return 0;
    return ws->ssl_read_complete_timestamp;
}

int ws_get_fd(websocket_context_t *ws) {
    if (!ws || !ws->ssl) return -1;
    return ssl_get_fd(ws->ssl);
}

uint64_t ws_get_nic_timestamp(websocket_context_t *ws) {
    if (!ws) return 0;
    return ws->last_nic_timestamp;
}

int ws_has_hw_timestamping(websocket_context_t *ws) {
    if (!ws) return 0;
    return ws->hw_timestamping_available;
}

const char* ws_get_cipher_name(websocket_context_t *ws) {
    if (!ws || !ws->ssl) return NULL;
    return ssl_get_cipher_name(ws->ssl);
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

// Base64 encoding for WebSocket key (simplified)
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
    
    if (colon && colon < slash) {
        size_t hostname_len = colon - url;
        *hostname = malloc(hostname_len + 1);
        strncpy(*hostname, url, hostname_len);
        (*hostname)[hostname_len] = '\0';
        
        *port = atoi(colon + 1);
        
        if (slash) {
            *path = strdup(slash);
        } else {
            *path = strdup("/");
        }
    } else if (slash) {
        size_t hostname_len = slash - url;
        *hostname = malloc(hostname_len + 1);
        strncpy(*hostname, url, hostname_len);
        (*hostname)[hostname_len] = '\0';

        *port = default_port;  // Use scheme-specific default
        *path = strdup(slash);
    } else {
        *hostname = strdup(url);
        *port = default_port;  // Use scheme-specific default
        *path = strdup("/");
    }
    
    return 0;
}

websocket_context_t *ws_init(const char *url) {
    // Allocate with cache-line alignment for optimal performance
    websocket_context_t *ws = NULL;
    if (posix_memalign((void**)&ws, CACHE_LINE_SIZE, sizeof(websocket_context_t)) != 0) {
        return NULL;
    }

    memset(ws, 0, sizeof(websocket_context_t));
    
    // Parse URL
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
    ws->hw_timestamping_available = ssl_hw_timestamping_enabled(ws->ssl);
    ws->last_nic_timestamp = 0;

    return ws;
}

void ws_free(websocket_context_t *ws) {
    if (!ws) return;
    
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
    char key[32];
    generate_ws_key(key);
    
    char handshake[1024];
    int len = snprintf(handshake, sizeof(handshake),
        "GET %s HTTP/1.1\r\n"
        "Host: %s\r\n"
        "Upgrade: websocket\r\n"
        "Connection: Upgrade\r\n"
        "Sec-WebSocket-Key: %s\r\n"
        "Sec-WebSocket-Version: 13\r\n"
        "\r\n",
        ws->path, ws->hostname, key);
    
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
    
    // Look for Upgrade: websocket
    if (strstr((char *)ws->http_buffer, "Upgrade: websocket") == NULL) {
        return -1;
    }
    
    return 1;  // Handshake complete
}

// Zero-copy WebSocket frame parser - HFT simplified version
// Assumes: well-formed frames, FIN=1, no masking, complete frames in buffer
// Optimization #7: Accept cached available to avoid redundant ringbuffer_available_read()
static int parse_ws_frame_zero_copy(websocket_context_t *ws, size_t available, uint8_t **payload_ptr, size_t *payload_len, uint8_t *opcode) {
    if (available < 2) return 0;

    // Get direct pointer to ringbuffer data (zero-copy)
    uint8_t *data_ptr = NULL;
    size_t data_len = 0;
    ringbuffer_peek_read(&ws->rx_buffer, &data_ptr, &data_len);
    if (data_len < 2) return 0;

    // Parse frame header - assume FIN=1, well-formed
    *opcode = data_ptr[0] & 0x0F;
    uint64_t payload_len_raw = data_ptr[1] & 0x7F;
    size_t header_len = 2;

    // Parse extended payload length
    if (payload_len_raw == 126) {
        if (data_len < 4) return 0;
        payload_len_raw = (data_ptr[2] << 8) | data_ptr[3];
        header_len = 4;
    } else if (payload_len_raw == 127) {
        if (data_len < 10) return 0;
        payload_len_raw = ((uint64_t)data_ptr[2] << 56) | ((uint64_t)data_ptr[3] << 48) |
                          ((uint64_t)data_ptr[4] << 40) | ((uint64_t)data_ptr[5] << 32) |
                          ((uint64_t)data_ptr[6] << 24) | ((uint64_t)data_ptr[7] << 16) |
                          ((uint64_t)data_ptr[8] << 8) | data_ptr[9];
        header_len = 10;
    }

    // Check if complete frame is available
    if (data_len < header_len + payload_len_raw) return 0;

    // Return direct pointer to payload (zero-copy)
    *payload_ptr = data_ptr + header_len;
    *payload_len = payload_len_raw;

    // Prefetch payload data for large messages
    if (payload_len_raw > CACHE_LINE_SIZE) {
        __builtin_prefetch(*payload_ptr + CACHE_LINE_SIZE, 0, 2);
        if (payload_len_raw > 512) {
            __builtin_prefetch(*payload_ptr + 256, 0, 1);
            __builtin_prefetch(*payload_ptr + 512, 0, 0);
        }
    }

    // Advance ringbuffer past the frame
    ringbuffer_advance_read(&ws->rx_buffer, header_len + payload_len_raw);

    // Return actual bytes consumed (header + payload) for cache tracking
    return (int)(header_len + payload_len_raw);
}

// Process incoming data - zero-copy from SSL to ring buffer
// HFT simplified: drains SSL, no error handling (fail-fast)
static inline int process_recv(websocket_context_t *ws) {
    // Capture NIC arrival timestamp (or start of recv)
    ws->last_recv_timestamp = os_get_cpu_cycle();

    uint8_t *write_ptr = NULL;
    size_t write_len = 0;
    int total_read = 0;
    int first_read = 1;  // Track first successful read to capture SSL completion time

    // Optimization: Use do-while to save one ssl_pending() call
    // Since we're called when event notifier reports data available,
    // we know there's data to read, so do at least one attempt
    do {
        ringbuffer_get_write_ptr(&ws->rx_buffer, &write_ptr, &write_len);
        if (write_len == 0) break;  // Ringbuffer full

        int ret = ssl_read_into(ws->ssl, write_ptr, write_len);
        if (ret > 0) {
            // Capture timestamp after first successful SSL_read (data decrypted and in userspace)
            if (first_read) {
                ws->ssl_read_complete_timestamp = os_get_cpu_cycle();
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
    size_t space_available = sizeof(ws->http_buffer) - ws->http_len;
    if (space_available == 0) return;  // Buffer full, wait

    int ret = ssl_recv(ws->ssl, ws->http_buffer + ws->http_len, space_available);
    if (ret > 0) {
        ws->http_len += ret;
        if (parse_http_response(ws) == 1) {
            ws->connected = 1;
            if (ws->on_status) ws->on_status(ws, 0);
        }
    }
}

// Send PONG frame in response to PING (RFC 6455 Section 5.5.2)
static inline void send_pong_frame(websocket_context_t *ws, const uint8_t *ping_payload, size_t ping_len) {
    // PONG frame: FIN=1, opcode=0xA (PONG)
    uint8_t frame[14];
    size_t frame_len = 2;

    frame[0] = 0x8A;  // FIN + PONG frame
    frame[1] = ping_len & 0x7F;  // Payload length (control frames must be <= 125 bytes)

    // Extended length for payloads > 125 bytes (rare for PING)
    if (ping_len > 125) {
        frame[1] = 126;
        frame[2] = (ping_len >> 8) & 0xFF;
        frame[3] = ping_len & 0xFF;
        frame_len = 4;
    }

    size_t total_size = frame_len + ping_len;

    // Write to TX buffer
    uint8_t *write_ptr = NULL;
    size_t available = 0;
    ringbuffer_get_write_ptr(&ws->tx_buffer, &write_ptr, &available);

    if (available >= total_size) {
        // Write frame header + payload
        memcpy(write_ptr, frame, frame_len);
        if (ping_len > 0) {
            memcpy(write_ptr + frame_len, ping_payload, ping_len);
        }
        ringbuffer_commit_write(&ws->tx_buffer, total_size);
        ws->has_pending_tx = 1;
    }
    // If no space, silently drop (control frames are best-effort in tight loops)
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
        if (consumed > 0) {
            // Handle PING frames automatically (RFC 6455: MUST respond with PONG)
            if (opcode == WS_FRAME_PING) {
                send_pong_frame(ws, payload_ptr, payload_len);
            }

            // Pass all frames to callback (application can see PINGs/PONGs for monitoring)
            if (ws->on_msg) {
                ws->on_msg(ws, payload_ptr, payload_len, opcode);
            }

            // Update cached available by subtracting consumed bytes
            available -= consumed;
        } else {
            break;  // No more complete frames
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
            // SSL handshake failed - notify application via status callback
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
        }
    }

    return 0;
}

int ws_send(websocket_context_t *ws, const uint8_t *data, size_t len) {
    if (__builtin_expect(!ws || !ws->connected, 0)) return -1;  // Unlikely: validation

    // Create WebSocket frame header
    uint8_t frame[14];
    size_t frame_len = 2;

    frame[0] = 0x81;  // FIN + TEXT frame
    frame[1] = len & 0x7F;

    // Likely: market data and typical messages are >125 bytes
    if (__builtin_expect(len > 125, 1)) {
        // Extended payload length
        frame[1] = 126;
        frame[2] = (len >> 8) & 0xFF;
        frame[3] = len & 0xFF;
        frame_len = 4;
    }

    size_t total_size = frame_len + len;

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
            // Write frame header
            memcpy(write_ptr, frame, frame_len);
            ringbuffer_commit_write(&ws->tx_buffer, frame_len);

            // Write payload
            ringbuffer_get_write_ptr(&ws->tx_buffer, &write_ptr, &available);
            // Unlikely: should have space for payload after wraparound
            if (__builtin_expect(available < len, 0)) {
                return -1;  // Not enough space for payload
            }
            memcpy(write_ptr, data, len);
            ringbuffer_commit_write(&ws->tx_buffer, len);
        } else {
            return -1;  // Not enough space
        }
    } else {
        // Likely: enough contiguous space - write both frame and data in one go
        memcpy(write_ptr, frame, frame_len);
        memcpy(write_ptr + frame_len, data, len);
        ringbuffer_commit_write(&ws->tx_buffer, total_size);
    }

    // Optimization #8: Mark that we have pending TX data
    ws->has_pending_tx = 1;

    return len;
}

void ws_close(websocket_context_t *ws) {
    if (ws) {
        ws->connected = 0;
        ws->closed = 1;
        // Note: Socket fd is not closed here - it will be closed in ws_free()
        // This allows any buffered data to be processed before cleanup
    }
}

ws_state_t ws_get_state(websocket_context_t *ws) {
    if (!ws) return WS_STATE_ERROR;
    if (ws->closed) return WS_STATE_CLOSED;
    if (!ws->connected) return WS_STATE_CONNECTING;
    return WS_STATE_CONNECTED;
}

