#ifndef INSTALLER_STEPS_VERIFY_SIGNATURE_H
#define INSTALLER_STEPS_VERIFY_SIGNATURE_H

#include "installer/IJobStep.h"
#include "installer/security/isecurity_manager.h"
#include "installer/IPackageManager.h"
#include "installer/log/ilogger.h"
#include <memory>

namespace installer {

class VerifySignatureStep : public IJobStep {
public:
    VerifySignatureStep(std::shared_ptr<ISecurityManager> sec_mgr,
                        std::shared_ptr<IPackageManager> pkg_mgr,
                        std::shared_ptr<ILogger> logger);

    std::string step_id() const override { return "verify_signature"; }
    std::string description() const override { return "Verifying package signature"; }
    int weight_percent() const override { return 10; }

    Result<void> prepare(JobContext& ctx) override;
    Result<void> execute(JobContext& ctx, ProgressCallback progress,
                         CancellationToken& cancel) override;
    Result<void> verify(JobContext& ctx, ProgressCallback progress,
                        CancellationToken& cancel) override;
    Result<void> rollback(JobContext& ctx) override;
    bool can_resume() const override { return true; }

private:
    std::shared_ptr<ISecurityManager> sec_mgr_;
    std::shared_ptr<IPackageManager> pkg_mgr_;
    std::shared_ptr<ILogger> logger_;
};

} // namespace installer

#endif // INSTALLER_STEPS_VERIFY_SIGNATURE_H
