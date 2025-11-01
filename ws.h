#ifndef WS_H
#define WS_H

#include <stdint.h>
#include <stddef.h>
#include <sys/time.h>

typedef struct websocket_context websocket_context_t;

// Zero-copy message callback - receives direct memory pointer and length
typedef void (*ws_on_msg_t)(websocket_context_t *ws, const uint8_t *payload_ptr, size_t payload_len, uint8_t opcode);

// Callback function type for connection status
typedef void (*ws_on_status_t)(websocket_context_t *ws, int status);

typedef enum {
    WS_STATE_CONNECTING,
    WS_STATE_HANDSHAKING,
    WS_STATE_CONNECTED,
    WS_STATE_ERROR,
    WS_STATE_CLOSED
} ws_state_t;

// Initialize WebSocket context
websocket_context_t *ws_init(const char *url);

// Free WebSocket context
void ws_free(websocket_context_t *ws);

// Set zero-copy message callback
void ws_set_on_msg(websocket_context_t *ws, ws_on_msg_t callback);

// Set status callback
void ws_set_on_status(websocket_context_t *ws, ws_on_status_t callback);

// Update WebSocket (call in event loop)
int ws_update(websocket_context_t *ws);

// Send message
int ws_send(websocket_context_t *ws, const uint8_t *data, size_t len);

// Close connection
void ws_close(websocket_context_t *ws);

// Get connection state
ws_state_t ws_get_state(websocket_context_t *ws);

// Get timestamp when data was last received from socket (NIC arrival or start of recv)
// Use this to measure socket-to-callback latency
uint64_t ws_get_last_recv_timestamp(websocket_context_t *ws);

// Get timestamp when SSL_read completed (data decrypted and in userspace)
// Use this to measure SSL decryption time separately from application processing
uint64_t ws_get_ssl_read_timestamp(websocket_context_t *ws);

// Get socket file descriptor for select/poll/epoll
// Returns -1 on error
int ws_get_fd(websocket_context_t *ws);

// Get hardware NIC timestamp (Linux only, returns 0 if not available)
// Timestamp in nanoseconds from hardware network card
uint64_t ws_get_nic_timestamp(websocket_context_t *ws);

// Check if hardware timestamping is available (Linux only)
int ws_has_hw_timestamping(websocket_context_t *ws);

// Get ringbuffer status information
int ws_get_rx_buffer_is_mirrored(websocket_context_t *ws);
int ws_get_rx_buffer_is_mmap(websocket_context_t *ws);
int ws_get_tx_buffer_is_mirrored(websocket_context_t *ws);
int ws_get_tx_buffer_is_mmap(websocket_context_t *ws);

// Include OS utilities for CPU affinity and real-time priority
// Use os_* functions directly for thread configuration
#include "os.h"

#endif // WS_H


