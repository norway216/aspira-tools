/**
 * @file IJobManager.h
 * @brief Job orchestration interface for the embedded Linux installer.
 *
 * The JobManager is the central orchestrator that accepts job requests
 * (install, backup, restore, verify) and drives the state machine
 * through each step.  Progress and state changes are reported via
 * the registered EventCallback.
 */

#ifndef INSTALLER_IJOBMANAGER_H
#define INSTALLER_IJOBMANAGER_H

#include "installer/core/types.h"
#include "installer/core/result.h"
#include <functional>
#include <memory>
#include <string>
#include <vector>

#ifndef NLOHMANN_JSON_FWD_HPP
namespace nlohmann {
    class json;
}
#endif

namespace installer {

class IJobManager {
public:
    virtual ~IJobManager() = default;

    using EventCallback = std::function<void(const std::string& event_name,
                                             const nlohmann::json& data)>;

    virtual void set_event_callback(EventCallback cb) = 0;

    virtual Result<std::string> start_install(const std::string& package_path,
                                              const std::string& target_device,
                                              const std::string& target_slot) = 0;

    virtual Result<std::string> start_backup(const std::string& profile_name) = 0;

    virtual Result<std::string> start_restore(const std::string& backup_path) = 0;

    virtual Result<void> cancel_job(const std::string& job_id) = 0;

    virtual Result<nlohmann::json> get_job_status(const std::string& job_id) = 0;

    virtual Result<nlohmann::json> get_job_progress(const std::string& job_id) = 0;

    virtual Result<nlohmann::json> list_jobs() = 0;
};

} // namespace installer

#endif // INSTALLER_IJOBMANAGER_H
