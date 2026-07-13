/**
 * @file IPartitionManager.h
 * @brief Disk partitioning interface.
 */

#ifndef INSTALLER_IPARTITIONMANAGER_H
#define INSTALLER_IPARTITIONMANAGER_H

#include "installer/core/types.h"
#include "installer/core/result.h"
#include <string>
#include <vector>

namespace installer {

class IPartitionManager {
public:
    virtual ~IPartitionManager() = default;

    virtual Result<void> create_partition_table(const std::string& device,
                                                const std::string& table_type) = 0;

    virtual Result<void> create_partitions(const std::string& device,
                                           const PartitionLayout& layout) = 0;

    virtual Result<std::vector<std::string>> list_partitions(
        const std::string& device) = 0;

    virtual Result<PartitionSpec> get_partition_info(const std::string& device,
                                                     int part_number) = 0;

    virtual Result<void> wipe_partition_table(const std::string& device) = 0;
};

} // namespace installer

#endif // INSTALLER_IPARTITIONMANAGER_H
