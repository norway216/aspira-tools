# ============================================================
#  Lightweight Network Audit Framework V1 — Makefile
# ============================================================
#
#  Targets:
#    make [release]    — Optimised build (-O2 -flto)
#    make debug        — Debug build with address sanitizer
#    make test         — Build release and run test scripts
#    make clean        — Remove build artifacts
#    make size         — Show code size breakdown
#
#  Feature toggles:
#    HAVE_SQLITE=1     — Enable SQLite3 persistence layer
#    BUILD=debug       — Debug build (default: release)
#

CC              ?= gcc
BUILD           ?= release
HAVE_SQLITE     ?= 0

# ---- Compiler flags ----
CFLAGS_COMMON   := -std=c11 -Wall -Wextra -Wpedantic \
                   -Werror=implicit-function-declaration \
                   -Wshadow -Wconversion -Wunreachable-code \
                   -Wno-sign-conversion

# Debug: no optimisation, address sanitizer, debug symbols
CFLAGS_DEBUG    := -O0 -g -DDEBUG -fsanitize=address -fno-omit-frame-pointer
LDFLAGS_DEBUG   := -fsanitize=address

# Release: full optimisation, LTO
CFLAGS_RELEASE  := -O2 -DNDEBUG -flto
LDFLAGS_RELEASE := -flto

# ---- Derived flags ----
ifeq ($(BUILD),debug)
    CFLAGS  := $(CFLAGS_COMMON) $(CFLAGS_DEBUG) -Iinclude
    LDFLAGS := $(LDFLAGS_DEBUG)
else
    CFLAGS  := $(CFLAGS_COMMON) $(CFLAGS_RELEASE) -Iinclude
    LDFLAGS := $(LDFLAGS_RELEASE)
endif

LDLIBS := -lpthread -lrt

# ---- Optional SQLite3 ----
ifeq ($(HAVE_SQLITE),1)
    CFLAGS  += -DHAVE_SQLITE3
    LDLIBS  += -lsqlite3
endif

# ---- Source files (relative to this Makefile) ----
SRCS := \
    cli/main.c        cli/config.c   \
    core/reactor.c    core/timer_wheel.c \
    net/socket.c                      \
    scan/scanner.c                    \
    fp/fingerprint.c                  \
    result/queue.c                    \
    worker/pool.c                     \
    db/sqlite.c

OBJS := $(SRCS:.c=.o)
DEPS := $(SRCS:.c=.d)

TARGET := net-audit

# ============================================================
#  Phony targets
# ============================================================

.PHONY: all debug release clean test size help

all: $(TARGET)

debug:
	@$(MAKE) BUILD=debug $(TARGET)

release:
	@$(MAKE) BUILD=release $(TARGET)

help:
	@echo "Lightweight Network Audit Framework V1"
	@echo ""
	@echo "Usage:"
	@echo "  make [release]     Optimised build (default)"
	@echo "  make debug         Debug build with AddressSanitizer"
	@echo "  make test          Run test suite"
	@echo "  make clean         Remove build artifacts"
	@echo "  make size          Show per-module code size"
	@echo ""
	@echo "Options:"
	@echo "  HAVE_SQLITE=1      Enable SQLite3 support"
	@echo "  BUILD=debug        Build with -O0 -g -fsanitize=address"
	@echo "  CC=gcc             Compiler selection"

# ============================================================
#  Link
# ============================================================

$(TARGET): $(OBJS)
	$(CC) $(LDFLAGS) -o $@ $^ $(LDLIBS)
	@echo "  BUILD  $(TARGET)  ($(BUILD) mode)"

# ============================================================
#  Compile
# ============================================================

%.o: %.c
	$(CC) $(CFLAGS) -MMD -MP -c -o $@ $<

# ---- Auto-generated dependency files ----
-include $(DEPS)

# ============================================================
#  Code size analysis
# ============================================================

size: $(TARGET)
	@echo "=== Total binary size ==="
	@size $(TARGET)
	@echo ""
	@echo "=== Per-module size (bytes) ==="
	@for obj in $(OBJS); do \
	    if [ -f "$$obj" ]; then \
	        printf "  %-30s " "$$obj:"; \
	        size "$$obj" | awk 'NR==2 {printf "text=%-8s data=%-8s bss=%-8s total=%s\n", $$1, $$2, $$3, $$4}'; \
	    fi; \
	done

# ============================================================
#  Test
# ============================================================

TEST_SCRIPTS := \
    tests/test_queue.sh       \
    tests/test_timer.sh       \
    tests/test_fingerprint.sh \
    tests/test_scanner.sh

test: $(TARGET)
	@echo "=== Running test suite ==="
	@pass=0; fail=0; \
	for t in $(TEST_SCRIPTS); do \
	    if [ -f "$$t" ]; then \
	        printf "\n--- %s ---\n" "$$t"; \
	        bash "$$t" && pass=$$((pass+1)) || fail=$$((fail+1)); \
	    else \
	        echo "SKIP: $$t (not found)"; \
	    fi; \
	done; \
	echo ""; \
	echo "=== Results: $$pass passed, $$fail failed ==="; \
	[ $$fail -eq 0 ] || exit 1

# ============================================================
#  Clean
# ============================================================

clean:
	rm -f $(OBJS) $(DEPS) $(TARGET)
	rm -f *.gcda *.gcno *.gcov gmon.out
	@echo "  CLEAN  done"
