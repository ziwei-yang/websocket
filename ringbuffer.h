#ifndef RINGBUFFER_H
#define RINGBUFFER_H

#include <stddef.h>
#include <stdint.h>

// Memory barriers for multi-core safety
#ifdef __aarch64__
// ARM64 (Apple Silicon, etc.) - use DMB (Data Memory Barrier)
#define WRITE_BARRIER() __asm__ __volatile__("dmb ishst" ::: "memory")
#define READ_BARRIER()  __asm__ __volatile__("dmb ishld" ::: "memory")
#elif defined(__x86_64__) || defined(__i386__)
// x86/x64 - TSO memory model, only need compiler barrier
#define WRITE_BARRIER() __asm__ __volatile__("" ::: "memory")
#define READ_BARRIER()  __asm__ __volatile__("" ::: "memory")
#else
// Generic fallback - compiler barrier
#define WRITE_BARRIER() __asm__ __volatile__("" ::: "memory")
#define READ_BARRIER()  __asm__ __volatile__("" ::: "memory")
#endif

// Platform-specific expectations for branch prediction
// Virtual memory mirroring works better on Linux than macOS
#ifdef __linux__
#define LIKELY_MIRRORED 1    // Linux: mirroring usually succeeds
#else
#define LIKELY_MIRRORED 0    // macOS/others: mirroring often fails, expect fallback
#endif

// Power-of-2 size for cheap modulo via bitwise AND
// 1u << 23 = 8,388,608 bytes = 8 MB
#define RINGBUFFER_SIZE (1u << 23)

// Compile-time assertion: ensure size is power of 2
_Static_assert((RINGBUFFER_SIZE & (RINGBUFFER_SIZE - 1)) == 0,
               "RINGBUFFER_SIZE must be a power of 2");

// Platform-specific cache line size
#if defined(__aarch64__) && defined(__APPLE__)
#define CACHE_LINE_SIZE 128  // Apple Silicon M1/M2/M3/M4
#else
#define CACHE_LINE_SIZE 64   // x86/x64, other ARM
#endif

typedef struct {
    //
    // === PRODUCER-OWNED CACHE LINE ===
    //
    uint8_t *pulled_data;       // Buffer pointer (shared read-only after init)
    size_t write_offset;        // Producer writes frequently
    int is_mmap;                // Initialization only (read-only after init)
    int is_mirrored;            // Virtual memory mirroring enabled (read-only after init)

    // Padding to next cache line boundary
    uint8_t _pad_producer[CACHE_LINE_SIZE - sizeof(uint8_t*) - sizeof(size_t) - 2*sizeof(int)];

    //
    // === CONSUMER-OWNED CACHE LINE ===
    //
    size_t read_offset;         // Consumer writes frequently

    // Padding to next cache line boundary
    uint8_t _pad_consumer[CACHE_LINE_SIZE - sizeof(size_t)];

} __attribute__((aligned(CACHE_LINE_SIZE))) ringbuffer_t;

// Initialize ring buffer
int ringbuffer_init(ringbuffer_t *rb);

// Free ring buffer
void ringbuffer_free(ringbuffer_t *rb);

// Get available space for writing (hot function - inlined for performance)
static inline size_t ringbuffer_available_write(const ringbuffer_t *rb) {
    if (!rb || !rb->pulled_data) return 0;

    size_t w = rb->write_offset;
    size_t r = rb->read_offset;

    // Branchless calculation using power-of-2 wraparound
    return (r - w - 1) & (RINGBUFFER_SIZE - 1);
}

// Get available data for reading (hot function - inlined for performance)
static inline size_t ringbuffer_available_read(const ringbuffer_t *rb) {
    if (!rb || !rb->pulled_data) return 0;

    size_t w = rb->write_offset;
    size_t r = rb->read_offset;

    // Branchless calculation using power-of-2 wraparound
    return (w - r) & (RINGBUFFER_SIZE - 1);
}

// Get write pointer for direct SSL_read() writes
// Returns pointer to writable space and available length
// After writing, call ringbuffer_commit_write() to update offset
void ringbuffer_get_write_ptr(ringbuffer_t *rb, uint8_t **data, size_t *len);

// Commit written data (advance write position)
void ringbuffer_commit_write(ringbuffer_t *rb, size_t len);

// Get next readable memory pointer and length (zero-copy)
// This supports reading without any memory copying
void ringbuffer_next_read(ringbuffer_t *rb, uint8_t **data, size_t *len);

// Peek at next readable data without advancing read position
void ringbuffer_peek_read(const ringbuffer_t *rb, uint8_t **data, size_t *len);

// Advance read position (consume data that was read)
void ringbuffer_advance_read(ringbuffer_t *rb, size_t len);

// Get ringbuffer status information
int ringbuffer_is_mirrored(const ringbuffer_t *rb);
int ringbuffer_is_mmap(const ringbuffer_t *rb);

#endif // RINGBUFFER_H
