/**
 * @file process_runner.h
 * @brief IProcessRunner implementation using fork+execvp with timeout and cancellation.
 *
 * No shell is ever invoked — arguments are passed directly to execvp().
 * stdout and stderr are captured via pipes.  A dedicated timer thread
 * enforces the timeout (SIGTERM first, then SIGKILL after a 5-second grace
 * period).  The CancellationToken is checked before fork and can also
 * interrupt the child mid-flight.
 */

#ifndef INSTALLER_PLATFORM_PROCESS_RUNNER_H
#define INSTALLER_PLATFORM_PROCESS_RUNNER_H

#include "installer/platform/iprocess_runner.h"
#include <memory>

namespace installer {

class ProcessRunner : public IProcessRunner {
public:
    ProcessRunner() = default;
    ~ProcessRunner() override = default;

    Result<ProcessResult> run(
        const std::vector<std::string>& args,
        std::chrono::milliseconds timeout,
        const CancellationToken* token = nullptr) override;

    Result<ProcessResult> run_with_input(
        const std::vector<std::string>& args,
        const std::string& stdin_data,
        std::chrono::milliseconds timeout,
        const CancellationToken* token = nullptr) override;

private:
    /**
     * Internal implementation shared by run() and run_with_input().
     *
     * @param args       Command and arguments.
     * @param stdin_data If present, written to child's stdin before EOF.
     * @param timeout    Maximum wall-clock duration.
     * @param token      Optional cancellation token.
     * @return           Populated ProcessResult or error.
     */
    static Result<ProcessResult> run_impl(
        const std::vector<std::string>& args,
        const std::string* stdin_data,
        std::chrono::milliseconds timeout,
        const CancellationToken* token);
};

} // namespace installer

#endif // INSTALLER_PLATFORM_PROCESS_RUNNER_H
