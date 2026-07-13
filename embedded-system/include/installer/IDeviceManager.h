/**
 * @file IDeviceManager.h
 * @brief Device discovery and management interface.
 */

#ifndef INSTALLER_IDEVICEMANAGER_H
#define INSTALLER_IDEVICEMANAGER_H

#include "installer/core/types.h"
#include "installer/core/result.h"
#include <vector>
#include <string>

namespace installer {

class IDeviceManager {
public:
    virtual ~IDeviceManager() = default;

    virtual std::vector<BlockDeviceInfo> scan() = 0;
    virtual bool is_safe_target(const std::string& device_path) = 0;
    virtual Result<BlockDeviceInfo> get_device_info(const std::string& device_path) = 0;
    virtual Result<void> wait_for_device(const std::string& device_path, int timeout_ms) = 0;
};

} // namespace installer

#endif // INSTALLER_IDEVICEMANAGER_H
