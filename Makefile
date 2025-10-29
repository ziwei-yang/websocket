# High Performance WebSocket Library Makefile

# Detect operating system
UNAME_S := $(shell uname -s)
UNAME_M := $(shell uname -m)

# Try to find a C compiler
CC := $(shell which gcc 2>/dev/null || which clang 2>/dev/null || which cc 2>/dev/null || echo "gcc")

# Base flags
CFLAGS = -Wall -Wextra -O3 -pthread
LDFLAGS = -lssl -lcrypto -lm
INCLUDES = -I.

# Platform-specific configuration
ifeq ($(UNAME_S),Darwin)
    # macOS specific settings
    ifeq ($(UNAME_M),arm64)
        # macOS ARM (Apple Silicon) - use -mcpu instead of -march
        CFLAGS += -mcpu=apple-m1
        # Try to find OpenSSL via Homebrew
        OPENSSL_PREFIX := $(shell brew --prefix openssl 2>/dev/null)
        ifneq ($(OPENSSL_PREFIX),)
            INCLUDES += -I$(OPENSSL_PREFIX)/include
            LDFLAGS += -L$(OPENSSL_PREFIX)/lib
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
        # Try to find OpenSSL via Homebrew
        OPENSSL_PREFIX := $(shell brew --prefix openssl 2>/dev/null)
        ifneq ($(OPENSSL_PREFIX),)
            INCLUDES += -I$(OPENSSL_PREFIX)/include
            LDFLAGS += -L$(OPENSSL_PREFIX)/lib
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

# Object files
RINGBUFFER_OBJ = $(OBJDIR)/ringbuffer.o
SSL_OBJ = $(OBJDIR)/ssl.o
WS_OBJ = $(OBJDIR)/ws.o

# Libraries
LIBRARY = libws.a

# Common objects for library
LIB_OBJS = $(RINGBUFFER_OBJ) $(SSL_OBJ) $(WS_OBJ)

# Check for compiler
ifeq ($(shell which $(CC) 2>/dev/null),)
$(error C compiler not found! Please install gcc with: sudo apt-get install build-essential)
endif

# Default target
all: $(OBJDIR) $(LIBRARY)

# Create object directory
$(OBJDIR):
	mkdir -p $(OBJDIR)

# Library object files
$(RINGBUFFER_OBJ): $(RINGBUFFER_SRC) ringbuffer.h
	$(CC) $(CFLAGS) $(INCLUDES) -c $(RINGBUFFER_SRC) -o $@

$(SSL_OBJ): $(SSL_SRC) ssl.h ringbuffer.h
	$(CC) $(CFLAGS) $(INCLUDES) -c $(SSL_SRC) -o $@

$(WS_OBJ): $(WS_SRC) ws.h ssl.h ringbuffer.h
	$(CC) $(CFLAGS) $(INCLUDES) -c $(WS_SRC) -o $@

# Build static library
$(LIBRARY): $(LIB_OBJS)
	ar rcs $@ $^

# Test executable
TESTDIR = test
TEST_SRC = test/ringbuffer_test.c
TEST_OBJ = $(OBJDIR)/ringbuffer_test.o
TEST_EXE = test_ringbuffer

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

# Integration Test executable
$(INTEGRATION_TEST_OBJ): $(INTEGRATION_TEST_SRC) ws.h ssl.h ringbuffer.h
	$(CC) $(CFLAGS) $(INCLUDES) -c $(INTEGRATION_TEST_SRC) -o $@

$(INTEGRATION_TEST_EXE): $(INTEGRATION_TEST_OBJ) $(LIBRARY)
	$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS)

# Test executable
$(TEST_OBJ): $(TEST_SRC) ringbuffer.h
	$(CC) $(CFLAGS) $(INCLUDES) -c $(TEST_SRC) -o $@

$(TEST_EXE): $(TEST_OBJ) $(OBJDIR)/ringbuffer.o
	$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS) -lcmocka

# SSL Test executable
$(SSL_TEST_OBJ): $(SSL_TEST_SRC) ssl.h ringbuffer.h
	$(CC) $(CFLAGS) $(INCLUDES) -c $(SSL_TEST_SRC) -o $@

$(SSL_TEST_EXE): $(SSL_TEST_OBJ) $(LIBRARY)
	$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS) -lcmocka

# WebSocket Test executable
$(WS_TEST_OBJ): $(WS_TEST_SRC) ws.h ssl.h ringbuffer.h
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
test: $(OBJDIR) $(TEST_EXE) $(SSL_TEST_EXE) $(WS_TEST_EXE)
	@echo "Running all unit tests..."
	@echo ""
	@echo "=== Ringbuffer Tests ==="
	./$(TEST_EXE)
	@echo ""
	@echo "=== SSL Tests ==="
	./$(SSL_TEST_EXE)
	@echo ""
	@echo "=== WebSocket Tests ==="
	./$(WS_TEST_EXE)

# Run individual unit tests
test-ringbuffer: $(OBJDIR) $(TEST_EXE)
	./$(TEST_EXE)

test-ssl: $(OBJDIR) $(SSL_TEST_EXE)
	./$(SSL_TEST_EXE)

test-ws: $(OBJDIR) $(WS_TEST_EXE)
	./$(WS_TEST_EXE)

# Run integration test
integration-test: $(OBJDIR) $(INTEGRATION_TEST_EXE)
	@echo "Running integration test..."
	./$(INTEGRATION_TEST_EXE)

# Clean
clean:
	rm -rf $(OBJDIR) $(LIBRARY) $(TEST_EXE) $(SSL_TEST_EXE) $(WS_TEST_EXE) $(INTEGRATION_TEST_EXE)

# Debug build
debug: CFLAGS = -Wall -Wextra -g -O0 -DDEBUG -pthread
debug: clean all

# Release build
ifeq ($(UNAME_S),Darwin)
ifeq ($(UNAME_M),arm64)
release: CFLAGS = -Wall -Wextra -O3 -mcpu=apple-m1 -DNDEBUG -pthread
else
release: CFLAGS = -Wall -Wextra -O3 -march=native -DNDEBUG -pthread
endif
else
release: CFLAGS = -Wall -Wextra -O3 -march=native -DNDEBUG -pthread
endif
release: clean all

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
	@echo "  test-ringbuffer - Run ringbuffer tests only"
	@echo "  test-ssl        - Run SSL tests only"
	@echo "  test-ws         - Run WebSocket tests only"
	@echo "  integration-test-build - Build integration test executable only"
	@echo "  clean           - Remove all build artifacts"
	@echo "  debug           - Build with debug symbols and no optimization"
	@echo "  release         - Build optimized release version"
	@echo "  help            - Show this help message"
	@echo ""
	@echo "Example usage:"
	@echo "  make                # Build library"
	@echo "  make test           # Run all unit tests (requires cmocka)"
	@echo "  make integration-test # Run integration test (with latency benchmarking)"
	@echo "  make clean          # Clean build artifacts"

.PHONY: all clean install run-integration debug release help install-deps test test-ringbuffer test-ssl test-ws integration-test integration-test-build
