#include "src/job/steps/configure_boot_slot.h"

namespace installer {

ConfigureBootSlotStep::ConfigureBootSlotStep(std::shared_ptr<IBootControl> boot_ctrl,
                                             std::shared_ptr<ILogger> logger)
    : boot_ctrl_(std::move(boot_ctrl))
    , logger_(std::move(logger))
{
}

Result<void> ConfigureBootSlotStep::prepare(JobContext& ctx) {
    if (!boot_ctrl_) {
        return Result<void>::err(InstallerError::make(
            ErrorCode::INTERNAL_CONFIG_ERROR,
            "No Boot Control",
            "Boot control manager is not available"));
    }

    // Validate the target slot is either A or B.
    if (ctx.target_slot != "A" && ctx.target_slot != "B") {
        return Result<void>::err(InstallerError::make(
            ErrorCode::BOOT_SLOT_INVALID,
            "Invalid Boot Slot",
            "Target slot must be 'A' or 'B'",
            "slot=" + ctx.target_slot));
    }

    logger_->log(LogLevel::Info, step_id(), "prepare" + std::string(": ") + "Ready to configure boot slot " + ctx.target_slot, ctx.job_id);
    return Result<void>::ok();
}

Result<void> ConfigureBootSlotStep::execute(JobContext& ctx, ProgressCallback progress,
                                             CancellationToken& cancel) {
    logger_->log(LogLevel::Info, step_id(), "execute" + std::string(": ") + "Configuring boot environment for slot " + ctx.target_slot, ctx.job_id);

    // This step is NOT cancellable once started — it is a critical atomic
    // operation that must complete to avoid leaving the bootloader in an
    // inconsistent state.
    //
    // We still check cancellation before starting.
    if (cancel.is_cancelled()) {
        return Result<void>::err(InstallerError::make(
            ErrorCode::INTERNAL_CANCELLED, "Cancelled",
            "Boot slot configuration cancelled before start"));
    }

    if (progress) {
        progress(ProgressInfo{20, "Reading current boot environment...", "", 0, 0, 0.0});
    }

    // Read current boot environment.
    auto env_result = boot_ctrl_->read_boot_env();
    if (!env_result.is_ok()) {
        return env_result;
    }

    BootEnv env = env_result.value();

    // Configure for A/B update:
    //   - Set next_slot to the target slot.
    //   - Set upgrade_pending = true.
    //   - Set boot_attempts_left = 3 (allow 3 tries before fallback).
    //   - Mark the target slot as not-yet-good.
    env.next_slot = ctx.target_slot;
    env.upgrade_pending = true;
    env.boot_attempts_left = 3;

    // Mark the target slot as not yet confirmed good.
    if (ctx.target_slot == "A") {
        env.slot_a_good = false;
    } else {
        env.slot_b_good = false;
    }

    if (progress) {
        progress(ProgressInfo{50, "Writing boot environment variables...", "", 0, 0, 0.0});
    }

    logger_->log(LogLevel::Info, step_id(), "boot_config" + std::string(": ") + "active_slot=" + env.active_slot +
                       " next_slot=" + env.next_slot +
                       " upgrade_pending=1" +
                       " boot_attempts_left=3", ctx.job_id);

    // Write the updated boot environment — this is the critical step.
    auto write_result = boot_ctrl_->write_boot_env(env);
    if (!write_result.is_ok()) {
        logger_->log(LogLevel::Error, step_id(), "boot_env_write_failed" + std::string(": ") + "Failed to write boot environment: " +
                           write_result.error(, ctx.job_id).code);
        return write_result;
    }

    if (progress) {
        progress(ProgressInfo{80, "Setting active boot slot...", "", 0, 0, 0.0});
    }

    // Also explicitly set the active slot through the boot control interface.
    auto slot_result = boot_ctrl_->set_active_slot(ctx.target_slot);
    if (!slot_result.is_ok()) {
        logger_->log(LogLevel::Warn, step_id(), "set_active_slot_failed" + std::string(": ") + "Failed to set active slot (may already be set): " +
                           slot_result.error(, ctx.job_id).code);
        // Non-fatal: the boot environment write above is the authoritative
        // mechanism. set_active_slot is a convenience wrapper.
    }

    if (progress) {
        progress(ProgressInfo{100, "Boot slot configuration complete",
                              "", 0, 0, 0.0});
    }

    logger_->log(LogLevel::Info, step_id(), "complete" + std::string(": ") + "Boot slot configured: next boot will use slot " +
                       ctx.target_slot, ctx.job_id);

    return Result<void>::ok();
}

Result<void> ConfigureBootSlotStep::verify(JobContext& ctx, ProgressCallback progress,
                                            CancellationToken& cancel) {
    if (progress) {
        progress(ProgressInfo{50, "Verifying boot environment...", "", 0, 0, 0.0});
    }

    if (cancel.is_cancelled()) {
        return Result<void>::err(InstallerError::make(
            ErrorCode::INTERNAL_CANCELLED, "Cancelled",
            "Boot slot verification cancelled"));
    }

    // Read back the environment and verify our changes took effect.
    auto env_result = boot_ctrl_->read_boot_env();
    if (!env_result.is_ok()) {
        return env_result;
    }

    const auto& env = env_result.value();

    if (env.next_slot != ctx.target_slot) {
        return Result<void>::err(InstallerError::make(
            ErrorCode::BOOT_ENV_WRITE_FAILED,
            "Boot Slot Verification Failed",
            "The next_slot was not set correctly",
            "expected=" + ctx.target_slot +
            " actual=" + env.next_slot));
    }

    if (!env.upgrade_pending) {
        return Result<void>::err(InstallerError::make(
            ErrorCode::BOOT_ENV_WRITE_FAILED,
            "Boot Slot Verification Failed",
            "upgrade_pending was not set",
            "expected=true actual=false"));
    }

    if (env.boot_attempts_left <= 0) {
        return Result<void>::err(InstallerError::make(
            ErrorCode::BOOT_ENV_WRITE_FAILED,
            "Boot Slot Verification Failed",
            "boot_attempts_left was not set correctly",
            "actual=" + std::to_string(env.boot_attempts_left)));
    }

    logger_->log(LogLevel::Info, step_id(), "verify" + std::string(": ") + "Boot environment verified: next_slot=" + env.next_slot +
                       " upgrade_pending=" + (env.upgrade_pending ? "true" : "false", ctx.job_id) +
                       " attempts=" + std::to_string(env.boot_attempts_left));

    return Result<void>::ok();
}

Result<void> ConfigureBootSlotStep::rollback(JobContext& ctx) {
    // Best-effort rollback: clear the upgrade_pending flag and restore
    // the original active slot.
    logger_->log(LogLevel::Warn, step_id(), "rollback" + std::string(": ") + "Attempting to restore boot environment", ctx.job_id);

    auto env_result = boot_ctrl_->read_boot_env();
    if (env_result.is_ok()) {
        BootEnv env = env_result.value();
        env.upgrade_pending = false;
        // Restore next_slot to the current active (no change on next boot).
        env.next_slot = env.active_slot;
        env.boot_attempts_left = 0;

        auto write_result = boot_ctrl_->write_boot_env(env);
        if (!write_result.is_ok()) {
            logger_->log(LogLevel::Error, step_id(), "rollback_failed" + std::string(": ") + "Failed to restore boot environment during rollback", ctx.job_id);
        }
    }

    return Result<void>::ok();
}

} // namespace installer
