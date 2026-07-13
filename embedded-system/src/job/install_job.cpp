/**
 * @file install_job.cpp
 * @brief Implementation of the full system installation job.
 *
 * Step chain (from Architecture Doc section 7.2):
 *   1. DetectHardware     — 5%   — Identify target device
 *   2. LoadPackage        — 5%   — Open .espkg, load manifest
 *   3. VerifySignature    — 10%  — Ed25519 + payload hash verification
 *   4. CheckCompatibility — 5%   — HW profile, version, disk space
 *   5. CheckStorage       — 5%   — Verify capacity with margin
 *   6. PreparePartitions  — 10%  — Create GPT table and partitions
 *   7. CreateFilesystems  — 5%   — Format each partition
 *   8. WriteBootloader    — 3%   — Write bootloader image
 *   9. WriteKernel        — 5%   — Write kernel to inactive slot
 *   10. WriteRootfs       — 40%  — Write rootfs (largest step)
 *   11. VerifyTarget      — 5%   — Read-back verification
 *   12. ConfigureBootSlot — 2%   — Set U-Boot env for boot slot
 */

#include "src/job/install_job.h"

#include "installer/IDeviceManager.h"
#include "installer/IPackageManager.h"
#include "installer/image/iimage_writer.h"
#include "installer/partition/ipartition_manager.h"
#include "installer/filesystem/ifilesystem_manager.h"
#include "installer/IBootControl.h"
#include "installer/security/isecurity_manager.h"

#include "src/job/steps/detect_hardware.h"
#include "src/job/steps/load_package.h"
#include "src/job/steps/verify_signature.h"
#include "src/job/steps/check_compatibility.h"
#include "src/job/steps/check_storage.h"
#include "src/job/steps/prepare_partitions.h"
#include "src/job/steps/create_filesystems.h"
#include "src/job/steps/write_bootloader.h"
#include "src/job/steps/write_kernel.h"
#include "src/job/steps/write_rootfs.h"
#include "src/job/steps/verify_target.h"
#include "src/job/steps/configure_boot_slot.h"

namespace installer {

InstallJob::InstallJob(const std::string& job_id,
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
                       std::shared_ptr<ILogger> logger)
    : BaseJob(job_id, JobType::InstallSystem, logger)
{
    // Store installation parameters in the context.
    ctx_.job_id = job_id;
    ctx_.package_path = package_path;
    ctx_.target_device = target_device;
    ctx_.target_slot = target_slot;

    // Store service pointers (non-owning).
    ctx_.device_mgr   = dev_mgr.get();
    ctx_.package_mgr  = pkg_mgr.get();
    ctx_.image_writer = image_writer.get();
    ctx_.part_mgr     = part_mgr.get();
    ctx_.fs_mgr       = fs_mgr.get();
    ctx_.boot_ctrl    = boot_ctrl.get();
    ctx_.sec_mgr      = sec_mgr.get();
    // journal is set by the JobManager before start.
    // logger is set via BaseJob constructor.

    // Build the step chain.
    build_step_chain(std::move(dev_mgr), std::move(pkg_mgr),
                     std::move(image_writer), std::move(part_mgr),
                     std::move(fs_mgr), std::move(boot_ctrl),
                     std::move(sec_mgr));

    logger_->log(LogLevel::Info, "InstallJob", "constructed" + std::string(": ") + "InstallJob created for device=" + target_device +
                       " slot=" + target_slot +
                       " package=" + package_path +
                       " steps=" + std::to_string(steps_.size(, job_id)));
}

Result<void> InstallJob::start(CancellationToken& cancel) {
    // Validate context before starting.
    if (ctx_.target_device.empty()) {
        return Result<void>::err(InstallerError::make(
            ErrorCode::INTERNAL_CONFIG_ERROR,
            "Invalid Context",
            "Target device is not set",
            "job_id=" + job_id()));
    }

    if (ctx_.package_path.empty()) {
        return Result<void>::err(InstallerError::make(
            ErrorCode::INTERNAL_CONFIG_ERROR,
            "Invalid Context",
            "Package path is not set",
            "job_id=" + job_id()));
    }

    if (ctx_.target_slot != "A" && ctx_.target_slot != "B") {
        return Result<void>::err(InstallerError::make(
            ErrorCode::INTERNAL_CONFIG_ERROR,
            "Invalid Context",
            "Target slot must be 'A' or 'B'",
            "job_id=" + job_id() + " slot=" + ctx_.target_slot));
    }

    logger_->log(LogLevel::Info, "InstallJob", "start" + std::string(": ") + "Starting system installation to " + ctx_.target_device +
                       " slot " + ctx_.target_slot, job_id());

    return run_chain(ctx_, cancel);
}

void InstallJob::build_step_chain(
    std::shared_ptr<IDeviceManager> dev_mgr,
    std::shared_ptr<IPackageManager> pkg_mgr,
    std::shared_ptr<IImageWriter> image_writer,
    std::shared_ptr<IPartitionManager> part_mgr,
    std::shared_ptr<IFilesystemManager> fs_mgr,
    std::shared_ptr<IBootControl> boot_ctrl,
    std::shared_ptr<ISecurityManager> sec_mgr)
{
    // The step chain is ordered: each step depends on the previous.
    // Weights total 100%:
    //   5+5+10+5+5+10+5+3+5+40+5+2 = 100

    add_step(std::make_unique<DetectHardwareStep>(dev_mgr, logger_));
    add_step(std::make_unique<LoadPackageStep>(pkg_mgr, logger_));
    add_step(std::make_unique<VerifySignatureStep>(sec_mgr, pkg_mgr, logger_));
    add_step(std::make_unique<CheckCompatibilityStep>(pkg_mgr, dev_mgr, logger_));
    add_step(std::make_unique<CheckStorageStep>(dev_mgr, pkg_mgr, logger_));
    add_step(std::make_unique<PreparePartitionsStep>(part_mgr, dev_mgr, logger_));
    add_step(std::make_unique<CreateFilesystemsStep>(fs_mgr, part_mgr, logger_));
    add_step(std::make_unique<WriteBootloaderStep>(image_writer, pkg_mgr, logger_));
    add_step(std::make_unique<WriteKernelStep>(image_writer, pkg_mgr, part_mgr, logger_));
    add_step(std::make_unique<WriteRootfsStep>(image_writer, pkg_mgr, part_mgr, logger_));
    add_step(std::make_unique<VerifyTargetStep>(image_writer, pkg_mgr, part_mgr, fs_mgr, logger_));
    add_step(std::make_unique<ConfigureBootSlotStep>(boot_ctrl, logger_));
}

} // namespace installer
