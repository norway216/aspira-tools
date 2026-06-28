#!/bin/bash
# Common test functions for disk_health test suite

PASS_COUNT=0
FAIL_COUNT=0

# Resolve the disk_health binary path
if [ -f "./build/disk_health" ]; then
    DISK_HEALTH="./build/disk_health"
elif [ -f "../build/disk_health" ]; then
    DISK_HEALTH="../build/disk_health"
elif [ -f "./disk_health" ]; then
    DISK_HEALTH="./disk_health"
else
    echo "ERROR: cannot find disk_health binary"
    exit 1
fi

assert() {
    local desc="$1"
    shift
    if "$@" 2>/dev/null; then
        echo "  PASS: $desc"
        ((PASS_COUNT++))
    else
        echo "  FAIL: $desc"
        ((FAIL_COUNT++))
    fi
}

assert_eq() {
    local desc="$1"
    local expected="$2"
    local actual="$3"
    if [ "$expected" = "$actual" ]; then
        echo "  PASS: $desc (expected='$expected')"
        ((PASS_COUNT++))
    else
        echo "  FAIL: $desc (expected='$expected', actual='$actual')"
        ((FAIL_COUNT++))
    fi
}

assert_not_empty() {
    local desc="$1"
    local actual="$2"
    if [ -n "$actual" ]; then
        echo "  PASS: $desc (non-empty)"
        ((PASS_COUNT++))
    else
        echo "  FAIL: $desc (empty)"
        ((FAIL_COUNT++))
    fi
}

print_results() {
    echo "---"
    echo "Results: $PASS_COUNT passed, $FAIL_COUNT failed"
    if [ "$FAIL_COUNT" -eq 0 ]; then
        echo "ALL TESTS PASSED"
        return 0
    else
        echo "SOME TESTS FAILED"
        return 1
    fi
}
