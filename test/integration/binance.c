#include "../../ws.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>
#include <unistd.h>
#include <time.h>
#include <stdint.h>
#include <inttypes.h>
#include <math.h>

// Platform-specific event loop includes
#ifdef __linux__
#include <sys/epoll.h>
#define USE_EPOLL 1
#elif defined(__APPLE__) || defined(__FreeBSD__) || defined(__OpenBSD__) || defined(__NetBSD__) || defined(__DragonFly__)
#include <sys/event.h>
#include <sys/time.h>
#define USE_KQUEUE 1
#else
#include <sys/select.h>
#include <sys/time.h>
#define USE_SELECT 1
#endif

#define MAX_MESSAGES 1000
#define WARMUP_MESSAGES 10

// Timing record for each message - pre-allocated to avoid I/O during measurement
typedef struct {
    uint64_t recv_cycle;       // When data arrived from socket
    uint64_t callback_cycle;   // When callback was invoked
    uint64_t nic_timestamp_ns; // NIC hardware timestamp (if available)
    size_t payload_len;
    uint8_t opcode;
} timing_record_t;

static int running = 1;
static int connected = 0;
static int message_count = 0;
static timing_record_t timing_records[MAX_MESSAGES];
static int hw_timestamping_available = 0;

void on_message(websocket_context_t *ws, const uint8_t *payload_ptr, size_t payload_len, uint8_t opcode) {
    (void)payload_ptr;

    // Return if we've collected enough samples
    if (message_count >= MAX_MESSAGES) {
        return;
    }

    // CRITICAL: Capture callback timestamp FIRST - minimize operations before this
    uint64_t callback_cycle = ws_get_cpu_cycle();

    // Get timestamp when data was received from socket
    uint64_t recv_cycle = ws_get_last_recv_timestamp(ws);

    // Store timing data - NO I/O operations in hot path!
    timing_record_t *record = &timing_records[message_count];
    record->callback_cycle = callback_cycle;
    record->recv_cycle = recv_cycle;
    record->payload_len = payload_len;
    record->opcode = opcode;

    // Capture NIC timestamp if hardware timestamping is available
    if (hw_timestamping_available) {
        record->nic_timestamp_ns = ws_get_nic_timestamp(ws);
    } else {
        record->nic_timestamp_ns = 0;
    }

    message_count++;

    // Stop after collecting enough samples (no printf here!)
    if (message_count >= 100) {
        running = 0;
    }
}

void on_status(websocket_context_t *ws, int status) {
    (void)ws;
    if (status == 0) {
        printf("✅ WebSocket connected successfully!\n");
        connected = 1;
    } else {
        ws_state_t state = ws_get_state(ws);
        printf("⚠️  WebSocket status change: %d (state: %d)\n", status, state);

        // Only exit if we have a real error and not just status updates
        if (state == WS_STATE_ERROR && connected) {
            printf("❌ Connection error detected\n");
            running = 0;
        }
    }
}

// Get timer/counter frequency (differs between architectures)
double get_timer_frequency_ghz() {
#if defined(__aarch64__)
    // On ARM64, read the counter frequency register
    // Apple Silicon: fixed 24 MHz, ARM servers: varies (often 25-100 MHz)
    uint64_t freq;
    __asm__ volatile("mrs %0, cntfrq_el0" : "=r" (freq));
    return (double)freq / 1e9;  // Convert Hz to GHz
#elif defined(__i386__) || defined(__x86_64__)
    // On x86, measure TSC frequency using a calibration period
    // Use multiple measurements to improve accuracy

    const int num_measurements = 3;
    double frequencies[num_measurements];

    for (int m = 0; m < num_measurements; m++) {
        struct timespec start_time, end_time;
        uint64_t start_cycles, end_cycles;

        clock_gettime(CLOCK_MONOTONIC, &start_time);
        start_cycles = ws_get_cpu_cycle();

        // Busy-wait for more accurate timing (50ms)
        struct timespec target_time = start_time;
        target_time.tv_nsec += 50000000;  // Add 50ms
        if (target_time.tv_nsec >= 1000000000) {
            target_time.tv_sec += 1;
            target_time.tv_nsec -= 1000000000;
        }

        struct timespec current_time;
        do {
            clock_gettime(CLOCK_MONOTONIC, &current_time);
        } while (current_time.tv_sec < target_time.tv_sec ||
                 (current_time.tv_sec == target_time.tv_sec &&
                  current_time.tv_nsec < target_time.tv_nsec));

        end_cycles = ws_get_cpu_cycle();
        clock_gettime(CLOCK_MONOTONIC, &end_time);

        uint64_t cycles_elapsed = end_cycles - start_cycles;
        double ns_elapsed = (end_time.tv_sec - start_time.tv_sec) * 1e9 +
                            (end_time.tv_nsec - start_time.tv_nsec);

        // Prevent division by zero
        if (ns_elapsed < 1000.0) {
            frequencies[m] = 0.0;
        } else {
            frequencies[m] = (double)cycles_elapsed / ns_elapsed;
        }
    }

    // Use median of measurements to avoid outliers
    if (num_measurements == 3) {
        // Simple median of 3 values
        if (frequencies[0] > frequencies[1]) {
            double temp = frequencies[0];
            frequencies[0] = frequencies[1];
            frequencies[1] = temp;
        }
        if (frequencies[1] > frequencies[2]) {
            double temp = frequencies[1];
            frequencies[1] = frequencies[2];
            frequencies[2] = temp;
        }
        if (frequencies[0] > frequencies[1]) {
            double temp = frequencies[0];
            frequencies[0] = frequencies[1];
            frequencies[1] = temp;
        }
        return frequencies[1];  // Return median
    }

    return frequencies[0];
#else
    // Fallback: clock_gettime already returns nanoseconds
    return 1.0;  // 1 ns per tick
#endif
}

// Calculate standard deviation
double calculate_stddev(uint64_t *values, int count, double mean) {
    if (count <= 1) return 0.0;

    double sum_sq_diff = 0.0;
    for (int i = 0; i < count; i++) {
        double diff = (double)values[i] - mean;
        sum_sq_diff += diff * diff;
    }
    return sqrt(sum_sq_diff / (count - 1));
}

// Comparison function for qsort
int compare_uint64(const void *a, const void *b) {
    uint64_t val_a = *(const uint64_t*)a;
    uint64_t val_b = *(const uint64_t*)b;
    if (val_a < val_b) return -1;
    if (val_a > val_b) return 1;
    return 0;
}

void signal_handler(int sig) {
    (void)sig;
    running = 0;
}

int main(int argc, char *argv[]) {
    const char *url = "wss://stream.binance.com:443/stream?streams=btcusdt@trade&timeUnit=MICROSECOND";
    int use_cpu_affinity = 0;
    int use_realtime_priority = 0;
    int cpu_id = 0;
    int rt_priority = 0;

    // Parse command line arguments
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--cpu") == 0 && i + 1 < argc) {
            use_cpu_affinity = 1;
            cpu_id = atoi(argv[++i]);
        } else if (strcmp(argv[i], "--rt-priority") == 0 && i + 1 < argc) {
            use_realtime_priority = 1;
            rt_priority = atoi(argv[++i]);
        } else {
            url = argv[i];
        }
    }

    signal(SIGINT, signal_handler);
    signal(SIGTERM, signal_handler);

    printf("Binance WebSocket Integration Test\n");
    printf("===================================\n\n");

    // Try to set CPU affinity if requested
    if (use_cpu_affinity) {
        printf("⚙️  Setting CPU affinity to core %d...\n", cpu_id);
        if (ws_set_thread_affinity(cpu_id) == 0) {
            printf("   ✅ CPU affinity set successfully\n");
        } else {
            printf("   ⚠️  CPU affinity failed (continuing anyway)\n");
        }
    }

    // Try to set real-time priority if requested
    if (use_realtime_priority) {
        printf("⚙️  Setting real-time priority to %d...\n", rt_priority);
        if (ws_set_thread_realtime_priority(rt_priority) == 0) {
            printf("   ✅ Real-time priority set successfully\n");
        } else {
            printf("   ⚠️  Real-time priority failed (requires privileges)\n");
            printf("   💡 Try: sudo ./test_binance_integration --rt-priority %d\n", rt_priority);
        }
    }

    printf("\nConnecting to: %s\n\n", url);
    
    websocket_context_t *ws = ws_init(url);
    if (!ws) {
        fprintf(stderr, "❌ Failed to initialize WebSocket\n");
        return 1;
    }
    
    ws_set_on_msg(ws, on_message);
    ws_set_on_status(ws, on_status);

    // Check for hardware timestamping support
    hw_timestamping_available = ws_has_hw_timestamping(ws);
    if (hw_timestamping_available) {
        printf("📡 Hardware timestamping: ENABLED (NIC-level timestamps available)\n");
    } else {
#ifdef __linux__
        printf("📡 Hardware timestamping: Not available (requires NIC support)\n");
#else
        printf("📡 Hardware timestamping: Not available (Linux-only feature)\n");
#endif
    }

    // Get timer frequency for accurate latency calculations
    printf("⏱️  Calibrating timer frequency...\n");
    double timer_freq_ghz = get_timer_frequency_ghz();
#if defined(__aarch64__)
    printf("   Timer frequency: %.6f GHz (%.1f MHz - ARM system counter)\n",
           timer_freq_ghz, timer_freq_ghz * 1000.0);
#if defined(__APPLE__)
    printf("   Note: Apple Silicon uses fixed 24 MHz counter, not CPU frequency\n");
#else
    printf("   Note: ARM system counter frequency (read from cntfrq_el0)\n");
#endif
#elif defined(__i386__) || defined(__x86_64__)
    printf("   TSC frequency: %.3f GHz (measured via calibration)\n", timer_freq_ghz);
    if (timer_freq_ghz < 0.5 || timer_freq_ghz > 6.0) {
        printf("   ⚠️  Warning: Unusual frequency detected, measurements may be inaccurate\n");
    }
#else
    printf("   Timer frequency: %.3f GHz\n", timer_freq_ghz);
#endif

#ifdef USE_EPOLL
    printf("Starting event-driven loop (using epoll() - Linux optimized!)...\n");
#elif defined(USE_KQUEUE)
    printf("Starting event-driven loop (using kqueue() - macOS/BSD optimized!)...\n");
#else
    printf("Starting event-driven loop (using select() - portable)...\n");
#endif

    time_t start_time = time(NULL);
    int timeout_seconds = 120;

#ifdef USE_EPOLL
    // Linux: Use epoll for better performance
    int epoll_fd = epoll_create1(0);
    if (epoll_fd < 0) {
        fprintf(stderr, "❌ Failed to create epoll instance\n");
        ws_free(ws);
        return 1;
    }

    struct epoll_event ev, events[1];
    ev.events = EPOLLIN;  // Monitor for input events
    ev.data.fd = -1;      // Will be set later when fd is available
#elif defined(USE_KQUEUE)
    // macOS/BSD: Use kqueue for better performance
    int kq = kqueue();
    if (kq < 0) {
        fprintf(stderr, "❌ Failed to create kqueue instance\n");
        ws_free(ws);
        return 1;
    }

    struct kevent change;
    struct kevent event;
    int registered_fd = -1;  // Track registered fd
#endif

    while (running) {
        // Check for timeout
        if (time(NULL) - start_time > timeout_seconds) {
            printf("\n⏱️  Test timeout after %d seconds\n", timeout_seconds);
            break;
        }

        // Get socket file descriptor
        int fd = ws_get_fd(ws);
        if (fd < 0) {
            // Socket not ready yet, just update and continue
            ws_update(ws);
            continue;
        }

#ifdef USE_EPOLL
        // Register fd with epoll if not already registered
        if (ev.data.fd != fd) {
            if (ev.data.fd >= 0) {
                epoll_ctl(epoll_fd, EPOLL_CTL_DEL, ev.data.fd, NULL);
            }
            ev.data.fd = fd;
            if (epoll_ctl(epoll_fd, EPOLL_CTL_ADD, fd, &ev) < 0) {
                // If already added, modify it
                epoll_ctl(epoll_fd, EPOLL_CTL_MOD, fd, &ev);
            }
        }

        // Wait for events with 10ms timeout
        int nfds = epoll_wait(epoll_fd, events, 1, 10);

        if (nfds > 0) {
            // Data is available - process it immediately
            ws_update(ws);
        } else if (nfds == 0) {
            // Timeout - check state and continue
            ws_update(ws);
        } else {
            // Error in epoll_wait
            ws_state_t state = ws_get_state(ws);
            if (state == WS_STATE_ERROR || state == WS_STATE_CLOSED) {
                break;
            }
        }
#elif defined(USE_KQUEUE)
        // Register fd with kqueue if not already registered
        if (registered_fd != fd) {
            registered_fd = fd;
            EV_SET(&change, fd, EVFILT_READ, EV_ADD | EV_ENABLE, 0, 0, NULL);
            kevent(kq, &change, 1, NULL, 0, NULL);
        }

        // Wait for events with 10ms timeout
        struct timespec timeout = {0, 10000000};  // 10ms in nanoseconds
        int nev = kevent(kq, NULL, 0, &event, 1, &timeout);

        if (nev > 0) {
            // Data is available - process it immediately
            ws_update(ws);
        } else if (nev == 0) {
            // Timeout - check state and continue
            ws_update(ws);
        } else {
            // Error in kevent
            ws_state_t state = ws_get_state(ws);
            if (state == WS_STATE_ERROR || state == WS_STATE_CLOSED) {
                break;
            }
        }
#else
        // macOS/BSD: Use select() for portability
        fd_set readfds;
        FD_ZERO(&readfds);
        FD_SET(fd, &readfds);

        // Set timeout for select (10ms to allow checking state/running flag)
        struct timeval tv;
        tv.tv_sec = 0;
        tv.tv_usec = 10000;  // 10ms timeout

        int select_result = select(fd + 1, &readfds, NULL, NULL, &tv);

        if (select_result > 0) {
            // Data is available - process it immediately
            ws_update(ws);
        } else if (select_result == 0) {
            // Timeout - check state and continue
            ws_update(ws);
        } else {
            // Error in select
            ws_state_t state = ws_get_state(ws);
            if (state == WS_STATE_ERROR || state == WS_STATE_CLOSED) {
                break;
            }
        }
#endif

        // Check connection state
        ws_state_t state = ws_get_state(ws);

        if (state == WS_STATE_ERROR && connected) {
            printf("\n❌ Connection error occurred\n");
            break;
        }

        if (state == WS_STATE_CLOSED) {
            printf("\n📴 Connection closed\n");
            break;
        }
    }

#ifdef USE_EPOLL
    close(epoll_fd);
#elif defined(USE_KQUEUE)
    close(kq);
#endif
    
    printf("\nShutting down...\n");

    // Capture batch statistics before freeing websocket
    size_t total_batches = ws_get_total_batches(ws);
    double avg_batch_size = ws_get_avg_batch_size(ws);
    size_t max_batch_size = ws_get_max_batch_size(ws);
    size_t last_batch_size = ws_get_last_batch_size(ws);
    ws_state_t final_state = ws_get_state(ws);

    ws_close(ws);
    ws_free(ws);

    // ========================================================================
    // COMPREHENSIVE LATENCY ANALYSIS
    // ========================================================================

    printf("\n");
    printf("╔══════════════════════════════════════════════════════════════════╗\n");
    printf("║           LATENCY BENCHMARK RESULTS (NO I/O IN HOT PATH)        ║\n");
    printf("╚══════════════════════════════════════════════════════════════════╝\n");

    if (message_count == 0) {
        printf("\n❌ No messages received - cannot calculate statistics\n");
        return 1;
    }

    // Determine analysis range (skip warmup messages)
    int warmup_skip = (message_count > WARMUP_MESSAGES) ? WARMUP_MESSAGES : 0;
    int stats_count = message_count - warmup_skip;

    if (stats_count <= 0) {
        printf("\n⚠️  Not enough messages for analysis (need > %d)\n", WARMUP_MESSAGES);
        stats_count = message_count;
        warmup_skip = 0;
    }

    printf("\n📈 Dataset Information:\n");
    printf("   Total messages received:    %d\n", message_count);
    printf("   Warmup messages (skipped):  %d\n", warmup_skip);
    printf("   Messages analyzed:          %d\n", stats_count);
#if defined(__aarch64__)
    printf("   Timer frequency:            %.6f GHz (%.1f MHz)\n", timer_freq_ghz, timer_freq_ghz * 1000.0);
#else
    printf("   Timer frequency:            %.3f GHz\n", timer_freq_ghz);
#endif

    // Extract latency values from timing records
    uint64_t *latencies = (uint64_t *)malloc(stats_count * sizeof(uint64_t));
    if (!latencies) {
        fprintf(stderr, "Failed to allocate memory for latency analysis\n");
        return 1;
    }

    uint64_t total_latency = 0;
    uint64_t min_latency = UINT64_MAX;
    uint64_t max_latency = 0;

    for (int i = 0; i < stats_count; i++) {
        int idx = warmup_skip + i;
        uint64_t latency_cycles = timing_records[idx].callback_cycle - timing_records[idx].recv_cycle;
        latencies[i] = latency_cycles;

        total_latency += latency_cycles;
        if (latency_cycles < min_latency) min_latency = latency_cycles;
        if (latency_cycles > max_latency) max_latency = latency_cycles;
    }

    double avg_latency = (double)total_latency / stats_count;
    double stddev = calculate_stddev(latencies, stats_count, avg_latency);

    // Calculate percentiles using qsort
    uint64_t *sorted = (uint64_t *)malloc(stats_count * sizeof(uint64_t));
    if (!sorted) {
        fprintf(stderr, "Failed to allocate memory for sorting\n");
        free(latencies);
        return 1;
    }
    memcpy(sorted, latencies, stats_count * sizeof(uint64_t));
    qsort(sorted, stats_count, sizeof(uint64_t), compare_uint64);

    uint64_t p50 = sorted[stats_count / 2];
    uint64_t p90 = sorted[(int)(stats_count * 0.90)];
    uint64_t p95 = sorted[(int)(stats_count * 0.95)];
    uint64_t p99 = sorted[(int)(stats_count * 0.99)];
    uint64_t p999 = sorted[(int)(stats_count * 0.999)];

    // Convert to nanoseconds
    // On ARM: timer ticks are at 24 MHz (41.67 ns per tick)
    // On x86: TSC ticks are at CPU frequency
    double ticks_per_ns = timer_freq_ghz;

    printf("\n📊 Processing Latency (Socket Receive → Callback Invocation):\n");
#if defined(__aarch64__)
    printf("    Note: Measurements use 24 MHz ARM system counter (41.67 ns/tick)\n");
#endif
    printf("┌──────────────┬──────────────┬──────────────┐\n");
#if defined(__aarch64__)
    printf("│   Metric     │ Timer Ticks  │ Nanoseconds  │\n");
#else
    printf("│   Metric     │ CPU Cycles   │ Nanoseconds  │\n");
#endif
    printf("├──────────────┼──────────────┼──────────────┤\n");
    printf("│ Min          │ %12" PRIu64 " │ %12.2f │\n", min_latency, min_latency / ticks_per_ns);
    printf("│ Max          │ %12" PRIu64 " │ %12.2f │\n", max_latency, max_latency / ticks_per_ns);
    printf("│ Mean         │ %12.0f │ %12.2f │\n", avg_latency, avg_latency / ticks_per_ns);
    printf("│ Std Dev      │ %12.0f │ %12.2f │\n", stddev, stddev / ticks_per_ns);
    printf("│ P50 (median) │ %12" PRIu64 " │ %12.2f │\n", p50, p50 / ticks_per_ns);
    printf("│ P90          │ %12" PRIu64 " │ %12.2f │\n", p90, p90 / ticks_per_ns);
    printf("│ P95          │ %12" PRIu64 " │ %12.2f │\n", p95, p95 / ticks_per_ns);
    printf("│ P99          │ %12" PRIu64 " │ %12.2f │\n", p99, p99 / ticks_per_ns);
    printf("│ P99.9        │ %12" PRIu64 " │ %12.2f │\n", p999, p999 / ticks_per_ns);
    printf("└──────────────┴──────────────┴──────────────┘\n");

    // Outlier detection using IQR method
    uint64_t q1 = sorted[stats_count / 4];
    uint64_t q3 = sorted[(3 * stats_count) / 4];
    uint64_t iqr = q3 - q1;
    uint64_t outlier_threshold = q3 + (uint64_t)(1.5 * iqr);

    int outlier_count = 0;
    for (int i = 0; i < stats_count; i++) {
        if (latencies[i] > outlier_threshold) {
            outlier_count++;
        }
    }

    printf("\n🔍 Outlier Analysis (IQR Method):\n");
#if defined(__aarch64__)
    printf("   Q1 (25th percentile):    %" PRIu64 " ticks (%.2f ns)\n",
           q1, q1 / ticks_per_ns);
    printf("   Q3 (75th percentile):    %" PRIu64 " ticks (%.2f ns)\n",
           q3, q3 / ticks_per_ns);
    printf("   IQR:                     %" PRIu64 " ticks\n", iqr);
    printf("   Outlier threshold:       %" PRIu64 " ticks (%.2f ns)\n",
           outlier_threshold, outlier_threshold / ticks_per_ns);
#else
    printf("   Q1 (25th percentile):    %" PRIu64 " cycles (%.2f ns)\n",
           q1, q1 / ticks_per_ns);
    printf("   Q3 (75th percentile):    %" PRIu64 " cycles (%.2f ns)\n",
           q3, q3 / ticks_per_ns);
    printf("   IQR:                     %" PRIu64 " cycles\n", iqr);
    printf("   Outlier threshold:       %" PRIu64 " cycles (%.2f ns)\n",
           outlier_threshold, outlier_threshold / ticks_per_ns);
#endif
    printf("   Outliers detected:       %d / %d (%.2f%%)\n",
           outlier_count, stats_count, 100.0 * outlier_count / stats_count);

    // Sample timing details (first 5 after warmup and last 5)
    printf("\n📋 Sample Timing Records:\n");
    printf("   First 5 measurements (after warmup):\n");
    int sample_count = (stats_count < 5) ? stats_count : 5;
    for (int i = 0; i < sample_count; i++) {
        int idx = warmup_skip + i;
        uint64_t latency = timing_records[idx].callback_cycle - timing_records[idx].recv_cycle;
#if defined(__aarch64__)
        printf("      [%d] %" PRIu64 " ticks (%.2f ns), %zu bytes, opcode=%d\n",
               idx + 1, latency, latency / ticks_per_ns,
               timing_records[idx].payload_len, timing_records[idx].opcode);
#else
        printf("      [%d] %" PRIu64 " cycles (%.2f ns), %zu bytes, opcode=%d\n",
               idx + 1, latency, latency / ticks_per_ns,
               timing_records[idx].payload_len, timing_records[idx].opcode);
#endif
    }

    if (stats_count > 5) {
        printf("   Last 5 measurements:\n");
        for (int i = stats_count - 5; i < stats_count; i++) {
            int idx = warmup_skip + i;
            uint64_t latency = timing_records[idx].callback_cycle - timing_records[idx].recv_cycle;
#if defined(__aarch64__)
            printf("      [%d] %" PRIu64 " ticks (%.2f ns), %zu bytes, opcode=%d\n",
                   idx + 1, latency, latency / ticks_per_ns,
                   timing_records[idx].payload_len, timing_records[idx].opcode);
#else
            printf("      [%d] %" PRIu64 " cycles (%.2f ns), %zu bytes, opcode=%d\n",
                   idx + 1, latency, latency / ticks_per_ns,
                   timing_records[idx].payload_len, timing_records[idx].opcode);
#endif
        }
    }

    printf("\n📦 Batch Processing Statistics:\n");
    printf("   Total batches:       %zu\n", total_batches);
    printf("   Average batch size:  %.2f messages/batch\n", avg_batch_size);
    printf("   Maximum batch size:  %zu messages\n", max_batch_size);
    printf("   Last batch size:     %zu messages\n", last_batch_size);

    printf("\n🔧 Test Configuration:\n");
    printf("   Connection state:    %d\n", final_state);
    printf("   HW timestamping:     %s\n", hw_timestamping_available ? "ENABLED" : "DISABLED");

    // Clean up
    free(latencies);
    free(sorted);
    
    // Test passes if we received at least 100 messages
    if (message_count >= 100) {
        printf("\n✅ Test PASSED (received %d messages)\n", message_count);
        return 0;
    } else {
        printf("\n❌ Test FAILED - Expected at least 100 messages, got %d\n", message_count);
        return 1;
    }
}
