#ifndef OS_H
#define OS_H

#include <stdint.h>

// CPU Affinity and Real-Time Priority Functions
//
// These functions provide cross-platform abstractions for controlling
// thread affinity and real-time scheduling priorities.

// Set CPU affinity for current thread
// cpu_id: CPU core ID to bind to (0-based)
// Returns: 0 on success, -1 on failure
// Note: macOS uses affinity tags (hints), Linux uses hard pinning
int os_set_thread_affinity(int cpu_id);

// Set real-time priority for current thread
// priority: Priority level (0-99, where 0 resets to normal, higher = more priority)
// Returns: 0 on success, -1 on failure
// Note: Requires root/CAP_SYS_NICE on most platforms
int os_set_thread_realtime_priority(int priority);

// Get CPU affinity for current thread
// Returns: CPU core ID if set, -1 on error or if not supported
// Note: macOS does not support querying affinity tags
int os_get_thread_affinity(void);

// Get real-time priority for current thread
// Returns: Priority level (0 = normal, >0 = real-time), -1 on error
int os_get_thread_realtime_priority(void);

// Set time-constraint policy (macOS only)
// This provides real-time guarantees based on time budgets
// period: Time period in nanoseconds
// computation: Maximum computation time per period in nanoseconds
// constraint: Hard deadline in nanoseconds
// preemptible: 1 if thread can be preempted, 0 for non-preemptible
// Returns: 0 on success, -1 on failure
// Example: os_set_time_constraint_policy(1000000, 500000, 900000, 0)
//          (1ms period, 500μs computation, 900μs deadline)
int os_set_time_constraint_policy(uint64_t period, uint64_t computation,
                                   uint64_t constraint, int preemptible);

// Verify environment for real-time performance
// verbose: 1 to print detailed information, 0 for silent check
// Returns: Number of warnings (0 = optimal configuration)
int os_verify_env(int verbose);

// High-precision timing and CPU cycle counting
//
// These functions provide cross-platform abstractions for reading CPU cycle
// counters and converting cycles to nanoseconds with platform-specific calibration.

// Get current CPU cycle count
// Returns: CPU cycle count (platform-specific: TSC on x86, mach_absolute_time on macOS ARM, clock_gettime on fallback)
// Note: This is a high-precision, low-overhead timing function suitable for hot-path latency measurements
uint64_t os_get_cpu_cycle(void);

// Convert CPU cycles to nanoseconds
// cycles: Cycle count from os_get_cpu_cycle()
// Returns: Equivalent time in nanoseconds (double precision)
// Note: Uses prewarmed calibration data for zero-branch hot-path performance
double os_cycles_to_ns(uint64_t cycles);

// Inline performance utilities for hot-path optimization
//
// These inline functions provide low-level performance hints and intrinsics
// for use in performance-critical code paths. Inspired by websocket_766 implementation.

// Direct RDTSC instruction for x86 (alternative to os_get_cpu_cycle for maximum performance)
// Returns: CPU timestamp counter value
// Note: May be out-of-order on some CPUs - use RDTSCP or barriers if strict ordering needed
static inline uint64_t os_rdtsc(void) {
#if defined(__x86_64__) || defined(__i386__)
    uint32_t lo, hi;
    __asm__ __volatile__ ("rdtsc" : "=a"(lo), "=d"(hi));
    return ((uint64_t)hi << 32) | lo;
#elif defined(__aarch64__) || defined(__ARM_ARCH_8A__)
    // ARM64: Use virtual counter (similar to TSC)
    uint64_t val;
    __asm__ __volatile__ ("mrs %0, cntvct_el0" : "=r"(val));
    return val;
#else
    // Fallback to os_get_cpu_cycle()
    return os_get_cpu_cycle();
#endif
}

// Prefetch memory into cache (hint to CPU)
// ptr: Pointer to memory address to prefetch
// Note: This is a hint - CPU may ignore it. Most useful when access pattern is predictable
static inline void os_prefetch(const void *ptr) {
    __builtin_prefetch(ptr, 0, 3);  // Read access, high temporal locality
}

// Prefetch for write (useful before modifying data)
// ptr: Pointer to memory address to prefetch
static inline void os_prefetch_write(const void *ptr) {
    __builtin_prefetch(ptr, 1, 3);  // Write access, high temporal locality
}

// Memory barrier - ensures all memory operations complete before continuing
// Use when ordering of memory operations matters (e.g., inter-thread communication)
static inline void os_memory_barrier(void) {
    __sync_synchronize();
}

// Compiler barrier - prevents compiler reordering (no CPU barrier)
// Use when you need to prevent compiler optimization from reordering operations
static inline void os_compiler_barrier(void) {
    __asm__ __volatile__ ("" ::: "memory");
}

// Pause/yield hint for spin-wait loops
// Improves performance and reduces power consumption in busy-wait scenarios
static inline void os_pause(void) {
#if defined(__x86_64__) || defined(__i386__)
    __asm__ __volatile__ ("pause");
#elif defined(__aarch64__) || defined(__ARM_ARCH_8A__)
    __asm__ __volatile__ ("yield");
#else
    // Fallback: no-op
    os_compiler_barrier();
#endif
}

#endif // OS_H
