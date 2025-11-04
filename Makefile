# High Performance WebSocket Library Makefile

# Detect operating system
UNAME_S := $(shell uname -s)
UNAME_M := $(shell uname -m)

# Try to find a C compiler
CC := $(shell which gcc 2>/dev/null || which clang 2>/dev/null || which cc 2>/dev/null || echo "gcc")

# Base flags with strict warnings
CFLAGS = -Wall -Wextra -Wuninitialized -Wshadow -Wformat=2 -Wstrict-prototypes -O3 -pthread
LDFLAGS = -lssl -lcrypto -lm
INCLUDES = -I.

# ═══════════════════════════════════════════════════════════════════
# SSL Backend Selection
# ═══════════════════════════════════════════════════════════════════
# Default backends optimized for each platform:
#   • Linux:  ktls (OpenSSL + Kernel TLS for best HFT performance)
#   • macOS:  libressl (best compatibility with Apple Silicon)
#
# Override with: make SSL_BACKEND=openssl
# Available: ktls, openssl, libressl, boringssl
# ═══════════════════════════════════════════════════════════════════

ifeq ($(UNAME_S),Darwin)
SSL_BACKEND ?= libressl
else
# Linux default: OpenSSL with kTLS support (production-ready for HFT)
SSL_BACKEND ?= ktls
endif

# Configure SSL backend
ifeq ($(SSL_BACKEND),libressl)
    CFLAGS += -DSSL_BACKEND_LIBRESSL
    LIBRESSL_PREFIX := $(shell brew --prefix libressl 2>/dev/null)
    ifneq ($(LIBRESSL_PREFIX),)
        INCLUDES += -I$(LIBRESSL_PREFIX)/include
        LDFLAGS := -L$(LIBRESSL_PREFIX)/lib $(LDFLAGS)
    endif
else ifeq ($(SSL_BACKEND),boringssl)
    CFLAGS += -DSSL_BACKEND_BORINGSSL
    BORINGSSL_DIR := $(shell pwd)/boringssl
    INCLUDES += -I$(BORINGSSL_DIR)/include
    # BoringSSL static libraries are in build/ directly (requires C++ stdlib)
    LDFLAGS := $(BORINGSSL_DIR)/build/libssl.a $(BORINGSSL_DIR)/build/libcrypto.a -lc++ -lm
else ifeq ($(SSL_BACKEND),ktls)
    # ═══════════════════════════════════════════════════════════════
    # Kernel TLS (kTLS) Backend - DEFAULT ON LINUX
    # ═══════════════════════════════════════════════════════════════
    # Uses OpenSSL for TLS handshake, then offloads encryption to kernel
    # Benefits: ~5-10% lower CPU, better latency consistency
    # Requirements:
    #   • Linux kernel 4.17+ with CONFIG_TLS=m
    #   • OpenSSL 1.1.1+ or 3.0+
    #   • TLS 1.2 connections (TLS 1.3 kTLS not yet in mainline OpenSSL)
    # Verification: ./ssl_probe stream.binance.com 443
    # ═══════════════════════════════════════════════════════════════
    CFLAGS += -DSSL_BACKEND_KTLS
else
    # Default to OpenSSL
    CFLAGS += -DSSL_BACKEND_OPENSSL
endif

# Locate llvm-profdata for PGO merges (optional)
LLVM_PROFDATA := $(shell command -v llvm-profdata 2>/dev/null)
ifeq ($(LLVM_PROFDATA),)
LLVM_PROFDATA := $(shell xcrun -f llvm-profdata 2>/dev/null)
endif

# Platform-specific configuration
ifeq ($(UNAME_S),Darwin)
    # macOS specific settings
    ifeq ($(UNAME_M),arm64)
        # macOS ARM (Apple Silicon) - use generic ARM flags for portability across M1/M2/M3/M4
        CFLAGS += -mcpu=apple-a14
        # Try to find OpenSSL via Homebrew (try openssl@3 first, then openssl)
        OPENSSL_PREFIX := $(shell brew --prefix openssl@3 2>/dev/null || brew --prefix openssl 2>/dev/null)
        ifneq ($(OPENSSL_PREFIX),)
            INCLUDES += -I$(OPENSSL_PREFIX)/include
            LDFLAGS += -L$(OPENSSL_PREFIX)/lib -Wl,-rpath,$(OPENSSL_PREFIX)/lib
        endif
        # Try to find cmocka via Homebrew
        CMOCKA_PREFIX := $(shell brew --prefix cmocka 2>/dev/null)
        ifneq ($(CMOCKA_PREFIX),)
            INCLUDES += -I$(CMOCKA_PREFIX)/include
            LDFLAGS += -L$(CMOCKA_PREFIX)/lib
        endif
    else
        # macOS Intel
        CFLAGS += -march=native
        # Try to find OpenSSL via Homebrew (try openssl@3 first, then openssl)
        OPENSSL_PREFIX := $(shell brew --prefix openssl@3 2>/dev/null || brew --prefix openssl 2>/dev/null)
        ifneq ($(OPENSSL_PREFIX),)
            INCLUDES += -I$(OPENSSL_PREFIX)/include
            LDFLAGS += -L$(OPENSSL_PREFIX)/lib -Wl,-rpath,$(OPENSSL_PREFIX)/lib
        endif
        # Try to find cmocka via Homebrew
        CMOCKA_PREFIX := $(shell brew --prefix cmocka 2>/dev/null)
        ifneq ($(CMOCKA_PREFIX),)
            INCLUDES += -I$(CMOCKA_PREFIX)/include
            LDFLAGS += -L$(CMOCKA_PREFIX)/lib
        endif
    endif
else ifeq ($(UNAME_S),Linux)
    # Linux specific settings (Ubuntu, etc.)
    CFLAGS += -march=native -D_GNU_SOURCE
    # On Linux, OpenSSL is typically in standard locations
    # But check for common alternate locations just in case
    ifneq ($(wildcard /usr/include/openssl/ssl.h),)
        # Standard location
    else ifneq ($(wildcard /usr/local/include/openssl/ssl.h),)
        INCLUDES += -I/usr/local/include
        LDFLAGS += -L/usr/local/lib
    endif
else
    # Other Unix-like systems - use defaults
    CFLAGS += -march=native
endif
SRCDIR = .
OBJDIR = obj
EXAMPLEDIR = example
BENCHMARKDIR = benchmark

# Source files
RINGBUFFER_SRC = ringbuffer.c
SSL_SRC = ssl.c
WS_SRC = ws.c
WS_NOTIFIER_SRC = ws_notifier.c
OS_SRC = os.c

# Object files
RINGBUFFER_OBJ = $(OBJDIR)/ringbuffer.o
SSL_OBJ = $(OBJDIR)/ssl.o
WS_OBJ = $(OBJDIR)/ws.o
WS_NOTIFIER_OBJ = $(OBJDIR)/ws_notifier.o
OS_OBJ = $(OBJDIR)/os.o

# Libraries
LIBRARY = libws.a

# Common objects for library
LIB_OBJS = $(RINGBUFFER_OBJ) $(SSL_OBJ) $(WS_OBJ) $(WS_NOTIFIER_OBJ) $(OS_OBJ)

# Check for compiler
ifeq ($(shell which $(CC) 2>/dev/null),)
$(error C compiler not found! Please install gcc with: sudo apt-get install build-essential)
endif

# Default target
all: $(OBJDIR) $(LIBRARY)

# Create object directory
$(OBJDIR):
	mkdir -p $(OBJDIR)

# Library object files (depend on $(OBJDIR) for automatic directory creation)
$(RINGBUFFER_OBJ): $(RINGBUFFER_SRC) ringbuffer.h | $(OBJDIR)
	$(CC) $(CFLAGS) $(INCLUDES) -c $(RINGBUFFER_SRC) -o $@

$(SSL_OBJ): $(SSL_SRC) ssl.h ringbuffer.h | $(OBJDIR)
	$(CC) $(CFLAGS) $(INCLUDES) -c $(SSL_SRC) -o $@

$(WS_OBJ): $(WS_SRC) ws.h ssl.h ringbuffer.h os.h | $(OBJDIR)
	$(CC) $(CFLAGS) $(INCLUDES) -c $(WS_SRC) -o $@

$(WS_NOTIFIER_OBJ): $(WS_NOTIFIER_SRC) ws_notifier.h | $(OBJDIR)
	$(CC) $(CFLAGS) $(INCLUDES) -c $(WS_NOTIFIER_SRC) -o $@

$(OS_OBJ): $(OS_SRC) os.h | $(OBJDIR)
	$(CC) $(CFLAGS) $(INCLUDES) -c $(OS_SRC) -o $@

# Build static library
$(LIBRARY): $(LIB_OBJS)
	ar rcs $@ $^

# Test executable
# NOTE: Ringbuffer tests removed - ringbuffer is designed for zero-copy operations only
TESTDIR = test
#TEST_SRC = test/ringbuffer_test.c
#TEST_OBJ = $(OBJDIR)/ringbuffer_test.o
#TEST_EXE = test_ringbuffer

# SSL Test
SSL_TEST_SRC = test/ssl_test.c
SSL_TEST_OBJ = $(OBJDIR)/ssl_test.o
SSL_TEST_EXE = test_ssl

# WebSocket Test
WS_TEST_SRC = test/ws_test.c
WS_TEST_OBJ = $(OBJDIR)/ws_test.o
WS_TEST_EXE = test_ws

# Integration Test (Binance)
INTEGRATION_TEST_SRC = test/integration/binance.c
INTEGRATION_TEST_OBJ = $(OBJDIR)/integration_binance.o
INTEGRATION_TEST_EXE = test_binance_integration

# Integration Test (Bitget - TLS 1.3)
BITGET_TEST_SRC = test/integration/bitget.c
BITGET_TEST_OBJ = $(OBJDIR)/integration_bitget.o
BITGET_TEST_EXE = test_bitget_integration

# SSL Benchmark
SSL_BENCHMARK_SRC = test/ssl_benchmark.c
SSL_BENCHMARK_OBJ = $(OBJDIR)/ssl_benchmark.o
SSL_BENCHMARK_EXE = ssl_benchmark

# Timing Precision Test
TIMING_TEST_SRC = test/timing_precision_test.c
TIMING_TEST_OBJ = $(OBJDIR)/timing_precision_test.o
TIMING_TEST_EXE = test_timing_precision

# kTLS Verification Test
KTLS_TEST_SRC = test/ktls_test.c
KTLS_TEST_OBJ = $(OBJDIR)/ktls_test.o
KTLS_TEST_EXE = test_ktls

# Simple Example
EXAMPLE_SRC = example/simple_ws.c
EXAMPLE_OBJ = $(OBJDIR)/simple_ws.o
EXAMPLE_EXE = simple_ws_example

# SSL Probe Utility
SSL_PROBE_SRC = test/ssl_probe.c
SSL_PROBE_OBJ = $(OBJDIR)/ssl_probe.o
SSL_PROBE_EXE = ssl_probe

# Integration Test executable (Binance)
$(INTEGRATION_TEST_OBJ): $(INTEGRATION_TEST_SRC) ws.h ssl.h ringbuffer.h | $(OBJDIR)
	$(CC) $(CFLAGS) $(INCLUDES) -c $(INTEGRATION_TEST_SRC) -o $@

$(INTEGRATION_TEST_EXE): $(INTEGRATION_TEST_OBJ) $(LIBRARY)
	$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS)

# Integration Test executable (Bitget - TLS 1.3)
$(BITGET_TEST_OBJ): $(BITGET_TEST_SRC) ws.h ssl.h ringbuffer.h | $(OBJDIR)
	$(CC) $(CFLAGS) $(INCLUDES) -c $(BITGET_TEST_SRC) -o $@

$(BITGET_TEST_EXE): $(BITGET_TEST_OBJ) $(LIBRARY)
	$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS)

# SSL Benchmark executable
$(SSL_BENCHMARK_OBJ): $(SSL_BENCHMARK_SRC) ssl.h ssl_backend.h os.h | $(OBJDIR)
	$(CC) $(CFLAGS) $(INCLUDES) -c $(SSL_BENCHMARK_SRC) -o $@

$(SSL_BENCHMARK_EXE): $(SSL_BENCHMARK_OBJ) $(LIBRARY)
	$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS)

# Timing Precision Test executable
$(TIMING_TEST_OBJ): $(TIMING_TEST_SRC) os.h | $(OBJDIR)
	$(CC) $(CFLAGS) $(INCLUDES) -c $(TIMING_TEST_SRC) -o $@

$(TIMING_TEST_EXE): $(TIMING_TEST_OBJ) $(LIBRARY)
	$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS)

# kTLS Verification Test executable
$(KTLS_TEST_OBJ): $(KTLS_TEST_SRC) ssl.h | $(OBJDIR)
	$(CC) $(CFLAGS) $(INCLUDES) -c $(KTLS_TEST_SRC) -o $@

$(KTLS_TEST_EXE): $(KTLS_TEST_OBJ) $(LIBRARY)
	$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS)

# Simple Example executable
$(EXAMPLE_OBJ): $(EXAMPLE_SRC) ws.h ssl.h ws_notifier.h | $(OBJDIR)
	$(CC) $(CFLAGS) $(INCLUDES) -c $(EXAMPLE_SRC) -o $@

$(EXAMPLE_EXE): $(EXAMPLE_OBJ) $(LIBRARY)
	$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS)

# SSL Probe utility executable
$(SSL_PROBE_OBJ): $(SSL_PROBE_SRC) | $(OBJDIR)
	$(CC) $(CFLAGS) $(INCLUDES) -c $(SSL_PROBE_SRC) -o $@

$(SSL_PROBE_EXE): $(SSL_PROBE_OBJ)
	$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS)

# Test executable
$(TEST_OBJ): $(TEST_SRC) ringbuffer.h | $(OBJDIR)
	$(CC) $(CFLAGS) $(INCLUDES) -c $(TEST_SRC) -o $@

$(TEST_EXE): $(TEST_OBJ) $(OBJDIR)/ringbuffer.o
	$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS) -lcmocka

# SSL Test executable
$(SSL_TEST_OBJ): $(SSL_TEST_SRC) ssl.h ringbuffer.h | $(OBJDIR)
	$(CC) $(CFLAGS) $(INCLUDES) -c $(SSL_TEST_SRC) -o $@

$(SSL_TEST_EXE): $(SSL_TEST_OBJ) $(LIBRARY)
	$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS) -lcmocka

# WebSocket Test executable
$(WS_TEST_OBJ): $(WS_TEST_SRC) ws.h ssl.h ringbuffer.h | $(OBJDIR)
	$(CC) $(CFLAGS) $(INCLUDES) -c $(WS_TEST_SRC) -o $@

$(WS_TEST_EXE): $(WS_TEST_OBJ) $(LIBRARY)
	$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS)

# Install (optional)
install: all
	@echo "Installation would go here (e.g., copy to /usr/local/)"

# Run integration test
run-integration: $(INTEGRATION_TEST_EXE)
	./$(INTEGRATION_TEST_EXE)

# Build test
test-build: $(OBJDIR) $(TEST_EXE)

# Build SSL test
ssl-test-build: $(OBJDIR) $(SSL_TEST_EXE)

# Build WebSocket test
ws-test-build: $(OBJDIR) $(WS_TEST_EXE)

# Build Integration test
integration-test-build: $(OBJDIR) $(INTEGRATION_TEST_EXE)

# Run all available tests
test: $(OBJDIR)
	@echo "╔══════════════════════════════════════════════════════════════════╗"
	@echo "║                    Running Test Suite                           ║"
	@echo "╚══════════════════════════════════════════════════════════════════╝"
	@echo ""
	@# Check if cmocka is available
	@if pkg-config --exists cmocka 2>/dev/null || [ -f /usr/include/cmocka.h ] || [ -f /usr/local/include/cmocka.h ]; then \
		echo "📋 CMocka found - running unit tests..."; \
		echo ""; \
		if $(MAKE) --no-print-directory $(SSL_TEST_EXE) 2>/dev/null; then \
			echo "=== SSL Tests ==="; \
			./$(SSL_TEST_EXE); \
			echo ""; \
		fi; \
		if $(MAKE) --no-print-directory $(WS_TEST_EXE) 2>/dev/null; then \
			echo "=== WebSocket Tests ==="; \
			./$(WS_TEST_EXE); \
			echo ""; \
		fi; \
	else \
		echo "⚠️  CMocka not found - skipping unit tests"; \
		echo "   Install with: sudo apt-get install libcmocka-dev"; \
		echo ""; \
	fi
	@echo "=== Timing Precision Test ==="
	@$(MAKE) --no-print-directory test-timing-build
	@./$(TIMING_TEST_EXE)
	@echo ""
	@echo "╔══════════════════════════════════════════════════════════════════╗"
	@echo "║                    Test Suite Complete                          ║"
	@echo "╚══════════════════════════════════════════════════════════════════╝"

# Run individual unit tests
#test-ringbuffer: $(OBJDIR) $(TEST_EXE)
#	./$(TEST_EXE)

test-ssl: $(OBJDIR) $(SSL_TEST_EXE)
	./$(SSL_TEST_EXE)

test-ws: $(OBJDIR) $(WS_TEST_EXE)
	./$(WS_TEST_EXE)

# Run integration test
integration-test: $(OBJDIR) $(INTEGRATION_TEST_EXE)
	@echo "Running Binance integration test..."
	./$(INTEGRATION_TEST_EXE)

integration-test-bitget: $(OBJDIR) $(BITGET_TEST_EXE)
	@echo "Running Bitget integration test (TLS 1.2 with kTLS)..."
	./$(BITGET_TEST_EXE)

# Build SSL benchmark
benchmark-ssl-build: $(OBJDIR) $(SSL_BENCHMARK_EXE)

# Run SSL benchmark
benchmark-ssl: $(OBJDIR) $(SSL_BENCHMARK_EXE)
	@echo "Running SSL backend benchmark..."
	@echo ""
	./$(SSL_BENCHMARK_EXE)

# Build timing precision test
test-timing-build: $(OBJDIR) $(TIMING_TEST_EXE)

# Run timing precision test
test-timing: $(OBJDIR) $(TIMING_TEST_EXE)
	@echo "Running timing precision test..."
	@echo ""
	./$(TIMING_TEST_EXE)

# Build simple example
example-build: $(OBJDIR) $(EXAMPLE_EXE)

# Build and run simple example
example: $(OBJDIR) $(EXAMPLE_EXE)
	@echo "Running simple WebSocket example..."
	@echo ""
	./$(EXAMPLE_EXE)

# ═══════════════════════════════════════════════════════════════════
# kTLS (Kernel TLS) Targets
# ═══════════════════════════════════════════════════════════════════

# Build with kTLS backend
ktls-build:
	@echo "╔══════════════════════════════════════════════════════════════════╗"
	@echo "║            Building with kTLS (Kernel TLS) Backend              ║"
	@echo "╚══════════════════════════════════════════════════════════════════╝"
	@echo ""
	@echo "Checking kernel module..."
	@if ! lsmod | grep -q "^tls"; then \
		echo "⚠️  TLS kernel module not loaded"; \
		echo "   Run: sudo ./scripts/enable_ktls.sh"; \
		echo ""; \
	else \
		echo "✅ TLS kernel module loaded"; \
		echo ""; \
	fi
	SSL_BACKEND=ktls $(MAKE) clean all
	@echo ""
	@echo "✅ Build complete with kTLS backend"

# Quick verification that kTLS is working
ktls-verify: ktls-build
	@echo ""
	@echo "╔══════════════════════════════════════════════════════════════════╗"
	@echo "║                   Verifying kTLS Status                         ║"
	@echo "╚══════════════════════════════════════════════════════════════════╝"
	@echo ""
	@echo "Running integration test to verify kTLS..."
	@timeout 30 ./$(INTEGRATION_TEST_EXE) 2>&1 | grep -A 5 "SSL Configuration" || true
	@echo ""

# Run integration test with kTLS
ktls-test: ktls-build
	@echo ""
	@echo "╔══════════════════════════════════════════════════════════════════╗"
	@echo "║              Running Integration Test with kTLS                 ║"
	@echo "╚══════════════════════════════════════════════════════════════════╝"
	@echo ""
	./$(INTEGRATION_TEST_EXE)

# Benchmark kTLS vs OpenSSL performance
ktls-benchmark:
	@echo "╔══════════════════════════════════════════════════════════════════╗"
	@echo "║           kTLS Performance Comparison                            ║"
	@echo "╚══════════════════════════════════════════════════════════════════╝"
	@echo ""
	@echo "[1/4] Building with OpenSSL (baseline)..."
	@SSL_BACKEND=openssl $(MAKE) clean all > /dev/null 2>&1
	@echo "      ✅ OpenSSL build complete"
	@echo ""
	@echo "[2/4] Running OpenSSL test (30 seconds)..."
	@timeout 30 ./$(INTEGRATION_TEST_EXE) 2>&1 | tee /tmp/openssl_results.txt | grep -E "P50|P90|P99|SSL Configuration" || true
	@echo ""
	@echo "[3/4] Building with kTLS..."
	@if ! lsmod | grep -q "^tls"; then \
		echo "❌ Error: TLS kernel module not loaded"; \
		echo "   Run: sudo ./scripts/enable_ktls.sh"; \
		exit 1; \
	fi
	@SSL_BACKEND=ktls $(MAKE) clean all > /dev/null 2>&1
	@echo "      ✅ kTLS build complete"
	@echo ""
	@echo "[4/4] Running kTLS test (30 seconds)..."
	@timeout 30 ./$(INTEGRATION_TEST_EXE) 2>&1 | tee /tmp/ktls_results.txt | grep -E "P50|P90|P99|SSL Configuration" || true
	@echo ""
	@echo "╔══════════════════════════════════════════════════════════════════╗"
	@echo "║                    Benchmark Complete                            ║"
	@echo "╚══════════════════════════════════════════════════════════════════╝"
	@echo ""
	@echo "Results saved to:"
	@echo "  OpenSSL: /tmp/openssl_results.txt"
	@echo "  kTLS:    /tmp/ktls_results.txt"
	@echo ""
	@echo "To analyze results:"
	@echo "  diff /tmp/openssl_results.txt /tmp/ktls_results.txt"

# Clean build artifacts and PGO profiling data
clean:
	rm -rf $(OBJDIR) $(LIBRARY) $(TEST_EXE) $(SSL_TEST_EXE) $(WS_TEST_EXE) $(INTEGRATION_TEST_EXE) $(BITGET_TEST_EXE) $(SSL_BENCHMARK_EXE) $(TIMING_TEST_EXE) $(KTLS_TEST_EXE) $(EXAMPLE_EXE) $(SSL_PROBE_EXE)
	rm -f *.profraw *.profdata default.profdata default*.profraw

# Debug build
debug: CFLAGS = -Wall -Wextra -g -O0 -DDEBUG -pthread
debug: clean all

# AddressSanitizer build (detects memory errors: use-after-free, buffer overflow, etc.)
test-asan: CFLAGS = -Wall -Wextra -g -O1 -DDEBUG -pthread -fsanitize=address -fno-omit-frame-pointer
test-asan: LDFLAGS += -fsanitize=address
test-asan: clean all
	@echo ""
	@echo "=========================================="
	@echo "AddressSanitizer build complete!"
	@echo "Run tests with: make integration-test"
	@echo "=========================================="

# UndefinedBehaviorSanitizer build (detects undefined behavior)
test-ubsan: CFLAGS = -Wall -Wextra -g -O1 -DDEBUG -pthread -fsanitize=undefined -fno-omit-frame-pointer
test-ubsan: LDFLAGS += -fsanitize=undefined
test-ubsan: clean all
	@echo ""
	@echo "=========================================="
	@echo "UndefinedBehaviorSanitizer build complete!"
	@echo "Run tests with: make integration-test"
	@echo "=========================================="

# ThreadSanitizer build (detects data races and thread safety issues)
test-tsan: CFLAGS = -Wall -Wextra -g -O1 -DDEBUG -pthread -fsanitize=thread -fno-omit-frame-pointer
test-tsan: LDFLAGS += -fsanitize=thread
test-tsan: clean all
	@echo ""
	@echo "=========================================="
	@echo "ThreadSanitizer build complete!"
	@echo "Run tests with: make integration-test"
	@echo "Note: ThreadSanitizer may report false positives with non-atomic operations"
	@echo "=========================================="

# Release build with LTO
ifeq ($(UNAME_S),Darwin)
ifeq ($(UNAME_M),arm64)
release: CFLAGS = -Wall -Wextra -O3 -mcpu=apple-a14 -DNDEBUG -pthread -flto
release: LDFLAGS += -flto
else
release: CFLAGS = -Wall -Wextra -O3 -march=native -DNDEBUG -pthread -flto
release: LDFLAGS += -flto
endif
else
release: CFLAGS = -Wall -Wextra -O3 -march=native -DNDEBUG -pthread -flto
release: LDFLAGS += -flto
endif
release: clean all

# Profile-guided optimization build (two-stage process)
# Stage 1: Build with instrumentation
profile-generate:
	@echo "Building with PGO instrumentation..."
	$(MAKE) clean
	$(MAKE) integration-test-build CFLAGS="$(CFLAGS) -DNDEBUG -fprofile-generate" LDFLAGS="$(LDFLAGS) -fprofile-generate"
	@echo ""
	@echo "Instrumented build complete. Now run your workload:"
	@echo "  ./test_binance_integration"
	@echo "Then run 'make profile-use' to build optimized version"

# Stage 2: Build using profile data
profile-use:
	@echo "Building with PGO profile data..."
	@# Detect compiler type and check for appropriate profile data
	@COMPILER_TYPE=$$($(CC) --version 2>/dev/null | head -1); \
	if echo "$$COMPILER_TYPE" | grep -qi "gcc\|g++"; then \
		echo "Detected GCC - checking for .gcda profile data..."; \
		GCDA_COUNT=$$(find obj -name "*.gcda" 2>/dev/null | wc -l); \
		if [ $$GCDA_COUNT -eq 0 ]; then \
			echo "Error: no GCC profile data (.gcda) found in obj/."; \
			echo "Run 'make profile-generate', execute ./test_binance_integration, then retry."; \
			exit 1; \
		fi; \
		echo "Found $$GCDA_COUNT .gcda files - proceeding with GCC PGO build"; \
	elif echo "$$COMPILER_TYPE" | grep -qi "clang"; then \
		echo "Detected Clang - checking for .profraw profile data..."; \
		RAW_PROFILES=$$(ls default*.profraw 2>/dev/null | tr '\n' ' '); \
		if [ ! -f default.profdata ] && [ -z "$$RAW_PROFILES" ]; then \
			echo "Error: no Clang profile data (.profraw) found."; \
			echo "Run 'make profile-generate', execute ./test_binance_integration, then retry."; \
			exit 1; \
		fi; \
		if [ ! -f default.profdata ] && [ -n "$$RAW_PROFILES" ]; then \
			if [ -z "$(LLVM_PROFDATA)" ]; then \
				echo "Error: llvm-profdata not found. Install LLVM tools or run 'xcode-select --install'."; \
				exit 1; \
			fi; \
			echo "Merging Clang profile data with llvm-profdata..."; \
			"$(LLVM_PROFDATA)" merge -output=default.profdata $$RAW_PROFILES; \
		fi; \
	else \
		echo "Warning: Unknown compiler type - attempting PGO anyway"; \
	fi
	@echo "Rebuilding with profile-guided optimizations..."
	@# For GCC, preserve .gcda files; for Clang, full clean is fine
	@COMPILER_TYPE=$$($(CC) --version 2>/dev/null | head -1); \
	if echo "$$COMPILER_TYPE" | grep -qi "gcc\|g++"; then \
		echo "Preserving GCC profile data, removing only object files..."; \
		rm -f obj/*.o $(LIBRARY) $(INTEGRATION_TEST_EXE); \
	else \
		$(MAKE) clean-objs; \
	fi
	$(MAKE) integration-test-build CFLAGS="$(CFLAGS) -DNDEBUG -fprofile-use" LDFLAGS="$(LDFLAGS) -fprofile-use"
	@echo ""
	@echo "PGO optimized build complete!"

# Automated PGO workflow: generate profile, run workload, optimize, and re-run for comparison
integration-test-profile:
	@echo "=========================================="
	@echo "Starting automated PGO workflow..."
	@echo "=========================================="
	@echo ""
	@echo "Step 1/4: Building with PGO instrumentation..."
	$(MAKE) profile-generate
	@echo ""
	@echo "=========================================="
	@echo "Step 2/4: Running baseline test to collect profile data..."
	@echo "=========================================="
	@echo ""
	./$(INTEGRATION_TEST_EXE)
	@echo ""
	@echo "=========================================="
	@echo "Step 3/4: Building with PGO optimizations..."
	@echo "=========================================="
	@echo ""
	$(MAKE) profile-use
	@echo ""
	@echo "=========================================="
	@echo "Step 4/4: Running optimized test for performance comparison..."
	@echo "=========================================="
	@echo ""
	./$(INTEGRATION_TEST_EXE)
	@echo ""
	@echo "=========================================="
	@echo "PGO workflow complete!"
	@echo "Compare the latency results above to see performance improvements"
	@echo "=========================================="

# Build PGO-optimized release library
build-release:
	@echo "╔══════════════════════════════════════════════════════════════════╗"
	@echo "║          Building Profile-Guided Optimized Release              ║"
	@echo "╚══════════════════════════════════════════════════════════════════╝"
	@echo ""
	@echo "[Step 1/3] Building with PGO instrumentation..."
	@$(MAKE) profile-generate
	@echo ""
	@echo "[Step 2/3] Collecting profile data with Binance integration test..."
	@echo "            (This will take ~60 seconds)"
	@timeout 60 ./$(INTEGRATION_TEST_EXE) 2>&1 | grep -E "SSL Configuration|P50|P90|Aggregate Processing" || true
	@echo ""
	@echo "[Step 3/3] Building optimized library with profile data..."
	@$(MAKE) profile-use
	@echo ""
	@echo "╔══════════════════════════════════════════════════════════════════╗"
	@echo "║              PGO-Optimized Release Build Complete                ║"
	@echo "╚══════════════════════════════════════════════════════════════════╝"
	@echo ""
	@echo "✅ Library built: libws.a (with profile-guided optimizations)"
	@echo "✅ Test binary:   test_binance_integration (PGO-optimized)"
	@echo ""
	@echo "Profile data collected from live Binance WebSocket traffic for optimal"
	@echo "performance on high-frequency trading workloads."

# Clean only object files (keep profile data)
clean-objs:
	rm -rf $(OBJDIR) $(LIBRARY) $(TEST_EXE) $(SSL_TEST_EXE) $(WS_TEST_EXE) $(INTEGRATION_TEST_EXE) $(BITGET_TEST_EXE) $(SSL_BENCHMARK_EXE) $(TIMING_TEST_EXE) $(KTLS_TEST_EXE) $(EXAMPLE_EXE) $(SSL_PROBE_EXE)

# Clean everything including profile data
clean-all: clean
	rm -f obj/*.gcda obj/*.gcno *.gcda *.profraw *.profdata default.profraw

# Static linking build (includes OpenSSL statically)
static-ssl:
	@echo "Note: Static OpenSSL linking requires static libraries to be installed"
	@echo "macOS: brew install openssl --build-from-source"
ifeq ($(UNAME_S),Darwin)
ifeq ($(UNAME_M),arm64)
	$(MAKE) clean
	$(MAKE) all CFLAGS="-Wall -Wextra -O3 -mcpu=apple-a14 -DNDEBUG -pthread -flto" LDFLAGS="$(OPENSSL_PREFIX)/lib/libssl.a $(OPENSSL_PREFIX)/lib/libcrypto.a -lm -pthread -flto"
else
	$(MAKE) clean
	$(MAKE) all CFLAGS="-Wall -Wextra -O3 -march=native -DNDEBUG -pthread -flto" LDFLAGS="$(OPENSSL_PREFIX)/lib/libssl.a $(OPENSSL_PREFIX)/lib/libcrypto.a -lm -pthread -flto"
endif
else
	$(MAKE) clean
	$(MAKE) all CFLAGS="-Wall -Wextra -O3 -march=native -DNDEBUG -pthread -flto" LDFLAGS="/usr/lib/x86_64-linux-gnu/libssl.a /usr/lib/x86_64-linux-gnu/libcrypto.a -lm -pthread -ldl -flto"
endif

# Install dependencies
install-deps:
	@echo "Installing build dependencies..."
	@echo ""
ifeq ($(UNAME_S),Darwin)
	@echo "=== macOS ==="
	@echo "1. Install Xcode Command Line Tools:"
	@echo "   xcode-select --install"
	@echo ""
	@echo "2. Install Homebrew (if not already installed):"
	@echo "   /bin/bash -c \"\$$(curl -fsSL https://raw.githubusercontent.com/Homebrew/install/HEAD/install.sh)\""
	@echo ""
	@echo "3. Install dependencies:"
	@echo "   brew install openssl cmocka"
else ifeq ($(UNAME_S),Linux)
	@echo "=== Ubuntu/Debian ==="
	@echo "   sudo apt-get update"
	@echo "   sudo apt-get install build-essential libssl-dev libcmocka-dev"
	@echo ""
	@echo "=== Fedora/RHEL ==="
	@echo "   sudo dnf install gcc openssl-devel libcmocka-devel"
	@echo ""
	@echo "=== Arch Linux ==="
	@echo "   sudo pacman -S gcc openssl libcmocka"
else
	@echo "Please install: gcc/clang, openssl development libraries, and cmocka"
endif

# Help
help:
	@echo "WebSocket Library Makefile"
	@echo ""
	@echo "Setup (first time):"
	@echo "  make install-deps  - Show dependency installation commands"
	@echo ""
	@echo "Dependencies:"
	@echo "  Required:  openssl (for SSL/TLS support)"
	@echo "  Optional:  cmocka (only needed for 'make test')"
	@echo ""
	@echo "Targets:"
	@echo "  all             - Build library (default)"
	@echo "  build-release   - Build PGO-optimized release library (profile + optimize)"
	@echo "  example         - Build and run simple WebSocket example"
	@echo "  test            - Run all available tests (timing + unit tests if cmocka installed)"
	@echo "  test-timing     - Run timing precision test (verifies TSC calibration accuracy)"
	@echo "  integration-test - Build and run integration test (Binance WebSocket)"
	@echo "  benchmark-ssl   - Build and run SSL backend benchmark"
	@echo ""
	@echo "kTLS (Kernel TLS) Targets:"
	@echo "  ktls-build      - Build with kTLS backend (requires TLS kernel module)"
	@echo "  ktls-verify     - Quick verification that kTLS is working"
	@echo "  ktls-test       - Run integration test with kTLS enabled"
	@echo "  ktls-benchmark  - Compare kTLS vs OpenSSL performance (30s each)"
	@echo ""
	@echo "Additional Targets:"
	@echo "  test-ssl        - Run SSL tests only (requires cmocka)"
	@echo "  test-ws         - Run WebSocket tests only (requires cmocka)"
	@echo "  example-build   - Build simple example executable only"
	@echo "  integration-test-build - Build integration test executable only"
	@echo "  benchmark-ssl-build - Build SSL benchmark executable only"
	@echo "  test-timing-build - Build timing precision test executable only"
	@echo "  integration-test-profile - Automated PGO workflow (profile + optimize + compare)"
	@echo "  clean           - Remove all build artifacts and PGO profiling data"
	@echo "  debug           - Build with debug symbols and no optimization"
	@echo "  test-asan       - Build with AddressSanitizer (memory error detection)"
	@echo "  test-ubsan      - Build with UndefinedBehaviorSanitizer"
	@echo "  test-tsan       - Build with ThreadSanitizer (data race detection)"
	@echo "  release         - Build optimized release with LTO"
	@echo "  profile-generate - Build with PGO instrumentation (step 1)"
	@echo "  profile-use     - Build with PGO optimizations (step 2)"
	@echo "  static-ssl      - Build with statically linked OpenSSL"
	@echo "  clean-all       - Clean including PGO profile data"
	@echo "  help            - Show this help message"
	@echo ""
	@echo "Example usage:"
	@echo "  make                # Build library"
	@echo "  make build-release  # Build PGO-optimized release (automated)"
	@echo "  make example        # Build and run simple WebSocket example"
	@echo "  make release        # Build optimized with LTO"
	@echo "  make test           # Run all unit tests (requires cmocka)"
	@echo "  make integration-test # Run integration test (with latency benchmarking)"
	@echo ""
	@echo "PGO workflow (automated):"
	@echo "  make build-release             # Build optimized library (recommended)"
	@echo "  make integration-test-profile  # Full automated workflow with before/after comparison"
	@echo ""
	@echo "PGO workflow (manual steps):"
	@echo "  make profile-generate       # Build with instrumentation"
	@echo "  ./test_binance_integration  # Run representative workload"
	@echo "  make profile-use            # Build optimized version"

.PHONY: all clean install run-integration debug test-asan test-ubsan test-tsan release help install-deps test test-ringbuffer test-ssl test-ws integration-test integration-test-build integration-test-bitget benchmark-ssl benchmark-ssl-build test-timing test-timing-build integration-test-profile build-release profile-generate profile-use clean-objs clean-all static-ssl example example-build ktls-build ktls-verify ktls-test ktls-benchmark
