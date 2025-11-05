# Known Issues and Tradeoffs

This library is optimized for single-threaded, ultra-low-latency market data workloads. This document catalogs remaining risks and feature gaps to help you decide what to harden for your deployment.

**Last Updated:** 2025-11-05
**Total Active Issues:** 28 (1 Critical, 5 High, 10 Medium, 12 Low)
**Recently Fixed:** 4 bugs (frame overflow, INT_MAX checks, ws_send overflow, Host header port)

---

## Table of Contents

1. [Recently Fixed](#recently-fixed)
2. [Critical Issues](#critical-issues)
3. [High-Severity Issues](#high-severity-issues)
4. [Medium-Severity Issues](#medium-severity-issues)
5. [Low-Severity Issues](#low-severity-issues)
6. [Deployment Guidance](#deployment-guidance)

---

## Recently Fixed

The following bugs were identified and fixed in the latest code review:

### ✅ Fixed: Frame Arithmetic Overflow (was Tradeoff #20)
**Status:** FIXED
**Location:** `ws.c:545-549` (`parse_ws_frame_zero_copy`)
**Fix:** Added `__builtin_add_overflow()` check for `header_len + payload_len` calculation. Connection is closed if overflow detected.

### ✅ Fixed: INT_MAX Guard in ssl_read_into (was Tradeoff #18/31)
**Status:** FIXED
**Location:** `ssl.c:553-554` (`ssl_read_into`)
**Fix:** Added INT_MAX clamping consistent with `ssl_recv()` and `ssl_send()`.

### ✅ Fixed: ws_send Overflow Check
**Status:** FIXED
**Location:** `ws.c:942-947` (`ws_send`)
**Fix:** Added `__builtin_add_overflow()` check for `frame_len + len` calculation to prevent buffer overflow.

### ✅ Fixed: Host Header Port Number (was Issue #4)
**Status:** FIXED
**Location:** `ws.c:448-454` (`send_handshake`)
**Fix:** Host header now includes port number when non-standard (not 443). Enables WebSocket upgrades on custom ports like `:9443`, `:8443`.

---

## Critical Issues

### Issue #1 – Certificate Verification Disabled
**Location:** `ssl.c:47`
**Severity:** CRITICAL
**Impact:** MITM attackers can impersonate exchanges and intercept/modify trading data.

**Current Code:**
```c
SSL_CTX_set_verify(global_ctx, SSL_VERIFY_NONE, NULL);
```

**Mitigation:**
- **Essential:** Only deploy on trusted, private networks (VPC, dedicated links)
- **Recommended:** Re-enable verification:
  ```c
  SSL_CTX_set_verify(global_ctx, SSL_VERIFY_PEER, NULL);
  SSL_CTX_set_default_verify_paths(global_ctx);
  ```
- Use environment variable `WS_ENABLE_CERT_VERIFY=1` if implementing optional verification

**Risk:** Remote code execution, data theft, market manipulation via MITM attacks.

---

## High-Severity Issues

### Issue #2 – Unsafe Legacy Renegotiation Allowed
**Location:** `ssl.c:57`
**Severity:** HIGH
**Impact:** Vulnerable to TLS renegotiation attack (CVE-2009-3555).

**Current Code:**
```c
SSL_CTX_set_options(global_ctx, SSL_OP_ALLOW_UNSAFE_LEGACY_RENEGOTIATION);
```

**Mitigation:** Remove this flag unless connecting to legacy servers. Modern exchanges use TLS 1.2+ with secure renegotiation.

---

### Issue #3 – TLS 1.2 Pinning for kTLS
**Location:** `ssl.c:64-67`
**Severity:** HIGH
**Impact:** TLS 1.3-only venues (e.g., Bitget) refuse connection.

**Current Code:**
```c
#ifdef SSL_BACKEND_KTLS
SSL_CTX_set_max_proto_version(global_ctx, TLS1_2_VERSION);
#endif
```

**Mitigation:** Allow TLS 1.3 by default and fall back to TLS 1.2+kTLS only when server supports it. Detect kTLS capability dynamically.

---

### Issue #4 – HTTP 200 Accepted as Successful Upgrade
**Location:** `ws.c:408` (`parse_http_response`)
**Severity:** HIGH
**Impact:** State machine believes handshake succeeded when server is still speaking HTTP.

**Current Code:**
```c
if (strstr(ws->http_buffer, "HTTP/1.1 200") ||
    strstr(ws->http_buffer, "HTTP/1.1 101"))
```

**Mitigation:** Accept only `101 Switching Protocols` and validate `Sec-WebSocket-Accept` header.

---

### Issue #5 – 64-bit Frame Length Truncated to 32-bit
**Location:** `ws.c:933` (`ws_send`)
**Severity:** HIGH
**Impact:** Frames > 4 GB are encoded incorrectly.

**Current Code:**
```c
// 64-bit length encoding truncates high 32 bits
uint32_t len_high = (uint32_t)(len >> 32);
uint32_t len_low = (uint32_t)(len & 0xFFFFFFFF);
```

**Mitigation:** Either perform full 64-bit big-endian write or explicitly cap sends to ring buffer size (8 MB).

---

### Issue #6 – uint64_t to size_t Truncation on 32-bit Builds
**Location:** `ws.c:544-576` (`parse_ws_frame_zero_copy`)
**Severity:** HIGH (32-bit platforms only)
**Impact:** Large frames wrap on 32-bit systems despite overflow checks.

**Note:** Partially mitigated by overflow checks added in recent fixes, but 32-bit platforms still at risk.

**Mitigation:** Guard every downcast with explicit checks or drop 32-bit support entirely.

---

## Medium-Severity Issues

### Issue #8 – IPv4-Only DNS Resolution
**Location:** `ssl.c:158` (`ssl_init`)
**Severity:** MEDIUM
**Impact:** Cannot reach IPv6-only venues.

**Current Code:**
```c
hints.ai_family = AF_INET;  // IPv4 only
```

**Mitigation:** Use `AF_UNSPEC` and iterate through `addrinfo` results for both IPv4 and IPv6.

---

### Issue #9 – TEXT Opcode Hard-Coded in ws_send
**Location:** `ws.c:917` (`ws_send`)
**Severity:** MEDIUM
**Impact:** Cannot emit binary/continuation frames.

**Current Code:**
```c
frame[0] = 0x81;  // FIN + TEXT opcode hard-coded
```

**Mitigation:** Expose opcode parameter or provide `ws_send_binary()` function.

---

### Issue #10 – Missing Frame-Length vs Buffer Check
**Location:** `ws.c` (`parse_ws_frame_zero_copy`)
**Severity:** MEDIUM
**Impact:** Payloads larger than 8 MB ring buffer could cause wraparound issues.

**Mitigation:** Reject frames larger than `RINGBUFFER_SIZE` (8 MB) explicitly.

---

### Issue #11 – Dropped PONG When TX Buffer Full
**Location:** `ws.c:679-700` (`send_pong_frame`)
**Severity:** MEDIUM
**Impact:** Missing PONGs can cause server to close session.

**Current Code:**
```c
if (available >= total_size) {
    // Send PONG
} // else: silently drop
```

**Mitigation:** Reserve control-frame space or flush TX buffer before dropping.

---

### Issue #12 – Partial Commit on Wraparound Failure
**Location:** `ws.c:955-977` (`ws_send`)
**Severity:** MEDIUM
**Impact:** Header may be committed without payload when buffer wraps, corrupting stream.

**Mitigation:** Reserve space upfront atomically or implement rollback on failure.

---

### Issue #13 – Oversized HTTP Response Hangs Handshake
**Location:** `ws.c:614-622` (`handle_http_stage`)
**Severity:** MEDIUM
**Impact:** 4 KB handshake buffer fills and handshake stalls forever.

**Current Code:**
```c
// Reserve 1 byte for null terminator
size_t space_available = sizeof(ws->http_buffer) - ws->http_len - 1;
if (space_available == 0) return;  // Silently stalls
```

**Mitigation:** Fail fast with connection close when `http_len` reaches limit.

---

### Issue #14 – CLOSE Response May Never Send
**Location:** `ws.c:702-729` (`send_close_response`)
**Severity:** MEDIUM
**Impact:** If TX buffer full, socket marked closed without queuing CLOSE frame (RFC violation).

**Mitigation:** Flush buffer or retry instead of toggling state immediately.

---

### Issue #15 – Hardware Timestamping vs kTLS Incompatibility
**Location:** `bio_timestamp.c`
**Severity:** MEDIUM
**Impact:** Some NIC drivers disable RX hardware timestamps when TLS offload enabled.

**Mitigation:** Document platform constraints (Intel i40e has this issue) or add capability probes.

---

### Issue #16 – Fragmented Frames Unsupported
**Location:** `ws.c:528-530` (FIN=0 rejection)
**Severity:** MEDIUM
**Impact:** Servers sending FIN=0 frames (fragmentation) are rejected.

**Current Code:**
```c
if (!(frame_header & 0x80)) {
    ws->closed = 1;  // Reject fragmented frames
    return -1;
}
```

**Mitigation:** Add fragmentation support if compression or large multi-part messages needed.

---

### Issue #17 – Incomplete HTTP Header Validation
**Location:** `ws.c:408-424` (`parse_http_response`)
**Severity:** MEDIUM
**Impact:** Doesn't enforce full RFC handshake (Connection/Upgrade headers, Sec-WebSocket-Accept validation).

**Mitigation:** Harden validation if connecting to untrusted servers.

---

### Issue #18 – Timer Lazy-Init Race (Multi-Threaded Only)
**Location:** `os.c:476-480` (`os_get_cpu_cycle`)
**Severity:** MEDIUM (multi-threaded only)
**Impact:** First caller may race in multi-threaded builds (30ms race window).

**Note:** Library is documented as single-threaded. Not a bug for intended use case.

**Mitigation:** Call `os_init_timer()` before creating threads, or add atomics if multi-threading required.

---

## Low-Severity Issues

### Issue #19 – SSL Error Strings Skipped
**Location:** `ssl.c:52`
**Severity:** LOW
**Impact:** Debugging harder; production latency improved.

**Mitigation:** Enable error strings in debug builds only.

---

### Issue #20 – Global SSL_CTX Never Freed
**Location:** `ssl.c:35`
**Severity:** LOW
**Impact:** One-time 200 KB leak at exit.

**Mitigation:** Acceptable for long-running processes; call `ssl_cleanup()` in tooling/tests.

---

### Issue #21 – Timer Initialization Race (Explicit Path)
**Location:** `os.c:500-503` (`os_init_timer`)
**Severity:** LOW
**Impact:** Same as Issue #18 but for explicit initialization path.

**Mitigation:** Same as Issue #18 - single-threaded use only.

---

### Issue #22 – Ring Buffer Commit Clamps Silently
**Location:** `ringbuffer.c:308-318` (`ringbuffer_commit_write`)
**Severity:** LOW
**Impact:** Overcommit is clamped instead of erroring.

**Current Code:**
```c
if (len > available) len = available;  // Silent clamp
```

**Mitigation:** Add assertion or return error code for strict behavior.

---

### Issue #23 – Fixed 100ms Event-Loop Timeout
**Location:** `ws_notifier.c:164`
**Severity:** LOW
**Impact:** Adds latency when both sides idle.

**Mitigation:** Expose configurable timeout if workload is bursty.

---

### Issue #24 – Weak RNG Fallback for WebSocket Key
**Location:** `ws.c:339-346` (`generate_websocket_key`)
**Severity:** LOW
**Impact:** Falls back to `rand()` if `/dev/urandom` fails.

**Mitigation:** Abort instead, or use better fallback PRNG (xoshiro128+).

---

### Issue #25 – Hardware Timestamp Zero Ambiguity
**Location:** `bio_timestamp.c:130-144`
**Severity:** LOW
**Impact:** Zero value ambiguous (no timestamp vs epoch).

**Mitigation:** Check `hw_timestamp_ns` changed between reads rather than absolute value.

---

### Issue #26 – BASE64_ENCODE_SIZE Macro Overflow (Theoretical)
**Location:** `ws.c:5-7`
**Severity:** LOW
**Impact:** Only overflows for inputs > 16 exabytes (impossible with current buffers).

**Mitigation:** None needed - physically impossible with 4 KB HTTP buffers.

---

### Issue #27 – Port Parsing Doesn't Clear errno
**Location:** `ws.c:304` (`parse_url`)
**Severity:** LOW
**Impact:** Stale errno doesn't cause false positives (range check catches overflow).

**Analysis:** `strtol()` overflow returns `LONG_MAX` which exceeds 65535, caught by existing range check. Not a bug.

**Mitigation:** Clear errno for consistency if desired, but functionally correct as-is.

---

### Issue #28 – Hardware Timestamp Flag Not Reset
**Location:** `bio_timestamp.c:129-146`
**Severity:** LOW
**Impact:** `hw_available` may stay true after read with no timestamp.

**Mitigation:** Check timestamp value changed between reads, not just flag state.

---

### Issue #29 – Timestamp Arithmetic Overflow (Year 2262+)
**Location:** `bio_timestamp.c:135,143`; `os.c:422`
**Severity:** LOW
**Impact:** Timestamps overflow after year 2262.

**Status:** FIXED - overflow checks added, saturates to `UINT64_MAX`.

---

### Issue #30 – Timestamp Conversion Overflow (macOS Generic Path)
**Location:** `os.c:506-511`
**Severity:** LOW
**Impact:** Generic fallback overflows after 49 hours uptime on non-standard macOS configs.

**Status:** FIXED - overflow check added, switches to floating-point on overflow.

---

## Deployment Guidance

### Priority Triage

**Address immediately:**
1. **Issue #1 (CRITICAL):** Certificate verification - deploy only on trusted networks or enable verification
2. **Issues #2-6 (HIGH):** Protocol/compatibility issues affecting correctness

**Address for production:**
3. **Issues #8-18 (MEDIUM):** Feature gaps and edge cases
4. **Issues #19-30 (LOW):** Nice-to-have improvements

### Network Security

- **Always run on trusted networks.** Certificate verification disabled; vulnerable to MITM.
- **Use VPC/VLAN isolation** with strict firewall rules
- **Consider VPN tunnels** to exchanges for added protection

### Threading Model

- **Keep code single-threaded** (documented design constraint)
- Timer initialization races (Issues #18, #21) only affect multi-threaded use
- Call `os_init_timer()` before spawning threads if multi-threading required

### Hardening Checklist

Before connecting to untrusted servers:
- [ ] Re-enable certificate verification (Issue #1)
- [ ] Remove unsafe renegotiation (Issue #2)
- [ ] Validate HTTP 101 response (Issue #4)
- [ ] Harden handshake validation (Issue #17)
- [ ] Use strong RNG for WebSocket keys (Issue #24)

### Buffer Constraints

- **Ring buffer limit:** 8 MB (RINGBUFFER_SIZE)
- Frames > 8 MB not supported (Issue #10)
- 64-bit frame lengths truncated (Issue #5)
- 32-bit platforms have additional truncation risks (Issue #6)

### Testing Recommendations

- Test with TLS 1.3 servers (Issue #3)
- Test IPv6-only venues (Issue #8)
- Test fragmented frames if using compression (Issue #16)
- Test PING/PONG under high load (Issue #11)

---

**Document Version:** 3.0
**Last Audit:** 2025-11-05
**Next Review:** When adding new features or before production deployment
