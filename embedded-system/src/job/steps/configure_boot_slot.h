#ifndef INSTALLER_STEPS_CONFIGURE_BOOT_SLOT_H
#define INSTALLER_STEPS_CONFIGURE_BOOT_SLOT_H

#include "installer/IJobStep.h"
#include "installer/IBootControl.h"
#include "installer/log/ilogger.h"
#include <memory>

namespace installer {

class ConfigureBootSlotStep : public IJobStep {
public:
    ConfigureBootSlotStep(std::shared_ptr<IBootControl> boot_ctrl,
                          std::shared_ptr<ILogger> logger);

    std::string step_id() const override { return "configure_boot_slot"; }
    std::string description() const override { return "Configuring boot slot and U-Boot environment"; }
    int weight_percent() const override { return 2; }

    Result<void> prepare(JobContext& ctx) override;
    Result<void> execute(JobContext& ctx, ProgressCallback progress,
                         CancellationToken& cancel) override;
    Result<void> verify(JobContext& ctx, ProgressCallback progress,
                        CancellationToken& cancel) override;
    Result<void> rollback(JobContext& ctx) override;
    bool can_resume() const override { return false; }  // always re-apply boot config

private:
    std::shared_ptr<IBootControl> boot_ctrl_;
    std::shared_ptr<ILogger> logger_;
};

} // namespace installer

#endif // INSTALLER_STEPS_CONFIGURE_BOOT_SLOT_H
