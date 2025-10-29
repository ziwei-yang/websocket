#include "../ws.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>
#include <unistd.h>

// Global variables for signal handling
static websocket_context_t *g_ws = NULL;
static volatile int g_running = 1;
static int message_count = 0;

// Signal handler for graceful shutdown
void signal_handler(int sig) {
    printf("\nReceived signal %d, shutting down gracefully...\n", sig);
    g_running = 0;
    if (g_ws) {
        ws_close(g_ws);
    }
}

// Zero-copy message handler for Binance trade data
void on_binance_message(websocket_context_t *ws, const uint8_t *payload_ptr, size_t payload_len, uint8_t opcode) {
    message_count++;
    
    // Print message info
    printf("Message #%d: opcode=%d, len=%zu\n", message_count, opcode, payload_len);
    
    // Print the actual message content (zero-copy!)
    printf("Data: %.*s\n", (int)payload_len, payload_ptr);
    printf("---\n");
    
    // Limit output for testing (remove this in production)
    if (message_count >= 10) {
        printf("Received %d messages, stopping...\n", message_count);
        g_running = 0;
        ws_close(ws);
    }
}

// Status handler
void on_status(websocket_context_t *ws __attribute__((unused)), int status) {
    if (status == 0) {
        printf("✅ WebSocket connected to Binance!\n");
    } else {
        printf("❌ WebSocket error: %d\n", status);
        printf("Current state: %d\n", ws_get_state(ws));
        g_running = 0;
    }
}

int main() {
    printf("Binance WebSocket Integration Test\n");
    printf("==================================\n");
    
    // Set up signal handlers for graceful shutdown
    signal(SIGINT, signal_handler);
    signal(SIGTERM, signal_handler);
    
    // Initialize WebSocket connection to Binance
    const char *binance_url = "wss://stream.binance.com:9443/ws/btcusdt@trade";
    printf("Connecting to: %s\n", binance_url);
    
    g_ws = ws_init(binance_url);
    if (!g_ws) {
        printf("❌ Failed to initialize WebSocket connection\n");
        return 1;
    }
    
    // Set callbacks
    ws_set_on_msg(g_ws, on_binance_message);
    ws_set_on_status(g_ws, on_status);
    
    printf("🔄 Starting WebSocket event loop...\n");
    printf("Press Ctrl+C to stop\n\n");
    
    // Main event loop
    while (g_running) {
        int result = ws_update(g_ws);
        
        if (result < 0) {
            printf("❌ WebSocket update failed: %d\n", result);
            break;
        }
        
        // Small delay to prevent busy waiting
        usleep(1000); // 1ms
    }
    
    printf("\n📊 Final Statistics:\n");
    printf("- Total messages received: %d\n", message_count);
    printf("- Final state: %d\n", ws_get_state(g_ws));
    
    // Cleanup
    if (g_ws) {
        ws_free(g_ws);
    }
    
    printf("✅ Integration test completed\n");
    return 0;
}
