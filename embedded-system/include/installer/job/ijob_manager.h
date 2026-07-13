/**
 * @file ijob_manager.h
 * @brief Job manager — serialises high-risk operations.
 *
 * The job manager is the central scheduler for installation work.
 * It enforces the critical invariant that only ONE high-risk
 * (disk-modifying) job executes at any given time, preventing race
 * conditions on block devices, partition tables, and boot environments.
 *
 * @see Architecture Doc §7.1
 */

#ifndef INSTALLER_JOB_IJOB_MANAGER_H
#define INSTALLER_JOB_IJOB_MANAGER_H

#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "installer/core/result.h"
#include "installer/core/types.h"

namespace installer {

// Forward declaration — IJob is defined in job/ijob.h.
class IJob;

/**
 * Job manager — serialises high-risk disk operations.
 *
 * Accepts job submissions and runs them one at a time. While a
 * high-risk job (InstallSystem, UpgradeSystem, RestoreData, RepairSystem)
 * is running, subsequent submissions are queued. Read-only jobs
 * (VerifyPackage, ExportLogs) may run concurrently.
 *
 * Event Callback: clients can register a callback that fires whenever
 * a job's state changes (started, progress update, completed, failed).
 * This replaces the need for polling in UI / IPC layers.
 */
class IJobManager {
public:
    virtual ~IJobManager() = default;

    /**
     * Callback signature for job state-change events.
     *
     * @param job_id  The job that changed state.
     * @param state   The new JobState.
     * @param percent Current progress (0–100).
     * @param step    Human-readable description of the current step
     *                (empty string if not applicable).
     */
    using EventCallback = std::function<void(
        const std::string& job_id,
        JobState state,
        int percent,
        const std::string& step)>;

    /**
     * Submit a job for execution and return its assigned job_id.
     *
     * The manager takes ownership of @p job. If no high-risk job is
     * currently running, execution begins immediately (synchronously
     * or on a worker thread, depending on the implementation). If a
     * high-risk job is already running, the submitted job is queued.
     *
     * The @p callback is installed on the job and will receive progress
     * updates during execution.
     *
     * @param job      Unique pointer to the job (ownership transferred).
     * @param callback Progress callback installed on the job.
     * @return The job_id (UUID string) assigned to the submitted job,
     *         or an error if the submission itself fails.
     */
    virtual Result<std::string> submit_job(
        std::unique_ptr<IJob> job,
        ProgressCallback callback) = 0;

    /**
     * Cancel a running or queued job.
     *
     * If the job is running, this triggers a graceful cancellation —
     * the job will finish its current step and roll back before
     * returning. If the job is queued, it is removed from the queue.
     *
     * @param job_id The job to cancel.
     * @return Result<void> — ok if cancellation was requested
     *         successfully, error if the job_id is unknown.
     */
    virtual Result<void> cancel_job(const std::string& job_id) = 0;

    /**
     * Get the current state of a job.
     *
     * @param job_id The job to query.
     * @return The JobState, or an error if job_id is unknown.
     */
    virtual Result<JobState> get_job_state(
        const std::string& job_id) = 0;

    /**
     * Get the current progress of a job.
     *
     * @param job_id The job to query.
     * @return Progress percentage (0–100), or an error if unknown.
     */
    virtual Result<int> get_job_progress(
        const std::string& job_id) = 0;

    /**
     * Return the list of job_ids for all jobs that have not yet reached
     * Completed, Failed, or Aborted state.
     *
     * Used after a restart to discover jobs that might need resumption.
     *
     * @return Vector of job_id strings (may be empty).
     */
    virtual std::vector<std::string> get_unfinished_jobs() = 0;

    /**
     * Resume a previously interrupted job from its last completed step.
     *
     * The job must be in the Recoverable state (which implies it was
     * previously started, was interrupted, and is safe to resume).
     *
     * @param job_id   The job to resume.
     * @param callback Progress callback installed on the resumed job.
     * @return Result<void> — ok if resumption started successfully.
     */
    virtual Result<void> resume_job(const std::string& job_id,
                                    ProgressCallback callback) = 0;

    /**
     * Return whether a high-risk (disk-modifying) job is currently
     * executing.
     *
     * @return true if a high-risk job is running, false if idle or
     *         only read-only jobs are active.
     */
    virtual bool is_busy() const = 0;

    /**
     * Register a callback that fires on every job state transition.
     *
     * This is the primary mechanism for UI and IPC layers to stay
     * informed without polling. The callback receives the job_id,
     * new state, progress percentage, and current step name.
     *
     * Only one callback can be registered at a time; calling this
     * method replaces any previously registered callback.
     *
     * @param callback The event callback (pass empty function to clear).
     */
    virtual void set_event_callback(EventCallback callback) = 0;
};

} // namespace installer

#endif // INSTALLER_JOB_IJOB_MANAGER_H
