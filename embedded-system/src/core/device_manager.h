/**
 * @file device_manager.h
 * @brief DeviceManager — sysfs-based block device discovery for the embedded
 *        Linux installer.
 *
 * Implements IDeviceManager without depending on libudev. All device
 * enumeration is performed by reading /sys/block and /proc/mounts directly.
 */

#ifndef INSTALLER_CORE_DEVICE_MANAGER_H
#define INSTALLER_CORE_DEVICE_MANAGER_H

#include "installer/IDeviceManager.h"
#include "installer/core/result.h"
#include "installer/core/types.h"

#include <atomic>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace installer {

class DeviceManager : public IDeviceManager {
public:
    DeviceManager();
    ~DeviceManager() override;

    // ---- IDeviceManager interface ------------------------------------------

    std::vector<BlockDeviceInfo> scan() override;
    bool is_safe_target(const std::string& device_path) override;
    Result<BlockDeviceInfo> get_device_info(const std::string& device_path) override;
    Result<void> wait_for_device(const std::string& device_path, int timeout_ms) override;

    // ---- Extended API ------------------------------------------------------

    /**
     * Scan all block devices via /sys/block.
     * @return Result containing the device vector, or an error on critical
     *         failure (e.g. unable to open /sys/block).
     */
    Result<std::vector<BlockDeviceInfo>> scan_devices();

    /**
     * Return the device path of the system disk (the device mounted at "/").
     * Returns an empty string if it cannot be determined.
     */
    std::string get_system_disk();

    /**
     * Set the device (or partition) that hosts the installer media.
     * During scanning, disks that contain this device/partition will be
     * flagged with is_installer_media = true.
     */
    void set_installer_media_device(const std::string& device_path);

    /**
     * Hotplug callback signature.
     * @param info   Device information block.
     * @param added  true when the device appeared, false when it disappeared.
     */
    using HotplugCallback = std::function<void(const BlockDeviceInfo&, bool added)>;

    /**
     * Start a background thread that polls /sys/block every second and
     * invokes @p callback for every device insertion or removal.
     *
     * @return A positive integer handle on success, or -1 if a monitor is
     *         already active.
     */
    int start_hotplug_monitor(HotplugCallback callback);

    /**
     * Stop the background hotplug-monitor thread identified by @p handle.
     * @return Result<void>::ok() on success, or an error if the handle is
     *         unknown or the thread could not be joined.
     */
    Result<void> stop_hotplug_monitor(int handle);

private:
    // ---- Internal builders -------------------------------------------------

    /**
     * Build a BlockDeviceInfo for a single kernel device name
     * (e.g. "sda", "mmcblk0", "nvme0n1").  Returns a default-initialized
     * struct when any non-critical sysfs file is unreadable.
     */
    BlockDeviceInfo build_device_info(const std::string& name);

    /**
     * Check whether @p device_path is (or contains) the installer media
     * device previously set via set_installer_media_device().
     */
    bool is_installer_media(const std::string& device_path) const;

    // ---- Hotplug internals -------------------------------------------------

    void monitor_loop();

    // ---- State -------------------------------------------------------------

    mutable std::mutex       mutex_;
    std::atomic<bool>        monitoring_active_{false};
    std::thread              monitor_thread_;
    HotplugCallback          hotplug_callback_;
    std::vector<std::string> previous_device_names_;   // guarded by mutex_
    int                      monitor_handle_counter_{0};
    std::string              installer_media_device_;   // set via set_installer_media_device()
};

} // namespace installer

#endif // INSTALLER_CORE_DEVICE_MANAGER_H
