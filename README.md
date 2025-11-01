# High Performance WebSocket Library

A high-performance WebSocket library designed for high-frequency trading and real-time market data processing.

## Features

### Core Performance
- **Ultra-low latency**: Optimized for high-frequency trading use cases (~40-180 μs median processing latency)
- **Ring buffer memory model**: Pre-allocated 8MB ring buffer with virtual memory mirroring for zero-wraparound reads
- **Zero-copy design**: Single reader/writer lock-free design eliminates contention, direct pointer access to buffers
- **SSL/TLS support**: Multi-backend abstraction supporting LibreSSL, BoringSSL, and OpenSSL 3.x
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
- **SSL Library** (choose one):
  - **LibreSSL** (recommended for macOS): Fast, secure OpenSSL fork
  - **BoringSSL**: Google's OpenSSL fork optimized for performance
  - **OpenSSL 3.x**: Standard OpenSSL implementation
- **CMocka** (optional): Only required for unit tests
- **Operating System**: Linux, macOS, or BSD

### Installation (macOS)

```bash
# LibreSSL (recommended)
brew install libressl cmocka

# Or BoringSSL (for comparison)
brew install boringssl cmocka

# Or OpenSSL 3.x
brew install openssl@3 cmocka
```

### Installation (Ubuntu/Debian)

```bash
sudo apt-get update
sudo apt-get install build-essential libssl-dev libcmocka-dev
```

### Build Commands

```bash
# Build library with default SSL backend (LibreSSL on macOS, OpenSSL on Linux)
make

# Build with specific SSL backend
SSL_BACKEND=libressl make      # LibreSSL (recommended)
SSL_BACKEND=boringssl make     # BoringSSL
SSL_BACKEND=openssl make       # OpenSSL 3.x

# Build and run integration test (Binance WebSocket)
make integration-test

# Build SSL performance benchmark
make ssl-benchmark

# Run integration test with profiling
make integration-test-profile

# Clean build artifacts
make clean
```

### Build Output

- **`libws.a`** - Static library for linking
- **`test_binance_integration`** - Real-world integration test with comprehensive latency benchmarking
- **`ssl_benchmark`** - SSL performance comparison tool across LibreSSL/BoringSSL/OpenSSL backends

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
- Runs 5 iterations with 2000 total messages (300 analyzed per run after 100-message warmup)
- Reports detailed statistics (min/max/mean/stddev/percentiles)
- Performs outlier detection using IQR method
- Aggregate statistics across all runs
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
Exchange Server → TCP/IP → SSL/TLS → WebSocket Parser → Ring Buffer → User Callback
                                ↓
                        Event Notifier (kqueue/epoll)
```

1. **TCP/IP socket**: Raw network I/O using POSIX sockets
2. **Event Notifier**: Unified abstraction over kqueue/epoll/select for I/O multiplexing
3. **SSL/TLS**: LibreSSL/BoringSSL/OpenSSL for encryption/decryption
4. **HTTP Parser**: Custom parser for WebSocket handshake
5. **WebSocket Parser**: RFC 6455 frame parsing with opcode handling
6. **Ring Buffer**: Pre-allocated 8MB circular buffer with virtual memory mirroring

### Memory Model

- **Ring Buffer**: 8MB pre-allocated buffer with virtual memory mirroring
- **Virtual Mirroring**: Zero-wraparound reads via dual memory mapping (Linux/macOS)
- **Single Writer/Reader**: No contention, lock-free design
- **Cache-Line Alignment**: Producer/consumer offsets on separate cache lines (128B on Apple Silicon, 64B on x86)
- **Minimal Allocations**: Zero allocations during message processing

### SSL/TLS Backend Abstraction

The library supports multiple SSL backends through a unified `ssl.h` interface:
- **LibreSSL**: Recommended for macOS - clean API, good performance
- **BoringSSL**: Google's fork - optimized for speed, minimal features
- **OpenSSL 3.x**: Standard implementation - widely compatible

Backend selection via `SSL_BACKEND` environment variable at build time.

## Latency Benchmarking

### Comprehensive Statistical Analysis

The integration test (`test_binance_integration`) provides production-grade latency measurement with zero I/O overhead in the hot path:

```
╔══════════════════════════════════════════════════════════════════╗
║           LATENCY BENCHMARK RESULTS (NO I/O IN HOT PATH)        ║
╚══════════════════════════════════════════════════════════════════╝

📈 Aggregate Dataset Information:
   Completed runs:             5 / 5
   Messages analyzed per run:  300
   Total analyzed messages:    1500
   Timer: mach_absolute_time() (Apple Silicon)

📊 Aggregate Processing Latency:
┌──────────────┬──────────────┬──────────────┐
│   Metric     │ Timer Ticks  │ Nanoseconds  │
├──────────────┼──────────────┼──────────────┤
│ Min          │          165 │      6875.00 │
│ Max          │        31032 │   1293000.00 │
│ Mean         │         1592 │     66291.67 │
│ Std Dev      │         1585 │     66041.67 │
│ P50 (median) │         1015 │     42291.67 │
│ P90          │         3867 │    161125.00 │
│ P95          │         4145 │    172708.33 │
│ P99          │         4576 │    190666.67 │
│ P99.9        │        25791 │   1074625.00 │
└──────────────┴──────────────┴──────────────┘

🔍 Aggregate Outlier Analysis:
   Outliers detected: 28 / 1500 (1.87%)
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
├── ringbuffer.h/c              # Lock-free ring buffer with virtual memory mirroring
├── ssl.h/c                     # SSL/TLS abstraction layer
├── ssl_backend.h               # SSL backend selection (LibreSSL/BoringSSL/OpenSSL)
├── ws.h/c                      # WebSocket protocol implementation
├── ws_notifier.h/c             # Unified event notification (kqueue/epoll/select)
├── os.h/c                      # OS abstraction for CPU cycle timing
├── test/
│   ├── integration/
│   │   └── binance.c           # Integration test with comprehensive benchmarking
│   └── ssl_benchmark.c         # SSL backend performance comparison tool
├── example/
│   └── simple_ws.c             # Simple usage example
├── doc/
│   └── websocket_design.md     # Architecture and design documentation
├── .gitignore                  # Git ignore patterns
├── Makefile                    # Build system
└── README.md                   # This file
```

## Platform-Specific Notes

### macOS (Apple Silicon)
- **Event Loop**: Uses `kqueue()` with EV_CLEAR edge-triggered mode
- **Timer**: `mach_absolute_time()` (nanosecond precision)
- **Ringbuffer**: Virtual memory mirroring typically succeeds on macOS
- **Expected Latency**: ~40-180 μs median processing latency (P50-P95)

### Linux x86_64
- **Event Loop**: Uses `epoll()` with EPOLLET edge-triggered mode
- **Timer**: TSC (Time Stamp Counter) with calibration
- **Timer Calibration**: Busy-wait method with median of 3 measurements
- **Hardware Timestamping**: Optional NIC-level packet timestamps (SO_TIMESTAMPING)
- **Ringbuffer**: Virtual memory mirroring supported via memfd_create or shm_open
- **Expected Latency**: Varies by CPU and SSL backend (typically ~20-100 μs median)

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

## SSL Backend Benchmarking

The `ssl_benchmark` tool compares SSL performance across different backends:

```bash
# Build for all backends
make ssl-benchmark

# Or build for specific backend
SSL_BACKEND=boringssl make ssl-benchmark
```

The benchmark measures:
- **SSL_read latency**: Time to decrypt data from SSL stream
- **SSL_write latency**: Time to encrypt data for transmission
- **Throughput**: Bytes processed per second
- **Memory usage**: Heap allocations during SSL operations

Use this tool to select the optimal SSL backend for your platform and use case.

## License

This is a reference implementation for educational purposes.


