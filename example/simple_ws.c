#include "ws.h"
#include <stdio.h>
#include <string.h>

// WebSocket frame opcodes (from RFC 6455)
#define WS_OPCODE_TEXT   0x1
#define WS_OPCODE_BINARY 0x2
#define WS_OPCODE_CLOSE  0x8
#define WS_OPCODE_PING   0x9
#define WS_OPCODE_PONG   0xA

// Zero-copy message handler
void on_message(websocket_context_t *ws, const uint8_t *payload_ptr, size_t payload_len, uint8_t opcode) {
    if (opcode == WS_OPCODE_TEXT) {
        printf("Received text message: %.*s\n", (int)payload_len, payload_ptr);
    } else if (opcode == WS_OPCODE_BINARY) {
        printf("Received binary message: %zu bytes\n", payload_len);
        // Process binary data directly from payload_ptr - no copying!
    }
    (void)ws; // Suppress unused parameter warning
}

// Status handler
void on_status(websocket_context_t *ws, int status) {
    (void)ws; // Suppress unused parameter warning
    if (status == 0) {
        printf("WebSocket connected!\n");
    } else {
        printf("WebSocket error: %d\n", status);
    }
}

int main() {
    // Initialize WebSocket connection
    // Note: This library only supports secure WebSocket connections (wss://)
    // SSL/TLS is always used, even if you specify ws:// in the URL
    websocket_context_t *ws = ws_init("wss://echo.websocket.org/");
    if (!ws) {
        printf("Failed to initialize WebSocket\n");
        return 1;
    }
    
    // Set callbacks
    ws_set_on_msg(ws, on_message);
    ws_set_on_status(ws, on_status);
    
    // Main event loop
    while (ws_get_state(ws) != WS_STATE_CLOSED) {
        ws_update(ws);
        
        // Send a test message when connected
        if (ws_get_state(ws) == WS_STATE_CONNECTED) {
            const char *msg = "Hello, WebSocket!";
            ws_send(ws, (const uint8_t *)msg, strlen(msg));
            break; // Exit after sending one message for demo
        }
    }
    
    // Cleanup
    ws_free(ws);
    return 0;
}
