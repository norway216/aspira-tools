/**
 * @file config_loader.h
 * @brief YAML-based configuration loader for the installer.
 *
 * Parses the main installer configuration file (§23 of the architecture doc),
 * partition layout definitions, and hardware profile overrides.
 * All parsing is done via yaml-cpp.
 */

#ifndef INSTALLER_CONFIG_CONFIG_LOADER_H
#define INSTALLER_CONFIG_CONFIG_LOADER_H

#include "installer/core/result.h"
#include "installer/core/types.h"
#include <map>
#include <string>
#include <vector>

// Forward-declare yaml-cpp types so this header is self-contained.
namespace YAML {
class Node;
}

namespace installer {

// =============================================================================
// InstallerConfig — top-level configuration parsed from installer.yaml
// =============================================================================

struct InstallerConfig {
    std::string version;
    std::string log_dir;
    std::string journal_dir;
    bool require_external_power = true;
    int minimum_battery_percent = 20;
    bool full_verify_after_write = true;
    std::string target_by;                  // "partlabel", "partuuid", or "path"
    std::vector<std::string> device_candidates;
    std::map<std::string, PartitionLayout> partition_layouts;
};

// =============================================================================
// ConfigLoader — static parser methods
// =============================================================================

class ConfigLoader {
public:
    /**
     * Load the main installer configuration from a YAML file.
     *
     * @param config_path  Path to the YAML configuration file.
     * @return             Populated InstallerConfig or an error.
     */
    static Result<InstallerConfig> load(const std::string& config_path);

    /**
     * Load a specific named partition layout from a YAML file.
     *
     * The YAML file is expected to contain a top-level map where each key
     * is a layout name and the value is a PartitionLayout definition.
     *
     * @param yaml_path    Path to the partition layouts YAML file.
     * @param layout_name  Name of the layout to extract (e.g. "ab_standard_v1").
     * @return             The requested PartitionLayout or an error.
     */
    static Result<PartitionLayout> load_partition_layout(
        const std::string& yaml_path,
        const std::string& layout_name);

    /**
     * Load a hardware profile string from a YAML file.
     *
     * Hardware profiles are device-specific overrides (device tree, kernel
     * cmdline additions, etc.) keyed by profile ID.
     *
     * @param yaml_path   Path to the hardware profiles YAML file.
     * @param profile_id  Profile identifier (e.g. "rpi4", "jetson-nano").
     * @return            The raw profile content as a string, or an error.
     */
    static Result<std::string> load_hardware_profile(
        const std::string& yaml_path,
        const std::string& profile_id);

private:
    /** Parse a FilesystemType from a string (case-insensitive). */
    static Result<FilesystemType> parse_filesystem_type(const std::string& str);

    /** Parse a single PartitionSpec from a YAML node. */
    static Result<PartitionSpec> parse_partition_spec(const YAML::Node& node);

    /** Parse a full PartitionLayout from a YAML node. */
    static Result<PartitionLayout> parse_partition_layout(const YAML::Node& node);
};

} // namespace installer

#endif // INSTALLER_CONFIG_CONFIG_LOADER_H
