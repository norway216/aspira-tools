/**
 * @file base_job.h
 * @brief Base class implementing IJob with the step-chain state machine.
 *
 * Derived classes create their step chain in the constructor and
 * override start() to set up JobContext before calling run_chain().
 *
 * The step chain drives the job through:
 *   prepare -> execute -> verify  for each step,
 * with journal checkpointing between steps for crash recovery.
 */

#ifndef INSTALLER_BASE_JOB_H
#define INSTALLER_BASE_JOB_H

#include "installer/IJob.h"
#include "installer/IJobStep.h"
#include "installer/log/ilogger.h"
#include "installer/ITransactionJournal.h"
#include <atomic>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace installer {

class BaseJob : public IJob {
public:
    BaseJob(const std::string& job_id, JobType type,
            std::shared_ptr<ILogger> logger);

    ~BaseJob() override = default;

    // ---- IJob interface ----
    std::string job_id() const override;
    JobType type() const override;
    JobState state() const override;
    Result<void> start(CancellationToken& cancel) override;
    void request_cancel() override;
    bool can_resume() const override;
    Result<void> resume(CancellationToken& cancel) override;
    int progress_percent() const override;
    std::string current_step_name() const override;
    void set_progress_callback(ProgressCallback cb) override;

    // Set the transaction journal pointer in the job context.
    // Called by JobManager when a job is submitted or resumed.
    void set_journal(ITransactionJournal* journal) { ctx_.journal = journal; }

protected:
    // Add a step to the chain. Called by derived class constructors.
    void add_step(std::unique_ptr<IJobStep> step);

    // Execute the full step chain from start to finish.
    // Respects journal checkpoints: skips steps that were already completed.
    // Returns error on cancellation or step failure.
    Result<void> run_chain(JobContext& ctx, CancellationToken& cancel);

    // Step list, populated by derived class constructor.
    std::vector<std::unique_ptr<IJobStep>> steps_;

    // Context shared across all steps.
    JobContext ctx_;

    // Current job state (atomic for lock-free reads).
    std::atomic<JobState> state_{JobState::Idle};

    // Overall progress 0-100 (atomic for lock-free reads).
    std::atomic<int> progress_{0};

    // Index into steps_ of the currently executing step.
    std::atomic<size_t> current_step_index_{0};

    // Optional progress callback for UI updates.
    ProgressCallback progress_callback_;

    // Logger.
    std::shared_ptr<ILogger> logger_;

    // Mutex protecting state transitions.
    std::mutex state_mutex_;

private:
    std::string job_id_;
    JobType type_;

    // Internal cancellation token triggered by request_cancel().
    CancellationToken internal_cancel_;

    // Compute overall progress after a step completes.
    void update_overall_progress(size_t completed_step_index);

    // Log a state transition.
    void log_state_transition(JobState from, JobState to);
};

} // namespace installer

#endif // INSTALLER_BASE_JOB_H
