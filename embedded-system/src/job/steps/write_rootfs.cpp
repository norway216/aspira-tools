#include "src/job/steps/write_rootfs.h"

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
                       " -> " + part_result.value(, ctx.job_id));
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
    std::string source_path = "payload/" + rootfs_entry->file;
    uint64_t total_size = rootfs_entry->uncompressed_size > 0
                          ? rootfs_entry->uncompressed_size
                          : rootfs_entry->size;

    if (progress) {
        progress(ProgressInfo{0, "Writing rootfs to " + target_path + " ...",
                              source_path, 0, total_size, 0.0});
    }

    // Write rootfs. This is typically the largest payload and may use
    // compressed format (e.g. ext4_zstd, tar_zst).
    Result<void> write_result;
    if (rootfs_entry->type == "raw") {
        write_result = image_writer_->write_raw(
            source_path, target_path, rootfs_entry->size,
            [&progress](const ProgressInfo& pi) {
                if (progress) progress(pi);
            },
            cancel);
    } else {
        // Use decompressing writer for compressed payloads.
        write_result = image_writer_->write_decompressed(
            source_path, target_path, total_size,
            [&progress](const ProgressInfo& pi) {
                if (progress) progress(pi);
            },
            cancel);
    }

    if (!write_result.is_ok()) {
        return write_result;
    }

    if (progress) {
        progress(ProgressInfo{100, "Rootfs write complete",
                              target_path, total_size, total_size, 0.0});
    }

    logger_->log(LogLevel::Info, step_id(), "complete" + std::string(": ") + "Rootfs written to " + target_path +
                       " (" + std::to_string(total_size, ctx.job_id) + " bytes)");
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

    // Verify by reading back and comparing against source.
    auto verify_result = image_writer_->verify(
        "payload/" + rootfs_entry->file,
        part_result.value(),
        total_size,
        [&progress](const ProgressInfo& pi) {
            if (progress) progress(pi);
        },
        cancel);

    if (!verify_result.is_ok()) {
        logger_->log(LogLevel::Error, step_id(), "verify_failed" + std::string(": ") + "Rootfs verification failed: " +
                           verify_result.error(, ctx.job_id).code);
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
        logger_->log(LogLevel::Info, step_id(), "rollback" + std::string(": ") + "Rootfs partition " + part_result.value(, ctx.job_id) +
                           " left incomplete; will be re-written on next attempt");
    }
    return Result<void>::ok();
}

} // namespace installer
