#include <stdarg.h>
#include <stddef.h>
#include <setjmp.h>
#include <cmocka.h>
#include "../ringbuffer.h"
#include <string.h>
#include <stdio.h>
#include <stdlib.h>

// Test: Ring buffer initialization
static void test_ringbuffer_init(void **state) {
    (void)state;
    ringbuffer_t rb;
    int result = ringbuffer_init(&rb);
    
    assert_int_equal(result, 0);
    assert_int_equal(rb.read_offset, 0);
    assert_int_equal(rb.write_offset, 0);
    
    ringbuffer_free(&rb);
}

// Test: Available write space calculation
static void test_ringbuffer_available_write(void **state) {
    (void)state;
    ringbuffer_t rb;
    ringbuffer_init(&rb);
    
    // Initially should have almost full capacity (minus 1 to distinguish full from empty)
    size_t available = ringbuffer_available_write(&rb);
    assert_true(available > 0);
    assert_true(available < RINGBUFFER_SIZE);
    
    ringbuffer_free(&rb);
}

// Test: Available read space (initially empty)
static void test_ringbuffer_available_read_empty(void **state) {
    (void)state;
    ringbuffer_t rb;
    ringbuffer_init(&rb);
    
    size_t available = ringbuffer_available_read(&rb);
    assert_int_equal(available, 0);
    
    ringbuffer_free(&rb);
}

// Test: Write and read basic data
static void test_ringbuffer_write_read(void **state) {
    (void)state;
    ringbuffer_t rb;
    ringbuffer_init(&rb);
    
    const char *test_data = "Hello, RingBuffer!";
    size_t data_len = strlen(test_data);
    
    // Write data
    size_t written = ringbuffer_write(&rb, (const uint8_t *)test_data, data_len);
    assert_int_equal(written, data_len);
    
    // Check available read
    size_t available = ringbuffer_available_read(&rb);
    assert_int_equal(available, data_len);
    
    // Read data back
    uint8_t read_buffer[128];
    size_t read = ringbuffer_read(&rb, read_buffer, sizeof(read_buffer));
    assert_int_equal(read, data_len);
    assert_memory_equal(read_buffer, test_data, data_len);
    
    ringbuffer_free(&rb);
}

// Test: Zero-copy write pointer
static void test_ringbuffer_get_write_ptr(void **state) {
    (void)state;
    ringbuffer_t rb;
    ringbuffer_init(&rb);
    
    uint8_t *write_ptr = NULL;
    size_t write_len = 0;
    
    ringbuffer_get_write_ptr(&rb, &write_ptr, &write_len);
    
    assert_non_null(write_ptr);
    assert_true(write_len > 0);
    
    // Write some data directly
    const char *test = "Test";
    memcpy(write_ptr, test, 4);
    ringbuffer_commit_write(&rb, 4);
    
    // Verify
    size_t available = ringbuffer_available_read(&rb);
    assert_int_equal(available, 4);
    
    ringbuffer_free(&rb);
}

// Test: Zero-copy read pointer
static void test_ringbuffer_next_read(void **state) {
    (void)state;
    ringbuffer_t rb;
    ringbuffer_init(&rb);
    
    // Write some data
    const char *test_data = "Hello";
    ringbuffer_write(&rb, (const uint8_t *)test_data, 5);
    
    // Get read pointer
    uint8_t *read_ptr = NULL;
    size_t read_len = 0;
    
    ringbuffer_next_read(&rb, &read_ptr, &read_len);
    
    assert_non_null(read_ptr);
    assert_int_equal(read_len, 5);
    assert_memory_equal(read_ptr, test_data, 5);
    
    // Advance read position
    ringbuffer_advance_read(&rb, 5);
    
    // Should be empty now
    ringbuffer_next_read(&rb, &read_ptr, &read_len);
    assert_int_equal(read_len, 0);
    
    ringbuffer_free(&rb);
}

// Test: Peek read (doesn't consume data)
static void test_ringbuffer_peek_read(void **state) {
    (void)state;
    ringbuffer_t rb;
    ringbuffer_init(&rb);
    
    const char *test_data = "Peek Test";
    ringbuffer_write(&rb, (const uint8_t *)test_data, 9);
    
    // Peek first time
    uint8_t *read_ptr1 = NULL;
    size_t read_len1 = 0;
    ringbuffer_peek_read(&rb, &read_ptr1, &read_len1);
    
    assert_non_null(read_ptr1);
    assert_int_equal(read_len1, 9);
    
    // Peek again - should get same data
    uint8_t *read_ptr2 = NULL;
    size_t read_len2 = 0;
    ringbuffer_peek_read(&rb, &read_ptr2, &read_len2);
    
    assert_non_null(read_ptr2);
    assert_int_equal(read_len2, 9);
    assert_ptr_equal(read_ptr1, read_ptr2);
    
    // Verify available read hasn't changed
    size_t available = ringbuffer_available_read(&rb);
    assert_int_equal(available, 9);
    
    ringbuffer_free(&rb);
}

// Test: Wrap-around (write near end, read wraps)
static void test_ringbuffer_wraparound(void **state) {
    (void)state;
    ringbuffer_t rb;
    ringbuffer_init(&rb);
    
    // First, write enough data to move the write pointer near the end
    size_t write_size = RINGBUFFER_SIZE - 1000;
    uint8_t *dummy_data = malloc(write_size);
    for (size_t i = 0; i < write_size; i++) {
        dummy_data[i] = (uint8_t)i;
    }
    
    size_t written1 = ringbuffer_write(&rb, dummy_data, write_size);
    assert_int_equal(written1, write_size);
    
    // Read enough to move read pointer forward (leaving data near the end)
    size_t read_size = write_size - 500;
    uint8_t *read_data = malloc(read_size);
    size_t read1 = ringbuffer_read(&rb, read_data, read_size);
    assert_int_equal(read1, read_size);
    
    // Now we have read and write positions near the end of the buffer
    // Write data that will wrap around
    const char *test_data = "Wrap Data";
    size_t data_len = strlen(test_data);
    
    size_t written2 = ringbuffer_write(&rb, (const uint8_t *)test_data, data_len);
    assert_int_equal(written2, data_len);
    
    // Read the rest of the old data
    size_t remaining_old = ringbuffer_available_read(&rb) - data_len;
    free(read_data);
    read_data = malloc(remaining_old);
    ringbuffer_read(&rb, read_data, remaining_old);
    
    // Read the wrapped data
    uint8_t read_buffer[128];
    size_t read2 = ringbuffer_read(&rb, read_buffer, sizeof(read_buffer));
    assert_int_equal(read2, data_len);
    assert_memory_equal(read_buffer, test_data, data_len);
    
    free(dummy_data);
    free(read_data);
    ringbuffer_free(&rb);
}

// Test: Large data write and read
static void test_ringbuffer_large_data(void **state) {
    (void)state;
    ringbuffer_t rb;
    ringbuffer_init(&rb);
    
    // Allocate and fill large buffer
    size_t large_size = 1024 * 1024;  // 1MB
    uint8_t *write_data = malloc(large_size);
    uint8_t *read_data = malloc(large_size);
    
    // Fill with pattern
    for (size_t i = 0; i < large_size; i++) {
        write_data[i] = (uint8_t)(i % 256);
    }
    
    // Write
    size_t written = ringbuffer_write(&rb, write_data, large_size);
    assert_int_equal(written, large_size);
    
    // Read
    size_t read = ringbuffer_read(&rb, read_data, large_size);
    assert_int_equal(read, large_size);
    
    // Verify
    assert_memory_equal(read_data, write_data, large_size);
    
    free(write_data);
    free(read_data);
    ringbuffer_free(&rb);
}

// Test: Full buffer (write until full)
static void test_ringbuffer_full_buffer(void **state) {
    (void)state;
    ringbuffer_t rb;
    ringbuffer_init(&rb);
    
    size_t max_write = ringbuffer_available_write(&rb);
    uint8_t *test_data = malloc(max_write);
    
    for (size_t i = 0; i < max_write; i++) {
        test_data[i] = (uint8_t)(i % 256);
    }
    
    size_t written = ringbuffer_write(&rb, test_data, max_write);
    assert_int_equal(written, max_write);
    
    // Should be full now
    size_t available = ringbuffer_available_write(&rb);
    assert_int_equal(available, 0);
    
    // Can still read
    available = ringbuffer_available_read(&rb);
    assert_int_equal(available, max_write);
    
    free(test_data);
    ringbuffer_free(&rb);
}

// Test: Overwrite detection (trying to write beyond capacity)
static void test_ringbuffer_write_beyond_capacity(void **state) {
    (void)state;
    ringbuffer_t rb;
    ringbuffer_init(&rb);
    
    size_t max_write = ringbuffer_available_write(&rb);
    
    uint8_t *test_data = malloc(max_write + 1000);
    memset(test_data, 0xAA, max_write + 1000);
    
    // Write max capacity
    size_t written1 = ringbuffer_write(&rb, test_data, max_write);
    assert_int_equal(written1, max_write);
    
    // Try to write more - should only write available space
    size_t written2 = ringbuffer_write(&rb, test_data, 1000);
    assert_int_equal(written2, 0);  // Should be 0 since buffer is full
    
    free(test_data);
    ringbuffer_free(&rb);
}

// Test: Sequential writes and reads
static void test_ringbuffer_sequential(void **state) {
    (void)state;
    ringbuffer_t rb;
    ringbuffer_init(&rb);
    
    // Write multiple chunks
    const char *chunks[] = {"Chunk1", "Chunk2", "Chunk3", "Chunk4"};
    const int num_chunks = 4;
    
    for (int i = 0; i < num_chunks; i++) {
        size_t len = strlen(chunks[i]);
        size_t written = ringbuffer_write(&rb, (const uint8_t *)chunks[i], len);
        assert_int_equal(written, len);
    }
    
    // Read them back
    uint8_t read_buffer[256];
    size_t offset = 0;
    
    for (int i = 0; i < num_chunks; i++) {
        size_t len = strlen(chunks[i]);
        size_t read = ringbuffer_read(&rb, read_buffer + offset, len);
        assert_int_equal(read, len);
        offset += len;
    }
    
    // Verify complete message
    assert_memory_equal(read_buffer, "Chunk1Chunk2Chunk3Chunk4", offset);
    
    ringbuffer_free(&rb);
}

// Test: Zero-length operations
static void test_ringbuffer_zero_length(void **state) {
    (void)state;
    ringbuffer_t rb;
    ringbuffer_init(&rb);
    
    // Write zero bytes
    size_t written = ringbuffer_write(&rb, NULL, 0);
    assert_int_equal(written, 0);
    
    // Read zero bytes
    uint8_t buffer[128];
    size_t read = ringbuffer_read(&rb, buffer, 0);
    assert_int_equal(read, 0);
    
    ringbuffer_free(&rb);
}

// Test: NULL pointer handling for initialization
static void test_ringbuffer_init_null(void **state) {
    (void)state;
    int result = ringbuffer_init(NULL);
    assert_int_equal(result, -1);
}

// Test: NULL pointer handling for available_write
static void test_ringbuffer_available_write_null(void **state) {
    (void)state;
    size_t available = ringbuffer_available_write(NULL);
    assert_int_equal(available, 0);
}

// Test: NULL pointer handling for available_read
static void test_ringbuffer_available_read_null(void **state) {
    (void)state;
    size_t available = ringbuffer_available_read(NULL);
    assert_int_equal(available, 0);
}

// Test: NULL pointer handling for get_write_ptr
static void test_ringbuffer_get_write_ptr_null(void **state) {
    (void)state;
    uint8_t *data = (uint8_t*)0x12345678;  // Non-null to test if it gets set to NULL
    size_t len = 0x12345678;  // Non-zero to test if it gets set to 0
    
    ringbuffer_get_write_ptr(NULL, &data, &len);
    assert_null(data);
    assert_int_equal(len, 0);
}

// Test: NULL pointer handling for get_write_ptr with NULL data pointer
static void test_ringbuffer_get_write_ptr_null_data(void **state) {
    (void)state;
    ringbuffer_t rb;
    ringbuffer_init(&rb);
    
    size_t len = 0x12345678;
    ringbuffer_get_write_ptr(&rb, NULL, &len);
    // When data is NULL, len should be set to 0
    assert_int_equal(len, 0);
    
    ringbuffer_free(&rb);
}

// Test: NULL pointer handling for get_write_ptr with NULL len pointer
static void test_ringbuffer_get_write_ptr_null_len(void **state) {
    (void)state;
    ringbuffer_t rb;
    ringbuffer_init(&rb);
    
    uint8_t *data = (uint8_t*)0x12345678;
    ringbuffer_get_write_ptr(&rb, &data, NULL);
    // When len is NULL, data should be set to NULL
    assert_null(data);
    
    ringbuffer_free(&rb);
}

// Test: NULL pointer handling for commit_write
static void test_ringbuffer_commit_write_null(void **state) {
    (void)state;
    // Should not crash
    ringbuffer_commit_write(NULL, 100);
}

// Test: NULL pointer handling for next_read
static void test_ringbuffer_next_read_null(void **state) {
    (void)state;
    uint8_t *data = (uint8_t*)0x12345678;
    size_t len = 0x12345678;
    
    ringbuffer_next_read(NULL, &data, &len);
    assert_null(data);
    assert_int_equal(len, 0);
}

// Test: NULL pointer handling for peek_read
static void test_ringbuffer_peek_read_null(void **state) {
    (void)state;
    uint8_t *data = (uint8_t*)0x12345678;
    size_t len = 0x12345678;
    
    ringbuffer_peek_read(NULL, &data, &len);
    assert_null(data);
    assert_int_equal(len, 0);
}

// Test: NULL pointer handling for advance_read
static void test_ringbuffer_advance_read_null(void **state) {
    (void)state;
    // Should not crash
    ringbuffer_advance_read(NULL, 100);
}

// Test: NULL pointer handling for write
static void test_ringbuffer_write_null(void **state) {
    (void)state;
    const char *data = "test";
    size_t written = ringbuffer_write(NULL, (const uint8_t *)data, 4);
    assert_int_equal(written, 0);
}

// Test: NULL data pointer handling for write
static void test_ringbuffer_write_null_data(void **state) {
    (void)state;
    ringbuffer_t rb;
    ringbuffer_init(&rb);
    
    size_t written = ringbuffer_write(&rb, NULL, 4);
    assert_int_equal(written, 0);
    
    ringbuffer_free(&rb);
}

// Test: NULL pointer handling for read
static void test_ringbuffer_read_null(void **state) {
    (void)state;
    uint8_t buffer[128];
    size_t read = ringbuffer_read(NULL, buffer, sizeof(buffer));
    assert_int_equal(read, 0);
}

// Test: NULL data pointer handling for read
static void test_ringbuffer_read_null_data(void **state) {
    (void)state;
    ringbuffer_t rb;
    ringbuffer_init(&rb);
    
    const char *test_data = "test";
    ringbuffer_write(&rb, (const uint8_t *)test_data, 4);
    
    size_t read = ringbuffer_read(&rb, NULL, 4);
    assert_int_equal(read, 0);
    
    ringbuffer_free(&rb);
}

// Test: Uninitialized ringbuffer operations
static void test_ringbuffer_uninitialized(void **state) {
    (void)state;
    ringbuffer_t rb = {0};  // Uninitialized
    
    size_t available_write = ringbuffer_available_write(&rb);
    assert_int_equal(available_write, 0);
    
    size_t available_read = ringbuffer_available_read(&rb);
    assert_int_equal(available_read, 0);
    
    uint8_t *data = NULL;
    size_t len = 0;
    ringbuffer_get_write_ptr(&rb, &data, &len);
    assert_null(data);
    assert_int_equal(len, 0);
    
    ringbuffer_next_read(&rb, &data, &len);
    assert_null(data);
    assert_int_equal(len, 0);
    
    ringbuffer_peek_read(&rb, &data, &len);
    assert_null(data);
    assert_int_equal(len, 0);
    
    const char *test_data = "test";
    size_t written = ringbuffer_write(&rb, (const uint8_t *)test_data, 4);
    assert_int_equal(written, 0);
    
    uint8_t buffer[128];
    size_t read = ringbuffer_read(&rb, buffer, sizeof(buffer));
    assert_int_equal(read, 0);
}

// Test: Single byte operations
static void test_ringbuffer_single_byte(void **state) {
    (void)state;
    ringbuffer_t rb;
    ringbuffer_init(&rb);
    
    // Write single byte
    const uint8_t test_byte = 0xAB;
    size_t written = ringbuffer_write(&rb, &test_byte, 1);
    assert_int_equal(written, 1);
    
    // Read single byte
    uint8_t read_byte = 0;
    size_t read = ringbuffer_read(&rb, &read_byte, 1);
    assert_int_equal(read, 1);
    assert_int_equal(read_byte, test_byte);
    
    ringbuffer_free(&rb);
}

// Test: Read beyond available data
static void test_ringbuffer_read_beyond_available(void **state) {
    (void)state;
    ringbuffer_t rb;
    ringbuffer_init(&rb);
    
    const char *test_data = "Hello";
    ringbuffer_write(&rb, (const uint8_t *)test_data, 5);
    
    // Try to read more than available
    uint8_t buffer[128];
    size_t read = ringbuffer_read(&rb, buffer, 100);
    assert_int_equal(read, 5);  // Should only read what's available
    
    // Verify data
    assert_memory_equal(buffer, test_data, 5);
    
    // Try to read again - should get 0
    read = ringbuffer_read(&rb, buffer, 10);
    assert_int_equal(read, 0);
    
    ringbuffer_free(&rb);
}

// Test: Commit write with more data than available
static void test_ringbuffer_commit_write_excess(void **state) {
    (void)state;
    ringbuffer_t rb;
    ringbuffer_init(&rb);
    
    // Fill buffer almost to capacity
    size_t max_write = ringbuffer_available_write(&rb);
    uint8_t *test_data = malloc(max_write);
    memset(test_data, 0xAA, max_write);
    
    size_t written = ringbuffer_write(&rb, test_data, max_write);
    assert_int_equal(written, max_write);
    
    // Try to commit more than available
    size_t old_write_offset = rb.write_offset;
    ringbuffer_commit_write(&rb, 1000);
    
    // Write offset should not have changed
    assert_int_equal(rb.write_offset, old_write_offset);
    
    free(test_data);
    ringbuffer_free(&rb);
}

// Test: Advance read with more data than available
static void test_ringbuffer_advance_read_excess(void **state) {
    (void)state;
    ringbuffer_t rb;
    ringbuffer_init(&rb);
    
    const char *test_data = "Hello";
    ringbuffer_write(&rb, (const uint8_t *)test_data, 5);
    
    // Try to advance more than available
    size_t old_read_offset = rb.read_offset;
    ringbuffer_advance_read(&rb, 100);
    
    // Read offset should only advance by available amount
    assert_int_equal(rb.read_offset, (old_read_offset + 5) % RINGBUFFER_SIZE);
    
    ringbuffer_free(&rb);
}

// Test: Zero-length commit write
static void test_ringbuffer_commit_write_zero(void **state) {
    (void)state;
    ringbuffer_t rb;
    ringbuffer_init(&rb);
    
    size_t old_write_offset = rb.write_offset;
    ringbuffer_commit_write(&rb, 0);
    
    // Write offset should not change
    assert_int_equal(rb.write_offset, old_write_offset);
    
    ringbuffer_free(&rb);
}

// Test: Zero-length advance read
static void test_ringbuffer_advance_read_zero(void **state) {
    (void)state;
    ringbuffer_t rb;
    ringbuffer_init(&rb);
    
    const char *test_data = "Hello";
    ringbuffer_write(&rb, (const uint8_t *)test_data, 5);
    
    size_t old_read_offset = rb.read_offset;
    ringbuffer_advance_read(&rb, 0);
    
    // Read offset should not change
    assert_int_equal(rb.read_offset, old_read_offset);
    
    ringbuffer_free(&rb);
}

// Test: Peek vs next read consistency
static void test_ringbuffer_peek_vs_next_read(void **state) {
    (void)state;
    ringbuffer_t rb;
    ringbuffer_init(&rb);
    
    const char *test_data = "PeekTest";
    ringbuffer_write(&rb, (const uint8_t *)test_data, 8);
    
    uint8_t *peek_data = NULL;
    size_t peek_len = 0;
    ringbuffer_peek_read(&rb, &peek_data, &peek_len);
    
    uint8_t *next_data = NULL;
    size_t next_len = 0;
    ringbuffer_next_read(&rb, &next_data, &next_len);
    
    // Should return identical results
    assert_ptr_equal(peek_data, next_data);
    assert_int_equal(peek_len, next_len);
    assert_memory_equal(peek_data, test_data, peek_len);
    
    ringbuffer_free(&rb);
}

// Test: Double free protection
static void test_ringbuffer_double_free(void **state) {
    (void)state;
    ringbuffer_t rb;
    ringbuffer_init(&rb);
    
    // First free should work
    ringbuffer_free(&rb);
    
    // Second free should not crash
    ringbuffer_free(&rb);
    
    // Third free should not crash
    ringbuffer_free(&rb);
}

// Test: Buffer reuse (init/free multiple times)
static void test_ringbuffer_reuse(void **state) {
    (void)state;
    ringbuffer_t rb;
    
    // Multiple init/free cycles
    for (int i = 0; i < 5; i++) {
        int result = ringbuffer_init(&rb);
        assert_int_equal(result, 0);
        
        // Do some operations
        const char *test_data = "Test";
        size_t written = ringbuffer_write(&rb, (const uint8_t *)test_data, 4);
        assert_int_equal(written, 4);
        
        uint8_t buffer[128];
        size_t read = ringbuffer_read(&rb, buffer, sizeof(buffer));
        assert_int_equal(read, 4);
        
        ringbuffer_free(&rb);
    }
}

// Test: High-frequency operations
static void test_ringbuffer_high_frequency(void **state) {
    (void)state;
    ringbuffer_t rb;
    ringbuffer_init(&rb);
    
    const char *test_data = "H";
    
    // Rapid write/read cycles
    for (int i = 0; i < 1000; i++) {
        size_t written = ringbuffer_write(&rb, (const uint8_t *)test_data, 1);
        assert_int_equal(written, 1);
        
        uint8_t buffer[128];
        size_t read = ringbuffer_read(&rb, buffer, sizeof(buffer));
        assert_int_equal(read, 1);
        assert_int_equal(buffer[0], 'H');
    }
    
    ringbuffer_free(&rb);
}

// Test: Alternating small and large operations
static void test_ringbuffer_alternating_operations(void **state) {
    (void)state;
    ringbuffer_t rb;
    ringbuffer_init(&rb);
    
    // Small write
    const char *small_data = "Hi";
    size_t written = ringbuffer_write(&rb, (const uint8_t *)small_data, 2);
    assert_int_equal(written, 2);
    
    // Large write
    size_t large_size = 10000;
    uint8_t *large_data = malloc(large_size);
    for (size_t i = 0; i < large_size; i++) {
        large_data[i] = (uint8_t)(i % 256);
    }
    
    written = ringbuffer_write(&rb, large_data, large_size);
    assert_int_equal(written, large_size);
    
    // Read small data
    uint8_t buffer[128];
    size_t read = ringbuffer_read(&rb, buffer, 2);
    assert_int_equal(read, 2);
    assert_memory_equal(buffer, small_data, 2);
    
    // Read large data
    uint8_t *read_large = malloc(large_size);
    read = ringbuffer_read(&rb, read_large, large_size);
    assert_int_equal(read, large_size);
    assert_memory_equal(read_large, large_data, large_size);
    
    free(large_data);
    free(read_large);
    ringbuffer_free(&rb);
}

// Test: Mixed zero-copy and regular operations
static void test_ringbuffer_mixed_operations(void **state) {
    (void)state;
    ringbuffer_t rb;
    ringbuffer_init(&rb);
    
    // Regular write
    const char *regular_data = "Regular";
    size_t written = ringbuffer_write(&rb, (const uint8_t *)regular_data, 7);
    assert_int_equal(written, 7);
    
    // Zero-copy write
    uint8_t *write_ptr = NULL;
    size_t write_len = 0;
    ringbuffer_get_write_ptr(&rb, &write_ptr, &write_len);
    assert_non_null(write_ptr);
    assert_true(write_len > 0);
    
    const char *zero_copy_data = "ZeroCopy";
    size_t copy_len = strlen(zero_copy_data);
    if (write_len >= copy_len) {
        memcpy(write_ptr, zero_copy_data, copy_len);
        ringbuffer_commit_write(&rb, copy_len);
    }
    
    // Regular read
    uint8_t buffer[256];
    size_t read = ringbuffer_read(&rb, buffer, sizeof(buffer));
    assert_true(read >= 7);
    
    // Zero-copy read
    uint8_t *read_ptr = NULL;
    size_t read_len = 0;
    ringbuffer_next_read(&rb, &read_ptr, &read_len);
    if (read_len > 0) {
        ringbuffer_advance_read(&rb, read_len);
    }
    
    ringbuffer_free(&rb);
}

// Test: Boundary stress test
static void test_ringbuffer_boundary_stress(void **state) {
    (void)state;
    ringbuffer_t rb;
    ringbuffer_init(&rb);
    
    // Fill buffer to near capacity
    size_t available = ringbuffer_available_write(&rb);
    size_t fill_size = available - 100;  // Leave some space
    
    uint8_t *fill_data = malloc(fill_size);
    memset(fill_data, 0xAA, fill_size);
    
    size_t written = ringbuffer_write(&rb, fill_data, fill_size);
    assert_int_equal(written, fill_size);
    
    // Read most of it back, leaving data near the end
    size_t read_size = fill_size - 50;
    uint8_t *read_data = malloc(read_size);
    size_t read = ringbuffer_read(&rb, read_data, read_size);
    assert_int_equal(read, read_size);
    
    // Now write data that will wrap around
    const char *wrap_data = "WrapAround";
    written = ringbuffer_write(&rb, (const uint8_t *)wrap_data, 10);
    assert_int_equal(written, 10);
    
    // Read remaining old data
    size_t remaining = ringbuffer_available_read(&rb) - 10;
    if (remaining > 0) {
        uint8_t *remaining_data = malloc(remaining);
        read = ringbuffer_read(&rb, remaining_data, remaining);
        assert_int_equal(read, remaining);
        free(remaining_data);
    }
    
    // Read wrapped data
    uint8_t wrap_buffer[128];
    read = ringbuffer_read(&rb, wrap_buffer, sizeof(wrap_buffer));
    assert_int_equal(read, 10);
    assert_memory_equal(wrap_buffer, wrap_data, 10);
    
    free(fill_data);
    free(read_data);
    ringbuffer_free(&rb);
}

// Test suite
int main(void) {
    const struct CMUnitTest tests[] = {
        // Original tests
        cmocka_unit_test(test_ringbuffer_init),
        cmocka_unit_test(test_ringbuffer_available_write),
        cmocka_unit_test(test_ringbuffer_available_read_empty),
        cmocka_unit_test(test_ringbuffer_write_read),
        cmocka_unit_test(test_ringbuffer_get_write_ptr),
        cmocka_unit_test(test_ringbuffer_next_read),
        cmocka_unit_test(test_ringbuffer_peek_read),
        cmocka_unit_test(test_ringbuffer_wraparound),
        cmocka_unit_test(test_ringbuffer_large_data),
        cmocka_unit_test(test_ringbuffer_full_buffer),
        cmocka_unit_test(test_ringbuffer_write_beyond_capacity),
        cmocka_unit_test(test_ringbuffer_sequential),
        cmocka_unit_test(test_ringbuffer_zero_length),
        
        // Error handling tests
        cmocka_unit_test(test_ringbuffer_init_null),
        cmocka_unit_test(test_ringbuffer_available_write_null),
        cmocka_unit_test(test_ringbuffer_available_read_null),
        cmocka_unit_test(test_ringbuffer_get_write_ptr_null),
        cmocka_unit_test(test_ringbuffer_get_write_ptr_null_data),
        cmocka_unit_test(test_ringbuffer_get_write_ptr_null_len),
        cmocka_unit_test(test_ringbuffer_commit_write_null),
        cmocka_unit_test(test_ringbuffer_next_read_null),
        cmocka_unit_test(test_ringbuffer_peek_read_null),
        cmocka_unit_test(test_ringbuffer_advance_read_null),
        cmocka_unit_test(test_ringbuffer_write_null),
        cmocka_unit_test(test_ringbuffer_write_null_data),
        cmocka_unit_test(test_ringbuffer_read_null),
        cmocka_unit_test(test_ringbuffer_read_null_data),
        cmocka_unit_test(test_ringbuffer_uninitialized),
        
        // Edge case tests
        cmocka_unit_test(test_ringbuffer_single_byte),
        cmocka_unit_test(test_ringbuffer_read_beyond_available),
        cmocka_unit_test(test_ringbuffer_commit_write_excess),
        cmocka_unit_test(test_ringbuffer_advance_read_excess),
        cmocka_unit_test(test_ringbuffer_commit_write_zero),
        cmocka_unit_test(test_ringbuffer_advance_read_zero),
        cmocka_unit_test(test_ringbuffer_peek_vs_next_read),
        
        // Memory management tests
        cmocka_unit_test(test_ringbuffer_double_free),
        cmocka_unit_test(test_ringbuffer_reuse),
        
        // Stress tests
        cmocka_unit_test(test_ringbuffer_high_frequency),
        cmocka_unit_test(test_ringbuffer_alternating_operations),
        cmocka_unit_test(test_ringbuffer_mixed_operations),
        cmocka_unit_test(test_ringbuffer_boundary_stress),
    };
    
    return cmocka_run_group_tests(tests, NULL, NULL);
}

