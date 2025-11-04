#include "ringbuffer.h"
#include <string.h>
#include <stdlib.h>
#include <stdatomic.h>
#include <sys/mman.h>
#include <stdio.h>
#include <fcntl.h>
#include <unistd.h>

#ifdef __APPLE__
#include <sys/shm.h>
#endif

// Platform-specific prefetch intrinsics
#if defined(__x86_64__) || defined(__i386__)
#include <emmintrin.h>  // SSE2 for _mm_prefetch
#endif

// Enable hugepages/superpages by default on supported platforms
#ifndef WS_DISABLE_HUGEPAGES
#define WS_USE_HUGEPAGES 1
#endif

// Try to create virtual memory mirroring for zero-wraparound ringbuffer
// Returns 0 on success, -1 on failure
static int try_create_mirrored_buffer(ringbuffer_t *rb) {
#if defined(__APPLE__) || defined(__linux__)
    // Step 1: Reserve virtual address space (2x size)
    void *addr = mmap(NULL, 2 * RINGBUFFER_SIZE, PROT_NONE,
                      MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (addr == MAP_FAILED) {
        return -1;
    }

    // Step 2: Create shared memory
    int fd;
#ifdef __APPLE__
    // macOS: Use temporary file for shared memory
    // Use atomic counter for unique naming to avoid collisions
    static _Atomic int shm_counter = 0;
    int counter = __atomic_fetch_add(&shm_counter, 1, __ATOMIC_SEQ_CST);

    char shm_name[256];
    // Check snprintf return value
    int ret = snprintf(shm_name, sizeof(shm_name), "/tmp/ringbuffer_%d_%d_%lx",
                       getpid(), counter, (unsigned long)(uintptr_t)rb);
    if (ret < 0 || ret >= (int)sizeof(shm_name)) {
        munmap(addr, 2 * RINGBUFFER_SIZE);
        return -1;
    }

    fd = shm_open(shm_name, O_RDWR | O_CREAT | O_EXCL, 0600);
    if (fd < 0) {
        munmap(addr, 2 * RINGBUFFER_SIZE);
        return -1;
    }
    shm_unlink(shm_name);  // Unlink immediately (file deleted when last fd closed)
#else
    // Linux: Use memfd_create if available, otherwise shm_open
    fd = memfd_create("ringbuffer", 0);
    if (fd < 0) {
        // Use atomic counter for unique naming
        static _Atomic int shm_counter = 0;
        int counter = __atomic_fetch_add(&shm_counter, 1, __ATOMIC_SEQ_CST);

        char shm_name[256];
        // Check snprintf return value
        int ret = snprintf(shm_name, sizeof(shm_name), "/ringbuffer_%d_%d_%lx",
                           getpid(), counter, (unsigned long)(uintptr_t)rb);
        if (ret < 0 || ret >= (int)sizeof(shm_name)) {
            munmap(addr, 2 * RINGBUFFER_SIZE);
            return -1;
        }

        fd = shm_open(shm_name, O_RDWR | O_CREAT | O_EXCL, 0600);
        if (fd < 0) {
            munmap(addr, 2 * RINGBUFFER_SIZE);
            return -1;
        }
        shm_unlink(shm_name);
    }
#endif

    // Step 3: Size the shared memory
    if (ftruncate(fd, RINGBUFFER_SIZE) != 0) {
        close(fd);
        munmap(addr, 2 * RINGBUFFER_SIZE);
        return -1;
    }

    // Step 4: Map first half
    void *addr1 = mmap(addr, RINGBUFFER_SIZE, PROT_READ | PROT_WRITE,
                       MAP_FIXED | MAP_SHARED, fd, 0);
    if (addr1 == MAP_FAILED || addr1 != addr) {
        close(fd);
        munmap(addr, 2 * RINGBUFFER_SIZE);
        return -1;
    }

    // Step 5: Map second half (same physical memory)
    void *addr2 = mmap((uint8_t*)addr + RINGBUFFER_SIZE, RINGBUFFER_SIZE,
                       PROT_READ | PROT_WRITE, MAP_FIXED | MAP_SHARED, fd, 0);
    if (addr2 == MAP_FAILED || addr2 != (uint8_t*)addr + RINGBUFFER_SIZE) {
        // Properly clean up both mappings on failure
        munmap(addr1, RINGBUFFER_SIZE);
        munmap((uint8_t*)addr + RINGBUFFER_SIZE, RINGBUFFER_SIZE);
        close(fd);
        return -1;
    }

    // Success! No longer need the fd
    close(fd);

    rb->pulled_data = (uint8_t*)addr;
    rb->is_mmap = 1;
    rb->is_mirrored = 1;

    return 0;
#else
    (void)rb;
    return -1;  // Not supported on this platform
#endif
}

// Software prefetch helpers
static inline void prefetch_read(const void *addr) {
#if defined(__x86_64__) || defined(__i386__)
    _mm_prefetch((const char*)addr, _MM_HINT_T0);  // Prefetch to L1
#elif defined(__aarch64__)
    __asm__ __volatile__("prfm pldl1keep, [%0]" : : "r"(addr));
#else
    __builtin_prefetch(addr, 0, 3);  // GCC/Clang generic
#endif
}

static inline void prefetch_write(void *addr) {
#if defined(__x86_64__) || defined(__i386__)
    _mm_prefetch((const char*)addr, _MM_HINT_T0);
#elif defined(__aarch64__)
    __asm__ __volatile__("prfm pstl1keep, [%0]" : : "r"(addr));
#else
    __builtin_prefetch(addr, 1, 3);
#endif
}

int ringbuffer_init(ringbuffer_t *rb) {
    if (!rb) return -1;

    // Ensure structure itself is cache-line aligned
    if (((uintptr_t)rb) % CACHE_LINE_SIZE != 0) {
        fprintf(stderr, "Warning: ringbuffer_t not cache-line aligned\n");
        fprintf(stderr, "Allocate with posix_memalign() for best performance\n");
    }

    rb->read_offset = 0;
    rb->write_offset = 0;
    rb->is_mirrored = 0;

    // Try virtual memory mirroring first (best performance)
    if (try_create_mirrored_buffer(rb) == 0) {
        // Success - mirrored buffer allocated
        return 0;
    }

    // Mirroring failed, fall back to regular allocation
    fprintf(stderr, "Info: Virtual memory mirroring failed, using regular allocation\n");

#ifdef WS_USE_HUGEPAGES
#ifdef __linux__
    // Linux: Try to use hugepages (2MB)
    rb->pulled_data = (uint8_t *)mmap(NULL, RINGBUFFER_SIZE,
                                      PROT_READ | PROT_WRITE,
                                      MAP_PRIVATE | MAP_ANONYMOUS | MAP_HUGETLB,
                                      -1, 0);

    if (rb->pulled_data == MAP_FAILED) {
        // Fallback to regular pages with cache-line alignment
        fprintf(stderr, "Warning: Hugepage allocation failed, using aligned malloc\n");
        if (posix_memalign((void**)&rb->pulled_data, CACHE_LINE_SIZE, RINGBUFFER_SIZE) != 0) {
            return -1;
        }
        rb->is_mmap = 0;
    } else {
        rb->is_mmap = 1;
    }
#elif defined(__APPLE__)
    // macOS: Use superpages (VM_FLAGS_SUPERPAGE_SIZE_2MB)
    // Note: macOS will automatically use superpages for large allocations
    // We use mmap with alignment hints
    rb->pulled_data = (uint8_t *)mmap(NULL, RINGBUFFER_SIZE,
                                      PROT_READ | PROT_WRITE,
                                      MAP_PRIVATE | MAP_ANONYMOUS,
                                      -1, 0);

    if (rb->pulled_data != MAP_FAILED) {
        // Advise the kernel to use superpages for this region
        // MADV_WILLNEED helps with superpage allocation
        madvise(rb->pulled_data, RINGBUFFER_SIZE, MADV_WILLNEED);
        rb->is_mmap = 1;
    } else {
        // Fallback: aligned malloc
        if (posix_memalign((void**)&rb->pulled_data, CACHE_LINE_SIZE, RINGBUFFER_SIZE) != 0) {
            return -1;
        }
        rb->is_mmap = 0;
    }
#else
    // Other platforms: use regular mmap
    rb->pulled_data = (uint8_t *)mmap(NULL, RINGBUFFER_SIZE,
                                      PROT_READ | PROT_WRITE,
                                      MAP_PRIVATE | MAP_ANONYMOUS,
                                      -1, 0);

    if (rb->pulled_data == MAP_FAILED) {
        // Fallback to aligned malloc
        if (posix_memalign((void**)&rb->pulled_data, CACHE_LINE_SIZE, RINGBUFFER_SIZE) != 0) {
            return -1;
        }
        rb->is_mmap = 0;
    } else {
        rb->is_mmap = 1;
    }
#endif
#else
    // Hugepages disabled, use cache-line aligned allocation
    if (posix_memalign((void**)&rb->pulled_data, CACHE_LINE_SIZE, RINGBUFFER_SIZE) != 0) {
        return -1;
    }
    rb->is_mmap = 0;
#endif

    return 0;
}

void ringbuffer_free(ringbuffer_t *rb) {
    if (rb && rb->pulled_data) {
        if (rb->is_mmap) {
            // Unmap mirrored buffer (2x size) or regular mmap
            size_t unmap_size = rb->is_mirrored ? (2 * RINGBUFFER_SIZE) : RINGBUFFER_SIZE;
            munmap(rb->pulled_data, unmap_size);
        } else {
            free(rb->pulled_data);
        }
        rb->pulled_data = NULL;
    }
}

// Note: ringbuffer_available_write() and ringbuffer_available_read() are now
// inlined in ringbuffer.h for optimal performance

void ringbuffer_get_write_ptr(ringbuffer_t *rb, uint8_t **data, size_t *len) {
    if (__builtin_expect(!rb || !rb->pulled_data || !data || !len, 0)) {
        if (data) *data = NULL;
        if (len) *len = 0;
        return;
    }

    size_t available = ringbuffer_available_write(rb);

    if (__builtin_expect(available == 0, 0)) {
        *data = NULL;
        *len = 0;
        return;
    }

    // Always return contiguous space
    *data = &rb->pulled_data[rb->write_offset];

    // Aggressive prefetching for streaming writes (typical message sizes: 150-200 bytes)
    prefetch_write(*data);

    if (__builtin_expect(available > CACHE_LINE_SIZE, 1)) {
        prefetch_write(*data + CACHE_LINE_SIZE);

        // Prefetch third cache line for typical message sizes (>128 bytes)
        if (__builtin_expect(available > 2 * CACHE_LINE_SIZE, 1)) {
            prefetch_write(*data + 2 * CACHE_LINE_SIZE);
        }

        // For very large messages (>256 bytes), prefetch even further
        if (__builtin_expect(available > 4 * CACHE_LINE_SIZE, 0)) {
            prefetch_write(*data + 4 * CACHE_LINE_SIZE);
        }
    }

    if (__builtin_expect(rb->is_mirrored, LIKELY_MIRRORED)) {
        // Mirrored buffer: always contiguous, no wraparound logic!
        *len = available;
    } else {
        // Non-mirrored: need to handle wraparound
        size_t space_to_end = RINGBUFFER_SIZE - rb->write_offset;

        if (__builtin_expect(rb->write_offset >= rb->read_offset, 1)) {
            // Write pointer ahead, can write to end but leave 1 byte buffer
            if (__builtin_expect(space_to_end > 1, 1)) {
                *len = space_to_end - 1;
                if (*len > available) *len = available;
            } else {
                *len = 0;
            }
        } else {
            // Read pointer ahead, can write up to read pointer
            *len = rb->read_offset - rb->write_offset - 1;
        }
    }
}

void ringbuffer_commit_write(ringbuffer_t *rb, size_t len) {
    if (__builtin_expect(!rb || len == 0, 0)) return;

    size_t available = ringbuffer_available_write(rb);
    if (__builtin_expect(len > available, 0)) len = available;

    // Ensure all data writes complete before updating offset (critical for ARM)
    WRITE_BARRIER();

    // Optimized: bitwise AND instead of expensive modulo
    rb->write_offset = (rb->write_offset + len) & (RINGBUFFER_SIZE - 1);
}

void ringbuffer_next_read(ringbuffer_t *rb, uint8_t **data, size_t *len) {
    if (__builtin_expect(!rb || !rb->pulled_data || !data || !len, 0)) {
        if (data) *data = NULL;
        if (len) *len = 0;
        return;
    }

    // Ensure offset read completes before accessing data (critical for ARM)
    READ_BARRIER();

    size_t available = ringbuffer_available_read(rb);

    if (__builtin_expect(available == 0, 0)) {
        *data = NULL;
        *len = 0;
        return;
    }

    // Return pointer to readable data
    *data = &rb->pulled_data[rb->read_offset];

    if (__builtin_expect(rb->is_mirrored, LIKELY_MIRRORED)) {
        // Mirrored buffer: always contiguous, no wraparound logic!
        *len = available;
    } else {
        // Non-mirrored: need to handle wraparound
        if (__builtin_expect(rb->write_offset >= rb->read_offset, 1)) {
            // Contiguous data (common case)
            *len = rb->write_offset - rb->read_offset;
        } else {
            // Wrapped around - return first contiguous chunk
            *len = RINGBUFFER_SIZE - rb->read_offset;
        }

        if (__builtin_expect(*len > available, 0)) *len = available;
    }
}

void ringbuffer_peek_read(const ringbuffer_t *rb, uint8_t **data, size_t *len) {
    if (__builtin_expect(!rb || !rb->pulled_data || !data || !len, 0)) {
        if (data) *data = NULL;
        if (len) *len = 0;
        return;
    }

    // Ensure offset read completes before accessing data (critical for ARM)
    READ_BARRIER();

    size_t available = ringbuffer_available_read(rb);

    if (__builtin_expect(available == 0, 0)) {
        *data = NULL;
        *len = 0;
        return;
    }

    // Return pointer to readable data (const version, doesn't modify rb)
    *data = (uint8_t *)&rb->pulled_data[rb->read_offset];

    // Prefetch next cache line(s) immediately
    if (__builtin_expect(available > CACHE_LINE_SIZE, 1)) {
        prefetch_read(*data + CACHE_LINE_SIZE);

        // For large available data, prefetch further ahead
        if (__builtin_expect(available > 256, 1)) {
            prefetch_read(*data + 256);
        }
    }

    if (__builtin_expect(rb->is_mirrored, LIKELY_MIRRORED)) {
        // Mirrored buffer: always contiguous, no wraparound logic!
        *len = available;
    } else {
        // Non-mirrored: need to handle wraparound
        if (__builtin_expect(rb->write_offset >= rb->read_offset, 1)) {
            // Contiguous data (common case)
            *len = rb->write_offset - rb->read_offset;
        } else {
            // Wrapped around - return first contiguous chunk
            *len = RINGBUFFER_SIZE - rb->read_offset;
        }

        if (__builtin_expect(*len > available, 0)) *len = available;
    }
}

void ringbuffer_advance_read(ringbuffer_t *rb, size_t len) {
    if (__builtin_expect(!rb || !rb->pulled_data || len == 0, 0)) return;

    size_t available = ringbuffer_available_read(rb);
    if (__builtin_expect(len > available, 0)) len = available;

    // Optimized: bitwise AND instead of expensive modulo
    rb->read_offset = (rb->read_offset + len) & (RINGBUFFER_SIZE - 1);
}

int ringbuffer_is_mirrored(const ringbuffer_t *rb) {
    if (!rb) return 0;
    return rb->is_mirrored;
}

int ringbuffer_is_mmap(const ringbuffer_t *rb) {
    if (!rb) return 0;
    return rb->is_mmap;
}
