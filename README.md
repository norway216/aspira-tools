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
├── .gitignore             # Git ignore rules
└── README.md              # This file
```

## Projects

### disk-checker — Disk Health Monitoring Daemon

A high-reliability daemon that continuously monitors disk health for embedded Linux medical devices. It tracks SATA/SAS/HDD/SSD SMART attributes, NVMe endurance indicators, I/O performance metrics, and kernel device errors, providing real-time health scoring with predictive failure detection.

For detailed documentation, see [disk-checker/README.md](disk-checker/README.md).

### Quick Build

```bash
cd disk-checker
mkdir -p build && cd build
cmake ..
make
```

## Requirements

- Linux kernel 4.x or later
- GCC or Clang with C11 support
- CMake 3.10 or later
- Root privileges for SMART ioctl access (runtime)

## License

Internal Use — Medical Device

---

> **Author:** aspira
