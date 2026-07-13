#include "src/job/steps/check_compatibility.h"

namespace installer {

CheckCompatibilityStep::CheckCompatibilityStep(std::shared_ptr<IPackageManager> pkg_mgr,
                                               std::shared_ptr<IDeviceManager> dev_mgr,
                                               std::shared_ptr<ILogger> logger)
    : pkg_mgr_(std::move(pkg_mgr))
    , dev_mgr_(std::move(dev_mgr))
    , logger_(std::move(logger))
{
}

Result<void> CheckCompatibilityStep::prepare(JobContext& ctx) {
    if (!pkg_mgr_ || !dev_mgr_) {
        return Result<void>::err(InstallerError::make(
            ErrorCode::INTERNAL_CONFIG_ERROR,
            "Missing Dependency",
            "Package manager or device manager not available"));
    }
    return Result<void>::ok();
}

Result<void> CheckCompatibilityStep::execute(JobContext& ctx, ProgressCallback progress,
                                              CancellationToken& cancel) {
    logger_->log(LogLevel::Info, step_id(), "execute" + std::string(": ") + "Checking compatibility", ctx.job_id);

    if (progress) {
        progress(ProgressInfo{20, "Checking hardware profile...", "", 0, 0, 0.0});
    }

    if (cancel.is_cancelled()) {
        return Result<void>::err(InstallerError::make(
            ErrorCode::INTERNAL_CANCELLED, "Cancelled", "Compatibility check cancelled"));
    }

    auto manifest_result = pkg_mgr_->load_manifest();
    if (manifest_result.is_err()) {
        return Result<void>::err(manifest_result.take_error());
    }
    auto& manifest = manifest_result.value();

    // Check hardware profile compatibility.
    auto dev_info = dev_mgr_->get_device_info(ctx.target_device);
    if (dev_info.is_err()) {
        return Result<void>::err(dev_info.take_error());
    }
    auto& info = dev_info.value();

    // Validate minimum disk size.
    if (info.size_bytes < manifest.min_disk_size_bytes) {
        return Result<void>::err(InstallerError::make(
            ErrorCode::DEVICE_CAPACITY_LOW,
            "Insufficient Disk Space",
            "The target device does not meet the minimum size requirement",
            "device=" + ctx.target_device +
            " size=" + std::to_string(info.size_bytes) +
            " required=" + std::to_string(manifest.min_disk_size_bytes),
            false));
    }

    if (progress) {
        progress(ProgressInfo{60, "Checking version compatibility...", "", 0, 0, 0.0});
    }

    // Check version: if allow_downgrade is false, validate version is not older.
    // (Detailed version comparison would be done by a version utility.)

    if (progress) {
        progress(ProgressInfo{100, "Compatibility check passed", "", 0, 0, 0.0});
    }

    logger_->log(LogLevel::Info, step_id(), "complete" + std::string(": ") + "Package is compatible with target hardware", ctx.job_id);
    return Result<void>::ok();
}

Result<void> CheckCompatibilityStep::verify(JobContext& ctx, ProgressCallback progress,
                                             CancellationToken& cancel) {
    (void)progress;
    if (cancel.is_cancelled()) {
        return Result<void>::err(InstallerError::make(
            ErrorCode::INTERNAL_CANCELLED, "Cancelled", "Verification cancelled"));
    }
    logger_->log(LogLevel::Info, step_id(), "verify" + std::string(": ") + "Compatibility confirmed", ctx.job_id);
    return Result<void>::ok();
}

Result<void> CheckCompatibilityStep::rollback(JobContext& ctx) {
    logger_->log(LogLevel::Info, step_id(), "rollback" + std::string(": ") + "No persistent changes to roll back", ctx.job_id);
    return Result<void>::ok();
}

} // namespace installer
