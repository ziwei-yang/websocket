#ifndef WS_NOTIFIER_H
#define WS_NOTIFIER_H

// Unified event notification backend for WebSocket
// Abstracts epoll (Linux) and kqueue (macOS)

typedef struct ws_notifier ws_notifier_t;

// Event types
#define WS_EVENT_READ  (1 << 0)
#define WS_EVENT_WRITE (1 << 1)
#define WS_EVENT_ERROR (1 << 2)

// Initialize notifier
// Returns NULL on failure
ws_notifier_t* ws_notifier_init(void);

// Free notifier
void ws_notifier_free(ws_notifier_t *notifier);

// Add file descriptor to notifier
// events: bitmask of WS_EVENT_* flags
// Returns 0 on success, -1 on failure
int ws_notifier_add(ws_notifier_t *notifier, int fd, int events);

// Modify file descriptor events
// events: bitmask of WS_EVENT_* flags
// Returns 0 on success, -1 on failure
int ws_notifier_mod(ws_notifier_t *notifier, int fd, int events);

// Remove file descriptor from notifier
// Returns 0 on success, -1 on failure
int ws_notifier_del(ws_notifier_t *notifier, int fd);

// Wait for events (fixed 100ms timeout for HFT use case)
// Blocks until events are available (errors handled by callbacks)
void ws_notifier_wait(ws_notifier_t *notifier);

#endif // WS_NOTIFIER_H
