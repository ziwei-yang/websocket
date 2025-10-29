#ifndef RINGBUFFER_H
#define RINGBUFFER_H

#include <stddef.h>
#include <stdint.h>

#define RINGBUFFER_SIZE (10 * 1024 * 1024)  // 10MB

typedef struct {
    uint8_t *pulled_data;  // Dynamically allocated buffer
    size_t read_offset;    // Current read offset
    size_t write_offset;   // Current write offset
} ringbuffer_t;

// Initialize ring buffer
int ringbuffer_init(ringbuffer_t *rb);

// Free ring buffer
void ringbuffer_free(ringbuffer_t *rb);

// Get available space for writing
size_t ringbuffer_available_write(const ringbuffer_t *rb);

// Get available data for reading
size_t ringbuffer_available_read(const ringbuffer_t *rb);

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

// Write data to ring buffer (single writer only) - wrapper for convenience
size_t ringbuffer_write(ringbuffer_t *rb, const uint8_t *data, size_t len);

// Read data from ring buffer (single reader only) - wrapper for convenience
size_t ringbuffer_read(ringbuffer_t *rb, uint8_t *data, size_t len);

#endif // RINGBUFFER_H
