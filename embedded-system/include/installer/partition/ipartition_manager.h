/**
 * @file ipartition_manager.h
 * @brief GPT / MBR partition table management interface.
 *
 * Provides operations for reading, creating, and verifying partition
 * tables on block devices. Supports both GPT (preferred) and MBR
 * formats as described by the PartitionLayout structure.
 *
 * @see Architecture Doc §6.4
 */

#ifndef INSTALLER_PARTITION_IPARTITION_MANAGER_H
#define INSTALLER_PARTITION_IPARTITION_MANAGER_H

#include <string>
#include <vector>

#include "installer/core/result.h"
#include "installer/core/types.h"

namespace installer {

/**
 * GPT / MBR partition table management interface.
 *
 * Implementations use sgdisk, sfdisk, libfdisk, or direct system calls
 * to manipulate partition tables. All operations are idempotent where
 * possible — verifying a layout before creating it avoids unnecessary
 * writes.
 */
class IPartitionManager {
public:
    virtual ~IPartitionManager() = default;

    /**
     * Read the current partition table from a block device.
     *
     * Scans the on-disk GPT or MBR structures and returns a
     * PartitionLayout describing every partition found: name, size,
     * filesystem type, and label.
     *
     * @param device Absolute path to the block device
     *               (e.g. /dev/mmcblk0, not a partition).
     * @return The discovered PartitionLayout, or an error if the
     *         table is unreadable or the device does not exist.
     */
    virtual Result<PartitionLayout> read_partition_table(
        const std::string& device) = 0;

    /**
     * Create a new partition table on a block device.
     *
     * Destroys any existing partition table and writes a new one
     * according to @p layout. After returning, the kernel will have
     * re-read the partition table (via BLKRRPART or equivalent) so
     * that new device nodes are visible.
     *
     * @param device Absolute path to the block device.
     * @param layout Desired partition layout (table type, alignment,
     *               and list of partitions).
     * @return Result<void> — ok on success,
     *         PARTITION_CREATE_FAILED on error.
     */
    virtual Result<void> create_partition_table(
        const std::string& device,
        const PartitionLayout& layout) = 0;

    /**
     * Verify that the on-disk partition layout matches an expected layout.
     *
     * Compares partition count, names, sizes (with tolerance), types,
     * and labels. This is used as a safety check before proceeding with
     * an installation to confirm the target device is in the expected
     * state.
     *
     * @param device Absolute path to the block device.
     * @param layout Expected partition layout to compare against.
     * @return Result<void> — ok if the layouts match,
     *         PARTITION_LAYOUT_MISMATCH if they differ.
     */
    virtual Result<void> verify_partition_layout(
        const std::string& device,
        const PartitionLayout& layout) = 0;

    /**
     * Resolve a partition by its GPT label to a stable device path.
     *
     * Looks up the partition by PARTLABEL and returns the corresponding
     * /dev/disk/by-partlabel/<label> symlink path, which is stable across
     * reboots regardless of the kernel's device enumeration order.
     *
     * @param device Block device containing the partition.
     * @param label  GPT partition label (PARTLABEL) to look up.
     * @return The full path (e.g. "/dev/disk/by-partlabel/boot_a"),
     *         or PARTITION_NOT_FOUND if no partition has that label.
     */
    virtual Result<std::string> get_partition_by_label(
        const std::string& device,
        const std::string& label) = 0;
};

} // namespace installer

#endif // INSTALLER_PARTITION_IPARTITION_MANAGER_H
