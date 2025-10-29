# Integration Tests

This directory contains integration tests that connect to real WebSocket services.

## Binance Integration Test

The `binance.c` test connects to Binance's public WebSocket API for BTC/USDT trade data.

### Usage

```bash
# Build and run the integration test
make integration-test

# Or just build it
make integration-test-build
# Then run manually
./test_binance_integration
```

### Expected Behavior

- Connects to Binance WebSocket API
- Receives at least 100 messages within 10 seconds (as per design requirements)
- Displays received trade data
- Demonstrates zero-copy performance

### Binance Endpoint

- URL: `wss://stream.binance.com:443/stream?streams=btcusdt@trade&timeUnit=MICROSECOND`
- Public market data, no authentication required
- Stream: BTC/USDT trades

### Notes

- This test requires an active internet connection
- The test will run indefinitely until interrupted (Ctrl+C) or until 10 messages are received
