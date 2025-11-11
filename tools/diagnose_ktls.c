// kTLS Diagnostic Utility
// Checks system configuration for Kernel TLS (kTLS) compatibility
//
// Usage: ./diagnose_ktls

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/utsname.h>
#include <unistd.h>

#define COLOR_RESET   "\033[0m"
#define COLOR_RED     "\033[31m"
#define COLOR_GREEN   "\033[32m"
#define COLOR_YELLOW  "\033[33m"
#define COLOR_BLUE    "\033[34m"
#define COLOR_BOLD    "\033[1m"

#define CHECK_PASS "✓"
#define CHECK_FAIL "✗"
#define CHECK_WARN "⚠"

static int issues_found = 0;
static int warnings_found = 0;

static void print_header(const char *title) {
    printf("\n" COLOR_BOLD COLOR_BLUE "═══════════════════════════════════════════════════════════════\n");
    printf("  %s\n", title);
    printf("═══════════════════════════════════════════════════════════════" COLOR_RESET "\n");
}

static void print_pass(const char *msg) {
    printf(COLOR_GREEN "  " CHECK_PASS " %s" COLOR_RESET "\n", msg);
}

static void print_fail(const char *msg, const char *solution) {
    printf(COLOR_RED "  " CHECK_FAIL " %s" COLOR_RESET "\n", msg);
    if (solution) {
        printf(COLOR_YELLOW "    → %s" COLOR_RESET "\n", solution);
    }
    issues_found++;
}

static void print_warn(const char *msg, const char *recommendation) {
    printf(COLOR_YELLOW "  " CHECK_WARN " %s" COLOR_RESET "\n", msg);
    if (recommendation) {
        printf("    → %s\n", recommendation);
    }
    warnings_found++;
}

static void print_info(const char *label, const char *value) {
    printf("  " COLOR_BOLD "%s:" COLOR_RESET " %s\n", label, value);
}

// Parse kernel version string (e.g., "5.15.0-58-generic" -> {5, 15, 0})
static int parse_kernel_version(const char *version, int *major, int *minor, int *patch) {
    if (sscanf(version, "%d.%d.%d", major, minor, patch) >= 2) {
        if (*major < 0 || *minor < 0) return -1;
        return 0;
    }
    return -1;
}

// Compare kernel versions: returns 1 if ver1 >= ver2, 0 otherwise
static int kernel_version_gte(int maj1, int min1, int patch1, int maj2, int min2, int patch2) {
    if (maj1 > maj2) return 1;
    if (maj1 < maj2) return 0;
    if (min1 > min2) return 1;
    if (min1 < min2) return 0;
    return (patch1 >= patch2);
}

static void check_kernel_version() {
    print_header("KERNEL VERSION CHECK");

    struct utsname uts;
    if (uname(&uts) < 0) {
        print_fail("Failed to get kernel version", "Check uname command");
        return;
    }

    print_info("System", uts.sysname);
    print_info("Release", uts.release);
    print_info("Machine", uts.machine);

    int major, minor, patch = 0;
    if (parse_kernel_version(uts.release, &major, &minor, &patch) < 0) {
        print_fail("Unable to parse kernel version", "Unknown kernel version format");
        return;
    }

    char version_str[64];
    snprintf(version_str, sizeof(version_str), "%d.%d.%d", major, minor, patch);
    print_info("Parsed Version", version_str);

    // Check minimum version (4.13)
    if (!kernel_version_gte(major, minor, patch, 4, 13, 0)) {
        print_fail("Kernel version too old (< 4.13)", "Upgrade to Linux 4.13 or later");
        return;
    }

    print_pass("Kernel version >= 4.13 (minimum for kTLS)");

    // Check recommended version (5.2)
    if (!kernel_version_gte(major, minor, patch, 5, 2, 0)) {
        print_warn("Kernel version < 5.2", "Upgrade to 5.2+ recommended for full kTLS support");
    } else {
        print_pass("Kernel version >= 5.2 (recommended)");
    }

    // Check optimal version (5.10)
    if (kernel_version_gte(major, minor, patch, 5, 10, 0)) {
        print_pass("Kernel version >= 5.10 (optimal performance)");
    }
}

static void check_ktls_module() {
    print_header("kTLS KERNEL MODULE CHECK");

    // Check if tls module is loaded
    FILE *fp = popen("lsmod | grep -w '^tls'", "r");
    if (!fp) {
        print_fail("Failed to execute lsmod", "Check system permissions");
        return;
    }

    char buffer[256];
    int module_loaded = 0;
    if (fgets(buffer, sizeof(buffer), fp) != NULL) {
        module_loaded = 1;
    }
    pclose(fp);

    if (module_loaded) {
        print_pass("tls kernel module is loaded");
        print_info("Module info", buffer);
    } else {
        print_fail("tls kernel module is NOT loaded", "Run: sudo modprobe tls");

        // Check if module is available
        fp = popen("modinfo tls 2>/dev/null", "r");
        if (fp) {
            if (fgets(buffer, sizeof(buffer), fp) != NULL) {
                print_info("Module available", "Yes (but not loaded)");
            } else {
                print_fail("tls module not available in kernel", "Rebuild kernel with CONFIG_TLS=m");
            }
            pclose(fp);
        }
        return;
    }

    // Check /proc/net/tls_stat
    fp = fopen("/proc/net/tls_stat", "r");
    if (fp) {
        print_pass("/proc/net/tls_stat is available");
        printf("\n  " COLOR_BOLD "kTLS Statistics:" COLOR_RESET "\n");
        while (fgets(buffer, sizeof(buffer), fp)) {
            buffer[strcspn(buffer, "\n")] = 0;  // Remove newline
            printf("    %s\n", buffer);
        }
        fclose(fp);
    } else {
        print_warn("/proc/net/tls_stat not found", "kTLS stats unavailable");
    }
}

static void check_openssl_version() {
    print_header("OPENSSL VERSION CHECK");

    // Get OpenSSL version
    FILE *fp = popen("openssl version 2>/dev/null", "r");
    if (!fp) {
        print_fail("OpenSSL not found", "Install OpenSSL 3.0+");
        return;
    }

    char buffer[256];
    if (!fgets(buffer, sizeof(buffer), fp)) {
        print_fail("Failed to get OpenSSL version", NULL);
        pclose(fp);
        return;
    }
    pclose(fp);

    buffer[strcspn(buffer, "\n")] = 0;
    print_info("Version", buffer);

    // Parse version
    int major = 0, minor = 0, patch = 0;
    if (sscanf(buffer, "OpenSSL %d.%d.%d", &major, &minor, &patch) >= 2) {
        if (major >= 3) {
            print_pass("OpenSSL version >= 3.0 (kTLS supported)");
        } else if (major == 1 && minor == 1) {
            print_fail("OpenSSL 1.1.x does NOT support kTLS", "Upgrade to OpenSSL 3.0+");
            return;
        } else {
            print_warn("Unknown OpenSSL version", "OpenSSL 3.0+ recommended");
        }
    }

    // Check if kTLS is enabled in OpenSSL build
    fp = popen("openssl version -a 2>/dev/null | grep -i ktls", "r");
    if (fp) {
        int has_output = 0;
        while (fgets(buffer, sizeof(buffer), fp)) {
            has_output = 1;
            if (strstr(buffer, "OPENSSL_NO_KTLS")) {
                print_fail("OpenSSL built with OPENSSL_NO_KTLS", "Rebuild OpenSSL without --disable-ktls");
                break;
            }
        }
        if (!has_output) {
            print_pass("OpenSSL appears to be built with kTLS support");
        }
        pclose(fp);
    }

    // Get detailed OpenSSL build info
    fp = popen("openssl version -a 2>/dev/null", "r");
    if (fp) {
        printf("\n  " COLOR_BOLD "OpenSSL Build Details:" COLOR_RESET "\n");
        while (fgets(buffer, sizeof(buffer), fp)) {
            buffer[strcspn(buffer, "\n")] = 0;
            printf("    %s\n", buffer);
        }
        pclose(fp);
    }
}

static void check_cipher_support() {
    print_header("CIPHER SUITE SUPPORT CHECK");

    const char *ciphers[] = {
        "TLS_AES_128_GCM_SHA256",
        "TLS_AES_256_GCM_SHA384",
        "TLS_CHACHA20_POLY1305_SHA256"
    };

    printf("\n  " COLOR_BOLD "Testing kTLS-compatible ciphers:" COLOR_RESET "\n\n");

    for (int i = 0; i < 3; i++) {
        char cmd[512];
        // Check if cipher appears in the full OpenSSL cipher list
        snprintf(cmd, sizeof(cmd),
                 "openssl ciphers -v 2>/dev/null | grep -q '%s'",
                 ciphers[i]);

        int ret = system(cmd);
        if (ret == 0) {
            print_pass(ciphers[i]);
        } else {
            print_warn(ciphers[i], "Not available");
        }
    }
}

static void check_network_config() {
    print_header("NETWORK CONFIGURATION CHECK");

    // Check TCP buffer sizes
    FILE *fp = fopen("/proc/sys/net/core/rmem_max", "r");
    if (fp) {
        char buffer[64];
        if (fgets(buffer, sizeof(buffer), fp)) {
            long rmem_max = atol(buffer);
            print_info("net.core.rmem_max", buffer);
            if (rmem_max < 8388608) {  // 8MB
                print_warn("TCP receive buffer too small", "Increase to 67108864 for high throughput");
            } else {
                print_pass("TCP receive buffer size adequate");
            }
        }
        fclose(fp);
    }

    fp = fopen("/proc/sys/net/core/wmem_max", "r");
    if (fp) {
        char buffer[64];
        if (fgets(buffer, sizeof(buffer), fp)) {
            long wmem_max = atol(buffer);
            print_info("net.core.wmem_max", buffer);
            if (wmem_max < 8388608) {  // 8MB
                print_warn("TCP send buffer too small", "Increase to 67108864 for high throughput");
            } else {
                print_pass("TCP send buffer size adequate");
            }
        }
        fclose(fp);
    }
}

static void check_nic_offload() {
    print_header("NIC TLS OFFLOAD CHECK");

    // Find network interfaces
    FILE *fp = popen("ip -o link show | awk -F': ' '{print $2}' | grep -v lo", "r");
    if (!fp) {
        print_warn("Failed to enumerate network interfaces", NULL);
        return;
    }

    char iface[64];
    int found_nic = 0;

    while (fgets(iface, sizeof(iface), fp)) {
        iface[strcspn(iface, "\n")] = 0;

        char cmd[256];
        snprintf(cmd, sizeof(cmd), "ethtool -k %s 2>/dev/null | grep -i 'tls.*offload'", iface);

        FILE *ethtool_fp = popen(cmd, "r");
        if (ethtool_fp) {
            char buffer[256];
            int has_offload = 0;
            printf("\n  " COLOR_BOLD "Interface: %s" COLOR_RESET "\n", iface);

            while (fgets(buffer, sizeof(buffer), ethtool_fp)) {
                buffer[strcspn(buffer, "\n")] = 0;
                printf("    %s\n", buffer);
                if (strstr(buffer, ": on")) {
                    has_offload = 1;
                }
            }

            if (has_offload) {
                print_pass("Hardware TLS offload enabled");
            } else {
                print_info("Hardware TLS offload", "Not supported or disabled");
            }

            pclose(ethtool_fp);
            found_nic = 1;
        }
    }
    pclose(fp);

    if (!found_nic) {
        print_warn("No network interfaces found", NULL);
    }
}

static void print_summary() {
    print_header("SUMMARY");

    if (issues_found == 0 && warnings_found == 0) {
        printf(COLOR_GREEN COLOR_BOLD "\n  ✓ All checks passed! Your system is fully configured for kTLS.\n" COLOR_RESET);
    } else {
        printf("\n  " COLOR_BOLD "Issues found:" COLOR_RESET " %s%d%s\n",
               issues_found > 0 ? COLOR_RED : "", issues_found, COLOR_RESET);
        printf("  " COLOR_BOLD "Warnings:" COLOR_RESET " %s%d%s\n\n",
               warnings_found > 0 ? COLOR_YELLOW : "", warnings_found, COLOR_RESET);

        if (issues_found > 0) {
            printf(COLOR_RED "  → Fix critical issues above before using kTLS\n" COLOR_RESET);
        }
        if (warnings_found > 0) {
            printf(COLOR_YELLOW "  → Review warnings for optimal performance\n" COLOR_RESET);
        }
    }

    printf("\n  " COLOR_BLUE "For more information, see:" COLOR_RESET "\n");
    printf("    • docs/KTLS_GUIDE.md (this library's kTLS documentation)\n");
    printf("    • https://www.kernel.org/doc/html/latest/networking/tls.html\n");
    printf("    • https://www.openssl.org/docs/man3.0/man3/SSL_CTX_set_options.html\n");
    printf("\n");
}

int main(int argc, char *argv[]) {
    printf("\n" COLOR_BOLD "╔═══════════════════════════════════════════════════════════════╗\n");
    printf("║              kTLS (Kernel TLS) Diagnostic Tool               ║\n");
    printf("║                      websocket_zwy library                    ║\n");
    printf("╚═══════════════════════════════════════════════════════════════╝" COLOR_RESET "\n");

    check_kernel_version();
    check_ktls_module();
    check_openssl_version();
    check_cipher_support();
    check_network_config();
    check_nic_offload();
    print_summary();

    return (issues_found > 0) ? 1 : 0;
}
