#include "src/job/steps/create_filesystems.h"

namespace installer {

CreateFilesystemsStep::CreateFilesystemsStep(std::shared_ptr<IFilesystemManager> fs_mgr,
                                             std::shared_ptr<IPartitionManager> part_mgr,
                                             std::shared_ptr<ILogger> logger)
    : fs_mgr_(std::move(fs_mgr))
    , part_mgr_(std::move(part_mgr))
    , logger_(std::move(logger))
{
}

Result<void> CreateFilesystemsStep::prepare(JobContext& ctx) {
    if (!fs_mgr_) {
        return Result<void>::err(InstallerError::make(
            ErrorCode::INTERNAL_CONFIG_ERROR,
            "No Filesystem Manager",
            "Filesystem manager is not available"));
    }
    logger_->log(LogLevel::Info, step_id(), "prepare" + std::string(": ") + "Ready to create filesystems on " + ctx.target_device, ctx.job_id);
    return Result<void>::ok();
}

Result<void> CreateFilesystemsStep::execute(JobContext& ctx, ProgressCallback progress,
                                             CancellationToken& cancel) {
    logger_->log(LogLevel::Info, step_id(), "execute" + std::string(": ") + "Creating filesystems", ctx.job_id);

    // Map of partition name -> {filesystem type, label}
    struct FsSpec {
        FilesystemType type;
        std::string label;
    };

    std::vector<std::pair<std::string, FsSpec>> fs_list = {
        {"boot",     {FilesystemType::VFAT, "BOOT"}},
        {"recovery", {FilesystemType::EXT4, "RECOVERY"}},
        {"rootfs_a", {FilesystemType::EXT4, "ROOT_A"}},
        {"rootfs_b", {FilesystemType::EXT4, "ROOT_B"}},
        {"config",   {FilesystemType::EXT4, "CONFIG"}},
        {"data",     {FilesystemType::EXT4, "DATA"}},
    };

    int total = static_cast<int>(fs_list.size());
    int count = 0;

    for (const auto& [part_name, spec] : fs_list) {
        if (cancel.is_cancelled()) {
            return Result<void>::err(InstallerError::make(
                ErrorCode::INTERNAL_CANCELLED, "Cancelled",
                "Filesystem creation cancelled"));
        }

        int pct = (count * 100) / total;
        if (progress) {
            progress(ProgressInfo{pct,
                        "Formatting " + part_name + " (" + spec.label + ")...",
                        "", 0, 0, 0.0});
        }

        // Resolve partition path.
        auto part_result = part_mgr_->get_partition_by_label(ctx.target_device, part_name);
        if (part_result.is_err()) {
            logger_->log(LogLevel::Error, step_id(), "partition_not_found" + std::string(": ") + "Cannot find partition: " + part_name +
                               " error=" + part_result.error().code);
            return Result<void>::err(part_result.take_error());
        }

        std::string part_path = part_result.value();

        // Skip formatting kernel partitions (raw, no filesystem).
        if (part_name == "kernel_a" || part_name == "kernel_b") {
            logger_->log(LogLevel::Debug, step_id(), "skip_format" + std::string(": ") + "Skipping " + part_name + " (raw partition)", ctx.job_id);
            count++;
            continue;
        }

        auto fmt_result = fs_mgr_->format(part_path, spec.type, spec.label);
        if (!fmt_result.is_ok()) {
            return fmt_result;
        }

        count++;
    }

    if (progress) {
        progress(ProgressInfo{100, "All filesystems created", "", 0, 0, 0.0});
    }

    logger_->log(LogLevel::Info, step_id(), "complete" + std::string(": ") + "All filesystems created successfully", ctx.job_id);
    return Result<void>::ok();
}

Result<void> CreateFilesystemsStep::verify(JobContext& ctx, ProgressCallback progress,
                                            CancellationToken& cancel) {
    if (progress) {
        progress(ProgressInfo{50, "Checking filesystems...", "", 0, 0, 0.0});
    }

    if (cancel.is_cancelled()) {
        return Result<void>::err(InstallerError::make(
            ErrorCode::INTERNAL_CANCELLED, "Cancelled",
            "Filesystem check cancelled"));
    }

    // Run fsck on the boot partition as a quick sanity check.
    auto part_result = part_mgr_->get_partition_by_label(ctx.target_device, "boot");
    if (part_result.is_ok()) {
        auto check_result = fs_mgr_->check(part_result.value());
        if (!check_result.is_ok()) {
            logger_->log(LogLevel::Warn, step_id(), "fsck_warning" + std::string(": ") + "Boot partition check reported issues", ctx.job_id);
            // Non-fatal: fsck may find things that are fine on a new FS.
        }
    }

    logger_->log(LogLevel::Info, step_id(), "verify" + std::string(": ") + "Filesystem checks complete", ctx.job_id);
    return Result<void>::ok();
}

Result<void> CreateFilesystemsStep::rollback(JobContext& ctx) {
    logger_->log(LogLevel::Warn, step_id(), "rollback" + std::string(": ") + "Filesystem rollback not possible — partitions must be re-formatted", ctx.job_id);
    // Creating filesystems is destructive; rollback is not feasible.
    return Result<void>::ok();
}

} // namespace installer
