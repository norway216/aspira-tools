#ifndef INSTALLER_STEPS_CREATE_FILESYSTEMS_H
#define INSTALLER_STEPS_CREATE_FILESYSTEMS_H

#include "installer/IJobStep.h"
#include "installer/filesystem/ifilesystem_manager.h"
#include "installer/partition/ipartition_manager.h"
#include "installer/log/ilogger.h"
#include <memory>

namespace installer {

class CreateFilesystemsStep : public IJobStep {
public:
    CreateFilesystemsStep(std::shared_ptr<IFilesystemManager> fs_mgr,
                          std::shared_ptr<IPartitionManager> part_mgr,
                          std::shared_ptr<ILogger> logger);

    std::string step_id() const override { return "create_filesystems"; }
    std::string description() const override { return "Creating filesystems on target partitions"; }
    int weight_percent() const override { return 5; }

    Result<void> prepare(JobContext& ctx) override;
    Result<void> execute(JobContext& ctx, ProgressCallback progress,
                         CancellationToken& cancel) override;
    Result<void> verify(JobContext& ctx, ProgressCallback progress,
                        CancellationToken& cancel) override;
    Result<void> rollback(JobContext& ctx) override;
    bool can_resume() const override { return false; }  // re-format if interrupted

private:
    std::shared_ptr<IFilesystemManager> fs_mgr_;
    std::shared_ptr<IPartitionManager> part_mgr_;
    std::shared_ptr<ILogger> logger_;
};

} // namespace installer

#endif // INSTALLER_STEPS_CREATE_FILESYSTEMS_H
