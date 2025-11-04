/*
 * Clock Precision Test for x86 + Linux
 *
 * This test verifies the accuracy and precision of the TSC-based timing
 * implementation by comparing it against CLOCK_MONOTONIC and testing
 * various edge cases.
 */

#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <time.h>
#include <unistd.h>
#include <math.h>
#include <string.h>
#include <sched.h>

#include "../os.h"

// ANSI color codes for output
#define COLOR_GREEN  "\033[0;32m"
#define COLOR_RED    "\033[0;31m"
#define COLOR_YELLOW "\033[0;33m"
#define COLOR_BLUE   "\033[0;34m"
#define COLOR_RESET  "\033[0m"

// Test result tracking
static int total_tests = 0;
static int passed_tests = 0;
static int failed_tests = 0;

// Get nanosecond timestamp from CLOCK_MONOTONIC (reference clock)
static inline uint64_t get_monotonic_ns(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ULL + ts.tv_nsec;
}

// Sleep for specified nanoseconds
static void sleep_ns(uint64_t nanoseconds) {
    struct timespec ts;
    ts.tv_sec = nanoseconds / 1000000000ULL;
    ts.tv_nsec = nanoseconds % 1000000000ULL;
    nanosleep(&ts, NULL);
}

// Test result helper
static void test_result(const char *test_name, int passed, const char *details) {
    total_tests++;
    if (passed) {
        passed_tests++;
        printf("  [%s✓%s] %s", COLOR_GREEN, COLOR_RESET, test_name);
        if (details && strlen(details) > 0) {
            printf(" - %s", details);
        }
        printf("\n");
    } else {
        failed_tests++;
        printf("  [%s✗%s] %s", COLOR_RED, COLOR_RESET, test_name);
        if (details && strlen(details) > 0) {
            printf(" - %s", details);
        }
        printf("\n");
    }
}

// Calculate percentage error
static double percent_error(double measured, double expected) {
    return fabs((measured - expected) / expected) * 100.0;
}

// Test 1: Verify initialization works
static void test_initialization(void) {
    printf("\n%s[Test 1: Initialization]%s\n", COLOR_BLUE, COLOR_RESET);

    // Test calling os_cycles_to_ns before os_get_cpu_cycle
    double ns1 = os_cycles_to_ns(1000000);
    int init_works = (ns1 > 0.0);

    char details[256];
    snprintf(details, sizeof(details), "1M cycles = %.2f ns (%.3f ns/cycle)",
             ns1, ns1 / 1000000.0);
    test_result("Initialization without prior os_get_cpu_cycle() call", init_works, details);

    // Test consistency
    uint64_t cycles = os_get_cpu_cycle();
    double ns2 = os_cycles_to_ns(1000000);
    double diff = fabs(ns1 - ns2);
    int consistent = (diff < 1.0);  // Should be identical

    snprintf(details, sizeof(details), "difference = %.6f ns", diff);
    test_result("Consistent conversion results", consistent, details);
}

// Test 2: Verify calibration accuracy across multiple time scales
static void test_calibration_accuracy(void) {
    printf("\n%s[Test 2: Calibration Accuracy]%s\n", COLOR_BLUE, COLOR_RESET);

    struct {
        const char *name;
        uint64_t sleep_ns;
        double tolerance_percent;
    } tests[] = {
        {"100 microseconds", 100000, 20.0},     // 20% tolerance for short sleeps
        {"1 millisecond",    1000000, 10.0},    // 10% tolerance
        {"10 milliseconds",  10000000, 5.0},    // 5% tolerance
        {"50 milliseconds",  50000000, 3.0},    // 3% tolerance
        {"100 milliseconds", 100000000, 2.0},   // 2% tolerance
    };

    for (size_t i = 0; i < sizeof(tests) / sizeof(tests[0]); i++) {
        // Measure using TSC
        uint64_t tsc_start = os_get_cpu_cycle();
        uint64_t mono_start = get_monotonic_ns();

        sleep_ns(tests[i].sleep_ns);

        uint64_t tsc_end = os_get_cpu_cycle();
        uint64_t mono_end = get_monotonic_ns();

        uint64_t tsc_elapsed = tsc_end - tsc_start;
        double tsc_ns = os_cycles_to_ns(tsc_elapsed);
        double mono_ns = (double)(mono_end - mono_start);

        double error = percent_error(tsc_ns, mono_ns);
        int passed = (error <= tests[i].tolerance_percent);

        char details[256];
        snprintf(details, sizeof(details),
                 "TSC: %.1f µs, MONOTONIC: %.1f µs, error: %.2f%%",
                 tsc_ns / 1000.0, mono_ns / 1000.0, error);
        test_result(tests[i].name, passed, details);
    }
}

// Test 3: Check for calibration drift over time
static void test_calibration_drift(void) {
    printf("\n%s[Test 3: Calibration Drift]%s\n", COLOR_BLUE, COLOR_RESET);

    const int num_samples = 10;
    const uint64_t sample_interval_ns = 10000000;  // 10ms
    double errors[num_samples];

    for (int i = 0; i < num_samples; i++) {
        uint64_t tsc_start = os_get_cpu_cycle();
        uint64_t mono_start = get_monotonic_ns();

        sleep_ns(sample_interval_ns);

        uint64_t tsc_end = os_get_cpu_cycle();
        uint64_t mono_end = get_monotonic_ns();

        uint64_t tsc_elapsed = tsc_end - tsc_start;
        double tsc_ns = os_cycles_to_ns(tsc_elapsed);
        double mono_ns = (double)(mono_end - mono_start);

        errors[i] = percent_error(tsc_ns, mono_ns);
    }

    // Calculate statistics
    double sum = 0.0, sum_sq = 0.0;
    double min_err = errors[0], max_err = errors[0];

    for (int i = 0; i < num_samples; i++) {
        sum += errors[i];
        sum_sq += errors[i] * errors[i];
        if (errors[i] < min_err) min_err = errors[i];
        if (errors[i] > max_err) max_err = errors[i];
    }

    double mean_err = sum / num_samples;
    double stddev = sqrt((sum_sq / num_samples) - (mean_err * mean_err));

    // Check for drift: standard deviation should be low
    int no_drift = (stddev < 2.0);  // Less than 2% stddev

    char details[256];
    snprintf(details, sizeof(details),
             "mean: %.2f%%, stddev: %.2f%%, range: [%.2f%%, %.2f%%]",
             mean_err, stddev, min_err, max_err);
    test_result("No calibration drift over time", no_drift, details);
}

// Test 4: Measure timing overhead
static void test_timing_overhead(void) {
    printf("\n%s[Test 4: Timing Overhead]%s\n", COLOR_BLUE, COLOR_RESET);

    const int iterations = 10000;

    // Test os_get_cpu_cycle() overhead
    uint64_t start = os_get_cpu_cycle();
    for (int i = 0; i < iterations; i++) {
        volatile uint64_t dummy = os_get_cpu_cycle();
        (void)dummy;
    }
    uint64_t end = os_get_cpu_cycle();

    double cycles_per_call = (double)(end - start) / iterations;
    double ns_per_call = os_cycles_to_ns((uint64_t)cycles_per_call);

    char details[256];
    snprintf(details, sizeof(details), "%.0f cycles (%.1f ns) per call",
             cycles_per_call, ns_per_call);
    test_result("os_get_cpu_cycle() overhead", (ns_per_call < 100), details);

    // Test os_cycles_to_ns() overhead
    start = os_get_cpu_cycle();
    for (int i = 0; i < iterations; i++) {
        volatile double dummy = os_cycles_to_ns(1000000);
        (void)dummy;
    }
    end = os_get_cpu_cycle();

    cycles_per_call = (double)(end - start) / iterations;
    ns_per_call = os_cycles_to_ns((uint64_t)cycles_per_call);

    snprintf(details, sizeof(details), "%.0f cycles (%.1f ns) per call",
             cycles_per_call, ns_per_call);
    test_result("os_cycles_to_ns() overhead", (ns_per_call < 100), details);

    // Test combined overhead (typical use case)
    start = os_get_cpu_cycle();
    for (int i = 0; i < iterations; i++) {
        uint64_t t1 = os_get_cpu_cycle();
        uint64_t t2 = os_get_cpu_cycle();
        volatile double elapsed = os_cycles_to_ns(t2 - t1);
        (void)elapsed;
    }
    end = os_get_cpu_cycle();

    cycles_per_call = (double)(end - start) / iterations;
    ns_per_call = os_cycles_to_ns((uint64_t)cycles_per_call);

    snprintf(details, sizeof(details), "%.0f cycles (%.1f ns) per measurement",
             cycles_per_call, ns_per_call);
    test_result("Full timing measurement overhead", (ns_per_call < 200), details);
}

// Test 5: Verify fixed-point conversion accuracy
static void test_conversion_accuracy(void) {
    printf("\n%s[Test 5: Fixed-Point Conversion Accuracy]%s\n", COLOR_BLUE, COLOR_RESET);

    // Test various cycle counts
    uint64_t test_cycles[] = {
        1,              // Minimum
        100,            // Small
        1000,           // Microsecond scale
        10000,          // Ten microsecond scale
        100000,         // Hundred microsecond scale
        1000000,        // Millisecond scale
        10000000,       // Ten millisecond scale
        100000000,      // Hundred millisecond scale
        1000000000,     // Second scale
        10000000000ULL, // Ten second scale
    };

    int all_accurate = 1;
    double max_error = 0.0;

    for (size_t i = 0; i < sizeof(test_cycles) / sizeof(test_cycles[0]); i++) {
        double ns = os_cycles_to_ns(test_cycles[i]);

        // Convert back using calibration to verify round-trip
        // (This tests internal consistency)
        // We expect: cycles * (ns/cycle) = ns
        // So: ns / (ns/cycle) should equal cycles

        // For this test, we just verify the conversion is reasonable
        // (non-negative, scales properly)
        // Skip very small values where fixed-point precision is limited
        if (i > 0 && test_cycles[i - 1] >= 100) {
            double prev_ns = os_cycles_to_ns(test_cycles[i - 1]);
            double ratio = ns / prev_ns;
            double expected_ratio = (double)test_cycles[i] / test_cycles[i - 1];

            // Only test if both values are valid
            if (prev_ns > 0.0 && expected_ratio > 0.0 && !isinf(ratio) && !isnan(ratio)) {
                double error = fabs(ratio - expected_ratio) / expected_ratio * 100.0;

                if (!isinf(error) && !isnan(error)) {
                    if (error > max_error) max_error = error;
                    if (error > 1.0) {  // 1% tolerance for fixed-point arithmetic
                        all_accurate = 0;
                    }
                }
            }
        }
    }

    char details[256];
    snprintf(details, sizeof(details), "max error: %.4f%%", max_error);
    test_result("Fixed-point conversion linearity", all_accurate, details);

    // Test that conversion is monotonic
    int monotonic = 1;
    for (size_t i = 1; i < sizeof(test_cycles) / sizeof(test_cycles[0]); i++) {
        double ns_curr = os_cycles_to_ns(test_cycles[i]);
        double ns_prev = os_cycles_to_ns(test_cycles[i - 1]);
        if (ns_curr <= ns_prev) {
            monotonic = 0;
            break;
        }
    }

    test_result("Conversion is monotonically increasing", monotonic, "");
}

// Test 6: Rapid successive measurements
static void test_rapid_measurements(void) {
    printf("\n%s[Test 6: Rapid Successive Measurements]%s\n", COLOR_BLUE, COLOR_RESET);

    const int iterations = 1000;
    uint64_t measurements[iterations];

    // Take rapid measurements
    for (int i = 0; i < iterations; i++) {
        measurements[i] = os_get_cpu_cycle();
    }

    // Verify all measurements are strictly increasing
    int strictly_increasing = 1;
    int zero_deltas = 0;
    uint64_t min_delta = UINT64_MAX;
    uint64_t max_delta = 0;

    for (int i = 1; i < iterations; i++) {
        if (measurements[i] <= measurements[i - 1]) {
            strictly_increasing = 0;
        }

        uint64_t delta = measurements[i] - measurements[i - 1];
        if (delta == 0) zero_deltas++;
        if (delta < min_delta) min_delta = delta;
        if (delta > max_delta) max_delta = delta;
    }

    char details[256];
    snprintf(details, sizeof(details),
             "min delta: %llu cycles (%.1f ns), max: %llu cycles (%.1f ns), zeros: %d",
             (unsigned long long)min_delta, os_cycles_to_ns(min_delta),
             (unsigned long long)max_delta, os_cycles_to_ns(max_delta),
             zero_deltas);
    test_result("Measurements strictly increasing", strictly_increasing, details);

    // Calculate resolution
    double avg_delta = 0.0;
    int valid_deltas = 0;
    for (int i = 1; i < iterations; i++) {
        uint64_t delta = measurements[i] - measurements[i - 1];
        if (delta < 10000) {  // Ignore outliers (context switches)
            avg_delta += delta;
            valid_deltas++;
        }
    }
    avg_delta /= valid_deltas;

    snprintf(details, sizeof(details), "avg: %.0f cycles (%.1f ns)",
             avg_delta, os_cycles_to_ns((uint64_t)avg_delta));
    test_result("Timer resolution", (avg_delta < 1000), details);
}

// Test 7: CPU frequency detection
static void test_cpu_frequency(void) {
    printf("\n%s[Test 7: CPU Frequency Detection]%s\n", COLOR_BLUE, COLOR_RESET);

    // Measure over 100ms for accurate frequency detection
    uint64_t tsc_start = os_get_cpu_cycle();
    uint64_t mono_start = get_monotonic_ns();

    sleep_ns(100000000);  // 100ms

    uint64_t tsc_end = os_get_cpu_cycle();
    uint64_t mono_end = get_monotonic_ns();

    uint64_t tsc_elapsed = tsc_end - tsc_start;
    uint64_t mono_elapsed = mono_end - mono_start;

    // Calculate TSC frequency
    double tsc_freq_ghz = (double)tsc_elapsed / (double)mono_elapsed;
    double tsc_freq_mhz = tsc_freq_ghz * 1000.0;

    // Typical modern CPUs: 1.5 GHz to 4.5 GHz
    int reasonable = (tsc_freq_ghz >= 1.0 && tsc_freq_ghz <= 6.0);

    char details[256];
    snprintf(details, sizeof(details), "TSC frequency: %.2f GHz (%.0f MHz)",
             tsc_freq_ghz, tsc_freq_mhz);
    test_result("TSC frequency in reasonable range", reasonable, details);

    // Calculate ns per cycle from frequency
    double ns_per_cycle_from_freq = 1.0 / tsc_freq_ghz;
    double ns_per_cycle_from_conv = os_cycles_to_ns(1000000) / 1000000.0;

    double freq_error = percent_error(ns_per_cycle_from_conv, ns_per_cycle_from_freq);
    int consistent = (freq_error < 1.0);

    snprintf(details, sizeof(details),
             "conversion: %.3f ns/cycle, frequency: %.3f ns/cycle, error: %.2f%%",
             ns_per_cycle_from_conv, ns_per_cycle_from_freq, freq_error);
    test_result("Calibration matches frequency", consistent, details);
}

// Test 8: Long-duration accuracy
static void test_long_duration(void) {
    printf("\n%s[Test 8: Long Duration Accuracy]%s\n", COLOR_BLUE, COLOR_RESET);

    printf("  (This test takes ~1 second to complete...)\n");

    // Test 1 second measurement
    uint64_t tsc_start = os_get_cpu_cycle();
    uint64_t mono_start = get_monotonic_ns();

    sleep_ns(1000000000);  // 1 second

    uint64_t tsc_end = os_get_cpu_cycle();
    uint64_t mono_end = get_monotonic_ns();

    uint64_t tsc_elapsed = tsc_end - tsc_start;
    double tsc_ns = os_cycles_to_ns(tsc_elapsed);
    double mono_ns = (double)(mono_end - mono_start);

    double error = percent_error(tsc_ns, mono_ns);
    int accurate = (error < 0.5);  // 0.5% tolerance for 1 second

    char details[256];
    snprintf(details, sizeof(details),
             "TSC: %.3f ms, MONOTONIC: %.3f ms, error: %.3f%%",
             tsc_ns / 1000000.0, mono_ns / 1000000.0, error);
    test_result("1 second measurement accuracy", accurate, details);
}

// Print system information
static void print_system_info(void) {
    printf("\n%s=== System Information ===%s\n", COLOR_BLUE, COLOR_RESET);

    // CPU information
    FILE *fp = fopen("/proc/cpuinfo", "r");
    if (fp) {
        char line[256];
        while (fgets(line, sizeof(line), fp)) {
            if (strncmp(line, "model name", 10) == 0) {
                char *name = strchr(line, ':');
                if (name) {
                    printf("CPU: %s", name + 2);
                }
                break;
            }
        }
        fclose(fp);
    }

    // Check for constant_tsc flag
    fp = popen("grep -m1 'flags' /proc/cpuinfo | grep -o 'constant_tsc\\|nonstop_tsc'", "r");
    if (fp) {
        char flags[128] = {0};
        if (fgets(flags, sizeof(flags), fp)) {
            printf("TSC features: %s", flags);
        }
        pclose(fp);
    }

    // Get current CPU frequency
    fp = popen("grep -m1 'cpu MHz' /proc/cpuinfo", "r");
    if (fp) {
        char freq[128];
        if (fgets(freq, sizeof(freq), fp)) {
            printf("Current CPU MHz: %s", strchr(freq, ':') + 2);
        }
        pclose(fp);
    }

    printf("\n");
}

int main(int argc, char *argv[]) {
    printf("\n");
    printf("╔══════════════════════════════════════════════════════════════════╗\n");
    printf("║       Clock Precision Test for x86 + Linux (TSC-based)         ║\n");
    printf("╚══════════════════════════════════════════════════════════════════╝\n");

    print_system_info();

    // Pin to CPU 0 for consistent measurements
    cpu_set_t cpuset;
    CPU_ZERO(&cpuset);
    CPU_SET(0, &cpuset);
    if (sched_setaffinity(0, sizeof(cpuset), &cpuset) == 0) {
        printf("%s⚙️  Pinned to CPU 0 for consistent measurements%s\n\n",
               COLOR_YELLOW, COLOR_RESET);
    }

    // Run all tests
    test_initialization();
    test_calibration_accuracy();
    test_calibration_drift();
    test_timing_overhead();
    test_conversion_accuracy();
    test_rapid_measurements();
    test_cpu_frequency();
    test_long_duration();

    // Print summary
    printf("\n");
    printf("╔══════════════════════════════════════════════════════════════════╗\n");
    printf("║                         Test Summary                            ║\n");
    printf("╚══════════════════════════════════════════════════════════════════╝\n");

    printf("  Total tests:  %d\n", total_tests);
    printf("  %sPassed:       %d%s\n", COLOR_GREEN, passed_tests, COLOR_RESET);
    if (failed_tests > 0) {
        printf("  %sFailed:       %d%s\n", COLOR_RED, failed_tests, COLOR_RESET);
    } else {
        printf("  Failed:       %d\n", failed_tests);
    }

    printf("\n");

    if (failed_tests == 0) {
        printf("  %s✓ All tests passed! TSC timing is accurate.%s\n\n",
               COLOR_GREEN, COLOR_RESET);
        return 0;
    } else {
        printf("  %s✗ Some tests failed. Review timing implementation.%s\n\n",
               COLOR_RED, COLOR_RESET);
        return 1;
    }
}
