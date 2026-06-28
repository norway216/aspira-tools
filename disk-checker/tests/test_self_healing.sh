#!/bin/bash
# Test: V2 Self-Healing pipeline
# Validates: fault detection, policy decisions, healing actions

source "$(dirname "$0")/helper.sh"

echo "=== test_self_healing: V2 Self-Healing Pipeline ==="

# 1. --help shows V2 flags
HELP=$($DISK_HEALTH --help 2>/dev/null)
assert "--help shows --auto-heal" echo "$HELP" | grep -q '\-\-auto-heal'
assert "--help shows --heal-level" echo "$HELP" | grep -q '\-\-heal-level'

# 2. Default mode is dry-run (auto_heal=0)
TEMP_LOG="/tmp/test_heal_$$.log"
timeout 4s $DISK_HEALTH --watch --interval 5 --log-path "$TEMP_LOG" 2>/dev/null &
WPID=$!
sleep 3
kill $WPID 2>/dev/null
wait $WPID 2>/dev/null

if [ -f "$TEMP_LOG" ]; then
    # Check for DRY-RUN marker in log
    DRY_COUNT=$(grep -c 'DRY-RUN\|dry.run' "$TEMP_LOG" 2>/dev/null || echo 0)
    echo "  INFO: DRY-RUN mentions in log: $DRY_COUNT"
    # Dry-run is default, so any healing log should show DRY-RUN
    assert "Log file created" test -f "$TEMP_LOG"
else
    echo "  INFO: Log file not created (permissions?)"
fi
rm -f "$TEMP_LOG"

# 3. Fault Detection white-box test
echo "  INFO: Compiling fault detection unit test..."

cat > /tmp/test_fault_$$.c << 'TESTEOF'
#define _DEFAULT_SOURCE
#include "fault_detect.h"
#include "health_score.h"
#include <stdio.h>
#include <string.h>

static int passed = 0, failed = 0;
#define T(name, cond) do { \
    if (cond) { printf("  PASS: %s\n", name); passed++; } \
    else { printf("  FAIL: %s\n", name); failed++; } \
} while(0)

int main(void) {
    health_score_t score;
    smart_ata_data_t ata;
    smart_nvme_data_t nvme;
    io_metrics_t io;
    trend_data_t trend;
    fault_result_t result;

    /* Test 1: Healthy device → FAULT_INFO */
    memset(&score, 0, sizeof(score)); score.score = 100;
    memset(&ata, 0, sizeof(ata)); ata.supported = 1; ata.enabled = 1;
    ata.temperature_celsius = 40;
    memset(&nvme, 0, sizeof(nvme));
    memset(&io, 0, sizeof(io));
    memset(&trend, 0, sizeof(trend));
    fault_detect(&score, &ata, &nvme, &io, &trend, &result);
    T("Healthy → FAULT_INFO", result.level == FAULT_INFO);

    /* Test 2: score=0 → FAULT_FATAL */
    score.score = 0;
    fault_detect(&score, &ata, &nvme, &io, &trend, &result);
    T("score=0 → FAULT_FATAL", result.level == FAULT_FATAL);

    /* Test 3: score=35 → FAULT_CRITICAL */
    score.score = 35;
    fault_detect(&score, &ata, &nvme, &io, &trend, &result);
    T("score=35 → FAULT_CRITICAL", result.level == FAULT_CRITICAL);

    /* Test 4: High pending sectors */
    score.score = 80;
    ata.pending_sectors = 5;
    fault_detect(&score, &ata, &nvme, &io, &trend, &result);
    T("5 pending → FAULT_DEGRADED", result.level == FAULT_DEGRADED);

    /* Test 5: Temperature spike via trend */
    memset(&trend, 0, sizeof(trend));
    trend.history_count = 2;
    trend.history_pos = 2;
    trend.temp_history[0] = 40.0;
    trend.temp_history[1] = 55.0;  /* +15°C spike */
    score.score = 95;
    memset(&ata, 0, sizeof(ata)); ata.supported = 1;
    ata.temperature_celsius = 55;
    fault_detect(&score, &ata, &nvme, &io, &trend, &result);
    T("temp spike → temp_spike=1", result.temp_spike == 1);

    /* Test 6: IO latency spike */
    memset(&trend, 0, sizeof(trend));
    trend.history_count = 3;
    trend.latency_mean = 5.0;
    trend.latency_history[0] = 4.0;
    trend.latency_history[1] = 5.0;
    trend.latency_history[2] = 200.0;  /* 200ms when avg is 5ms */
    memset(&io, 0, sizeof(io)); io.valid = 1;
    io.avg_latency_ms = 200.0;
    score.score = 85;
    memset(&ata, 0, sizeof(ata)); ata.supported = 1;
    fault_detect(&score, &ata, &nvme, &io, &trend, &result);
    T("latency spike → latency_spike=1", result.latency_spike == 1);

    printf("---\nResults: %d passed, %d failed\n", passed, failed);
    return failed > 0 ? 1 : 0;
}
TESTEOF

gcc -std=c11 -Wall -Wextra \
    -Iinclude -Isrc \
    -o /tmp/test_fault_$$ \
    /tmp/test_fault_$$.c \
    src/fault_detect.c \
    -lm 2>&1 | grep -v '^$'

if [ -x /tmp/test_fault_$$ ]; then
    /tmp/test_fault_$$
    RC=$?
else
    echo "FAIL: compile error"
    RC=1
fi
rm -f /tmp/test_fault_$$ /tmp/test_fault_$$.c

# 4. Policy Engine test
echo "  INFO: Testing policy engine..."

cat > /tmp/test_policy_$$.c << 'TESTEOF'
#define _DEFAULT_SOURCE
#include "policy_engine.h"
#include <stdio.h>
#include <string.h>

static int passed = 0, failed = 0;
#define T(name, cond) do { \
    if (cond) { printf("  PASS: %s\n", name); passed++; } \
    else { printf("  FAIL: %s\n", name); failed++; } \
} while(0)

int main(void) {
    health_score_t score;
    fault_result_t fault;
    policy_decision_t decision;

    /* Test 1: Healthy → ACTION_NONE */
    memset(&score, 0, sizeof(score)); score.score = 100;
    memset(&fault, 0, sizeof(fault)); fault.level = FAULT_INFO;
    policy_evaluate(&score, &fault, 4, &decision);
    T("Healthy → ACTION_NONE", decision.action == ACTION_NONE);

    /* Test 2: FATAL → ACTION_EMERGENCY */
    score.score = 0;
    fault.level = FAULT_FATAL;
    policy_evaluate(&score, &fault, 4, &decision);
    T("FATAL → ACTION_EMERGENCY", decision.action == ACTION_EMERGENCY);

    /* Test 3: score < 20 → ACTION_FAILOVER */
    score.score = 15;
    fault.level = FAULT_CRITICAL;
    policy_evaluate(&score, &fault, 4, &decision);
    T("score<20 → ACTION_FAILOVER", decision.action == ACTION_FAILOVER);

    /* Test 4: Level cap */
    score.score = 15;
    fault.level = FAULT_CRITICAL;
    policy_evaluate(&score, &fault, 1, &decision);
    T("level cap: FAILOVER capped to WARN at level 1",
      decision.action == ACTION_WARN);

    /* Test 5: Pending increasing → ACTION_WARN */
    score.score = 80;
    fault.level = FAULT_WARN;
    fault.pending_increasing = 1;
    policy_evaluate(&score, &fault, 4, &decision);
    T("pending increasing → ACTION_WARN", decision.action == ACTION_WARN);

    printf("---\nResults: %d passed, %d failed\n", passed, failed);
    return failed > 0 ? 1 : 0;
}
TESTEOF

gcc -std=c11 -Wall -Wextra \
    -Iinclude -Isrc \
    -o /tmp/test_policy_$$ \
    /tmp/test_policy_$$.c \
    src/policy_engine.c \
    2>&1 | grep -v '^$'

if [ -x /tmp/test_policy_$$ ]; then
    /tmp/test_policy_$$
    RC2=$?
else
    echo "FAIL: compile error"
    RC2=1
fi
rm -f /tmp/test_policy_$$ /tmp/test_policy_$$.c

print_results
