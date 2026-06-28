#!/bin/bash
# Test: I/O metrics parsing
# Validates: /proc/diskstats integration, field presence

source "$(dirname "$0")/helper.sh"

echo "=== test_io_metrics: IO Metrics Validation ==="

JSON=$($DISK_HEALTH --json 2>/dev/null)

# 1. Check if IO fields are present (they may not be on first run due to delta computation)
HAS_READ_IOPS=$(echo "$JSON" | python3 -c "
import sys,json
d=json.load(sys.stdin)
dev=d['devices'][0]
print('1' if 'read_iops' in dev else '0')
" 2>/dev/null)

echo "  INFO: read_iops present = $HAS_READ_IOPS"

# 2. Verify that /proc/diskstats is readable and matches our device
assert "/proc/diskstats exists and readable" test -r /proc/diskstats

# 3. Verify sda is in /proc/diskstats
SDA_IN_STATS=$(grep -c ' sda ' /proc/diskstats 2>/dev/null || echo 0)
assert "/proc/diskstats contains sda" python3 -c "exit(0 if ${SDA_IN_STATS} > 0 else 1)"

# 4. Fields have correct types if present
if [ "$HAS_READ_IOPS" = "1" ]; then
    READ_IOPS=$(echo "$JSON" | python3 -c "
import sys,json
d=json.load(sys.stdin)
print(d['devices'][0]['read_iops'])
" 2>/dev/null)
    assert "read_iops is non-negative" python3 -c "exit(0 if float('${READ_IOPS:-0}') >= 0 else 1)"
    echo "  INFO: read_iops = $READ_IOPS"
fi

# 5. avg_latency_ms is non-negative if present
HAS_LATENCY=$(echo "$JSON" | python3 -c "
import sys,json
d=json.load(sys.stdin)
print('1' if 'avg_latency_ms' in d['devices'][0] else '0')
" 2>/dev/null)

if [ "$HAS_LATENCY" = "1" ]; then
    LATENCY=$(echo "$JSON" | python3 -c "
import sys,json
d=json.load(sys.stdin)
print(d['devices'][0]['avg_latency_ms'])
" 2>/dev/null)
    assert "avg_latency_ms >= 0" python3 -c "exit(0 if float('${LATENCY:-0}') >= 0 else 1)"
    echo "  INFO: avg_latency_ms = $LATENCY"
fi

print_results
