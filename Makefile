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

# SSL Backend Selection (default: libressl for optimal HFT performance)
SSL_BACKEND ?= libressl

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
    # Kernel TLS (Linux only) - uses OpenSSL for handshake, kernel for encryption
    CFLAGS += -DSSL_BACKEND_KTLS
    # kTLS requires OpenSSL 1.1.1+ or 3.0+ with kTLS support compiled in
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
    CFLAGS += -march=native
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

# Integration Test
INTEGRATION_TEST_SRC = test/integration/binance.c
INTEGRATION_TEST_OBJ = $(OBJDIR)/integration_binance.o
INTEGRATION_TEST_EXE = test_binance_integration

# SSL Benchmark
SSL_BENCHMARK_SRC = test/ssl_benchmark.c
SSL_BENCHMARK_OBJ = $(OBJDIR)/ssl_benchmark.o
SSL_BENCHMARK_EXE = ssl_benchmark

# Integration Test executable
$(INTEGRATION_TEST_OBJ): $(INTEGRATION_TEST_SRC) ws.h ssl.h ringbuffer.h | $(OBJDIR)
	$(CC) $(CFLAGS) $(INCLUDES) -c $(INTEGRATION_TEST_SRC) -o $@

$(INTEGRATION_TEST_EXE): $(INTEGRATION_TEST_OBJ) $(LIBRARY)
	$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS)

# SSL Benchmark executable
$(SSL_BENCHMARK_OBJ): $(SSL_BENCHMARK_SRC) ssl.h ssl_backend.h os.h | $(OBJDIR)
	$(CC) $(CFLAGS) $(INCLUDES) -c $(SSL_BENCHMARK_SRC) -o $@

$(SSL_BENCHMARK_EXE): $(SSL_BENCHMARK_OBJ) $(LIBRARY)
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

# Run all unit tests
test: $(OBJDIR) $(SSL_TEST_EXE) $(WS_TEST_EXE)
	@echo "Running all unit tests..."
	@echo ""
	@echo "=== SSL Tests ==="
	./$(SSL_TEST_EXE)
	@echo ""
	@echo "=== WebSocket Tests ==="
	./$(WS_TEST_EXE)

# Run individual unit tests
#test-ringbuffer: $(OBJDIR) $(TEST_EXE)
#	./$(TEST_EXE)

test-ssl: $(OBJDIR) $(SSL_TEST_EXE)
	./$(SSL_TEST_EXE)

test-ws: $(OBJDIR) $(WS_TEST_EXE)
	./$(WS_TEST_EXE)

# Run integration test
integration-test: $(OBJDIR) $(INTEGRATION_TEST_EXE)
	@echo "Running integration test..."
	./$(INTEGRATION_TEST_EXE)

# Build SSL benchmark
benchmark-ssl-build: $(OBJDIR) $(SSL_BENCHMARK_EXE)

# Run SSL benchmark
benchmark-ssl: $(OBJDIR) $(SSL_BENCHMARK_EXE)
	@echo "Running SSL backend benchmark..."
	@echo ""
	./$(SSL_BENCHMARK_EXE)

# Clean build artifacts and PGO profiling data
clean:
	rm -rf $(OBJDIR) $(LIBRARY) $(TEST_EXE) $(SSL_TEST_EXE) $(WS_TEST_EXE) $(INTEGRATION_TEST_EXE) $(SSL_BENCHMARK_EXE)
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
	@RAW_PROFILES=$$(ls default*.profraw 2>/dev/null | tr '\n' ' '); \
	if [ ! -f default.profdata ] && [ -z "$$RAW_PROFILES" ]; then \
		echo "Error: no profile data found. Run 'make profile-generate', execute ./test_binance_integration, then retry."; \
		exit 1; \
	fi; \
	if [ ! -f default.profdata ] && [ -n "$$RAW_PROFILES" ]; then \
		if [ -z "$(LLVM_PROFDATA)" ]; then \
			echo "Error: llvm-profdata not found. Install LLVM tools or run 'xcode-select --install'."; \
			exit 1; \
		fi; \
		echo "Merging profile data with llvm-profdata..."; \
		"$(LLVM_PROFDATA)" merge -output=default.profdata $$RAW_PROFILES; \
	fi
	$(MAKE) clean-objs
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

# Clean only object files (keep profile data)
clean-objs:
	rm -rf $(OBJDIR) $(LIBRARY) $(TEST_EXE) $(SSL_TEST_EXE) $(WS_TEST_EXE) $(INTEGRATION_TEST_EXE) $(SSL_BENCHMARK_EXE)

# Clean everything including profile data
clean-all: clean
	rm -f *.gcda *.profraw *.profdata default.profraw

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
	@echo "  test            - Build and run all unit tests (requires cmocka)"
	@echo "  integration-test - Build and run integration test (Binance WebSocket)"
	@echo "  benchmark-ssl   - Build and run SSL backend benchmark"
	@echo "  test-ringbuffer - Run ringbuffer tests only"
	@echo "  test-ssl        - Run SSL tests only"
	@echo "  test-ws         - Run WebSocket tests only"
	@echo "  integration-test-build - Build integration test executable only"
	@echo "  benchmark-ssl-build - Build SSL benchmark executable only"
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
	@echo "  make release        # Build optimized with LTO"
	@echo "  make test           # Run all unit tests (requires cmocka)"
	@echo "  make integration-test # Run integration test (with latency benchmarking)"
	@echo ""
	@echo "PGO workflow (automated):"
	@echo "  make integration-test-profile  # Full automated workflow with before/after comparison"
	@echo ""
	@echo "PGO workflow (manual steps):"
	@echo "  make profile-generate       # Build with instrumentation"
	@echo "  ./test_binance_integration  # Run representative workload"
	@echo "  make profile-use            # Build optimized version"

.PHONY: all clean install run-integration debug test-asan test-ubsan test-tsan release help install-deps test test-ringbuffer test-ssl test-ws integration-test integration-test-build benchmark-ssl benchmark-ssl-build integration-test-profile profile-generate profile-use clean-objs clean-all static-ssl
