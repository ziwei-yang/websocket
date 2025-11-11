# High Performance WebSocket Library

### Real-World Performance: Intel i9-12900 + Linux + kTLS

Benchmark results on Intel i9-12900 with Linux kernel kTLS (OpenSSL backend):

```
📊 Latency Breakdown (Mean) - 6 Stages:
   Stage 2: EVENT→RECV_START (prep):            271 ticks (  111.00 ns)  [  0.4%]
   Stage 3: RECV_START→END (SSL decrypt):     72529 ticks (29981.00 ns)  [ 96.6%]
   Stage 4: RECV_END→PARSED (WS parse):         366 ticks (  151.00 ns)  [  0.5%]
   Stage 5: PARSED→CALLBACK (app setup):       1901 ticks (  785.00 ns)  [  2.5%]
   ────────────────────────────────────────────────────────────
   Total (EVENT→CALLBACK):                    75067 ticks (31030.00 ns)  [100.0%]
```

**Key Insights:**
- **Total latency**: ~31 μs from event notification to application callback
- **SSL decryption**: ~30 μs (96.6% of total) - handled by kernel via kTLS
- **Library overhead**: ~1 μs (3.4% of total) - WebSocket parsing + frame handling + setup
  - Event preparation: ~111 ns (0.4%)
  - WebSocket parsing: ~151 ns (0.5%)
  - Application setup: ~785 ns (2.5%)
- **Platform**: Intel i9-12900, TSC frequency ~2.42 GHz

This demonstrates the library's minimal processing overhead (~1 μs) with most latency attributed to SSL/TLS decryption, which is efficiently handled by the Linux kernel through kTLS offload. The granular 6-stage breakdown provides precise visibility into where every microsecond is spent.

### Core Performance
- **Ultra-low latency**: Optimized for high-frequency trading use cases (~40-180 μs median processing latency)
- **Ring buffer memory model**: Pre-allocated 8MB ring buffer with virtual memory mirroring for zero-wraparound reads
- **Zero-copy design**: Single reader/writer lock-free design eliminates contention, direct pointer access to buffers
- **SSL/TLS support**: Multi-backend abstraction (kTLS default on Linux, LibreSSL default on macOS)
- **Lightweight HTTP parsing**: Custom HTTP parser optimized for WebSocket handshakes

### Platform-Specific Optimizations
- **Event loops**: Platform-optimized I/O multiplexing  
  - Linux: `epoll()` for high-performance event handling  
  - macOS/BSD: `kqueue()` for efficient event notification  
  - Fallback: `select()` for portability
- **Timer precision**: Architecture-aware timestamp measurement  
  - ARM64: 24 MHz system counter (Apple Silicon) with hardware register reads  
  - x86_64: TSC (Time Stamp Counter) with calibration

### Using the Library in Your Code

```c
#include "ws.h"
#include <stdio.h>
#include <unistd.h>

void on_message(websocket_context_t *ws, const uint8_t *data,
                size_t len, uint8_t opcode) {
    (void)ws;
    (void)opcode;

    // Process your message here
    printf("Received %zu bytes: %.*s\n", len, (int)len, data);
}

void on_status(websocket_context_t *ws, int status) {
    (void)ws;

    if (status == 0) {
        printf("WebSocket connected!\n");
    } else {
        printf("Status change: %d\n", status);
    }
}

int main() {
    // Initialize WebSocket connection
    websocket_context_t *ws = ws_init("wss://stream.binance.com:443/ws/btcusdt@trade");
    if (!ws) {
        fprintf(stderr, "Failed to initialize WebSocket\n");
        return 1;
    }

    // Set callbacks
    ws_set_on_msg(ws, on_message);    // Callback for incoming messages
    ws_set_on_status(ws, on_status);  // Callback for connection status

    // Main event loop
    // Note: PING/PONG and CLOSE frames are handled automatically
    while (ws_get_state(ws) != WS_STATE_ERROR &&
           ws_get_state(ws) != WS_STATE_CLOSED) {
        ws_update(ws);                 // Process I/O and parse frames
        usleep(1000);                  // 1ms polling interval
    }

    // Cleanup (sends proper CLOSE frame per RFC 6455)
    ws_close(ws);
    ws_free(ws);
    return 0;
}
```

**Compile and link:**
```bash
gcc -o my_app my_app.c libws.a -lssl -lcrypto -lm
```

### Running Integration Test with Benchmarking

```bash
# Run integration test (connects to live Binance WebSocket)
make integration-test

# Or run directly
./test_binance_integration

# With CPU affinity (pin to core 2)
./test_binance_integration --cpu 2

# With real-time priority (requires sudo)
sudo ./test_binance_integration --rt-priority 50

# Combine both optimizations
sudo ./test_binance_integration --cpu 2 --rt-priority 50
```

The integration test connects to Binance's real-time market data stream and provides comprehensive latency analysis:
- Runs 5 iterations with 2000 total messages (300 analyzed per run after 100-message warmup)
- Reports detailed statistics (min/max/mean/stddev/percentiles)
- Performs outlier detection using IQR method
- Aggregate statistics across all runs
- Zero I/O overhead during measurement
