# AIC8800 USB WiFi 6 Driver — High-Performance cfg80211 FullMAC

**Author:** aspira  
**Version:** 1.2.0  
**License:** GPL-2.0-only  
**Target Chips:** AIC8800D80 / AIC8800DC / AIC8800D80U  
**Supported Kernels:** Linux 5.10 LTS, 5.15 LTS, 6.1 LTS, 6.6 LTS, 6.12+  
**Target Platforms:** RK3568 / RK3588 / x86 Ubuntu / Debian / Yocto / Buildroot

---

## Overview

A production-grade out-of-tree Linux kernel driver for the AIC8800 series USB WiFi 6 modules. Built on a single-module architecture (`aic8800_fdrv.ko`) with FullMAC firmware integration via the standard cfg80211/nl80211 wireless stack. Designed for industrial, medical, and embedded applications requiring high throughput, rock-solid stability, and field-proven self-healing.

### Why This Driver Exists

Vendor-provided AIC8800 drivers commonly suffer from:

- Kernel API breakage across LTS versions
- Firmware/driver version mismatches causing hard hangs
- USB hotplug and suspend/resume instability
- URB leaks, use-after-free bugs, and workqueue dangling
- Incomplete cfg80211 state reporting (iw shows connected, NetworkManager shows unavailable)
- Missing diagnostics for field troubleshooting

This driver addresses all of the above with a clean, layered architecture and rigorous lifecycle management.

---

## Performance & Stability Targets

| Metric | Target |
|---|---|
| USB Mode | USB 2.0 High-Speed (USB 3.0 compatible) |
| 2.4 GHz Throughput | 60–120 Mbps (TCP) |
| 5 GHz Throughput | 150–400 Mbps (TCP) |
| Disconnect Recovery | < 5 seconds |
| Hotplug Recovery | < 3 seconds |
| CPU Usage at Full Load | < 50% of a single core |
| Continuous Operation | 7×24 hours, no kernel panic |
| Memory Leak | Zero sustained growth over 72-hour soak test |

---

## Architecture

The driver is organized into 8 logical layers, all compiled into a single `aic8800_fdrv.ko`:

```
 ┌──────────────────────────────────────────────┐
 │  Userspace                                    │
 │  NetworkManager / wpa_supplicant / iw / ip    │
 └──────────────────┬───────────────────────────┘
                    │ nl80211
 ┌──────────────────▼───────────────────────────┐
 │  Linux Wireless Core — cfg80211               │
 └──────────────────┬───────────────────────────┘
                    │ cfg80211_ops
 ┌──────────────────▼───────────────────────────┐
 │  AIC8800 WiFi Core                            │
 │  State Machine / Scan / Connect / Key / Stats │
 └──────────────────┬───────────────────────────┘
                    │
 ┌──────────────────▼───────────────────────────┐
 │  AIC8800 Data Path                            │
 │  TX Queue / RX Queue / QoS / WRR Scheduling   │
 └──────────────────┬───────────────────────────┘
                    │
 ┌──────────────────▼───────────────────────────┐
 │  AIC8800 Control Path                         │
 │  Command Queue / Event Queue / FW Mailbox     │
 └──────────────────┬───────────────────────────┘
                    │
 ┌──────────────────▼───────────────────────────┐
 │  AIC8800 Firmware Loader                      │
 │  Manifest Parsing / SHA256 / Download / Boot  │
 └──────────────────┬───────────────────────────┘
                    │
 ┌──────────────────▼───────────────────────────┐
 │  USB HCI Layer                                │
 │  Bulk IN/OUT / URB Anchors / Pre-alloc Pools  │
 └──────────────────┬───────────────────────────┘
                    │
 ┌──────────────────▼───────────────────────────┐
 │  USB Core / xHCI / EHCI / PHY / Power Mgmt    │
 └──────────────────────────────────────────────┘
```

### Source Tree

```
aic-wifi-driver/
├── include/                  # 15 public headers
│   ├── aic_compat.h          # Kernel 5.10 → 6.6 API compatibility layer
│   ├── aic_dev.h             # Core device struct + 15-state machine
│   ├── aic_usb.h             # USB layer: URB anchors, endpoint config
│   ├── aic_hci.h             # HCI protocol: frame types, cmd/event IDs
│   ├── aic_fw.h              # Firmware loader: manifest, chip identification
│   ├── aic_cmd.h             # Command queue: sync/async submission
│   ├── aic_event.h           # Event queue: demux + dispatch
│   ├── aic_tx.h              # TX path: 4 AC queues, WRR scheduler
│   ├── aic_rx.h              # RX path: pre-submit, data/event demux
│   ├── aic_cfg80211.h        # cfg80211_ops: scan, connect, key management
│   ├── aic_pm.h              # Power management: suspend, resume, runtime PM
│   ├── aic_recovery.h        # 6-level recovery engine + rate limiter
│   ├── aic_stats.h           # 64-bit atomic statistics counters
│   ├── aic_debugfs.h         # Debug filesystem interface (10 nodes)
│   └── aic_trace.h           # Kernel tracepoint definitions (5 events)
├── src/                      # 14 source files
│   ├── aic_main.c            # module_init/exit, 7 module parameters
│   ├── aic_usb.c             # USB probe/disconnect/suspend/resume
│   ├── aic_hci.c             # HCI header build/parse, SKB allocation
│   ├── aic_fw.c              # Firmware loading pipeline + SHA256 verification
│   ├── aic_cmd.c             # Command submission, timeout, response matching
│   ├── aic_event.c           # Event handling + per-event dispatch table
│   ├── aic_tx.c              # ndo_start_xmit, QoS classification, flow control
│   ├── aic_rx.c              # RX URB completion, data/event demultiplexing
│   ├── aic_cfg80211.c        # Full cfg80211_ops implementation
│   ├── aic_netdev.c          # net_device_ops: open, stop, start_xmit, tx_timeout
│   ├── aic_pm.c              # System/runtime suspend and resume
│   ├── aic_recovery.c        # Recovery execution, health check, link watch
│   ├── aic_stats.c           # Statistics dump and reset
│   └── aic_debugfs.c         # debugfs node creation and show/write handlers
├── Kbuild                    # Kernel build system
├── Makefile                  # Top-level make wrapper (build, install, DKMS)
├── dkms.conf                 # DKMS integration config
├── modprobe.d/               # Module parameter presets
│   └── aic8800.conf
├── udev/                     # USB hotplug rules
│   └── 99-aic8800.rules
├── firmware/                 # Firmware manifest template + install guide
│   ├── manifest.json
│   └── README.md
└── docs/                     # Full architecture specification
    └── AIC8800_USB_WiFi_Driver_Architecture.md
```

---

## Key Design Decisions

### 1. Single Module (not split fw loader + main driver)

All functionality lives in `aic8800_fdrv.ko`. This eliminates module load ordering issues and simplifies dependency management. The `softdep pre: cfg80211` directive ensures cfg80211 loads first.

### 2. FullMAC + cfg80211 (not mac80211)

The AIC8800 firmware handles all 802.11 MAC logic internally. The Linux driver acts as a transport and control interface — sending commands, receiving events, and shuttling data frames. A mac80211 rewrite without firmware source access would be impractical and risky.

### 3. Central State Machine

Every component checks `aic_state_can_tx()`, `aic_state_can_scan()`, `aic_state_is_online()`, etc. — never raw `bool` flags. The 15-state machine prevents impossible transitions (e.g., TX during firmware download).

```
UNINIT → USB_PROBED → FW_LOADING → FW_READY → HW_READY →
NETDEV_REGISTERED ↔ SCANNING ↔ CONNECTING ↔ CONNECTED ↔ DISCONNECTING
                                              ↕
                                      SUSPENDING ↔ SUSPENDED
                                              ↕
                                      RECOVERING → (back to NETDEV_REGISTERED or DEAD)
```

### 4. URB Anchors + Pre-allocated Pools

Every submitted URB is tracked by a `usb_anchor`. During disconnect or suspend, `usb_kill_anchored_urbs()` stops all in-flight transfers atomically. RX URBs (32 by default) are pre-submitted for zero-allocation hot-path performance. TX URBs (32 by default) are pooled to avoid per-packet alloc/free.

### 5. Weighted Round-Robin QoS

Four Access Category queues (VO/VI/BE/BK) with WRR weights of 4:3:2:1 prevent priority inversion. High/low watermarks (512/128) provide backpressure to the network stack.

### 6. Firmware Manifest Verification

A `manifest.json` file in the firmware directory specifies the chip model, driver ABI version, and per-file SHA256 hashes. The driver refuses to load mismatched firmware rather than hanging the system — a common failure mode with vendor drivers.

### 7. Tiered Recovery with Rate Limiting

Six recovery levels escalate from "ignore single error" through "restart queues", "clear USB halt", "firmware soft reset", "USB device reset", to "full driver re-init". Rate limiting (3/min, 2 USB resets per 10 minutes) prevents recovery storms that would mask root-cause hardware failures.

---

## Quick Start

### Prerequisites

```bash
# Kernel headers for your running kernel
sudo apt install linux-headers-$(uname -r) build-essential

# Verify cfg80211 is available
modprobe cfg80211
```

### Build

```bash
cd aic-wifi-driver
make
```

If successful, `aic8800_fdrv.ko` will be produced in the current directory.

### Install Firmware

The firmware binary files (`fw_patch.bin`, `wifi_fw.bin`, etc.) are chip-vendor proprietary and must be obtained from your module supplier or the original driver package. Place them at:

```
/lib/firmware/aic8800/aic8800d80/
├── fw_patch.bin
├── wifi_fw.bin
├── rf_config.bin          # optional
└── cali_config.bin        # optional
```

Update `/lib/firmware/aic8800/manifest.json` with actual SHA256 hashes:

```bash
sha256sum /lib/firmware/aic8800/aic8800d80/wifi_fw.bin
sha256sum /lib/firmware/aic8800/aic8800d80/rf_config.bin
```

### Install Module

```bash
sudo make install
```

This installs the kernel module, firmware, modprobe configuration, and udev rules.

### Load and Verify

```bash
# Load the module
sudo modprobe aic8800_fdrv

# Check that it loaded cleanly
dmesg | grep -i aic

# Verify the wireless interface
iw dev
ip link

# Scan for networks
sudo iw dev wlan0 scan | grep SSID

# Connect (via NetworkManager)
nmcli dev wifi connect "YourSSID" password "YourPassword"

# Or via wpa_supplicant directly
wpa_supplicant -B -i wlan0 -c /etc/wpa_supplicant/wpa_supplicant.conf
dhclient wlan0

# Verify connectivity
iw dev wlan0 link
ping -c 4 8.8.8.8
```

---

## Module Parameters

Configure in `/etc/modprobe.d/aic8800.conf` or pass on the kernel command line:

| Parameter | Type | Default | Description |
|---|---|---|---|
| `log_level` | int | 2 | 0=ERR, 1=WARN, 2=INFO, 3=DEBUG, 4=TRACE |
| `rx_urb_num` | int | 32 | Number of pre-allocated RX URBs (8–64) |
| `tx_urb_num` | int | 32 | Number of pre-allocated TX URBs (8–64) |
| `disable_usb_autosuspend` | bool | 1 | Disable USB autosuspend (recommended for stability) |
| `power_save` | bool | 0 | Enable firmware power saving |
| `low_latency` | bool | 1 | Prioritize latency over power saving |
| `recovery_enable` | bool | 1 | Enable automatic self-healing recovery |
| `firmware_verify` | bool | 1 | Verify manifest ABI version and SHA256 hashes |

### Industrial / Medical Recommended Settings

```
options aic8800_fdrv power_save=0 disable_usb_autosuspend=1 rx_urb_num=32 tx_urb_num=32 log_level=2 recovery_enable=1 firmware_verify=1 low_latency=1
```

### Consumer Device Settings

```
options aic8800_fdrv power_save=1 disable_usb_autosuspend=0 rx_urb_num=16 tx_urb_num=16 log_level=1 recovery_enable=1 firmware_verify=1 low_latency=0
```

---

## Debugging & Diagnostics

### debugfs Interface

All diagnostic nodes are under `/sys/kernel/debug/aic8800/<ifname>/`:

```bash
# Current device state
cat /sys/kernel/debug/aic8800/wlan0/state

# Full statistics dump
cat /sys/kernel/debug/aic8800/wlan0/stats

# Firmware version and CRC
cat /sys/kernel/debug/aic8800/wlan0/fw_version

# USB endpoint and URB status
cat /sys/kernel/debug/aic8800/wlan0/usb

# TX queue depths per AC
cat /sys/kernel/debug/aic8800/wlan0/txq

# RX queue status
cat /sys/kernel/debug/aic8800/wlan0/rxq

# Recovery history and rate limit info
cat /sys/kernel/debug/aic8800/wlan0/recovery

# Last 16 firmware events
cat /sys/kernel/debug/aic8800/wlan0/last_events

# Change log level at runtime
echo 3 > /sys/kernel/debug/aic8800/wlan0/log_level

# Manually trigger recovery (level 1–6)
echo 3 > /sys/kernel/debug/aic8800/wlan0/trigger_recovery
```

### Kernel Tracepoints

```bash
# Enable all AIC8800 tracepoints
echo 1 > /sys/kernel/debug/tracing/events/aic8800/enable

# View trace output
cat /sys/kernel/debug/tracing/trace_pipe

# Available events:
#   aic8800:aic_state_change   — state transitions
#   aic8800:aic_tx_frame       — TX frame submissions
#   aic8800:aic_rx_frame       — RX frame deliveries
#   aic8800:aic_fw_event       — firmware event reception
#   aic8800:aic_recovery       — recovery triggers
#   aic8800:aic_urb_error      — URB transfer errors
```

### Field Diagnostic Commands

```bash
# System-level
uname -a && lsusb && lsmod | grep -E "aic|cfg80211"

# Module info
modinfo aic8800_fdrv

# Wireless state
iw dev wlan0 info && iw dev wlan0 link && iw dev wlan0 station dump

# Driver logs
dmesg -T | grep -iE "aic|wlan|cfg80211|firmware"

# USB topology
lsusb -t
cat /sys/bus/usb/devices/*/power/control
```

---

## Throughput Testing

### Server (AP side)

```bash
iperf3 -s
```

### Client (device under test)

```bash
# TCP download (from AP to device)
iperf3 -c <AP_IP> -t 300

# TCP upload (from device to AP)
iperf3 -c <AP_IP> -t 300 -R

# Multi-stream
iperf3 -c <AP_IP> -t 300 -P 4

# UDP test
iperf3 -c <AP_IP> -t 60 -u -b 200M
```

---

## Stability Testing

### Hotplug Stress Test

```bash
for i in $(seq 1 100); do
    echo "Round $i"
    # Physically disconnect/reconnect or use a USB relay
    sleep 5
    iw dev
    dmesg | tail -20
done
```

### Suspend/Resume Loop

```bash
for i in $(seq 1 100); do
    echo mem | sudo tee /sys/power/state
    sleep 10
    iw dev wlan0 link
    ping -c 5 <gateway>
done
```

### Long-Running Soak Test

```bash
while true; do
    date
    iw dev wlan0 link
    ping -c 10 <gateway>
    iperf3 -c <server> -t 60
    sleep 30
done
# Run for 24h (basic), 72h (stability), 168h (production qualification)
```

---

## DKMS Integration (Ubuntu / Debian)

```bash
# Copy source tree to DKMS directory
sudo cp -r aic-wifi-driver /usr/src/aic8800-1.0.0

# Register, build, and install
sudo dkms add /usr/src/aic8800-1.0.0
sudo dkms build aic8800/1.0.0
sudo dkms install aic8800/1.0.0

# The module will auto-rebuild on kernel updates
```

---

## Yocto Integration

```bitbake
# recipes-kernel/aic8800/aic8800_git.bb
inherit module

SRC_URI = "git://<your-repo>/aic8800.git;protocol=https;branch=main"
S = "${WORKDIR}/git"

RPROVIDES:${PN} += "kernel-module-aic8800-fdrv"
FILES:${PN} += "${nonarch_base_libdir}/firmware/aic8800"
```

---

## Recovery Engine Reference

| Level | Name | Trigger | Action |
|---|---|---|---|
| 0 | NONE | Occasional single error | Count only, no action |
| 1 | RESTART_QUEUES | TX timeout | Flush and restart TX/RX queues |
| 2 | CLEAR_HALT | USB endpoint stall | `usb_clear_halt()` + re-submit URBs |
| 3 | FW_SOFT_RESET | Heartbeat loss, scan stuck | Firmware soft reset + re-download |
| 4 | USB_RESET | Firmware crash | `usb_reset_device()` + full re-init |
| 5 | REINIT | Repeated USB reset failures | Driver-level remove/probe cycle |
| 6 | DEGRADED | Rate limit exceeded | Stop auto-recovery, notify userspace |

**Rate Limits:** Max 3 recoveries per minute; max 2 USB resets per 10 minutes. Exceeding these thresholds places the driver in DEGRADED state requiring manual intervention.

---

## USB Device ID Table

| Vendor ID | Product ID | Chip | Notes |
|---|---|---|---|
| `0xa69c` | `0x5721` | AIC8800D80 | Native AIC VID/PID |
| `0x2357` | `0x014e` | AIC8800D80 | TP-Link branded variant |

To add support for additional VID/PID combinations, append entries to the `aic_usb_id_table` array in [src/aic_usb.c](src/aic_usb.c).

---

## Security Considerations

- **Input validation:** All firmware event payloads, USB RX buffers, and command responses are length-checked before parsing. Array indices from firmware (event ID, channel, rate, queue) are range-validated.
- **Private ioctl:** This driver implements only the standard cfg80211/nl80211 interface. No custom ioctls are exposed.
- **debugfs controls:** The `trigger_recovery` and `log_level` write nodes require root access (`0600`/`0644` permissions).
- **Firmware verification:** SHA256 hash checking in `manifest.json` prevents loading tampered or corrupted firmware binaries.
- **Memory safety:** `kfree_sensitive()` is used for key material. `copy_from_user()` bounds are checked on all write paths.

---

## Known Limitations & Risks

1. **Firmware binary availability:** The actual `fw_patch.bin`, `wifi_fw.bin`, and calibration files are proprietary and must be sourced from the chip vendor or module supplier. This repository includes only the manifest template.
2. **Firmware protocol opacity:** Without public firmware interface documentation, the command/event structures in `aic_hci.h` are based on reverse engineering and may need adjustment for specific firmware builds.
3. **Single STA mode only:** AP mode, P2P, and monitor mode are not implemented. The firmware may or may not support these modes.
4. **USB 3.0 throughput ceiling:** While the driver supports USB 3.0 devices, actual throughput is bounded by the chip's internal bus, antenna design, and RF environment.
5. **Kernel API churn:** cfg80211 and netdev APIs change between kernel versions. The `aic_compat.h` layer handles known differences for 5.10 through 6.12, but new kernels may require updates.

---

## Acceptance Criteria

### Functional

| Test | Pass Criteria |
|---|---|
| Module load | No warnings, oops, or errors in dmesg |
| WiFi scan (×100) | No hangs, scan results returned each time |
| WiFi connect/disconnect (×100) | Successful association and clean disconnect |
| DHCP | IP address obtained within 10 seconds |
| Ping (×1000) | No packet loss beyond 0.1% |
| iperf3 (5 min bidirectional) | Stable throughput, no driver crashes |
| Hotplug (×100) | No kernel panics or use-after-free |
| Suspend/resume (×100) | Functional WiFi after each resume |

### Stability

- 72-hour continuous iperf3 + ping loop
- Zero kernel panics
- Zero slab leaks (verified via `/proc/slabinfo`)
- Zero unrecoverable disconnections
- All recovery events have explainable root causes

### Performance

| Scenario | Target |
|---|---|
| 5 GHz, line-of-sight, TCP download | ≥ 150 Mbps |
| 5 GHz, line-of-sight, TCP upload | ≥ 100 Mbps |
| 2.4 GHz, line-of-sight, TCP download | ≥ 50 Mbps |
| 2.4 GHz, line-of-sight, TCP upload | ≥ 30 Mbps |

*Actual throughput depends on module model, antenna, AP capability, channel congestion, USB controller, and RF environment.*

---

## References

- [Linux Kernel cfg80211 Documentation](https://www.kernel.org/doc/html/latest/driver-api/80211/cfg80211.html)
- [Linux Wireless — cfg80211 Developer Docs](https://wireless.docs.kernel.org/en/latest/en/developers/documentation/cfg80211.html)
- [Linux Kernel USB Anchor API](https://docs.kernel.org/driver-api/usb/anchors.html)
- [Linux Kernel USB Host-Side API](https://www.kernel.org/doc/html/latest/driver-api/usb/index.html)
- [Linux Kernel NAPI Documentation](https://docs.kernel.org/networking/napi.html)
- [Linux Kernel Device Power Management](https://docs.kernel.org/driver-api/pm/devices.html)
- [radxa-pkg/aic8800 (GitHub)](https://github.com/radxa-pkg/aic8800)
- [Gentoo net-wireless/aic8800](https://packages.gentoo.org/packages/net-wireless/aic8800)
