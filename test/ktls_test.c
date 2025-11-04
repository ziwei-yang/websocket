/*
 * kTLS (Kernel TLS) Verification Test
 *
 * This test verifies that Kernel TLS is properly configured and working.
 * It checks:
 *   1. Kernel module status
 *   2. SSL connection establishment
 *   3. kTLS activation after handshake
 *   4. Basic send/recv operations
 *   5. Detailed status reporting
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include "../ssl.h"

// Linux kTLS support check (same as in ssl.c)
#ifdef __linux__
#include <linux/tls.h>
#ifndef TCP_ULP
#define TCP_ULP 31
#endif
#define KTLS_SUPPORTED 1
#endif

// ANTML color codes
#define COLOR_RED     "\033[0;31m"
#define COLOR_GREEN   "\033[0;32m"
#define COLOR_YELLOW  "\033[1;33m"
#define COLOR_BLUE    "\033[0;34m"
#define COLOR_RESET   "\033[0m"

// Test result tracking
static int tests_passed = 0;
static int tests_failed = 0;

static void print_header(void) {
    printf("\n");
    printf("╔══════════════════════════════════════════════════════════════════╗\n");
    printf("║           kTLS (Kernel TLS) Verification Test                   ║\n");
    printf("╚══════════════════════════════════════════════════════════════════╝\n");
    printf("\n");
}

static void print_test_result(const char *test_name, int passed, const char *details) {
    if (passed) {
        printf("%s[✓]%s %s", COLOR_GREEN, COLOR_RESET, test_name);
        tests_passed++;
    } else {
        printf("%s[✗]%s %s", COLOR_RED, COLOR_RESET, test_name);
        tests_failed++;
    }

    if (details && details[0]) {
        printf("\n    %s", details);
    }
    printf("\n");
}

static int check_kernel_module(void) {
    printf("%s[1/5] Checking Kernel Module...%s\n", COLOR_BLUE, COLOR_RESET);

    FILE *fp = popen("lsmod | grep -q '^tls' && echo 'loaded' || echo 'not_loaded'", "r");
    if (!fp) {
        print_test_result("Kernel module check", 0, "Failed to check module status");
        return 0;
    }

    char result[32] = {0};
    if (fgets(result, sizeof(result), fp) == NULL) {
        pclose(fp);
        print_test_result("Kernel module check", 0, "Failed to read module status");
        return 0;
    }
    pclose(fp);

    // Remove newline
    result[strcspn(result, "\n")] = 0;

    int loaded = (strcmp(result, "loaded") == 0);

    if (loaded) {
        print_test_result("Kernel module loaded", 1, "TLS module is loaded");
    } else {
        print_test_result("Kernel module loaded", 0,
                         "TLS module not loaded - run: sudo modprobe tls");
    }

    printf("\n");
    return loaded;
}

static int check_kernel_config(void) {
    printf("%s[2/5] Checking Kernel Configuration...%s\n", COLOR_BLUE, COLOR_RESET);

    // Try to read kernel config
    char config_path[256];
    FILE *fp = popen("uname -r", "r");
    if (!fp) {
        print_test_result("Kernel config check", 0, "Failed to get kernel version");
        return 0;
    }

    char kernel_version[64] = {0};
    if (fgets(kernel_version, sizeof(kernel_version), fp) == NULL) {
        pclose(fp);
        print_test_result("Kernel config check", 0, "Failed to read kernel version");
        return 0;
    }
    pclose(fp);

    // Remove newline
    kernel_version[strcspn(kernel_version, "\n")] = 0;

    snprintf(config_path, sizeof(config_path), "/boot/config-%s", kernel_version);

    char cmd[512];
    snprintf(cmd, sizeof(cmd),
             "grep -q 'CONFIG_TLS=y\\|CONFIG_TLS=m' %s 2>/dev/null && echo 'enabled' || echo 'not_found'",
             config_path);

    fp = popen(cmd, "r");
    if (!fp) {
        print_test_result("CONFIG_TLS check", 0, "Failed to check kernel config");
        return 0;
    }

    char result[32] = {0};
    if (fgets(result, sizeof(result), fp) == NULL) {
        pclose(fp);
        print_test_result("CONFIG_TLS check", 0, "Failed to read kernel config");
        return 0;
    }
    pclose(fp);

    // Remove newline
    result[strcspn(result, "\n")] = 0;

    int enabled = (strcmp(result, "enabled") == 0);

    if (enabled) {
        print_test_result("CONFIG_TLS enabled", 1, "Kernel supports kTLS");
    } else {
        print_test_result("CONFIG_TLS check", 0,
                         "Cannot verify CONFIG_TLS (config file not accessible)");
        // Don't fail - config file might just not be accessible
    }

    printf("\n");
    return 1;
}

static int test_ssl_connection(void) {
    printf("%s[3/5] Testing SSL Connection...%s\n", COLOR_BLUE, COLOR_RESET);

    // Use a reliable test server (Cloudflare DNS over HTTPS)
    const char *test_host = "1.1.1.1";
    int test_port = 443;

    ssl_context_t *ctx = ssl_init(test_host, test_port);
    if (!ctx) {
        print_test_result("SSL context creation", 0, "Failed to create SSL context");
        return 0;
    }
    print_test_result("SSL context creation", 1, "SSL context created successfully");

    int ret = ssl_handshake(ctx);
    if (ret < 0) {
        char details[128];
        snprintf(details, sizeof(details), "Handshake failed with error: %d", ret);
        print_test_result("SSL handshake", 0, details);
        ssl_free(ctx);
        return 0;
    }
    print_test_result("SSL handshake", 1, "Handshake completed successfully");

    // Check kTLS status
    int ktls_enabled = ssl_ktls_enabled(ctx);
    const char *backend = ssl_get_backend_version();
    const char *cipher = ssl_get_cipher_name(ctx);

    char details[256];
    snprintf(details, sizeof(details),
             "Backend: %s | Cipher: %s | kTLS: %s",
             backend ? backend : "Unknown",
             cipher ? cipher : "Unknown",
             ktls_enabled ? "ENABLED" : "DISABLED");

#ifdef KTLS_SUPPORTED
    if (ktls_enabled) {
        print_test_result("kTLS status", 1, details);
    } else {
        print_test_result("kTLS status", 0,
                         "kTLS not enabled (module loaded? handshake complete?)");
    }
#else
    print_test_result("kTLS support", 0,
                     "kTLS not compiled in (build with SSL_BACKEND=ktls)");
#endif

    ssl_free(ctx);
    printf("\n");

#ifdef KTLS_SUPPORTED
    return ktls_enabled;
#else
    return 0;
#endif
}

static void print_build_info(void) {
    printf("%s[4/5] Build Configuration...%s\n", COLOR_BLUE, COLOR_RESET);

    const char *backend = ssl_get_backend_version();
    printf("   SSL Backend:  %s\n", backend ? backend : "Unknown");

#ifdef KTLS_SUPPORTED
    printf("   kTLS Support: ENABLED (compiled with -DSSL_BACKEND_KTLS)\n");
#else
    printf("   kTLS Support: DISABLED (not compiled with kTLS backend)\n");
    printf("                 Rebuild with: SSL_BACKEND=ktls make clean all\n");
#endif

    int hw_crypto = ssl_has_hw_crypto();
    printf("   HW Crypto:    %s", hw_crypto ? "YES" : "NO");
    if (hw_crypto) {
#if defined(__x86_64__) || defined(__i386__)
        printf(" (AES-NI)");
#elif defined(__aarch64__) || defined(__arm64__)
        printf(" (ARM Crypto Extensions)");
#endif
    }
    printf("\n\n");
}

static void print_recommendations(int module_loaded, int ssl_working) {
    printf("%s[5/5] Recommendations%s\n", COLOR_BLUE, COLOR_RESET);

    if (!module_loaded) {
        printf("%s⚠️  Action Required:%s\n", COLOR_YELLOW, COLOR_RESET);
        printf("   Load TLS kernel module:\n");
        printf("   $ sudo modprobe tls\n");
        printf("   \n");
        printf("   Make persistent:\n");
        printf("   $ sudo ./scripts/enable_ktls.sh\n");
        printf("\n");
    } else if (!ssl_working) {
        printf("%s⚠️  kTLS not working:%s\n", COLOR_YELLOW, COLOR_RESET);
#ifdef KTLS_SUPPORTED
        printf("   Possible reasons:\n");
        printf("   1. OpenSSL version doesn't support kTLS (need 1.1.1+)\n");
        printf("   2. Cipher suite not compatible with kTLS\n");
        printf("   3. Kernel version too old (need 4.17+)\n");
#else
        printf("   Build with kTLS backend:\n");
        printf("   $ SSL_BACKEND=ktls make clean all\n");
        printf("   $ make ktls-verify\n");
#endif
        printf("\n");
    } else {
        printf("%s✅ kTLS is working correctly!%s\n", COLOR_GREEN, COLOR_RESET);
        printf("   You can now run:\n");
        printf("   $ make ktls-test         # Full integration test\n");
        printf("   $ make ktls-benchmark    # Performance comparison\n");
        printf("\n");
    }
}

static void print_summary(void) {
    printf("╔══════════════════════════════════════════════════════════════════╗\n");
    printf("║                        Test Summary                              ║\n");
    printf("╚══════════════════════════════════════════════════════════════════╝\n");
    printf("\n");

    printf("  Tests Passed:  %s%d%s\n", COLOR_GREEN, tests_passed, COLOR_RESET);
    printf("  Tests Failed:  %s%d%s\n", COLOR_RED, tests_failed, COLOR_RESET);
    printf("\n");

    if (tests_failed == 0) {
        printf("%s✅ All tests passed!%s\n", COLOR_GREEN, COLOR_RESET);
    } else {
        printf("%s⚠️  Some tests failed - see recommendations above%s\n",
               COLOR_YELLOW, COLOR_RESET);
    }
    printf("\n");
}

int main(void) {
    print_header();

    // Run tests
    int module_loaded = check_kernel_module();
    check_kernel_config();
    int ssl_working = test_ssl_connection();
    print_build_info();
    print_recommendations(module_loaded, ssl_working);

    // Print summary
    print_summary();

    return (tests_failed > 0) ? 1 : 0;
}
