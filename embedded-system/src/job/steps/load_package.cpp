#include "src/job/steps/load_package.h"

namespace installer {

LoadPackageStep::LoadPackageStep(std::shared_ptr<IPackageManager> pkg_mgr,
                                 std::shared_ptr<ILogger> logger)
    : pkg_mgr_(std::move(pkg_mgr))
    , logger_(std::move(logger))
{
}

Result<void> LoadPackageStep::prepare(JobContext& ctx) {
    if (!pkg_mgr_) {
        return Result<void>::err(InstallerError::make(
            ErrorCode::INTERNAL_CONFIG_ERROR,
            "No Package Manager",
            "Package manager is not available"));
    }
    if (ctx.package_path.empty()) {
        return Result<void>::err(InstallerError::make(
            ErrorCode::PACKAGE_INVALID_FORMAT,
            "No Package Path",
            "No package path was specified"));
    }
    return Result<void>::ok();
}

Result<void> LoadPackageStep::execute(JobContext& ctx, ProgressCallback progress,
                                       CancellationToken& cancel) {
    logger_->log(LogLevel::Info, step_id(), "execute" + std::string(": ") + "Opening package: " + ctx.package_path, ctx.job_id);

    if (progress) {
        progress(ProgressInfo{10, "Opening package...", ctx.package_path, 0, 0, 0.0});
    }

    if (cancel.is_cancelled()) {
        return Result<void>::err(InstallerError::make(
            ErrorCode::INTERNAL_CANCELLED, "Cancelled", "Package loading cancelled"));
    }

    auto open_result = pkg_mgr_->open(ctx.package_path);
    if (!open_result.is_ok()) {
        return open_result;
    }

    if (progress) {
        progress(ProgressInfo{50, "Loading manifest...", "", 0, 0, 0.0});
    }

    auto manifest_result = pkg_mgr_->load_manifest();
    if (manifest_result.is_err()) {
        pkg_mgr_->close();
        return Result<void>::err(manifest_result.take_error());
    }
    auto& manifest = manifest_result.value();

    // Store manifest in the job context for later steps.
    // We store the manifest data we need directly in ctx fields.
    logger_->log(LogLevel::Info, step_id(), "manifest_loaded" + std::string(": ") + "package=" + manifest.package_id + " job=" + ctx.job_id +
                       " version=" + manifest.version +
                       " product=" + manifest.product);

    if (progress) {
        progress(ProgressInfo{100, "Package loaded successfully", "", 0, 0, 0.0});
    }

    return Result<void>::ok();
}

Result<void> LoadPackageStep::verify(JobContext& ctx, ProgressCallback progress,
                                      CancellationToken& cancel) {
    (void)progress;
    if (cancel.is_cancelled()) {
        return Result<void>::err(InstallerError::make(
            ErrorCode::INTERNAL_CANCELLED, "Cancelled", "Verification cancelled"));
    }

    auto manifest_result = pkg_mgr_->load_manifest();
    if (manifest_result.is_err()) {
        return Result<void>::err(manifest_result.take_error());
    }
    auto& m = manifest_result.value();

    if (m.package_id.empty()) {
        return Result<void>::err(InstallerError::make(
            ErrorCode::PACKAGE_MANIFEST_ERROR,
            "Invalid Manifest",
            "Package manifest has an empty package ID"));
    }

    logger_->log(LogLevel::Info, step_id(), "verify" + std::string(": ") + "Manifest verified: package_id=" + m.package_id, ctx.job_id);
    return Result<void>::ok();
}

Result<void> LoadPackageStep::rollback(JobContext& ctx) {
    logger_->log(LogLevel::Info, step_id(), "rollback" + std::string(": ") + "Closing package", ctx.job_id);
    pkg_mgr_->close();
    return Result<void>::ok();
}

} // namespace installer
