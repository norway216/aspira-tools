#!/bin/bash
# ============================================================================
# run_tests.sh
# Build and run the embedded Linux installer test suite.
#
# Usage:
#   ./run_tests.sh [OPTIONS]
#
# Options:
#   --build-dir PATH     CMake build directory (default: build)
#   --type TYPE          CMake build type: Debug|Release|RelWithDebInfo
#   --filter PATTERN     Run only tests matching the given pattern (gtest_filter)
#   --no-build           Skip the build step (run previously built tests)
#   --verbose            Show verbose test output
#   --help               Show this help message
# ============================================================================

set -euo pipefail

BUILD_DIR="build"
BUILD_TYPE="Debug"
FILTER=""
NO_BUILD=false
VERBOSE=false
PROJECT_DIR="$(cd "$(dirname "$0")/.." && pwd)"

print_usage() {
    cat << EOF
Usage: $0 [OPTIONS]

Build and run the embedded Linux installer test suite.

Options:
  --build-dir PATH     CMake build directory (default: build)
  --type TYPE          CMake build type (default: Debug)
  --filter PATTERN     Run only tests matching pattern (gtest_filter)
  --no-build           Skip build step
  --verbose            Show verbose test output
  --help               Show this help message

Examples:
  $0                                    # Build and run all tests
  $0 --filter "DeviceManager*"          # Run device manager tests only
  $0 --no-build --verbose               # Run existing build with verbose output
  $0 --type Release                      # Build in Release mode
EOF
}

while [[ $# -gt 0 ]]; do
    case "$1" in
        --build-dir)
            BUILD_DIR="$2"
            shift 2
            ;;
        --type)
            BUILD_TYPE="$2"
            shift 2
            ;;
        --filter)
            FILTER="$2"
            shift 2
            ;;
        --no-build)
            NO_BUILD=true
            shift
            ;;
        --verbose)
            VERBOSE=true
            shift
            ;;
        --help|-h)
            print_usage
            exit 0
            ;;
        *)
            echo "Error: Unknown option: $1"
            print_usage
            exit 1
            ;;
    esac
done

echo "========================================"
echo "  Installer Test Suite"
echo "========================================"
echo "  Project:    $PROJECT_DIR"
echo "  Build dir:  $BUILD_DIR"
echo "  Build type: $BUILD_TYPE"
echo "========================================"

# ---- Build ----
if ! $NO_BUILD; then
    echo ""
    echo "[1/3] Configuring CMake..."
    mkdir -p "$PROJECT_DIR/$BUILD_DIR"
    cd "$PROJECT_DIR/$BUILD_DIR"

    cmake "$PROJECT_DIR" \
        -DCMAKE_BUILD_TYPE="$BUILD_TYPE" \
        -DBUILD_CORE_SERVICE=ON \
        -DBUILD_CLI=ON \
        -DBUILD_TESTS=ON \
        -DENABLE_SANITIZERS=OFF \
        -DENABLE_FAULT_INJECT=ON

    echo ""
    echo "[2/3] Building..."
    cmake --build . --target all -j"$(nproc 2>/dev/null || echo 4)"
else
    echo ""
    echo "[1/3] Skipping build (--no-build)"
    echo "[2/3] Skipping build"
    cd "$PROJECT_DIR/$BUILD_DIR"
fi

# ---- Run tests ----
echo ""
echo "[3/3] Running tests..."

TEST_ARGS=""
if $VERBOSE; then
    TEST_ARGS="$TEST_ARGS --gtest_print_time=1"
fi
if [[ -n "$FILTER" ]]; then
    TEST_ARGS="$TEST_ARGS --gtest_filter=$FILTER"
fi

# Run CTest
cd "$PROJECT_DIR/$BUILD_DIR"

if command -v ctest &>/dev/null; then
    if $VERBOSE; then
        ctest --output-on-failure --test-dir . $TEST_ARGS
    else
        ctest --output-on-failure --test-dir . $TEST_ARGS
    fi
    CTEST_EXIT_CODE=$?
else
    # Fallback: run test binaries directly
    echo "ctest not found, running test binaries directly..."
    TEST_EXIT_CODE=0
    for test_bin in tests/test_*; do
        if [[ -x "$test_bin" ]]; then
            echo ""
            echo "--- Running: $test_bin ---"
            if $VERBOSE; then
                "$test_bin" $TEST_ARGS || TEST_EXIT_CODE=1
            else
                "$test_bin" $TEST_ARGS || TEST_EXIT_CODE=1
            fi
        fi
    done
    CTEST_EXIT_CODE=$TEST_EXIT_CODE
fi

# ---- Summary ----
echo ""
echo "========================================"
if [[ $CTEST_EXIT_CODE -eq 0 ]]; then
    echo "  All tests PASSED"
else
    echo "  Some tests FAILED (exit code: $CTEST_EXIT_CODE)"
fi
echo "========================================"

exit $CTEST_EXIT_CODE
