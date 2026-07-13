/**
 * @file idevice_manager.h
 * @brief Block device enumeration and safety checks interface.
 *
 * Discovers block devices attached to the system and provides safety
 * predicates to prevent accidental writes to protected devices (the
 * installer media itself, the currently running system disk, etc.).
 *
 * @see Architecture Doc §6.1
 */

#ifndef INSTALLER_DEVICE_IDEVICE_MANAGER_H
#define INSTALLER_DEVICE_IDEVICE_MANAGER_H

#include <chrono>
#include <string>
#include <vector>

#include "installer/core/result.h"
#include "installer/core/types.h"

namespace installer {

/**
 * Block device enumeration and safety checks.
 *
 * Implementations query sysfs, udev, or equivalent kernel interfaces to
 * discover and inspect block devices. The safety predicates are the
 * primary guard against operator error — accidentally overwriting the
 * system disk or the installer media itself.
 */
class IDeviceManager {
public:
    virtual ~IDeviceManager() = default;

    /**
     * Enumerate all block devices currently visible to the kernel.
     *
     * Filters out loopback devices, ramdisks, zram, and other virtual
     * devices that are not valid installation targets. Results are
     * sorted by device path for deterministic ordering.
     *
     * @return A vector of BlockDeviceInfo structures, one per physical
     *         block device.
     */
    virtual std::vector<BlockDeviceInfo> scan_devices() = 0;

    /**
     * Determine whether a device is safe to use as an installation target.
     *
     * A device is considered unsafe if:
     * - It is the currently running system disk (hosts the rootfs).
     * - It is the installer media itself.
     * - Any of its partitions are currently mounted.
     * - It is marked read-only by the block layer.
     *
     * @param device_path Absolute kernel device node path
     *                    (e.g. /dev/mmcblk0, /dev/nvme0n1).
     * @return true if the device may safely be used as a target,
     *         or an InstallerError describing why it is unsafe
     *         (DEVICE_NOT_SAFE_TARGET).
     */
    virtual Result<bool> is_safe_target(const std::string& device_path) = 0;

    /**
     * Block until the specified device node appears in /dev, or the
     * timeout expires.
     *
     * Useful after a reboot or physical media insertion where the kernel
     * may take several seconds to enumerate the device and create the
     * device node.
     *
     * @param path    Device node path to wait for (e.g. /dev/sda).
     * @param timeout Maximum wall-clock time to wait.
     * @return Result<void> — ok when the device appears,
     *         DEVICE_HOTPLUG_TIMEOUT if the timeout expires first.
     */
    virtual Result<void> wait_for_device(const std::string& path,
                                         std::chrono::milliseconds timeout) = 0;

    /**
     * Determine whether a device hosts the currently running system.
     *
     * The implementation checks whether any partition on the device is
     * mounted as the root filesystem ("/").
     *
     * @param device_path Absolute path to the block device.
     * @return true if this device is the currently running system disk.
     */
    virtual bool is_system_disk(const std::string& device_path) = 0;

    /**
     * Determine whether a device is the installer media itself.
     *
     * The implementation checks for a well-known volume label, UUID,
     * or filesystem signature that identifies the installer media.
     *
     * @param device_path Absolute path to the block device.
     * @return true if this device is the installer media.
     */
    virtual bool is_installer_media(const std::string& device_path) = 0;
};

} // namespace installer

#endif // INSTALLER_DEVICE_IDEVICE_MANAGER_H
