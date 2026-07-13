#include "src/job/steps/prepare_partitions.h"

namespace installer {

PreparePartitionsStep::PreparePartitionsStep(std::shared_ptr<IPartitionManager> part_mgr,
                                             std::shared_ptr<IDeviceManager> dev_mgr,
                                             std::shared_ptr<ILogger> logger)
    : part_mgr_(std::move(part_mgr))
    , dev_mgr_(std::move(dev_mgr))
    , logger_(std::move(logger))
{
}

Result<void> PreparePartitionsStep::prepare(JobContext& ctx) {
    if (!part_mgr_) {
        return Result<void>::err(InstallerError::make(
            ErrorCode::INTERNAL_CONFIG_ERROR,
            "No Partition Manager",
            "Partition manager is not available"));
    }

    // Verify the device is still safe.
    if (!dev_mgr_->is_safe_target(ctx.target_device)) {
        return Result<void>::err(InstallerError::make(
            ErrorCode::DEVICE_NOT_SAFE_TARGET,
            "Unsafe Target",
            "The target device is no longer considered safe",
            "device=" + ctx.target_device));
    }

    logger_->log(LogLevel::Info, step_id(), "prepare" + std::string(": ") + "Ready to partition " + ctx.target_device, ctx.job_id);
    return Result<void>::ok();
}

Result<void> PreparePartitionsStep::execute(JobContext& ctx, ProgressCallback progress,
                                             CancellationToken& cancel) {
    logger_->log(LogLevel::Info, step_id(), "execute" + std::string(": ") + "Creating GPT partition table on " + ctx.target_device, ctx.job_id);

    if (progress) {
        progress(ProgressInfo{20, "Creating partition table...",
                              ctx.target_device, 0, 0, 0.0});
    }

    if (cancel.is_cancelled()) {
        return Result<void>::err(InstallerError::make(
            ErrorCode::INTERNAL_CANCELLED, "Cancelled",
            "Partition creation cancelled"));
    }

    // Create GPT partition table.
    auto table_result = part_mgr_->create_partition_table(ctx.target_device, "gpt");
    if (!table_result.is_ok()) {
        return table_result;
    }

    if (progress) {
        progress(ProgressInfo{40, "Creating boot partition...", "", 0, 0, 0.0});
    }

    // Create standard partitions for A/B layout.
    // Partition sizes in MiB: boot=512, recovery=4096, kernel_a=256, kernel_b=256,
    //                        rootfs_a=6144, rootfs_b=6144, config=1024, data=remaining

    auto create_part = [&](const std::string& name, uint64_t size_mib, const std::string& label) -> Result<void> {
        if (cancel.is_cancelled()) {
            return Result<void>::err(InstallerError::make(
                ErrorCode::INTERNAL_CANCELLED, "Cancelled",
                "Partition creation cancelled during " + name));
        }
        return part_mgr_->create_partition(ctx.target_device, name, size_mib, label);
    };

    // Create partitions in order.
    auto r = create_part("boot", 512, "BOOT");
    if (!r.is_ok()) return r;

    r = create_part("recovery", 4096, "RECOVERY");
    if (!r.is_ok()) return r;

    r = create_part("kernel_a", 256, "KERNEL_A");
    if (!r.is_ok()) return r;

    r = create_part("kernel_b", 256, "KERNEL_B");
    if (!r.is_ok()) return r;

    r = create_part("rootfs_a", 6144, "ROOT_A");
    if (!r.is_ok()) return r;

    if (progress) {
        progress(ProgressInfo{60, "Creating rootfs_b partition...", "", 0, 0, 0.0});
    }

    r = create_part("rootfs_b", 6144, "ROOT_B");
    if (!r.is_ok()) return r;

    r = create_part("config", 1024, "CONFIG");
    if (!r.is_ok()) return r;

    if (progress) {
        progress(ProgressInfo{80, "Creating data partition...", "", 0, 0, 0.0});
    }

    // data partition fills remaining space (size_mib=0).
    r = create_part("data", 0, "DATA");
    if (!r.is_ok()) return r;

    // Reread partition table so kernel knows about new partitions.
    if (progress) {
        progress(ProgressInfo{90, "Rereading partition table...", "", 0, 0, 0.0});
    }

    auto reread_result = part_mgr_->reread_partition_table(ctx.target_device);
    if (!reread_result.is_ok()) {
        logger_->log(LogLevel::Warn, step_id(), "reread_failed" + std::string(": ") + "Failed to reread partition table; may need reboot", ctx.job_id);
    }

    if (progress) {
        progress(ProgressInfo{100, "Partitions created successfully", "", 0, 0, 0.0});
    }

    logger_->log(LogLevel::Info, step_id(), "complete" + std::string(": ") + "GPT partition table created on " + ctx.target_device, ctx.job_id);
    return Result<void>::ok();
}

Result<void> PreparePartitionsStep::verify(JobContext& ctx, ProgressCallback progress,
                                            CancellationToken& cancel) {
    if (progress) {
        progress(ProgressInfo{50, "Verifying partition table...", ctx.target_device, 0, 0, 0.0});
    }

    if (cancel.is_cancelled()) {
        return Result<void>::err(InstallerError::make(
            ErrorCode::INTERNAL_CANCELLED, "Cancelled",
            "Partition verification cancelled"));
    }

    // Verify partitions exist by trying to find them.
    auto boot_result = part_mgr_->find_partition(ctx.target_device, "boot");
    if (!boot_result.is_ok()) {
        return Result<void>::err(InstallerError::make(
            ErrorCode::PARTITION_NOT_FOUND,
            "Partition Verification Failed",
            "Boot partition not found after creation",
            "device=" + ctx.target_device));
    }

    logger_->log(LogLevel::Info, step_id(), "verify" + std::string(": ") + "Partition table verified — at least the boot partition exists", ctx.job_id);
    return Result<void>::ok();
}

Result<void> PreparePartitionsStep::rollback(JobContext& ctx) {
    logger_->log(LogLevel::Warn, step_id(), "rollback" + std::string(": ") + "Partition table rollback not possible; device must be re-partitioned", ctx.job_id);
    // Partition table creation cannot easily be rolled back.
    // The device must be re-partitioned from scratch.
    return Result<void>::ok();
}

} // namespace installer
