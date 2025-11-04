/*
 * SSL/TLS Probe Utility
 *
 * Tests TLS version support and cipher suites for any given host:port
 * Usage: ssl_probe [host:port]
 * Default: stream.binance.com:443
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <netdb.h>
#include <openssl/ssl.h>
#include <openssl/err.h>

#define COLOR_GREEN  "\033[0;32m"
#define COLOR_RED    "\033[0;31m"
#define COLOR_YELLOW "\033[1;33m"
#define COLOR_BLUE   "\033[0;34m"
#define COLOR_CYAN   "\033[0;36m"
#define COLOR_RESET  "\033[0m"

typedef struct {
    const char *name;
    int min_version;
    int max_version;
} tls_version_t;

static const tls_version_t tls_versions[] = {
#ifdef TLS1_VERSION
    {"TLS 1.0", TLS1_VERSION, TLS1_VERSION},
#endif
#ifdef TLS1_1_VERSION
    {"TLS 1.1", TLS1_1_VERSION, TLS1_1_VERSION},
#endif
#ifdef TLS1_2_VERSION
    {"TLS 1.2", TLS1_2_VERSION, TLS1_2_VERSION},
#endif
#ifdef TLS1_3_VERSION
    {"TLS 1.3", TLS1_3_VERSION, TLS1_3_VERSION},
#endif
};

static const char *ktls_compatible_ciphers[] = {
    // TLS 1.3
    "TLS_AES_128_GCM_SHA256",
    "TLS_AES_256_GCM_SHA384",
    "TLS_CHACHA20_POLY1305_SHA256",
    // TLS 1.2
    "ECDHE-RSA-AES128-GCM-SHA256",
    "ECDHE-RSA-AES256-GCM-SHA384",
    "ECDHE-ECDSA-AES128-GCM-SHA256",
    "ECDHE-ECDSA-AES256-GCM-SHA384",
    "AES128-GCM-SHA256",
    "AES256-GCM-SHA384",
    NULL
};

int is_ktls_compatible(const char *cipher) {
    if (!cipher) return 0;

    for (int i = 0; ktls_compatible_ciphers[i] != NULL; i++) {
        if (strcmp(cipher, ktls_compatible_ciphers[i]) == 0) {
            return 1;
        }
    }
    return 0;
}

int connect_to_host(const char *hostname, int port) {
    struct hostent *server = gethostbyname(hostname);
    if (!server) {
        fprintf(stderr, "%sError: Cannot resolve hostname '%s'%s\n",
                COLOR_RED, hostname, COLOR_RESET);
        return -1;
    }

    int sockfd = socket(AF_INET, SOCK_STREAM, 0);
    if (sockfd < 0) {
        perror("socket");
        return -1;
    }

    struct sockaddr_in serv_addr = {0};
    serv_addr.sin_family = AF_INET;
    serv_addr.sin_port = htons(port);
    memcpy(&serv_addr.sin_addr.s_addr, server->h_addr, server->h_length);

    if (connect(sockfd, (struct sockaddr *)&serv_addr, sizeof(serv_addr)) < 0) {
        perror("connect");
        close(sockfd);
        return -1;
    }

    return sockfd;
}

void test_tls_version(const char *hostname, int port, const tls_version_t *ver) {
    int sockfd = connect_to_host(hostname, port);
    if (sockfd < 0) {
        printf("  %s%-8s%s: %s[CONNECTION FAILED]%s\n",
               COLOR_CYAN, ver->name, COLOR_RESET, COLOR_RED, COLOR_RESET);
        return;
    }

    SSL_CTX *ctx = SSL_CTX_new(TLS_client_method());
    if (!ctx) {
        close(sockfd);
        printf("  %s%-8s%s: %s[SSL_CTX_new FAILED]%s\n",
               COLOR_CYAN, ver->name, COLOR_RESET, COLOR_RED, COLOR_RESET);
        return;
    }

    SSL_CTX_set_verify(ctx, SSL_VERIFY_NONE, NULL);
    SSL_CTX_set_min_proto_version(ctx, ver->min_version);
    SSL_CTX_set_max_proto_version(ctx, ver->max_version);

    SSL *ssl = SSL_new(ctx);
    if (!ssl) {
        SSL_CTX_free(ctx);
        close(sockfd);
        printf("  %s%-8s%s: %s[SSL_new FAILED]%s\n",
               COLOR_CYAN, ver->name, COLOR_RESET, COLOR_RED, COLOR_RESET);
        return;
    }

    SSL_set_fd(ssl, sockfd);
    SSL_set_tlsext_host_name(ssl, hostname);

#ifdef SSL_OP_ENABLE_KTLS
    SSL_set_options(ssl, SSL_OP_ENABLE_KTLS);
#endif

    int ret = SSL_connect(ssl);
    if (ret == 1) {
        const char *cipher = SSL_get_cipher(ssl);
        const char *version = SSL_get_version(ssl);
        int ktls_compat = is_ktls_compatible(cipher);

        printf("  %s%-8s%s: %s✓ SUPPORTED%s\n",
               COLOR_CYAN, ver->name, COLOR_RESET, COLOR_GREEN, COLOR_RESET);
        printf("           Cipher: %s%s%s%s%s\n",
               ktls_compat ? COLOR_GREEN : COLOR_YELLOW,
               cipher,
               ktls_compat ? " [kTLS compatible]" : "",
               COLOR_RESET,
               ktls_compat ? " ✅" : "");
        printf("           Version: %s\n", version);

#if OPENSSL_VERSION_NUMBER >= 0x30000000L
        // Check if kTLS actually activated
        BIO *wbio = SSL_get_wbio(ssl);
        BIO *rbio = SSL_get_rbio(ssl);
        if (wbio && rbio) {
            int send_ktls = BIO_get_ktls_send(wbio);
            int recv_ktls = BIO_get_ktls_recv(rbio);
            if (send_ktls && recv_ktls) {
                printf("           %skTLS: ACTIVATED 🎉%s\n", COLOR_GREEN, COLOR_RESET);
            } else if (ktls_compat) {
                printf("           kTLS: Not activated (cipher compatible, but OpenSSL didn't enable)\n");
            }
        }
#endif

        SSL_shutdown(ssl);
    } else {
        int err = SSL_get_error(ssl, ret);
        printf("  %s%-8s%s: %s✗ NOT SUPPORTED%s",
               COLOR_CYAN, ver->name, COLOR_RESET, COLOR_RED, COLOR_RESET);

        if (err == SSL_ERROR_SYSCALL) {
            printf(" (connection closed by server)\n");
        } else {
            unsigned long ssl_err = ERR_get_error();
            if (ssl_err) {
                char err_buf[256];
                ERR_error_string_n(ssl_err, err_buf, sizeof(err_buf));
                printf(" (%s)\n", err_buf);
            } else {
                printf(" (SSL error: %d)\n", err);
            }
        }
        ERR_clear_error();
    }

    SSL_free(ssl);
    SSL_CTX_free(ctx);
    close(sockfd);
}

void print_ktls_info() {
    printf("\n%s📋 kTLS Compatible Ciphers:%s\n", COLOR_BLUE, COLOR_RESET);
    printf("   TLS 1.3:\n");
    printf("     • TLS_AES_128_GCM_SHA256\n");
    printf("     • TLS_AES_256_GCM_SHA384\n");
    printf("     • TLS_CHACHA20_POLY1305_SHA256\n");
    printf("   TLS 1.2:\n");
    printf("     • ECDHE-RSA-AES128-GCM-SHA256\n");
    printf("     • ECDHE-RSA-AES256-GCM-SHA384\n");
    printf("     • ECDHE-ECDSA-AES128-GCM-SHA256\n");
    printf("     • ECDHE-ECDSA-AES256-GCM-SHA384\n");
    printf("     • AES128-GCM-SHA256\n");
    printf("     • AES256-GCM-SHA384\n");
}

int main(int argc, char *argv[]) {
    const char *hostname = "stream.binance.com";
    int port = 443;

    // Parse command line arguments
    if (argc > 1) {
        char *host_port = strdup(argv[1]);
        char *colon = strchr(host_port, ':');

        if (colon) {
            *colon = '\0';
            hostname = host_port;
            port = atoi(colon + 1);
        } else {
            hostname = host_port;
        }
    }

    SSL_library_init();
    OpenSSL_add_all_algorithms();

    printf("\n");
    printf("╔═══════════════════════════════════════════════════════════════════╗\n");
    printf("║                 SSL/TLS Probe Utility                             ║\n");
    printf("╚═══════════════════════════════════════════════════════════════════╝\n");
    printf("\n");
    printf("%sTarget:%s %s:%d\n", COLOR_BLUE, COLOR_RESET, hostname, port);
    printf("%sOpenSSL Version:%s %s\n\n", COLOR_BLUE, COLOR_RESET,
           OpenSSL_version(OPENSSL_VERSION));

    printf("%s🔍 Testing TLS Version Support:%s\n\n", COLOR_BLUE, COLOR_RESET);

    size_t num_versions = sizeof(tls_versions) / sizeof(tls_versions[0]);
    for (size_t i = 0; i < num_versions; i++) {
        test_tls_version(hostname, port, &tls_versions[i]);
    }

    print_ktls_info();

    printf("\n%s💡 Note:%s kTLS activation requires:\n", COLOR_YELLOW, COLOR_RESET);
    printf("   • Linux kernel 4.17+ with TLS module loaded\n");
    printf("   • OpenSSL 1.1.1+ or 3.0+ with kTLS support\n");
    printf("   • Compatible cipher suite (AES-GCM or ChaCha20-Poly1305)\n");
    printf("   • SSL_OP_ENABLE_KTLS flag set\n");
    printf("   • OpenSSL's internal decision to enable kTLS\n");
    printf("\n");

    return 0;
}
