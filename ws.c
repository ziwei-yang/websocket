// Define _GNU_SOURCE before any includes for Linux
#ifdef __linux__
#define _GNU_SOURCE
#endif

#include "ws.h"
#include "ssl.h"
#include "ringbuffer.h"
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <stdarg.h>
#include <unistd.h>
#include <time.h>
#include <pthread.h>
#include <errno.h>

// Platform-specific includes for CPU affinity and real-time priority
#ifdef __linux__
#include <sched.h>
#include <sys/types.h>
#define HAS_CPU_AFFINITY 1
#define HAS_RT_PRIORITY 1
#elif defined(__APPLE__)
#include <mach/thread_policy.h>
#include <mach/thread_act.h>
#include <mach/mach_init.h>
#define HAS_CPU_AFFINITY 1  // Best-effort via thread affinity tags
#define HAS_RT_PRIORITY 1   // Via pthread_setschedparam
#else
#define HAS_CPU_AFFINITY 0
#define HAS_RT_PRIORITY 0
#endif
// Use RDTSC instruction for CPU cycle counting
#if defined(__i386__) || defined(__x86_64__)
# include <x86intrin.h>
static inline uint64_t rdtsc(void) {
    return __rdtsc();
}
#elif defined(__aarch64__)
static inline uint64_t rdtsc(void) {
    uint64_t val;
    __asm__ volatile("mrs %0, cntvct_el0" : "=r" (val));
    return val;
}
#else
static inline uint64_t rdtsc(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ULL + ts.tv_nsec;
}
#endif

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
    ws_on_status_t on_status;
    ws_state_t state;
    
    // URL parsing
    char *hostname;
    int port;
    char *path;
    
    // HTTP state
    uint8_t http_buffer[WS_HTTP_BUFFER_SIZE];
    size_t http_len;
    
    // Statistics
    uint64_t connect_time_cycle;
    uint64_t first_msg_cycle;
    size_t total_messages;

    // Latency measurement - timestamp when data last received from socket
    uint64_t last_recv_timestamp;

    // Hardware NIC timestamp (Linux only, in nanoseconds)
    uint64_t last_nic_timestamp;
    int hw_timestamping_available;

    // Batch processing statistics
    size_t total_batches;
    size_t last_batch_size;
    size_t max_batch_size;
    size_t max_messages_per_update;  // 0 = unlimited
};

static uint64_t get_cpu_cycle(void) {
    return rdtsc();
}

uint64_t ws_get_cpu_cycle(void) {
    return get_cpu_cycle();
}

uint64_t ws_get_last_recv_timestamp(websocket_context_t *ws) {
    if (!ws) return 0;
    return ws->last_recv_timestamp;
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

void ws_set_max_batch_size(websocket_context_t *ws, size_t max_size) {
    if (!ws) return;
    ws->max_messages_per_update = max_size;
}

size_t ws_get_last_batch_size(websocket_context_t *ws) {
    if (!ws) return 0;
    return ws->last_batch_size;
}

size_t ws_get_max_batch_size(websocket_context_t *ws) {
    if (!ws) return 0;
    return ws->max_batch_size;
}

size_t ws_get_total_batches(websocket_context_t *ws) {
    if (!ws) return 0;
    return ws->total_batches;
}

double ws_get_avg_batch_size(websocket_context_t *ws) {
    if (!ws || ws->total_batches == 0) return 0.0;
    return (double)ws->total_messages / (double)ws->total_batches;
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

// Generate WebSocket key
static void generate_ws_key(char *key) {
    uint8_t random_bytes[16];
    for (int i = 0; i < 16; i++) {
        random_bytes[i] = rand() & 0xFF;
    }
    base64_encode(random_bytes, key, 16);
}

// Parse URL
static int parse_url(const char *url, char **hostname, int *port, char **path) {
    // Simple URL parser for wss:// and ws://
    if (strncmp(url, "wss://", 6) == 0) {
        url += 6;
    } else if (strncmp(url, "ws://", 5) == 0) {
        url += 5;
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
        
        *path = strdup(slash);
    } else {
        *hostname = strdup(url);
        *port = 443;  // Default for wss://
        *path = strdup("/");
    }
    
    return 0;
}

websocket_context_t *ws_init(const char *url) {
    websocket_context_t *ws = (websocket_context_t *)malloc(sizeof(websocket_context_t));
    if (!ws) return NULL;
    
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

    ws->state = WS_STATE_CONNECTING;
    ws->connect_time_cycle = get_cpu_cycle();
    ws->hw_timestamping_available = ssl_hw_timestamping_enabled(ws->ssl);
    ws->last_nic_timestamp = 0;

    // Initialize batch processing statistics
    ws->total_batches = 0;
    ws->last_batch_size = 0;
    ws->max_batch_size = 0;
    ws->max_messages_per_update = 0;  // 0 = unlimited (process all available)

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

// Zero-copy WebSocket frame parser - returns payload pointer and length directly
static int parse_ws_frame_zero_copy(websocket_context_t *ws, uint8_t **payload_ptr, size_t *payload_len, uint8_t *opcode) {
    size_t available = ringbuffer_available_read(&ws->rx_buffer);
    
    if (available < 2) return 0;  // Not enough data
    
    // Get direct pointer to ringbuffer data (zero-copy)
    uint8_t *data_ptr = NULL;
    size_t data_len = 0;
    ringbuffer_peek_read(&ws->rx_buffer, &data_ptr, &data_len);
    
    if (data_len < 2) return 0;
    
    // Parse frame header directly from ringbuffer memory
    uint8_t fin = (data_ptr[0] >> 7) & 0x01;
    *opcode = data_ptr[0] & 0x0F;
    // uint8_t mask = (data_ptr[1] >> 7) & 0x01; // Not used for performance
    uint64_t payload_len_raw = data_ptr[1] & 0x7F;
    
    // Skip security checks for maximum performance - accept all frames
    // Only require FIN bit for simplicity
    if (!fin) {
        return 0;  // Not a complete frame yet
    }
    
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
    
    // Check if we have enough data for the complete frame
    if (data_len < header_len + payload_len_raw) {
        return 0;  // Not enough data for complete frame
    }
    
    // Return direct pointer to payload data (zero-copy!)
    *payload_ptr = data_ptr + header_len;
    *payload_len = payload_len_raw;
    
    // Advance ringbuffer read position past the entire frame
    ringbuffer_advance_read(&ws->rx_buffer, header_len + payload_len_raw);
    
    return 1;  // Frame parsed successfully
}

// Process incoming data - zero-copy from SSL to ring buffer
static int process_recv(websocket_context_t *ws) {
    uint8_t *write_ptr = NULL;
    size_t write_len = 0;

    // Get write pointer from ring buffer
    ringbuffer_get_write_ptr(&ws->rx_buffer, &write_ptr, &write_len);

    if (write_len == 0) return 0;

    // Read directly from SSL into ring buffer (zero-copy)
    int ret = ssl_read_into(ws->ssl, write_ptr, write_len);

    if (ret > 0) {
        // Capture timestamp immediately when data received from socket
        ws->last_recv_timestamp = get_cpu_cycle();
        ringbuffer_commit_write(&ws->rx_buffer, ret);
        return ret;
    } else if (ret == 0) {
        ws->state = WS_STATE_CLOSED;
        return 0;
    } else if (ret < 0) {
        int err = ssl_get_error_code(ws->ssl, ret);
        // Don't treat want read/write as errors (SSL_ERROR_WANT_READ=2, SSL_ERROR_WANT_WRITE=3)
        if (err != 2 && err != 3 && err != 0) {
            ws->state = WS_STATE_ERROR;
            if (ws->on_status) ws->on_status(ws, -1);
        }
        return ret;
    }
    
    return ret;
}

// Handle HTTP/WebSocket state machine
static void handle_http_stage(websocket_context_t *ws) {
    // Move HTTP data from SSL to ring buffer
    uint8_t buffer[4096];
    int ret = ssl_recv(ws->ssl, buffer, sizeof(buffer));
    
    if (ret > 0) {
        memcpy(ws->http_buffer + ws->http_len, buffer, ret);
        ws->http_len += ret;
        
        if (ws->http_len >= sizeof(ws->http_buffer)) {
            ws->state = WS_STATE_ERROR;
            if (ws->on_status) ws->on_status(ws, -1);
            return;
        }
        
        int http_result = parse_http_response(ws);
        if (http_result == 1) {
            ws->state = WS_STATE_CONNECTED;
            if (ws->on_status) ws->on_status(ws, 0);
            ws->first_msg_cycle = get_cpu_cycle();
        } else if (http_result < 0) {
            ws->state = WS_STATE_ERROR;
            if (ws->on_status) ws->on_status(ws, -1);
        }
    }
}

// Handle WebSocket data stage with zero-copy parsing and batched processing
static void handle_ws_stage(websocket_context_t *ws) {
    // Exhaustive read loop - read from SSL until socket is empty (WANT_READ)
    // This ensures we process all available data in a single ws_update() call
    int read_result;
    do {
        read_result = process_recv(ws);
    } while (read_result > 0);  // Continue until no more data available

    // Track batch size for this update
    size_t batch_size = 0;

    // Parse frames using zero-copy approach
    while (ringbuffer_available_read(&ws->rx_buffer) >= 2) {
        // Check if we've hit the batch size limit
        if (ws->max_messages_per_update > 0 && batch_size >= ws->max_messages_per_update) {
            break;  // Batch limit reached, leave remaining messages for next update
        }

        uint8_t *payload_ptr = NULL;
        size_t payload_len = 0;
        uint8_t opcode = 0;

        int result = parse_ws_frame_zero_copy(ws, &payload_ptr, &payload_len, &opcode);

        if (result > 0) {
            if (!ws->first_msg_cycle) {
                ws->first_msg_cycle = get_cpu_cycle();
            }
            ws->total_messages++;
            batch_size++;

            // Call zero-copy callback
            if (ws->on_msg) {
                ws->on_msg(ws, payload_ptr, payload_len, opcode);
            }

            // Handle control frames
            if (opcode == WS_FRAME_CLOSE) {
                ws->state = WS_STATE_CLOSED;
                break;
            }
        } else if (result < 0) {
            break;  // Error
        } else {
            break;  // Not enough data
        }
    }

    // Update batch statistics
    if (batch_size > 0) {
        ws->total_batches++;
        ws->last_batch_size = batch_size;
        if (batch_size > ws->max_batch_size) {
            ws->max_batch_size = batch_size;
        }
    }
}

int ws_update(websocket_context_t *ws) {
    if (!ws) return -1;
    
    if (ws->state == WS_STATE_CONNECTING) {
        int handshake_result = ssl_handshake(ws->ssl);
        if (handshake_result == 1) {
            // Handshake completed successfully
            ws->state = WS_STATE_HANDSHAKING;
            ws->http_len = 0;
        } else if (handshake_result < 0) {
            // Handshake failed
            ws->state = WS_STATE_ERROR;
            if (ws->on_status) ws->on_status(ws, -1);
            return -1;
        }
        // If handshake_result == 0, handshake is still in progress, continue
    } else if (ws->state == WS_STATE_HANDSHAKING) {
        static int handshake_sent = 0;
        
        // Send handshake if not sent yet
        if (!handshake_sent) {
            int send_result = send_handshake(ws);
            if (send_result > 0) {
                handshake_sent = 1;
            } else if (send_result < 0) {
                ws->state = WS_STATE_ERROR;
                return -1;
            }
            // If send_result == 0, would block, try again next time
        }
        
        handle_http_stage(ws);
    } else if (ws->state == WS_STATE_CONNECTED) {
        handle_ws_stage(ws);
        
        // Send pending data
        if (ringbuffer_available_read(&ws->tx_buffer) > 0) {
            uint8_t send_buf[4096];
            size_t to_send = ringbuffer_read(&ws->tx_buffer, send_buf, sizeof(send_buf));
            ssl_send(ws->ssl, send_buf, to_send);
        }
    }
    
    return 0;
}

int ws_send(websocket_context_t *ws, const uint8_t *data, size_t len) {
    if (!ws || ws->state != WS_STATE_CONNECTED) return -1;
    
    // Create WebSocket frame
    uint8_t frame[14];
    size_t frame_len = 2;
    
    frame[0] = 0x81;  // FIN + TEXT frame
    frame[1] = len & 0x7F;
    
    if (len > 125) {
        // Extended payload length
        frame[1] = 126;
        frame[2] = (len >> 8) & 0xFF;
        frame[3] = len & 0xFF;
        frame_len = 4;
    }
    
    ringbuffer_write(&ws->tx_buffer, frame, frame_len);
    ringbuffer_write(&ws->tx_buffer, data, len);
    
    return len;
}

void ws_close(websocket_context_t *ws) {
    if (ws) {
        ws->state = WS_STATE_CLOSED;
    }
}

ws_state_t ws_get_state(websocket_context_t *ws) {
    return ws ? ws->state : WS_STATE_ERROR;
}

// CPU Affinity and Real-Time Priority Implementation

#ifdef __linux__
// Linux implementation using pthread_setaffinity_np and sched_setscheduler

int ws_set_thread_affinity(int cpu_id) {
    cpu_set_t cpuset;
    CPU_ZERO(&cpuset);
    CPU_SET(cpu_id, &cpuset);

    pthread_t thread = pthread_self();
    int result = pthread_setaffinity_np(thread, sizeof(cpu_set_t), &cpuset);

    if (result != 0) {
        fprintf(stderr, "Failed to set CPU affinity to core %d: %s\n",
                cpu_id, strerror(result));
        return -1;
    }

    return 0;
}

int ws_set_thread_realtime_priority(int priority) {
    if (priority < 0 || priority > 99) {
        fprintf(stderr, "Invalid priority %d (must be 0-99)\n", priority);
        return -1;
    }

    struct sched_param param;
    param.sched_priority = priority;

    // Try SCHED_FIFO first (deterministic, no time slicing)
    int policy = (priority > 0) ? SCHED_FIFO : SCHED_OTHER;
    int result = sched_setscheduler(0, policy, &param);

    if (result != 0) {
        // Try SCHED_RR as fallback (round-robin with time slicing)
        if (priority > 0) {
            policy = SCHED_RR;
            result = sched_setscheduler(0, policy, &param);
        }

        if (result != 0) {
            fprintf(stderr, "Failed to set realtime priority %d: %s\n",
                    priority, strerror(errno));
            fprintf(stderr, "Hint: Run with CAP_SYS_NICE capability or as root\n");
            return -1;
        }
    }

    return 0;
}

int ws_get_thread_affinity(void) {
    cpu_set_t cpuset;
    CPU_ZERO(&cpuset);

    pthread_t thread = pthread_self();
    int result = pthread_getaffinity_np(thread, sizeof(cpu_set_t), &cpuset);

    if (result != 0) {
        return -1;
    }

    // Return first CPU in the set
    for (int i = 0; i < CPU_SETSIZE; i++) {
        if (CPU_ISSET(i, &cpuset)) {
            return i;
        }
    }

    return -1;
}

int ws_get_thread_realtime_priority(void) {
    int policy = sched_getscheduler(0);
    if (policy < 0) {
        return -1;
    }

    if (policy != SCHED_FIFO && policy != SCHED_RR) {
        return 0;  // Not real-time
    }

    struct sched_param param;
    if (sched_getparam(0, &param) != 0) {
        return -1;
    }

    return param.sched_priority;
}

#elif defined(__APPLE__)
// macOS implementation using thread affinity tags and pthread scheduling

int ws_set_thread_affinity(int cpu_id) {
    // macOS doesn't support hard CPU pinning like Linux
    // We use thread affinity tags as a hint to the scheduler
    thread_affinity_policy_data_t policy = { cpu_id + 1 };  // Tag (not CPU ID)

    thread_port_t thread_port = pthread_mach_thread_np(pthread_self());
    kern_return_t result = thread_policy_set(
        thread_port,
        THREAD_AFFINITY_POLICY,
        (thread_policy_t)&policy,
        THREAD_AFFINITY_POLICY_COUNT
    );

    if (result != KERN_SUCCESS) {
        fprintf(stderr, "Failed to set thread affinity tag %d: %d\n", cpu_id, result);
        fprintf(stderr, "Note: macOS uses affinity tags, not hard CPU pinning\n");
        return -1;
    }

    return 0;
}

int ws_set_thread_realtime_priority(int priority) {
    if (priority < 0 || priority > 99) {
        fprintf(stderr, "Invalid priority %d (must be 0-99)\n", priority);
        return -1;
    }

    pthread_t thread = pthread_self();
    struct sched_param param;
    int policy;

    if (priority > 0) {
        // Set real-time priority (requires root on macOS)
        policy = SCHED_RR;  // macOS prefers SCHED_RR over SCHED_FIFO
        param.sched_priority = priority;
    } else {
        // Reset to normal priority
        policy = SCHED_OTHER;
        param.sched_priority = 0;
    }

    int result = pthread_setschedparam(thread, policy, &param);

    if (result != 0) {
        fprintf(stderr, "Failed to set realtime priority %d: %s\n",
                priority, strerror(result));
        fprintf(stderr, "Hint: Requires root privileges on macOS\n");
        return -1;
    }

    return 0;
}

int ws_get_thread_affinity(void) {
    // macOS doesn't provide API to query affinity tags
    fprintf(stderr, "Thread affinity query not supported on macOS\n");
    return -1;
}

int ws_get_thread_realtime_priority(void) {
    pthread_t thread = pthread_self();
    struct sched_param param;
    int policy;

    int result = pthread_getschedparam(thread, &policy, &param);
    if (result != 0) {
        return -1;
    }

    if (policy != SCHED_FIFO && policy != SCHED_RR) {
        return 0;  // Not real-time
    }

    return param.sched_priority;
}

#else
// Fallback implementation for unsupported platforms

int ws_set_thread_affinity(int cpu_id) {
    (void)cpu_id;
    fprintf(stderr, "CPU affinity not supported on this platform\n");
    return -1;
}

int ws_set_thread_realtime_priority(int priority) {
    (void)priority;
    fprintf(stderr, "Real-time priority not supported on this platform\n");
    return -1;
}

int ws_get_thread_affinity(void) {
    return -1;
}

int ws_get_thread_realtime_priority(void) {
    return -1;
}

#endif

