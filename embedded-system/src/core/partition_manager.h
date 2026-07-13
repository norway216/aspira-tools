/**
 * @file partition_manager.h
 * @brief PartitionManager — sgdisk-based GPT partition management for the
 *        embedded Linux installer.
 *
 * Implements IPartitionManager using sgdisk invoked through the
 * IProcessRunner abstraction. All operations are thread-safe and return
 * Result<T> for explicit error propagation.
 */

#ifndef INSTALLER_CORE_PARTITION_MANAGER_H
#define INSTALLER_CORE_PARTITION_MANAGER_H

#include "installer/partition/ipartition_manager.h"
#include "installer/core/types.h"
#include "installer/core/result.h"
#include <string>
#include <vector>
#include <mutex>

namespace installer {

class IProcessRunner;

class PartitionManager : public IPartitionManager {
public:
    explicit PartitionManager(IProcessRunner* proc_runner);
    ~PartitionManager() override;

    // ---- IPartitionManager interface ----------------------------------------

    Result<void> create_partition_table(const std::string& device_path,
                                         const PartitionLayout& layout) override;
    Result<std::vector<PartitionSpec>> read_partition_table(const std::string& device_path) override;
    Result<bool> verify_partition_layout(const std::string& device_path,
                                          const PartitionLayout& expected) override;
    Result<std::string> get_partition_by_label(const std::string& label) override;
    Result<void> wait_for_partitions(const std::string& device_path, int timeout_sec = 10) override;

private:
    // ---- GPT type code helpers ----------------------------------------------

    /**
     * Convert a FilesystemType enum value to the corresponding GPT
     * partition type GUID shorthand used by sgdisk.
     *
     *   EXT4     -> "8300"  (Linux filesystem)
     *   VFAT     -> "EF00"  (EFI System Partition)
     *   SquashFS -> "8300"
     *   Raw      -> "8300"
     *   Unknown  -> "8300"
     */
    static std::string fs_type_to_sgdisk_code(FilesystemType fs_type);

    /**
     * Reverse mapping: convert a 4-character sgdisk type code to
     * the best-matching FilesystemType.
     */
    static FilesystemType sgdisk_code_to_fs_type(const std::string& code);

    // ---- Parsing ------------------------------------------------------------

    /**
     * Parse the stdout of `sgdisk --print` into a vector of PartitionSpec.
     *
     * Handles the standard sgdisk tabular output format.  If the output
     * cannot be parsed (e.g. unknown header format), an error is returned.
     */
    Result<std::vector<PartitionSpec>> parse_sgdisk_output(const std::string& output);

    // ---- Device naming ------------------------------------------------------

    /**
     * Return the suffix that must be inserted between a base block-device
     * path and the partition number.
     *
     *   /dev/mmcblk0  -> "p"   (partitions: /dev/mmcblk0p1)
     *   /dev/nvme0n1  -> "p"   (partitions: /dev/nvme0n1p1)
     *   /dev/sda      -> ""    (partitions: /dev/sda1)
     *   /dev/vda      -> ""    (partitions: /dev/vda1)
     */
    static std::string partition_suffix(const std::string& device_path);

    // ---- Fallback block-device enumeration ----------------------------------

    /**
     * Enumerate base block devices from /sys/block.
     *
     * Returns device paths such as "/dev/sda", "/dev/mmcblk0", etc.
     * Loop devices, RAM disks, and partitions are filtered out.
     */
    static std::vector<std::string> enumerate_block_devices();

    // ---- Size conversion ----------------------------------------------------

    /**
     * Parse a human-readable size string from sgdisk output
     * (e.g. "256.0 MiB", "7.0 GiB", "512.0 KiB") and return
     * the value in MiB.
     */
    static uint64_t parse_size_to_mib(const std::string& size_str,
                                       const std::string& unit_str);

    // ---- Data members -------------------------------------------------------

    IProcessRunner* proc_runner_;    // non-owning, injected
    mutable std::mutex mutex_;       // serialises external tool invocations
};

} // namespace installer

#endif // INSTALLER_CORE_PARTITION_MANAGER_H
