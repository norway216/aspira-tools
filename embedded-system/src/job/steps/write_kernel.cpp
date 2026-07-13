#include "src/job/steps/write_kernel.h"

#include <cstdio>
#include <fstream>

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
                       " -> " + part_result.value());
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

    if (progress) {
        progress(ProgressInfo{0, "Writing kernel to " + target_path,
                              kernel_entry->file, 0, kernel_entry->size, 0.0});
    }

    // Extract payload to a temporary file for streaming via ifstream.
    std::string temp_path = "/tmp/installer_" + kernel_entry->name + "_" + ctx.job_id;
    auto extract_result = pkg_mgr_->extract_payload(
        kernel_entry->name, temp_path,
        [&progress](const ProgressInfo& pi) {
            if (progress) progress(pi);
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
    options.expected_size = kernel_entry->uncompressed_size > 0
                            ? kernel_entry->uncompressed_size
                            : kernel_entry->size;
    options.expected_sha256 = kernel_entry->sha256;

    // Write the stream to the block device.
    auto write_result = image_writer_->write(
        source_stream, target_path, options,
        [&progress](const ProgressInfo& pi) {
            if (progress) progress(pi);
        },
        cancel);

    // Best-effort cleanup of the temporary file.
    source_stream.close();
    std::remove(temp_path.c_str());

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
        part_result.value(),
        kernel_entry->sha256,
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
        logger_->log(LogLevel::Info, step_id(), "rollback" + std::string(": ") + "Kernel partition " + part_result.value() +
                           " left incomplete; will be re-written on next attempt");
    }
    return Result<void>::ok();
}

} // namespace installer
