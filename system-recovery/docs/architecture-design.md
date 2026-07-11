# System Recovery - Architecture Design Document

## 1. Overview

### 1.1 Purpose
System Recovery is an embedded Linux application that provides system recovery, system installation, software backup/restore functionality through a graphical user interface. It runs on ARM64 platforms using the Linux framebuffer for display and LVGL for UI rendering.

### 1.2 Design Goals
- **Modularity**: Clear separation of concerns with well-defined module boundaries
- **Extensibility**: Plugin-based architecture for recovery operations
- **Maintainability**: Clean, readable code with consistent patterns
- **Testability**: Each module can be independently tested
- **Configurability**: Runtime configuration instead of compile-time defines
- **Portability**: Hardware abstraction layer for easy platform migration

## 2. Architecture Overview

### 2.1 Layer Architecture

```
┌─────────────────────────────────────────────────────────┐
│                   Application Layer                      │
│  ┌─────────────┐  ┌──────────────┐  ┌───────────────┐  │
│  │  App Core   │  │  UI Manager  │  │  Event Bus    │  │
│  └─────────────┘  └──────────────┘  └───────────────┘  │
├─────────────────────────────────────────────────────────┤
│                   Service Layer                          │
│  ┌───────────┐  ┌────────────┐  ┌──────────────────┐   │
│  │ Recovery  │  │  Install   │  │  Backup/Restore  │   │
│  │ Service   │  │  Service   │  │  Service          │   │
│  └───────────┘  └────────────┘  └──────────────────┘   │
│  ┌───────────┐  ┌────────────┐  ┌──────────────────┐   │
│  │  System   │  │   Log      │  │  Config          │   │
│  │  Ops      │  │  Service   │  │  Manager         │   │
│  └───────────┘  └────────────┘  └──────────────────┘   │
├─────────────────────────────────────────────────────────┤
│               Hardware Abstraction Layer (HAL)           │
│  ┌───────────┐  ┌────────────┐  ┌──────────────────┐   │
│  │ Display   │  │   Input    │  │  Filesystem      │   │
│  │ Driver    │  │  Manager   │  │  Manager         │   │
│  └───────────┘  └────────────┘  └──────────────────┘   │
├─────────────────────────────────────────────────────────┤
│                    Platform Layer                        │
│  ┌───────────┐  ┌────────────┐  ┌──────────────────┐   │
│  │  Linux    │  │  LVGL      │  │  POSIX APIs      │   │
│  │ Framebuf  │  │  Library   │  │  (pthread,etc)   │   │
│  └───────────┘  └────────────┘  └──────────────────┘   │
└─────────────────────────────────────────────────────────┘
```

### 2.2 Module Dependency Diagram

```
main.c
  └── app_core (application lifecycle, event loop)
        ├── event_bus (pub/sub event system)
        ├── ui_manager (screen management, navigation)
        │     ├── ui_screens/ (individual screen modules)
        │     └── ui_widgets/ (reusable widgets)
        ├── service_manager (service lifecycle)
        │     ├── recovery_service
        │     ├── install_service
        │     └── backup_service
        ├── config_manager (configuration loading)
        ├── log_service (logging)
        └── hal/
              ├── display (framebuffer abstraction)
              ├── input (multi-device input)
              └── storage (mount/partition operations)
```

## 3. Component Design

### 3.1 Application Core (`src/core/`)

The App Core manages the application lifecycle: initialization, main event loop, and cleanup.

```
src/core/
├── app_core.h          - Application lifecycle API
├── app_core.c          - Main loop, init sequence, shutdown
├── event_bus.h         - Event types and subscriber API
└── event_bus.c         - Publish/subscribe event system
```

**Event Types:**
- `EVENT_SCREEN_CHANGE` - Screen navigation events
- `EVENT_OPERATION_START` - Recovery/install/backup operation started
- `EVENT_OPERATION_PROGRESS` - Operation progress update
- `EVENT_OPERATION_COMPLETE` - Operation completed (success/failure)
- `EVENT_INPUT_KEY` - Hardware key events (Grape keyboard)
- `EVENT_SYSTEM_SHUTDOWN` - Shutdown/reboot request

### 3.2 UI Manager (`src/ui/`)

Handles screen creation, navigation, and the screen lifecycle.

```
src/ui/
├── ui_manager.h        - Screen management API
├── ui_manager.c        - Screen registry, navigation, lifecycle
├── ui_widgets/
│   ├── ui_button.h/c   - Styled button widget
│   ├── ui_dialog.h/c   - Confirmation/info dialog widget
│   ├── ui_progress.h/c - Progress bar widget
│   └── ui_label.h/c    - Styled label widget
└── ui_screens/
    ├── screen_main.h/c       - Main menu screen (Recovery/Install)
    ├── screen_recovery.h/c   - Recovery selection screen
    ├── screen_install.h/c    - Install selection screen
    ├── screen_backup.h/c     - Backup screen
    ├── screen_progress.h/c   - Operation progress screen
    └── screen_notify.h/c     - Notification/result screen
```

**Screen State Machine:**
```
[Boot] → SCREEN_MAIN → SCREEN_RECOVERY → SCREEN_PROGRESS → SCREEN_NOTIFY
                     → SCREEN_INSTALL  → SCREEN_PROGRESS → SCREEN_NOTIFY
                     → SCREEN_BACKUP   → SCREEN_PROGRESS → SCREEN_NOTIFY
```

### 3.3 Service Layer (`src/services/`)

Business logic for recovery operations, separated from UI.

```
src/services/
├── service_manager.h   - Service registry and lifecycle
├── service_manager.c
├── recovery_service.h  - System recovery operations
├── recovery_service.c
├── install_service.h   - System installation operations
├── install_service.c
├── backup_service.h    - Software backup/restore operations
├── backup_service.c
├── operations/              - Pluggable operation modules
│   ├── op_interface.h       - Operation plugin interface
│   ├── op_light_recovery.c  - Lightweight recovery
│   ├── op_deep_recovery.c   - Deep/full recovery
│   ├── op_app_recovery.c    - Application recovery
│   ├── op_light_install.c   - Lightweight install
│   ├── op_deep_install.c    - Deep/full install
│   └── op_app_backup.c      - Application backup
├── log_service.h       - Logging API
├── log_service.c
├── config_manager.h    - Configuration API
└── config_manager.c
```

**Operation Plugin Interface:**
```c
typedef struct operation_plugin {
    const char *name;
    const char *description;
    int (*init)(void *config);
    int (*execute)(progress_callback_t progress, void *ctx);
    int (*validate)(void);   // Pre-flight check
    void (*cleanup)(void);
} operation_plugin_t;
```

### 3.4 Hardware Abstraction Layer (`src/hal/`)

```
src/hal/
├── display/
│   ├── display.h       - Display driver interface
│   ├── display_fb.c    - Linux framebuffer implementation
│   └── display_dummy.c - Dummy display for testing
├── input/
│   ├── input_manager.h - Multi-device input abstraction
│   ├── input_manager.c
│   ├── input_touchpad.c - Touchpad device handler
│   ├── input_touchscreen.c - Touchscreen handler
│   ├── input_mouse.c    - Mouse device handler
│   ├── input_keyboard.c - Keyboard device handler
│   └── input_grape.c    - Grape custom HID handler
└── storage/
    ├── storage.h        - Filesystem operations API
    └── storage.c        - Mount/umount/partition operations
```

### 3.5 Configuration System

Replace compile-time defines with a runtime configuration file:

```ini
# /etc/system-recovery/config.ini
[display]
width = 1920
height = 1080

[input]
touchpad_sensitivity = 100
double_click_ms = 450
debug = false

[storage]
recovery_partition = /dev/mmcblk0p2
data_partition = /dev/mmcblk0p7
root_a_partition = /dev/mmcblk0p5
root_b_partition = /dev/mmcblk0p6

[recovery]
timeout_minutes = 20
verify_checksum = true

[paths]
update_script = /scripts/init-bottom/zz-jelina_flasher
vars_file = /tmp/flash_vars.conf
log_dir = /tmp
```

## 4. Directory Structure

```
system-recovery/
├── docs/
│   ├── architecture-design.md     - This document
│   ├── api-reference.md           - API documentation
│   └── test-reports/              - Test reports
├── src/
│   ├── main.c                     - Entry point
│   ├── core/
│   │   ├── app_core.h / .c
│   │   └── event_bus.h / .c
│   ├── ui/
│   │   ├── ui_manager.h / .c
│   │   ├── ui_widgets/
│   │   └── ui_screens/
│   ├── services/
│   │   ├── service_manager.h / .c
│   │   ├── recovery_service.h / .c
│   │   ├── install_service.h / .c
│   │   ├── backup_service.h / .c
│   │   ├── operations/
│   │   ├── log_service.h / .c
│   │   └── config_manager.h / .c
│   ├── hal/
│   │   ├── display/
│   │   ├── input/
│   │   └── storage/
│   └── common/
│       ├── types.h                - Common type definitions
│       ├── utils.h / .c           - Utility functions
│       └── version.h              - Build version info
├── config/
│   └── default_config.ini         - Default configuration
├── tests/
│   ├── unit/                      - Unit tests
│   ├── integration/               - Integration tests
│   └── system/                    - System tests
├── scripts/
│   └── build.sh                   - Build script
├── lvgl/                          - LVGL library (git submodule)
├── lv_drivers/                    - LVGL drivers (git submodule)
├── lv_conf.h                      - LVGL configuration
├── lv_drv_conf.h                  - LVGL driver configuration
├── Makefile                       - Build system
└── README.md
```

## 5. Key Design Patterns

### 5.1 Observer/Event Bus Pattern
All inter-module communication uses events. Modules subscribe to events they care about and publish events when state changes. This decouples UI from business logic.

### 5.2 Strategy Pattern (Operation Plugins)
Recovery/install/backup operations implement a common interface, allowing new operation types to be added without modifying existing code.

### 5.3 Singleton Service Manager
Services are managed by a central registry that handles initialization order and dependencies.

### 5.4 State Machine for UI Navigation
Screen transitions are governed by an explicit state machine, making navigation flow clear and testable.

## 6. Data Flow

### 6.1 Recovery Operation Flow

```
User clicks "Recover System"
  → ui_manager publishes EVENT_OPERATION_START
  → recovery_service receives event, selects appropriate plugin
  → recovery_service calls plugin->validate() (pre-flight checks)
  → recovery_service spawns worker thread
  → worker thread calls plugin->execute(progress_callback)
  → progress_callback publishes EVENT_OPERATION_PROGRESS
  → ui_manager updates progress bar on EVENT_OPERATION_PROGRESS
  → worker thread completes
  → recovery_service publishes EVENT_OPERATION_COMPLETE {success, message}
  → ui_manager navigates to notify screen
```

### 6.2 Input Event Flow

```
Linux input device → input_manager polls fd
  → input_manager classifies device (touchpad/touchscreen/mouse/keyboard/grape)
  → input_manager processes raw events into unified cursor/key events
  → LVGL indev driver reads from input_manager
  → LVGL dispatches to active screen widgets
  → (or) grape keys → grape_key_handler → event_bus publishes EVENT_INPUT_KEY
```

## 7. Migration Path from Legacy Code

| Legacy Component | New Component | Key Improvements |
|-----------------|---------------|------------------|
| main.c (210 lines mixed) | main.c + app_core.c | Clean init sequence, event loop |
| screen_manager.c (hardcoded) | ui_manager.c | Dynamic screen registration, state machine |
| ui_recovery_screen.c (1800+ lines) | screen_recovery.c + recovery_service.c | UI/business logic separation |
| recovery_process.c (720 lines, duplicate) | operations/*.c plugins | No duplication, pluggable |
| ext_input.c (1557 lines) | input/*.c split modules | One file per device type |
| ext_grape_hid.c | input/input_grape.c | Integrated into input manager |
| ext_keyboard.c | input/input_keyboard.c | Integrated into input manager |
| common.c (mixed utils) | utils.c + log_service.c | Separated concerns |
| compile-time defines | config_manager.c + config.ini | Runtime configuration |
| No tests | tests/ directory | Unit, integration, system tests |

## 8. Build System

The project uses a Makefile-based build system, cross-compiling for aarch64:

- Cross-compiler: `aarch64-linux-gnu-gcc`
- Build targets: `all`, `clean`, `install`, `uninstall`, `test`
- Test framework: Unity (lightweight C test framework)
- Output: `build/bin/system-recovery`

## 9. Testing Strategy

### 9.1 Unit Tests
- Each service module tested in isolation
- Mock HAL layer for platform-independent testing
- Operation plugins tested with mock filesystem

### 9.2 Integration Tests
- UI Manager + Screen navigation
- Service Manager + Operation plugins
- Input Manager + device handlers
- Configuration loading and parsing

### 9.3 System Tests
- Full application startup and shutdown
- Screen navigation end-to-end
- Recovery operation simulation
- Input device simulation
- Error handling and timeout scenarios
