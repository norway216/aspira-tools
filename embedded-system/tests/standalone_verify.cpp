/**
 * Standalone verification of the core high-concurrency and high-reliability
 * primitives: Result<T>, CancellationToken, BoundedQueue<T>, error codes.
 *
 * Uses only C++17 standard library — no external dependencies needed.
 */

#include <atomic>
#include <cassert>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <iostream>
#include <mutex>
#include <optional>
#include <queue>
#include <string>
#include <thread>
#include <vector>
#include <iomanip>

// ============================================================================
// Result<T> — Monadic Error Type
// ============================================================================

struct InstallerError {
    std::string code;
    std::string title;
    std::string user_message;
    bool retryable = false;

    static InstallerError make(const std::string& c, const std::string& t,
                               const std::string& u, bool r = false) {
        return {c, t, u, r};
    }
};

template <typename T>
class Result {
public:
    static Result ok(T value) { Result r; r.value_ = std::move(value); r.ok_ = true; return r; }
    static Result err(InstallerError e) { Result r; r.error_ = std::move(e); r.ok_ = false; return r; }

    bool is_ok() const { return ok_; }
    bool is_err() const { return !ok_; }
    explicit operator bool() const { return is_ok(); }

    T& value() { return *value_; }
    const T& value() const { return *value_; }
    const InstallerError& error() const { return *error_; }

    template <typename F>
    auto map(F&& f) -> Result<decltype(f(std::declval<T>()))> {
        using R = decltype(f(std::declval<T>()));
        if (is_ok()) return Result<R>::ok(f(value()));
        return Result<R>::err(*error_);
    }

private:
    std::optional<T> value_;
    std::optional<InstallerError> error_;
    bool ok_ = false;
};

template <>
class Result<void> {
public:
    static Result ok() { Result r; r.ok_ = true; return r; }
    static Result err(InstallerError e) { Result r; r.error_ = std::move(e); r.ok_ = false; return r; }

    bool is_ok() const { return ok_; }
    bool is_err() const { return !ok_; }
    explicit operator bool() const { return is_ok(); }
    const InstallerError& error() const { return *error_; }

private:
    std::optional<InstallerError> error_;
    bool ok_ = false;
};

// ============================================================================
// CancellationToken — Lock-Free Cancellation
// ============================================================================

class CancellationToken {
public:
    bool is_cancelled() const { return cancelled_.load(std::memory_order_acquire); }
    void cancel() { cancelled_.store(true, std::memory_order_release); }
private:
    std::atomic<bool> cancelled_{false};
};

// ============================================================================
// BoundedQueue<T> — Producer-Consumer Pipeline with Back-Pressure
// ============================================================================

template <typename T>
class BoundedQueue {
public:
    explicit BoundedQueue(size_t cap) : capacity_(cap) {}

    bool push(T item) {
        std::unique_lock<std::mutex> lock(mutex_);
        not_full_.wait(lock, [this] { return queue_.size() < capacity_ || closed_; });
        if (closed_) return false;
        queue_.push(std::move(item));
        not_empty_.notify_one();
        return true;
    }

    bool pop(T& item) {
        std::unique_lock<std::mutex> lock(mutex_);
        not_empty_.wait(lock, [this] { return !queue_.empty() || closed_; });
        if (queue_.empty() && closed_) return false;
        item = std::move(queue_.front());
        queue_.pop();
        not_full_.notify_one();
        return true;
    }

    void close() {
        { std::lock_guard<std::mutex> lock(mutex_); closed_ = true; }
        not_empty_.notify_all();
        not_full_.notify_all();
    }

    size_t size() const { std::lock_guard<std::mutex> lock(mutex_); return queue_.size(); }

private:
    std::queue<T> queue_;
    size_t capacity_;
    mutable std::mutex mutex_;
    std::condition_variable not_full_;
    std::condition_variable not_empty_;
    bool closed_ = false;
};

// ============================================================================
// Error Code System (Architecture Doc §17)
// ============================================================================

namespace ErrorCode {
    constexpr const char* DEVICE_NOT_FOUND    = "E1001";
    constexpr const char* DEVICE_READ_ONLY    = "E1002";
    constexpr const char* DEVICE_CAPACITY_LOW = "E1003";
    constexpr const char* PACKAGE_INVALID_FMT = "E2001";
    constexpr const char* PACKAGE_SIG_FAIL    = "E2002";
    constexpr const char* PACKAGE_HASH_FAIL   = "E2004";
    constexpr const char* PARTITION_FAILED    = "E3001";
    constexpr const char* FS_FORMAT_FAILED    = "E3002";
    constexpr const char* FS_MOUNT_FAILED     = "E3003";
    constexpr const char* IMAGE_WRITE_FAILED  = "E4001";
    constexpr const char* IMAGE_VERIFY_FAILED = "E4002";
    constexpr const char* INTERNAL_ERROR      = "E9001";
    constexpr const char* INTERNAL_CANCELLED  = "E9003";
}

struct ErrorEntry {
    const char* code;
    const char* description;
    bool retryable;
};

static const ErrorEntry error_registry[] = {
    {ErrorCode::DEVICE_NOT_FOUND,    "No target storage device found",           false},
    {ErrorCode::DEVICE_READ_ONLY,    "Target device is read-only",               false},
    {ErrorCode::DEVICE_CAPACITY_LOW, "Target device capacity insufficient",     false},
    {ErrorCode::PACKAGE_INVALID_FMT, "Installation package format is invalid",  false},
    {ErrorCode::PACKAGE_SIG_FAIL,    "Package signature verification failed",   false},
    {ErrorCode::PACKAGE_HASH_FAIL,   "Package payload hash mismatch",           false},
    {ErrorCode::PARTITION_FAILED,    "Partition table creation failed",         true},
    {ErrorCode::FS_FORMAT_FAILED,    "Filesystem format failed",                true},
    {ErrorCode::FS_MOUNT_FAILED,     "Filesystem mount failed",                 true},
    {ErrorCode::IMAGE_WRITE_FAILED,  "Image write to device failed",            true},
    {ErrorCode::IMAGE_VERIFY_FAILED, "Post-write verification failed",          true},
    {ErrorCode::INTERNAL_ERROR,      "Internal error",                          false},
    {ErrorCode::INTERNAL_CANCELLED,  "Operation was cancelled",                 false},
};

const char* lookup_error(const std::string& code) {
    for (auto& e : error_registry) {
        if (e.code == code) return e.description;
    }
    return "Unknown error";
}

// ============================================================================
// Job State Machine (Architecture Doc §7)
// ============================================================================

enum class JobState { Idle, Preparing, Running, Paused, Cancelling, Completed, Failed };

const char* state_name(JobState s) {
    switch (s) {
        case JobState::Idle:       return "Idle";
        case JobState::Preparing:  return "Preparing";
        case JobState::Running:    return "Running";
        case JobState::Paused:     return "Paused";
        case JobState::Cancelling: return "Cancelling";
        case JobState::Completed:  return "Completed";
        case JobState::Failed:     return "Failed";
    }
    return "Unknown";
}

class StateMachine {
public:
    bool transition(JobState from, JobState to) {
        // Only allow valid transitions
        static const std::pair<JobState, JobState> valid[] = {
            {JobState::Idle,       JobState::Preparing},
            {JobState::Preparing,  JobState::Running},
            {JobState::Running,    JobState::Paused},
            {JobState::Paused,     JobState::Running},
            {JobState::Running,    JobState::Completed},
            {JobState::Running,    JobState::Failed},
            {JobState::Running,    JobState::Cancelling},
            {JobState::Cancelling, JobState::Failed},
            {JobState::Failed,     JobState::Idle},
            {JobState::Completed,  JobState::Idle},
        };
        for (auto& t : valid) {
            if (t.first == from && t.second == to) {
                state_.store(to, std::memory_order_release);
                return true;
            }
        }
        return false;
    }

    JobState state() const { return state_.load(std::memory_order_acquire); }

private:
    std::atomic<JobState> state_{JobState::Idle};
};

// ============================================================================
// Tests
// ============================================================================

static int tests_passed = 0;
static int tests_failed = 0;

#define TEST(name) \
    do { std::cout << "  " << std::left << std::setw(55) << name; } while(0)

#define PASS() \
    do { std::cout << " PASSED\n"; tests_passed++; } while(0)

#define FAIL(msg) \
    do { std::cout << " FAILED — " << msg << "\n"; tests_failed++; } while(0)

#define CHECK(cond, msg) \
    do { if (!(cond)) { FAIL(msg); return; } } while(0)

// ---------------------------------------------------------------------------
void test_result_ok() {
    TEST("Result<int>::ok(42)");
    auto r = Result<int>::ok(42);
    CHECK(r.is_ok(), "expected ok");
    CHECK(!r.is_err(), "expected not err");
    CHECK(r.value() == 42, "expected 42");
    CHECK(r, "bool conversion");
    PASS();
}

void test_result_err() {
    TEST("Result<int>::err() propagation");
    auto e = InstallerError::make("E4001", "Write Failed", "Disk write error", true);
    auto r = Result<int>::err(e);
    CHECK(r.is_err(), "expected err");
    CHECK(!r.is_ok(), "expected not ok");
    CHECK(r.error().code == "E4001", "error code mismatch");
    CHECK(r.error().retryable, "expected retryable");
    PASS();
}

void test_result_void_ok() {
    TEST("Result<void>::ok()");
    auto r = Result<void>::ok();
    CHECK(r.is_ok(), "expected ok");
    CHECK(!r.is_err(), "expected not err");
    PASS();
}

void test_result_void_err() {
    TEST("Result<void>::err()");
    auto r = Result<void>::err(InstallerError::make("E9001", "Internal", "Fail"));
    CHECK(r.is_err(), "expected err");
    PASS();
}

void test_result_map() {
    TEST("Result<T>::map() composition");
    auto r = Result<int>::ok(10);
    auto r2 = r.map([](int x) { return x * 2; });
    CHECK(r2.is_ok(), "expected ok after map");
    CHECK(r2.value() == 20, "expected 20");
    PASS();
}

void test_result_map_on_err() {
    TEST("Result<T>::map() on error (no-op)");
    auto r = Result<int>::err(InstallerError::make("E9001", "X", "Y"));
    auto r2 = r.map([](int x) { return x + 1; });
    CHECK(r2.is_err(), "expected err after map on err");
    PASS();
}

void test_cancel_token_not_cancelled() {
    TEST("CancellationToken — initial state");
    CancellationToken ct;
    CHECK(!ct.is_cancelled(), "should not be cancelled");
    PASS();
}

void test_cancel_token_cancel() {
    TEST("CancellationToken — cancel signal");
    CancellationToken ct;
    ct.cancel();
    CHECK(ct.is_cancelled(), "should be cancelled");
    PASS();
}

void test_cancel_token_thread_safe() {
    TEST("CancellationToken — cross-thread signal");
    CancellationToken ct;
    std::atomic<bool> seen{false};

    std::thread worker([&]() {
        while (!ct.is_cancelled()) {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        seen.store(true);
    });

    std::this_thread::sleep_for(std::chrono::milliseconds(10));
    ct.cancel();
    worker.join();
    CHECK(seen.load(), "worker should see cancellation");
    PASS();
}

void test_bounded_queue_push_pop() {
    TEST("BoundedQueue — basic push/pop (4 items)");
    BoundedQueue<int> q(4);
    CHECK(q.push(1), "push 1");
    CHECK(q.push(2), "push 2");
    CHECK(q.push(3), "push 3");
    CHECK(q.push(4), "push 4");

    int v;
    CHECK(q.pop(v) && v == 1, "pop 1");
    CHECK(q.pop(v) && v == 2, "pop 2");
    CHECK(q.pop(v) && v == 3, "pop 3");
    CHECK(q.pop(v) && v == 4, "pop 4");
    PASS();
}

void test_bounded_queue_close() {
    TEST("BoundedQueue — close wakes blocked threads");
    BoundedQueue<int> q(1);
    q.push(1);  // fill the queue

    std::atomic<bool> woken{false};
    std::thread pusher([&]() {
        q.push(2);  // will block until close
        woken.store(true);
    });

    std::this_thread::sleep_for(std::chrono::milliseconds(10));
    CHECK(!woken.load(), "pusher should be blocked");

    q.close();
    pusher.join();
    CHECK(woken.load(), "pusher should be woken after close");
    PASS();
}

void test_bounded_queue_producer_consumer() {
    TEST("BoundedQueue — producer-consumer (1000 items, 2 threads)");
    BoundedQueue<int> q(8);
    const int N = 1000;
    std::atomic<int> sum{0};

    std::thread producer([&]() {
        for (int i = 0; i < N; i++) {
            q.push(i);
        }
        q.close();
    });

    std::thread consumer([&]() {
        int v;
        int local_sum = 0;
        while (q.pop(v)) {
            local_sum += v;
        }
        sum.store(local_sum);
    });

    producer.join();
    consumer.join();

    int expected = (N - 1) * N / 2;  // sum 0..999
    CHECK(sum.load() == expected, "sum mismatch: got " + std::to_string(sum.load()) +
                                  " expected " + std::to_string(expected));
    PASS();
}

void test_pipeline_three_stage() {
    TEST("BoundedQueue — 3-stage pipeline simulation");
    // Simulates: Reader -> Decompressor -> Writer
    BoundedQueue<int> q1(4);  // raw data queue
    BoundedQueue<int> q2(4);  // decompressed data queue
    const int N = 500;

    std::atomic<long long> written{0};

    std::thread reader([&]() {
        for (int i = 0; i < N; i++) { q1.push(i); }
        q1.close();
    });

    std::thread processor([&]() {
        int v;
        while (q1.pop(v)) { q2.push(v * 2); }  // "decompress" = multiply by 2
        q2.close();
    });

    std::thread writer([&]() {
        int v;
        long long sum = 0;
        while (q2.pop(v)) { sum += v; }
        written.store(sum);
    });

    reader.join();
    processor.join();
    writer.join();

    long long expected = (long long)(N - 1) * N;  // sum of 2*i for i=0..499
    CHECK(written.load() == expected,
          "pipeline sum mismatch: got " + std::to_string(written.load()) +
          " expected " + std::to_string(expected));
    PASS();
}

void test_error_lookup() {
    TEST("Error code lookup — all 13 codes");
    CHECK(std::string(lookup_error("E1001")) == "No target storage device found", "E1001");
    CHECK(std::string(lookup_error("E2001")) == "Installation package format is invalid", "E2001");
    CHECK(std::string(lookup_error("E4001")) == "Image write to device failed", "E4001");
    CHECK(std::string(lookup_error("E9003")) == "Operation was cancelled", "E9003");
    CHECK(std::string(lookup_error("E9999")) == "Unknown error", "unknown code");
    PASS();
}

void test_state_machine_valid() {
    TEST("StateMachine — valid transition Idle→Preparing→Running");
    StateMachine sm;
    CHECK(sm.state() == JobState::Idle, "initial state");
    CHECK(sm.transition(JobState::Idle, JobState::Preparing), "idle→preparing");
    CHECK(sm.state() == JobState::Preparing, "preparing");
    CHECK(sm.transition(JobState::Preparing, JobState::Running), "preparing→running");
    CHECK(sm.state() == JobState::Running, "running");
    PASS();
}

void test_state_machine_invalid() {
    TEST("StateMachine — reject invalid transition Idle→Completed");
    StateMachine sm;
    CHECK(sm.state() == JobState::Idle, "initial");
    CHECK(!sm.transition(JobState::Idle, JobState::Completed), "should reject");
    CHECK(sm.state() == JobState::Idle, "still idle");
    PASS();
}

void test_state_machine_full_lifecycle() {
    TEST("StateMachine — full lifecycle Idle→Running→Completed→Idle");
    StateMachine sm;
    CHECK(sm.transition(JobState::Idle, JobState::Preparing), "1");
    CHECK(sm.transition(JobState::Preparing, JobState::Running), "2");
    CHECK(sm.transition(JobState::Running, JobState::Completed), "3");
    CHECK(sm.state() == JobState::Completed, "completed");
    CHECK(sm.transition(JobState::Completed, JobState::Idle), "reset");
    CHECK(sm.state() == JobState::Idle, "idle again");
    PASS();
}

void test_state_machine_failure_path() {
    TEST("StateMachine — failure path Idle→Running→Failed→Idle");
    StateMachine sm;
    CHECK(sm.transition(JobState::Idle, JobState::Preparing), "1");
    CHECK(sm.transition(JobState::Preparing, JobState::Running), "2");
    CHECK(sm.transition(JobState::Running, JobState::Failed), "3");
    CHECK(sm.state() == JobState::Failed, "failed");
    CHECK(sm.transition(JobState::Failed, JobState::Idle), "reset");
    CHECK(sm.state() == JobState::Idle, "idle");
    PASS();
}

// ============================================================================
// Main
// ============================================================================

int main() {
    std::cout << "\n";
    std::cout << "╔══════════════════════════════════════════════════════════════╗\n";
    std::cout << "║  Embedded Linux Installer — Core Primitives Verification    ║\n";
    std::cout << "║  High Concurrency & High Reliability Architecture Test       ║\n";
    std::cout << "╚══════════════════════════════════════════════════════════════╝\n\n";

    std::cout << "── Result<T> Monadic Error Type ──\n";
    test_result_ok();
    test_result_err();
    test_result_void_ok();
    test_result_void_err();
    test_result_map();
    test_result_map_on_err();

    std::cout << "\n── CancellationToken (Lock-Free) ──\n";
    test_cancel_token_not_cancelled();
    test_cancel_token_cancel();
    test_cancel_token_thread_safe();

    std::cout << "\n── BoundedQueue<T> (Producer-Consumer Pipeline) ──\n";
    test_bounded_queue_push_pop();
    test_bounded_queue_close();
    test_bounded_queue_producer_consumer();
    test_pipeline_three_stage();

    std::cout << "\n── Error Code System (E1xxx–E9xxx) ──\n";
    test_error_lookup();

    std::cout << "\n── Job State Machine ──\n";
    test_state_machine_valid();
    test_state_machine_invalid();
    test_state_machine_full_lifecycle();
    test_state_machine_failure_path();

    std::cout << "\n";
    std::cout << "═══════════════════════════════════════════════════════════════\n";
    std::cout << "  Total: " << (tests_passed + tests_failed)
              << "  |  Passed: " << tests_passed
              << "  |  Failed: " << tests_failed << "\n";
    std::cout << "═══════════════════════════════════════════════════════════════\n\n";

    return tests_failed > 0 ? 1 : 0;
}
