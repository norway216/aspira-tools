/**
 * @file IProcessRunner.h
 * @brief External process execution interface.
 */

#ifndef INSTALLER_IPROCESSRUNNER_H
#define INSTALLER_IPROCESSRUNNER_H

#include "installer/core/types.h"
#include "installer/core/result.h"
#include <chrono>
#include <string>
#include <vector>

namespace installer {

struct ProcessResult {
    int exit_code = -1;
    std::string stdout_output;
    std::string stderr_output;
    bool timed_out = false;
};

class IProcessRunner {
public:
    virtual ~IProcessRunner() = default;

    virtual Result<ProcessResult> run(
        const std::vector<std::string>& args,
        std::chrono::milliseconds timeout = std::chrono::milliseconds(0),
        const std::string& input = "") = 0;

    virtual Result<ProcessResult> run_shell(const std::string& command) = 0;
};

} // namespace installer

#endif // INSTALLER_IPROCESSRUNNER_H
