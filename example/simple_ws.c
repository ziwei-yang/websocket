#include "../ws.h"
#include "../ws_notifier.h"
#include "../ssl.h"
#include <stdio.h>
#include <string.h>
#include <signal.h>
#include <unistd.h>

// WebSocket frame opcodes (from RFC 6455)
#define WS_OPCODE_TEXT   0x1
#define WS_OPCODE_BINARY 0x2
#define WS_OPCODE_CLOSE  0x8
#define WS_OPCODE_PING   0x9
#define WS_OPCODE_PONG   0xA

static int running = 1;
static int connected = 0;
static int message_count = 0;

// Zero-copy message handler
void on_message(websocket_context_t *ws, const uint8_t *payload_ptr, size_t payload_len, uint8_t opcode) {
    (void)ws;

    if (opcode == WS_OPCODE_TEXT) {
        printf("\n📩 Received text message (%zu bytes):\n%.*s\n",
               payload_len, (int)payload_len, payload_ptr);
    } else if (opcode == WS_OPCODE_BINARY) {
        printf("\n📩 Received binary message: %zu bytes\n", payload_len);
        // Process binary data directly from payload_ptr - no copying!
    } else if (opcode == WS_OPCODE_PING) {
        printf("🏓 Received PING\n");
    } else if (opcode == WS_OPCODE_PONG) {
        printf("🏓 Received PONG\n");
    }

    message_count++;
    if (message_count >= 3) {
        printf("\n✅ Received %d messages, exiting...\n", message_count);
        running = 0;
    }
}

// Status handler
void on_status(websocket_context_t *ws, int status) {
    if (status == 0) {
        printf("✅ WebSocket connected successfully!\n\n");

        // Display SSL configuration (new feature from SSL optimization)
        printf("🔐 SSL Configuration:\n");
        const char *backend_version = ssl_get_backend_version();
        printf("   Backend:               %s\n", backend_version ? backend_version : "Unknown");

        const char *cipher_name = ws_get_cipher_name(ws);
        printf("   Cipher Suite:          %s\n", cipher_name ? cipher_name : "Unknown");

        int hw_crypto = ssl_has_hw_crypto();
        printf("   Hardware Acceleration: %s", hw_crypto ? "YES" : "NO");
        if (hw_crypto) {
#if defined(__x86_64__) || defined(__i386__)
            printf(" (AES-NI)");
#elif defined(__aarch64__) || defined(__arm64__)
            printf(" (ARM Crypto Extensions)");
#endif
        }
        printf("\n\n");

        connected = 1;
    } else {
        ws_state_t state = ws_get_state(ws);
        printf("⚠️  WebSocket status change: %d (state: %d)\n", status, state);

        if (status == -1) {
            printf("❌ Connection failed\n");
            running = 0;
        } else if (state == WS_STATE_ERROR && connected) {
            printf("❌ Connection error detected\n");
            running = 0;
        } else if (state == WS_STATE_CLOSED) {
            printf("📴 Connection closed\n");
            running = 0;
        }
    }
}

void signal_handler(int sig) {
    (void)sig;
    printf("\n\n⚠️  Caught interrupt signal, shutting down...\n");
    running = 0;
}

int main(int argc, char *argv[]) {
    const char *default_url = "wss://echo.websocket.org/";
    const char *url = (argc > 1) ? argv[1] : default_url;

    signal(SIGINT, signal_handler);
    signal(SIGTERM, signal_handler);

    printf("═══════════════════════════════════════════\n");
    printf("  WebSocket Library - Simple Example\n");
    printf("═══════════════════════════════════════════\n\n");

    if (argc > 1) {
        printf("Connecting to: %s\n\n", url);
    } else {
        printf("Connecting to: %s\n", url);
        printf("(You can specify a custom URL as first argument)\n\n");
    }

    // Initialize WebSocket connection
    websocket_context_t *ws = ws_init(url);
    if (!ws) {
        fprintf(stderr, "❌ Failed to initialize WebSocket\n");
        return 1;
    }

    // Set callbacks
    ws_set_on_msg(ws, on_message);
    ws_set_on_status(ws, on_status);

    // Create unified event notifier
    ws_notifier_t *notifier = ws_notifier_init();
    if (!notifier) {
        fprintf(stderr, "❌ Failed to create event notifier\n");
        ws_free(ws);
        return 1;
    }

#ifdef __linux__
    printf("📡 Event backend: epoll (Linux)\n\n");
#elif defined(__APPLE__)
    printf("📡 Event backend: kqueue (macOS)\n\n");
#else
    printf("📡 Event backend: select (fallback)\n\n");
#endif

    // Wait for connection with 30-second timeout
    time_t start_time = time(NULL);
    int connection_timeout = 30; // seconds
    while (running && !connected) {
        ws_update(ws);
        usleep(1000); // 1ms

        if (time(NULL) - start_time > connection_timeout) {
            fprintf(stderr, "❌ Connection timeout after %d seconds\n", connection_timeout);
            running = 0;
            break;
        }
    }

    if (connected) {
        // Register file descriptor with notifier
        int fd = ws_get_fd(ws);
        if (fd >= 0) {
            if (ws_notifier_add(notifier, fd, WS_EVENT_READ) < 0) {
                fprintf(stderr, "❌ Failed to register fd with notifier\n");
                running = 0;
            }
        }

        // Send test messages
        printf("📤 Sending test messages...\n\n");

        const char *messages[] = {
            "Hello, WebSocket!",
            "This is message 2",
            "Final test message"
        };

        for (int i = 0; i < 3 && running; i++) {
            ws_send(ws, (const uint8_t *)messages[i], strlen(messages[i]));
            printf("📨 Sent: %s\n", messages[i]);
            usleep(100000); // 100ms delay between messages
        }

        printf("\n⏳ Waiting for echo responses...\n");

        // Main event loop
        while (running) {
            ws_notifier_wait(notifier);
            ws_update(ws);

            if (!running) break;
        }
    }

    // Cleanup
    printf("\n🔧 Cleaning up...\n");
    ws_notifier_free(notifier);
    ws_close(ws);
    ws_free(ws);

    printf("✅ Example completed successfully!\n");
    return 0;
}
