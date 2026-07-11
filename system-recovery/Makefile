#
# System Recovery – Makefile
#
# Cross-compiles for aarch64 Linux with LVGL.
#

# ---- Toolchain ----------------------------------------------------------

CC              ?= aarch64-linux-gnu-gcc
# For native test builds:  make CC=gcc NATIVE=1 test
ifdef NATIVE
  CC            := gcc
endif

# ---- Project Root -------------------------------------------------------

PRJ_ROOT        := $(shell pwd)
SRC_DIR         := src
BUILD_DIR       := build
OBJ_DIR         := $(BUILD_DIR)/obj
BIN_DIR         := $(BUILD_DIR)/bin
TEST_DIR        := tests

BIN             := system-recovery
TEST_BIN        := system-recovery-test

# ---- Flags --------------------------------------------------------------

WARNINGS        := -Wall -Wextra -Wshadow -Wundef \
                   -Wno-unused-parameter -Wno-missing-field-initializers \
                   -fno-strict-aliasing -std=gnu99

CFLAGS          := -O2 -g -I$(SRC_DIR) $(WARNINGS)
LDFLAGS         := -lm -lpthread -lrt

# Native builds don't link LVGL; use stubs and skip lvgl/lv_drivers sources.
ifdef NATIVE
  CFLAGS        += -DNATIVE_BUILD
else
  # Include LVGL library (bundled in-tree)
  LVGL_DIR      ?= $(PRJ_ROOT)
  LVGL_DIR_NAME ?= lvgl
  CFLAGS        += -I$(PRJ_ROOT)
  include $(PRJ_ROOT)/lvgl/lvgl.mk
  include $(PRJ_ROOT)/lv_drivers/lv_drivers.mk
  # Add project root to VPATH so relative/bare source paths resolve
  VPATH         += :$(PRJ_ROOT)
endif

# ---- Sources ------------------------------------------------------------

# Project sources under src/
SRCS            := $(SRC_DIR)/common/utils.c \
                   $(SRC_DIR)/common/version.c

# HAL
SRCS            += $(SRC_DIR)/hal/display/display_fb.c \
                   $(SRC_DIR)/hal/input/input_manager.c \
                   $(SRC_DIR)/hal/storage/storage.c

# Core
SRCS            += $(SRC_DIR)/core/event_bus.c
# app_core depends on UI/LVGL — excluded from NATIVE
ifndef NATIVE
SRCS            += $(SRC_DIR)/core/app_core.c
endif

# Services
SRCS            += $(SRC_DIR)/services/config_manager.c \
                   $(SRC_DIR)/services/log_service.c \
                   $(SRC_DIR)/services/service_manager.c \
                   $(SRC_DIR)/services/operations/op_interface.c \
                   $(SRC_DIR)/services/operations/op_light_recovery.c \
                   $(SRC_DIR)/services/operations/op_deep_recovery.c \
                   $(SRC_DIR)/services/operations/op_app_recovery.c \
                   $(SRC_DIR)/services/operations/op_deep_install.c \
                   $(SRC_DIR)/services/operations/op_light_install.c \
                   $(SRC_DIR)/services/operations/op_app_backup.c

# UI and Main — excluded from NATIVE builds (require LVGL headers)
ifndef NATIVE
SRCS            += $(SRC_DIR)/ui/ui_manager.c \
                   $(SRC_DIR)/ui/ui_screens/screen_main.c \
                   $(SRC_DIR)/ui/ui_screens/screen_recovery.c \
                   $(SRC_DIR)/ui/ui_screens/screen_progress.c \
                   $(SRC_DIR)/ui/ui_screens/screen_notify.c \
                   $(SRC_DIR)/main.c
endif

# LVGL resources (non-native only)
ifndef NATIVE
  SRCS          += mouse_cursor_icon.c
endif

# Test sources (native only)
TEST_SRCS       := $(TEST_DIR)/test_main.c \
                   $(TEST_DIR)/unit/test_event_bus.c \
                   $(TEST_DIR)/unit/test_config_manager.c \
                   $(TEST_DIR)/unit/test_operation_plugins.c \
                   $(TEST_DIR)/integration/test_screen_navigation.c \
                   $(TEST_DIR)/system/test_app_lifecycle.c

# ---- Objects ------------------------------------------------------------

# Project sources (under src/) → build/obj/<path>.o
OBJS            := $(patsubst $(SRC_DIR)/%.c, $(OBJ_DIR)/%.o, \
                       $(filter $(SRC_DIR)/%,$(SRCS)))

# Root-level sources (e.g. mouse_cursor_icon.c) → build/obj/<name>.o
_ROOT_SRCS      := $(filter-out $(SRC_DIR)/%,$(SRCS))
ROOT_OBJS       := $(addprefix $(OBJ_DIR)/,$(notdir $(_ROOT_SRCS:.c=.o)))

ifndef NATIVE
  # CSRCS from LVGL includes has two kinds of entries:
  #   1. Bare names like "lv_disp.c" — VPATH resolves them
  #   2. Absolute paths like "/path/lv_drivers/win_drv.c" — strip PRJ_ROOT
  _LVGL_SRCS     := $(filter %.c,$(CSRCS))
  _LVGL_SRCS_NORM:= $(foreach s,$(_LVGL_SRCS),\
      $(if $(filter $(PRJ_ROOT)/%,$(s)),\
          $(patsubst $(PRJ_ROOT)/%,%,$(s)),\
          $(s)))
  LVGL_OBJS      := $(addprefix $(OBJ_DIR)/,$(_LVGL_SRCS_NORM:.c=.o))
endif
TEST_OBJS       := $(patsubst $(TEST_DIR)/%.c, $(OBJ_DIR)/test/%.o, $(TEST_SRCS))

# ---- Rules --------------------------------------------------------------

.PHONY: all clean install uninstall test help

# Native 'all' only compiles objects (no LVGL UI to run).
# Cross-compile 'all' produces the full binary.
ifdef NATIVE
all: $(OBJS) $(ROOT_OBJS)
	@echo "Native objects built ($(words $(OBJS) $(ROOT_OBJS)) files)."
	@echo "Run 'make NATIVE=1 test' to execute the test suite."
else
all: $(BIN_DIR)/$(BIN)
endif

# Project sources under src/
$(OBJ_DIR)/%.o: $(SRC_DIR)/%.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -c $< -o $@
	@echo "  CC  $(notdir $<)"

# LVGL library sources (lvgl/, lv_drivers/) — resolved via VPATH
# Also handles root-level sources (mouse_cursor_icon.c)
$(OBJ_DIR)/%.o: %.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -c $< -o $@
	@echo "  CC  $(notdir $<)"

# Test objects
$(OBJ_DIR)/test/%.o: $(TEST_DIR)/%.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -I$(TEST_DIR) -c $< -o $@
	@echo "  CC  $(notdir $<)"

$(BIN_DIR)/$(BIN): $(OBJS) $(ROOT_OBJS) $(LVGL_OBJS)
	@mkdir -p $(BIN_DIR)
	$(CC) $(OBJS) $(ROOT_OBJS) $(LVGL_OBJS) $(LDFLAGS) -o $@
	@echo "  LD  $(BIN)"
	@echo "Build complete: $(BIN_DIR)/$(BIN)"

# ---- Test ---------------------------------------------------------------

$(BIN_DIR)/$(TEST_BIN): $(OBJS) $(ROOT_OBJS) $(TEST_OBJS)
	@mkdir -p $(BIN_DIR)
	$(CC) $(OBJS) $(ROOT_OBJS) $(TEST_OBJS) $(filter-out $(OBJ_DIR)/main.o,$(LVGL_OBJS)) $(LDFLAGS) -o $@
	@echo "  LD  $(TEST_BIN)"

test: $(BIN_DIR)/$(TEST_BIN)
	@echo "Running tests..."
	$(BIN_DIR)/$(TEST_BIN) 2>&1 | tee $(BUILD_DIR)/test_output.txt
	@echo "Test output saved to $(BUILD_DIR)/test_output.txt"

# ---- Utility ------------------------------------------------------------

clean:
	rm -rf $(BUILD_DIR)
	@# Remove stray .o files left from old LVGL in-tree builds
	@find $(PRJ_ROOT) -maxdepth 1 -name "*.o" -delete 2>/dev/null || true
	@echo "Cleaned build artifacts"

install:
	install -d $(DESTDIR)/usr/bin
	install $(BIN_DIR)/$(BIN) $(DESTDIR)/usr/bin/$(BIN)
	install -d $(DESTDIR)/etc/system-recovery
	install -m 644 config/default_config.ini $(DESTDIR)/etc/system-recovery/config.ini

uninstall:
	rm -f $(DESTDIR)/usr/bin/$(BIN)
	rm -rf $(DESTDIR)/etc/system-recovery

help:
	@echo "System Recovery – Build System"
	@echo ""
	@echo "Targets:"
	@echo "  all        Build the application (cross-compile for aarch64)"
	@echo "  clean      Remove build artifacts"
	@echo "  install    Install to DESTDIR"
	@echo "  uninstall  Remove from DESTDIR"
	@echo "  test       Build and run test suite (requires NATIVE=1)"
	@echo ""
	@echo "Variables:"
	@echo "  CC         Compiler (default: aarch64-linux-gnu-gcc)"
	@echo "  NATIVE=1   Build for host (testing only)"
	@echo "  DESTDIR    Installation root"
