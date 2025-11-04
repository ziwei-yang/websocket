// _GNU_SOURCE is defined via Makefile (-D_GNU_SOURCE) for Linux builds
#include "os.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>
#include <pthread.h>
#include <time.h>

// Platform-specific includes for CPU affinity and real-time priority
#ifdef __linux__
#include <sched.h>
#include <sys/types.h>
#elif defined(__APPLE__)
#include <mach/thread_policy.h>
#include <mach/thread_act.h>
#include <mach/mach_init.h>
#include <mach/mach_time.h>
#endif

// Platform-specific includes for timing
#if defined(__i386__) || defined(__x86_64__)
#include <x86intrin.h>
#endif

// CPU Affinity and Real-Time Priority Implementation

#ifdef __linux__
// Linux implementation using pthread_setaffinity_np and sched_setscheduler

int os_set_thread_affinity(int cpu_id) {
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

int os_set_thread_realtime_priority(int priority) {
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

int os_get_thread_affinity(void) {
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

int os_get_thread_realtime_priority(void) {
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

int os_set_thread_affinity(int cpu_id) {
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

int os_set_thread_realtime_priority(int priority) {
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

int os_get_thread_affinity(void) {
    // macOS doesn't provide API to query affinity tags
    fprintf(stderr, "Thread affinity query not supported on macOS\n");
    return -1;
}

int os_get_thread_realtime_priority(void) {
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

int os_set_thread_affinity(int cpu_id) {
    (void)cpu_id;
    fprintf(stderr, "CPU affinity not supported on this platform\n");
    return -1;
}

int os_set_thread_realtime_priority(int priority) {
    (void)priority;
    fprintf(stderr, "Real-time priority not supported on this platform\n");
    return -1;
}

int os_get_thread_affinity(void) {
    return -1;
}

int os_get_thread_realtime_priority(void) {
    return -1;
}

#endif

// Time-constraint policy (macOS only)
int os_set_time_constraint_policy(uint64_t period, uint64_t computation,
                                   uint64_t constraint, int preemptible) {
#ifdef __APPLE__
    // Get timebase info for conversion
    mach_timebase_info_data_t timebase;
    mach_timebase_info(&timebase);

    // Convert nanoseconds to absolute time units
    // absolute_time = nanoseconds * denom / numer
    uint32_t period_abs = (uint32_t)(period * timebase.denom / timebase.numer);
    uint32_t computation_abs = (uint32_t)(computation * timebase.denom / timebase.numer);
    uint32_t constraint_abs = (uint32_t)(constraint * timebase.denom / timebase.numer);

    thread_time_constraint_policy_data_t policy;
    policy.period = period_abs;
    policy.computation = computation_abs;
    policy.constraint = constraint_abs;
    policy.preemptible = preemptible ? 1 : 0;

    thread_port_t thread_port = pthread_mach_thread_np(pthread_self());
    kern_return_t result = thread_policy_set(
        thread_port,
        THREAD_TIME_CONSTRAINT_POLICY,
        (thread_policy_t)&policy,
        THREAD_TIME_CONSTRAINT_POLICY_COUNT
    );

    if (result != KERN_SUCCESS) {
        fprintf(stderr, "Failed to set time constraint policy: %d\n", result);
        fprintf(stderr, "Hint: Requires root privileges on macOS\n");
        fprintf(stderr, "Period: %llu ns, Computation: %llu ns, Constraint: %llu ns\n",
                (unsigned long long)period,
                (unsigned long long)computation,
                (unsigned long long)constraint);
        return -1;
    }

    return 0;
#else
    (void)period;
    (void)computation;
    (void)constraint;
    (void)preemptible;
    fprintf(stderr, "Time-constraint policy only supported on macOS\n");
    return -1;
#endif
}

// Environment verification
int os_verify_env(int verbose) {
    int warnings = 0;

    if (verbose) {
        printf("=== OS Environment Verification ===\n\n");
    }

#ifdef __APPLE__
    // Check if running as root (needed for some RT features)
    if (geteuid() != 0) {
        warnings++;
        if (verbose) {
            printf("[WARN] Not running as root\n");
            printf("       Some real-time features require root privileges\n");
            printf("       Run with: sudo ./your_program\n\n");
        }
    } else {
        if (verbose) {
            printf("[OK] Running with root privileges\n\n");
        }
    }

    // Check CPU affinity (if set)
    int cpu_affinity = os_get_thread_affinity();
    if (cpu_affinity >= 0) {
        if (verbose) {
            printf("[OK] CPU affinity set to core %d\n\n", cpu_affinity);
        }
    } else {
        // On macOS, os_get_thread_affinity() always returns -1 (write-only API)
        // so don't count this as a warning
        if (verbose) {
            printf("[INFO] CPU affinity not readable on macOS (write-only API)\n");
            printf("       Call os_set_thread_affinity(cpu_id) to set affinity hint\n");
            printf("       Note: macOS affinity tags are hints, not strict bindings\n\n");
        }
    }

    // Check real-time priority
    int rt_priority = os_get_thread_realtime_priority();
    if (rt_priority > 0) {
        if (verbose) {
            printf("[OK] Real-time priority set: %d\n\n", rt_priority);
        }
    } else {
        warnings++;
        if (verbose) {
            printf("[WARN] Real-time priority not set\n");
            printf("       Call os_set_thread_realtime_priority(priority) for RT scheduling\n");
            printf("       Or use os_set_time_constraint_policy() for time-constraint policy\n\n");
        }
    }

    // Check system-wide settings
    if (verbose) {
        printf("[INFO] macOS System Configuration:\n");

        // Get number of CPUs
        int num_cpus = (int)sysconf(_SC_NPROCESSORS_ONLN);
        printf("       CPUs available: %d\n", num_cpus);

        // Get page size
        long page_size = sysconf(_SC_PAGESIZE);
        printf("       Page size: %ld bytes\n", page_size);

        // Check if running on ARM
#ifdef __aarch64__
        printf("       Architecture: ARM64 (Apple Silicon)\n");
        printf("       Using mach_absolute_time() for timing\n");
#else
        printf("       Architecture: x86_64 (Intel)\n");
        printf("       Using RDTSC for timing\n");
#endif

        printf("\n");
    }

    // Recommendations
    if (verbose && warnings > 0) {
        printf("=== Recommendations for Optimal Performance ===\n");
        printf("1. Run as root for full real-time capabilities\n");
        printf("2. Pin thread to isolated CPU core: os_set_thread_affinity()\n");
        printf("3. Set time-constraint policy: os_set_time_constraint_policy()\n");
        printf("4. Disable background processes and services\n");
        printf("5. Close unnecessary applications\n");
        printf("6. Ensure system is not under memory pressure\n");
        printf("\n");
    }

    if (verbose) {
        if (warnings == 0) {
            printf("=== Environment Check: PASSED ===\n");
        } else {
            printf("=== Environment Check: %d warning(s) ===\n", warnings);
        }
    }

#elif defined(__linux__)
    // Linux-specific checks
    if (verbose) {
        printf("Platform: Linux\n");
        printf("Note: Linux-specific verification not yet implemented\n");
        printf("      Check: CPU isolation, RT scheduling, IRQ affinity\n\n");
    }

#else
    if (verbose) {
        printf("Platform: Unknown\n");
        printf("Environment verification not supported on this platform\n\n");
    }
    warnings++;
#endif

    return warnings;
}

// ============================================================================
// High-Precision Timing and CPU Cycle Counting
// ============================================================================

// Platform-specific CPU cycle reading
#if defined(__i386__) || defined(__x86_64__)
// Use RDTSC instruction for CPU cycle counting on x86/x64
static inline uint64_t rdtsc(void) {
    return __rdtsc();
}
#elif defined(__aarch64__)
// ARM64 platforms
static inline uint64_t rdtsc(void) {
#ifdef __APPLE__
    // Use mach_absolute_time() on Apple Silicon for consistency with os_cycles_to_ns()
    return mach_absolute_time();
#else
    // On non-Apple ARM platforms, use the virtual counter
    uint64_t val;
    __asm__ volatile("mrs %0, cntvct_el0" : "=r" (val));
    return val;
#endif
}
#else
// Fallback: use clock_gettime() and return nanoseconds directly
static inline uint64_t rdtsc(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ULL + ts.tv_nsec;
}
#endif

// Prewarmed calibration data to eliminate initialization checks in hot path
#if defined(__APPLE__) && defined(__aarch64__)
static mach_timebase_info_data_t g_timebase = {0, 0};
#elif defined(__i386__) || defined(__x86_64__)
static double g_ns_per_cycle = 0.0;
#endif

// Comparison function for qsort (used for median calculation)
static int compare_double(const void *a, const void *b) {
    double diff = *(const double*)a - *(const double*)b;
    if (diff < 0) return -1;
    if (diff > 0) return 1;
    return 0;
}

// Initialize timer conversion - called lazily on first os_cycles_to_ns() call
static void init_timer_conversion(void) {
#if defined(__APPLE__) && defined(__aarch64__)
    mach_timebase_info(&g_timebase);
#elif defined(__i386__) || defined(__x86_64__)
    // Calibrate TSC frequency against CLOCK_MONOTONIC
    // Take 3 measurements and use median for robustness against interrupts
    double samples[3];

    for (int sample = 0; sample < 3; sample++) {
        struct timespec start, end;
        uint64_t tsc_start, tsc_end;

        clock_gettime(CLOCK_MONOTONIC, &start);
        tsc_start = rdtsc();

        // Sleep for 10ms to get accurate calibration
        struct timespec sleep_time = {0, 10000000};
        nanosleep(&sleep_time, NULL);

        clock_gettime(CLOCK_MONOTONIC, &end);
        tsc_end = rdtsc();

        uint64_t ns_elapsed = (end.tv_sec - start.tv_sec) * 1000000000ULL +
                             (end.tv_nsec - start.tv_nsec);
        uint64_t cycles_elapsed = tsc_end - tsc_start;

        samples[sample] = (double)ns_elapsed / (double)cycles_elapsed;
    }

    // Use median to filter out outliers
    qsort(samples, 3, sizeof(double), compare_double);
    g_ns_per_cycle = samples[1];  // Middle value
#endif
}

// Public API: Get current CPU cycle count
uint64_t os_get_cpu_cycle(void) {
    // Initialize timer conversion on first call (moved here from os_cycles_to_ns)
    static int timer_initialized = 0;
    if (__builtin_expect(!timer_initialized, 0)) {
        init_timer_conversion();
        timer_initialized = 1;
    }
    return rdtsc();
}

// Public API: Convert CPU cycles to nanoseconds (branch-free, integer-only hot path)
// Note: Calls init if needed (rare on first call only)
double os_cycles_to_ns(uint64_t cycles) {
#if defined(__APPLE__) && defined(__aarch64__)
    // Ensure initialization (only happens once, on first call if not already done)
    if (__builtin_expect(g_timebase.denom == 0, 0)) {
        init_timer_conversion();
    }

    // macOS ARM - mach_absolute_time to nanoseconds using integer math
    // Common case: numer=125, denom=3 (M1/M2/M3/M4)
    // cycles * 125 / 3 = (cycles * 42667) >> 10 (error < 0.01%)
    if (g_timebase.numer == 125 && g_timebase.denom == 3) {
        // Check for potential overflow
        if (cycles > (UINT64_MAX / 42667ULL)) {
            // Fall back to division method for large values
            return (double)(cycles * g_timebase.numer / g_timebase.denom);
        }
        // Fast path: fixed-point multiplication (5-10x faster than FP)
        return (double)((cycles * 42667ULL) >> 10);
    }
    // Generic fallback: integer division (still faster than FP)
    return (double)(cycles * g_timebase.numer / g_timebase.denom);
#elif defined(__i386__) || defined(__x86_64__)
    // Ensure initialization (only happens once, on first call if not already done)
    if (__builtin_expect(g_ns_per_cycle == 0.0, 0)) {
        init_timer_conversion();
    }

    // x86/x64 - TSC cycles to nanoseconds
    // Use fixed-point for common frequencies (2.0-4.0 GHz range)
    // Pre-compute: g_ns_per_cycle_fp = (uint64_t)(g_ns_per_cycle * (1ULL << 32))
    static uint64_t g_ns_per_cycle_fp = 0;
    if (__builtin_expect(g_ns_per_cycle_fp == 0, 0)) {
        g_ns_per_cycle_fp = (uint64_t)(g_ns_per_cycle * 4294967296.0);
    }

    // Check for overflow
    if (cycles > (UINT64_MAX / g_ns_per_cycle_fp)) {
        // Fall back to floating-point for very large values
        return cycles * g_ns_per_cycle;
    }
    return (double)((cycles * g_ns_per_cycle_fp) >> 32);
#else
    // Fallback - cycles are already in nanoseconds from clock_gettime
    return (double)cycles;
#endif
}
