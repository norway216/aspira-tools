/**
 * @file IJobStep.h
 * @brief Single step within a job's state machine chain.
 */

#ifndef INSTALLER_IJOBSTEP_H
#define INSTALLER_IJOBSTEP_H

#include "installer/core/types.h"
#include "installer/core/result.h"
#include <string>

namespace installer {

class IJobStep {
public:
    virtual ~IJobStep() = default;

    virtual std::string step_id() const = 0;
    virtual std::string description() const = 0;
    virtual int weight_percent() const = 0;

    // Pre-flight checks; called on initial execution and resume
    virtual Result<void> prepare(JobContext& ctx) = 0;

    // Perform the actual work; receives progress callback and cancellation token
    virtual Result<void> execute(JobContext& ctx,
                                 ProgressCallback progress,
                                 CancellationToken& cancel) = 0;

    // Post-execution verification
    virtual Result<void> verify(JobContext& ctx,
                                ProgressCallback progress,
                                CancellationToken& cancel) = 0;

    // Best-effort undo; called on failure of execute or verify
    virtual Result<void> rollback(JobContext& ctx) = 0;

    // Whether it is safe to resume this step after a crash
    virtual bool can_resume() const = 0;
};

} // namespace installer

#endif // INSTALLER_IJOBSTEP_H
