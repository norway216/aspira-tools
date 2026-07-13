/**
 * @file test_result.cpp
 * @brief Unit tests for the Result<T> monadic error type.
 */

#include "installer/core/result.h"
#include "installer/core/types.h"

#include <string>
#include <utility>

#if __has_include(<gtest/gtest.h>)
#include <gtest/gtest.h>
#else
#include "helpers/minimal_test.h"
#endif

using namespace installer;

// =========================================================================
//  Basic Result<T> tests
// =========================================================================

TEST(ResultTest, OkInt) {
    auto r = Result<int>::ok(42);
    EXPECT_TRUE(r.is_ok());
    EXPECT_FALSE(r.is_err());
    EXPECT_EQ(r.value(), 42);
}

TEST(ResultTest, ErrInt) {
    auto err = InstallerError::make("E9999", "Test Error", "Something failed");
    auto r = Result<int>::err(err);
    EXPECT_TRUE(r.is_err());
    EXPECT_FALSE(r.is_ok());
    EXPECT_EQ(r.error().code, "E9999");
    EXPECT_EQ(r.error().title, "Test Error");
}

TEST(ResultTest, BoolConversion) {
    auto r = Result<int>::ok(10);
    if (r) {
        // Should enter here
        EXPECT_TRUE(true);
    } else {
        EXPECT_TRUE(false);  // Should not reach here
    }
}

TEST(ResultTest, InvalidDefault) {
    // Default-constructed Result is neither ok nor err (invalid)
    Result<int> r;
    EXPECT_FALSE(r.is_ok());
    EXPECT_TRUE(r.is_err());
}

TEST(ResultTest, TakeValue) {
    auto r = Result<int>::ok(100);
    EXPECT_EQ(r.value(), 100);
    int taken = r.take_value();
    EXPECT_EQ(taken, 100);
    EXPECT_FALSE(r.is_ok());  // value consumed
}

TEST(ResultTest, TakeError) {
    auto err = InstallerError::make("E1001", "Not Found", "Device missing");
    auto r = Result<int>::err(err);
    auto taken_error = r.take_error();
    EXPECT_EQ(taken_error.code, "E1001");
}

// =========================================================================
//  Result<void> tests
// =========================================================================

TEST(ResultVoidTest, Ok) {
    auto r = Result<void>::ok();
    EXPECT_TRUE(r.is_ok());
    EXPECT_FALSE(r.is_err());
}

TEST(ResultVoidTest, Err) {
    auto err = InstallerError::make("E9001", "Internal", "Oops");
    auto r = Result<void>::err(err);
    EXPECT_TRUE(r.is_err());
    EXPECT_EQ(r.error().code, "E9001");
}

// =========================================================================
//  Monadic operations: map
// =========================================================================

TEST(ResultTest, MapOnOk) {
    auto r = Result<int>::ok(5);
    auto r2 = r.map([](int x) { return x * 2; });
    EXPECT_TRUE(r2.is_ok());
    EXPECT_EQ(r2.value(), 10);
}

TEST(ResultTest, MapOnErr) {
    auto err = InstallerError::make("E9999", "Base Error", "original");
    auto r = Result<int>::err(err);
    auto r2 = r.map([](int x) { return x * 2; });
    EXPECT_TRUE(r2.is_err());
    EXPECT_EQ(r2.error().code, "E9999");
}

TEST(ResultTest, MapChaining) {
    auto r = Result<int>::ok(3)
        .map([](int x) { return x + 1; })
        .map([](int x) { return x * 10; });
    EXPECT_TRUE(r.is_ok());
    EXPECT_EQ(r.value(), 40);
}

// =========================================================================
//  Monadic operations: and_then
// =========================================================================

TEST(ResultTest, AndThenOnOk) {
    auto r = Result<int>::ok(7);
    auto r2 = r.and_then([](int x) -> Result<std::string> {
        return Result<std::string>::ok(std::to_string(x));
    });
    EXPECT_TRUE(r2.is_ok());
    EXPECT_EQ(r2.value(), "7");
}

TEST(ResultTest, AndThenOnErr) {
    auto err = InstallerError::make("E9999", "Base", "fail");
    auto r = Result<int>::err(err);
    auto r2 = r.and_then([](int x) -> Result<std::string> {
        return Result<std::string>::ok(std::to_string(x));
    });
    EXPECT_TRUE(r2.is_err());
    EXPECT_EQ(r2.error().code, "E9999");
}

TEST(ResultTest, AndThenChain) {
    auto r = Result<int>::ok(5)
        .and_then([](int x) -> Result<int> {
            return Result<int>::ok(x * 2);
        })
        .and_then([](int x) -> Result<std::string> {
            return Result<std::string>::ok("value=" + std::to_string(x));
        });
    EXPECT_TRUE(r.is_ok());
    EXPECT_EQ(r.value(), "value=10");
}

// =========================================================================
//  Monadic operations: or_else
// =========================================================================

TEST(ResultTest, OrElseOnOk) {
    auto r = Result<int>::ok(42);
    auto r2 = r.or_else([](const InstallerError&) -> Result<int> {
        return Result<int>::ok(0);  // fallback, should not be used
    });
    EXPECT_TRUE(r2.is_ok());
    EXPECT_EQ(r2.value(), 42);
}

TEST(ResultTest, OrElseOnErr) {
    auto err = InstallerError::make("E1001", "Not Found", "gone");
    auto r = Result<int>::err(err);
    auto r2 = r.or_else([](const InstallerError& e) -> Result<int> {
        // Provide a fallback value
        EXPECT_EQ(e.code, "E1001");
        return Result<int>::ok(-1);
    });
    EXPECT_TRUE(r2.is_ok());
    EXPECT_EQ(r2.value(), -1);
}

// =========================================================================
//  Move semantics
// =========================================================================

TEST(ResultTest, MoveConstruct) {
    auto r1 = Result<int>::ok(99);
    Result<int> r2(std::move(r1));
    EXPECT_TRUE(r2.is_ok());
    EXPECT_EQ(r2.value(), 99);
    EXPECT_FALSE(r1.is_ok());  // moved-from is invalid
}

TEST(ResultTest, MoveAssign) {
    auto r1 = Result<int>::ok(55);
    Result<int> r2;
    r2 = std::move(r1);
    EXPECT_TRUE(r2.is_ok());
    EXPECT_EQ(r2.value(), 55);
}

TEST(ResultTest, CopyConstruct) {
    auto r1 = Result<int>::ok(123);
    Result<int> r2(r1);  // copy
    EXPECT_TRUE(r1.is_ok());
    EXPECT_TRUE(r2.is_ok());
    EXPECT_EQ(r1.value(), 123);
    EXPECT_EQ(r2.value(), 123);
}

// =========================================================================
//  Error propagation pattern
// =========================================================================

static Result<int> divide(int a, int b) {
    if (b == 0) {
        return Result<int>::err(
            InstallerError::make("E9999", "Division Error", "Divide by zero"));
    }
    return Result<int>::ok(a / b);
}

static Result<int> add_ten(int x) {
    return Result<int>::ok(x + 10);
}

TEST(ResultTest, ErrorPropagation) {
    auto r = Result<int>::ok(100)
        .and_then([](int x) { return divide(x, 5);  })   // 20
        .and_then([](int x) { return add_ten(x);   });   // 30
    EXPECT_TRUE(r.is_ok());
    EXPECT_EQ(r.value(), 30);
}

TEST(ResultTest, ErrorPropagationShortCircuit) {
    auto r = Result<int>::ok(100)
        .and_then([](int x) { return divide(x, 0); })    // error
        .and_then([](int x) { return add_ten(x); });     // never called
    EXPECT_TRUE(r.is_err());
    EXPECT_EQ(r.error().code, "E9999");
}

// =========================================================================
//  Struct types
// =========================================================================

struct TestPayload {
    std::string name;
    int value = 0;
};

TEST(ResultTest, StructValue) {
    TestPayload p{"test", 42};
    auto r = Result<TestPayload>::ok(std::move(p));
    EXPECT_TRUE(r.is_ok());
    EXPECT_EQ(r.value().name, "test");
    EXPECT_EQ(r.value().value, 42);
}

// =========================================================================
//  CancellationToken tests
// =========================================================================

TEST(CancellationTokenTest, InitialState) {
    CancellationToken token;
    EXPECT_FALSE(token.is_cancelled());
}

TEST(CancellationTokenTest, Cancel) {
    CancellationToken token;
    token.cancel();
    EXPECT_TRUE(token.is_cancelled());
}

TEST(CancellationTokenTest, Reset) {
    CancellationToken token;
    token.cancel();
    EXPECT_TRUE(token.is_cancelled());
    token.reset();
    EXPECT_FALSE(token.is_cancelled());
}
