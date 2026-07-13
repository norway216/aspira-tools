#include "src/job/steps/write_kernel.h"

namespace installer {

WriteKernelStep::WriteKernelStep(std::shared_ptr<IImageWriter> image_writer,
                                 std::shared_ptr<IPackageManager> pkg_mgr,
                                 std::shared_ptr<IPartitionManager> part_mgr,
                                 std::shared_ptr<ILogger> logger)
    : image_writer_(std::move(image_writer))
    , pkg_mgr_(std::move(pkg_mgr))
    , part_mgr_(std::move(part_mgr))
    , logger_(std::move(logger))
{
}

Result<void> WriteKernelStep::prepare(JobContext& ctx) {
    if (!image_writer_ || !pkg_mgr_ || !part_mgr_) {
        return Result<void>::err(InstallerError::make(
            ErrorCode::INTERNAL_CONFIG_ERROR,
            "Missing Dependency",
            "Required service not available for kernel write"));
    }

    // Determine the inactive kernel partition.
    std::string kernel_part = "kernel_b";
    if (ctx.target_slot == "B") {
        kernel_part = "kernel_a";
    }

    auto part_result = part_mgr_->find_partition(ctx.target_device, kernel_part);
    if (!part_result.is_ok()) {
        return Result<void>::err(InstallerError::make(
            ErrorCode::PARTITION_NOT_FOUND,
            "Kernel Partition Not Found",
            "Cannot find " + kernel_part + " partition",
            "device=" + ctx.target_device));
    }

    logger_->log(LogLevel::Info, step_id(), "prepare" + std::string(": ") + "Target kernel partition: " + kernel_part +
                       " -> " + part_result.value(, ctx.job_id));
    return Result<void>::ok();
}

Result<void> WriteKernelStep::execute(JobContext& ctx, ProgressCallback progress,
                                       CancellationToken& cancel) {
    logger_->log(LogLevel::Info, step_id(), "execute" + std::string(": ") + "Writing kernel image", ctx.job_id);

    if (cancel.is_cancelled()) {
        return Result<void>::err(InstallerError::make(
            ErrorCode::INTERNAL_CANCELLED, "Cancelled",
            "Kernel write cancelled"));
    }

    auto manifest_result = pkg_mgr_->load_manifest();
    if (!manifest_result.is_ok()) {
        return manifest_result;
    }

    // Find kernel payload.
    const PayloadEntry* kernel_entry = nullptr;
    for (const auto& payload : manifest_result.value().payloads) {
        if (payload.name == "kernel" ||
            payload.target == "kernel_inactive" ||
            payload.name.find("kernel") != std::string::npos) {
            kernel_entry = &payload;
            break;
        }
    }

    if (!kernel_entry) {
        return Result<void>::err(InstallerError::make(
            ErrorCode::PACKAGE_MANIFEST_ERROR,
            "No Kernel Payload",
            "Package manifest does not contain a kernel payload"));
    }

    // Determine target partition.
    std::string kernel_part = (ctx.target_slot == "B") ? "kernel_a" : "kernel_b";
    auto part_result = part_mgr_->find_partition(ctx.target_device, kernel_part);
    if (!part_result.is_ok()) {
        return part_result;
    }

    std::string target_path = part_result.value();
    std::string source_path = "payload/" + kernel_entry->file;

    if (progress) {
        progress(ProgressInfo{0, "Writing kernel to " + target_path,
                              source_path, 0, kernel_entry->size, 0.0});
    }

    // Write kernel image.
    Result<void> write_result;
    if (kernel_entry->type == "raw") {
        write_result = image_writer_->write_raw(
            source_path, target_path, kernel_entry->size,
            [&progress](const ProgressInfo& pi) {
                if (progress) progress(pi);
            },
            cancel);
    } else {
        write_result = image_writer_->write_decompressed(
            source_path, target_path, kernel_entry->uncompressed_size,
            [&progress](const ProgressInfo& pi) {
                if (progress) progress(pi);
            },
            cancel);
    }

    if (!write_result.is_ok()) {
        return write_result;
    }

    if (progress) {
        progress(ProgressInfo{100, "Kernel write complete",
                              target_path, kernel_entry->size, kernel_entry->size, 0.0});
    }

    logger_->log(LogLevel::Info, step_id(), "complete" + std::string(": ") + "Kernel written to " + target_path, ctx.job_id);
    return Result<void>::ok();
}

Result<void> WriteKernelStep::verify(JobContext& ctx, ProgressCallback progress,
                                      CancellationToken& cancel) {
    if (cancel.is_cancelled()) {
        return Result<void>::err(InstallerError::make(
            ErrorCode::INTERNAL_CANCELLED, "Cancelled",
            "Kernel verification cancelled"));
    }

    auto manifest_result = pkg_mgr_->load_manifest();
    if (!manifest_result.is_ok()) {
        return manifest_result;
    }

    const PayloadEntry* kernel_entry = nullptr;
    for (const auto& payload : manifest_result.value().payloads) {
        if (payload.name == "kernel" || payload.target == "kernel_inactive" ||
            payload.name.find("kernel") != std::string::npos) {
            kernel_entry = &payload;
            break;
        }
    }

    if (!kernel_entry) {
        return Result<void>::ok();  // nothing to verify
    }

    std::string kernel_part = (ctx.target_slot == "B") ? "kernel_a" : "kernel_b";
    auto part_result = part_mgr_->find_partition(ctx.target_device, kernel_part);
    if (!part_result.is_ok()) {
        return part_result;
    }

    if (progress) {
        progress(ProgressInfo{30, "Verifying kernel write...",
                              part_result.value(), 0, kernel_entry->size, 0.0});
    }

    auto verify_result = image_writer_->verify(
        "payload/" + kernel_entry->file,
        part_result.value(),
        kernel_entry->size,
        [&progress](const ProgressInfo& pi) {
            if (progress) {
                ProgressInfo adjusted = pi;
                adjusted.percent = 30 + (pi.percent * 70) / 100;
                progress(adjusted);
            }
        },
        cancel);

    if (!verify_result.is_ok()) {
        return verify_result;
    }

    logger_->log(LogLevel::Info, step_id(), "verify" + std::string(": ") + "Kernel write verified", ctx.job_id);
    return Result<void>::ok();
}

Result<void> WriteKernelStep::rollback(JobContext& ctx) {
    // Best-effort: mark the target partition as invalid by zeroing first sector.
    std::string kernel_part = (ctx.target_slot == "B") ? "kernel_a" : "kernel_b";
    auto part_result = part_mgr_->find_partition(ctx.target_device, kernel_part);
    if (part_result.is_ok()) {
        logger_->log(LogLevel::Info, step_id(), "rollback" + std::string(": ") + "Kernel partition " + part_result.value(, ctx.job_id) +
                           " left incomplete; will be re-written on next attempt");
    }
    return Result<void>::ok();
}

} // namespace installer
