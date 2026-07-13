/**
 * @file IBootControl.h
 * @brief A/B boot slot control interface.
 */

#ifndef INSTALLER_IBOOTCONTROL_H
#define INSTALLER_IBOOTCONTROL_H

#include "installer/core/types.h"
#include "installer/core/result.h"
#include <string>

namespace installer {

class IBootControl {
public:
    virtual ~IBootControl() = default;

    virtual Result<BootEnv> read_boot_env() = 0;

    virtual Result<void> write_boot_env(const BootEnv& env) = 0;

    virtual Result<std::string> get_current_slot() = 0;

    virtual Result<std::string> get_inactive_slot() = 0;

    virtual Result<void> set_active_slot(const std::string& slot) = 0;

    virtual Result<void> set_next_slot(const std::string& slot) = 0;

    virtual Result<void> mark_boot_successful() = 0;

    virtual Result<void> mark_slot_good(const std::string& slot) = 0;
};

} // namespace installer

#endif // INSTALLER_IBOOTCONTROL_H
