/**
 * @file service_setup.cpp
 * @brief Composition root: creates and wires all services.
 *
 * This is the central wiring point for the embedded Linux installer.
 * All concrete implementations are constructed here with their
 * dependencies injected, IPC method handlers are registered, and
 * event callbacks are wired between components.
 */

#include "service_setup.h"

// ---- Concrete implementations ----
#include "src/log/structured_logger.h"
#include "src/core/device_manager.h"
#include "src/core/security_manager.h"
#include "src/core/package_manager.h"
#include "src/core/image_writer.h"
#include "src/core/partition_manager.h"
#include "src/core/filesystem_manager.h"
#include "src/core/boot_control.h"
#include "src/core/job_manager.h"
#include "src/core/service_context.h"
#include "src/platform/process_runner.h"
#include "src/journal/transaction_journal.h"
#include "src/ipc/ipc_server.h"
#include "src/ipc/ipc_protocol.h"
#include "src/config/config_loader.h"

#include <nlohmann/json.hpp>
#include <iostream>

namespace installer {

using json = nlohmann::json;

namespace {

json ipc_error(const std::string& message, const std::string& code = "") {
    json err;
    err["success"] = false;
    err["error"] = message;
    if (!code.empty()) {
        err["code"] = code;
    }
    return err;
}

} // anonymous namespace

// ============================================================================
//  ServiceSetup::create
// ============================================================================

Result<ServiceSetup::Services> ServiceSetup::create(
    const std::string& config_path) {

    Services svc;

    // ---- Step 1: Load configuration ----
    auto config_result = ConfigLoader::load(config_path);
    if (config_result.is_err()) {
        return Result<Services>::err(config_result.take_error());
    }
    auto config = config_result.take_value();

    // ---- Step 2: Create logger (first, before all else) ----
    svc.logger = std::make_shared<StructuredLogger>();

    std::string log_dir = config.log_dir.empty()
                              ? "/var/log/installer"
                              : config.log_dir;
    auto log_file_result = svc.logger->set_log_file(log_dir + "/installer.log");
    if (log_file_result.is_err()) {
        std::cerr << "Warning: could not set log file: "
                  << log_file_result.error().user_message << std::endl;
    }

    svc.logger->log(LogLevel::Info, "ServiceSetup",
                    "Loading configuration from " + config_path);

    // ---- Step 3: Create low-level services ----
    svc.proc_runner = std::make_shared<ProcessRunner>();
    svc.sec_mgr = std::make_shared<SecurityManager>();
    svc.device_mgr = std::make_shared<DeviceManager>();

    // ---- Step 4: Create mid-level services ----
    svc.package_mgr = std::make_shared<PackageManager>(
        svc.proc_runner.get(), svc.sec_mgr.get());

    svc.image_writer = std::make_shared<ImageWriter>();

    svc.part_mgr = std::make_shared<PartitionManager>(
        svc.proc_runner.get());

    svc.fs_mgr = std::make_shared<FilesystemManager>(
        svc.proc_runner.get());

    svc.boot_ctrl = std::make_shared<BootControl>(
        svc.proc_runner.get(), "/etc/fw_env.config");

    // ---- Step 5: Create transaction journal ----
    std::string journal_dir = config.journal_dir.empty()
                                  ? "/var/lib/installer/journal"
                                  : config.journal_dir;
    svc.journal = std::make_shared<TransactionJournal>(
        journal_dir, svc.logger);

    // ---- Step 6: Build ServiceContext ----
    svc.service_ctx = std::make_shared<ServiceContext>();
    svc.service_ctx->logger       = svc.logger;
    svc.service_ctx->device_mgr   = svc.device_mgr;
    svc.service_ctx->sec_mgr      = svc.sec_mgr;
    svc.service_ctx->proc_runner  = svc.proc_runner;
    svc.service_ctx->package_mgr  = svc.package_mgr;
    svc.service_ctx->image_writer = svc.image_writer;
    svc.service_ctx->part_mgr     = svc.part_mgr;
    svc.service_ctx->fs_mgr       = svc.fs_mgr;
    svc.service_ctx->boot_ctrl    = svc.boot_ctrl;
    svc.service_ctx->journal      = svc.journal;

    // ---- Step 7: Create JobManager ----
    svc.job_mgr = std::make_shared<JobManager>(
        svc.journal,
        svc.logger,
        svc.device_mgr,
        svc.package_mgr,
        svc.image_writer,
        svc.part_mgr,
        svc.fs_mgr,
        svc.boot_ctrl,
        svc.sec_mgr);

    // ---- Step 8: Create IPC server ----
    svc.ipc_server = std::make_shared<ipc::UnixSocketJsonRpcServer>(
        svc.logger);

    // ---- Step 9: Register IPC method handlers ----

    // ping
    svc.ipc_server->register_method(
        ipc::methods::PING,
        [](const json&) -> json {
            return json{{"message", "pong"}};
        });

    // device.list
    svc.ipc_server->register_method(
        ipc::methods::LIST_DEVICES,
        [device_mgr = svc.device_mgr](const json&) -> json {
            auto devices = device_mgr->scan();
            json device_array = json::array();
            for (const auto& dev : devices) {
                json d;
                d["path"] = dev.path;
                d["model"] = dev.model;
                d["serial"] = dev.serial;
                d["size_bytes"] = dev.size_bytes;
                d["logical_sector_size"] = dev.logical_sector_size;
                d["removable"] = dev.removable;
                d["read_only"] = dev.read_only;
                d["is_system_disk"] = dev.is_system_disk;
                device_array.push_back(d);
            }
            return json{{"devices", device_array}};
        });

    // device.info
    svc.ipc_server->register_method(
        ipc::methods::GET_DEVICE_INFO,
        [device_mgr = svc.device_mgr](const json& params) -> json {
            if (!params.contains("path") || !params["path"].is_string()) {
                return ipc_error("Missing required parameter: path");
            }
            std::string path = params["path"].get<std::string>();
            auto info_result = device_mgr->get_device_info(path);
            if (info_result.is_err()) {
                return ipc_error(info_result.error().user_message,
                                 info_result.error().code);
            }
            const auto& dev = info_result.value();
            return json{
                {"path", dev.path},
                {"model", dev.model},
                {"serial", dev.serial},
                {"size_bytes", dev.size_bytes},
                {"logical_sector_size", dev.logical_sector_size},
                {"physical_sector_size", dev.physical_sector_size},
                {"removable", dev.removable},
                {"read_only", dev.read_only},
                {"is_system_disk", dev.is_system_disk}
            };
        });

    // package.verify
    svc.ipc_server->register_method(
        ipc::methods::VERIFY_PACKAGE,
        [package_mgr = svc.package_mgr](const json& params) -> json {
            if (!params.contains("path") || !params["path"].is_string()) {
                return ipc_error("Missing required parameter: path");
            }
            std::string path = params["path"].get<std::string>();
            auto open_result = package_mgr->open(path);
            if (open_result.is_err()) {
                return ipc_error(open_result.error().user_message,
                                 open_result.error().code);
            }
            auto manifest_result = package_mgr->load_manifest();
            if (manifest_result.is_err()) {
                package_mgr->close();
                return ipc_error(manifest_result.error().user_message,
                                 manifest_result.error().code);
            }
            const auto& manifest = manifest_result.value();
            package_mgr->close();
            return json{
                {"valid", true},
                {"package_id", manifest.package_id},
                {"product", manifest.product},
                {"version", manifest.version},
                {"architecture", manifest.architecture},
                {"payload_count", manifest.payloads.size()}
            };
        });

    // install.start
    svc.ipc_server->register_method(
        ipc::methods::START_INSTALL,
        [job_mgr = svc.job_mgr](const json& params) -> json {
            if (!params.contains("package") || !params["package"].is_string()) {
                return ipc_error("Missing required parameter: package");
            }
            if (!params.contains("target") || !params["target"].is_string()) {
                return ipc_error("Missing required parameter: target");
            }
            std::string package = params["package"].get<std::string>();
            std::string target = params["target"].get<std::string>();
            std::string slot = params.value("slot", "A");
            auto result = job_mgr->start_install(package, target, slot);
            if (result.is_err()) {
                return ipc_error(result.error().user_message,
                                 result.error().code);
            }
            return json{{"job_id", result.value()}, {"success", true}};
        });

    // backup.start
    svc.ipc_server->register_method(
        ipc::methods::START_BACKUP,
        [job_mgr = svc.job_mgr](const json& params) -> json {
            if (!params.contains("profile") || !params["profile"].is_string()) {
                return ipc_error("Missing required parameter: profile");
            }
            auto result = job_mgr->start_backup(
                params["profile"].get<std::string>());
            if (result.is_err()) {
                return ipc_error(result.error().user_message,
                                 result.error().code);
            }
            return json{{"job_id", result.value()}, {"success", true}};
        });

    // restore.start
    svc.ipc_server->register_method(
        ipc::methods::START_RESTORE,
        [job_mgr = svc.job_mgr](const json& params) -> json {
            if (!params.contains("backup") || !params["backup"].is_string()) {
                return ipc_error("Missing required parameter: backup");
            }
            auto result = job_mgr->start_restore(
                params["backup"].get<std::string>());
            if (result.is_err()) {
                return ipc_error(result.error().user_message,
                                 result.error().code);
            }
            return json{{"job_id", result.value()}, {"success", true}};
        });

    // job.cancel
    svc.ipc_server->register_method(
        ipc::methods::CANCEL_JOB,
        [job_mgr = svc.job_mgr](const json& params) -> json {
            if (!params.contains("id") || !params["id"].is_string()) {
                return ipc_error("Missing required parameter: id");
            }
            auto result = job_mgr->cancel_job(
                params["id"].get<std::string>());
            if (result.is_err()) {
                return ipc_error(result.error().user_message,
                                 result.error().code);
            }
            return json{{"success", true}};
        });

    // job.status
    svc.ipc_server->register_method(
        ipc::methods::GET_JOB_STATUS,
        [job_mgr = svc.job_mgr](const json& params) -> json {
            if (!params.contains("id") || !params["id"].is_string()) {
                return ipc_error("Missing required parameter: id");
            }
            auto result = job_mgr->get_job_status(
                params["id"].get<std::string>());
            if (result.is_err()) {
                return ipc_error(result.error().user_message,
                                 result.error().code);
            }
            return result.value();
        });

    // job.progress
    svc.ipc_server->register_method(
        ipc::methods::GET_JOB_PROGRESS,
        [job_mgr = svc.job_mgr](const json& params) -> json {
            if (!params.contains("id") || !params["id"].is_string()) {
                return ipc_error("Missing required parameter: id");
            }
            auto result = job_mgr->get_job_progress(
                params["id"].get<std::string>());
            if (result.is_err()) {
                return ipc_error(result.error().user_message,
                                 result.error().code);
            }
            return result.value();
        });

    // job.list
    svc.ipc_server->register_method(
        ipc::methods::LIST_JOBS,
        [job_mgr = svc.job_mgr](const json&) -> json {
            auto result = job_mgr->list_jobs();
            if (result.is_err()) {
                return ipc_error(result.error().user_message,
                                 result.error().code);
            }
            return result.value();
        });

    // system.reboot
    svc.ipc_server->register_method(
        ipc::methods::REBOOT,
        [logger = svc.logger](const json&) -> json {
            logger->log(LogLevel::Warn, "ServiceSetup",
                        "Reboot requested via IPC");
            return json{{"success", true}, {"message", "Reboot initiated"}};
        });

    // system.shutdown
    svc.ipc_server->register_method(
        ipc::methods::SHUTDOWN,
        [logger = svc.logger](const json&) -> json {
            logger->log(LogLevel::Warn, "ServiceSetup",
                        "Shutdown requested via IPC");
            return json{{"success", true}, {"message", "Shutdown initiated"}};
        });

    // boot.status
    svc.ipc_server->register_method(
        ipc::methods::BOOT_STATUS,
        [boot_ctrl = svc.boot_ctrl](const json&) -> json {
            auto env_result = boot_ctrl->read_boot_env();
            if (env_result.is_err()) {
                return ipc_error(env_result.error().user_message,
                                 env_result.error().code);
            }
            const auto& env = env_result.value();
            return json{
                {"active_slot", env.active_slot},
                {"next_slot", env.next_slot},
                {"upgrade_pending", env.upgrade_pending},
                {"boot_attempts_left", env.boot_attempts_left},
                {"slot_a_good", env.slot_a_good},
                {"slot_b_good", env.slot_b_good}
            };
        });

    // boot.set_slot
    svc.ipc_server->register_method(
        ipc::methods::BOOT_SET_SLOT,
        [boot_ctrl = svc.boot_ctrl](const json& params) -> json {
            if (!params.contains("slot") || !params["slot"].is_string()) {
                return ipc_error("Missing required parameter: slot");
            }
            auto result = boot_ctrl->set_active_slot(
                params["slot"].get<std::string>());
            if (result.is_err()) {
                return ipc_error(result.error().user_message,
                                 result.error().code);
            }
            return json{{"success", true}};
        });

    // ---- Step 10: Wire JobManager events to IPC broadcast ----
    svc.job_mgr->set_event_callback(
        [ipc_server = svc.ipc_server,
         logger = svc.logger](const std::string& event_name,
                               const json& data) {
            std::string ipc_event;
            if (event_name == "JobStateChanged") {
                ipc_event = ipc::events::JOB_STATE_CHANGED;
            } else if (event_name == "JobProgressChanged") {
                ipc_event = ipc::events::JOB_PROGRESS_CHANGED;
            } else if (event_name == "JobCreated" ||
                       event_name == "JobCompleted") {
                json enriched = data;
                enriched["event_type"] = event_name;
                ipc_server->push_event(ipc::events::JOB_STATE_CHANGED,
                                       enriched);
                return;
            } else {
                logger->log(LogLevel::Debug, "ServiceSetup",
                            "Unknown internal event: " + event_name);
                return;
            }
            ipc_server->push_event(ipc_event, data);
        });

    // ---- Step 11: Start device hotplug monitor ----
    auto* device_mgr_raw =
        dynamic_cast<DeviceManager*>(svc.device_mgr.get());
    if (device_mgr_raw) {
        device_mgr_raw->start_hotplug_monitor(
            [ipc_server = svc.ipc_server](const BlockDeviceInfo& info,
                                           bool added) {
                json data;
                data["path"] = info.path;
                data["model"] = info.model;
                data["serial"] = info.serial;
                data["size_bytes"] = info.size_bytes;
                data["removable"] = info.removable;
                ipc_server->push_event(
                    added ? ipc::events::DEVICE_ADDED
                          : ipc::events::DEVICE_REMOVED,
                    data);
            });
    }

    svc.logger->log(LogLevel::Info, "ServiceSetup",
                    "All services created and wired successfully");
    return Result<Services>::ok(std::move(svc));
}

// ============================================================================
//  ServiceSetup::shutdown
// ============================================================================

void ServiceSetup::shutdown(Services& svc) {
    if (svc.logger) {
        svc.logger->log(LogLevel::Info, "ServiceSetup", "Shutdown initiated");
    }

    // 1. Stop IPC server
    if (svc.ipc_server) {
        svc.ipc_server->stop();
    }

    // 2. Shutdown JobManager (cancel jobs, stop worker)
    if (svc.job_mgr) {
        svc.job_mgr->shutdown();
    }

    // 3. Close package
    if (svc.package_mgr) {
        svc.package_mgr->close();
    }

    // 4. Flush logs
    if (svc.logger) {
        svc.logger->log(LogLevel::Info, "ServiceSetup", "Shutdown complete");
        svc.logger->flush();
    }

    // Release all shared_ptr references in reverse dependency order
    svc.ipc_server.reset();
    svc.job_mgr.reset();
    svc.journal.reset();
    svc.boot_ctrl.reset();
    svc.fs_mgr.reset();
    svc.part_mgr.reset();
    svc.image_writer.reset();
    svc.package_mgr.reset();
    svc.device_mgr.reset();
    svc.sec_mgr.reset();
    svc.proc_runner.reset();
    svc.service_ctx.reset();
    svc.logger.reset();
}

} // namespace installer
