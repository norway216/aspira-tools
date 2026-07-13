/**
 * @file job_manager.h
 * @brief Central job orchestrator implementing IJobManager.
 *
 * Manages job lifecycle with a single worker thread. Only one
 * high-risk disk operation runs at a time. Progress and state
 * changes are broadcast via the EventCallback.
 *
 * On startup, checks the transaction journal for unfinished
 * jobs and offers resume capability.
 */

#ifndef INSTALLER_JOB_MANAGER_H
#define INSTALLER_JOB_MANAGER_H

#include "installer/IJobManager.h"
#include "installer/IJob.h"
#include "installer/ITransactionJournal.h"
#include "installer/log/ilogger.h"

#include <atomic>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

#include <nlohmann/json.hpp>

namespace installer {

// Forward declarations for service dependencies.
class IDeviceManager;
class IPackageManager;
class IImageWriter;
class IPartitionManager;
class IFilesystemManager;
class IBootControl;
class ISecurityManager;

class JobManager : public IJobManager {
public:
    /**
     * @param journal     Transaction journal for crash recovery.
     * @param logger      Logger.
     * @param dev_mgr     Device manager.
     * @param pkg_mgr     Package manager.
     * @param image_writer Image writer.
     * @param part_mgr    Partition manager.
     * @param fs_mgr      Filesystem manager.
     * @param boot_ctrl   Boot control.
     * @param sec_mgr     Security manager.
     */
    JobManager(std::shared_ptr<ITransactionJournal> journal,
               std::shared_ptr<ILogger> logger,
               std::shared_ptr<IDeviceManager> dev_mgr,
               std::shared_ptr<IPackageManager> pkg_mgr,
               std::shared_ptr<IImageWriter> image_writer,
               std::shared_ptr<IPartitionManager> part_mgr,
               std::shared_ptr<IFilesystemManager> fs_mgr,
               std::shared_ptr<IBootControl> boot_ctrl,
               std::shared_ptr<ISecurityManager> sec_mgr);

    ~JobManager() override;

    // ---- IJobManager interface ----

    void set_event_callback(EventCallback cb) override;

    Result<std::string> start_install(const std::string& package_path,
                                      const std::string& target_device,
                                      const std::string& target_slot) override;

    Result<std::string> start_backup(const std::string& profile_name) override;

    Result<std::string> start_restore(const std::string& backup_path) override;

    Result<void> cancel_job(const std::string& job_id) override;

    Result<nlohmann::json> get_job_status(const std::string& job_id) override;

    Result<nlohmann::json> get_job_progress(const std::string& job_id) override;

    Result<nlohmann::json> list_jobs() override;

    // ---- Additional operations ----

    // Check if a job is currently executing on the worker thread.
    bool is_busy();

    // Resume an incomplete job from the journal.
    Result<void> resume_job(const std::string& job_id,
                            ProgressCallback progress);

    // Get list of unfinished job IDs (from journal).
    std::vector<std::string> get_unfinished_jobs();

    // Graceful shutdown: cancel current job and stop worker.
    void shutdown();

private:
    // Worker thread function.
    void worker_loop();

    // Broadcast an event to the registered callback.
    void broadcast_event(const std::string& event_name,
                         const nlohmann::json& data);

    // Generate a UUID for job IDs.
    static std::string generate_uuid();

    struct Impl {
        // Services (shared ownership).
        std::shared_ptr<ITransactionJournal> journal;
        std::shared_ptr<ILogger> logger;
        std::shared_ptr<IDeviceManager> dev_mgr;
        std::shared_ptr<IPackageManager> pkg_mgr;
        std::shared_ptr<IImageWriter> image_writer;
        std::shared_ptr<IPartitionManager> part_mgr;
        std::shared_ptr<IFilesystemManager> fs_mgr;
        std::shared_ptr<IBootControl> boot_ctrl;
        std::shared_ptr<ISecurityManager> sec_mgr;

        // Worker thread.
        std::thread worker;
        std::atomic<bool> running{false};
        std::atomic<bool> shutdown_requested{false};

        // Job state.
        std::mutex job_mutex;
        std::condition_variable job_cv;
        std::unique_ptr<IJob> current_job;
        std::atomic<bool> job_in_progress{false};

        // Job history: maps job_id -> {state, progress}.
        struct JobRecord {
            JobState state = JobState::Idle;
            int progress = 0;
            std::string current_step;
        };
        std::unordered_map<std::string, JobRecord> job_records;
        std::mutex records_mutex;

        // Event callback (thread-safe via its own mutex).
        std::mutex event_mutex;
        EventCallback event_callback;
    };
    std::unique_ptr<Impl> impl_;
};

} // namespace installer

#endif // INSTALLER_JOB_MANAGER_H
