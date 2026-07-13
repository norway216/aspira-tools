#ifndef INSTALLER_STEPS_PREPARE_PARTITIONS_H
#define INSTALLER_STEPS_PREPARE_PARTITIONS_H

#include "installer/IJobStep.h"
#include "installer/partition/ipartition_manager.h"
#include "installer/IDeviceManager.h"
#include "installer/log/ilogger.h"
#include <memory>

namespace installer {

class PreparePartitionsStep : public IJobStep {
public:
    PreparePartitionsStep(std::shared_ptr<IPartitionManager> part_mgr,
                          std::shared_ptr<IDeviceManager> dev_mgr,
                          std::shared_ptr<ILogger> logger);

    std::string step_id() const override { return "prepare_partitions"; }
    std::string description() const override { return "Creating partition table"; }
    int weight_percent() const override { return 10; }

    Result<void> prepare(JobContext& ctx) override;
    Result<void> execute(JobContext& ctx, ProgressCallback progress,
                         CancellationToken& cancel) override;
    Result<void> verify(JobContext& ctx, ProgressCallback progress,
                        CancellationToken& cancel) override;
    Result<void> rollback(JobContext& ctx) override;
    bool can_resume() const override { return false; }  // re-create partitions if interrupted

private:
    std::shared_ptr<IPartitionManager> part_mgr_;
    std::shared_ptr<IDeviceManager> dev_mgr_;
    std::shared_ptr<ILogger> logger_;
};

} // namespace installer

#endif // INSTALLER_STEPS_PREPARE_PARTITIONS_H
