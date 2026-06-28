#!/bin/bash
# Test: V2 Failure Prediction Engine
# Validates: moving average, linear regression, probability estimation

source "$(dirname "$0")/helper.sh"

echo "=== test_failure_predict: V2 Failure Prediction ==="

cat > /tmp/test_predict_$$.c << 'TESTEOF'
#define _DEFAULT_SOURCE
#include "failure_predict.h"
#include "health_score.h"
#include <stdio.h>
#include <string.h>
#include <math.h>

static int passed = 0, failed = 0;
#define T(name, cond) do { \
    if (cond) { printf("  PASS: %s\n", name); passed++; } \
    else { printf("  FAIL: %s\n", name); failed++; } \
} while(0)

int main(void) {
    trend_data_t trend;
    smart_ata_data_t ata;
    io_metrics_t io;
    health_score_t score;
    predict_result_t result;

    /* Test 1: Empty trend → 0 probability */
    trend_data_init(&trend);
    memset(&score, 0, sizeof(score)); score.score = 100;
    failure_predict_compute(&trend, &score, &result);
    T("empty trend → failure_prob=0 (100-score)",
      result.failure_probability == 0);

    /* Test 2: Score 50 → base probability ~50% */
    score.score = 50;
    failure_predict_compute(&trend, &score, &result);
    T("score=50 → prob≈50", result.failure_probability >= 40
       && result.failure_probability <= 60);

    /* Test 3: Seed trend data with increasing pending sectors */
    trend_data_init(&trend);
    memset(&ata, 0, sizeof(ata)); ata.supported = 1;
    memset(&io, 0, sizeof(io));

    /* Feed 5 samples with growing pending count */
    for (int i = 0; i < 5; i++) {
        ata.pending_sectors = (uint64_t)(i * 2);  /* 0, 2, 4, 6, 8 */
        ata.temperature_celsius = 45;
        trend_data_update(&trend, &ata, &io);
    }
    T("5 samples → history_count=5", trend.history_count == 5);
    T("pending slope > 0", trend.pending_slope > 0.0);

    score.score = 80;
    failure_predict_compute(&trend, &score, &result);
    T("increasing pending → prob>0", result.failure_probability > 0);
    T("primary_risk=pending_sectors",
      strcmp(result.primary_risk, "pending_sectors") == 0);

    /* Test 4: Reallocated trend */
    trend_data_init(&trend);
    for (int i = 0; i < 5; i++) {
        ata.reallocated_sectors = (uint64_t)i;
        ata.pending_sectors = 0;
        trend_data_update(&trend, &ata, &io);
    }
    score.score = 90;
    failure_predict_compute(&trend, &score, &result);
    T("increasing realloc → primary=reallocated_sectors",
      strcmp(result.primary_risk, "reallocated_sectors") == 0);

    /* Test 5: score 40 → days_until_critical = 0 */
    trend_data_init(&trend);
    score.score = 40;
    failure_predict_compute(&trend, &score, &result);
    T("score=40 → days_until_critical=0", result.days_until_critical == 0);

    /* Test 6: Trend slope computation */
    trend_data_init(&trend);
    /* Feed exactly linear data: 0, 1, 2, 3, 4 */
    for (int i = 0; i < 5; i++) {
        ata.pending_sectors = (uint64_t)i;
        trend_data_update(&trend, &ata, &io);
    }
    /* Slope should be exactly 1.0 for values 0,1,2,3,4 */
    T("linear data slope ≈ 1.0", fabs(trend.pending_slope - 1.0) < 0.01);

    printf("---\nResults: %d passed, %d failed\n", passed, failed);
    return failed > 0 ? 1 : 0;
}
TESTEOF

gcc -std=c11 -Wall -Wextra \
    -Iinclude -Isrc \
    -o /tmp/test_predict_$$ \
    /tmp/test_predict_$$.c \
    src/failure_predict.c \
    src/health_score.c \
    -lm 2>&1 | grep -v '^$'

if [ -x /tmp/test_predict_$$ ]; then
    /tmp/test_predict_$$
    RC=$?
else
    echo "FAIL: compile error"
    RC=1
fi
rm -f /tmp/test_predict_$$ /tmp/test_predict_$$.c

print_results
