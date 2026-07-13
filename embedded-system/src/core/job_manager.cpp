/**
 * @file job_manager.cpp
 * @brief Implementation of the central job orchestrator.
 *
 * Key design decisions:
 *   - Single worker thread processes one job at a time.
 *   - start_install/start_backup/start_restore fail if a job is already running.
 *   - Cancellation token is shared between manager and job.
 *   - Progress events are forwarded via EventCallback for IPC broadcasting.
 *   - On startup, the journal is checked for unfinished jobs.
 */

#include "src/core/job_manager.h"
#include "src/journal/transaction_journal.h"

#include "installer/IJob.h"
#include "installer/ITransactionJournal.h"
#include "installer/IDeviceManager.h"
#include "installer/IPackageManager.h"
#include "installer/image/iimage_writer.h"
#include "installer/partition/ipartition_manager.h"
#include "installer/filesystem/ifilesystem_manager.h"
#include "installer/IBootControl.h"
#include "installer/security/isecurity_manager.h"

#include "src/job/install_job.h"

#include <nlohmann/json.hpp>

#include <random>
#include <sstream>

using json = nlohmann::json;

namespace installer {

// ============================================================================
//  UUID generation
// ============================================================================

static std::string generate_uuid_impl() {
    static std::random_device rd;
    static std::mt19937_64 gen(rd());
    static std::uniform_int_distribution<uint64_t> dis;

    std::ostringstream oss;
    oss << std::hex << std::setfill('0');
    oss << std::setw(8) << (dis(gen) & 0xFFFFFFFF) << "-";
    oss << std::setw(4) << ((dis(gen) >> 16) & 0xFFFF) << "-";
    oss << std::setw(4) << ((dis(gen) & 0x0FFF) | 0x4000) << "-";
    oss << std::setw(4) << ((dis(gen) & 0x3FFF) | 0x8000) << "-";
    oss << std::setw(12) << (dis(gen) & 0xFFFFFFFFFFFFULL);
    return oss.str();
}

std::string JobManager::generate_uuid() {
    return generate_uuid_impl();
}

// ============================================================================
//  Construction / Destruction
// ============================================================================

JobManager::JobManager(std::shared_ptr<ITransactionJournal> journal,
                       std::shared_ptr<ILogger> logger,
                       std::shared_ptr<IDeviceManager> dev_mgr,
                       std::shared_ptr<IPackageManager> pkg_mgr,
                       std::shared_ptr<IImageWriter> image_writer,
                       std::shared_ptr<IPartitionManager> part_mgr,
                       std::shared_ptr<IFilesystemManager> fs_mgr,
                       std::shared_ptr<IBootControl> boot_ctrl,
                       std::shared_ptr<ISecurityManager> sec_mgr)
    : impl_(std::make_unique<Impl>())
{
    impl_->journal = std::move(journal);
    impl_->logger = std::move(logger);
    impl_->dev_mgr = std::move(dev_mgr);
    impl_->pkg_mgr = std::move(pkg_mgr);
    impl_->image_writer = std::move(image_writer);
    impl_->part_mgr = std::move(part_mgr);
    impl_->fs_mgr = std::move(fs_mgr);
    impl_->boot_ctrl = std::move(boot_ctrl);
    impl_->sec_mgr = std::move(sec_mgr);

    // Start the worker thread.
    impl_->running.store(true, std::memory_order_release);
    impl_->worker = std::thread(&JobManager::worker_loop, this);

    impl_->logger->log(LogLevel::Info, "JobManager", "Worker thread running");

    // Check for unfinished jobs on startup.
    auto unfinished_result = impl_->journal->find_incomplete();
    if (unfinished_result.is_ok() && !unfinished_result.value().empty()) {
        auto& unfinished = unfinished_result.value();
        impl_->logger->log(LogLevel::Warn, "JobManager",
                            "Found " + std::to_string(unfinished.size()) +
                            " unfinished job(s) from previous session");
        for (const auto& entry : unfinished) {
            impl_->logger->log(LogLevel::Warn, "JobManager",
                                "  - " + entry.transaction_id +
                                " state=" + journal_state_name(entry.state) +
                                " operation=" + entry.operation +
                                " safe_to_resume=" + (entry.safe_to_resume ? "true" : "false"));

            // Register in records so they show up in list_jobs().
            std::lock_guard<std::mutex> rec_lock(impl_->records_mutex);
            impl_->job_records[entry.transaction_id] = {
                entry.safe_to_resume ? JobState::Recoverable : JobState::Failed,
                entry.progress,
                journal_state_name(entry.state)
            };
        }
    }
}

JobManager::~JobManager() {
    shutdown();
    if (impl_->worker.joinable()) {
        impl_->worker.join();
    }
}

// ============================================================================
//  IJobManager interface
// ============================================================================

void JobManager::set_event_callback(EventCallback cb) {
    std::lock_guard<std::mutex> lock(impl_->event_mutex);
    impl_->event_callback = std::move(cb);
}

Result<std::string> JobManager::start_install(const std::string& package_path,
                                               const std::string& target_device,
                                               const std::string& target_slot) {
    // Check if a job is already in progress.
    if (is_busy()) {
        return Result<std::string>::err(InstallerError::make(
            ErrorCode::INTERNAL_ERROR,
            "Job In Progress",
            "Another job is already running. Cancel it first or wait for completion.",
            "",
            true));  // retryable
    }

    std::string job_id = generate_uuid();

    impl_->logger->log(LogLevel::Info, "JobManager", "start_install: package=" + package_path + " device=" + target_device + " slot=" + target_slot, job_id);

    // Create the journal entry.
    JournalEntry entry;
    entry.transaction_id = job_id;
    entry.operation = "system_install";
    entry.state = JournalState::None;
    entry.target_device = target_device;
    entry.target_slot = target_slot;
    entry.safe_to_resume = true;

    auto journal_result = impl_->journal->begin(entry.transaction_id, entry.operation);
    if (journal_result.is_ok()) {
        // Persist target_device and target_slot via update()
        impl_->journal->update(entry);
    }
    if (!journal_result.is_ok()) {
        impl_->logger->log(LogLevel::Error, "JobManager", "journal_begin_failed: " + journal_result.error().code, job_id);
        return Result<std::string>::err(journal_result.take_error());
    }

    // Create the install job.
    auto job = std::make_unique<InstallJob>(
        job_id, package_path, target_device, target_slot,
        impl_->dev_mgr, impl_->pkg_mgr, impl_->image_writer,
        impl_->part_mgr, impl_->fs_mgr, impl_->boot_ctrl,
        impl_->sec_mgr, impl_->logger);

    // Set journal in job context via BaseJob::set_journal().
    auto* base_job = static_cast<BaseJob*>(job.get());
    // We need to access ctx_.journal. Since ctx_ is protected, we need
    // either a setter or friend access. I'll add a setter to BaseJob.
    base_job->set_journal(impl_->journal.get());

    // Set the progress callback and event forwarding.
    job->set_progress_callback([this, job_id](const ProgressInfo& info) {
        // Update record.
        {
            std::lock_guard<std::mutex> lock(impl_->records_mutex);
            auto& rec = impl_->job_records[job_id];
            rec.progress = info.percent;
            rec.current_step = info.step_description;
        }

        // Broadcast progress event.
        json data;
        data["job_id"] = job_id;
        data["percent"] = info.percent;
        data["step"] = info.step_description;
        data["current_file"] = info.current_file;
        data["bytes_processed"] = info.bytes_processed;
        data["bytes_total"] = info.bytes_total;
        data["speed_bytes_per_sec"] = info.speed_bytes_per_sec;
        broadcast_event("JobProgressChanged", data);
    });

    // Register the job record.
    {
        std::lock_guard<std::mutex> lock(impl_->records_mutex);
        impl_->job_records[job_id] = {JobState::Idle, 0, ""};
    }

    // Submit the job to the worker thread.
    {
        std::lock_guard<std::mutex> lock(impl_->job_mutex);
        if (impl_->current_job) {
            return Result<std::string>::err(InstallerError::make(
                ErrorCode::INTERNAL_ERROR,
                "Job In Progress",
                "Another job is already running (race condition)",
                "",
                true));
        }
        impl_->current_job = std::move(job);
        impl_->job_in_progress.store(true, std::memory_order_release);
    }
    impl_->job_cv.notify_one();

    // Broadcast job created event.
    json data;
    data["job_id"] = job_id;
    data["type"] = "system_install";
    data["target_device"] = target_device;
    data["target_slot"] = target_slot;
    broadcast_event("JobCreated", data);

    return Result<std::string>::ok(job_id);
}

Result<std::string> JobManager::start_backup(const std::string& /*profile_name*/) {
    // Backup job is not implemented yet in the step chain.
    // Return an error indicating this.
    return Result<std::string>::err(InstallerError::make(
        ErrorCode::INTERNAL_ERROR,
        "Not Implemented",
        "Backup job is not yet implemented",
        "profile=" + /*profile_name*/ ""));
}

Result<std::string> JobManager::start_restore(const std::string& /*backup_path*/) {
    // Restore job is not implemented yet in the step chain.
    return Result<std::string>::err(InstallerError::make(
        ErrorCode::INTERNAL_ERROR,
        "Not Implemented",
        "Restore job is not yet implemented",
        "backup_path=" + /*backup_path*/ ""));
}

Result<void> JobManager::cancel_job(const std::string& job_id) {
    impl_->logger->log(LogLevel::Warn, "JobManager", "cancel_job: Cancellation requested", job_id);

    std::unique_ptr<IJob> job_ptr;
    {
        std::lock_guard<std::mutex> lock(impl_->job_mutex);
        if (impl_->current_job && impl_->current_job->job_id() == job_id) {
            impl_->current_job->request_cancel();
        }
    }

    // Update the record.
    {
        std::lock_guard<std::mutex> lock(impl_->records_mutex);
        auto it = impl_->job_records.find(job_id);
        if (it != impl_->job_records.end()) {
            it->second.state = JobState::Cancelling;
        }
    }

    json data;
    data["job_id"] = job_id;
    broadcast_event("JobStateChanged", data);

    return Result<void>::ok();
}

Result<json> JobManager::get_job_status(const std::string& job_id) {
    std::lock_guard<std::mutex> lock(impl_->records_mutex);

    auto it = impl_->job_records.find(job_id);
    if (it == impl_->job_records.end()) {
        return Result<json>::err(InstallerError::make(
            ErrorCode::INTERNAL_INVALID_STATE,
            "Job Not Found",
            "No job found with the given ID",
            "job_id=" + job_id));
    }

    json j;
    j["job_id"] = job_id;
    j["state"] = job_state_name(it->second.state);
    j["progress"] = it->second.progress;
    j["current_step"] = it->second.current_step;

    return Result<json>::ok(j);
}

Result<json> JobManager::get_job_progress(const std::string& job_id) {
    std::lock_guard<std::mutex> lock(impl_->records_mutex);

    auto it = impl_->job_records.find(job_id);
    if (it == impl_->job_records.end()) {
        return Result<json>::err(InstallerError::make(
            ErrorCode::INTERNAL_INVALID_STATE,
            "Job Not Found",
            "No job found with the given ID",
            "job_id=" + job_id));
    }

    json j;
    j["job_id"] = job_id;
    j["percent"] = it->second.progress;
    j["current_step"] = it->second.current_step;

    return Result<json>::ok(j);
}

Result<json> JobManager::list_jobs() {
    std::lock_guard<std::mutex> lock(impl_->records_mutex);

    json jobs = json::array();
    for (const auto& [id, rec] : impl_->job_records) {
        json j;
        j["job_id"] = id;
        j["state"] = job_state_name(rec.state);
        j["progress"] = rec.progress;
        j["current_step"] = rec.current_step;
        jobs.push_back(j);
    }

    json result;
    result["jobs"] = jobs;
    return Result<json>::ok(result);
}

// ============================================================================
//  Additional operations
// ============================================================================

bool JobManager::is_busy() {
    return impl_->job_in_progress.load(std::memory_order_acquire);
}

Result<void> JobManager::resume_job(const std::string& job_id,
                                     ProgressCallback progress) {
    if (is_busy()) {
        return Result<void>::err(InstallerError::make(
            ErrorCode::INTERNAL_ERROR,
            "Job In Progress",
            "Another job is already running",
            "",
            true));
    }

    // Look up the incomplete transaction.
    auto tx_result = impl_->journal->get(job_id);
    if (!tx_result.is_ok()) {
        return Result<void>::err(tx_result.take_error());
    }

    const auto& entry = tx_result.value();
    if (entry.state == JournalState::Complete ||
        entry.state == JournalState::Aborted) {
        return Result<void>::err(InstallerError::make(
            ErrorCode::INTERNAL_INVALID_STATE,
            "Already Complete",
            "The transaction is already complete or aborted",
            "job_id=" + job_id));
    }

    if (!entry.safe_to_resume) {
        return Result<void>::err(InstallerError::make(
            ErrorCode::INTERNAL_INVALID_STATE,
            "Not Resumable",
            "This transaction is not safe to resume",
            "job_id=" + job_id));
    }

    // Re-create the job from the journal entry.
    // For now, we support resuming install jobs.
    std::unique_ptr<IJob> job;
    if (entry.operation == "system_install") {
        job = std::make_unique<InstallJob>(
            job_id, /*package_path=*/"",  // would need to be recovered
            entry.target_device, entry.target_slot,
            impl_->dev_mgr, impl_->pkg_mgr, impl_->image_writer,
            impl_->part_mgr, impl_->fs_mgr, impl_->boot_ctrl,
            impl_->sec_mgr, impl_->logger);
    } else {
        return Result<void>::err(InstallerError::make(
            ErrorCode::INTERNAL_ERROR,
            "Not Implemented",
            "Resume is only implemented for install jobs",
            "operation=" + entry.operation));
    }

    // Set journal pointer.
    auto* base_job = static_cast<BaseJob*>(job.get());
    base_job->set_journal(impl_->journal.get());

    if (progress) {
        job->set_progress_callback(std::move(progress));
    }

    // Submit to worker.
    {
        std::lock_guard<std::mutex> lock(impl_->job_mutex);
        impl_->current_job = std::move(job);
        impl_->job_in_progress.store(true, std::memory_order_release);
    }
    impl_->job_cv.notify_one();

    return Result<void>::ok();
}

std::vector<std::string> JobManager::get_unfinished_jobs() {
    auto entries_result = impl_->journal->find_incomplete();
    if (!entries_result.is_ok()) {
        return {};
    }
    auto& entries = entries_result.value();
    std::vector<std::string> ids;
    ids.reserve(entries.size());
    for (const auto& e : entries) {
        ids.push_back(e.transaction_id);
    }
    return ids;
}

void JobManager::shutdown() {
    impl_->logger->log(LogLevel::Info, "JobManager", "JobManager: shutdown requested");
    impl_->shutdown_requested.store(true, std::memory_order_release);

    // Cancel any running job.
    {
        std::lock_guard<std::mutex> lock(impl_->job_mutex);
        if (impl_->current_job) {
            impl_->current_job->request_cancel();
        }
    }

    impl_->job_cv.notify_all();

    if (impl_->worker.joinable()) {
        impl_->worker.join();
    }

    impl_->logger->log(LogLevel::Info, "JobManager", "JobManager: shutdown complete");
}

// ============================================================================
//  Private: worker thread
// ============================================================================

void JobManager::worker_loop() {
    impl_->logger->log(LogLevel::Info, "JobManager", "JobManager: worker thread started");

    while (!impl_->shutdown_requested.load(std::memory_order_acquire)) {
        std::unique_ptr<IJob> job;

        // Wait for a job to be submitted.
        {
            std::unique_lock<std::mutex> lock(impl_->job_mutex);
            impl_->job_cv.wait(lock, [this] {
                return impl_->current_job != nullptr ||
                       impl_->shutdown_requested.load(std::memory_order_acquire);
            });

            if (impl_->shutdown_requested.load(std::memory_order_acquire)) {
                break;
            }

            job = std::move(impl_->current_job);
        }

        if (!job) continue;

        std::string job_id = job->job_id();
        impl_->logger->log(LogLevel::Info, "JobManager", "worker_start: Worker picked up job", job_id);

        // Update record state.
        {
            std::lock_guard<std::mutex> lock(impl_->records_mutex);
            auto& rec = impl_->job_records[job_id];
            rec.state = JobState::Preparing;
        }

        // Broadcast state change.
        json state_data;
        state_data["job_id"] = job_id;
        state_data["state"] = job_state_name(JobState::Preparing);
        broadcast_event("JobStateChanged", state_data);

        // Create a cancellation token for this job run.
        CancellationToken cancel_token;

        // Execute the job.
        auto result = job->start(cancel_token);

        // Handle result.
        if (result.is_ok()) {
            impl_->logger->log(LogLevel::Info, "JobManager", "job_completed: Job completed successfully", job_id);

            json complete_data;
            complete_data["job_id"] = job_id;
            complete_data["state"] = job_state_name(JobState::Completed);
            complete_data["success"] = true;
            broadcast_event("JobCompleted", complete_data);

            {
                std::lock_guard<std::mutex> lock(impl_->records_mutex);
                auto& rec = impl_->job_records[job_id];
                rec.state = JobState::Completed;
                rec.progress = 100;
            }
        } else {
            const auto& err = result.error();
            bool is_cancelled = (err.code == ErrorCode::INTERNAL_CANCELLED);

            impl_->logger->log(is_cancelled ? LogLevel::Warn : LogLevel::Error, "JobManager", "job_failed: code=" + err.code + " msg=" + err.user_message, job_id);

            json fail_data;
            fail_data["job_id"] = job_id;
            fail_data["state"] = is_cancelled ? job_state_name(JobState::Failed)
                                              : job_state_name(JobState::Failed);
            fail_data["success"] = false;
            fail_data["error_code"] = err.code;
            fail_data["error_message"] = err.user_message;
            broadcast_event("JobCompleted", fail_data);

            {
                std::lock_guard<std::mutex> lock(impl_->records_mutex);
                auto& rec = impl_->job_records[job_id];
                rec.state = is_cancelled ? JobState::Failed : JobState::Failed;
            }
        }

        // Mark job as no longer in progress.
        impl_->job_in_progress.store(false, std::memory_order_release);
    }

    impl_->logger->log(LogLevel::Info, "JobManager", "JobManager: worker thread exiting");
}

// ============================================================================
//  Private: event broadcasting
// ============================================================================

void JobManager::broadcast_event(const std::string& event_name,
                                  const json& data) {
    std::lock_guard<std::mutex> lock(impl_->event_mutex);
    if (impl_->event_callback) {
        try {
            impl_->event_callback(event_name, data);
        } catch (const std::exception& e) {
            impl_->logger->error("JobManager: event callback threw exception: " +
                                 std::string(e.what()));
        }
    }
}

} // namespace installer
