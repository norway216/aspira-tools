#ifndef INSTALLER_STEPS_CHECK_COMPATIBILITY_H
#define INSTALLER_STEPS_CHECK_COMPATIBILITY_H

#include "installer/IJobStep.h"
#include "installer/IPackageManager.h"
#include "installer/IDeviceManager.h"
#include "installer/log/ilogger.h"
#include <memory>

namespace installer {

class CheckCompatibilityStep : public IJobStep {
public:
    CheckCompatibilityStep(std::shared_ptr<IPackageManager> pkg_mgr,
                           std::shared_ptr<IDeviceManager> dev_mgr,
                           std::shared_ptr<ILogger> logger);

    std::string step_id() const override { return "check_compatibility"; }
    std::string description() const override { return "Checking hardware and software compatibility"; }
    int weight_percent() const override { return 5; }

    Result<void> prepare(JobContext& ctx) override;
    Result<void> execute(JobContext& ctx, ProgressCallback progress,
                         CancellationToken& cancel) override;
    Result<void> verify(JobContext& ctx, ProgressCallback progress,
                        CancellationToken& cancel) override;
    Result<void> rollback(JobContext& ctx) override;
    bool can_resume() const override { return true; }

private:
    std::shared_ptr<IPackageManager> pkg_mgr_;
    std::shared_ptr<IDeviceManager> dev_mgr_;
    std::shared_ptr<ILogger> logger_;
};

} // namespace installer

#endif // INSTALLER_STEPS_CHECK_COMPATIBILITY_H
