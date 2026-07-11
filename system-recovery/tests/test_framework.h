/**
 * @file test_framework.h
 * @brief Minimal test framework – no external dependency.
 *
 * Provides TEST_ASSERT macros and a simple test runner.
 * Output format is TAP-compatible for CI integration.
 */

#ifndef TESTS_TEST_FRAMEWORK_H
#define TESTS_TEST_FRAMEWORK_H

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

extern int _tests_total;

#define TEST_ASSERT(cond, msg) do { \
    if (!(cond)) { \
        fprintf(stderr, "  FAIL %s:%d – %s\n", __FILE__, __LINE__, msg); \
        return 1; \
    } \
} while(0)

#define TEST_ASSERT_EQ(a, b, msg) do { \
    if ((a) != (b)) { \
        fprintf(stderr, "  FAIL %s:%d – %s (expected %d, got %d)\n", \
                __FILE__, __LINE__, msg, (int)(b), (int)(a)); \
        return 1; \
    } \
} while(0)

#define TEST_ASSERT_STR_EQ(a, b, msg) do { \
    if (strcmp((a), (b)) != 0) { \
        fprintf(stderr, "  FAIL %s:%d – %s (expected '%s', got '%s')\n", \
                __FILE__, __LINE__, msg, (b), (a)); \
        return 1; \
    } \
} while(0)

#define TEST_ASSERT_NULL(p, msg) do { \
    if ((p) != NULL) { \
        fprintf(stderr, "  FAIL %s:%d – %s (expected NULL)\n", \
                __FILE__, __LINE__, msg); \
        return 1; \
    } \
} while(0)

#define TEST_ASSERT_NOT_NULL(p, msg) do { \
    if ((p) == NULL) { \
        fprintf(stderr, "  FAIL %s:%d – %s (expected non-NULL)\n", \
                __FILE__, __LINE__, msg); \
        return 1; \
    } \
} while(0)

#define TEST_ASSERT_NOT_ZERO(v, msg) do { \
    if ((v) == 0) { \
        fprintf(stderr, "  FAIL %s:%d – %s (expected non-zero)\n", \
                __FILE__, __LINE__, msg); \
        return 1; \
    } \
} while(0)

/** Declare and register a test function. */
typedef int (*test_func_t)(void);

typedef struct {
    const char  *suite;
    const char  *name;
    test_func_t  fn;
} test_case_t;

#define MAX_TESTS 256
extern test_case_t _test_cases[MAX_TESTS];
extern int         _test_count;

#define REGISTER_TEST(suite_name, test_fn) do { \
    if (_test_count < MAX_TESTS) { \
        _test_cases[_test_count].suite = suite_name; \
        _test_cases[_test_count].name  = #test_fn; \
        _test_cases[_test_count].fn    = (test_func_t)(test_fn); \
        _test_count++; \
    } \
} while(0)

/** Run all registered tests. */
static inline int run_all_tests(void)
{
    int passed = 0, failed = 0;

    printf("1..%d\n", _test_count);

    for (int i = 0; i < _test_count; i++) {
        int ret = _test_cases[i].fn();

        if (ret == 0) {
            printf("ok %d - [%s] %s\n", i + 1,
                   _test_cases[i].suite, _test_cases[i].name);
            passed++;
        } else {
            printf("not ok %d - [%s] %s\n", i + 1,
                   _test_cases[i].suite, _test_cases[i].name);
            failed++;
        }
    }

    printf("\n# Test summary\n");
    printf("# Total: %d | Passed: %d | Failed: %d\n",
           _test_count, passed, failed);

    return (failed == 0) ? 0 : 1;
}

#endif /* TESTS_TEST_FRAMEWORK_H */
