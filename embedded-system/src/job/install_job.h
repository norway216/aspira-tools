/**
 * @file install_job.h
 * @brief Concrete job that performs a full system installation.
 *
 * Creates the 12-step installation chain and manages the job context.
 */

#ifndef INSTALLER_JOB_INSTALL_JOB_H
#define INSTALLER_JOB_INSTALL_JOB_H

#include "src/job/base_job.h"

#include <memory>
#include <string>

namespace installer {

// Forward declarations for service dependencies.
class IDeviceManager;
class IPackageManager;
class IImageWriter;
class IPartitionManager;
class IFilesystemManager;
class IBootControl;
class ISecurityManager;

class InstallJob : public BaseJob {
public:
    /**
     * @param job_id         Unique identifier for this job.
     * @param package_path   Path to the installation package (.espkg).
     * @param target_device  Block device to install to (e.g. /dev/mmcblk0).
     * @param target_slot    Boot slot to install to ("A" or "B").
     * @param dev_mgr        Device manager service.
     * @param pkg_mgr        Package manager service.
     * @param image_writer   Image writer service.
     * @param part_mgr       Partition manager service.
     * @param fs_mgr         Filesystem manager service.
     * @param boot_ctrl      Boot control service.
     * @param sec_mgr        Security manager service.
     * @param logger         Logger.
     */
    InstallJob(const std::string& job_id,
               const std::string& package_path,
               const std::string& target_device,
               const std::string& target_slot,
               std::shared_ptr<IDeviceManager> dev_mgr,
               std::shared_ptr<IPackageManager> pkg_mgr,
               std::shared_ptr<IImageWriter> image_writer,
               std::shared_ptr<IPartitionManager> part_mgr,
               std::shared_ptr<IFilesystemManager> fs_mgr,
               std::shared_ptr<IBootControl> boot_ctrl,
               std::shared_ptr<ISecurityManager> sec_mgr,
               std::shared_ptr<ILogger> logger);

    ~InstallJob() override = default;

    // Override start to set up JobContext before running the chain.
    Result<void> start(CancellationToken& cancel) override;

private:
    void build_step_chain(std::shared_ptr<IDeviceManager> dev_mgr,
                          std::shared_ptr<IPackageManager> pkg_mgr,
                          std::shared_ptr<IImageWriter> image_writer,
                          std::shared_ptr<IPartitionManager> part_mgr,
                          std::shared_ptr<IFilesystemManager> fs_mgr,
                          std::shared_ptr<IBootControl> boot_ctrl,
                          std::shared_ptr<ISecurityManager> sec_mgr);
};

} // namespace installer

#endif // INSTALLER_JOB_INSTALL_JOB_H
