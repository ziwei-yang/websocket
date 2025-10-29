# Running Tests

## Prerequisites

Install the testing framework:

```bash
sudo apt-get install libcmocka-dev
```

## Running Tests

```bash
make test
```

Or build test only:

```bash
make test-build
./test_ringbuffer
```

## Test Coverage

The `ringbuffer_test.c` includes comprehensive tests for:

- ✅ Buffer initialization
- ✅ Available space calculations  
- ✅ Basic write/read operations
- ✅ Zero-copy write pointer access
- ✅ Zero-copy read pointer access
- ✅ Peek read (non-destructive)
- ✅ Wrap-around handling
- ✅ Large data handling (1MB+)
- ✅ Full buffer detection
- ✅ Overflow protection
- ✅ Sequential operations
- ✅ Zero-length operations

## Test Framework

We use **CMocka** - a modern, elegant unit testing framework for C.

CMocka provides:
- Clean, readable test syntax
- Excellent error reporting
- Support for mocking
- Memory leak detection
- No external runtime dependencies

Learn more: https://cmocka.org/


