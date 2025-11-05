#!/bin/bash
# PTP Hardware Clock Synchronization Setup for HFT Applications
# This script synchronizes the NIC's hardware clock (PHC) with the system clock
# Required for accurate hardware timestamp measurements

set -e

# Colors for output
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
NC='\033[0m' # No Color

echo "=================================================="
echo "PTP Hardware Clock Synchronization Setup"
echo "=================================================="
echo ""

# Check if running as root
if [ "$EUID" -ne 0 ]; then
    echo -e "${RED}Error: This script must be run as root${NC}"
    echo "Usage: sudo $0 [interface]"
    exit 1
fi

# Get network interface
INTERFACE=${1:-$(ip route | grep default | awk '{print $5}' | head -1)}

if [ -z "$INTERFACE" ]; then
    echo -e "${RED}Error: No network interface specified and could not detect default${NC}"
    echo "Usage: sudo $0 <interface>"
    echo "Example: sudo $0 eth0"
    exit 1
fi

echo -e "${GREEN}Target interface: $INTERFACE${NC}"
echo ""

# Check if interface supports hardware timestamping
echo "Checking hardware timestamping support..."
if ! ethtool -T "$INTERFACE" &>/dev/null; then
    echo -e "${RED}Error: Interface $INTERFACE does not support hardware timestamping${NC}"
    exit 1
fi

# Display capabilities
echo ""
echo "Hardware timestamping capabilities:"
ethtool -T "$INTERFACE" | grep -E "(Capabilities|PTP Hardware Clock)"

# Check for PHC device
PHC_DEVICE=$(ethtool -T "$INTERFACE" | grep "PTP Hardware Clock" | awk '{print $4}')
if [ -z "$PHC_DEVICE" ]; then
    echo -e "${RED}Error: No PTP Hardware Clock found for $INTERFACE${NC}"
    exit 1
fi

echo -e "${GREEN}Found PHC device: $PHC_DEVICE${NC}"
echo ""

# Install required tools
echo "Checking for required packages..."
if ! command -v phc2sys &> /dev/null; then
    echo "Installing linuxptp package..."
    if command -v apt-get &> /dev/null; then
        apt-get update && apt-get install -y linuxptp
    elif command -v yum &> /dev/null; then
        yum install -y linuxptp
    elif command -v dnf &> /dev/null; then
        dnf install -y linuxptp
    else
        echo -e "${RED}Error: Could not install linuxptp. Please install manually.${NC}"
        exit 1
    fi
fi

echo -e "${GREEN}Required packages installed${NC}"
echo ""

# Stop existing phc2sys processes
echo "Stopping any existing phc2sys processes..."
pkill -9 phc2sys 2>/dev/null || true
sleep 1

# Start phc2sys to sync PHC with system clock
echo "Starting PHC synchronization daemon..."
echo "Command: phc2sys -s CLOCK_REALTIME -c $PHC_DEVICE -O 0 -m -S 1.0"
echo ""

# Run phc2sys in background
nohup phc2sys -s CLOCK_REALTIME -c "$PHC_DEVICE" -O 0 -m -S 1.0 > /var/log/phc2sys.log 2>&1 &
PHC2SYS_PID=$!

sleep 2

# Check if process is running
if ! ps -p $PHC2SYS_PID > /dev/null; then
    echo -e "${RED}Error: phc2sys failed to start. Check /var/log/phc2sys.log${NC}"
    cat /var/log/phc2sys.log
    exit 1
fi

echo -e "${GREEN}PHC synchronization started successfully (PID: $PHC2SYS_PID)${NC}"
echo ""

# Monitor synchronization for a few seconds
echo "Monitoring synchronization (5 seconds)..."
tail -f /var/log/phc2sys.log &
TAIL_PID=$!
sleep 5
kill $TAIL_PID 2>/dev/null || true

echo ""
echo -e "${GREEN}=================================================="
echo "PHC Synchronization Setup Complete!"
echo "==================================================${NC}"
echo ""
echo "Configuration:"
echo "  Interface:    $INTERFACE"
echo "  PHC Device:   $PHC_DEVICE"
echo "  Process PID:  $PHC2SYS_PID"
echo "  Log file:     /var/log/phc2sys.log"
echo ""
echo "To make this persistent across reboots, create a systemd service:"
echo ""
echo "cat > /etc/systemd/system/phc2sys.service <<EOF"
echo "[Unit]"
echo "Description=PTP Hardware Clock Synchronization"
echo "After=network.target"
echo ""
echo "[Service]"
echo "Type=simple"
echo "ExecStart=/usr/sbin/phc2sys -s CLOCK_REALTIME -c $PHC_DEVICE -O 0 -m -S 1.0"
echo "Restart=always"
echo "RestartSec=5"
echo ""
echo "[Install]"
echo "WantedBy=multi-user.target"
echo "EOF"
echo ""
echo "Then run:"
echo "  sudo systemctl daemon-reload"
echo "  sudo systemctl enable phc2sys"
echo "  sudo systemctl start phc2sys"
echo ""
echo -e "${YELLOW}Note: It may take 30-60 seconds for clocks to synchronize${NC}"
echo "      Monitor with: tail -f /var/log/phc2sys.log"
