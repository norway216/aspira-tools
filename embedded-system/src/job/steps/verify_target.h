#ifndef INSTALLER_STEPS_VERIFY_TARGET_H
#define INSTALLER_STEPS_VERIFY_TARGET_H

#include "installer/IJobStep.h"
#include "installer/image/iimage_writer.h"
#include "installer/IPackageManager.h"
#include "installer/partition/ipartition_manager.h"
#include "installer/filesystem/ifilesystem_manager.h"
#include "installer/log/ilogger.h"
#include <memory>

namespace installer {

class VerifyTargetStep : public IJobStep {
public:
    VerifyTargetStep(std::shared_ptr<IImageWriter> image_writer,
                     std::shared_ptr<IPackageManager> pkg_mgr,
                     std::shared_ptr<IPartitionManager> part_mgr,
                     std::shared_ptr<IFilesystemManager> fs_mgr,
                     std::shared_ptr<ILogger> logger);

    std::string step_id() const override { return "verify_target"; }
    std::string description() const override { return "Verifying all written data on target"; }
    int weight_percent() const override { return 5; }

    Result<void> prepare(JobContext& ctx) override;
    Result<void> execute(JobContext& ctx, ProgressCallback progress,
                         CancellationToken& cancel) override;
    Result<void> verify(JobContext& ctx, ProgressCallback progress,
                        CancellationToken& cancel) override;
    Result<void> rollback(JobContext& ctx) override;
    bool can_resume() const override { return true; }

private:
    std::shared_ptr<IImageWriter> image_writer_;
    std::shared_ptr<IPackageManager> pkg_mgr_;
    std::shared_ptr<IPartitionManager> part_mgr_;
    std::shared_ptr<IFilesystemManager> fs_mgr_;
    std::shared_ptr<ILogger> logger_;
};

} // namespace installer

#endif // INSTALLER_STEPS_VERIFY_TARGET_H
