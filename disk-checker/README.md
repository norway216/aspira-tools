# disk_health — Disk Health Monitoring Daemon

## Overview

`disk_health` is a high-reliability disk health monitoring daemon designed for embedded Linux medical devices, including ultrasound, CT, MRI consoles, and edge AI imaging systems.

It continuously monitors:

- **SATA / SAS / HDD / SSD** SMART attributes
- **NVMe** health status and endurance indicators
- **I/O performance** metrics (IOPS, latency, throughput)
- **Kernel device errors** and drive temperature

And provides:

- Real-time health scoring with severity-level alerts
- Predictive failure detection based on SMART attribute trends
- Persistent audit logging with timestamps and severity levels
- Optional JSON output for integration with UI dashboards or cloud monitoring
- systemd-based daemon lifecycle management

---

## Design Principles

This program adheres to medical device software requirements:

| Principle | Implementation |
|-----------|---------------|
| **Reliability** | No single point of failure, systemd auto-restart, non-blocking I/O, safe degradation on missing SMART support |
| **Predictability** | Deterministic sampling interval (5–120 s configurable), no dynamic memory allocation in the runtime loop, fixed-size buffers |
| **Traceability** | Every scan logged with timestamp, raw SMART snapshot preserved, event-driven alarm logging |
| **Extensibility** | Plugin-based device support, JSON output for easy integration, reserved interface for future AI failure prediction |

---

## Health Scoring Model

```
score = 100
  - 5 × reallocated_sectors       (max 40)
  - 10 × pending_sectors          (max 30)
  - 20 × uncorrectable_errors     (max 40)
  - 1 × (crc_errors / 10)         (max 10)
  - temperature penalty           (max 30)
  - NVMe wear penalty             (max 20)
```

| Score  | State      | Meaning                   |
|--------|------------|---------------------------|
| 90–100 | HEALTHY    | Normal operation          |
| 70–89  | WARNING    | Attention recommended     |
| 40–69  | DEGRADED   | Performance degradation   |
| < 40   | CRITICAL   | Failure predicted         |

---

## Quick Start

### Build

```bash
# Using CMake
mkdir -p build && cd build
cmake ..
make

# Or compile directly
gcc -std=c11 -Wall -Wextra -O2 -Iinclude src/*.c -o disk_health
```

### Usage

```bash
# One-shot JSON output
./disk_health --json
./disk_health --device /dev/sda --json

# Foreground continuous monitoring
./disk_health --watch --interval 30

# Daemon mode (requires root)
sudo ./disk_health

# Show help
./disk_health --help
```

### Installation (systemd)

```bash
sudo cmake --install build
sudo systemctl daemon-reload
sudo systemctl enable --now disk_healthd
```

---

## JSON Output Example

```json
{
  "timestamp": 1782568003,
  "devices": [{
    "device": "/dev/sda",
    "name": "sda",
    "model": "VBOX HARDDISK",
    "serial": "VB1234567890",
    "type": "SATA",
    "rotational": true,
    "score": 92,
    "state": "HEALTHY",
    "temperature": 42,
    "reallocated_sectors": 0,
    "pending_sectors": 0,
    "uncorrectable_errors": 0
  }]
}
```

---

## Logging

### Log Levels

| Level | Description |
|-------|-------------|
| `DEBUG` | Diagnostic details (disabled by default) |
| `INFO` | Normal operational messages |
| `WARN` | Warning conditions that may require attention |
| `CRITICAL` | Critical failures requiring immediate action |

Set the minimum level with `--log-level`:

```bash
./disk_health --watch --log-level debug
```

### Log Rotation

By default, log files are automatically rotated when they reach **10 MiB**, keeping the last **5 backups**:

```
/var/log/disk_health/disk_health.log      ← current (active)
/var/log/disk_health/disk_health.log.1    ← newest backup
/var/log/disk_health/disk_health.log.2
/var/log/disk_health/disk_health.log.3
/var/log/disk_health/disk_health.log.4
/var/log/disk_health/disk_health.log.5    ← oldest backup (deleted next rotation)
```

Configure rotation:

```bash
# Set max size to 50 MiB and keep 10 backups
./disk_health --log-max-size 50 --log-keep 10

# Disable rotation entirely
./disk_health --log-max-size 0
```

### Log Format

```
[2026-06-27 21:32:10] [INFO] disk_health v1.0.0 starting
[2026-06-27 21:32:40] [INFO] /dev/sda score=92 state=HEALTHY temp=42 realloc=0 ...
[2026-06-27 21:33:10] [WARN] Device /dev/sda is WARNING (score=75) — check recommended
[2026-06-27 21:33:40] [CRITICAL] CRITICAL: Device /dev/sda score=10 — immediate attention!
```

---

## Configuration File (Optional)

`/etc/disk_health/disk_health.conf`:

```ini
interval=30
log_path=/var/log/disk_health/disk_health.log
log_level=info
log_max_size=10          # MiB, 0 = disable rotation
log_keep=5               # number of backup files
watch_device=/dev/sda
```

---

## Testing

```bash
cd tests/
./test_scanner.sh       # Device detection
./test_smart_ata.sh     # SMART data parsing
./test_io_metrics.sh    # I/O metrics
./test_health_score.sh  # Health scoring (white-box)
./test_json_output.sh   # JSON format
./test_daemon.sh        # Daemon lifecycle
```

Or via CTest:

```bash
cd build && ctest
```

---

## Requirements

- Linux kernel 4.x or later
- GCC or Clang with C11 support
- Root privileges for SMART ioctl access

---

## Security

- Read-only disk access — no write, format, or destructive commands
- systemd capability set restricted to `CAP_SYS_RAWIO`
- Append-only logging ensures integrity
- Filesystem mounted read-only except for the log directory

---

## Project Structure

```
disk-health-check/
├── include/disk_health.h        # Core type and constant definitions
├── src/
│   ├── main.c                   # Entry point
│   ├── config.c                 # CLI / config file parser
│   ├── daemon.c                 # Daemon & main monitoring loop
│   ├── scanner.c                # Block device scanning and identification
│   ├── smart_ata.c              # ATA SMART reader (ioctl)
│   ├── smart_nvme.c             # NVMe SMART reader (sysfs)
│   ├── io_metrics.c             # /proc/diskstats parser
│   ├── health_score.c           # Health scoring engine
│   ├── logger.c                 # Logging system
│   └── json_builder.c           # Inline JSON builder
├── systemd/disk_healthd.service # systemd unit
├── tests/                       # Test scripts
└── docs/                        # Architecture documentation
```

---

## Usage Scenarios

| Scenario | Description |
|----------|-------------|
| **Medical Imaging Console** | Monitors the primary storage of CT / MRI / ultrasound consoles. Alerts the clinical engineering team before a disk failure causes scan interruption or data loss. |
| **Embedded Edge AI System** | Runs on inference nodes at the edge that continuously write model outputs to local SSD storage. Detects NAND wear-out early through NVMe percentage-used tracking. |
| **Industrial Automation Controller** | Protects factory-floor Linux controllers against HDD mechanical failure. Pending-sector growth triggers a WARNING so maintenance can be scheduled during planned downtime. |
| **Remote Telemetry Node** | Combined with the `--json` flag, health metrics are ingested by a central monitoring pipeline (e.g., Prometheus, Grafana, or a custom cloud dashboard). |
| **Development & Staging Environments** | Non-root `--watch` mode allows developers and QA engineers to monitor disk health during stress testing without deploying the full daemon. |

---

## Precautions

- **Root privileges required for SMART access.** The daemon mode (`sudo disk_health`) requires root because the `HDIO_DRIVE_CMD` and `HDIO_GET_IDENTITY` ioctls are privileged operations. For non-root usage, the `--json` and `--watch` flags provide read-only metrics where kernel permits.
- **Not a substitute for backups.** A score of HEALTHY (90–100) does not guarantee the disk will not fail. This tool provides statistical early warning based on SMART indicators; always maintain a separate backup and disaster-recovery strategy.
- **SMART support varies by hardware.** USB-attached drives, some hardware RAID controllers, and virtualized block devices (e.g., VirtIO) may not expose SMART data. The daemon handles this gracefully by reporting `"type": "UNKNOWN"` and skipping SMART-dependent scoring.
- **Log directory must exist or be writable.** By default logs go to `/var/log/disk_health/disk_health.log`. If the directory does not exist and cannot be created, the daemon falls back to stderr. In production, ensure the directory is provisioned before starting the service.
- **Interval selection matters.** Setting the sampling interval too low (e.g., 5 seconds) may cause unnecessary I/O wakeups on battery-powered devices. For most medical imaging workflows, the default 30-second interval is appropriate.
- **NVMe percentage-used is a wear indicator, not a health score.** A drive at 85% used is still functional, but the scoring engine will begin applying penalties above 80% to prompt proactive replacement before the endurance limit is reached.
- **Do not run multiple instances against the same device.** Concurrent SMART reads from two daemon instances will produce inconsistent results and may trigger false alerts. Use the PID file (`/var/run/disk_healthd.pid`) to guard against duplicate launches.

---

> **Author:** aspira  
> **Version:** 1.0.0  
> **License:** Internal Use — Medical Device
