/**
 * @file process_runner.cpp
 * @brief Implementation of ProcessRunner using fork+execvp.
 *
 * Design:
 *  - fork() the child, set up pipes for stdout/stderr/stdin, then execvp().
 *  - In the parent, use poll() to read stdout/stderr non-blocking while
 *    a timer thread enforces the timeout via SIGTERM/SIGKILL.
 *  - CancellationToken is checked pre-fork.  The timer thread also polls
 *    the token and can pre-empt the child.
 *  - ProcessResult is populated with exit code, captured streams, and
 *    timeout/cancelled flags.
 *
 * NEVER uses system(), popen(), or /bin/sh.  All arguments go directly to
 * execvp().
 */

#include "process_runner.h"

#include <atomic>
#include <cerrno>
#include <csignal>
#include <cstring>
#include <fcntl.h>
#include <poll.h>
#include <sys/wait.h>
#include <thread>
#include <unistd.h>

#include <algorithm>
#include <vector>

namespace installer {

// =============================================================================
// Internal helpers
// =============================================================================

namespace {

/** Small helper: build an InstallerError from errno. */
InstallerError make_errno(const std::string& context) {
    const char* msg = ::strerror(errno);
    return InstallerError::make(
        ErrorCode::INTERNAL_ERROR,
        "Process Error",
        context + ": " + std::string(msg),
        context + " (errno=" + std::to_string(errno) + ": " + msg + ")",
        true, false);
}

/**
 * RAII wrapper for a pipe pair.  Closes both ends on destruction.
 */
struct PipePair {
    int read_end = -1;
    int write_end = -1;

    ~PipePair() {
        if (read_end >= 0) ::close(read_end);
        if (write_end >= 0) ::close(write_end);
    }

    // Non-copyable, non-movable (simple stack-based lifetime)
    PipePair(const PipePair&) = delete;
    PipePair& operator=(const PipePair&) = delete;
    PipePair(PipePair&&) = delete;
    PipePair& operator=(PipePair&&) = delete;
};

/** Create a pipe with O_CLOEXEC.  Returns true on success. */
bool make_pipe(PipePair& pp) {
    int fds[2];
    if (::pipe2(fds, O_CLOEXEC) != 0) return false;
    pp.read_end  = fds[0];
    pp.write_end = fds[1];
    return true;
}

/**
 * Set a file descriptor to non-blocking mode.
 * Returns true on success.
 */
bool set_nonblocking(int fd) {
    int flags = ::fcntl(fd, F_GETFL, 0);
    if (flags < 0) return false;
    return ::fcntl(fd, F_SETFL, flags | O_NONBLOCK) == 0;
}

/**
 * Read all available data from fd into `buffer`.
 * Returns true on success (even if 0 bytes were read), false on error.
 */
bool slurp_fd(int fd, std::string& buffer) {
    char buf[8192];
    while (true) {
        ssize_t n = ::read(fd, buf, sizeof(buf));
        if (n > 0) {
            buffer.append(buf, static_cast<size_t>(n));
        } else if (n == 0) {
            // EOF — writer closed
            return true;
        } else {
            // n < 0
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                return true;   // no more data for now
            }
            if (errno == EINTR) {
                continue;      // interrupted — retry
            }
            return false;      // real error
        }
    }
    return true;
}

/**
 * Write all of `data` to `fd`, handling partial writes and EINTR.
 * Returns true on success.
 */
bool write_all_to_fd(int fd, const std::string& data) {
    size_t written = 0;
    while (written < data.size()) {
        ssize_t n = ::write(fd, data.data() + written, data.size() - written);
        if (n < 0) {
            if (errno == EINTR) continue;
            return false;
        }
        written += static_cast<size_t>(n);
    }
    return true;
}

/**
 * Convert a vector of strings to a NULL-terminated argv array.
 * The returned pointers are valid as long as `args` is alive.
 */
std::vector<char*> build_argv(const std::vector<std::string>& args) {
    std::vector<char*> argv;
    argv.reserve(args.size() + 1);
    for (const auto& a : args) {
        argv.push_back(const_cast<char*>(a.c_str()));
    }
    argv.push_back(nullptr);
    return argv;
}

/**
 * Timeout / cancellation monitor thread.
 *
 * Shared state (all atomic for lock-free coordination):
 *  - `pid`          Child PID to kill.
 *  - `done`         Main thread sets to true when it wants the monitor to exit.
 *  - `timed_out`    Monitor sets to true when timeout fires.
 *  - `cancelled`    Monitor sets to true when CancellationToken fires.
 *  - `token`        Pointer to CancellationToken (may be null).
 *  - `timeout_ms`   Total timeout in milliseconds.
 *
 * Strategy:
 *   Poll every 100ms.  On timeout or cancellation: send SIGTERM, wait 5s,
 *   then SIGKILL if the child is still alive.
 */
struct MonitorState {
    pid_t pid = -1;
    std::atomic<bool> done{false};
    std::atomic<bool> timed_out{false};
    std::atomic<bool> cancelled{false};
    const CancellationToken* token = nullptr;
    std::chrono::milliseconds timeout_ms{0};
};

void monitor_thread_func(MonitorState& ms) {
    using clock = std::chrono::steady_clock;
    auto start = clock::now();

    while (!ms.done.load(std::memory_order_acquire)) {
        // Check cancellation token
        if (ms.token && ms.token->is_cancelled()) {
            ms.cancelled.store(true, std::memory_order_release);
            ::kill(ms.pid, SIGTERM);
            // Grace period then hard kill
            for (int i = 0; i < 50 && !ms.done.load(std::memory_order_acquire); ++i) {
                std::this_thread::sleep_for(std::chrono::milliseconds(100));
            }
            if (!ms.done.load(std::memory_order_acquire)) {
                // Secondary safeguard: verify the PID still belongs to
                // our child before sending SIGKILL.  If the child has
                // exited and the PID was recycled, waitpid returns a
                // different result or -1 (ESRCH).
                int wstatus = 0;
                pid_t wresult = ::waitpid(ms.pid, &wstatus, WNOHANG);
                if (wresult == ms.pid) {
                    // Child exited and we reaped it — do NOT kill
                    ms.done.store(true, std::memory_order_release);
                } else if (wresult == 0) {
                    // Child still running — safe to kill
                    ::kill(ms.pid, SIGKILL);
                }
                // wresult == -1: child does not exist (ESRCH), do nothing
            }
            break;
        }

        // Check timeout
        auto elapsed = clock::now() - start;
        if (elapsed >= ms.timeout_ms) {
            ms.timed_out.store(true, std::memory_order_release);
            ::kill(ms.pid, SIGTERM);
            for (int i = 0; i < 50 && !ms.done.load(std::memory_order_acquire); ++i) {
                std::this_thread::sleep_for(std::chrono::milliseconds(100));
            }
            if (!ms.done.load(std::memory_order_acquire)) {
                // Secondary safeguard against PID reuse (see above)
                int wstatus = 0;
                pid_t wresult = ::waitpid(ms.pid, &wstatus, WNOHANG);
                if (wresult == ms.pid) {
                    ms.done.store(true, std::memory_order_release);
                } else if (wresult == 0) {
                    ::kill(ms.pid, SIGKILL);
                }
            }
            break;
        }

        // Sleep, but wake up early if done is set
        for (int i = 0; i < 10 && !ms.done.load(std::memory_order_acquire); ++i) {
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
    }
}

} // anonymous namespace

// =============================================================================
// ProcessRunner::run_impl
// =============================================================================

Result<ProcessResult> ProcessRunner::run_impl(
    const std::vector<std::string>& args,
    const std::string*        stdin_data,
    std::chrono::milliseconds timeout,
    const CancellationToken*  token) {

    // ---- Pre-flight checks ----

    if (args.empty()) {
        return Result<ProcessResult>::err(InstallerError::make(
            ErrorCode::INTERNAL_ERROR,
            "Invalid Arguments",
            "Cannot run a process with an empty argument list.",
            "args is empty", false, false));
    }

    if (token && token->is_cancelled()) {
        ProcessResult pr;
        pr.cancelled = true;
        return Result<ProcessResult>::ok(std::move(pr));
    }

    // ---- Create pipes ----

    PipePair stdout_pipe;
    PipePair stderr_pipe;
    PipePair stdin_pipe;   // only used when stdin_data != nullptr

    if (!make_pipe(stdout_pipe)) {
        return Result<ProcessResult>::err(make_errno("pipe() for stdout failed"));
    }
    if (!make_pipe(stderr_pipe)) {
        return Result<ProcessResult>::err(make_errno("pipe() for stderr failed"));
    }
    if (stdin_data != nullptr && !make_pipe(stdin_pipe)) {
        return Result<ProcessResult>::err(make_errno("pipe() for stdin failed"));
    }

    // ---- Fork ----

    pid_t pid = ::fork();
    if (pid < 0) {
        return Result<ProcessResult>::err(make_errno("fork() failed"));
    }

    // -----------------------------------------------------------------------
    // Child process
    // -----------------------------------------------------------------------
    if (pid == 0) {
        // Redirect stdout
        ::dup2(stdout_pipe.write_end, STDOUT_FILENO);
        ::close(stdout_pipe.read_end);
        ::close(stdout_pipe.write_end);

        // Redirect stderr
        ::dup2(stderr_pipe.write_end, STDERR_FILENO);
        ::close(stderr_pipe.read_end);
        ::close(stderr_pipe.write_end);

        // Redirect stdin if needed
        if (stdin_data != nullptr) {
            ::dup2(stdin_pipe.read_end, STDIN_FILENO);
            ::close(stdin_pipe.read_end);
            ::close(stdin_pipe.write_end);
        } else {
            // Connect stdin to /dev/null so the child does not read from our TTY
            int null_fd = ::open("/dev/null", O_RDONLY | O_CLOEXEC);
            if (null_fd >= 0) {
                ::dup2(null_fd, STDIN_FILENO);
                ::close(null_fd);
            }
        }

        // Build argv and exec
        auto argv = build_argv(args);
        ::execvp(argv[0], argv.data());

        // execvp only returns on error
        const char* msg = ::strerror(errno);
        std::fprintf(stderr, "execvp(%s) failed: %s\n", args[0].c_str(), msg);
        ::_exit(127);
    }

    // -----------------------------------------------------------------------
    // Parent process
    // -----------------------------------------------------------------------

    // Close the write ends (not needed in the parent) ...
    ::close(stdout_pipe.write_end);
    stdout_pipe.write_end = -1;
    ::close(stderr_pipe.write_end);
    stderr_pipe.write_end = -1;

    // ... and the read end of stdin
    if (stdin_data != nullptr) {
        ::close(stdin_pipe.read_end);
        stdin_pipe.read_end = -1;
    }

    // Set read ends to non-blocking for poll()
    set_nonblocking(stdout_pipe.read_end);
    set_nonblocking(stderr_pipe.read_end);

    // Write stdin data if provided (close write end when done to signal EOF)
    if (stdin_data != nullptr) {
        write_all_to_fd(stdin_pipe.write_end, *stdin_data);
        ::close(stdin_pipe.write_end);
        stdin_pipe.write_end = -1;
    }

    // ---- Start monitor thread ----

    MonitorState monitor_state;
    monitor_state.pid        = pid;
    monitor_state.timeout_ms = timeout;
    monitor_state.token      = token;

    std::thread monitor_thread(monitor_thread_func, std::ref(monitor_state));

    // ---- Main read loop ----

    ProcessResult result;
    bool child_exited = false;

    while (!child_exited) {
        // Build pollfd array
        std::vector<struct pollfd> fds;
        fds.reserve(2);

        if (stdout_pipe.read_end >= 0) {
            fds.push_back({stdout_pipe.read_end, POLLIN, 0});
        }
        if (stderr_pipe.read_end >= 0) {
            fds.push_back({stderr_pipe.read_end, POLLIN, 0});
        }

        if (fds.empty()) {
            // Both pipes already closed/EOF'd — just wait for the child
            break;
        }

        int poll_ret = ::poll(fds.data(), static_cast<nfds_t>(fds.size()), 100);
        if (poll_ret < 0) {
            if (errno == EINTR) continue;
            // Real error on poll — break and clean up
            break;
        }

        // Read available data from stdout
        if (stdout_pipe.read_end >= 0 && (poll_ret > 0)) {
            for (const auto& pfd : fds) {
                if (pfd.revents & (POLLIN | POLLHUP)) {
                    if (pfd.fd == stdout_pipe.read_end) {
                        if (!slurp_fd(stdout_pipe.read_end, result.stdout_data)) {
                            ::close(stdout_pipe.read_end);
                            stdout_pipe.read_end = -1;
                        }
                        if (pfd.revents & POLLHUP) {
                            ::close(stdout_pipe.read_end);
                            stdout_pipe.read_end = -1;
                        }
                    } else if (pfd.fd == stderr_pipe.read_end) {
                        if (!slurp_fd(stderr_pipe.read_end, result.stderr_data)) {
                            ::close(stderr_pipe.read_end);
                            stderr_pipe.read_end = -1;
                        }
                        if (pfd.revents & POLLHUP) {
                            ::close(stderr_pipe.read_end);
                            stderr_pipe.read_end = -1;
                        }
                    }
                }
                if (pfd.revents & (POLLERR | POLLNVAL)) {
                    if (pfd.fd == stdout_pipe.read_end) {
                        ::close(stdout_pipe.read_end);
                        stdout_pipe.read_end = -1;
                    } else if (pfd.fd == stderr_pipe.read_end) {
                        ::close(stderr_pipe.read_end);
                        stderr_pipe.read_end = -1;
                    }
                }
            }
        }

        // Check if child has exited (non-blocking)
        int status = 0;
        pid_t waited = ::waitpid(pid, &status, WNOHANG);
        if (waited == pid) {
            child_exited = true;
            // Signal monitor IMMEDIATELY after reaping the child,
            // BEFORE the PID can be recycled.  This prevents the
            // monitor from sending SIGKILL to a reused PID.
            monitor_state.done.store(true, std::memory_order_release);

            if (WIFEXITED(status)) {
                result.exit_code = WEXITSTATUS(status);
            } else if (WIFSIGNALED(status)) {
                result.exit_code = -1;   // signalled
            }
        } else if (waited < 0 && errno != EINTR) {
            // waitpid error — break
            break;
        }
    }

    // ---- Drain remaining pipe data after child exits ----
    if (stdout_pipe.read_end >= 0) {
        slurp_fd(stdout_pipe.read_end, result.stdout_data);
    }
    if (stderr_pipe.read_end >= 0) {
        slurp_fd(stderr_pipe.read_end, result.stderr_data);
    }

    // ---- Stop monitor thread ----
    monitor_state.done.store(true, std::memory_order_release);
    monitor_thread.join();

    // ---- Capture monitor outcome ----
    result.timed_out = monitor_state.timed_out.load(std::memory_order_acquire);
    result.cancelled  = monitor_state.cancelled.load(std::memory_order_acquire);

    // ---- Final child reaping (in case monitor killed it) ----
    if (!child_exited) {
        // Poll with timeout instead of blocking forever.
        // A child stuck in D-state (uninterruptible sleep) will never
        // respond to SIGKILL and a blocking waitpid would hang us too.
        auto final_deadline = std::chrono::steady_clock::now()
                              + std::chrono::seconds(30);
        while (std::chrono::steady_clock::now() < final_deadline) {
            int status = 0;
            pid_t waited = ::waitpid(pid, &status, WNOHANG);
            if (waited == pid) {
                if (WIFEXITED(status)) {
                    result.exit_code = WEXITSTATUS(status);
                } else if (WIFSIGNALED(status)) {
                    result.exit_code = -1;
                }
                child_exited = true;
                break;
            }
            if (waited < 0 && errno != EINTR) {
                break;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
        if (!child_exited) {
            result.timed_out = true;
            // Don't block forever — the zombie will be reaped by init
            // when our process exits.
        }
    }

    return Result<ProcessResult>::ok(std::move(result));
}

// =============================================================================
// Public API
// =============================================================================

Result<ProcessResult> ProcessRunner::run(
    const std::vector<std::string>& args,
    std::chrono::milliseconds timeout,
    const CancellationToken* token) {
    return run_impl(args, nullptr, timeout, token);
}

Result<ProcessResult> ProcessRunner::run_with_input(
    const std::vector<std::string>& args,
    const std::string& stdin_data,
    std::chrono::milliseconds timeout,
    const CancellationToken* token) {
    return run_impl(args, &stdin_data, timeout, token);
}

} // namespace installer
