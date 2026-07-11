# System Recovery v2

**Embedded Linux System Recovery / Installation Tool**

A modular, maintainable replacement for the legacy lvglsysrecovery application.
Provides system recovery, system installation, and software backup/restore
via an LVGL-based graphical interface on ARM64 embedded platforms.

## Architecture

```
┌──────────────────────────────────────────────────────┐
│  Application Layer:  app_core, event_bus, ui_manager │
├──────────────────────────────────────────────────────┤
│  Service Layer:  recovery, install, backup services  │
│                  + pluggable operation modules        │
├──────────────────────────────────────────────────────┤
│  HAL Layer:  display (fbdev), input (multi-device),   │
│              storage (mount/partition)                │
└──────────────────────────────────────────────────────┘
```

## Directory Structure

```
system-recovery/
├── docs/
│   ├── architecture-design.md
│   └── test-reports/
│       ├── functional-test-report.md
│       └── system-test-report.md
├── src/
│   ├── main.c                 # Entry point
│   ├── core/                  # Event bus, app lifecycle
│   ├── ui/                    # Screen manager + screens
│   ├── services/              # Business logic + plugins
│   ├── hal/                   # Hardware abstraction
│   └── common/                # Shared types, utilities
├── config/
│   └── default_config.ini     # Runtime configuration
├── tests/
│   ├── unit/                  # Unit tests
│   ├── integration/           # Integration tests
│   └── system/                # System-level tests
└── Makefile
```

## Building

### Cross-compile for ARM64 (target)
```sh
make CC=aarch64-linux-gnu-gcc
```

### Native build for testing
```sh
make NATIVE=1 CC=gcc test
```

### Install on target
```sh
make install DESTDIR=/path/to/rootfs
```

## Testing

```sh
# Build and run test suite
make NATIVE=1 CC=gcc test

# Test output (TAP format)
build/test_runner
```

## Configuration

Runtime configuration is read from (in priority order):
1. Environment variables (e.g., `RECOVERY_TOUCHPAD=/dev/input/event1`)
2. `/etc/system-recovery/config.ini`
3. `./config/default_config.ini`
4. Hard-coded defaults

## Key Improvements over Legacy

| Area | Legacy | New |
|------|--------|-----|
| Architecture | Monolithic, mixed concerns | 4-layer modular |
| Operations | Duplicated code (3 copies) | Plugin system (1 per op) |
| Configuration | Compile-time #defines | Runtime INI + env vars |
| Input handling | 1557-line monolithic file | Per-device-type modules |
| Testing | 0 tests | 22 tests (all passing) |
| Screen registration | Hardcoded array | Interface-based |
| Extensibility | Edit existing code | Add plugin file |

## License

Proprietary — Internal use only.
