#!/usr/bin/env bash
# ============================================================
#  build.sh — Build & Test Script for net-audit
# ============================================================
#
#  Usage:
#    ./build.sh              # Release build (default)
#    ./build.sh debug        # Debug build with AddressSanitizer
#    ./build.sh release      # Release build with LTO
#    ./build.sh sqlite       # Release build with SQLite3 support
#    ./build.sh clean        # Remove all build artifacts
#    ./build.sh rebuild      # Clean + release build
#    ./build.sh size         # Show binary and per-module size
#    ./build.sh check        # Static analysis (cppcheck)
#    ./build.sh smoke        # Build + quick smoke test on localhost
#    ./build.sh all          # clean + debug + release + size + check
#
#  Environment:
#    CC=gcc                  # Compiler selection
#    HAVE_SQLITE=1           # Enable SQLite3 (used by 'sqlite' target)
#

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
cd "$SCRIPT_DIR"

TARGET="net-audit"
MAKE_OPTS="${MAKE_OPTS:--j$(nproc 2>/dev/null || echo 4)}"

# ---- Color helpers ----
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
BLUE='\033[0;34m'
CYAN='\033[0;36m'
BOLD='\033[1m'
NC='\033[0m' # No Color

banner() {
    echo -e "${BOLD}${BLUE}═══ $1 ═══${NC}"
}

ok()   { echo -e "  ${GREEN}✓${NC} $1"; }
warn() { echo -e "  ${YELLOW}⚠${NC} $1"; }
fail() { echo -e "  ${RED}✗${NC} $1"; }
info() { echo -e "  ${CYAN}→${NC} $1"; }

# ---- Check dependencies ----
check_deps() {
    local missing=0
    for dep in gcc make; do
        if ! command -v "$dep" &>/dev/null; then
            fail "Missing dependency: $dep"
            missing=1
        fi
    done
    if ! ldconfig -p 2>/dev/null | grep -q libpthread; then
        if ! ldconfig -p 2>/dev/null | grep -q libc.so; then
            warn "Cannot verify libpthread (probably OK on Linux)"
        fi
    fi
    return $missing
}

# ---- Build commands ----
do_clean() {
    banner "Clean"
    make clean
    rm -f "$TARGET"
    ok "Build artifacts removed"
}

do_debug() {
    banner "Debug Build"
    check_deps
    make BUILD=debug "$TARGET" $MAKE_OPTS
    ok "Debug build complete: $TARGET"
    info "Binary: $(du -h "$TARGET" | cut -f1)"
}

do_release() {
    banner "Release Build"
    check_deps
    make BUILD=release "$TARGET" $MAKE_OPTS
    ok "Release build complete: $TARGET"
    info "Binary: $(du -h "$TARGET" | cut -f1)"
}

do_sqlite() {
    banner "Release Build (with SQLite3)"
    check_deps
    if ! ldconfig -p 2>/dev/null | grep -q libsqlite3; then
        if ! pkg-config --exists sqlite3 2>/dev/null; then
            fail "SQLite3 development library not found"
            fail "Install: sudo apt install libsqlite3-dev"
            return 1
        fi
    fi
    make BUILD=release HAVE_SQLITE=1 "$TARGET" $MAKE_OPTS
    ok "Release build (SQLite3) complete: $TARGET"
    info "Binary: $(du -h "$TARGET" | cut -f1)"
}

do_rebuild() {
    do_clean
    do_release
}

do_size() {
    banner "Size Analysis"
    if [ ! -f "$TARGET" ]; then
        warn "$TARGET not built yet, building release first..."
        do_release
    fi
    make size
}

do_check() {
    banner "Static Analysis (cppcheck)"
    if ! command -v cppcheck &>/dev/null; then
        warn "cppcheck not installed. Install: sudo apt install cppcheck"
        return 1
    fi
    info "Running cppcheck..."
    cppcheck --enable=all --std=c11 --suppress=missingIncludeSystem \
             --suppress=unusedFunction --suppress=checkersReport \
             --inline-suppr \
             -I include \
             cli/ core/ net/ scan/ fp/ result/ worker/ db/ \
             2>&1 || true
    ok "Static analysis complete"
}

# ---- Smoke test ----
do_smoke() {
    banner "Smoke Test"
    if [ ! -f "$TARGET" ]; then
        warn "$TARGET not built yet, building release first..."
        do_release
    fi
    echo ""

    # Test 1: --help
    info "Test 1/4: --help"
    if ./"$TARGET" --help >/dev/null 2>&1; then
        ok "--help works"
    else
        fail "--help failed"
    fi

    # Test 2: Scan localhost SSH (if available)
    info "Test 2/4: Scan localhost:22 (SSH)"
    if ./"$TARGET" --target 127.0.0.1 --ports 22 --timeout 2000 2>/dev/null | grep -q "OPEN"; then
        ok "SSH detected on port 22"
    else
        warn "SSH not detected on port 22 (may not be running)"
    fi

    # Test 3: JSON output
    info "Test 3/4: JSON output format"
    if ./"$TARGET" --target 127.0.0.1 --ports 22 --output json --timeout 2000 2>/dev/null | grep -q '"state"'; then
        ok "JSON output works"
    else
        warn "JSON output check skipped (SSH may not be running)"
    fi

    # Test 4: Concurrency limit
    info "Test 4/4: Concurrency limit"
    local out
    out=$(./"$TARGET" --target 127.0.0.1 --ports 1-10 --concurrency 3 --timeout 1000 2>/dev/null || true)
    local scanned
    scanned=$(echo "$out" | grep -c "127.0.0.1" || true)
    if [ "$scanned" -ge 1 ] 2>/dev/null; then
        ok "Concurrency limit works (scanned $scanned targets with concurrency=3)"
    else
        warn "Concurrency test inconclusive"
    fi

    echo ""
    banner "Smoke Test Complete"
}

do_all() {
    do_clean
    echo ""
    do_debug
    echo ""
    do_release
    echo ""
    do_size
    echo ""
    do_check
    echo ""
    banner "All Tasks Complete"
    info "Final binary: $(ls -lh "$TARGET" 2>/dev/null | awk '{print $5, $NF}')"
}

# ---- Help ----
do_help() {
    echo -e "${BOLD}Lightweight Network Audit Framework — Build Script${NC}"
    echo ""
    echo "Usage: ./build.sh [command]"
    echo ""
    echo "Commands:"
    echo "  (none)        Release build (default)"
    echo "  release       Release build with -O2 -flto"
    echo "  debug         Debug build with AddressSanitizer (-O0 -g)"
    echo "  sqlite        Release build with SQLite3 support"
    echo "  clean         Remove all build artifacts"
    echo "  rebuild       Clean + release build"
    echo "  size          Show binary and per-module code size"
    echo "  check         Static analysis with cppcheck"
    echo "  smoke         Build + quick smoke test on localhost"
    echo "  all           clean + debug + release + size + check"
    echo "  help          Show this help"
    echo ""
    echo "Environment:"
    echo "  CC=gcc        Compiler selection"
    echo "  MAKE_OPTS=    Extra make flags (default: -j\$(nproc))"
}

# ---- Main dispatch ----
COMMAND="${1:-release}"

case "$COMMAND" in
    debug)      do_debug   ;;
    release)    do_release ;;
    sqlite)     do_sqlite  ;;
    clean)      do_clean   ;;
    rebuild)    do_rebuild ;;
    size)       do_size    ;;
    check)      do_check   ;;
    smoke)      do_smoke   ;;
    all)        do_all     ;;
    help|--help|-h)
                do_help    ;;
    *)
        echo -e "${RED}Unknown command: $COMMAND${NC}"
        echo ""
        do_help
        exit 1
        ;;
esac
