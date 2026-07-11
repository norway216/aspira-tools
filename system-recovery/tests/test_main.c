/**
 * @file test_main.c
 * @brief Test runner entry point.
 *
 * Usage:
 *   cc -DNATIVE_BUILD -Isrc tests/test_main.c \
 *      tests/unit/test_*.c tests/integration/test_*.c tests/system/test_*.c \
 *      src/services/operations/op_*.c src/services/operations/op_interface.c \
 *      src/services/config_manager.c src/core/event_bus.c \
 *      src/common/utils.c src/common/version.c \
 *      -o test_runner && ./test_runner
 */

#include "test_framework.h"

/* Actual storage for the global test registry */
test_case_t _test_cases[MAX_TESTS];
int         _test_count = 0;
int         _tests_total = 0;

/* Forward declarations for all test registration functions */
extern void register_event_bus_tests(void);
extern void register_config_tests(void);
extern void register_plugin_tests(void);
extern void register_integration_tests(void);
extern void register_system_tests(void);

int main(void)
{
    printf("# System Recovery – Test Suite\n");
    printf("# =============================\n\n");

    /* Unit tests */
    register_event_bus_tests();
    register_config_tests();
    register_plugin_tests();

    /* Integration tests */
    register_integration_tests();

    /* System tests */
    register_system_tests();

    return run_all_tests();
}
