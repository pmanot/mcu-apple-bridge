# ESP32-S3 USB NCM iPhone Connectivity - Context Document

## Project Overview
This project creates a USB NCM (Network Control Model) Ethernet adapter on an ESP32-S3 that allows an iPhone to connect via USB-C and access an HTTP server running on the ESP32.

## Current Architecture

### USB Composite Device
The ESP32-S3 presents as a USB composite device with:
1. **CDC-ACM** (Virtual Serial Port) - for log output via `screen /dev/cu.usbmodem* 115200`
2. **NCM** (Network Control Model) - Ethernet-over-USB for iOS/macOS

### Network Stack
```
[iPhone] <--USB NCM--> [TinyUSB] <--callbacks--> [esp-netif] <--lwIP--> [HTTP Server]
```

- **DHCP Server**: Assigns 192.168.7.2-10 to clients, ESP32 is at 192.168.7.1
- **HTTP Server**: Serves endpoints on port 80
- **WiFi STA**: Connects to local WiFi as backup debug channel

### Key Files
| File | Purpose |
|------|---------|
| `main/network_setup.c` | USB NCM + esp-netif + DHCP setup + self-healing logic |
| `main/http_server.c` | HTTP endpoints including `/logs`, `/events`, `/status` |
| `main/log_stream.c` | Circular buffer for rolling logs (100 lines) |
| `main/event_log.c` | Sticky event buffer for critical events (never truncated) |
| `main/wifi_setup.c` | WiFi STA mode for debug access when USB fails |
| `main/usb_ncm_server.c` | Main app entry point |
| `managed_components/espressif__esp_tinyusb/tinyusb_net.c` | **PATCHED** - ESP-IDF TinyUSB wrapper |

---

## The iOS NCM Problem (SOLVED)

### Original Symptoms
- iPhone shows ESP32 as "Ethernet" in Settings
- iPhone **cannot get IP address via DHCP** (intermittently)
- Works if you wait a few seconds after ESP32 boot before connecting
- Fails if you connect immediately or replug after a failed attempt
- Once it fails, replug doesn't fix it - requires ESP32 reset

### What the Event Log Revealed

**Failing case:**
```
[   501 ms] NETIF_READY
[   596 ms] USB_MOUNTED
(nothing else - no packets received)
```

**Working case (after waiting):**
```
[   485 ms] NETIF_READY
[   663 ms] USB_MOUNTED
[   974 ms] FIRST_RX
[   976 ms] DHCP_DISCOVER_RX
[   979 ms] FIRST_TX
[   979 ms] DHCP_OFFER_TX
```

Key observation: `USB_MOUNTED` fires in both cases, but `FIRST_RX` only happens in the working case.

---

## Debugging Journey

### Initial Hypotheses Tested (All Wrong or Incomplete)

1. **Race condition in callback registration** - Moved `tinyusb_net_init()` earlier. Didn't help.

2. **TinyUSB NCM doesn't call `tud_network_init_cb()`** - Confirmed this is true (NCM never calls it, only ECM/RNDIS does), but this turned out to be a cosmetic issue, not the root cause.

3. **USB disconnect/connect to force clean enumeration** - Tried calling `tud_disconnect()` at boot then `tud_connect()` after setup. Didn't help.

4. **Recovery task with polling** - Added a task that polls `tud_mounted()` and tries `tud_network_recv_renew()` + `tud_network_link_state()`. This revealed important diagnostic info but didn't fix the issue.

### Key Diagnostic Discovery

The recovery task logged:
```
RECOVERY attempt 8: can_xmit=YES, trying to kick NCM...
```

**`can_xmit=YES`** means the NCM data interface WAS activated (iOS sent SET_INTERFACE). So packets should flow. But they don't.

This ruled out the "NCM not activated" theory and pointed to a TX-side problem.

---

## Root Cause Analysis

### Two Bugs Identified

#### Bug 1: `tud_ready()` vs `tud_mounted()` in esp_tinyusb

**Location:** `managed_components/espressif__esp_tinyusb/tinyusb_net.c`

**The bug:** Both `tinyusb_net_send_async()` and `tinyusb_net_send_sync()` check `tud_ready()` before sending:

```c
if (!tud_ready()) {
    return ESP_ERR_INVALID_STATE;
}
```

**The problem:** `tud_ready()` returns true only when:
- Device is mounted AND
- Device is not suspended AND
- **CDC line state is set (DTR/RTS)**

The CDC line state may **never** be set if no terminal is connected to the serial port. This means our DHCP OFFER responses get silently dropped!

**The fix:** Change to `tud_mounted()` which only checks if USB configuration is set:
```c
if (!tud_mounted()) {
    return ESP_ERR_INVALID_STATE;
}
```

#### Bug 2: iOS Only Does DHCP Once Per Link-Up

**The problem:** iOS treats NCM link-up notification as the trigger to start DHCP. If:
1. iOS sends DHCP DISCOVER
2. We fail to respond (due to Bug 1)
3. iOS gives up and caches this failure
4. Replug doesn't help because iOS remembers the failure

**The fix:** Explicitly control NCM link state:
1. Start with link DOWN
2. Only bring link UP when stack is fully ready
3. If no RX after timeout, force USB disconnect/reconnect to clear iOS's cached state
4. Re-send link-up notification to trigger fresh DHCP

---

## The Fix Implementation

### 1. Patch to tinyusb_net.c

Changed `tud_ready()` to `tud_mounted()` in two places (lines 71 and 86):

```diff
- if (!tud_ready()) {
+ if (!tud_mounted()) {
```

**Note:** This is in `managed_components/` which gets overwritten on component updates. Consider:
- Upstreaming the fix to Espressif
- Creating a local component override
- Adding a post-install patch script

### 2. Rewritten network_setup.c

Key changes:

1. **Explicit link state control:**
   - `usb_set_link_state(false, "reason")` - Link DOWN
   - `usb_set_link_state(true, "reason")` - Link UP + notification to iOS

2. **Link state flow:**
   - Boot: Link DOWN
   - USB mount: Keep link DOWN
   - Stack ready + mounted: Kick DOWN→UP to trigger DHCP

3. **Self-healing watchdog:**
   - If mounted + stack ready but no RX for 2 seconds: force `tud_disconnect()`/`tud_connect()`
   - This clears iOS's "gave up" state
   - Exponential backoff to avoid thrashing

4. **TX retry loop:**
   - DHCP timing is tight, retry send up to 3 times

---

## Verification

### How to Confirm Fix is Working

1. Check `/events` endpoint - should show full DHCP sequence:
   ```
   NETIF_READY
   USB_MOUNTED
   NCM_LINK_UP (stack_ready_kick_up)
   FIRST_RX
   DHCP_DISCOVER_RX
   FIRST_TX
   DHCP_OFFER_TX
   ```

2. Check `/status` endpoint - all DHCP flags should be YES:
   ```json
   {
     "FIRST_RX": true,
     "DHCP_DISCOVER_RX": true,
     "DHCP_OFFER_TX": true
   }
   ```

3. Serial logs should show:
   ```
   *** USB MOUNTED ***
   *** USB NCM LINK DOWN *** (mounted)
   *** USB NCM LINK UP *** (stack_ready_kick_up)
   ```

4. If recovery activates, you'll see:
   ```
   *** USB RECOVER: tud_disconnect/tud_connect (attempt 1) ***
   ```

### Test Scenarios Needed

- [ ] Connect immediately after boot
- [ ] Connect after waiting 5 seconds
- [ ] Replug after successful connection
- [ ] Replug after failed connection (before fix would stay broken)
- [ ] Multiple rapid replug cycles
- [ ] USB suspend/resume (phone screen lock/unlock)
- [ ] Test with macOS (should also work)

---

## Lessons Learned

### 1. USB Composite Device Interactions
CDC-ACM and NCM share the USB device. The `tud_ready()` function checks CDC state, which can block NCM operations even though they're independent protocols.

### 2. iOS NCM Behavior
- iOS only initiates DHCP on link-up notification
- iOS caches failure state and won't retry without re-enumeration
- iOS is very timing-sensitive during initial connection

### 3. TinyUSB NCM Gaps
- `tud_network_init_cb()` is never called by NCM driver (only ECM/RNDIS)
- Need to use `tud_network_link_state()` explicitly for proper iOS behavior
- `tud_network_can_xmit()` tells you if data interface is active

### 4. Debugging USB Issues
- Event logging with timestamps is essential
- Polling `tud_mounted()` is more reliable than waiting for callbacks
- `can_xmit` returning YES but no traffic = TX path problem
- `can_xmit` returning NO = NCM not activated (SET_INTERFACE issue)

---

## Configuration Constants

```c
#define USB_LINK_KICK_DELAY_MS        250     // DOWN→UP delay for iOS to notice
#define USB_NO_RX_GRACE_MS            2000    // Wait this long for first RX
#define USB_RECOVER_DETACH_MS         400     // Detach duration
#define USB_RECOVER_POST_ATTACH_MS    400     // Settle time after attach
#define USB_RECOVER_MAX_ATTEMPTS      5       // Per mount cycle
#define USB_RECOVER_BACKOFF_START_MS  2500    // Initial backoff
#define USB_RECOVER_BACKOFF_MAX_MS    15000   // Max backoff
```

These may need tuning based on further testing.

---

## Known Issues / Future Work

1. **Patch fragility**: The `tinyusb_net.c` patch is in managed_components and will be overwritten. Need a permanent solution.

2. **DHCP handshake may still not complete**: The event log sometimes shows OFFER sent but no REQUEST received. This might be a separate issue with DHCP options or timing.

3. **No DNS option**: iOS may want DNS server in DHCP. Consider adding.

4. **Gateway is fake**: Using 192.168.7.254 as gateway (non-existent) to prevent iOS routing internet traffic. May need to use ESP32's IP (192.168.7.1) instead.

5. **Testing needed**: The fix appears to work but needs more rigorous testing across different iOS versions and connection scenarios.

---

## HTTP Endpoints Reference

| Endpoint | Description |
|----------|-------------|
| `/` | Status page |
| `/led`, `/led/on`, `/led/off` | LED control |
| `/reset` | Restart device |
| `/logs` | SSE real-time log stream |
| `/logs_all` | Static dump of last 100 log lines |
| `/events` | Critical events (sticky, never truncated) |
| `/status` | JSON with boolean flags for each event type |

---

## Debug Workflow

1. Flash the device
2. Connect via WiFi to `http://192.168.0.207/` (or whatever IP)
3. Check `/events` to see critical milestones
4. Check `/status` for JSON flags
5. Connect iPhone via USB-C
6. Refresh `/events` to see what happened
7. Compare against expected sequence

### Expected Successful Sequence
```
NETIF_READY
USB_MOUNTED
NCM_LINK_UP (with reason)
FIRST_RX
DHCP_DISCOVER_RX
FIRST_TX
DHCP_OFFER_TX
(ideally: DHCP_REQUEST_RX, DHCP_ACK_TX, DHCP_ASSIGNED)
```

---

## References

- Espressif ESP-IDF TinyUSB component: `espressif__esp_tinyusb`
- TinyUSB NCM driver: `src/class/net/ncm_device.c`
- Related Espressif issue: iOS/NCM compatibility problems with `tud_ready()` gating
