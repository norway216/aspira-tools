/**
 * @file minimal_test.h
 * @brief Minimal test framework used when GTest is not available.
 *
 * Provides simple assertion macros and a test registration mechanism
 * that mimics a subset of the GTest API.
 */

#ifndef INSTALLER_MINIMAL_TEST_H
#define INSTALLER_MINIMAL_TEST_H

#include <cstdlib>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>
#include <functional>

namespace minimal_test {

struct TestCase {
    std::string suite;
    std::string name;
    std::function<void()> body;
};

inline std::vector<TestCase>& registry() {
    static std::vector<TestCase> r;
    return r;
}

struct TestRegistrar {
    TestRegistrar(const std::string& suite, const std::string& name,
                  std::function<void()> body) {
        registry().push_back({suite, name, std::move(body)});
    }
};

#define MINIMAL_TEST_CONCAT_(a, b) a##b
#define MINIMAL_TEST_CONCAT(a, b)  MINIMAL_TEST_CONCAT_(a, b)

#define TEST(suite, name)                                                      \
    static void MINIMAL_TEST_CONCAT(test_body_, __LINE__)();                   \
    static ::minimal_test::TestRegistrar                                       \
        MINIMAL_TEST_CONCAT(test_reg_, __LINE__)(#suite, #name,                \
            MINIMAL_TEST_CONCAT(test_body_, __LINE__));                        \
    static void MINIMAL_TEST_CONCAT(test_body_, __LINE__)()

// ---- Assertions ----

#define EXPECT_TRUE(cond)  _MINIMAL_EXPECT(cond, true)
#define EXPECT_FALSE(cond) _MINIMAL_EXPECT(!(cond), true)
#define EXPECT_EQ(a, b)    _MINIMAL_EXPECT_OP(a, b, ==, "==")
#define EXPECT_NE(a, b)    _MINIMAL_EXPECT_OP(a, b, !=, "!=")
#define EXPECT_LT(a, b)    _MINIMAL_EXPECT_OP(a, b, <,  "<")
#define EXPECT_GT(a, b)    _MINIMAL_EXPECT_OP(a, b, >,  ">")
#define EXPECT_THROW(stmt, ex_type)   _MINIMAL_EXPECT_THROW(stmt, ex_type)
#define EXPECT_NO_THROW(stmt)         _MINIMAL_EXPECT_NO_THROW(stmt)

// Asserts that terminate the test
#define ASSERT_TRUE(cond)  if (!(cond)) { _MINIMAL_FAIL(#cond, "true"); return; }
#define ASSERT_EQ(a, b)    if (!((a) == (b))) { _MINIMAL_FAIL_OP(a, b, "=="); return; }

#define _MINIMAL_EXPECT(cond, _)                                               \
    do {                                                                       \
        if (!(cond)) {                                                         \
            std::cerr << "  FAIL: " << __FILE__ << ":" << __LINE__             \
                      << ": expected " << #cond << "\n";                       \
        }                                                                      \
    } while (0)

#define _MINIMAL_EXPECT_OP(a, b, op, op_str)                                   \
    do {                                                                       \
        if (!((a) op (b))) {                                                   \
            std::cerr << "  FAIL: " << __FILE__ << ":" << __LINE__             \
                      << ": expected " << #a << " " op_str " " << #b           \
                      << " (" << (a) << " vs " << (b) << ")\n";               \
        }                                                                      \
    } while (0)

#define _MINIMAL_FAIL(expr, expected)                                          \
    std::cerr << "  FAIL: " << __FILE__ << ":" << __LINE__                     \
              << ": " << #expr << " is not " << #expected << "\n"

#define _MINIMAL_FAIL_OP(a, b, op)                                             \
    std::cerr << "  FAIL: " << __FILE__ << ":" << __LINE__                     \
              << ": " << #a << " " op " " << #b                                \
              << " (" << (a) << " vs " << (b) << ")\n"

#define _MINIMAL_EXPECT_THROW(stmt, ex_type)                                   \
    do {                                                                       \
        bool caught = false;                                                   \
        try { stmt; } catch (const ex_type&) { caught = true; }                \
        catch (...) {}                                                         \
        if (!caught) {                                                         \
            std::cerr << "  FAIL: " << __FILE__ << ":" << __LINE__             \
                      << ": expected exception not thrown\n";                  \
        }                                                                      \
    } while (0)

#define _MINIMAL_EXPECT_NO_THROW(stmt)                                         \
    do {                                                                       \
        try { stmt; } catch (const std::exception& e) {                        \
            std::cerr << "  FAIL: " << __FILE__ << ":" << __LINE__             \
                      << ": unexpected exception: " << e.what() << "\n";       \
        } catch (...) {                                                        \
            std::cerr << "  FAIL: " << __FILE__ << ":" << __LINE__             \
                      << ": unexpected exception\n";                           \
        }                                                                      \
    } while (0)

} // namespace minimal_test

int main() {
    int failed = 0;
    int passed = 0;

    for (const auto& tc : minimal_test::registry()) {
        std::cout << "[ RUN      ] " << tc.suite << "." << tc.name << "\n";
        try {
            tc.body();
            std::cout << "[       OK ] " << tc.suite << "." << tc.name << "\n";
            ++passed;
        } catch (const std::exception& e) {
            std::cerr << "[  FAILED  ] " << tc.suite << "." << tc.name
                      << " — exception: " << e.what() << "\n";
            ++failed;
        } catch (...) {
            std::cerr << "[  FAILED  ] " << tc.suite << "." << tc.name
                      << " — unknown exception\n";
            ++failed;
        }
    }

    std::cout << "\n---\n"
              << passed << " passed, " << failed << " failed\n";
    return (failed > 0) ? 1 : 0;
}

#endif // INSTALLER_MINIMAL_TEST_H
