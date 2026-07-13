/**
 * @file base_job.cpp
 * @brief Implementation of the step-chain state machine for BaseJob.
 *
 * The run_chain() method is the core of the state machine. For each step:
 *   1. Check cancellation -> return E9003 (cancelled).
 *   2. Check if step already completed in journal -> skip.
 *   3. Call step->prepare(ctx) — pre-flight checks.
 *   4. Update journal state to this step (atomic save).
 *   5. Call step->execute(ctx, progress, cancel) — do the work.
 *   6. Call step->verify(ctx, progress, cancel) — validate result.
 *   7. On failure: call step->rollback(ctx), return error.
 *   8. Mark step complete in journal (atomic save).
 *   9. Update overall progress based on step weights.
 */

#include "src/job/base_job.h"
#include "src/journal/transaction_journal.h"

#include "installer/ITransactionJournal.h"
#include <sstream>

namespace installer {

// ============================================================================
//  Construction
// ============================================================================

BaseJob::BaseJob(const std::string& job_id, JobType type,
                 std::shared_ptr<ILogger> logger)
    : job_id_(job_id)
    , type_(type)
    , logger_(std::move(logger))
{
}

// ============================================================================
//  IJob — simple accessors
// ============================================================================

std::string BaseJob::job_id() const {
    return job_id_;
}

JobType BaseJob::type() const {
    return type_;
}

JobState BaseJob::state() const {
    return state_.load(std::memory_order_acquire);
}

void BaseJob::request_cancel() {
    logger_->log(LogLevel::Warn, "BaseJob", "request_cancel" + std::string(": ") + "Cancellation requested", job_id_);
    internal_cancel_.cancel();
    state_.store(JobState::Cancelling, std::memory_order_release);
}

bool BaseJob::can_resume() const {
    // A job can resume if it has a journal with an incomplete entry
    // and each remaining step reports can_resume() == true.
    if (!ctx_.journal) return false;

    auto result = ctx_.journal->get(job_id_);
    if (!result.is_ok()) return false;

    const auto& entry = result.value();
    if (entry.state == JournalState::Complete ||
        entry.state == JournalState::Aborted) {
        return false;
    }

    if (!entry.safe_to_resume) return false;

    // Check each incomplete step.
    for (size_t i = 0; i < steps_.size(); ++i) {
        bool already_done = false;
        for (const auto& done_step : entry.completed_steps) {
            if (done_step == steps_[i]->step_id()) {
                already_done = true;
                break;
            }
        }
        if (!already_done && !steps_[i]->can_resume()) {
            return false;
        }
    }

    return true;
}

Result<void> BaseJob::resume(CancellationToken& cancel) {
    // Check cancellation first.
    if (cancel.is_cancelled() || internal_cancel_.is_cancelled()) {
        return Result<void>::err(InstallerError::make(
            ErrorCode::INTERNAL_CANCELLED,
            "Job Cancelled",
            "The job was cancelled before it could be resumed",
            "job_id=" + job_id_));
    }

    // Resume just restarts the chain from the beginning; steps that
    // were already completed will be skipped by run_chain().
    return run_chain(ctx_, cancel);
}

int BaseJob::progress_percent() const {
    return progress_.load(std::memory_order_acquire);
}

std::string BaseJob::current_step_name() const {
    size_t idx = current_step_index_.load(std::memory_order_acquire);
    if (idx < steps_.size()) {
        return steps_[idx]->description();
    }
    return "complete";
}

void BaseJob::set_progress_callback(ProgressCallback cb) {
    std::lock_guard<std::mutex> lock(state_mutex_);
    progress_callback_ = std::move(cb);
}

// ============================================================================
//  Default start() — derived classes should override to set up ctx_
// ============================================================================

Result<void> BaseJob::start(CancellationToken& cancel) {
    if (cancel.is_cancelled() || internal_cancel_.is_cancelled()) {
        return Result<void>::err(InstallerError::make(
            ErrorCode::INTERNAL_CANCELLED,
            "Job Cancelled",
            "The job was cancelled before it started",
            "job_id=" + job_id_));
    }
    return run_chain(ctx_, cancel);
}

// ============================================================================
//  Protected API
// ============================================================================

void BaseJob::add_step(std::unique_ptr<IJobStep> step) {
    steps_.push_back(std::move(step));
}

Result<void> BaseJob::run_chain(JobContext& ctx, CancellationToken& cancel) {
    std::lock_guard<std::mutex> lock(state_mutex_);

    JobState current = state_.load(std::memory_order_acquire);

    // Only start from Idle or Recoverable.
    if (current != JobState::Idle && current != JobState::Recoverable) {
        return Result<void>::err(InstallerError::make(
            ErrorCode::INTERNAL_INVALID_STATE,
            "Invalid State",
            "Job is not in a startable state",
            "job_id=" + job_id_ + " state=" + job_state_name(current)));
    }

    log_state_transition(current, JobState::Running);
    state_.store(JobState::Running, std::memory_order_release);

    // Determine which steps are already completed (for resume).
    std::vector<std::string> completed_steps;
    if (ctx.journal) {
        auto tx_result = ctx.journal->get(job_id_);
        if (tx_result.is_ok()) {
            completed_steps = tx_result.value().completed_steps;
        }
    }

    // Helper: check if a step is already done.
    auto is_step_done = [&completed_steps](const std::string& step_id) -> bool {
        for (const auto& s : completed_steps) {
            if (s == step_id) return true;
        }
        return false;
    };

    // Compute cumulative weight for progress calculation.
    int total_weight = 0;
    for (const auto& step : steps_) {
        total_weight += step->weight_percent();
    }
    if (total_weight == 0) total_weight = 1;  // avoid division by zero

    int cumulative_weight = 0;

    for (size_t i = 0; i < steps_.size(); ++i) {
        auto& step = steps_[i];
        current_step_index_.store(i, std::memory_order_release);

        // ---- 1. Check cancellation ----
        if (cancel.is_cancelled() || internal_cancel_.is_cancelled()) {
            log_state_transition(JobState::Running, JobState::Failed);
            state_.store(JobState::Failed, std::memory_order_release);
            return Result<void>::err(InstallerError::make(
                ErrorCode::INTERNAL_CANCELLED,
                "Job Cancelled",
                "The job was cancelled by user request",
                "job_id=" + job_id_ + " step=" + step->step_id(),
                false));
        }

        // ---- 2. Check if step already completed in journal ----
        if (is_step_done(step->step_id())) {
            logger_->log(LogLevel::Info, "BaseJob", "skip_step" + std::string(": ") + "step=" + step->step_id(, job_id_) + " (already completed)");
            cumulative_weight += step->weight_percent();
            update_overall_progress(i);
            continue;
        }

        logger_->log(LogLevel::Info, "BaseJob", "begin_step" + std::string(": ") + "step=" + step->step_id(, job_id_) + " desc=" + step->description());

        // ---- 3. Prepare ----
        {
            auto result = step->prepare(ctx);
            if (!result.is_ok()) {
                logger_->log(LogLevel::Error, "BaseJob", "step_prepare_failed" + std::string(": ") + "step=" + step->step_id(, job_id_) +
                                   " error=" + result.error().code);
                log_state_transition(JobState::Running, JobState::Failed);
                state_.store(JobState::Failed, std::memory_order_release);
                return result;
            }
        }

        // ---- 4. Update journal state to this step ----
        if (ctx.journal) {
            // Map step IDs to journal states for coarse-grained tracking.
            JournalState js = JournalState::VerifyPackage; // fallback
            auto sid = step->step_id();
            if (sid == "detect_hardware" || sid == "check_compatibility" ||
                sid == "check_storage")
                js = JournalState::CheckTarget;
            else if (sid == "prepare_partitions")
                js = JournalState::PreparePartitions;
            else if (sid == "create_filesystems")
                js = JournalState::PreparePartitions;
            else if (sid == "write_bootloader")
                js = JournalState::WriteBootloader;
            else if (sid == "write_kernel")
                js = JournalState::WriteKernel;
            else if (sid == "write_rootfs")
                js = JournalState::WriteRootfs;
            else if (sid == "verify_target")
                js = JournalState::VerifyTarget;
            else if (sid == "configure_boot_slot")
                js = JournalState::ConfigureBoot;

            auto journal_result = static_cast<TransactionJournal*>(ctx.journal)->update_state(job_id_, js, progress_.load());
            if (!journal_result.is_ok()) {
                logger_->log(LogLevel::Error, "BaseJob", "journal_update_failed" + std::string(": ") + "step=" + step->step_id(, job_id_));
            }
        }

        // ---- 5. Execute ----
        {
            auto progress_wrapper = [this, i, total_weight, &cumulative_weight]
                (const ProgressInfo& info) {
                // Report current step's progress, scaled by its weight.
                int step_pct = info.percent;
                int base = (cumulative_weight * 100) / total_weight;
                int step_range = (steps_[i]->weight_percent() * 100) / total_weight;
                int overall = base + (step_pct * step_range) / 100;
                if (overall > 100) overall = 100;
                if (overall < 0) overall = 0;
                progress_.store(overall, std::memory_order_release);

                ProgressInfo wrapped = info;
                wrapped.percent = overall;
                wrapped.step_description = steps_[i]->description();

                if (progress_callback_) {
                    progress_callback_(wrapped);
                }
            };

            auto result = step->execute(ctx, progress_wrapper, cancel);
            if (!result.is_ok()) {
                logger_->log(LogLevel::Error, "BaseJob", "step_execute_failed" + std::string(": ") + "step=" + step->step_id(, job_id_) +
                                   " error=" + result.error().code);

                // Attempt rollback (best-effort).
                auto rb_result = step->rollback(ctx);
                if (!rb_result.is_ok()) {
                    logger_->log(LogLevel::Warn, "BaseJob", "step_rollback_failed" + std::string(": ") + "step=" + step->step_id(, job_id_));
                }

                log_state_transition(JobState::Running, JobState::Failed);
                state_.store(JobState::Failed, std::memory_order_release);
                return result;
            }
        }

        // ---- 6. Verify ----
        {
            auto result = step->verify(ctx, progress_callback_, cancel);
            if (!result.is_ok()) {
                logger_->log(LogLevel::Error, "BaseJob", "step_verify_failed" + std::string(": ") + "step=" + step->step_id(, job_id_) +
                                   " error=" + result.error().code);

                auto rb_result = step->rollback(ctx);
                if (!rb_result.is_ok()) {
                    logger_->log(LogLevel::Warn, "BaseJob", "step_rollback_failed" + std::string(": ") + "step=" + step->step_id(, job_id_));
                }

                log_state_transition(JobState::Running, JobState::Failed);
                state_.store(JobState::Failed, std::memory_order_release);
                return result;
            }
        }

        // ---- 7. Mark step complete in journal ----
        if (ctx.journal) {
            auto journal_result = static_cast<TransactionJournal*>(ctx.journal)->mark_step_complete(
                job_id_, step->step_id());
            if (!journal_result.is_ok()) {
                logger_->log(LogLevel::Error, "BaseJob", "journal_mark_failed" + std::string(": ") + "step=" + step->step_id(, job_id_));
            }
        }

        // ---- 8. Update overall progress ----
        cumulative_weight += step->weight_percent();
        update_overall_progress(i);

        logger_->log(LogLevel::Info, "BaseJob", "complete_step" + std::string(": ") + "step=" + step->step_id(, job_id_));
    }

    // All steps completed successfully.
    log_state_transition(JobState::Running, JobState::Completed);
    state_.store(JobState::Completed, std::memory_order_release);
    progress_.store(100, std::memory_order_release);

    // Commit the transaction.
    if (ctx.journal) {
        auto commit_result = ctx.journal->commit(job_id_);
        if (!commit_result.is_ok()) {
            logger_->log(LogLevel::Error, "BaseJob", "journal_commit_failed" + std::string(": ") + "Transaction commit failed", job_id_);
        }
    }

    return Result<void>::ok();
}

// ============================================================================
//  Private helpers
// ============================================================================

void BaseJob::update_overall_progress(size_t completed_step_index) {
    int total_weight = 0;
    int cumulative_weight = 0;
    for (size_t i = 0; i < steps_.size(); ++i) {
        total_weight += steps_[i]->weight_percent();
        if (i <= completed_step_index) {
            cumulative_weight += steps_[i]->weight_percent();
        }
    }
    if (total_weight == 0) total_weight = 1;
    int pct = (cumulative_weight * 100) / total_weight;
    if (pct > 100) pct = 100;
    if (pct < 0) pct = 0;
    progress_.store(pct, std::memory_order_release);
}

void BaseJob::log_state_transition(JobState from, JobState to) {
    if (logger_) {
        std::ostringstream oss;
        oss << "State transition: " << job_state_name(from)
            << " -> " << job_state_name(to);
        logger_->log(LogLevel::Info, "BaseJob", "state_transition" + std::string(": ") + oss.str(, job_id_));
    }
}

} // namespace installer
