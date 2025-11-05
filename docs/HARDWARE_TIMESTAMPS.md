# Hardware Timestamp Clock Synchronization

## Problem: Incorrect HW→EVENT Latency

When hardware timestamping is enabled, you may observe unrealistically high HW→EVENT latencies (e.g., 20+ milliseconds), even though actual network latency should be microseconds. This indicates a **clock synchronization issue**.

### Root Cause

The library uses two different clock sources:

1. **Hardware NIC Timestamps** (`hw_timestamp_ns`)
   - Source: NIC's internal **PTP Hardware Clock (PHC)**
   - Reference: Arbitrary epoch (NIC power-on time)
   - Clock domain: NIC hardware

2. **Event Timestamps** (`event_timestamp`)
   - Source: CPU's **Time Stamp Counter (TSC)**
   - Reference: CPU cycles since system boot
   - Clock domain: System/CPU

These clocks:
- Have **different reference points** (epochs)
- **Drift independently** over time
- Cannot be directly compared without synchronization

When you calculate `HW→EVENT = event_timestamp - hw_timestamp`, you're subtracting timestamps from **unsynchronized clocks**, producing garbage values.

## Solution: PTP Clock Synchronization

### What is PTP?

**PTP (Precision Time Protocol)**, also known as IEEE 1588, synchronizes clocks across network devices with sub-microsecond precision. Modern NICs have a **PHC (PTP Hardware Clock)** that can be synchronized with the system clock.

### Synchronization Architecture

```
┌─────────────────────────────────────────────────────────┐
│                    System Clock                          │
│              (CLOCK_REALTIME)                            │
└────────────────────┬────────────────────────────────────┘
                     │
                     │ phc2sys daemon
                     │ (continuous sync)
                     ↓
┌─────────────────────────────────────────────────────────┐
│            NIC PTP Hardware Clock (PHC)                  │
│               (/dev/ptp0, /dev/ptp1, ...)               │
└─────────────────────────────────────────────────────────┘
                     │
                     │ SO_TIMESTAMPING
                     ↓
              Hardware Timestamps
```

### Quick Setup

Run the provided setup script:

```bash
# Auto-detect network interface
sudo ./scripts/setup_ptp_sync.sh

# Or specify interface explicitly
sudo ./scripts/setup_ptp_sync.sh eth0
```

The script will:
1. Check if your NIC supports hardware timestamping
2. Identify the PHC device (e.g., `/dev/ptp0`)
3. Install `linuxptp` package if needed
4. Start `phc2sys` daemon to sync PHC ↔ System Clock
5. Monitor synchronization status

### Manual Setup

#### 1. Check NIC Capabilities

```bash
# Check if NIC supports hardware timestamping
ethtool -T eth0

# Expected output should include:
#   PTP Hardware Clock: 0
#   Hardware Transmit Timestamp Modes:
#     off
#     on
#   Hardware Receive Filter Modes:
#     none
#     all
```

#### 2. Install PTP Tools

```bash
# Ubuntu/Debian
sudo apt-get install linuxptp

# RHEL/CentOS/Fedora
sudo yum install linuxptp
```

#### 3. Start PHC Synchronization

```bash
# Sync PHC with system clock
# -s CLOCK_REALTIME: Use system clock as source
# -c /dev/ptp0: Target PHC device
# -O 0: No offset correction
# -m: Print messages to stdout
# -S 1.0: Update interval (1 second)

sudo phc2sys -s CLOCK_REALTIME -c /dev/ptp0 -O 0 -m -S 1.0
```

You should see output like:
```
phc2sys[12345.678]: CLOCK_REALTIME phc offset      -123 s0 freq    +456 delay   1234
phc2sys[12345.789]: CLOCK_REALTIME phc offset       +45 s1 freq    +501 delay   1235
phc2sys[12345.890]: CLOCK_REALTIME phc offset       -12 s2 freq    +489 delay   1236
```

**Key metrics:**
- **offset**: Clock difference in nanoseconds (should converge to <100 ns)
- **freq**: Frequency adjustment in ppb (parts per billion)
- **s0/s1/s2**: Servo state (s2 = locked, best)

#### 4. Make Persistent (systemd)

Create `/etc/systemd/system/phc2sys.service`:

```ini
[Unit]
Description=PTP Hardware Clock Synchronization
After=network.target

[Service]
Type=simple
ExecStart=/usr/sbin/phc2sys -s CLOCK_REALTIME -c /dev/ptp0 -O 0 -m -S 1.0
Restart=always
RestartSec=5

[Install]
WantedBy=multi-user.target
```

Enable and start:
```bash
sudo systemctl daemon-reload
sudo systemctl enable phc2sys
sudo systemctl start phc2sys
sudo systemctl status phc2sys
```

### Verification

After synchronization is running, test the application:

```bash
# Enable hardware timestamps
WS_ENABLE_HW_TIMESTAMPS=1 ./test_binance_integration
```

Expected latency breakdown (after sync):
```
📊 Latency Breakdown (Mean):
   HW→EVENT (kernel):         5000 ns  [16.7%]    # Should be microseconds, not milliseconds
   EVENT→SSL (decryption):   20000 ns  [66.7%]
   SSL→APP (processing):      5000 ns  [16.6%]
   ────────────────────────────────────────────────
   Total (HW→APP):           30000 ns  [100.0%]
```

**Before sync**: HW→EVENT shows 20+ milliseconds (clock drift)
**After sync**: HW→EVENT shows 1-10 microseconds (realistic kernel processing)

### Monitoring

Check sync status:
```bash
# View sync log
tail -f /var/log/phc2sys.log

# Check servo state (should show "s2" for locked)
grep "s2" /var/log/phc2sys.log

# Monitor offset (should be <100 ns when locked)
tail -f /var/log/phc2sys.log | grep -oP 'offset\s+[+-]?\d+'
```

### Troubleshooting

#### Issue: "No PTP Hardware Clock found"

**Solution**: Your NIC doesn't support hardware timestamping. Use a compatible NIC:
- Intel: X710, XL710, XXV710, E810
- Mellanox: ConnectX-5, ConnectX-6
- Broadcom: BCM957xxx series

#### Issue: "phc2sys offset not converging"

**Solution**:
1. Check system clock is synchronized: `timedatectl status`
2. If system clock is not synced, use NTP:
   ```bash
   sudo apt-get install ntp
   sudo systemctl start ntp
   ```
3. Wait 30-60 seconds for PHC servo to lock

#### Issue: "Permission denied accessing /dev/ptp0"

**Solution**: Run as root or add user to required group:
```bash
sudo usermod -a -G dialout $USER
# Log out and log back in
```

## Alternative: Software-Only Timestamps

If hardware timestamping is not available or PTP sync is not feasible, the library automatically falls back to software timestamps:

```bash
# kTLS mode (default) - no hardware timestamps
./test_binance_integration

# Shows EVENT→SSL→APP breakdown only (no HW→EVENT)
```

This mode uses:
- **EVENT**: TSC timestamp when `epoll_wait()` returns
- **SSL**: TSC timestamp when `SSL_read()` completes
- **APP**: TSC timestamp when callback is invoked

All timestamps use the same clock (TSC), so no synchronization is needed.

## Performance Comparison

| Mode                          | Latency | HW Timestamps | kTLS | Use Case                          |
|-------------------------------|---------|---------------|------|-----------------------------------|
| kTLS (default)                | ~24 μs  | ❌ No         | ✅ Yes | Production (best performance)     |
| HW Timestamps (with PTP sync) | ~46 μs  | ✅ Yes        | ❌ No  | Performance analysis & debugging  |
| HW Timestamps (no sync)       | Invalid | ⚠️  Incorrect | ❌ No  | Unusable (clock drift)            |

## References

- [Linux PTP Project](https://linuxptp.sourceforge.net/)
- [IEEE 1588 Precision Time Protocol](https://en.wikipedia.org/wiki/Precision_Time_Protocol)
- [Linux Kernel SO_TIMESTAMPING Documentation](https://www.kernel.org/doc/Documentation/networking/timestamping.txt)
- [Intel Ethernet Hardware Timestamping](https://www.intel.com/content/www/us/en/support/articles/000007151/ethernet-products.html)
