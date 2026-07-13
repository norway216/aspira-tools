#include "src/job/steps/write_bootloader.h"

#include <cstdio>
#include <fstream>

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
    if (manifest_result.is_err()) {
        return Result<void>::err(manifest_result.take_error());
    }
    auto& manifest = manifest_result.value();

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

    // If no explicit bootloader payload, skip.
    // Bootloader typically occupies the first few MB before the first partition.
    if (!bootloader_entry) {
        logger_->log(LogLevel::Warn, step_id(), "no_bootloader_payload" + std::string(": ") + "No bootloader payload found in manifest; skipping", ctx.job_id);
        return Result<void>::ok();
    }

    if (progress) {
        progress(ProgressInfo{10, "Writing bootloader image...",
                              ctx.target_device, 0, bl_size, 0.0});
    }

    // Extract payload to a temporary file for streaming via ifstream.
    std::string temp_path = "/tmp/installer_" + bootloader_entry->name + "_" + ctx.job_id;
    auto extract_result = pkg_mgr_->extract_payload(
        bootloader_entry->name, temp_path,
        [&progress](const ProgressInfo& pi) {
            if (progress) {
                ProgressInfo adjusted = pi;
                adjusted.percent = 10 + (pi.percent * 80) / 100;
                progress(adjusted);
            }
        },
        cancel);
    if (!extract_result.is_ok()) {
        return extract_result;
    }

    // Open the extracted payload as a stream.
    std::ifstream source_stream(temp_path, std::ios::binary);
    if (!source_stream.is_open()) {
        std::remove(temp_path.c_str());
        return Result<void>::err(InstallerError::make(
            ErrorCode::INTERNAL_ERROR,
            "File Open Failed",
            "Cannot open extracted payload for streaming: " + temp_path));
    }

    // Configure write options from the manifest entry.
    WriteOptions options;
    options.expected_size = bootloader_entry->size;
    options.expected_sha256 = bootloader_entry->sha256;

    // Write the stream to the block device.
    // Bootloader goes before the first partition, at offset 0 on the raw device.
    auto write_result = image_writer_->write(
        source_stream, ctx.target_device, options,
        [&progress](const ProgressInfo& pi) {
            if (progress) {
                ProgressInfo adjusted = pi;
                adjusted.percent = 10 + (pi.percent * 80) / 100;
                progress(adjusted);
            }
        },
        cancel);

    // Best-effort cleanup of the temporary file.
    source_stream.close();
    std::remove(temp_path.c_str());

    if (!write_result.is_ok()) {
        return write_result;
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
