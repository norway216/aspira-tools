/**
 * @file ijob_step.h
 * @brief Job step plugin interface.
 *
 * A Job is composed of a sequence of IJobStep instances, each
 * representing a discrete phase of work (verify package, write
 * bootloader, format partitions, etc.). Steps follow a standard
 * lifecycle: prepare -> execute -> verify, with rollback if needed.
 *
 * @see Architecture Doc §7.2
 */

#ifndef INSTALLER_JOB_IJOB_STEP_H
#define INSTALLER_JOB_IJOB_STEP_H

#include <string>

#include "installer/core/result.h"
#include "installer/core/types.h"

namespace installer {

/**
 * Job step plugin interface.
 *
 * Each step is an independent, testable unit of work. Steps receive a
 * JobContext that provides access to all injected services (logger,
 * device manager, partition manager, etc.) and the job's configuration
 * (target device, slot, package path).
 *
 * Steps contribute a weight_percent that determines their share of the
 * overall job progress bar. For example, a 5-step job might assign
 * weights of 5, 10, 60, 15, and 10 percent respectively.
 */
class IJobStep {
public:
    virtual ~IJobStep() = default;

    /**
     * Return a machine-readable identifier for this step.
     *
     * Examples: "verify_package", "write_bootloader", "format_data".
     * Used for journal step-tracking and log correlation.
     *
     * @return The step identifier string.
     */
    virtual std::string step_id() const = 0;

    /**
     * Return a human-readable description of what this step does.
     *
     * Displayed in the UI during execution (e.g. "Writing root
     * filesystem to slot B").
     *
     * @return A user-facing description string.
     */
    virtual std::string description() const = 0;

    /**
     * Return this step's contribution to the overall job progress,
     * expressed as a percentage of the total (0–100).
     *
     * The sum of weight_percent across all steps in a job should equal
     * 100. The job uses these weights to scale each step's internal
     * progress into the overall job progress.
     *
     * @return Weight in percent (e.g. 20 for a step that represents
     *         20% of the total job work).
     */
    virtual int weight_percent() const = 0;

    /**
     * Prepare the step for execution.
     *
     * Performs pre-flight checks and allocates resources. Called once
     * before execute(). If prepare() fails, execute() will not be called
     * and rollback() is invoked to release any partially acquired
     * resources.
     *
     * @param ctx The job context with configuration and injected services.
     * @return Result<void> — ok if ready, error on failure.
     */
    virtual Result<void> prepare(JobContext& ctx) = 0;

    /**
     * Execute the step's primary work.
     *
     * This is the main body of the step. The implementation must:
     * - Report progress via @p callback.
     * - Poll @p token periodically and return INTERNAL_CANCELLED if
     *   cancellation is requested.
     * - Leave the system in a consistent state on failure so that
     *   rollback() can undo the partial work.
     *
     * @param ctx      The job context.
     * @param callback Progress reporting callback.
     * @param token    Cancellation token.
     * @return Result<void> — ok on success, error on failure.
     */
    virtual Result<void> execute(JobContext& ctx,
                                 ProgressCallback callback,
                                 CancellationToken& token) = 0;

    /**
     * Verify that the step's work was performed correctly.
     *
     * Called after a successful execute(). Performs integrity checks
     * (e.g. verify written data, check partition layout, validate
     * filesystem). If verify() fails, rollback() may be invoked
     * depending on the job's error-handling policy.
     *
     * @param ctx      The job context.
     * @param callback Progress reporting callback.
     * @param token    Cancellation token.
     * @return Result<void> — ok if verification passes, error on failure.
     */
    virtual Result<void> verify(JobContext& ctx,
                                ProgressCallback callback,
                                CancellationToken& token) = 0;

    /**
     * Roll back any changes made by this step.
     *
     * Called when execute() or verify() fails, or when the job is
     * cancelled mid-step. The implementation should restore the system
     * to its pre-step state as much as possible.
     *
     * Rollback is best-effort: if rollback itself fails, the error is
     * logged but does not prevent other steps' rollbacks from running.
     *
     * @param ctx The job context.
     * @return Result<void> — ok if rollback succeeded, error describing
     *         what could not be undone.
     */
    virtual Result<void> rollback(JobContext& ctx) = 0;

    /**
     * Return whether this step can be resumed if interrupted.
     *
     * Steps that are idempotent or that record their progress in the
     * transaction journal typically return true. Steps whose work
     * cannot be safely repeated (e.g. partition table creation that
     * is not idempotent) return false.
     *
     * @return true if the step can safely resume from the last
     *         completed sub-step.
     */
    virtual bool can_resume() const = 0;
};

} // namespace installer

#endif // INSTALLER_JOB_IJOB_STEP_H
