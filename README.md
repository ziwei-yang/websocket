# High Performance WebSocket Library

A high-performance WebSocket library designed for high-frequency trading and real-time market data processing.

## Features

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
- **SSL Library**:
  - **Linux**: OpenSSL 3.x (default, used with kTLS)
  - **macOS**: LibreSSL (default) or OpenSSL 3.x
  - Alternatives: BoringSSL (any platform)
- **CMocka** (optional): Only required for unit tests
- **Operating System**: Linux (with kTLS support), macOS, or BSD

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
# Build library with default SSL backend (LibreSSL on macOS, ktls on Linux)
make

# Build with specific SSL backend
SSL_BACKEND=ktls make          # OpenSSL with Kernel TLS (DEFAULT on Linux)
SSL_BACKEND=libressl make      # LibreSSL (DEFAULT on macOS)
SSL_BACKEND=openssl make       # OpenSSL 3.x (without kTLS)
SSL_BACKEND=boringssl make     # BoringSSL

# Build and run integration tests
make integration-test          # Binance (high-frequency market)
make integration-test-bitget   # Bitget (low-frequency market, TLS 1.2)

# kTLS targets (Linux only)
make ktls-build                # Build with kTLS
make ktls-verify               # Verify kTLS is working
make ktls-benchmark            # Compare kTLS vs OpenSSL performance

# Build SSL performance benchmark
make ssl-benchmark

# Run integration test with profiling
make integration-test-profile

# Clean build artifacts
make clean
```

### Build Output

- **`libws.a`** - Static library for linking
- **`test_binance_integration`** - High-frequency market integration test (Binance)
- **`test_bitget_integration`** - Low-frequency market integration test (Bitget, TLS 1.2 with kTLS)
- **`ssl_benchmark`** - SSL performance comparison tool across LibreSSL/BoringSSL/OpenSSL backends
- **`ssl_probe`** - SSL connection diagnostic utility

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
3. **SSL/TLS**: kTLS (Linux default), LibreSSL (macOS default), or BoringSSL/OpenSSL
4. **HTTP Parser**: Custom parser for WebSocket handshake with SNI support
5. **WebSocket Parser**: RFC 6455 compliant frame parsing with masking
6. **Ring Buffer**: Pre-allocated 8MB circular buffer with virtual memory mirroring

### Memory Model

- **Ring Buffer**: 8MB pre-allocated buffer with virtual memory mirroring
- **Virtual Mirroring**: Zero-wraparound reads via dual memory mapping (Linux/macOS)
- **Single Writer/Reader**: No contention, lock-free design
- **Cache-Line Alignment**: Producer/consumer offsets on separate cache lines (128B on Apple Silicon, 64B on x86)
- **Minimal Allocations**: Zero allocations during message processing

### SSL/TLS Backend Abstraction

The library supports multiple SSL backends through a unified `ssl.h` interface:

**Platform Defaults (Optimized for HFT):**
- **Linux**: kTLS (OpenSSL + Kernel TLS) - ~5-10% lower CPU, better latency consistency
- **macOS**: LibreSSL - clean API, best compatibility with Apple Silicon

**Available Backends:**
- **ktls**: OpenSSL with Kernel TLS offload (Linux only, production-ready for HFT)
- **openssl**: Standard OpenSSL 3.x without kernel offload
- **libressl**: LibreSSL (OpenBSD's fork)
- **boringssl**: Google's fork optimized for speed

Backend selection via `SSL_BACKEND` make variable at build time (e.g., `make SSL_BACKEND=openssl`).

### Kernel TLS (kTLS) Support - DEFAULT ON LINUX

**kTLS** is the **default SSL backend on Linux** and offloads TLS encryption/decryption from userspace to the kernel for improved performance:

- **10-30% lower latency**: Reduces SSL processing time from ~80μs to ~55μs
- **20-40% less CPU usage**: Kernel handles crypto operations more efficiently
- **Better cache utilization**: Fewer context switches and memory copies
- **Zero code changes**: Automatic fallback to OpenSSL if unavailable

#### Requirements

- **Linux kernel 4.17+** (5.2+ recommended for TLS 1.3 support)
- **OpenSSL 1.1.1+** or **OpenSSL 3.x**
- **CONFIG_TLS** enabled in kernel (check with `grep CONFIG_TLS /boot/config-$(uname -r)`)

#### Quick Start with kTLS

```bash
# 1. Enable kernel module (one-time setup)
sudo ./scripts/enable_ktls.sh

# 2. Build with kTLS backend
make ktls-build

# 3. Verify kTLS is working
make ktls-verify

# 4. Run integration test
make ktls-test

# 5. Compare performance (kTLS vs OpenSSL)
make ktls-benchmark
```

#### Verification

The integration test will show kTLS status in the SSL configuration:

```
🔐 SSL Configuration:
   Backend:               OpenSSL 3.0.13
   Cipher Suite:          TLS_AES_256_GCM_SHA384
   Hardware Acceleration: YES (AES-NI)
   TLS Mode:              kTLS (Kernel) ✅
```

For more details, see:
- **`doc/how_to_enable_ktls.md`** - Comprehensive kTLS setup and troubleshooting guide
- **`doc/ktls_proposal.md`** - kTLS design proposal
- **`doc/ktls_implementation_checklist.md`** - Implementation checklist

### TLS Version Control

Control TLS version negotiation via environment variables:

```bash
# Force TLS 1.2 (enables kTLS on Linux)
WS_ALLOW_TLS12=1 ./test_bitget_integration

# Force TLS 1.3 (disables kTLS, uses userspace OpenSSL)
WS_FORCE_TLS13=1 ./test_binance_integration

# Debug kTLS activation
WS_DEBUG_KTLS=1 ./test_binance_integration

# General WebSocket debugging
WS_DEBUG=1 ./test_binance_integration
```

**Note:** kTLS only works with TLS 1.2 in OpenSSL 3.0.13. TLS 1.3 kTLS support requires kernel patches (see `doc/how_to_enable_ktls.md`).

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

## Bitget WebSocket Integration

The Bitget integration test validates TLS 1.2 connectivity with kTLS support:

```
wss://ws.bitget.com/v3/ws/public
```

**Features:**
- **TLS 1.2 negotiation**: Forces TLS 1.2 to enable kTLS on Linux
- **Low-frequency market**: Optimized for ~20 messages/minute (BTC/USDT ticker)
- **SNI validation**: Tests Server Name Indication for CDN-backed endpoints
- **Bitget V3 API**: Uses latest WebSocket API format

**Subscription format:**
```json
{
  "op": "subscribe",
  "args": [{
    "instType": "spot",
    "topic": "ticker",
    "symbol": "BTCUSDT"
  }]
}
```

**Run the test:**
```bash
make integration-test-bitget

# Or manually with TLS 1.2 forced
WS_ALLOW_TLS12=1 ./test_bitget_integration
```

This test complements the Binance test by validating:
- TLS 1.2 + kTLS activation
- SNI support for CDN endpoints
- WebSocket frame masking (RFC 6455 compliance)
- Low-frequency data stream handling

## Project Structure

```
.
├── ringbuffer.h/c              # Lock-free ring buffer with virtual memory mirroring
├── ssl.h/c                     # SSL/TLS abstraction layer (with SNI and kTLS support)
├── ssl_backend.h               # SSL backend selection (ktls/LibreSSL/BoringSSL/OpenSSL)
├── ws.h/c                      # WebSocket protocol implementation (RFC 6455 compliant)
├── ws_notifier.h/c             # Unified event notification (kqueue/epoll/select)
├── os.h/c                      # OS abstraction for CPU cycle timing
├── test/
│   ├── integration/
│   │   ├── binance.c           # High-frequency market test (Binance)
│   │   └── bitget.c            # Low-frequency market test (Bitget, TLS 1.2)
│   ├── ssl_benchmark.c         # SSL backend performance comparison tool
│   └── ssl_probe.c             # SSL connection diagnostic utility
├── scripts/
│   ├── enable_ktls.sh          # kTLS kernel module setup script
│   └── lock_cpu_performance.sh # CPU performance mode script
├── example/
│   └── simple_ws.c             # Simple usage example
├── doc/
│   ├── websocket_design.md     # Architecture and design documentation
│   ├── how_to_enable_ktls.md   # Comprehensive kTLS setup guide
│   ├── ktls_proposal.md        # kTLS design proposal
│   └── ktls_implementation_checklist.md  # Implementation checklist
├── .gitignore                  # Git ignore patterns
├── Makefile                    # Build system with kTLS support
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


