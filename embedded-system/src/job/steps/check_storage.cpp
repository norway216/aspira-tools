#include "src/job/steps/check_storage.h"

namespace installer {

CheckStorageStep::CheckStorageStep(std::shared_ptr<IDeviceManager> dev_mgr,
                                   std::shared_ptr<IPackageManager> pkg_mgr,
                                   std::shared_ptr<ILogger> logger)
    : dev_mgr_(std::move(dev_mgr))
    , pkg_mgr_(std::move(pkg_mgr))
    , logger_(std::move(logger))
{
}

Result<void> CheckStorageStep::prepare(JobContext& ctx) {
    if (!dev_mgr_ || !pkg_mgr_) {
        return Result<void>::err(InstallerError::make(
            ErrorCode::INTERNAL_CONFIG_ERROR,
            "Missing Dependency",
            "Device manager or package manager not available"));
    }
    return Result<void>::ok();
}

Result<void> CheckStorageStep::execute(JobContext& ctx, ProgressCallback progress,
                                        CancellationToken& cancel) {
    logger_->log(LogLevel::Info, step_id(), "execute" + std::string(": ") + "Checking storage capacity on " + ctx.target_device, ctx.job_id);

    if (progress) {
        progress(ProgressInfo{10, "Retrieving device information...", ctx.target_device, 0, 0, 0.0});
    }

    if (cancel.is_cancelled()) {
        return Result<void>::err(InstallerError::make(
            ErrorCode::INTERNAL_CANCELLED, "Cancelled", "Storage check cancelled"));
    }

    auto dev_info = dev_mgr_->get_device_info(ctx.target_device);
    if (!dev_info.is_ok()) {
        return dev_info;
    }

    const auto& info = dev_info.value();

    if (progress) {
        progress(ProgressInfo{40, "Computing required space...", "", 0, 0, 0.0});
    }

    // Sum up the total payload size from the manifest.
    auto manifest_result = pkg_mgr_->load_manifest();
    if (!manifest_result.is_ok()) {
        return manifest_result;
    }

    uint64_t total_required = 0;
    for (const auto& payload : manifest_result.value().payloads) {
        total_required += payload.uncompressed_size > 0
                          ? payload.uncompressed_size
                          : payload.size;
    }

    if (progress) {
        progress(ProgressInfo{70, "Comparing capacity...", "",
                              total_required, info.size_bytes, 0.0});
    }

    // Add 20% overhead margin for filesystem metadata, alignment, etc.
    uint64_t required_with_margin = total_required + (total_required / 5);

    if (info.size_bytes < required_with_margin) {
        return Result<void>::err(InstallerError::make(
            ErrorCode::DEVICE_CAPACITY_LOW,
            "Insufficient Storage",
            "The target device is too small for the installation package",
            "available=" + std::to_string(info.size_bytes) +
            " required=" + std::to_string(required_with_margin),
            false));
    }

    if (progress) {
        progress(ProgressInfo{100, "Storage check passed", "",
                              info.size_bytes, info.size_bytes, 0.0});
    }

    logger_->log(LogLevel::Info, step_id(), "complete" + std::string(": ") + "Storage check passed: available=" +
                       std::to_string(info.size_bytes, ctx.job_id) +
                       " required=" + std::to_string(required_with_margin));

    return Result<void>::ok();
}

Result<void> CheckStorageStep::verify(JobContext& ctx, ProgressCallback progress,
                                       CancellationToken& cancel) {
    (void)progress;
    if (cancel.is_cancelled()) {
        return Result<void>::err(InstallerError::make(
            ErrorCode::INTERNAL_CANCELLED, "Cancelled", "Verification cancelled"));
    }

    // Re-verify the device is still accessible and its capacity.
    auto dev_info = dev_mgr_->get_device_info(ctx.target_device);
    if (!dev_info.is_ok()) {
        return dev_info;
    }

    if (dev_info.value().size_bytes == 0) {
        return Result<void>::err(InstallerError::make(
            ErrorCode::DEVICE_CAPACITY_LOW,
            "Device Size Invalid",
            "Device reports zero capacity after check",
            "device=" + ctx.target_device));
    }

    return Result<void>::ok();
}

Result<void> CheckStorageStep::rollback(JobContext& ctx) {
    logger_->log(LogLevel::Info, step_id(), "rollback" + std::string(": ") + "No persistent changes to roll back", ctx.job_id);
    return Result<void>::ok();
}

} // namespace installer
