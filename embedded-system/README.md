# Embedded Linux System Installer, Backup & Restore

A high-concurrency, high-reliability embedded Linux system management platform for ARM64 and x86_64 medical, industrial, and edge-computing devices. Provides system installation, A/B upgrade with automatic rollback, software backup and restore, and comprehensive power-loss recovery — all driven through a transaction-journaled, verifiable pipeline.

## Architecture Overview

```
 QML UI  ───  C++ ViewModel (MVVM)  ───  IPC (D-Bus / Unix Socket)  ───  installer-core (root)  ───  Linux Kernel
```

The system is split into five layers:

| Layer | Responsibility | Technology |
|---|---|---|
| QML UI | Pages, animations, progress, error display | Qt Quick / QML |
| UI Application | ViewModel, state, validation, IPC calls | C++ / Qt |
| IPC | UI ↔ privileged-service communication, event push | D-Bus or Unix Domain Socket + JSON-RPC |
| Core Service | Install, backup, restore, verify, partition, mount, journal | C++17 (this project) |
| System Adapter | Block devices, filesystems, bootloader, network | Linux API + tool wrappers |

This repository contains the **Core Service** (`installer-core` daemon), the **CLI tool** (`installer-cli`), and a **shared library** (`installer_core_lib`) that the QML UI layer can link against.

## Features

### Core Capabilities
- **Fresh system installation** — partition disk, create filesystems, write bootloader/kernel/rootfs, configure boot slots
- **A/B system upgrade** — write to inactive slot, verify, switch boot slot, automatic fallback on boot failure
- **Software & configuration backup** — consistent snapshots with encryption and compression
- **Software & configuration restore** — verified restore with schema migration and rollback safety
- **System partition backup & restore** — block-level and file-level, online and offline modes
- **Recovery mode** — independent recovery partition or initramfs environment
- **Package integrity** — SHA-256 hash verification and Ed25519 digital signature validation

### High Reliability
- **Transaction journal** — every critical operation is journaled with atomic writes (write → fsync → rename → fsync directory). After unexpected power loss, incomplete transactions are detected and resumed on restart.
- **Write-then-verify pipeline** — every image write is followed by read-back SHA-256 verification before the operation is marked complete.
- **`.partial` file pattern** — all file outputs (backups, packages) are written to a temporary `.partial` file and atomically renamed only after full verification.
- **A/B boot with rollback** — upgrades target the inactive slot only. The new system must pass a health check before being marked good. Failed boots automatically fall back to the previous slot.
- **RAII everywhere** — file descriptors, mount points, loop devices, child processes, and temporary files are all managed by RAII wrappers with guaranteed cleanup.
- **ProcessRunner** — all external commands are executed via `fork+execvp` with separate program and argument lists (no shell interpolation). Every process has a configurable timeout with SIGTERM → SIGKILL escalation.
- **Comprehensive error codes** — E1xxx through E9xxx with structured error information, retryability flags, and user-facing messages.

### High Concurrency
- **Producer-consumer I/O pipeline** — image writing uses a multi-stage bounded-queue pipeline: Reader → Decompressor → Writer (O_DIRECT) → Verifier. Each stage runs in its own thread with back-pressure to prevent memory exhaustion.
- **epoll-based IPC server** — the Unix domain socket server uses edge-triggered epoll for non-blocking, high-throughput client communication.
- **Fine-grained locking** — `std::shared_mutex` for read-heavy workloads, `std::mutex` for exclusive access, `std::atomic` for lock-free progress and cancellation propagation.
- **Concurrent device scanning** — block device enumeration and package hash verification run in parallel using a shared thread pool.

## Project Structure

```
embedded-system/
├── CMakeLists.txt                  # Top-level build
├── cmake/                          # Toolchain files, find modules
├── config/
│   └── installer.yaml              # Runtime configuration
├── include/installer/              # Public API (pure virtual interfaces + POD types)
│   ├── core/                       #   types.h, result.h, error_codes.h
│   ├── device/                     #   IDeviceManager
│   ├── package/                    #   IPackageManager
│   ├── image/                      #   IImageWriter
│   ├── partition/                  #   IPartitionManager
│   ├── filesystem/                 #   IFilesystemManager
│   ├── boot/                       #   IBootControl
│   ├── security/                   #   ISecurityManager
│   ├── journal/                    #   ITransactionJournal
│   ├── job/                        #   IJob, IJobStep, IJobManager
│   ├── log/                        #   ILogger
│   ├── platform/                   #   IProcessRunner
│   └── ipc/                        #   IIPCServer, IIPCClient
├── src/
│   ├── CMakeLists.txt              # Library & executable targets
│   ├── app/                        #   Entry points (core_service_main, service_setup)
│   ├── common/                     #   Error codes, file utilities, types
│   ├── log/                        #   StructuredLogger (JSON Lines)
│   ├── platform/                   #   ProcessRunner (fork+execvp)
│   ├── config/                     #   ConfigLoader (YAML)
│   ├── core/                       #   Domain implementations
│   │   ├── device_manager.cpp      #     sysfs + libudev enumeration
│   │   ├── package_manager.cpp     #     .espkg parsing, manifest, signature
│   │   ├── image_writer.cpp        #     Pipeline: read → decompress → write → verify
│   │   ├── partition_manager.cpp   #     GPT/MBR via sgdisk
│   │   ├── filesystem_manager.cpp  #     mkfs, fsck, mount/umount
│   │   ├── boot_control.cpp        #     U-Boot fw_setenv/fw_printenv
│   │   ├── security_manager.cpp    #     SHA-256, Ed25519, AES-256-GCM
│   │   ├── hardware_profile.cpp    #     Device-tree matching, profile loading
│   │   └── job_manager.cpp         #     Single-job serialization, event dispatch
│   ├── job/                        #   Job state machine & step chain
│   │   ├── base_job.cpp            #     Step-chain executor with rollback
│   │   ├── install_job.cpp         #     12-step install pipeline
│   │   └── steps/                  #     Individual step implementations
│   ├── journal/                    #   TransactionJournal (atomic JSON journal)
│   └── ipc/                        #   Unix socket server, JSON-RPC 2.0 codec
├── cli/                            # installer-cli tool
├── tests/                          # Unit tests, integration tests, helpers
│   └── helpers/                    #   Loop device, test package builder
├── systemd/                        # installer-core.service, installer-watchdog.service
├── scripts/                        # create_test_disk.sh, run_tests.sh
└── docs/                           # Architecture document
```

## Quick Start

### Prerequisites

| Dependency | Purpose |
|---|---|
| C++17 compiler (GCC 8+ or Clang 7+) | Build |
| CMake 3.16+ | Build system |
| yaml-cpp | Configuration parsing |
| nlohmann_json | JSON-RPC, manifest parsing |
| libsodium | Ed25519 signatures, AES-256-GCM |
| libzstd | Compression |
| spdlog | Structured logging |
| libudev | Device enumeration |
| Google Test (optional) | Unit tests |

Install on Debian/Ubuntu:

```bash
sudo apt-get install build-essential cmake ninja-build \
    libyaml-cpp-dev nlohmann-json3-dev libsodium-dev libzstd-dev \
    libspdlog-dev libudev-dev libgtest-dev
```

### Build (Native)

```bash
cd embedded-system
cmake -B build -GNinja -DCMAKE_BUILD_TYPE=Debug
cmake --build build
```

Build options:

| Option | Default | Description |
|---|---|---|
| `BUILD_CORE_SERVICE` | ON | Build the `installer-core` daemon |
| `BUILD_CLI` | ON | Build the `installer-cli` tool |
| `BUILD_TESTS` | ON | Build the test suite |
| `ENABLE_SANITIZERS` | OFF | Enable AddressSanitizer + UBSan |
| `ENABLE_FAULT_INJECT` | OFF | Enable fault injection hooks for testing |

### Cross-Compile for ARM64

```bash
cmake -B build-arm64 -GNinja \
    -DCMAKE_TOOLCHAIN_FILE=cmake/Toolchain-aarch64.cmake \
    -DCMAKE_BUILD_TYPE=Release \
    -DBUILD_TESTS=OFF
cmake --build build-arm64
```

## Usage

### Daemon

Start the core service:

```bash
sudo installer-core --config /etc/installer/installer.yaml --socket /var/run/installer.sock
```

The daemon listens on a Unix domain socket for JSON-RPC 2.0 requests from the CLI tool or QML UI.

### CLI

```bash
# List block devices
installer-cli device list

# Verify an installation package
installer-cli package verify /mnt/usb/system.espkg

# Start a system installation
installer-cli install --package /mnt/usb/system.espkg --target /dev/mmcblk0 --slot B

# Check job status
installer-cli job status
installer-cli job progress

# Cancel a running job
installer-cli job cancel

# Create a backup
installer-cli backup start --profile full_user_data --destination /mnt/usb/

# Restore from backup
installer-cli restore start --backup /mnt/usb/backup-20260712.esbak

# View boot slot configuration
installer-cli boot status

# Set next boot slot
installer-cli boot set-slot B

# Reboot the device
installer-cli reboot
```

## Installation Package Format (`.espkg`)

```
system-package.espkg
├── manifest.json          # Package metadata, payload list with SHA-256 hashes
├── manifest.sig           # Ed25519 signature over manifest.json
├── payload/
│   ├── bootloader.img
│   ├── kernel.img
│   ├── rootfs.img.zst
│   ├── recovery.img
│   └── device-tree.dtb
├── scripts/
│   ├── pre_install.sh
│   ├── post_install.sh
│   └── migrate.sh
└── metadata/
    ├── release-notes.md
    └── license.txt
```

## Partition Layout (A/B Standard)

```
Disk (eMMC / NVMe / SSD)
├── boot_a       256 MiB   vfat    BOOT_A
├── boot_b       256 MiB   vfat    BOOT_B
├── rootfs_a    3072 MiB   ext4    ROOTFS_A
├── rootfs_b    3072 MiB   ext4    ROOTFS_B
└── data        remaining  ext4    DATA
```

Partitions are identified by GPT PARTLABEL, never by hardcoded device paths.

## Key Design Principles

1. **UI and root-privileged operations are strictly separated.** The QML UI runs as an unprivileged user; only `installer-core` holds root capabilities.
2. **All high-risk operations are job/state-machine driven.** Install, backup, and restore are unified under a single `IJob` → `IJobStep` chain with prepare/execute/verify/rollback semantics.
3. **Every mutation is journaled.** The transaction journal guarantees that after a crash, the system knows exactly what was in progress and whether it is safe to resume.
4. **Upgrades never overwrite the active slot.** The running system is always preserved until the new system passes its health check.
5. **No device paths, partition numbers, or sizes are hardcoded.** Everything is driven by YAML configuration and hardware profiles.
6. **MD5 is never used as a security integrity check.** SHA-256 and Ed25519 are used for all cryptographic verification.
7. **Backup files are written to `.partial` first, then atomically renamed.** Users can never mistake an incomplete backup for a valid one.
8. **Every write is flushed and read-back verified.** `fsync()` + `BLKFLSBUF` + SHA-256 read-back before declaring success.
9. **The CLI, QML UI, and factory tools share the same core service.** No duplicated business logic.
10. **Recovery must be able to boot independently when the main system is damaged.**

## Testing

```bash
# Build and run all tests
cmake -B build -DBUILD_TESTS=ON -DENABLE_FAULT_INJECT=ON
cmake --build build
cd build && ctest --output-on-failure

# Run only unit tests
ctest -R unit

# Run integration tests (requires root for loop devices)
sudo ctest -R integration

# Run fault injection tests
ctest -R fault
```

Integration tests use Linux loop devices to simulate full disk operations without physical hardware:

```bash
# Create a 16 GB test disk image
./scripts/create_test_disk.sh 16384 test-disk.img
```

## License

Internal Use — Medical Device

---

> **Author:** aspira
