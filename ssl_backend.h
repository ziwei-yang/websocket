#ifndef SSL_BACKEND_H
#define SSL_BACKEND_H

/*
 * SSL Backend Selection
 *
 * Compile-time selection of SSL/TLS backend via -DSSL_BACKEND_XXX flag.
 * Default: OpenSSL
 *
 * Available backends:
 * - SSL_BACKEND_OPENSSL    : Standard OpenSSL (default)
 * - SSL_BACKEND_LIBRESSL   : OpenBSD's LibreSSL (OpenSSL fork)
 * - SSL_BACKEND_BORINGSSL  : Google's BoringSSL (OpenSSL fork) [requires manual build]
 * - SSL_BACKEND_KTLS       : Kernel TLS (Linux only, requires OpenSSL for handshake)
 * - SSL_BACKEND_S2N        : Amazon's s2n-tls [requires different API]
 *
 * Usage:
 *   make SSL_BACKEND=openssl    # Use OpenSSL (default)
 *   make SSL_BACKEND=libressl   # Use LibreSSL
 *   make SSL_BACKEND=boringssl  # Use BoringSSL [not available via package manager]
 *   make SSL_BACKEND=ktls       # Use Kernel TLS (Linux only)
 *   make SSL_BACKEND=s2n        # Use s2n-tls [requires code changes]
 */

// Auto-detect backend or use default
#if !defined(SSL_BACKEND_OPENSSL) && !defined(SSL_BACKEND_LIBRESSL) && \
    !defined(SSL_BACKEND_BORINGSSL) && !defined(SSL_BACKEND_KTLS) && \
    !defined(SSL_BACKEND_S2N)
#define SSL_BACKEND_OPENSSL 1
#endif

// Backend name for runtime identification
#ifdef SSL_BACKEND_OPENSSL
#define SSL_BACKEND_NAME "OpenSSL"
#endif

#ifdef SSL_BACKEND_LIBRESSL
#define SSL_BACKEND_NAME "LibreSSL"
#endif

#ifdef SSL_BACKEND_BORINGSSL
#define SSL_BACKEND_NAME "BoringSSL"
#endif

#ifdef SSL_BACKEND_KTLS
#define SSL_BACKEND_NAME "Kernel TLS"
// kTLS requires OpenSSL for handshake, so also define OpenSSL backend
#ifndef SSL_BACKEND_OPENSSL
#define SSL_BACKEND_OPENSSL 1
#define SSL_BACKEND_OPENSSL_FOR_KTLS 1
#endif
#endif

#ifdef SSL_BACKEND_S2N
#define SSL_BACKEND_NAME "s2n-tls"
#endif

// Validate only one backend is selected (kTLS can coexist with OpenSSL)
#if (defined(SSL_BACKEND_OPENSSL) + defined(SSL_BACKEND_LIBRESSL) + defined(SSL_BACKEND_BORINGSSL) + defined(SSL_BACKEND_S2N)) > 1
#error "Multiple SSL backends defined. Please select only one (OpenSSL, LibreSSL, BoringSSL, or s2n)."
#endif

#if defined(SSL_BACKEND_KTLS) && (defined(SSL_BACKEND_BORINGSSL) || defined(SSL_BACKEND_S2N))
#error "kTLS requires OpenSSL for handshake. Cannot combine with BoringSSL or s2n."
#endif

// Get backend name at runtime
const char *ssl_get_backend_name(void);

#endif // SSL_BACKEND_H
