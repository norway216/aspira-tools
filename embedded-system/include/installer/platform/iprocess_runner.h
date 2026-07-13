/**
 * @file iprocess_runner.h
 * @brief Safe process execution interface — shell-free by design.
 *
 * Every external program invocation goes through this interface so that
 * execution can be monitored, timed out, and cancelled uniformly. The
 * program path and its arguments are always kept separate — the
 * implementation MUST NEVER construct a shell command line.
 *
 * @see Architecture Doc §21
 */

#ifndef INSTALLER_PLATFORM_IPROCESS_RUNNER_H
#define INSTALLER_PLATFORM_IPROCESS_RUNNER_H

#include <chrono>
#include <string>
#include <unordered_map>
#include <vector>

#include "installer/core/result.h"
#include "installer/core/types.h"

namespace installer {

/**
 * Arguments for a single process invocation.
 *
 * The program and its arguments are kept separate from each other and
 * from the shell. Implementations must use fork+exec (or posix_spawn),
 * never system() or popen().
 */
struct ProcessArgs {
    /** Full path or resolvable name of the executable. */
    std::string program;

    /** Argument vector (becomes argv[1..n] in the child).
     *  Does NOT include argv[0] (the program name). */
    std::vector<std::string> args;

    /** Maximum wall-clock time the process is allowed to run.
     *  A value of zero disables the timeout. */
    std::chrono::milliseconds timeout{0};

    /** Additional environment variables for the child process.
     *  These augment (do not replace) the parent's environment. */
    std::unordered_map<std::string, std::string> env;

    /** Working directory for the child process.
     *  An empty string means inherit the parent's current directory. */
    std::string work_dir;
};

/**
 * Result of a completed, timed-out, or cancelled process invocation.
 */
struct ProcessResult {
    /** Exit code returned by the process. Only meaningful when both
     *  timed_out and cancelled are false. */
    int exit_code = 0;

    /** Captured standard output (stdout). */
    std::string stdout_output;

    /** Captured standard error (stderr). */
    std::string stderr_output;

    /** True if the process was terminated because its timeout expired. */
    bool timed_out = false;

    /** True if the process was terminated due to a cancellation request. */
    bool cancelled = false;
};

/**
 * Safe process execution interface.
 *
 * All external process launches are channelled through this interface so
 * they can be monitored, timed out, and cancelled in a uniform way.
 * The implementation MUST NOT invoke a shell under any circumstances;
 * program and arguments are always passed separately to the kernel.
 */
class IProcessRunner {
public:
    virtual ~IProcessRunner() = default;

    /**
     * Run an external program and wait for it to exit.
     *
     * The implementation must:
     * - Use fork+exec (or posix_spawn) with separate program/args.
     * - Capture stdout and stderr into separate buffers.
     * - Enforce the timeout specified in @p args.
     * - Periodically check @p token and terminate the child process
     *   (SIGTERM then SIGKILL) if cancellation is requested.
     *
     * @param args  Fully specified process arguments (program, args,
     *              timeout, environment, working directory).
     * @param token Cancellation token checked periodically during
     *              execution.
     * @return ProcessResult on success, or an InstallerError if the
     *         process could not be launched at all (e.g. ENOENT).
     */
    virtual Result<ProcessResult> run(const ProcessArgs& args,
                                      CancellationToken& token) = 0;
};

} // namespace installer

#endif // INSTALLER_PLATFORM_IPROCESS_RUNNER_H
