# High Performance WebSocket Library

This library delivers high-performance WebSocket capabilities optimized for high-frequency trading (HFT) applications. A typical use case involves receiving market data updates from exchanges and parsing JSON-structured data from incoming messages. The library is designed for single-threaded operation, eliminating the need for thread-safety considerations.

### Data Flow

Exchange Server → TCP/IP Socket → SSL/TLS → Ringbuffer -> HTTP/Websocket Parser → on_msg()

- **TCP/IP Socket**: Uses socket.h to handle network traffic
- **SSL/TLS**: Uses OpenSSL/LibreSSL/OpenSSL+kTLS(Linux only) library for handshaking and message encryption/decryption support.
- **HTTP/Websocket**: Custom implementation (no external library) that parses HTTP 200 OK responses and extracts payload content
- **Event poll**: epoll on Linux, kqueue on macos

### Memory Model

- **Ring Buffer**: Pre-allocated 8192KB ring buffer for receiving data from rx_queue and transmitting to tx_queue
- **Zero-Copy Operations**: Both sending and consuming data within the ring buffer use offset-based operations with zero-copy semantics
  - Specifically: `ringbuffer_next_read(rb, *data, *len)` retrieves the next readable memory pointer and available content length. `ws_send()` is invoked with the address and offset in the tx_queue buffer directly. No in-stack `buffer[]` is used for data transmission.
- **Single Producer-Consumer Model**: The ring buffer is designed for exactly one writer and one reader, eliminating contention. The SSL context is the sole writer, writing directly into the ring buffer via `SSL_read()`.


### SSL/TLS Library Replaceable Design

The `ssl.h` module provides an abstraction layer that wraps OpenSSL standard methods, enabling future TLS performance enhancements or alternative implementations.

This project prioritizes extreme performance, and security features that introduce latency can be omitted. Any steps that increase latency should be skipped. The library is intentionally thread-unsafe, with no threads or locks introduced to maximize performance.

### Code Structure

```
ws.h/c
ssl.h/c
ringbuffer.h/c
ws_notifier.h/c # Event machine
os.h/c # OS API: cpu cycle, etc.
test/ws_test.c
test/ssl_test.c
test/ringbuffer_test.c
test/integration/binance.c
```

### Testing and Benchmarking

#### Unit Tests
- Test files for each C module should be located in `/test/`
- Run all unit tests using the Makefile task: `make test`

#### Integration Test
- A real-world integration test using the Binance WebSocket API as the data source
- **Requirements**:
  - Receive at least 100 messages within 10 seconds
  - Measure and validate reasonable NIC-to-RAM latency
- **Test location**: `/test/integration/`
- **Makefile task**: `make integration-test`
- **Binance endpoint**: `wss://stream.binance.com:443/stream?streams=btcusdt@trade&timeUnit=MICROSECOND`

#### Latency Measurement
- Record CPU cycle count when the message arrives at the socket layer
- Record CPU cycle count when the message decrypted after the ssl_read()
- Record CPU cycle count when the message appears in user-space memory
- Calculate processing latency by comparing these timestamps









