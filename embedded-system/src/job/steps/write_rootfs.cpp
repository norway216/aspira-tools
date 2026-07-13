#include "src/job/steps/write_rootfs.h"

#include <cstdio>
#include <fstream>

namespace installer {

WriteRootfsStep::WriteRootfsStep(std::shared_ptr<IImageWriter> image_writer,
                                 std::shared_ptr<IPackageManager> pkg_mgr,
                                 std::shared_ptr<IPartitionManager> part_mgr,
                                 std::shared_ptr<ILogger> logger)
    : image_writer_(std::move(image_writer))
    , pkg_mgr_(std::move(pkg_mgr))
    , part_mgr_(std::move(part_mgr))
    , logger_(std::move(logger))
{
}

Result<void> WriteRootfsStep::prepare(JobContext& ctx) {
    if (!image_writer_ || !pkg_mgr_ || !part_mgr_) {
        return Result<void>::err(InstallerError::make(
            ErrorCode::INTERNAL_CONFIG_ERROR,
            "Missing Dependency",
            "Required service not available for rootfs write"));
    }

    // Determine the inactive rootfs partition.
    std::string rootfs_part = "rootfs_b";
    if (ctx.target_slot == "B") {
        rootfs_part = "rootfs_a";
    }

    auto part_result = part_mgr_->find_partition(ctx.target_device, rootfs_part);
    if (!part_result.is_ok()) {
        return Result<void>::err(InstallerError::make(
            ErrorCode::PARTITION_NOT_FOUND,
            "Rootfs Partition Not Found",
            "Cannot find " + rootfs_part + " partition",
            "device=" + ctx.target_device));
    }

    logger_->log(LogLevel::Info, step_id(), "prepare" + std::string(": ") + "Target rootfs partition: " + rootfs_part +
                       " -> " + part_result.value());
    return Result<void>::ok();
}

Result<void> WriteRootfsStep::execute(JobContext& ctx, ProgressCallback progress,
                                       CancellationToken& cancel) {
    logger_->log(LogLevel::Info, step_id(), "execute" + std::string(": ") + "Writing root filesystem — this is the largest step", ctx.job_id);

    if (cancel.is_cancelled()) {
        return Result<void>::err(InstallerError::make(
            ErrorCode::INTERNAL_CANCELLED, "Cancelled",
            "Rootfs write cancelled"));
    }

    auto manifest_result = pkg_mgr_->load_manifest();
    if (!manifest_result.is_ok()) {
        return manifest_result;
    }

    // Find rootfs payload (may be named "rootfs" or target "rootfs_inactive").
    const PayloadEntry* rootfs_entry = nullptr;
    for (const auto& payload : manifest_result.value().payloads) {
        if (payload.name == "rootfs" ||
            payload.target == "rootfs_inactive" ||
            payload.name.find("rootfs") != std::string::npos) {
            rootfs_entry = &payload;
            break;
        }
    }

    if (!rootfs_entry) {
        return Result<void>::err(InstallerError::make(
            ErrorCode::PACKAGE_MANIFEST_ERROR,
            "No Rootfs Payload",
            "Package manifest does not contain a rootfs payload"));
    }

    // Determine target partition.
    std::string rootfs_part = (ctx.target_slot == "B") ? "rootfs_a" : "rootfs_b";
    auto part_result = part_mgr_->find_partition(ctx.target_device, rootfs_part);
    if (!part_result.is_ok()) {
        return part_result;
    }

    std::string target_path = part_result.value();
    uint64_t total_size = rootfs_entry->uncompressed_size > 0
                          ? rootfs_entry->uncompressed_size
                          : rootfs_entry->size;

    if (progress) {
        progress(ProgressInfo{0, "Writing rootfs to " + target_path + " ...",
                              rootfs_entry->file, 0, total_size, 0.0});
    }

    // Extract payload to a temporary file for streaming via ifstream.
    std::string temp_path = "/tmp/installer_" + rootfs_entry->name + "_" + ctx.job_id;
    auto extract_result = pkg_mgr_->extract_payload(
        rootfs_entry->name, temp_path,
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
    options.expected_size = total_size;
    options.expected_sha256 = rootfs_entry->sha256;

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
        progress(ProgressInfo{100, "Rootfs write complete",
                              target_path, total_size, total_size, 0.0});
    }

    logger_->log(LogLevel::Info, step_id(), "complete" + std::string(": ") + "Rootfs written to " + target_path +
                       " (" + std::to_string(total_size) + " job=" + ctx.job_id + " bytes)");
    return Result<void>::ok();
}

Result<void> WriteRootfsStep::verify(JobContext& ctx, ProgressCallback progress,
                                      CancellationToken& cancel) {
    if (cancel.is_cancelled()) {
        return Result<void>::err(InstallerError::make(
            ErrorCode::INTERNAL_CANCELLED, "Cancelled",
            "Rootfs verification cancelled"));
    }

    auto manifest_result = pkg_mgr_->load_manifest();
    if (!manifest_result.is_ok()) {
        return manifest_result;
    }

    const PayloadEntry* rootfs_entry = nullptr;
    for (const auto& payload : manifest_result.value().payloads) {
        if (payload.name == "rootfs" || payload.target == "rootfs_inactive" ||
            payload.name.find("rootfs") != std::string::npos) {
            rootfs_entry = &payload;
            break;
        }
    }

    if (!rootfs_entry) {
        return Result<void>::err(InstallerError::make(
            ErrorCode::PACKAGE_MANIFEST_ERROR,
            "No Rootfs Payload",
            "Cannot verify: no rootfs payload in manifest"));
    }

    std::string rootfs_part = (ctx.target_slot == "B") ? "rootfs_a" : "rootfs_b";
    auto part_result = part_mgr_->find_partition(ctx.target_device, rootfs_part);
    if (!part_result.is_ok()) {
        return part_result;
    }

    uint64_t total_size = rootfs_entry->uncompressed_size > 0
                          ? rootfs_entry->uncompressed_size
                          : rootfs_entry->size;

    if (progress) {
        progress(ProgressInfo{0, "Verifying rootfs (read-back comparison)...",
                              part_result.value(), 0, total_size, 0.0});
    }

    // Verify by reading back from the device and comparing the SHA-256 hash.
    auto verify_result = image_writer_->verify(
        part_result.value(),
        rootfs_entry->sha256,
        total_size,
        [&progress](const ProgressInfo& pi) {
            if (progress) progress(pi);
        },
        cancel);

    if (!verify_result.is_ok()) {
        logger_->log(LogLevel::Error, step_id(), "verify_failed" + std::string(": ") + "Rootfs verification failed: " +
                           verify_result.error().code);
        return verify_result;
    }

    if (progress) {
        progress(ProgressInfo{100, "Rootfs verification complete",
                              part_result.value(), total_size, total_size, 0.0});
    }

    logger_->log(LogLevel::Info, step_id(), "verify" + std::string(": ") + "Rootfs write verified successfully", ctx.job_id);
    return Result<void>::ok();
}

Result<void> WriteRootfsStep::rollback(JobContext& ctx) {
    std::string rootfs_part = (ctx.target_slot == "B") ? "rootfs_a" : "rootfs_b";
    auto part_result = part_mgr_->find_partition(ctx.target_device, rootfs_part);
    if (part_result.is_ok()) {
        logger_->log(LogLevel::Info, step_id(), "rollback" + std::string(": ") + "Rootfs partition " + part_result.value() +
                           " left incomplete; will be re-written on next attempt");
    }
    return Result<void>::ok();
}

} // namespace installer
