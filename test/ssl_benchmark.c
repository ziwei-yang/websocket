#include "../ssl.h"
#include "../ssl_backend.h"
#include "../os.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <fcntl.h>
#include <math.h>
#include <inttypes.h>

#define BENCHMARK_ITERATIONS 1000
#define WARMUP_ITERATIONS 100
#define HANDSHAKE_ITERATIONS 10
#define DATA_SIZES_COUNT 6

// Test data sizes (bytes)
static const size_t DATA_SIZES[] = {
    64,      // Tiny (control messages)
    256,     // Small (typical WebSocket frame)
    1024,    // Medium (1KB)
    4096,    // Large (4KB)
    16384,   // Very large (16KB)
    65536    // Maximum (64KB)
};

typedef struct {
    uint64_t min;
    uint64_t max;
    uint64_t sum;
    double mean;
    double stddev;
    uint64_t p50;
    uint64_t p90;
    uint64_t p95;
    uint64_t p99;
} benchmark_stats_t;

// Global buffer for send/receive operations
static uint8_t *test_buffer = NULL;

static int compare_uint64(const void *a, const void *b) {
    uint64_t val_a = *(const uint64_t *)a;
    uint64_t val_b = *(const uint64_t *)b;
    if (val_a < val_b) return -1;
    if (val_a > val_b) return 1;
    return 0;
}

static void calculate_stats(uint64_t *samples, int count, benchmark_stats_t *stats) {
    if (count == 0) {
        memset(stats, 0, sizeof(benchmark_stats_t));
        return;
    }

    // Sort for percentiles
    uint64_t *sorted = (uint64_t *)malloc(count * sizeof(uint64_t));
    memcpy(sorted, samples, count * sizeof(uint64_t));
    qsort(sorted, count, sizeof(uint64_t), compare_uint64);

    stats->min = sorted[0];
    stats->max = sorted[count - 1];
    stats->p50 = sorted[count / 2];
    stats->p90 = sorted[(int)(count * 0.90)];
    stats->p95 = sorted[(int)(count * 0.95)];
    stats->p99 = sorted[(int)(count * 0.99)];

    // Calculate mean
    stats->sum = 0;
    for (int i = 0; i < count; i++) {
        stats->sum += samples[i];
    }
    stats->mean = (double)stats->sum / count;

    // Calculate standard deviation
    double sum_sq_diff = 0.0;
    for (int i = 0; i < count; i++) {
        double diff = (double)samples[i] - stats->mean;
        sum_sq_diff += diff * diff;
    }
    stats->stddev = sqrt(sum_sq_diff / count);

    free(sorted);
}

static void print_stats(const char *label, benchmark_stats_t *stats) {
    printf("\n%s:\n", label);
    printf("  Min:        %10" PRIu64 " cycles  (%10.2f ns)\n",
           stats->min, os_cycles_to_ns(stats->min));
    printf("  Max:        %10" PRIu64 " cycles  (%10.2f ns)\n",
           stats->max, os_cycles_to_ns(stats->max));
    printf("  Mean:       %10.0f cycles  (%10.2f ns)\n",
           stats->mean, os_cycles_to_ns((uint64_t)stats->mean));
    printf("  Std Dev:    %10.0f cycles  (%10.2f ns)\n",
           stats->stddev, os_cycles_to_ns((uint64_t)stats->stddev));
    printf("  P50:        %10" PRIu64 " cycles  (%10.2f ns)\n",
           stats->p50, os_cycles_to_ns(stats->p50));
    printf("  P90:        %10" PRIu64 " cycles  (%10.2f ns)\n",
           stats->p90, os_cycles_to_ns(stats->p90));
    printf("  P95:        %10" PRIu64 " cycles  (%10.2f ns)\n",
           stats->p95, os_cycles_to_ns(stats->p95));
    printf("  P99:        %10" PRIu64 " cycles  (%10.2f ns)\n",
           stats->p99, os_cycles_to_ns(stats->p99));
}

// Benchmark handshake latency
static int benchmark_handshake(const char *hostname, int port) {
    printf("\n═══════════════════════════════════════════════════════════\n");
    printf("  HANDSHAKE LATENCY BENCHMARK\n");
    printf("═══════════════════════════════════════════════════════════\n");
    printf("  Target: %s:%d\n", hostname, port);
    printf("  Iterations: %d\n", HANDSHAKE_ITERATIONS);

    uint64_t *samples = (uint64_t *)malloc(HANDSHAKE_ITERATIONS * sizeof(uint64_t));
    int successful = 0;

    for (int i = 0; i < HANDSHAKE_ITERATIONS; i++) {
        uint64_t start = os_get_cpu_cycle();

        ssl_context_t *ctx = ssl_init(hostname, port);
        if (!ctx) {
            printf("  [%d/%d] Init failed\n", i + 1, HANDSHAKE_ITERATIONS);
            continue;
        }

        // Wait for handshake to complete
        int result = -1;
        for (int retry = 0; retry < 100 && result != 1; retry++) {
            result = ssl_handshake(ctx);
            if (result == 0) usleep(10000); // 10ms
        }

        uint64_t end = os_get_cpu_cycle();
        ssl_free(ctx);

        if (result == 1) {
            samples[successful++] = end - start;
            printf("  [%d/%d] Success: %.2f ms\n", i + 1, HANDSHAKE_ITERATIONS,
                   os_cycles_to_ns(end - start) / 1000000.0);
        } else {
            printf("  [%d/%d] Handshake failed\n", i + 1, HANDSHAKE_ITERATIONS);
        }

        usleep(100000); // 100ms between tests
    }

    if (successful > 0) {
        benchmark_stats_t stats;
        calculate_stats(samples, successful, &stats);
        print_stats("Handshake Latency", &stats);
        printf("\n  Success rate: %d/%d (%.1f%%)\n",
               successful, HANDSHAKE_ITERATIONS,
               100.0 * successful / HANDSHAKE_ITERATIONS);
    } else {
        printf("\n  ❌ All handshakes failed\n");
    }

    free(samples);
    return successful > 0 ? 0 : -1;
}

// Benchmark encryption throughput
static int benchmark_encryption(ssl_context_t *ctx, size_t data_size) {
    (void)data_size;  // Unused - fixed at 2 bytes for PING frame
    uint64_t *samples = (uint64_t *)malloc(BENCHMARK_ITERATIONS * sizeof(uint64_t));

    // Create a proper WebSocket PING frame to send
    // This won't break the connection like random data would
    uint8_t ping_frame[14];
    ping_frame[0] = 0x89;  // FIN=1, opcode=PING
    ping_frame[1] = 0x00;  // MASK=0, payload length=0 (empty ping)
    size_t frame_size = 2;

    // No warmup - it breaks the connection with production servers
    // Benchmark measures SSL_write() overhead for small control frames

    int successful = 0;
    int failures = 0;
    for (int i = 0; i < BENCHMARK_ITERATIONS && successful < BENCHMARK_ITERATIONS; i++) {
        uint64_t start = os_get_cpu_cycle();
        int ret = ssl_send(ctx, ping_frame, frame_size);
        uint64_t end = os_get_cpu_cycle();

        if (ret > 0) {
            samples[successful++] = end - start;
        } else {
            failures++;
            // Stop after too many failures
            if (failures > 10) break;
        }

        // Small delay between pings to not overwhelm server
        usleep(100);  // 100 microseconds
    }

    benchmark_stats_t stats;
    calculate_stats(samples, successful, &stats);

    printf("\n  WebSocket PING frame: 2 bytes (successful: %d/%d)\n", successful, BENCHMARK_ITERATIONS);
    print_stats("    SSL Write Latency", &stats);

    if (successful > 0) {
        printf("    Mean latency: %.2f μs\n", os_cycles_to_ns((uint64_t)stats.mean) / 1000.0);
    }

    free(samples);
    return successful > 0 ? 0 : -1;
}

// Benchmark decryption throughput
static int benchmark_decryption(ssl_context_t *ctx) {
    (void)ctx; // Unused
    printf("\n  Note: Decryption benchmark requires active SSL stream with incoming data\n");
    printf("  Skipping for standalone benchmark (requires live connection)\n");
    return 0;
}

// Benchmark round-trip latency
static int benchmark_roundtrip(ssl_context_t *ctx, size_t data_size) {
    uint64_t *samples = (uint64_t *)malloc(BENCHMARK_ITERATIONS * sizeof(uint64_t));

    // Fill buffer
    for (size_t i = 0; i < data_size; i++) {
        test_buffer[i] = (uint8_t)(rand() & 0xFF);
    }

    int successful = 0;
    for (int i = 0; i < BENCHMARK_ITERATIONS && successful < BENCHMARK_ITERATIONS; i++) {
        uint64_t start = os_get_cpu_cycle();

        // Send
        int sent = ssl_send(ctx, test_buffer, data_size);
        if (sent <= 0) continue;

        // Receive (non-blocking, may not succeed immediately)
        int received = ssl_recv(ctx, test_buffer, data_size);

        uint64_t end = os_get_cpu_cycle();

        if (received > 0) {
            samples[successful++] = end - start;
        }

        usleep(1000); // 1ms between attempts
    }

    if (successful > 0) {
        benchmark_stats_t stats;
        calculate_stats(samples, successful, &stats);
        printf("\n  Data size: %zu bytes\n", data_size);
        print_stats("    Round-trip Latency", &stats);
    } else {
        printf("\n  Data size: %zu bytes - No successful round-trips\n", data_size);
    }

    free(samples);
    return successful > 0 ? 0 : -1;
}

int main(int argc, char *argv[]) {
    const char *hostname = "stream.binance.com";
    int port = 443;
    int run_handshake = 1;
    int run_encryption = 0;  // Disabled by default - requires echo server
    int run_decryption = 0;
    int run_roundtrip = 0;

    // Parse arguments
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--host") == 0 && i + 1 < argc) {
            hostname = argv[++i];
        } else if (strcmp(argv[i], "--port") == 0 && i + 1 < argc) {
            port = atoi(argv[++i]);
        } else if (strcmp(argv[i], "--no-handshake") == 0) {
            run_handshake = 0;
        } else if (strcmp(argv[i], "--with-encryption") == 0) {
            run_encryption = 1;
        } else if (strcmp(argv[i], "--with-decryption") == 0) {
            run_decryption = 1;
        } else if (strcmp(argv[i], "--with-roundtrip") == 0) {
            run_roundtrip = 1;
        } else if (strcmp(argv[i], "--help") == 0) {
            printf("Usage: %s [options]\n", argv[0]);
            printf("Options:\n");
            printf("  --host HOST          Target hostname (default: stream.binance.com)\n");
            printf("  --port PORT          Target port (default: 443)\n");
            printf("  --no-handshake       Skip handshake benchmark\n");
            printf("  --with-encryption    Include encryption benchmark (requires echo server)\n");
            printf("  --with-decryption    Include decryption benchmark (requires live stream)\n");
            printf("  --with-roundtrip     Include round-trip benchmark (requires echo server)\n");
            printf("  --help               Show this help\n");
            printf("\n");
            printf("Note: By default, only handshake latency is benchmarked.\n");
            printf("Throughput tests require a suitable server:\n");
            printf("  - Encryption/roundtrip: SSL echo server (e.g., openssl s_server)\n");
            printf("  - Decryption: Any SSL stream server (e.g., Binance WebSocket)\n");
            return 0;
        }
    }

    printf("╔═══════════════════════════════════════════════════════════╗\n");
    printf("║          SSL/TLS BACKEND PERFORMANCE BENCHMARK            ║\n");
    printf("╚═══════════════════════════════════════════════════════════╝\n");
    printf("\n Backend: %s\n", SSL_BACKEND_NAME);
    printf(" Target: %s:%d\n", hostname, port);

    // Allocate test buffer
    test_buffer = (uint8_t *)malloc(DATA_SIZES[DATA_SIZES_COUNT - 1]);
    if (!test_buffer) {
        fprintf(stderr, "Failed to allocate test buffer\n");
        return 1;
    }

    // Handshake benchmark
    if (run_handshake) {
        if (benchmark_handshake(hostname, port) < 0) {
            fprintf(stderr, "\n⚠️  Handshake benchmark failed, skipping other tests\n");
            free(test_buffer);
            return 1;
        }
    }

    // Setup persistent connection for throughput tests
    if (run_encryption || run_decryption || run_roundtrip) {
        printf("\n═══════════════════════════════════════════════════════════\n");
        printf("  THROUGHPUT BENCHMARKS\n");
        printf("═══════════════════════════════════════════════════════════\n");
        printf("  Setting up persistent connection...\n");

        ssl_context_t *ctx = ssl_init(hostname, port);
        if (!ctx) {
            fprintf(stderr, "  ❌ Failed to initialize SSL context\n");
            free(test_buffer);
            return 1;
        }

        // Wait for handshake
        int result = -1;
        for (int retry = 0; retry < 100 && result != 1; retry++) {
            result = ssl_handshake(ctx);
            if (result == 0) usleep(10000);
        }

        if (result != 1) {
            fprintf(stderr, "  ❌ Handshake failed\n");
            ssl_free(ctx);
            free(test_buffer);
            return 1;
        }

        // Set socket to blocking mode for accurate benchmark timing
        // In non-blocking mode, SSL_write returns immediately after buffering
        int sockfd = ssl_get_fd(ctx);
        if (sockfd >= 0) {
            int flags = fcntl(sockfd, F_GETFL, 0);
            fcntl(sockfd, F_SETFL, flags & ~O_NONBLOCK);
        }

        printf("  ✅ Connected (blocking mode for accurate timing)\n");

        // Encryption benchmark
        if (run_encryption) {
            printf("\n─────────────────────────────────────────────────────────────\n");
            printf(" ENCRYPTION BENCHMARK (WebSocket PING frames)\n");
            printf("─────────────────────────────────────────────────────────────\n");
            printf("  Note: Testing with production server - using 2-byte PING frames\n");
            printf("  For arbitrary size testing, use a local echo server\n");
            benchmark_encryption(ctx, 0);  // data_size parameter unused
        }

        // Decryption benchmark
        if (run_decryption) {
            printf("\n─────────────────────────────────────────────────────────────\n");
            printf(" DECRYPTION BENCHMARK\n");
            printf("─────────────────────────────────────────────────────────────\n");
            benchmark_decryption(ctx);
        }

        // Round-trip benchmark
        if (run_roundtrip) {
            printf("\n─────────────────────────────────────────────────────────────\n");
            printf(" ROUND-TRIP BENCHMARK\n");
            printf("─────────────────────────────────────────────────────────────\n");
            for (int i = 0; i < DATA_SIZES_COUNT; i++) {
                benchmark_roundtrip(ctx, DATA_SIZES[i]);
            }
        }

        ssl_free(ctx);
    }

    free(test_buffer);

    printf("\n╔═══════════════════════════════════════════════════════════╗\n");
    printf("║              BENCHMARK COMPLETE                           ║\n");
    printf("╚═══════════════════════════════════════════════════════════╝\n\n");

    return 0;
}
