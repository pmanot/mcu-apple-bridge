# ESP32-S3 USB NCM Server

USB CDC-NCM (Network Control Model) implementation for ESP32-S3 that creates an Ethernet-over-USB connection with iPhone and macOS. No host drivers required.

## Motivation

Connecting an ESP32 to an iPhone typically requires either:
- Bluetooth (limited throughput, pairing complexity)
- WiFi (requires network infrastructure, power hungry)
- MFi-certified accessories (expensive licensing)

USB CDC-NCM provides a direct, low-latency network connection over USB-C using the built-in networking stack on iOS/macOS. The ESP32 appears as a standard USB Ethernet adapter.

## What This Achieves

- ESP32-S3 enumerates as a class-compliant USB Ethernet adapter
- DHCP server assigns IP addresses to connected hosts (192.168.7.x)
- HTTP server at 192.168.7.1 for application communication
- Works on iOS 17+ and macOS without installing drivers
- Maintains host WiFi/cellular connectivity (does not hijack default route)

## Hardware

- ESP32-S3 with USB-OTG support
- USB-C cable (data-capable)

Pin mapping (directly to USB connector on most dev boards):
- D+ = GPIO20
- D- = GPIO19

## Build and Flash

Requires ESP-IDF v5.0+.

```bash
cd usb-ncm-server
. ~/esp/esp-idf/export.sh
idf.py set-target esp32s3
idf.py build
idf.py flash
```

## Usage

1. Connect ESP32-S3 to iPhone/Mac via USB-C
2. Wait for DHCP lease (~2 seconds)
3. Access `http://192.168.7.1/` from browser or app

### HTTP Endpoints

| Endpoint | Method | Description |
|----------|--------|-------------|
| `/` | GET | Status page |
| `/led` | GET | Get LED state (JSON) |
| `/led/on` | POST | Turn LED on |
| `/led/off` | POST | Turn LED off |
| `/reset` | POST | Restart ESP32 |

### Command Line Testing (macOS)

```bash
curl http://192.168.7.1/
curl -X POST http://192.168.7.1/led/on
curl -X POST http://192.168.7.1/reset
```

## Protocol Overview

CDC-NCM (Communications Device Class - Network Control Model) is a USB class specification for network adapters. Unlike CDC-ECM, NCM supports packet aggregation and is more reliably supported on iOS.

The implementation uses TinyUSB's NCM device class integrated via `esp_tinyusb`. The ESP32 runs:
1. USB device stack with NCM descriptors
2. lwIP network interface bound to USB
3. DHCP server for automatic IP assignment
4. HTTP server for application layer

Data flow:
```
iPhone/Mac <--USB NCM--> ESP32 lwIP stack <--> HTTP server
```

## Common Issues

### Interface appears but no IP address

- Ensure NCM mode is enabled (`CONFIG_TINYUSB_NET_MODE_NCM=y`)
- Check that DHCP server started (monitor logs)
- On iOS: device must be unlocked when first connecting

### iPhone loses WiFi/cellular when connected

The DHCP server advertises a non-routable gateway (192.168.7.254) to prevent iOS from routing internet traffic through the ESP32. If this still occurs:
- Disconnect and reconnect
- Check iOS Settings > Ethernet for manual IP configuration

### Connection fails after replug

The USB stack may not reinitialize cleanly on rapid reconnection. Workarounds:
- Wait 2-3 seconds between unplug/replug
- Call `POST /reset` from your app before disconnecting
- Press the reset button on ESP32

### Works on macOS but not iOS

iOS has stricter NCM requirements:
- Must advertise router option in DHCP (even if non-routable)
- Device must be unlocked (USB accessory restrictions)
- Some iOS versions require specific NCM state machine behavior

### macOS blocks WiFi when connected

macOS may prioritize USB Ethernet over WiFi. Fix:
1. System Settings > Network
2. Click "..." menu > Set Service Order
3. Drag WiFi above USB Ethernet

## Network Configuration

| Parameter | Value |
|-----------|-------|
| ESP32 IP | 192.168.7.1 |
| Subnet | 255.255.255.0 |
| DHCP Pool | 192.168.7.2 - 192.168.7.10 |
| Gateway (advertised) | 192.168.7.254 (non-existent) |
| USB MAC | 02:02:11:22:33:01 |
| lwIP MAC | 02:02:11:22:33:02 |

## License

MIT
