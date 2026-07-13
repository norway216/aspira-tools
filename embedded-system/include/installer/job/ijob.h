/**
 * @file ijob.h
 * @brief Job interface — the top-level unit of work in the installer.
 *
 * A Job encapsulates a complete installation operation (system install,
 * upgrade, backup, restore, repair, etc.). It is submitted to the
 * IJobManager which serialises execution so that only one high-risk
 * (disk-modifying) job runs at a time.
 *
 * @see Architecture Doc §7
 */

#ifndef INSTALLER_JOB_IJOB_H
#define INSTALLER_JOB_IJOB_H

#include <string>

#include "installer/core/result.h"
#include "installer/core/types.h"

namespace installer {

/**
 * Job interface.
 *
 * Represents a single install/upgrade/backup/restore operation. Jobs
 * are stateful: they progress through Idle -> Preparing -> Running ->
 * Completed (or Failed). Some jobs support cancellation and/or
 * resumption after interruption.
 *
 * Thread-safety: start(), resume(), and request_cancel() may be called
 * from different threads; implementations must synchronise access to
 * internal state.
 */
class IJob {
public:
    virtual ~IJob() = default;

    /**
     * Return the unique identifier for this job.
     *
     * The job_id is typically a UUID generated at construction time
     * and is used to correlate log entries and journal records.
     *
     * @return The job's UUID string.
     */
    virtual std::string job_id() const = 0;

    /**
     * Return the type of operation this job performs.
     *
     * @return One of the JobType enum values.
     */
    virtual JobType type() const = 0;

    /**
     * Return the current state of the job.
     *
     * @return The current JobState.
     */
    virtual JobState state() const = 0;

    /**
     * Start executing the job.
     *
     * Transitions from Idle to Preparing, then through each step to
     * Running, and finally to Completed (or Failed). The caller must
     * provide a CancellationToken that the implementation polls
     * periodically; if cancelled, the job transitions to Cancelling
     * and performs any necessary rollback before returning.
     *
     * This method blocks until the job finishes, fails, or is cancelled.
     *
     * @param token CancellationToken polled throughout execution.
     * @return Result<void> — ok on completion, error on failure.
     */
    virtual Result<void> start(CancellationToken& token) = 0;

    /**
     * Request cancellation of a running job.
     *
     * Sets an internal flag that the executing thread checks at its
     * next cancellation point. The job will attempt to reach a safe
     * stopping point before returning from start() or resume().
     *
     * This method is non-blocking; it returns immediately.
     */
    virtual void request_cancel() = 0;

    /**
     * Return whether this job is resumable after interruption.
     *
     * Resumability depends on the job type and on how far execution
     * had progressed before the interruption. Jobs that use the
     * transaction journal can typically be resumed from the last
     * completed step.
     *
     * @return true if resume() is expected to succeed.
     */
    virtual bool can_resume() const = 0;

    /**
     * Resume a previously interrupted job.
     *
     * Reads the transaction journal to determine the last completed
     * step and continues execution from that point. The job must be
     * in the Recoverable state for this call to succeed.
     *
     * @param token CancellationToken polled throughout execution.
     * @return Result<void> — ok on completion, error on failure.
     */
    virtual Result<void> resume(CancellationToken& token) = 0;

    /**
     * Return the current progress percentage (0–100).
     *
     * May be called from any thread while the job is running.
     *
     * @return Integer in [0, 100].
     */
    virtual int progress_percent() const = 0;

    /**
     * Return a human-readable description of the currently executing step.
     *
     * Examples: "Verifying package signature", "Writing root filesystem",
     * "Configuring bootloader".
     *
     * @return Current step name, or empty string if not started.
     */
    virtual std::string current_step_name() const = 0;

    /**
     * Install a progress callback that is invoked at regular intervals
     * during execution.
     *
     * The callback receives a ProgressInfo struct with percent, current
     * step description, bytes processed, and throughput.
     *
     * @param callback A ProgressCallback (std::function). Pass nullptr
     *                 or an empty function to clear.
     */
    virtual void set_progress_callback(ProgressCallback callback) = 0;
};

} // namespace installer

#endif // INSTALLER_JOB_IJOB_H
