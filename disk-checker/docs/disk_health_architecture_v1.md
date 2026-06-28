# Medical-Grade Disk Health Monitoring System (C Daemon Architecture)

## Version: V1.0 (Industrial Embedded / Medical Imaging Systems)

---

# 1. System Overview

This system is a **high-reliability storage health monitoring daemon** designed for embedded Linux-based medical devices (e.g., ultrasound, CT, MRI consoles, edge AI imaging systems).

It continuously monitors:

- SATA/SAS HDD/SSD SMART attributes
- NVMe health status
- IO performance metrics
- Device errors from kernel
- Temperature and endurance indicators

It provides:

- Real-time health scoring
- Predictive failure detection
- Persistent audit logging
- System-level alerting integration
- Systemd-based daemon lifecycle management

---

# 2. Design Goals (Medical Device Requirements)

## 2.1 Reliability
- No single point of failure
- Daemon auto-restart via systemd
- Non-blocking IO operations
- Safe degradation on missing SMART support

## 2.2 Predictability
- Deterministic sampling interval (5s–60s configurable)
- No dynamic memory leaks in runtime loop
- Fixed-size buffers for logs and metrics

## 2.3 Traceability (Audit Requirement)
- Every scan logged with timestamp
- Raw SMART snapshot stored
- Event-based alarm logging

## 2.4 Extensibility
- Plugin-based device support
- JSON output for UI / remote monitoring
- Future AI-based failure prediction module

---

# 3. High-Level Architecture

```
+------------------------------------------------------+
|                 Disk Health Daemon                   |
+------------------------------------------------------+
|                                                      |
|  [Core Engine]                                       |
|      |                                               |
|      +--> Disk Scanner (/dev/sdX, nvme0n1)          |
|      +--> Device Manager                             |
|                                                      |
|  [SMART Layer]                                       |
|      +--> ATA SMART Reader (ioctl)                  |
|      +--> NVMe SMART Reader (/sys/class/nvme)       |
|                                                      |
|  [Metrics Layer]                                     |
|      +--> /proc/diskstats IO analyzer               |
|      +--> latency / throughput calculator           |
|                                                      |
|  [Health Engine]                                     |
|      +--> scoring model                             |
|      +--> anomaly detection                         |
|                                                      |
|  [Logging System]                                    |
|      +--> file logger                               |
|      +--> rotation                                 |
|      +--> event tagging                             |
|                                                      |
|  [Daemon Controller]                                 |
|      +--> systemd integration                       |
|      +--> config reload (SIGHUP)                   |
+------------------------------------------------------+
```

---

# 4. Module Design

## 4.1 Core Module

### Responsibilities:
- Detect block devices
- Identify SATA / NVMe type
- Maintain device list

### Interfaces:
```c
int scan_block_devices(device_list_t *list);
int classify_device(const char *dev);
```

---

## 4.2 SMART Module

### SATA / HDD (ATA SMART)

Uses:
```c
ioctl(fd, HDIO_GET_IDENTITY)
ioctl(fd, SMART_READ_DATA)
```

Extracted attributes:

| Attribute | Meaning |
|----------|--------|
| 5 | Reallocated Sector Count |
| 187 | Uncorrectable Errors |
| 197 | Pending Sectors |
| 198 | Offline Uncorrectable |
| 194 | Temperature |

---

### NVMe SMART

From:

```
/sys/class/nvme/nvme0/device/
```

Attributes:

- media_errors
- critical_warning
- temperature
- percentage_used
- data_units_read/write

---

## 4.3 IO Metrics Module

Reads:

```
/proc/diskstats
```

Metrics:

- Read/write IOPS
- Latency approximation
- Error counters
- Queue depth behavior

---

## 4.4 Health Scoring Engine

### Formula (Medical Device Grade)

```
score = 100

- 5 * reallocated_sector
- 10 * pending_sector
- 20 * uncorrectable_error
- 1  * crc_error_rate

if temperature > 55°C:
    score -= 10

if nvme_percentage_used > 80%:
    score -= 15
```

### Health Levels

| Score | State |
|------|------|
| 90–100 | Healthy |
| 70–89 | Warning |
| 40–69 | Degraded |
| <40 | Critical |

---

## 4.5 Logging System

### Features:
- Append-only log
- Timestamped entries
- Severity levels

### Log Format:

```
[2026-06-27 10:01:22] [INFO] Disk sda OK score=92
[2026-06-27 10:02:10] [WARN] Pending sector detected
[2026-06-27 10:02:50] [CRITICAL] Disk failure predicted
```

### Storage Path:
```
/var/log/disk_health/disk_health.log
```

---

## 4.6 Daemon Lifecycle

### Steps:

1. systemd starts daemon
2. scan devices
3. periodic monitoring loop
4. health computation
5. log + alert output

### Loop Interval:
- Default: 30 seconds
- Configurable: 5–120 seconds

---

# 5. Data Flow

```
[Disk Devices]
      ↓
[Scanner]
      ↓
[SMART + IO Reader]
      ↓
[Metrics Aggregation]
      ↓
[Health Scoring Engine]
      ↓
+-------------------+
| Logging System     |
| Alert System       |
| JSON Export        |
+-------------------+
      ↓
[Systemd / UI / Cloud]
```

---

# 6. System Interfaces

## 6.1 CLI Output Mode

```
disk_health --json
disk_health --watch
disk_health --device /dev/sda
```

---

## 6.2 JSON Output Example

```json
{
  "device": "/dev/sda",
  "score": 86,
  "state": "WARNING",
  "temperature": 48,
  "reallocated": 2,
  "pending": 1
}
```

---

# 7. Systemd Integration

```ini
[Unit]
Description=Disk Health Monitoring Daemon
After=multi-user.target

[Service]
ExecStart=/usr/local/bin/disk_healthd
Restart=always
RestartSec=3

[Install]
WantedBy=multi-user.target
```

---

# 8. Failure Prediction Strategy (Important)

## Trend-Based Detection

### Indicators:

- Pending sector growth rate > 0
- Reallocated sector increasing
- Temperature instability
- IO latency spike

### Rule:

```
if (trend(smart_attr) increasing):
    trigger WARNING
if (rapid increase):
    trigger CRITICAL
```

---

# 9. Security Considerations (Medical Devices)

- Read-only SMART access
- No destructive disk commands
- No format/write operations
- Privilege separation (root-only daemon)
- Log integrity protection

---

# 10. Future Extensions (V2/V3 Roadmap)

## V2:
- Full SMART ioctl parser
- NVMe advanced telemetry
- REST API (localhost)
- Qt dashboard

## V3:
- RAID monitoring
- dual-disk failover
- cloud telemetry
- AI failure prediction model

---

# 11. Summary

This system is designed as a **medical-grade storage reliability subsystem**, focusing on:

- Deterministic monitoring
- Predictive failure detection
- Industrial logging standards
- Embedded Linux compatibility
