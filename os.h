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

#endif // OS_H
