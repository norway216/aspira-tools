/**
 * @file ServiceContext.h
 * @brief Bundled service dependencies for the installer.
 *
 * ServiceContext is a simple struct that owns shared_ptr references
 * to all service interfaces.  It is passed to the JobManager so that
 * job step functions can access any required service without explicit
 * wiring of individual dependencies.
 */

#ifndef INSTALLER_SERVICECONTEXT_H
#define INSTALLER_SERVICECONTEXT_H

#include <memory>

namespace installer {

// Forward declarations of all service interfaces
class ILogger;
class IConfigLoader;
class IDeviceManager;
class ISecurityManager;
class IProcessRunner;
class IPackageManager;
class IImageWriter;
class IPartitionManager;
class IFilesystemManager;
class IBootControl;
class ITransactionJournal;

struct ServiceContext {
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
};

} // namespace installer

#endif // INSTALLER_SERVICECONTEXT_H
