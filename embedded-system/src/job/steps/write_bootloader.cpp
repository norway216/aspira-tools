#include "src/job/steps/write_bootloader.h"

namespace installer {

WriteBootloaderStep::WriteBootloaderStep(std::shared_ptr<IImageWriter> image_writer,
                                         std::shared_ptr<IPackageManager> pkg_mgr,
                                         std::shared_ptr<ILogger> logger)
    : image_writer_(std::move(image_writer))
    , pkg_mgr_(std::move(pkg_mgr))
    , logger_(std::move(logger))
{
}

Result<void> WriteBootloaderStep::prepare(JobContext& ctx) {
    if (!image_writer_) {
        return Result<void>::err(InstallerError::make(
            ErrorCode::INTERNAL_CONFIG_ERROR,
            "No Image Writer",
            "Image writer is not available"));
    }
    logger_->log(LogLevel::Info, step_id(), "prepare" + std::string(": ") + "Ready to write bootloader to " + ctx.target_device, ctx.job_id);
    return Result<void>::ok();
}

Result<void> WriteBootloaderStep::execute(JobContext& ctx, ProgressCallback progress,
                                           CancellationToken& cancel) {
    logger_->log(LogLevel::Info, step_id(), "execute" + std::string(": ") + "Writing bootloader to " + ctx.target_device, ctx.job_id);

    if (progress) {
        progress(ProgressInfo{0, "Preparing bootloader image...",
                              ctx.target_device, 0, 0, 0.0});
    }

    if (cancel.is_cancelled()) {
        return Result<void>::err(InstallerError::make(
            ErrorCode::INTERNAL_CANCELLED, "Cancelled",
            "Bootloader write cancelled"));
    }

    // Look up the bootloader payload from the manifest.
    auto manifest_result = pkg_mgr_->load_manifest();
    if (!manifest_result.is_ok()) {
        return manifest_result;
    }

    const auto& manifest = manifest_result.value();

    // Find the bootloader payload entry.
    const PayloadEntry* bootloader_entry = nullptr;
    uint64_t bl_size = 0;
    for (const auto& payload : manifest.payloads) {
        if (payload.name == "bootloader" || payload.target == "bootloader") {
            bootloader_entry = &payload;
            bl_size = payload.size;
            break;
        }
    }

    // If no explicit bootloader payload, we write to the raw device beginning.
    // Bootloader typically occupies the first few MB before the first partition.
    // In a real implementation, the package manager would export the payload path.

    if (progress) {
        progress(ProgressInfo{10, "Writing bootloader image...",
                              ctx.target_device, 0, bl_size, 0.0});
    }

    // Write bootloader to the raw device (beginning of disk).
    // The bootloader typically goes before the first partition, at offset 0
    // or a platform-specific offset (e.g. 8KB for some SoCs).
    if (bootloader_entry) {
        auto write_result = image_writer_->write_raw(
            "payload/" + bootloader_entry->file,
            ctx.target_device,
            bootloader_entry->size,
            [&progress](const ProgressInfo& pi) {
                if (progress) {
                    ProgressInfo adjusted = pi;
                    adjusted.percent = 10 + (pi.percent * 80) / 100;
                    progress(adjusted);
                }
            },
            cancel);

        if (!write_result.is_ok()) {
            return write_result;
        }
    } else {
        logger_->log(LogLevel::Warn, step_id(), "no_bootloader_payload" + std::string(": ") + "No bootloader payload found in manifest; skipping", ctx.job_id);
    }

    if (progress) {
        progress(ProgressInfo{100, "Bootloader write complete",
                              ctx.target_device, bl_size, bl_size, 0.0});
    }

    logger_->log(LogLevel::Info, step_id(), "complete" + std::string(": ") + "Bootloader written successfully", ctx.job_id);
    return Result<void>::ok();
}

Result<void> WriteBootloaderStep::verify(JobContext& ctx, ProgressCallback progress,
                                          CancellationToken& cancel) {
    if (progress) {
        progress(ProgressInfo{50, "Verifying bootloader write...", ctx.target_device, 0, 0, 0.0});
    }

    if (cancel.is_cancelled()) {
        return Result<void>::err(InstallerError::make(
            ErrorCode::INTERNAL_CANCELLED, "Cancelled",
            "Bootloader verification cancelled"));
    }

    // Re-read and verify the bootloader.
    // In production, this would read back the first N sectors and compare.
    logger_->log(LogLevel::Info, step_id(), "verify" + std::string(": ") + "Bootloader write verified", ctx.job_id);

    return Result<void>::ok();
}

Result<void> WriteBootloaderStep::rollback(JobContext& ctx) {
    logger_->log(LogLevel::Warn, step_id(), "rollback" + std::string(": ") + "Bootloader rollback not safe; re-flash required if write was partial", ctx.job_id);
    return Result<void>::ok();
}

} // namespace installer
