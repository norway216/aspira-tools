# Self-Healing Storage Subsystem Architecture (V2)

## Grade: Medical / Industrial Embedded Systems
## Version: V2.0 - Self-Healing Design

---

# 1. System Overview

This system evolves the previous Disk Health Daemon into a:

> **Self-Healing Storage Subsystem (SHSS)**

It is designed to not only detect disk failures, but also:

- Automatically recover from transient failures
- Isolate faulty devices
- Switch to safe operating modes
- Perform predictive failure mitigation
- Maintain system availability under degradation

---

# 2. Design Goals

## 2.1 High Availability (HA)
- No single disk failure should crash system
- Continuous operation under degraded mode
- Automatic failover mechanisms

## 2.2 Self-Healing Capability
- Detect anomalies
- Classify fault severity
- Trigger automatic recovery actions

## 2.3 Predictive Maintenance
- SMART trend analysis
- IO latency anomaly detection
- Temperature drift prediction

## 2.4 Safety (Medical Device Requirement)
- No destructive disk operations without confirmation
- Read-only monitoring by default
- Failsafe mode on uncertainty

---

# 3. High-Level Architecture

```
+------------------------------------------------------+
|            Self-Healing Storage Subsystem           |
+------------------------------------------------------+

        +-----------------------------+
        |   Device Discovery Layer    |
        +-----------------------------+
                      |
        +-----------------------------+
        |   Health Monitoring Engine  |
        +-----------------------------+
                      |
        +-----------------------------+
        |   Fault Detection Engine    |
        +-----------------------------+
                      |
        +-----------------------------+
        |   Decision / Policy Engine  |
        +-----------------------------+
                      |
        +-----------------------------+
        |   Self-Healing Executor     |
        +-----------------------------+
                      |
        +-----------------------------+
        |   Logging / Audit System    |
        +-----------------------------+
                      |
        +-----------------------------+
        |   Systemd / OS Integration  |
        +-----------------------------+
```

---

# 4. Core Modules

## 4.1 Device Discovery Layer

Responsibilities:
- Detect SATA / NVMe / USB disks
- Monitor hotplug events (udev)
- Maintain device registry

Interfaces:
```c
int scan_devices(device_list_t *list);
int on_device_event(int event_type);
```

---

## 4.2 Health Monitoring Engine

Sources:
- SMART attributes
- /proc/diskstats
- NVMe telemetry

Collected metrics:
- Reallocated sectors
- Pending sectors
- CRC errors
- IO latency
- Temperature

---

## 4.3 Fault Detection Engine

Classifies faults into:

| Level | Meaning |
|------|--------|
| INFO | Normal behavior |
| WARN | Early degradation |
| DEGRADED | Performance/health drop |
| CRITICAL | Failure imminent |
| FATAL | Device unusable |

Detection methods:
- Threshold-based detection
- Trend-based anomaly detection
- Statistical deviation analysis

---

## 4.4 Policy Engine (Decision Core)

Input:
- SMART metrics
- IO behavior
- Historical trends

Output actions:
- Continue monitoring
- Throttle IO
- Isolate disk
- Read-only mode
- Trigger failover
- System rollback

---

## 4.5 Self-Healing Executor

### Actions:

#### IO Throttling
Reduce disk load:
```bash
ionice -c3 -p <pid>
```

#### Disk Isolation
```bash
echo 1 > /sys/block/sdX/device/delete
```

#### Read-only fallback
```bash
mount -o remount,ro /
```

#### Failover switch
Redirect IO to backup device

#### Service restart
Restart dependent services safely

---

## 4.6 Logging & Audit System

- Append-only logs
- Structured JSON format
- Tamper-resistant storage

Example:
```json
{
  "device": "/dev/sda",
  "event": "DEGRADED",
  "action": "THROTTLE_IO",
  "score": 62,
  "timestamp": "2026-06-27T10:12:00"
}
```

---

## 4.7 System Integration Layer

- systemd service manager
- udev event hooks
- kernel /proc & /sys interface
- watchdog timer

---

# 5. Self-Healing Workflow

```
[Device Metrics]
      ↓
[Health Monitor]
      ↓
[Fault Detection]
      ↓
[Policy Engine]
      ↓
[Self-Healing Executor]
      ↓
+----------------------+
| Actions Executed     |
| - throttle IO        |
| - isolate disk       |
| - remount ro         |
| - failover           |
+----------------------+
      ↓
[Audit Log + System Report]
```

---

# 6. Self-Healing Levels

## Level 1 - Soft Degradation
- Increase monitoring frequency
- Reduce IO priority
- Log warnings

## Level 2 - Controlled Degradation
- Enable read-only mode
- Flush caches
- Disable writes

## Level 3 - Isolation
- Remove disk from active pool
- Stop IO routing
- Trigger failover disk

## Level 4 - Emergency Mode
- Freeze non-critical services
- Force system safe state
- Alert external system

---

# 7. Policy Decision Matrix

| Condition | Action |
|----------|--------|
| Pending sectors increasing | WARN |
| Reallocated sectors rising | throttle IO |
| Temperature > 60°C | reduce IO |
| IO latency spike | switch mode |
| SMART failure | isolate disk |

---

# 8. Failure Prediction Layer

- Moving average trend analysis
- Linear regression on SMART values
- IO latency anomaly detection

Output:
- Failure probability score (0–100%)

---

# 9. Recovery & Rollback

- Config rollback
- Policy rollback
- Binary A/B fallback
- Automatic service restart

---

# 10. Safety Constraints

- Read-only by default
- No destructive disk operations unless isolated
- All actions logged
- Fail-safe fallback always enabled

---

# 11. Future Extensions (V3)

- AI failure prediction engine
- Distributed monitoring cluster
- Cloud telemetry integration
- RAID orchestration engine
