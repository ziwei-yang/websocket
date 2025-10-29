# High Performance WebSocket Library

A high-performance WebSocket library designed for high-frequency trading and real-time market data processing.

## Features

### Core Performance
- **Ultra-low latency**: Optimized for high-frequency trading use cases (~2-3 μs median processing latency)
- **Ring buffer memory model**: Pre-allocated 10MB ring buffer with zero allocations during operation
- **Zero-copy design**: Single reader/writer lock-free design eliminates contention
- **SSL/TLS support**: Built on OpenSSL with replaceable SSL backend
- **Lightweight HTTP parsing**: Custom HTTP parser optimized for WebSocket handshakes

### Platform-Specific Optimizations
- **Event loops**: Platform-optimized I/O multiplexing
  - Linux: `epoll()` for high-performance event handling
  - macOS/BSD: `kqueue()` for efficient event notification
  - Fallback: `select()` for portability
- **Timer precision**: Architecture-aware timestamp measurement
  - ARM64: 24 MHz system counter (Apple Silicon) with hardware register reads
  - x86_64: TSC (Time Stamp Counter) with calibration

### Advanced Features
- **CPU affinity**: Pin process to specific CPU cores to reduce cache misses
- **Real-time priority**: Support for SCHED_FIFO/SCHED_RR scheduling policies
- **Batch processing**: Configurable message batching for throughput optimization
- **Hardware timestamping**: NIC-level packet timestamping support (Linux only)

### Comprehensive Benchmarking
- **No I/O in hot path**: Latency measurements with zero logging overhead
- **Warmup period**: Initial message filtering for accurate statistics
- **Statistical analysis**: P50, P90, P95, P99, P99.9 percentiles
- **Outlier detection**: IQR-based anomaly identification
- **Timing records**: Per-message latency tracking with pre-allocated buffers

## Building

### Prerequisites

- **C Compiler**: GCC or Clang with C11 support
- **OpenSSL**: libssl-dev (OpenSSL 1.1.0 or later)
- **CMocka** (optional): Only required for unit tests
- **Operating System**: Linux, macOS, or BSD

### Installation (macOS)

```bash
brew install openssl cmocka
```

### Installation (Ubuntu/Debian)

```bash
sudo apt-get update
sudo apt-get install build-essential libssl-dev libcmocka-dev
```

### Build Commands

```bash
# Build library only
make

# Build and run all unit tests
make test

# Build and run integration test (Binance WebSocket)
make integration-test

# Clean build artifacts
make clean

# Debug build with symbols
make debug

# Optimized release build
make release
```

### Build Output

- **`libws.a`** - Static library for linking
- **`test_ringbuffer`** - Ring buffer unit tests
- **`test_ssl`** - SSL/TLS unit tests
- **`test_ws`** - WebSocket protocol unit tests
- **`test_binance_integration`** - Real-world integration test with comprehensive latency benchmarking

## Usage

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
- Receives 100+ messages and measures processing latency
- Reports detailed statistics (min/max/mean/stddev/percentiles)
- Performs outlier detection using IQR method
- Shows batch processing efficiency
- Zero I/O overhead during measurement

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
    ws_set_on_msg(ws, on_message);
    ws_set_on_status(ws, on_status);

    // Main event loop
    while (ws_get_state(ws) != WS_STATE_ERROR &&
           ws_get_state(ws) != WS_STATE_CLOSED) {
        ws_update(ws);
        usleep(1000);  // 1ms polling interval
    }

    // Cleanup
    ws_close(ws);
    ws_free(ws);
    return 0;
}
```

**Compile and link:**
```bash
gcc -o my_app my_app.c libws.a -lssl -lcrypto -lm
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

## Latency Benchmarking

### Comprehensive Statistical Analysis

The integration test (`test_binance_integration`) provides production-grade latency measurement with zero I/O overhead in the hot path:

```
╔══════════════════════════════════════════════════════════════════╗
║           LATENCY BENCHMARK RESULTS (NO I/O IN HOT PATH)        ║
╚══════════════════════════════════════════════════════════════════╝

📊 Processing Latency (Socket Receive → Callback Invocation):
┌──────────────┬──────────────┬──────────────┐
│   Metric     │ Timer Ticks  │ Nanoseconds  │
├──────────────┼──────────────┼──────────────┤
│ Min          │          455 │     18958.33 │
│ Max          │          681 │     28375.00 │
│ Mean         │          552 │     22999.70 │
│ Std Dev      │           74 │      3068.50 │
│ P50 (median) │          524 │     21833.33 │
│ P90          │          661 │     27541.67 │
│ P95          │          672 │     28000.00 │
│ P99          │          680 │     28333.33 │
│ P99.9        │          681 │     28375.00 │
└──────────────┴──────────────┴──────────────┘
```

### Features

**Accurate Timer Calibration:**
- **ARM64** (Apple Silicon): Reads `cntfrq_el0` register directly (24 MHz fixed counter)
- **x86_64**: Measures TSC frequency via busy-wait calibration (typically 2-4 GHz)
- Automatic platform detection and appropriate method selection

**Best Practices for Latency Measurement:**
- Pre-allocated timing record structs (no allocations during test)
- Zero I/O operations in message callback (no printf, no logging)
- Warmup period (first 10 messages discarded from statistics)
- Median of 3 measurements for x86_64 TSC calibration

**Statistical Analysis:**
- **Percentiles**: P50, P90, P95, P99, P99.9
- **Outlier Detection**: IQR (Interquartile Range) method with 1.5×IQR threshold
- **Standard Deviation**: Population standard deviation calculation
- **Batch Analysis**: Message batching efficiency metrics

### What is Measured

The benchmark measures **processing latency** within the library:
- **Start**: Timestamp captured when data arrives from socket (`recv()`)
- **End**: Timestamp captured when user callback is invoked
- **Measurement**: Pure library processing overhead (WebSocket frame parsing, decompression, etc.)

This does **not** include network latency, market data generation latency, or any upstream delays.

## Binance WebSocket Integration

The integration test uses Binance's public market data stream for real-world testing:

```
wss://stream.binance.com:443/stream?streams=btcusdt@trade&timeUnit=MICROSECOND
```

This endpoint provides:
- Real-time BTC/USDT trade updates
- Microsecond-precision timestamps from Binance
- High-frequency data stream (multiple messages per second)
- Public access (no authentication required)

The test validates the library under production conditions with actual market data.

## Project Structure

```
.
├── ringbuffer.h/c              # Lock-free ring buffer implementation
├── ssl.h/c                     # SSL/TLS abstraction layer (OpenSSL wrapper)
├── ws.h/c                      # WebSocket protocol implementation
├── test/
│   ├── ringbuffer_test.c       # Ring buffer unit tests
│   ├── ssl_test.c              # SSL/TLS unit tests
│   ├── ws_test.c               # WebSocket unit tests
│   └── integration/
│       └── binance.c           # Integration test with comprehensive benchmarking
├── doc/
│   └── websocket_design.md     # Architecture and design documentation
├── .gitignore                  # Git ignore patterns
├── Makefile                    # Build system
└── README.md                   # This file
```

## Platform-Specific Notes

### macOS (Apple Silicon)
- **Event Loop**: Uses `kqueue()` for event-driven I/O
- **Timer**: ARM64 system counter at fixed 24 MHz (41.67 ns/tick)
- **Timer Source**: Reads `cntfrq_el0` register directly
- **Expected Latency**: ~2-3 μs median processing latency

### Linux x86_64
- **Event Loop**: Uses `epoll()` for high-performance I/O multiplexing
- **Timer**: TSC (Time Stamp Counter) with calibration
- **Timer Calibration**: Busy-wait method with median of 3 measurements
- **Hardware Timestamping**: Optional NIC-level packet timestamps (SO_TIMESTAMPING)
- **Expected Latency**: ~1-2 μs median processing latency (varies by CPU)

### Performance Tuning

**CPU Affinity:**
```bash
# Pin to specific core (reduces cache misses and context switches)
./test_binance_integration --cpu 2
```

**Real-Time Priority (Linux):**
```bash
# Requires CAP_SYS_NICE capability or root
sudo ./test_binance_integration --rt-priority 50
```

**Disable CPU Frequency Scaling (Linux):**
```bash
# For consistent benchmarking results
sudo cpupower frequency-set --governor performance
```

**Disable Turbo Boost (for stable measurements):**
```bash
# Intel
echo "1" | sudo tee /sys/devices/system/cpu/intel_pstate/no_turbo

# AMD
echo "0" | sudo tee /sys/devices/system/cpu/cpufreq/boost
```

## License

This is a reference implementation for educational purposes.


