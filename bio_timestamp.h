#ifndef BIO_TIMESTAMP_H
#define BIO_TIMESTAMP_H

#include <stdint.h>
#include <openssl/bio.h>

// Shared structure to store hardware timestamp from BIO layer
typedef struct {
    uint64_t hw_timestamp_ns;  // Hardware NIC timestamp in nanoseconds (0 if unavailable)
    int hw_available;          // Flag indicating if hardware timestamp was captured
} bio_timestamp_t;

#ifdef __linux__
// Create a custom BIO for a socket with hardware timestamping support
// Returns BIO* on success, NULL on failure
BIO* BIO_new_timestamp_socket(int fd, bio_timestamp_t *ts_storage);
#endif

#endif // BIO_TIMESTAMP_H
