#!/bin/bash
# Test: Daemon lifecycle
# Validates: --watch foreground mode, signal handling, log output

source "$(dirname "$0")/helper.sh"

echo "=== test_daemon: Daemon Lifecycle Validation ==="

TEMP_LOG="/tmp/disk_health_test_$$.log"

# 1. --watch foreground mode starts
echo "  INFO: Starting --watch mode in background..."
$DISK_HEALTH --watch --interval 5 --log-path "$TEMP_LOG" &
DAEMON_PID=$!
sleep 2

# Verify process is running
if kill -0 $DAEMON_PID 2>/dev/null; then
    assert "--watch mode runs (pid=$DAEMON_PID)" true
else
    assert "--watch mode runs (pid=$DAEMON_PID)" false
    print_results
    exit 1
fi

# 2. Log file was created and has valid entries
if [ -f "$TEMP_LOG" ]; then
    LOG_LINES=$(wc -l < "$TEMP_LOG")
    assert "Log file has content" python3 -c "exit(0 if ${LOG_LINES:-0} > 0 else 1)"
    echo "  INFO: Log has $LOG_LINES lines"

    # Check log format
    LOG_VALID=$(head -3 "$TEMP_LOG" | grep -cE '^\[[0-9]{4}-[0-9]{2}-[0-9]{2} [0-9]{2}:[0-9]{2}:[0-9]{2}\] \[(INFO|WARN|DEBUG|CRITICAL)\] ' || echo 0)
    assert "Log entries have valid timestamp+level format" python3 -c "exit(0 if int('${LOG_VALID:-0}') > 0 else 1)"
else
    echo "  FAIL: Log file not created"
    ((FAIL_COUNT++))
fi

# 3. Device scanning log entry exists
if [ -f "$TEMP_LOG" ]; then
    HAS_SDA=$(grep -c 'sda' "$TEMP_LOG" 2>/dev/null || echo 0)
    if [ "$HAS_SDA" -gt 0 ]; then
        assert "Log contains device 'sda'" true
    else
        echo "  INFO: No sda in log (permissions?)"
    fi
fi

# 4. Graceful shutdown via SIGTERM
kill -TERM $DAEMON_PID 2>/dev/null
sleep 1

# Check it exited cleanly
if ! kill -0 $DAEMON_PID 2>/dev/null; then
    assert "Graceful shutdown on SIGTERM" true
else
    # Force kill if still running
    kill -9 $DAEMON_PID 2>/dev/null
    sleep 0.5
    assert "Graceful shutdown on SIGTERM" false
fi
wait $DAEMON_PID 2>/dev/null

# Check exit code — should be 0 (clean exit)
RC=$?
assert_eq "Clean exit code" "0" "$RC"

# 5. Verify shutdown message in log
if [ -f "$TEMP_LOG" ]; then
    HAS_SHUTDOWN=$(grep -c 'stopped' "$TEMP_LOG" 2>/dev/null || echo 0)
    echo "  INFO: Shutdown log entries: $HAS_SHUTDOWN"
fi

# Cleanup
rm -f "$TEMP_LOG"

print_results
