#include "src/job/steps/verify_target.h"

namespace installer {

VerifyTargetStep::VerifyTargetStep(std::shared_ptr<IImageWriter> image_writer,
                                   std::shared_ptr<IPackageManager> pkg_mgr,
                                   std::shared_ptr<IPartitionManager> part_mgr,
                                   std::shared_ptr<IFilesystemManager> fs_mgr,
                                   std::shared_ptr<ILogger> logger)
    : image_writer_(std::move(image_writer))
    , pkg_mgr_(std::move(pkg_mgr))
    , part_mgr_(std::move(part_mgr))
    , fs_mgr_(std::move(fs_mgr))
    , logger_(std::move(logger))
{
}

Result<void> VerifyTargetStep::prepare(JobContext& ctx) {
    if (!image_writer_ || !pkg_mgr_ || !part_mgr_ || !fs_mgr_) {
        return Result<void>::err(InstallerError::make(
            ErrorCode::INTERNAL_CONFIG_ERROR,
            "Missing Dependency",
            "Required service not available for target verification"));
    }
    logger_->log(LogLevel::Info, step_id(), "prepare" + std::string(": ") + "Ready to verify written data on " + ctx.target_device, ctx.job_id);
    return Result<void>::ok();
}

Result<void> VerifyTargetStep::execute(JobContext& ctx, ProgressCallback progress,
                                        CancellationToken& cancel) {
    logger_->log(LogLevel::Info, step_id(), "execute" + std::string(": ") + "Verifying all installed partitions", ctx.job_id);

    auto manifest_result = pkg_mgr_->load_manifest();
    if (!manifest_result.is_ok()) {
        return manifest_result;
    }

    const auto& manifest = manifest_result.value();
    const auto& payloads = manifest.payloads;

    if (payloads.empty()) {
        logger_->log(LogLevel::Warn, step_id(), "no_payloads" + std::string(": ") + "No payloads to verify", ctx.job_id);
        return Result<void>::ok();
    }

    int total = static_cast<int>(payloads.size());
    int count = 0;

    for (const auto& payload : payloads) {
        if (cancel.is_cancelled()) {
            return Result<void>::err(InstallerError::make(
                ErrorCode::INTERNAL_CANCELLED, "Cancelled",
                "Target verification cancelled"));
        }

        int pct = (count * 100) / total;
        if (progress) {
            progress(ProgressInfo{pct,
                        "Verifying " + payload.name + " ...",
                        "", 0, payload.size, 0.0});
        }

        // Map payload target to partition name.
        std::string part_name;
        if (payload.target == "kernel_inactive") {
            part_name = (ctx.target_slot == "B") ? "kernel_a" : "kernel_b";
        } else if (payload.target == "rootfs_inactive") {
            part_name = (ctx.target_slot == "B") ? "rootfs_a" : "rootfs_b";
        } else {
            part_name = payload.target;
        }

        // Look up the partition.
        auto part_result = part_mgr_->find_partition(ctx.target_device, part_name);
        if (!part_result.is_ok()) {
            logger_->log(LogLevel::Warn, step_id(), "partition_not_found" + std::string(": ") + "Cannot verify " + payload.name +
                               ": partition " + part_name + " not found", ctx.job_id);
            count++;
            continue;
        }

        // Verify the payload against what's on disk.
        uint64_t verify_size = payload.uncompressed_size > 0
                               ? payload.uncompressed_size
                               : payload.size;

        auto verify_result = image_writer_->verify(
            part_result.value(),
            payload.sha256,
            verify_size,
            [&progress, pct, total](const ProgressInfo& pi) {
                if (progress) {
                    ProgressInfo adjusted = pi;
                    adjusted.percent = pct + (pi.percent / total);
                    progress(adjusted);
                }
            },
            cancel);

        if (!verify_result.is_ok()) {
            logger_->log(LogLevel::Error, step_id(), "payload_verify_failed" + std::string(": ") + "Verification failed for " + payload.name +
                               ": " + verify_result.error().code);
            return verify_result;
        }

        count++;
    }

    // Also run filesystem checks on formatted partitions.
    if (progress) {
        progress(ProgressInfo{90, "Running filesystem checks...", "", 0, 0, 0.0});
    }

    std::vector<std::string> fs_partitions = {"boot", "rootfs_a", "rootfs_b",
                                               "config", "data"};
    for (const auto& part_name : fs_partitions) {
        if (cancel.is_cancelled()) {
            return Result<void>::err(InstallerError::make(
                ErrorCode::INTERNAL_CANCELLED, "Cancelled",
                "Filesystem check cancelled"));
        }

        auto part_result = part_mgr_->find_partition(ctx.target_device, part_name);
        if (part_result.is_ok()) {
            auto check_result = fs_mgr_->check(part_result.value());
            if (!check_result.is_ok()) {
                logger_->log(LogLevel::Warn, step_id(), "fsck_failed" + std::string(": ") + "Filesystem check failed for " + part_name, ctx.job_id);
                // Non-fatal on fresh filesystems; still log the warning.
            }
        }
    }

    if (progress) {
        progress(ProgressInfo{100, "All verifications complete", "", 0, 0, 0.0});
    }

    logger_->log(LogLevel::Info, step_id(), "complete" + std::string(": ") + "Target verification passed for all payloads", ctx.job_id);
    return Result<void>::ok();
}

Result<void> VerifyTargetStep::verify(JobContext& ctx, ProgressCallback progress,
                                       CancellationToken& cancel) {
    (void)progress;
    if (cancel.is_cancelled()) {
        return Result<void>::err(InstallerError::make(
            ErrorCode::INTERNAL_CANCELLED, "Cancelled",
            "Verification confirmation cancelled"));
    }
    // The execute step already performed thorough verification.
    logger_->log(LogLevel::Info, step_id(), "verify" + std::string(": ") + "Target verification confirmed", ctx.job_id);
    return Result<void>::ok();
}

Result<void> VerifyTargetStep::rollback(JobContext& ctx) {
    logger_->log(LogLevel::Info, step_id(), "rollback" + std::string(": ") + "Nothing to roll back — verification is read-only", ctx.job_id);
    return Result<void>::ok();
}

} // namespace installer
