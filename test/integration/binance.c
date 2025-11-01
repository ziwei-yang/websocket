#include "../../ws.h"
#include "../../ws_notifier.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>
#include <unistd.h>
#include <time.h>
#include <stdint.h>
#include <inttypes.h>
#include <math.h>

#define NUM_RUNS 5
#define WARMUP_MESSAGES 100          // Warm up with 100 messages per run
#define STATS_MESSAGES 300           // Messages collected for statistics per run
#define MESSAGES_PER_RUN (WARMUP_MESSAGES + STATS_MESSAGES)
#define MAX_MESSAGES (NUM_RUNS * MESSAGES_PER_RUN)

// Timing record for each message - pre-allocated to avoid I/O during measurement
typedef struct {
    uint64_t recv_cycle;       // When data arrived from socket (NIC arrival or start of recv)
    uint64_t ssl_read_cycle;   // When SSL_read completed (data decrypted)
    uint64_t callback_cycle;   // When callback was invoked
    uint64_t nic_timestamp_ns; // NIC hardware timestamp (if available)
    size_t payload_len;
    uint8_t opcode;
} timing_record_t;

static int running = 1;
static int connected = 0;
static int message_count = 0;
static int runs_reported = 0;
static timing_record_t timing_records[MAX_MESSAGES];
static int hw_timestamping_available = 0;

void on_message(websocket_context_t *ws, const uint8_t *payload_ptr, size_t payload_len, uint8_t opcode) {
    (void)payload_ptr;

    // Return if we've collected enough samples
    if (message_count >= MAX_MESSAGES) {
        return;
    }

    // CRITICAL: Capture callback timestamp FIRST - minimize operations before this
    uint64_t callback_cycle = os_get_cpu_cycle();

    // Get timestamps for latency breakdown analysis
    uint64_t recv_cycle = ws_get_last_recv_timestamp(ws);      // NIC arrival (or start of recv)
    uint64_t ssl_read_cycle = ws_get_ssl_read_timestamp(ws);   // SSL_read completed

    // Store timing data - NO I/O operations in hot path!
    timing_record_t *record = &timing_records[message_count];
    record->callback_cycle = callback_cycle;
    record->recv_cycle = recv_cycle;
    record->ssl_read_cycle = ssl_read_cycle;
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
    if (message_count >= MAX_MESSAGES) {
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

        // Exit on error or closed state
        if (state == WS_STATE_ERROR && connected) {
            printf("❌ Connection error detected\n");
            running = 0;
        } else if (state == WS_STATE_CLOSED) {
            printf("📴 Connection closed\n");
            running = 0;
        }
    }
}

// Convert cycles to nanoseconds using the library function
double cycles_to_nanoseconds(uint64_t cycles) {
    return os_cycles_to_ns(cycles);
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

static int run_is_complete(int run_index) {
    return message_count >= (run_index + 1) * MESSAGES_PER_RUN;
}

static size_t percentile_index(int count, double percentile) {
    double raw = percentile * (double)count;
    size_t idx = (size_t)raw;
    if (idx >= (size_t)count) {
        idx = (size_t)count - 1;
    }
    return idx;
}

static void print_run_statistics(int run_index) {
    int run_start = run_index * MESSAGES_PER_RUN;
    int stats_start = run_start + WARMUP_MESSAGES;
    int stats_count = STATS_MESSAGES;

    if (message_count < stats_start + stats_count) {
        return;
    }

    printf("\n════════════════════════════════════════════════════════════════\n");
    printf("Run %d/%d — warmup %d messages, analyzing next %d messages\n",
           run_index + 1, NUM_RUNS, WARMUP_MESSAGES, STATS_MESSAGES);

    // Allocate buffers for three latency metrics
    uint64_t *total_latencies = (uint64_t *)malloc(stats_count * sizeof(uint64_t));  // NIC→APP
    uint64_t *nic_ssl_latencies = (uint64_t *)malloc(stats_count * sizeof(uint64_t)); // NIC→SSL
    uint64_t *ssl_app_latencies = (uint64_t *)malloc(stats_count * sizeof(uint64_t)); // SSL→APP

    if (!total_latencies || !nic_ssl_latencies || !ssl_app_latencies) {
        fprintf(stderr, "Failed to allocate latency buffers for run %d\n", run_index + 1);
        free(total_latencies);
        free(nic_ssl_latencies);
        free(ssl_app_latencies);
        return;
    }

    // Calculate all three latency components
    for (int i = 0; i < stats_count; i++) {
        int idx = stats_start + i;
        total_latencies[i] = timing_records[idx].callback_cycle - timing_records[idx].recv_cycle;
        nic_ssl_latencies[i] = timing_records[idx].ssl_read_cycle - timing_records[idx].recv_cycle;
        ssl_app_latencies[i] = timing_records[idx].callback_cycle - timing_records[idx].ssl_read_cycle;
    }

    // Keep backward compatibility - use total latency for existing logic
    uint64_t *latencies = total_latencies;
    uint64_t total_latency = 0;
    uint64_t min_latency = UINT64_MAX;
    uint64_t max_latency = 0;

    for (int i = 0; i < stats_count; i++) {
        uint64_t latency_cycles = latencies[i];
        total_latency += latency_cycles;
        if (latency_cycles < min_latency) min_latency = latency_cycles;
        if (latency_cycles > max_latency) max_latency = latency_cycles;
    }

    double avg_latency = (double)total_latency / stats_count;
    double stddev = calculate_stddev(latencies, stats_count, avg_latency);

    uint64_t *sorted = (uint64_t *)malloc(stats_count * sizeof(uint64_t));
    if (!sorted) {
        fprintf(stderr, "Failed to allocate sort buffer for run %d\n", run_index + 1);
        free(latencies);
        return;
    }
    memcpy(sorted, latencies, stats_count * sizeof(uint64_t));
    qsort(sorted, stats_count, sizeof(uint64_t), compare_uint64);

    uint64_t p50 = sorted[percentile_index(stats_count, 0.50)];
    uint64_t p90 = sorted[percentile_index(stats_count, 0.90)];
    uint64_t p95 = sorted[percentile_index(stats_count, 0.95)];
    uint64_t p99 = sorted[percentile_index(stats_count, 0.99)];
    uint64_t p999 = sorted[percentile_index(stats_count, 0.999)];

    printf("┌──────────────┬──────────────┬──────────────┐\n");
#if defined(__aarch64__)
    printf("│   Metric     │ Timer Ticks  │ Nanoseconds  │\n");
#else
    printf("│   Metric     │ CPU Cycles   │ Nanoseconds  │\n");
#endif
    printf("├──────────────┼──────────────┼──────────────┤\n");
    printf("│ Min          │ %12" PRIu64 " │ %12.2f │\n", min_latency, cycles_to_nanoseconds(min_latency));
    printf("│ Max          │ %12" PRIu64 " │ %12.2f │\n", max_latency, cycles_to_nanoseconds(max_latency));
    printf("│ Mean         │ %12.0f │ %12.2f │\n", avg_latency, cycles_to_nanoseconds((uint64_t)avg_latency));
    printf("│ Std Dev      │ %12.0f │ %12.2f │\n", stddev, cycles_to_nanoseconds((uint64_t)stddev));
    printf("│ P50 (median) │ %12" PRIu64 " │ %12.2f │\n", p50, cycles_to_nanoseconds(p50));
    printf("│ P90          │ %12" PRIu64 " │ %12.2f │\n", p90, cycles_to_nanoseconds(p90));
    printf("│ P95          │ %12" PRIu64 " │ %12.2f │\n", p95, cycles_to_nanoseconds(p95));
    printf("│ P99          │ %12" PRIu64 " │ %12.2f │\n", p99, cycles_to_nanoseconds(p99));
    printf("│ P99.9        │ %12" PRIu64 " │ %12.2f │\n", p999, cycles_to_nanoseconds(p999));
    printf("└──────────────┴──────────────┴──────────────┘\n");

    uint64_t q1 = sorted[percentile_index(stats_count, 0.25)];
    uint64_t q3 = sorted[percentile_index(stats_count, 0.75)];
    uint64_t iqr = q3 - q1;
    uint64_t outlier_threshold = q3 + (uint64_t)(1.5 * (double)iqr);

    int outlier_count = 0;
    for (int i = 0; i < stats_count; i++) {
        if (latencies[i] > outlier_threshold) {
            outlier_count++;
        }
    }

    printf("\nOutliers (> Q3 + 1.5 × IQR): %d / %d (%.2f%%)\n",
           outlier_count, stats_count, 100.0 * outlier_count / stats_count);

    int sample_count = (stats_count < 5) ? stats_count : 5;
    if (sample_count > 0) {
        printf("\nSample measurements (first %d after warmup):\n", sample_count);
        for (int i = 0; i < sample_count; i++) {
            int global_idx = stats_start + i;
            uint64_t latency = timing_records[global_idx].callback_cycle - timing_records[global_idx].recv_cycle;
            printf("  [%d] %" PRIu64 " ticks (%.2f ns), %zu bytes, opcode=%d\n",
                   global_idx + 1, latency, cycles_to_nanoseconds(latency),
                   timing_records[global_idx].payload_len, timing_records[global_idx].opcode);
        }
        if (stats_count > 5) {
            printf("Sample measurements (last %d of run):\n", sample_count);
            for (int i = stats_count - sample_count; i < stats_count; i++) {
                int global_idx = stats_start + i;
                uint64_t latency = timing_records[global_idx].callback_cycle - timing_records[global_idx].recv_cycle;
                printf("  [%d] %" PRIu64 " ticks (%.2f ns), %zu bytes, opcode=%d\n",
                       global_idx + 1, latency, cycles_to_nanoseconds(latency),
                       timing_records[global_idx].payload_len, timing_records[global_idx].opcode);
            }
        }
    }

    // Display latency breakdown (NIC→SSL vs SSL→APP)
    printf("\n📊 Latency Breakdown (Mean):\n");
    double mean_nic_ssl = 0.0, mean_ssl_app = 0.0;
    for (int i = 0; i < stats_count; i++) {
        mean_nic_ssl += (double)nic_ssl_latencies[i];
        mean_ssl_app += (double)ssl_app_latencies[i];
    }
    mean_nic_ssl /= stats_count;
    mean_ssl_app /= stats_count;

    printf("   NIC→SSL (decryption):  %10.0f ticks (%10.2f ns)  [%.1f%%]\n",
           mean_nic_ssl, cycles_to_nanoseconds((uint64_t)mean_nic_ssl),
           100.0 * mean_nic_ssl / avg_latency);
    printf("   SSL→APP (processing):  %10.0f ticks (%10.2f ns)  [%.1f%%]\n",
           mean_ssl_app, cycles_to_nanoseconds((uint64_t)mean_ssl_app),
           100.0 * mean_ssl_app / avg_latency);
    printf("   ────────────────────────────────────────────────\n");
    printf("   Total (NIC→APP):       %10.0f ticks (%10.2f ns)  [100.0%%]\n",
           avg_latency, cycles_to_nanoseconds((uint64_t)avg_latency));

    // Flush output immediately for real-time visibility
    fflush(stdout);

    free(sorted);
    free(total_latencies);
    free(nic_ssl_latencies);
    free(ssl_app_latencies);
}

static void flush_ready_runs(void) {
    while (runs_reported < NUM_RUNS && run_is_complete(runs_reported)) {
        print_run_statistics(runs_reported);
        runs_reported++;
    }
}

static void print_overall_statistics(void) {
    int completed_runs = message_count / MESSAGES_PER_RUN;
    if (completed_runs <= 0) {
        printf("\n❌ No complete runs captured - cannot calculate aggregate statistics\n");
        return;
    }

    int expected_messages = completed_runs * STATS_MESSAGES;
    uint64_t *latencies = (uint64_t *)malloc((size_t)expected_messages * sizeof(uint64_t));
    if (!latencies) {
        fprintf(stderr, "Failed to allocate aggregate latency buffer\n");
        return;
    }

    size_t count = 0;
    for (int run = 0; run < completed_runs; run++) {
        int stats_start = run * MESSAGES_PER_RUN + WARMUP_MESSAGES;
        int available = message_count - stats_start;
        int to_copy = (available > STATS_MESSAGES) ? STATS_MESSAGES : available;
        for (int i = 0; i < to_copy; i++) {
            int idx = stats_start + i;
            if (idx >= message_count) {
                break;
            }
            latencies[count++] = timing_records[idx].callback_cycle - timing_records[idx].recv_cycle;
        }
    }

    if (count == 0) {
        free(latencies);
        printf("\n⚠️  Not enough messages for aggregate analysis\n");
        return;
    }

    uint64_t total_latency = 0;
    uint64_t min_latency = UINT64_MAX;
    uint64_t max_latency = 0;

    for (size_t i = 0; i < count; i++) {
        uint64_t latency = latencies[i];
        total_latency += latency;
        if (latency < min_latency) min_latency = latency;
        if (latency > max_latency) max_latency = latency;
    }

    double avg_latency = (double)total_latency / (double)count;
    double stddev = calculate_stddev(latencies, (int)count, avg_latency);

    uint64_t *sorted = (uint64_t *)malloc(count * sizeof(uint64_t));
    if (!sorted) {
        fprintf(stderr, "Failed to allocate aggregate sort buffer\n");
        free(latencies);
        return;
    }
    memcpy(sorted, latencies, count * sizeof(uint64_t));
    qsort(sorted, count, sizeof(uint64_t), compare_uint64);

    uint64_t p50 = sorted[percentile_index((int)count, 0.50)];
    uint64_t p90 = sorted[percentile_index((int)count, 0.90)];
    uint64_t p95 = sorted[percentile_index((int)count, 0.95)];
    uint64_t p99 = sorted[percentile_index((int)count, 0.99)];
    uint64_t p999 = sorted[percentile_index((int)count, 0.999)];

    printf("\n📈 Aggregate Dataset Information:\n");
    printf("   Completed runs:             %d / %d\n", completed_runs, NUM_RUNS);
    printf("   Messages analyzed per run:  %d\n", STATS_MESSAGES);
    printf("   Total analyzed messages:    %zu\n", count);
#if defined(__aarch64__) && defined(__APPLE__)
    printf("   Timer: mach_absolute_time() (Apple Silicon)\n");
#elif defined(__i386__) || defined(__x86_64__)
    printf("   Timer: TSC with auto-calibration\n");
#else
    printf("   Timer: clock_gettime\n");
#endif

    printf("\n📊 Aggregate Processing Latency:\n");
    printf("┌──────────────┬──────────────┬──────────────┐\n");
#if defined(__aarch64__)
    printf("│   Metric     │ Timer Ticks  │ Nanoseconds  │\n");
#else
    printf("│   Metric     │ CPU Cycles   │ Nanoseconds  │\n");
#endif
    printf("├──────────────┼──────────────┼──────────────┤\n");
    printf("│ Min          │ %12" PRIu64 " │ %12.2f │\n", min_latency, cycles_to_nanoseconds(min_latency));
    printf("│ Max          │ %12" PRIu64 " │ %12.2f │\n", max_latency, cycles_to_nanoseconds(max_latency));
    printf("│ Mean         │ %12.0f │ %12.2f │\n", avg_latency, cycles_to_nanoseconds((uint64_t)avg_latency));
    printf("│ Std Dev      │ %12.0f │ %12.2f │\n", stddev, cycles_to_nanoseconds((uint64_t)stddev));
    printf("│ P50 (median) │ %12" PRIu64 " │ %12.2f │\n", p50, cycles_to_nanoseconds(p50));
    printf("│ P90          │ %12" PRIu64 " │ %12.2f │\n", p90, cycles_to_nanoseconds(p90));
    printf("│ P95          │ %12" PRIu64 " │ %12.2f │\n", p95, cycles_to_nanoseconds(p95));
    printf("│ P99          │ %12" PRIu64 " │ %12.2f │\n", p99, cycles_to_nanoseconds(p99));
    printf("│ P99.9        │ %12" PRIu64 " │ %12.2f │\n", p999, cycles_to_nanoseconds(p999));
    printf("└──────────────┴──────────────┴──────────────┘\n");

    uint64_t q1 = sorted[percentile_index((int)count, 0.25)];
    uint64_t q3 = sorted[percentile_index((int)count, 0.75)];
    uint64_t iqr = q3 - q1;
    uint64_t outlier_threshold = q3 + (uint64_t)(1.5 * (double)iqr);

    size_t outlier_count = 0;
    for (size_t i = 0; i < count; i++) {
        if (latencies[i] > outlier_threshold) {
            outlier_count++;
        }
    }

    printf("\n🔍 Aggregate Outlier Analysis:\n");
    printf("   Outliers detected: %zu / %zu (%.2f%%)\n",
           outlier_count, count, 100.0 * (double)outlier_count / (double)count);

    free(sorted);
    free(latencies);
}

void signal_handler(int sig) {
    (void)sig;
    running = 0;
}

int main(int argc, char *argv[]) {
    const char *url = "wss://stream.binance.com:443/stream?streams=btcusdt@trade&timeUnit=MICROSECOND";
    int use_cpu_affinity = 0;
    int use_realtime_priority = 0;
    int use_time_constraint = 0;
    int verify_environment = 0;
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
        } else if (strcmp(argv[i], "--time-constraint") == 0) {
            use_time_constraint = 1;
        } else if (strcmp(argv[i], "--verify-env") == 0) {
            verify_environment = 1;
        } else if (strcmp(argv[i], "--help") == 0) {
            printf("Usage: %s [options] [url]\n", argv[0]);
            printf("Options:\n");
            printf("  --cpu N              Pin to CPU core N\n");
            printf("  --rt-priority N      Set real-time priority (requires root)\n");
            printf("  --time-constraint    Use time-constraint policy (macOS, requires root)\n");
            printf("  --verify-env         Run environment verification\n");
            printf("  --help               Show this help\n");
            printf("\nExample:\n");
            printf("  sudo %s --cpu 2 --time-constraint --verify-env\n", argv[0]);
            return 0;
        } else {
            url = argv[i];
        }
    }

    signal(SIGINT, signal_handler);
    signal(SIGTERM, signal_handler);

    printf("Binance WebSocket Integration Test (Enhanced)\n");
    printf("==============================================\n\n");

    // Environment verification if requested
    if (verify_environment) {
        os_verify_env(1);
        printf("\n");
    }

    // Try to set CPU affinity if requested
    if (use_cpu_affinity) {
        printf("⚙️  Setting CPU affinity to core %d...\n", cpu_id);
        if (os_set_thread_affinity(cpu_id) == 0) {
            printf("   ✅ CPU affinity set successfully\n");
        } else {
            printf("   ⚠️  CPU affinity failed (continuing anyway)\n");
        }
    }

    // Try to set real-time priority if requested
    if (use_realtime_priority) {
        printf("⚙️  Setting real-time priority to %d...\n", rt_priority);
        if (os_set_thread_realtime_priority(rt_priority) == 0) {
            printf("   ✅ Real-time priority set successfully\n");
        } else {
            printf("   ⚠️  Real-time priority failed (requires privileges)\n");
            printf("   💡 Try: sudo ./test_binance_integration --rt-priority %d\n", rt_priority);
        }
    }

#ifdef __APPLE__
    // Try to set time-constraint policy if requested (macOS only)
    if (use_time_constraint) {
        printf("⚙️  Setting time-constraint policy (macOS)...\n");
        // 1ms period, 500us computation, 900us constraint, non-preemptible
        uint64_t period = 1000000;      // 1ms
        uint64_t computation = 500000;  // 500us
        uint64_t constraint = 900000;   // 900us
        if (os_set_time_constraint_policy(period, computation, constraint, 0) == 0) {
            printf("   ✅ Time-constraint policy set successfully\n");
            printf("      Period: %llu ns, Computation: %llu ns, Constraint: %llu ns\n",
                   (unsigned long long)period, (unsigned long long)computation,
                   (unsigned long long)constraint);
        } else {
            printf("   ⚠️  Time-constraint policy failed (requires root)\n");
            printf("   💡 Try: sudo ./test_binance_integration --time-constraint\n");
        }
    }
#else
    if (use_time_constraint) {
        printf("⚠️  Time-constraint policy is macOS-only\n");
    }
#endif

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

    // Check ringbuffer configuration
    int rx_mirrored = ws_get_rx_buffer_is_mirrored(ws);
    int rx_mmap = ws_get_rx_buffer_is_mmap(ws);
    int tx_mirrored = ws_get_tx_buffer_is_mirrored(ws);
    int tx_mmap = ws_get_tx_buffer_is_mmap(ws);

    printf("🔄 Ringbuffer Configuration:\n");
    printf("   RX Buffer: %s | %s\n",
           rx_mirrored ? "MIRRORED ✅" : "Standard",
           rx_mmap ? "mmap" : "malloc");
    printf("   TX Buffer: %s | %s\n",
           tx_mirrored ? "MIRRORED ✅" : "Standard",
           tx_mmap ? "mmap" : "malloc");
    if (rx_mirrored || tx_mirrored) {
        printf("   → Zero-wraparound optimization ACTIVE\n");
    } else {
        printf("   → Using standard wraparound logic\n");
    }

    // Test timer conversion
    printf("⏱️  Testing timer conversion...\n");
    uint64_t test_cycles = 1000;
    double test_ns = cycles_to_nanoseconds(test_cycles);
    printf("   %llu cycles = %.2f ns (%.6f ns/cycle)\n",
           (unsigned long long)test_cycles, test_ns, test_ns / test_cycles);
#if defined(__aarch64__) && defined(__APPLE__)
    printf("   Note: Apple Silicon uses mach_absolute_time() for precise timing\n");
#elif defined(__i386__) || defined(__x86_64__)
    printf("   Note: Using TSC with automatic frequency calibration\n");
#endif

    printf("Starting event-driven loop (using unified notifier backend)...\n");
#ifdef __linux__
    printf("   Backend: epoll with edge-triggered mode\n");
#elif defined(__APPLE__)
    printf("   Backend: kqueue with EV_CLEAR edge-triggered mode\n");
#endif

    // Use unified event notifier backend
    ws_notifier_t *notifier = ws_notifier_init();
    if (!notifier) {
        fprintf(stderr, "❌ Failed to create event notifier\n");
        ws_free(ws);
        return 1;
    }

    // Wait for connection and register fd (one-time setup)
    while (running && !connected) {
        ws_update(ws);
    }

    if (connected) {
        int fd = ws_get_fd(ws);
        if (fd >= 0) {
            if (ws_notifier_add(notifier, fd, WS_EVENT_READ) < 0) {
                fprintf(stderr, "❌ Failed to register fd with notifier\n");
                running = 0;
            }
        }
    }

    // Main event loop - minimal hot path
    while (running) {
        ws_notifier_wait(notifier);
        ws_update(ws);

        // Print statistics as each run completes (non-blocking check)
        flush_ready_runs();

        // If running was set to 0 during this iteration, break immediately
        // This avoids one more ws_notifier_wait() call that would block
        if (!running) {
            break;
        }
    }

    ws_notifier_free(notifier);

    flush_ready_runs();

    printf("\nShutting down...\n");

    // Capture state before freeing websocket
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

    print_overall_statistics();

    printf("\n🔧 Test Configuration:\n");
    printf("   Connection state:    %d\n", final_state);
    printf("   HW timestamping:     %s\n", hw_timestamping_available ? "ENABLED" : "DISABLED");
    printf("   RX buffer mirrored:  %s\n", rx_mirrored ? "YES (zero-wraparound)" : "NO");
    printf("   TX buffer mirrored:  %s\n", tx_mirrored ? "YES (zero-wraparound)" : "NO");
    printf("   Memory allocation:   %s\n", (rx_mmap && tx_mmap) ? "mmap" : (rx_mmap || tx_mmap) ? "mixed" : "malloc");

    // Test passes if we received at least MAX_MESSAGES
    if (message_count >= MAX_MESSAGES) {
        printf("\n✅ Test PASSED (received %d messages)\n", message_count);
        return 0;
    } else {
        printf("\n❌ Test FAILED - Expected at least %d messages, got %d\n", MAX_MESSAGES, message_count);
        return 1;
    }
}
