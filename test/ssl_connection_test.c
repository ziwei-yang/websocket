#include "../ssl.h"
#include <stdio.h>
#include <stdlib.h>

int main() {
    printf("Testing SSL connection to echo.websocket.org:443\n");
    
    ssl_context_t *ssl = ssl_init("echo.websocket.org", 443);
    if (!ssl) {
        printf("❌ Failed to initialize SSL context\n");
        return 1;
    }
    
    printf("✅ SSL context initialized\n");
    
    // Try handshake multiple times (non-blocking)
    int attempts = 0;
    int result;
    do {
        result = ssl_handshake(ssl);
        attempts++;
        printf("Handshake attempt %d: result=%d\n", attempts, result);
        if (result == 0) {
            usleep(10000); // Wait 10ms
        }
    } while (result == 0 && attempts < 100); // Max 100 attempts
    
    if (result == 1) {
        printf("✅ SSL handshake successful after %d attempts\n", attempts);
    } else {
        printf("❌ SSL handshake failed after %d attempts\n", attempts);
    }
    
    ssl_free(ssl);
    return 0;
}
