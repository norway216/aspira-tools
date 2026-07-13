/**
 * @file service_context.h
 * @brief Dependency injection container holding all service shared_ptrs.
 *
 * ServiceContext is passed to the JobManager constructor to provide
 * all the service dependencies needed to create and execute jobs.
 *
 * The helper make_job_context() populates a JobContext from the
 * ServiceContext for jobs that need raw pointers to services.
 */

#ifndef INSTALLER_SERVICE_CONTEXT_H
#define INSTALLER_SERVICE_CONTEXT_H

#include "installer/core/types.h"

#include <memory>
#include <string>

namespace installer {

// Forward declarations for all service interfaces.
class IDeviceManager;
class IPackageManager;
class IImageWriter;
class IPartitionManager;
class IFilesystemManager;
class IBootControl;
class ISecurityManager;
class ITransactionJournal;
class ILogger;
class IProcessRunner;

/**
 * ServiceContext — central dependency injection container.
 *
 * All services are held as shared_ptr for shared ownership across
 * the application. The JobManager receives this struct and passes
 * individual services to job and step constructors.
 */
struct ServiceContext {
    std::shared_ptr<IDeviceManager> device_mgr;
    std::shared_ptr<IPackageManager> package_mgr;
    std::shared_ptr<IImageWriter> image_writer;
    std::shared_ptr<IPartitionManager> part_mgr;
    std::shared_ptr<IFilesystemManager> fs_mgr;
    std::shared_ptr<IBootControl> boot_ctrl;
    std::shared_ptr<ISecurityManager> sec_mgr;
    std::shared_ptr<ITransactionJournal> journal;
    std::shared_ptr<ILogger> logger;
    std::shared_ptr<IProcessRunner> proc_runner;

    /**
     * Verify that all required services are available.
     * Returns true if all non-optional services are set.
     */
    bool is_valid() const {
        return device_mgr != nullptr &&
               package_mgr != nullptr &&
               image_writer != nullptr &&
               part_mgr != nullptr &&
               fs_mgr != nullptr &&
               boot_ctrl != nullptr &&
               sec_mgr != nullptr &&
               journal != nullptr &&
               logger != nullptr;
    }
};

/**
 * Populate a JobContext from a ServiceContext.
 *
 * The JobContext uses raw (non-owning) pointers to services,
 * while ServiceContext holds shared_ptr for lifetime management.
 */
inline JobContext make_job_context(const ServiceContext& services,
                                    const std::string& job_id) {
    JobContext ctx;
    ctx.job_id = job_id;
    ctx.device_mgr   = services.device_mgr.get();
    ctx.package_mgr  = services.package_mgr.get();
    ctx.image_writer = services.image_writer.get();
    ctx.part_mgr     = services.part_mgr.get();
    ctx.fs_mgr       = services.fs_mgr.get();
    ctx.boot_ctrl    = services.boot_ctrl.get();
    ctx.sec_mgr      = services.sec_mgr.get();
    ctx.journal      = services.journal.get();
    ctx.logger       = services.logger.get();
    ctx.proc_runner  = services.proc_runner.get();
    return ctx;
}

} // namespace installer

#endif // INSTALLER_SERVICE_CONTEXT_H
