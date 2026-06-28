# aspira-tools

A collection of reliable, embedded-systems-oriented monitoring and diagnostic tools for Linux-based medical and industrial devices.

## Repository Structure

```
aspira-tools/
├── disk-checker/          # Disk health monitoring daemon
│   ├── include/           # Core header files
│   ├── src/               # C source modules
│   ├── tests/             # Test scripts
│   ├── docs/              # Architecture and design documentation
│   ├── systemd/           # systemd service unit
│   ├── CMakeLists.txt     # CMake build configuration
│   └── README.md          # Project-specific documentation
├── network-audit/         # Lightweight network audit framework
│   ├── include/           # Master header (types, enums, constants)
│   ├── core/              # epoll reactor + timer wheel + FD manager
│   ├── net/               # Non-blocking socket layer
│   ├── scan/              # TCP connect scan engine (state machine)
│   ├── fp/                # Banner fingerprint engine
│   ├── result/            # Lock-free SPSC ring buffer
│   ├── worker/            # Optional pthread worker pool
│   ├── db/                # Optional SQLite3 persistence
│   ├── cli/               # CLI entry point + argument parsing
│   ├── docs/              # Design documentation
│   ├── build.sh           # Build & test script
│   └── Makefile           # Makefile build configuration
├── .gitignore             # Git ignore rules
└── README.md              # This file
```

## Projects

### disk-checker — Disk Health Monitoring Daemon

A high-reliability daemon that continuously monitors disk health for embedded Linux medical devices. It tracks SATA/SAS/HDD/SSD SMART attributes, NVMe endurance indicators, I/O performance metrics, and kernel device errors, providing real-time health scoring with predictive failure detection.

For detailed documentation, see [disk-checker/README.md](disk-checker/README.md).

### network-audit — Lightweight Network Audit Framework

An ultra-low-memory (<20MB), high-concurrency (10k+ connections) TCP network audit framework built on epoll. Features non-blocking connect scanning with a 5-state machine, O(1) timer wheel, lock-free result pipeline, banner fingerprinting (SSH, HTTP, TLS, SMTP, FTP, MySQL, RDP, etc.), and optional SQLite3 persistence. Designed for internal lab network auditing with strict security constraints (no brute force, no exploitation).

For detailed documentation, see [network-audit/docs/lightweight_network_audit_framework_v1.md](network-audit/docs/lightweight_network_audit_framework_v1.md).

### Quick Build

**disk-checker:**
```bash
cd disk-checker
mkdir -p build && cd build
cmake ..
make
```

**network-audit:**
```bash
cd network-audit
./build.sh release      # Optimized release build
./build.sh smoke        # Build + localhost smoke test
./build.sh all          # Full pipeline: clean + debug + release + size + check
```

## Requirements

- Linux kernel 4.x or later
- GCC or Clang with C11 support
- CMake 3.10 or later (disk-checker only)
- Root privileges for SMART ioctl access (disk-checker runtime)

## License

Internal Use — Medical Device

---

> **Author:** aspira
