/**
 * @file detect_hardware.h
 * @brief Hardware detection step — identifies the target device and platform.
 */

#ifndef INSTALLER_STEPS_DETECT_HARDWARE_H
#define INSTALLER_STEPS_DETECT_HARDWARE_H

#include "installer/IJobStep.h"
#include "installer/IDeviceManager.h"
#include "installer/log/ilogger.h"
#include <memory>

namespace installer {

class DetectHardwareStep : public IJobStep {
public:
    DetectHardwareStep(std::shared_ptr<IDeviceManager> dev_mgr,
                       std::shared_ptr<ILogger> logger);

    std::string step_id() const override { return "detect_hardware"; }
    std::string description() const override { return "Detecting hardware platform"; }
    int weight_percent() const override { return 5; }

    Result<void> prepare(JobContext& ctx) override;
    Result<void> execute(JobContext& ctx, ProgressCallback progress,
                         CancellationToken& cancel) override;
    Result<void> verify(JobContext& ctx, ProgressCallback progress,
                        CancellationToken& cancel) override;
    Result<void> rollback(JobContext& ctx) override;
    bool can_resume() const override { return false; }  // always re-detect

private:
    std::shared_ptr<IDeviceManager> dev_mgr_;
    std::shared_ptr<ILogger> logger_;
};

} // namespace installer

#endif // INSTALLER_STEPS_DETECT_HARDWARE_H
