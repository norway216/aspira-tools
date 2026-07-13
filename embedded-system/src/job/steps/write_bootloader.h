#ifndef INSTALLER_STEPS_WRITE_BOOTLOADER_H
#define INSTALLER_STEPS_WRITE_BOOTLOADER_H

#include "installer/IJobStep.h"
#include "installer/image/iimage_writer.h"
#include "installer/IPackageManager.h"
#include "installer/log/ilogger.h"
#include <memory>

namespace installer {

class WriteBootloaderStep : public IJobStep {
public:
    WriteBootloaderStep(std::shared_ptr<IImageWriter> image_writer,
                        std::shared_ptr<IPackageManager> pkg_mgr,
                        std::shared_ptr<ILogger> logger);

    std::string step_id() const override { return "write_bootloader"; }
    std::string description() const override { return "Writing bootloader to target device"; }
    int weight_percent() const override { return 3; }

    Result<void> prepare(JobContext& ctx) override;
    Result<void> execute(JobContext& ctx, ProgressCallback progress,
                         CancellationToken& cancel) override;
    Result<void> verify(JobContext& ctx, ProgressCallback progress,
                        CancellationToken& cancel) override;
    Result<void> rollback(JobContext& ctx) override;
    bool can_resume() const override { return false; }  // re-write if interrupted

private:
    std::shared_ptr<IImageWriter> image_writer_;
    std::shared_ptr<IPackageManager> pkg_mgr_;
    std::shared_ptr<ILogger> logger_;
};

} // namespace installer

#endif // INSTALLER_STEPS_WRITE_BOOTLOADER_H
