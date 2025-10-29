#include "ws.h"
#include <stdio.h>
#include <string.h>

// Zero-copy message handler
void on_message(websocket_context_t *ws, const uint8_t *payload_ptr, size_t payload_len, uint8_t opcode) {
    if (opcode == 1) { // WS_FRAME_TEXT
        printf("Received text message: %.*s\n", (int)payload_len, payload_ptr);
    } else if (opcode == 2) { // WS_FRAME_BINARY
        printf("Received binary message: %zu bytes\n", payload_len);
        // Process binary data directly from payload_ptr - no copying!
    }
}

// Status handler
void on_status(websocket_context_t *ws, int status) {
    if (status == 0) {
        printf("WebSocket connected!\n");
    } else {
        printf("WebSocket error: %d\n", status);
    }
}

int main() {
    // Initialize WebSocket connection
    websocket_context_t *ws = ws_init("ws://localhost:8080/");
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
