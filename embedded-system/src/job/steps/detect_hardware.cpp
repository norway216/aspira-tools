#include "src/job/steps/detect_hardware.h"

namespace installer {

DetectHardwareStep::DetectHardwareStep(std::shared_ptr<IDeviceManager> dev_mgr,
                                       std::shared_ptr<ILogger> logger)
    : dev_mgr_(std::move(dev_mgr))
    , logger_(std::move(logger))
{
}

Result<void> DetectHardwareStep::prepare(JobContext& ctx) {
    logger_->log(LogLevel::Info, step_id(), "prepare" + std::string(": ") + "Checking device manager availability", ctx.job_id);
    if (!dev_mgr_) {
        return Result<void>::err(InstallerError::make(
            ErrorCode::INTERNAL_CONFIG_ERROR,
            "No Device Manager",
            "Device manager is not available",
            "component=DetectHardwareStep"));
    }
    return Result<void>::ok();
}

Result<void> DetectHardwareStep::execute(JobContext& ctx, ProgressCallback progress,
                                          CancellationToken& cancel) {
    logger_->log(LogLevel::Info, step_id(), "execute" + std::string(": ") + "Scanning for target device: " + ctx.target_device, ctx.job_id);

    if (progress) {
        progress(ProgressInfo{10, "Scanning block devices...", "", 0, 0, 0.0});
    }

    if (cancel.is_cancelled()) {
        return Result<void>::err(InstallerError::make(
            ErrorCode::INTERNAL_CANCELLED,
            "Cancelled",
            "Hardware detection cancelled",
            "step=detect_hardware"));
    }

    if (ctx.target_device.empty()) {
        return Result<void>::err(InstallerError::make(
            ErrorCode::DEVICE_NOT_FOUND,
            "No Target Device",
            "No target device was specified for installation",
            "step=detect_hardware"));
    }

    // Resolve the device info.
    auto dev_info = dev_mgr_->get_device_info(ctx.target_device);
    if (!dev_info.is_ok()) {
        return Result<void>::err(dev_info.take_error());
    }

    auto info = dev_info.value();
    logger_->log(LogLevel::Info, step_id(), "device_found" + std::string(": ") + "path=" + info.path +
                       " model=" + info.model +
                       " size=" + std::to_string(info.size_bytes, ctx.job_id) +
                       " type=" + std::to_string(static_cast<int>(info.type)));

    if (progress) {
        progress(ProgressInfo{50, "Checking device safety...", info.path, 0, info.size_bytes, 0.0});
    }

    // Verify it is a safe target.
    if (!dev_mgr_->is_safe_target(ctx.target_device)) {
        return Result<void>::err(InstallerError::make(
            ErrorCode::DEVICE_NOT_SAFE_TARGET,
            "Unsafe Target Device",
            "The selected device is not a safe target for installation. "
            "It may be the system disk, installer media, or mounted.",
            "device=" + ctx.target_device));
    }

    // Ensure the device is accessible.
    auto wait_result = dev_mgr_->wait_for_device(ctx.target_device, 5000);
    if (!wait_result.is_ok()) {
        return Result<void>::err(wait_result.take_error());
    }

    if (progress) {
        progress(ProgressInfo{100, "Hardware detection complete", info.path,
                              info.size_bytes, info.size_bytes, 0.0});
    }

    logger_->log(LogLevel::Info, step_id(), "complete" + std::string(": ") + "Hardware detection successful", ctx.job_id);

    return Result<void>::ok();
}

Result<void> DetectHardwareStep::verify(JobContext& ctx, ProgressCallback progress,
                                         CancellationToken& cancel) {
    (void)progress;
    if (cancel.is_cancelled()) {
        return Result<void>::err(InstallerError::make(
            ErrorCode::INTERNAL_CANCELLED, "Cancelled",
            "Verification cancelled"));
    }

    // Verify the device is still accessible.
    auto dev_info = dev_mgr_->get_device_info(ctx.target_device);
    if (!dev_info.is_ok()) {
        return Result<void>::err(dev_info.take_error());
    }

    if (dev_info.value().read_only) {
        return Result<void>::err(InstallerError::make(
            ErrorCode::DEVICE_READ_ONLY,
            "Device is Read-Only",
            "The target device is read-only and cannot be written to",
            "device=" + ctx.target_device));
    }

    if (dev_info.value().size_bytes == 0) {
        return Result<void>::err(InstallerError::make(
            ErrorCode::DEVICE_CAPACITY_LOW,
            "Invalid Device Size",
            "The target device reports zero capacity",
            "device=" + ctx.target_device));
    }

    return Result<void>::ok();
}

Result<void> DetectHardwareStep::rollback(JobContext& ctx) {
    logger_->log(LogLevel::Info, step_id(), "rollback" + std::string(": ") + "Nothing to roll back for hardware detection", ctx.job_id);
    return Result<void>::ok();
}

} // namespace installer
