/**
 * @file service_setup.h
 * @brief Composition root for the embedded Linux installer.
 *
 * ServiceSetup is the central wiring point: it creates all concrete
 * service implementations, injects their dependencies, registers
 * IPC method handlers, and wires event callbacks.
 */

#ifndef INSTALLER_SERVICE_SETUP_H
#define INSTALLER_SERVICE_SETUP_H

#include "installer/ServiceContext.h"
#include "installer/IIPCServer.h"
#include "installer/IJobManager.h"
#include "installer/log/ilogger.h"
#include "installer/IConfigLoader.h"
#include "installer/IDeviceManager.h"
#include "installer/security/isecurity_manager.h"
#include "installer/platform/iprocess_runner.h"
#include "installer/IPackageManager.h"
#include "installer/image/iimage_writer.h"
#include "installer/IPartitionManager.h"
#include "installer/IFilesystemManager.h"
#include "installer/IBootControl.h"
#include "installer/ITransactionJournal.h"
#include "installer/core/result.h"
#include <memory>
#include <string>

namespace installer {

class ServiceSetup {
public:
    /**
     * Bundle of all constructed services.
     * Callers take ownership of individual shared_ptr members
     * as needed.  Call shutdown() before the bundle goes out of
     * scope to ensure orderly teardown.
     */
    struct Services {
        std::shared_ptr<ILogger> logger;
        std::shared_ptr<IConfigLoader> config;
        std::shared_ptr<IDeviceManager> device_mgr;
        std::shared_ptr<ISecurityManager> sec_mgr;
        std::shared_ptr<IProcessRunner> proc_runner;
        std::shared_ptr<IPackageManager> package_mgr;
        std::shared_ptr<IImageWriter> image_writer;
        std::shared_ptr<IPartitionManager> part_mgr;
        std::shared_ptr<IFilesystemManager> fs_mgr;
        std::shared_ptr<IBootControl> boot_ctrl;
        std::shared_ptr<ITransactionJournal> journal;
        std::shared_ptr<ServiceContext> service_ctx;
        std::shared_ptr<IJobManager> job_mgr;
        std::shared_ptr<IIPCServer> ipc_server;
    };

    /**
     * Create and wire all services.
     *
     * @param config_path  Path to installer.yaml.
     * @return             Populated Services bundle on success.
     */
    static Result<Services> create(const std::string& config_path);

    /**
     * Orderly shutdown: flush logs, commit journals, stop IPC.
     */
    static void shutdown(Services& services);
};

} // namespace installer

#endif // INSTALLER_SERVICE_SETUP_H
