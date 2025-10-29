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

// Get CPU cycle for benchmark
uint64_t ws_get_cpu_cycle(void);

// Get timestamp when data was last received from socket
// Use this to measure socket-to-callback latency
uint64_t ws_get_last_recv_timestamp(websocket_context_t *ws);

// Get socket file descriptor for select/poll/epoll
// Returns -1 on error
int ws_get_fd(websocket_context_t *ws);

// Get hardware NIC timestamp (Linux only, returns 0 if not available)
// Timestamp in nanoseconds from hardware network card
uint64_t ws_get_nic_timestamp(websocket_context_t *ws);

// Check if hardware timestamping is available (Linux only)
int ws_has_hw_timestamping(websocket_context_t *ws);

// Batch processing configuration and statistics
// Set maximum messages to process per ws_update() call (0 = unlimited)
void ws_set_max_batch_size(websocket_context_t *ws, size_t max_size);

// Get last batch size (messages processed in last ws_update())
size_t ws_get_last_batch_size(websocket_context_t *ws);

// Get maximum batch size seen (peak messages in single ws_update())
size_t ws_get_max_batch_size(websocket_context_t *ws);

// Get total number of batches processed
size_t ws_get_total_batches(websocket_context_t *ws);

// Get average batch size (total_messages / total_batches)
double ws_get_avg_batch_size(websocket_context_t *ws);

// CPU Affinity and Real-Time Priority Utilities
// These functions configure the calling thread for optimal performance

// Pin calling thread to specific CPU core (0-indexed)
// Returns 0 on success, -1 on failure
// Platform support: Linux (full), macOS (best-effort), others (no-op)
int ws_set_thread_affinity(int cpu_id);

// Set real-time scheduling priority for calling thread
// priority: 1-99 (higher = more urgent), 0 = normal scheduling
// Returns 0 on success, -1 on failure
// Requires: Linux (CAP_SYS_NICE or root), macOS (root), others (no-op)
// Policy: SCHED_FIFO on Linux, SCHED_RR fallback
int ws_set_thread_realtime_priority(int priority);

// Get current thread's CPU affinity (-1 if not set or unsupported)
int ws_get_thread_affinity(void);

// Get current thread's real-time priority (0 if not RT, -1 if error)
int ws_get_thread_realtime_priority(void);

#endif // WS_H


