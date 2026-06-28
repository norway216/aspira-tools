#!/bin/bash
# Test: Health scoring engine
# Validates: formula correctness via white-box test harness

source "$(dirname "$0")/helper.sh"

echo "=== test_health_score: Scoring Formula Validation ==="

# Compile a small test harness that calls health_score_compute directly
TEST_SRC="/tmp/test_health_score_$$.c"
TEST_BIN="/tmp/test_health_score_$$"

cat > "$TEST_SRC" << 'EOF'
#include "disk_health.h"
#include "health_score.h"
#include <stdio.h>
#include <string.h>

static int tests_run = 0;
static int tests_failed = 0;

#define TEST(name, cond) do { \
    tests_run++; \
    if (!(cond)) { printf("FAIL: %s\n", name); tests_failed++; } \
    else { printf("PASS: %s\n", name); } \
} while(0)

int main(void) {
    health_score_t s;

    /* Test 1: perfect health */
    smart_ata_data_t ata1 = {0};
    ata1.supported = 1;
    ata1.enabled = 1;
    ata1.reallocated_sectors = 0;
    ata1.pending_sectors = 0;
    ata1.uncorrectable_errors = 0;
    ata1.crc_errors = 0;
    ata1.temperature_celsius = 40;
    smart_nvme_data_t nvme1 = {0};
    io_metrics_t io1 = {0};
    health_score_compute(&ata1, &nvme1, &io1, &s);
    TEST("Perfect health score=100", s.score == 100);
    TEST("Perfect health state=HEALTHY", s.state == HEALTH_HEALTHY);

    /* Test 2: reallocated sectors */
    smart_ata_data_t ata2 = {0};
    ata2.supported = 1; ata2.enabled = 1;
    ata2.reallocated_sectors = 5;
    ata2.temperature_celsius = 40;
    smart_nvme_data_t nvme2 = {0};
    io_metrics_t io2 = {0};
    health_score_compute(&ata2, &nvme2, &io2, &s);
    TEST("5 reallocated -> score=75", s.score == 75);
    TEST("5 reallocated -> penalty=25", s.realloc_penalty == 25);
    TEST("5 reallocated -> WARNING", s.state == HEALTH_WARNING);

    /* Test 3: pending sectors */
    smart_ata_data_t ata3 = {0};
    ata3.supported = 1; ata3.enabled = 1;
    ata3.pending_sectors = 8;
    ata3.temperature_celsius = 40;
    smart_nvme_data_t nvme3 = {0};
    io_metrics_t io3 = {0};
    health_score_compute(&ata3, &nvme3, &io3, &s);
    /* 10 * 8 = 80 penalty, score = 20 */
    TEST("8 pending -> penalty=80 capped at 30", s.pending_penalty == 30);
    TEST("8 pending -> score=70", s.score == 70);
    TEST("8 pending -> WARNING", s.state == HEALTH_WARNING);

    /* Test 4: temperature penalty */
    smart_ata_data_t ata4 = {0};
    ata4.supported = 1; ata4.enabled = 1;
    ata4.temperature_celsius = 70;
    smart_nvme_data_t nvme4 = {0};
    io_metrics_t io4 = {0};
    health_score_compute(&ata4, &nvme4, &io4, &s);
    /* temp 70: base 10 + (70-65) = 15 */
    TEST("temp=70 -> penalty=15", s.temp_penalty == 15);
    TEST("temp=70 -> score=85", s.score == 85);
    TEST("temp=70 -> WARNING", s.state == HEALTH_WARNING);

    /* Test 5: temperature mild */
    smart_ata_data_t ata5 = {0};
    ata5.supported = 1; ata5.enabled = 1;
    ata5.temperature_celsius = 56;
    smart_nvme_data_t nvme5 = {0};
    io_metrics_t io5 = {0};
    health_score_compute(&ata5, &nvme5, &io5, &s);
    TEST("temp=56 -> penalty=10", s.temp_penalty == 10);
    TEST("temp=56 -> score=90", s.score == 90);

    /* Test 6: combined penalties */
    smart_ata_data_t ata6 = {0};
    ata6.supported = 1; ata6.enabled = 1;
    ata6.reallocated_sectors = 10;
    ata6.pending_sectors = 3;
    ata6.uncorrectable_errors = 1;
    ata6.temperature_celsius = 50;
    smart_nvme_data_t nvme6 = {0};
    io_metrics_t io6 = {0};
    health_score_compute(&ata6, &nvme6, &io6, &s);
    /* realloc: min(5*10,40)=40, pending: min(10*3,30)=30, uncorrect: min(20*1,40)=20
     * total=90, score=10 */
    TEST("combined: realloc_penalty=40", s.realloc_penalty == 40);
    TEST("combined: pending_penalty=30", s.pending_penalty == 30);
    TEST("combined: uncorrect_penalty=20", s.uncorrectable_penalty == 20);
    TEST("combined: score=10", s.score == 10);
    TEST("combined: CRITICAL", s.state == HEALTH_CRITICAL);

    /* Test 7: no SMART support — should not penalize */
    smart_ata_data_t ata7 = {0};
    ata7.supported = 0;  /* SMART not supported */
    ata7.reallocated_sectors = 999;
    smart_nvme_data_t nvme7 = {0};
    io_metrics_t io7 = {0};
    health_score_compute(&ata7, &nvme7, &io7, &s);
    TEST("no SMART support -> score=100", s.score == 100);

    printf("---\nResults: %d run, %d failed\n", tests_run, tests_failed);
    return tests_failed > 0 ? 1 : 0;
}
EOF

# Find the include path
INCLUDE_DIR="$(dirname "$0")/../include"
SRC_DIR="$(dirname "$0")/../src"

# Compile test harness
gcc -std=c11 -Wall -Wextra \
    -I"$INCLUDE_DIR" \
    -I"$SRC_DIR" \
    -o "$TEST_BIN" \
    "$TEST_SRC" \
    "$SRC_DIR/health_score.c" \
    2>&1 | grep -v '^$'

if [ -x "$TEST_BIN" ]; then
    echo "Running health score unit tests..."
    "$TEST_BIN"
    RC=$?
else
    echo "FAIL: Could not compile test harness"
    RC=1
fi

# Cleanup
rm -f "$TEST_SRC" "$TEST_BIN"

exit $RC
