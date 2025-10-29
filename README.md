# High Performance WebSocket Library

A high-performance WebSocket library designed for high-frequency trading and real-time market data processing.

## Features

- **Ultra-low latency**: Optimized for high-frequency trading use cases
- **Ring buffer memory model**: Pre-allocated 10MB ring buffer for minimal allocations
- **Zero-copy design**: Single reader/writer design eliminates contention
- **SSL/TLS support**: Built on OpenSSL with replaceable SSL backend
- **Lightweight HTTP parsing**: Custom HTTP parser optimized for WebSocket handshakes
- **CPU cycle benchmarking**: Built-in latency measurement using CPU cycles

## Building

### Prerequisites

- GCC compiler with C11 support
- OpenSSL library (libssl-dev)
- POSIX-compliant system

### Build Commands

```bash
# Build everything
make

# Debug build
make debug

# Release build (optimized)
make release

# Clean build artifacts
make clean
```

This will create:
- `libws.a` - Static library
- `binance` - Example program
- `latency` - Benchmark tool

## Usage

### Example: Receive Binance Market Data

```bash
./binance
```

This connects to Binance WebSocket stream and prints incoming market data.

### Benchmark: Measure Latency

```bash
./latency
```

This measures the latency from message arrival to processing, reporting CPU cycles and estimated microseconds.

### Using in Your Code

```c
#include "ws.h"

void on_message(websocket_context_t *ws, const uint8_t *data, size_t len) {
    // Process your message here
    printf("Received: %.*s\n", (int)len, data);
}

void on_status(websocket_context_t *ws, int status) {
    if (status == 0) {
        printf("Connected!\n");
    }
}

int main() {
    // Initialize WebSocket
    websocket_context_t *ws = ws_init("wss://your-server.com/path");
    ws_set_on_message(ws, on_message);
    ws_set_on_status(ws, on_status);
    
    // Event loop
    while (ws_get_state(ws) != WS_STATE_ERROR) {
        ws_update(ws);
        usleep(1000);  // 1ms
    }
    
    ws_free(ws);
    return 0;
}
```

## Architecture

### Data Flow

```
Exchange Server → TCP/IP → SSL/TLS → HTTP Parser → Ring Buffer → User Code
```

1. **TCP/IP socket**: Raw network I/O using socket.h
2. **SSL/TLS**: OpenSSL for encryption/decryption
3. **HTTP Parser**: Custom parser for WebSocket handshake
4. **Ring Buffer**: Pre-allocated 10MB circular buffer

### Memory Model

- **Ring Buffer**: 10MB pre-allocated buffer
- **Single Writer/Reader**: No contention, lock-free design
- **Minimal Allocations**: Zero allocations during message processing

### SSL/TLS Backend

The `ssl.h` interface wraps OpenSSL, allowing future performance enhancements or backend replacement without changing the WebSocket API.

## Benchmarking

The library includes CPU cycle-based latency measurement. The benchmark tool measures:

- Total messages received
- Average latency in CPU cycles
- Min/Max latency
- Messages per second

To calibrate your system's CPU frequency for accurate microsecond measurements, adjust the `cpu_freq_ghz` constant in `benchmark/latency.c`.

## Example URL

The library defaults to Binance public market data stream:

```
wss://stream.binance.com:9443/stream?streams=btcusdt@trade&timeUnit=MICROSECOND
```

You can override this by passing a different URL as a command-line argument.

## Project Structure

```
.
├── ringbuffer.h/c    # Ring buffer implementation
├── ssl.h/c            # SSL/TLS wrapper
├── ws.h/c             # WebSocket core library
├── example/
│   └── binance.c      # Binance example
├── benchmark/
│   └── latency.c      # Latency benchmark
└── Makefile           # Build system
```

## License

This is a reference implementation for educational purposes.


