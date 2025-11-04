#!/bin/bash
#
# Enable Kernel TLS (kTLS) Support
#
# This script loads the TLS kernel module and makes it persistent across reboots.
# kTLS offloads TLS encryption/decryption from userspace to the kernel for better performance.
#
# Requirements:
#   - Linux kernel 4.17+ (preferably 5.2+ for TLS 1.3)
#   - CONFIG_TLS enabled in kernel
#   - Root privileges

set -e  # Exit on error

# Colors for output
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
BLUE='\033[0;34m'
NC='\033[0m' # No Color

echo -e "${BLUE}╔══════════════════════════════════════════════════════════════════╗${NC}"
echo -e "${BLUE}║              Enable Kernel TLS (kTLS) Support                   ║${NC}"
echo -e "${BLUE}╚══════════════════════════════════════════════════════════════════╝${NC}"
echo ""

# Check if running as root
if [ "$EUID" -ne 0 ]; then
    echo -e "${RED}✗ Error: This script must be run as root${NC}"
    echo "  Please run: sudo $0"
    exit 1
fi

# Check kernel version
echo -e "${BLUE}[1/4] Checking kernel version...${NC}"
KERNEL_VERSION=$(uname -r)
KERNEL_MAJOR=$(echo "$KERNEL_VERSION" | cut -d. -f1)
KERNEL_MINOR=$(echo "$KERNEL_VERSION" | cut -d. -f2)

echo "      Kernel: $KERNEL_VERSION"

if [ "$KERNEL_MAJOR" -lt 4 ] || ([ "$KERNEL_MAJOR" -eq 4 ] && [ "$KERNEL_MINOR" -lt 17 ]); then
    echo -e "${RED}✗ Error: Kernel 4.17+ required for kTLS${NC}"
    echo "  Your kernel: $KERNEL_VERSION"
    exit 1
fi

if [ "$KERNEL_MAJOR" -ge 5 ] && [ "$KERNEL_MINOR" -ge 2 ]; then
    echo -e "${GREEN}✓ Kernel version OK (TLS 1.3 supported)${NC}"
elif [ "$KERNEL_MAJOR" -ge 5 ]; then
    echo -e "${GREEN}✓ Kernel version OK (TLS 1.2 supported)${NC}"
else
    echo -e "${YELLOW}⚠ Kernel version OK (TLS 1.2 only, consider upgrade to 5.2+ for TLS 1.3)${NC}"
fi

# Check if CONFIG_TLS is enabled
echo ""
echo -e "${BLUE}[2/4] Checking kernel configuration...${NC}"
CONFIG_FILE="/boot/config-$(uname -r)"

if [ -f "$CONFIG_FILE" ]; then
    if grep -q "CONFIG_TLS=y\|CONFIG_TLS=m" "$CONFIG_FILE"; then
        echo -e "${GREEN}✓ CONFIG_TLS enabled in kernel${NC}"
    else
        echo -e "${RED}✗ Error: CONFIG_TLS not enabled in kernel${NC}"
        echo "  Kernel must be compiled with CONFIG_TLS=y or CONFIG_TLS=m"
        exit 1
    fi
else
    echo -e "${YELLOW}⚠ Cannot find kernel config file (will try to load module anyway)${NC}"
fi

# Check if module exists
if ! modinfo tls >/dev/null 2>&1; then
    echo -e "${RED}✗ Error: TLS kernel module not found${NC}"
    echo "  Module 'tls' is not available on this system"
    exit 1
fi

# Load TLS kernel module
echo ""
echo -e "${BLUE}[3/4] Loading TLS kernel module...${NC}"

if lsmod | grep -q "^tls"; then
    echo -e "${GREEN}✓ TLS module already loaded${NC}"
else
    if modprobe tls; then
        echo -e "${GREEN}✓ TLS module loaded successfully${NC}"
    else
        echo -e "${RED}✗ Error: Failed to load TLS module${NC}"
        echo "  Try: dmesg | tail -20  (to see kernel errors)"
        exit 1
    fi
fi

# Verify module is loaded
if ! lsmod | grep -q "^tls"; then
    echo -e "${RED}✗ Error: TLS module not loaded after modprobe${NC}"
    exit 1
fi

# Make persistent across reboots
echo ""
echo -e "${BLUE}[4/4] Making TLS module persistent...${NC}"

MODULES_FILE="/etc/modules"
MODULES_LOAD_DIR="/etc/modules-load.d"

# Try /etc/modules-load.d first (systemd way)
if [ -d "$MODULES_LOAD_DIR" ]; then
    KTLS_CONF="$MODULES_LOAD_DIR/ktls.conf"
    if [ -f "$KTLS_CONF" ] && grep -q "^tls" "$KTLS_CONF"; then
        echo -e "${GREEN}✓ TLS module already persistent ($KTLS_CONF)${NC}"
    else
        echo "# Load TLS module for kTLS support" > "$KTLS_CONF"
        echo "tls" >> "$KTLS_CONF"
        echo -e "${GREEN}✓ TLS module persistent: $KTLS_CONF${NC}"
    fi
# Fallback to /etc/modules (older systems)
elif [ -f "$MODULES_FILE" ]; then
    if grep -q "^tls" "$MODULES_FILE"; then
        echo -e "${GREEN}✓ TLS module already persistent ($MODULES_FILE)${NC}"
    else
        echo "tls" >> "$MODULES_FILE"
        echo -e "${GREEN}✓ TLS module persistent: $MODULES_FILE${NC}"
    fi
else
    echo -e "${YELLOW}⚠ Warning: Cannot make module persistent (no /etc/modules or /etc/modules-load.d)${NC}"
    echo "  Module will need to be loaded manually after reboot: sudo modprobe tls"
fi

# Success summary
echo ""
echo -e "${GREEN}╔══════════════════════════════════════════════════════════════════╗${NC}"
echo -e "${GREEN}║                    kTLS Enabled Successfully                     ║${NC}"
echo -e "${GREEN}╚══════════════════════════════════════════════════════════════════╝${NC}"
echo ""
echo "Next steps:"
echo "  1. Build with kTLS: SSL_BACKEND=ktls make clean all"
echo "  2. Run test: ./test_binance_integration"
echo "  3. Look for: ✅ kTLS: ENABLED (kernel offload active)"
echo ""
echo "To verify module is loaded:"
echo "  lsmod | grep tls"
echo ""
echo "To disable kTLS (if needed):"
echo "  sudo rmmod tls"
echo ""
