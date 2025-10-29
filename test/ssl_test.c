#include <stdarg.h>
#include <stddef.h>
#include <setjmp.h>
#include <cmocka.h>
#include "../ssl.h"
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <sys/socket.h>
#include <netinet/in.h>

// Test: SSL initialization with NULL parameters
static void test_ssl_init_null_hostname(void **state) {
    (void)state;
    
    ssl_context_t *ctx = ssl_init(NULL, 443);
    assert_null(ctx);  // Should fail gracefully
}

// Test: SSL initialization with invalid port
static void test_ssl_init_invalid_port(void **state) {
    (void)state;
    
    ssl_context_t *ctx = ssl_init("example.com", -1);
    // May succeed or fail depending on implementation
    if (ctx) {
        ssl_free(ctx);
    }
}

// Test: SSL free with NULL context
static void test_ssl_free_null(void **state) {
    (void)state;
    
    // Should not crash
    ssl_free(NULL);
}

// Test: SSL get fd with NULL context
static void test_ssl_get_fd_null(void **state) {
    (void)state;
    
    int fd = ssl_get_fd(NULL);
    assert_int_equal(fd, -1);
}

// Test: SSL set fd with NULL context
static void test_ssl_set_fd_null(void **state) {
    (void)state;
    
    int result = ssl_set_fd(NULL, 5);
    assert_int_equal(result, -1);
}

// Test: SSL pending with NULL context
static void test_ssl_pending_null(void **state) {
    (void)state;
    
    int result = ssl_pending(NULL);
    assert_int_equal(result, 0);
}

// Test: SSL pending with uninitialized context
static void test_ssl_pending_uninitialized(void **state) {
    (void)state;
    
    ssl_context_t *ctx = ssl_init("example.com", 443);
    if (!ctx) {
        skip();  // Skip if init failed (might be network issue)
        return;
    }
    
    // Before handshake, SSL is NULL
    int result = ssl_pending(ctx);
    assert_int_equal(result, 0);
    
    ssl_free(ctx);
}

// Test: SSL get error code with NULL context
static void test_ssl_get_error_code_null(void **state) {
    (void)state;
    
    int result = ssl_get_error_code(NULL, 0);
    assert_int_equal(result, 0);
}

// Test: SSL get error code with uninitialized context
static void test_ssl_get_error_code_uninitialized(void **state) {
    (void)state;
    
    ssl_context_t *ctx = ssl_init("example.com", 443);
    if (!ctx) {
        skip();
        return;
    }
    
    int result = ssl_get_error_code(ctx, -1);
    assert_int_equal(result, 0);
    
    ssl_free(ctx);
}

// Test: SSL read into with NULL context
static void test_ssl_read_into_null(void **state) {
    (void)state;
    
    uint8_t buf[100];
    int result = ssl_read_into(NULL, buf, sizeof(buf));
    assert_int_equal(result, -1);
}

// Test: SSL read into with NULL buffer
static void test_ssl_read_into_null_buffer(void **state) {
    (void)state;
    
    ssl_context_t *ctx = ssl_init("example.com", 443);
    if (!ctx) {
        skip();
        return;
    }
    
    // Should handle NULL buffer gracefully
    int result = ssl_read_into(ctx, NULL, 100);
    // OpenSSL may return -1 or cause issues, implementation dependent
    assert_true(result <= 0);
    
    ssl_free(ctx);
}

// Test: SSL get handle with NULL context
static void test_ssl_get_handle_null(void **state) {
    (void)state;
    
    void *handle = ssl_get_handle(NULL);
    assert_null(handle);
}

// Test: SSL get handle with uninitialized context
static void test_ssl_get_handle_uninitialized(void **state) {
    (void)state;
    
    ssl_context_t *ctx = ssl_init("example.com", 443);
    if (!ctx) {
        skip();
        return;
    }
    
    // Before handshake, SSL handle should be NULL
    void *handle = ssl_get_handle(ctx);
    assert_null(handle);
    
    ssl_free(ctx);
}

// Test: SSL send with NULL context
static void test_ssl_send_null(void **state) {
    (void)state;
    
    uint8_t data[] = "test";
    int result = ssl_send(NULL, data, sizeof(data));
    assert_int_equal(result, -1);
}

// Test: SSL send with NULL data
static void test_ssl_send_null_data(void **state) {
    (void)state;
    
    ssl_context_t *ctx = ssl_init("example.com", 443);
    if (!ctx) {
        skip();
        return;
    }
    
    // Should handle NULL data
    int result = ssl_send(ctx, NULL, 10);
    assert_true(result <= 0);
    
    ssl_free(ctx);
}

// Test: SSL recv with NULL context
static void test_ssl_recv_null(void **state) {
    (void)state;
    
    uint8_t data[100];
    int result = ssl_recv(NULL, data, sizeof(data));
    assert_int_equal(result, -1);
}

// Test: SSL recv with NULL buffer
static void test_ssl_recv_null_buffer(void **state) {
    (void)state;
    
    ssl_context_t *ctx = ssl_init("example.com", 443);
    if (!ctx) {
        skip();
        return;
    }
    
    // Should handle NULL buffer gracefully
    int result = ssl_recv(ctx, NULL, 100);
    assert_true(result <= 0);
    
    ssl_free(ctx);
}

// Test: SSL handshake with NULL context
static void test_ssl_handshake_null(void **state) {
    (void)state;
    
    int result = ssl_handshake(NULL);
    assert_int_equal(result, -1);
}

// Test: SSL handshake with non-connected socket (should fail or return 0 for progress)
static void test_ssl_handshake_no_connection(void **state) {
    (void)state;
    
    ssl_context_t *ctx = ssl_init("example.com", 443);
    if (!ctx) {
        skip();
        return;
    }
    
    // Handshake without actual connection
    int result = ssl_handshake(ctx);
    // Should return -1 (failed) or 0 (in progress)
    assert_true(result <= 0);
    
    ssl_free(ctx);
}

// Test: Multiple SSL contexts initialization (tests global context)
static void test_ssl_multiple_init(void **state) {
    (void)state;
    
    ssl_context_t *ctx1 = ssl_init("example.com", 443);
    ssl_context_t *ctx2 = ssl_init("example.com", 443);
    
    // Both should be created (or both NULL)
    // If ctx1 is NULL, skip the test
    if (!ctx1 || !ctx2) {
        if (ctx1) ssl_free(ctx1);
        if (ctx2) ssl_free(ctx2);
        skip();
        return;
    }
    
    // Both contexts should have valid socket FDs
    int fd1 = ssl_get_fd(ctx1);
    int fd2 = ssl_get_fd(ctx2);
    assert_true(fd1 >= 0);
    assert_true(fd2 >= 0);
    
    ssl_free(ctx1);
    ssl_free(ctx2);
}

// Test: SSL context reuse after free
static void test_ssl_reuse_after_free(void **state) {
    (void)state;
    
    ssl_context_t *ctx = ssl_init("example.com", 443);
    if (!ctx) {
        skip();
        return;
    }
    
    int fd_before = ssl_get_fd(ctx);
    assert_true(fd_before >= 0);
    
    ssl_free(ctx);
    
    // Create a new context
    ctx = ssl_init("example.com", 443);
    if (!ctx) {
        skip();
        return;
    }
    
    int fd_after = ssl_get_fd(ctx);
    assert_true(fd_after >= 0);
    
    ssl_free(ctx);
}

// Test: SSL operations with zero length
static void test_ssl_operations_zero_length(void **state) {
    (void)state;
    
    ssl_context_t *ctx = ssl_init("example.com", 443);
    if (!ctx) {
        skip();
        return;
    }
    
    uint8_t buf[100];
    int result = ssl_send(ctx, buf, 0);
    assert_true(result <= 0);  // Should handle gracefully
    
    result = ssl_recv(ctx, buf, 0);
    assert_true(result <= 0);  // Should handle gracefully
    
    ssl_free(ctx);
}

// Test: SSL operations with very large length
static void test_ssl_operations_large_length(void **state) {
    (void)state;
    
    ssl_context_t *ctx = ssl_init("example.com", 443);
    if (!ctx) {
        skip();
        return;
    }
    
    size_t large_size = SIZE_MAX;
    uint8_t buf[100];
    
    // These should handle large sizes gracefully
    int result = ssl_send(ctx, buf, large_size);
    assert_true(result <= 0);
    
    result = ssl_recv(ctx, buf, large_size);
    assert_true(result <= 0);
    
    ssl_free(ctx);
}

// Test: SSL set and get fd
static void test_ssl_set_get_fd(void **state) {
    (void)state;
    
    ssl_context_t *ctx = ssl_init("example.com", 443);
    if (!ctx) {
        skip();
        return;
    }
    
    int original_fd = ssl_get_fd(ctx);
    assert_true(original_fd >= 0);
    
    // Try to set a new fd
    int result = ssl_set_fd(ctx, 999);
    assert_int_equal(result, 0);
    
    int new_fd = ssl_get_fd(ctx);
    assert_int_equal(new_fd, 999);
    
    ssl_free(ctx);
}

// Test: SSL operations with uninitialized SSL (before handshake)
static void test_ssl_operations_before_handshake(void **state) {
    (void)state;
    
    ssl_context_t *ctx = ssl_init("example.com", 443);
    if (!ctx) {
        skip();
        return;
    }
    
    // Try to use SSL functions before handshake
    uint8_t buf[100];
    int result = ssl_send(ctx, buf, 10);
    assert_int_equal(result, -1);  // Should fail without handshake
    
    result = ssl_recv(ctx, buf, 10);
    assert_int_equal(result, -1);  // Should fail without handshake
    
    ssl_free(ctx);
}

// Test: SSL free twice (double free protection)
static void test_ssl_double_free(void **state) {
    (void)state;
    
    ssl_context_t *ctx = ssl_init("example.com", 443);
    if (!ctx) {
        skip();
        return;
    }
    
    ssl_free(ctx);
    
    // Second free should not crash
    ssl_free(ctx);
}

// Test: Check that pending returns 0 for uninitialized connections
static void test_ssl_pending_progression(void **state) {
    (void)state;
    
    ssl_context_t *ctx = ssl_init("example.com", 443);
    if (!ctx) {
        skip();
        return;
    }
    
    // Initially pending should be 0
    int pending1 = ssl_pending(ctx);
    assert_int_equal(pending1, 0);
    
    // After failed handshake, still 0
    ssl_handshake(ctx);
    int pending2 = ssl_pending(ctx);
    assert_int_equal(pending2, 0);
    
    ssl_free(ctx);
}

// Main test runner
int main(void) {
    const struct CMUnitTest tests[] = {
        // NULL parameter tests
        cmocka_unit_test(test_ssl_init_null_hostname),
        cmocka_unit_test(test_ssl_init_invalid_port),
        cmocka_unit_test(test_ssl_free_null),
        cmocka_unit_test(test_ssl_get_fd_null),
        cmocka_unit_test(test_ssl_set_fd_null),
        cmocka_unit_test(test_ssl_pending_null),
        cmocka_unit_test(test_ssl_pending_uninitialized),
        cmocka_unit_test(test_ssl_get_error_code_null),
        cmocka_unit_test(test_ssl_get_error_code_uninitialized),
        cmocka_unit_test(test_ssl_read_into_null),
        cmocka_unit_test(test_ssl_read_into_null_buffer),
        cmocka_unit_test(test_ssl_get_handle_null),
        cmocka_unit_test(test_ssl_get_handle_uninitialized),
        cmocka_unit_test(test_ssl_send_null),
        cmocka_unit_test(test_ssl_send_null_data),
        cmocka_unit_test(test_ssl_recv_null),
        cmocka_unit_test(test_ssl_recv_null_buffer),
        cmocka_unit_test(test_ssl_handshake_null),
        
        // Operation tests
        cmocka_unit_test(test_ssl_handshake_no_connection),
        cmocka_unit_test(test_ssl_multiple_init),
        cmocka_unit_test(test_ssl_reuse_after_free),
        cmocka_unit_test(test_ssl_operations_zero_length),
        cmocka_unit_test(test_ssl_operations_large_length),
        cmocka_unit_test(test_ssl_set_get_fd),
        cmocka_unit_test(test_ssl_operations_before_handshake),
        
        // Memory management tests
        cmocka_unit_test(test_ssl_double_free),
        cmocka_unit_test(test_ssl_pending_progression),
    };
    
    return cmocka_run_group_tests(tests, NULL, NULL);
}

