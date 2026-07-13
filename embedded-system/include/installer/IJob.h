/**
 * @file IJob.h
 * @brief High-level job abstraction (install, backup, restore, etc.).
 */

#ifndef INSTALLER_IJOB_H
#define INSTALLER_IJOB_H

#include "installer/core/types.h"
#include "installer/core/result.h"
#include <string>

namespace installer {

class IJob {
public:
    virtual ~IJob() = default;

    virtual std::string job_id() const = 0;
    virtual JobType type() const = 0;
    virtual JobState state() const = 0;

    virtual Result<void> start(CancellationToken& cancel) = 0;
    virtual void request_cancel() = 0;

    virtual bool can_resume() const = 0;
    virtual Result<void> resume(CancellationToken& cancel) = 0;

    virtual int progress_percent() const = 0;
    virtual std::string current_step_name() const = 0;

    virtual void set_progress_callback(ProgressCallback cb) = 0;
};

} // namespace installer

#endif // INSTALLER_IJOB_H
