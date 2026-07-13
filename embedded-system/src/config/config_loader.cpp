/**
 * @file config_loader.cpp
 * @brief Implementation of YAML-based configuration loading.
 *
 * Uses yaml-cpp for all parsing.  Every fallible parse step returns
 * Result<T> so callers receive structured errors rather than raw
 * exceptions.
 */

#include "config_loader.h"

#include <yaml-cpp/yaml.h>

#include <algorithm>
#include <cctype>
#include <fstream>
#include <sstream>
#include <stdexcept>

namespace installer {

// =============================================================================
// Internal helpers
// =============================================================================

namespace {

/**
 * Read an entire file into a string.  Returns an error Result on failure.
 */
Result<std::string> read_file_string(const std::string& path) {
    std::ifstream ifs(path, std::ios::in | std::ios::binary);
    if (!ifs.is_open()) {
        return Result<std::string>::err(InstallerError::make(
            ErrorCode::INTERNAL_CONFIG_ERROR,
            "File Not Found",
            "Cannot open configuration file: " + path,
            "ifstream::open failed for '" + path + "'",
            false, false));
    }
    std::ostringstream oss;
    oss << ifs.rdbuf();
    if (ifs.bad()) {
        return Result<std::string>::err(InstallerError::make(
            ErrorCode::INTERNAL_CONFIG_ERROR,
            "Read Error",
            "Failed to read configuration file: " + path,
            "ifstream read error for '" + path + "'",
            false, false));
    }
    return Result<std::string>::ok(oss.str());
}

/**
 * Convert a string to lower-case in-place.
 */
void to_lower(std::string& s) {
    for (auto& ch : s) ch = static_cast<char>(::tolower(static_cast<unsigned char>(ch)));
}

/**
 * Build a parse error for a missing or invalid YAML key.
 */
InstallerError config_parse_error(const std::string& key,
                                  const std::string& context) {
    return InstallerError::make(
        ErrorCode::INTERNAL_CONFIG_ERROR,
        "Configuration Parse Error",
        "Invalid or missing value for '" + key + "' in " + context,
        "key='" + key + "' context='" + context + "'",
        false, false);
}

/**
 * Try to load a YAML file.  Returns an error Result on parse failure.
 */
Result<YAML::Node> load_yaml_file(const std::string& path) {
    auto content = read_file_string(path);
    if (content.is_err()) {
        return Result<YAML::Node>::err(content.take_error());
    }

    try {
        YAML::Node root = YAML::Load(content.value());
        if (root.IsNull()) {
            return Result<YAML::Node>::err(InstallerError::make(
                ErrorCode::INTERNAL_CONFIG_ERROR,
                "Empty Configuration",
                "Configuration file is empty: " + path,
                "YAML::Load returned null node for '" + path + "'",
                false, false));
        }
        return Result<YAML::Node>::ok(std::move(root));
    } catch (const YAML::Exception& e) {
        return Result<YAML::Node>::err(InstallerError::make(
            ErrorCode::INTERNAL_CONFIG_ERROR,
            "YAML Parse Error",
            "Failed to parse YAML: " + std::string(e.what()),
            std::string("YAML::Exception in '") + path + "': " + e.what(),
            false, false));
    }
}

/**
 * Safely read an optional string field from a YAML map node.
 */
std::string opt_string(const YAML::Node& node, const std::string& key,
                       const std::string& default_val = "") {
    if (!node.IsMap()) return default_val;
    auto child = node[key];
    if (!child || child.IsNull()) return default_val;
    return child.as<std::string>(default_val);
}

/**
 * Safely read an optional bool field from a YAML map node.
 */
bool opt_bool(const YAML::Node& node, const std::string& key, bool default_val) {
    if (!node.IsMap()) return default_val;
    auto child = node[key];
    if (!child || child.IsNull()) return default_val;
    try {
        return child.as<bool>();
    } catch (...) {
        return default_val;
    }
}

/**
 * Safely read an optional int field from a YAML map node.
 */
int opt_int(const YAML::Node& node, const std::string& key, int default_val) {
    if (!node.IsMap()) return default_val;
    auto child = node[key];
    if (!child || child.IsNull()) return default_val;
    try {
        return child.as<int>();
    } catch (...) {
        return default_val;
    }
}

/**
 * Safely read an optional uint64_t field from a YAML map node.
 */
uint64_t opt_uint64(const YAML::Node& node, const std::string& key,
                    uint64_t default_val) {
    if (!node.IsMap()) return default_val;
    auto child = node[key];
    if (!child || child.IsNull()) return default_val;
    try {
        return child.as<uint64_t>();
    } catch (...) {
        return default_val;
    }
}

/**
 * Read a vector of strings from a YAML sequence.
 */
std::vector<std::string> read_string_list(const YAML::Node& node,
                                          const std::string& key) {
    std::vector<std::string> result;
    if (!node.IsMap()) return result;
    auto child = node[key];
    if (!child || !child.IsSequence()) return result;
    for (const auto& item : child) {
        result.push_back(item.as<std::string>());
    }
    return result;
}

} // anonymous namespace

// =============================================================================
// ConfigLoader::parse_filesystem_type
// =============================================================================

Result<FilesystemType> ConfigLoader::parse_filesystem_type(const std::string& str) {
    std::string lower = str;
    to_lower(lower);

    if (lower == "vfat" || lower == "fat32" || lower == "fat16") {
        return Result<FilesystemType>::ok(FilesystemType::VFAT);
    }
    if (lower == "ext4") {
        return Result<FilesystemType>::ok(FilesystemType::EXT4);
    }
    if (lower == "squashfs") {
        return Result<FilesystemType>::ok(FilesystemType::SquashFS);
    }
    if (lower == "raw" || lower == "none") {
        return Result<FilesystemType>::ok(FilesystemType::Raw);
    }
    if (lower == "unknown") {
        return Result<FilesystemType>::ok(FilesystemType::Unknown);
    }

    return Result<FilesystemType>::err(InstallerError::make(
        ErrorCode::INTERNAL_CONFIG_ERROR,
        "Unknown Filesystem Type",
        "Unrecognized filesystem type '" + str +
            "'. Expected: vfat, ext4, squashfs, or raw.",
        "Unknown filesystem type string: '" + str + "'",
        false, false));
}

// =============================================================================
// ConfigLoader::parse_partition_spec
// =============================================================================

Result<PartitionSpec> ConfigLoader::parse_partition_spec(const YAML::Node& node) {
    if (!node.IsMap()) {
        return Result<PartitionSpec>::err(config_parse_error(
            "<sequence element>", "partition_spec"));
    }

    PartitionSpec spec;

    // name (required)
    auto name_node = node["name"];
    if (!name_node || name_node.IsNull()) {
        return Result<PartitionSpec>::err(config_parse_error(
            "name", "partition_spec"));
    }
    spec.name = name_node.as<std::string>();

    // size_mib (optional, default 0 = fill remaining)
    spec.size_mib = opt_uint64(node, "size_mib", 0);

    // filesystem (optional, default ext4)
    std::string fs_str = opt_string(node, "filesystem", "ext4");
    auto fs_result = parse_filesystem_type(fs_str);
    if (fs_result.is_err()) {
        return Result<PartitionSpec>::err(fs_result.take_error());
    }
    spec.filesystem = fs_result.value();

    // label (optional)
    spec.label = opt_string(node, "label", "");

    return Result<PartitionSpec>::ok(std::move(spec));
}

// =============================================================================
// ConfigLoader::parse_partition_layout
// =============================================================================

Result<PartitionLayout> ConfigLoader::parse_partition_layout(
    const YAML::Node& node) {
    if (!node.IsMap()) {
        return Result<PartitionLayout>::err(config_parse_error(
            "<layout>", "partition_layout"));
    }

    PartitionLayout layout;

    // name
    auto name_node = node["name"];
    if (!name_node || name_node.IsNull()) {
        return Result<PartitionLayout>::err(config_parse_error(
            "name", "partition_layout"));
    }
    layout.name = name_node.as<std::string>();

    // table_type (default "gpt")
    layout.table_type = opt_string(node, "table_type", "gpt");
    to_lower(layout.table_type);
    if (layout.table_type != "gpt" && layout.table_type != "mbr") {
        return Result<PartitionLayout>::err(InstallerError::make(
            ErrorCode::INTERNAL_CONFIG_ERROR,
            "Invalid Partition Table Type",
            "Table type '" + layout.table_type +
                "' is not supported. Use 'gpt' or 'mbr'.",
            "layout '" + layout.name + "' has unknown table_type '" +
                layout.table_type + "'",
            false, false));
    }

    // alignment_mib (default 4)
    layout.alignment_mib = opt_uint64(node, "alignment_mib", 4);

    // partitions (required)
    auto parts_node = node["partitions"];
    if (!parts_node || !parts_node.IsSequence()) {
        return Result<PartitionLayout>::err(config_parse_error(
            "partitions", "layout '" + layout.name + "'"));
    }

    for (const auto& pn : parts_node) {
        auto spec_result = parse_partition_spec(pn);
        if (spec_result.is_err()) {
            return Result<PartitionLayout>::err(spec_result.take_error());
        }
        layout.partitions.push_back(spec_result.take_value());
    }

    return Result<PartitionLayout>::ok(std::move(layout));
}

// =============================================================================
// ConfigLoader::load
// =============================================================================

Result<InstallerConfig> ConfigLoader::load(const std::string& config_path) {
    auto yaml_result = load_yaml_file(config_path);
    if (yaml_result.is_err()) {
        return Result<InstallerConfig>::err(yaml_result.take_error());
    }

    const auto& root = yaml_result.value();
    if (!root.IsMap()) {
        return Result<InstallerConfig>::err(InstallerError::make(
            ErrorCode::INTERNAL_CONFIG_ERROR,
            "Invalid Configuration Format",
            "Configuration file must be a YAML map.",
            "'" + config_path + "' root node is not a map.",
            false, false));
    }

    InstallerConfig cfg;

    // ---- Simple scalar fields ----
    cfg.version   = opt_string(root, "version", "1.0");
    cfg.log_dir   = opt_string(root, "log_dir", "/var/log/installer");
    cfg.journal_dir = opt_string(root, "journal_dir", "/var/lib/installer/journal");
    cfg.require_external_power = opt_bool(root, "require_external_power", true);
    cfg.minimum_battery_percent = opt_int(root, "minimum_battery_percent", 20);
    cfg.full_verify_after_write = opt_bool(root, "full_verify_after_write", true);
    cfg.target_by = opt_string(root, "target_by", "partlabel");

    // ---- Device candidates ----
    cfg.device_candidates = read_string_list(root, "device_candidates");

    // ---- Partition layouts ----
    auto layouts_node = root["partition_layouts"];
    if (layouts_node && layouts_node.IsMap()) {
        for (const auto& kv : layouts_node) {
            std::string layout_name = kv.first.as<std::string>();
            auto layout_result = parse_partition_layout(kv.second);
            if (layout_result.is_err()) {
                return Result<InstallerConfig>::err(layout_result.take_error());
            }
            cfg.partition_layouts[layout_name] = layout_result.take_value();
        }
    }

    return Result<InstallerConfig>::ok(std::move(cfg));
}

// =============================================================================
// ConfigLoader::load_partition_layout
// =============================================================================

Result<PartitionLayout> ConfigLoader::load_partition_layout(
    const std::string& yaml_path,
    const std::string& layout_name) {
    auto yaml_result = load_yaml_file(yaml_path);
    if (yaml_result.is_err()) {
        return Result<PartitionLayout>::err(yaml_result.take_error());
    }

    const auto& root = yaml_result.value();

    if (!root.IsMap()) {
        return Result<PartitionLayout>::err(InstallerError::make(
            ErrorCode::INTERNAL_CONFIG_ERROR,
            "Invalid Layout File",
            "Partition layout file must be a YAML map.",
            "'" + yaml_path + "' root node is not a map.",
            false, false));
    }

    // Try the layout as a named child under the root map
    auto layout_node = root[layout_name];
    if (layout_node && !layout_node.IsNull()) {
        return parse_partition_layout(layout_node);
    }

    // Maybe the root IS the layout definition (single-layout file)
    if (root["name"] && root["name"].as<std::string>() == layout_name) {
        return parse_partition_layout(root);
    }

    return Result<PartitionLayout>::err(InstallerError::make(
        ErrorCode::INTERNAL_CONFIG_ERROR,
        "Layout Not Found",
        "Partition layout '" + layout_name + "' not found in " + yaml_path,
        "layout_name='" + layout_name + "' not in '" + yaml_path + "'",
        false, false));
}

// =============================================================================
// ConfigLoader::load_hardware_profile
// =============================================================================

Result<std::string> ConfigLoader::load_hardware_profile(
    const std::string& yaml_path,
    const std::string& profile_id) {
    auto yaml_result = load_yaml_file(yaml_path);
    if (yaml_result.is_err()) {
        return Result<std::string>::err(yaml_result.take_error());
    }

    const auto& root = yaml_result.value();
    if (!root.IsMap()) {
        return Result<std::string>::err(InstallerError::make(
            ErrorCode::INTERNAL_CONFIG_ERROR,
            "Invalid Profile File",
            "Hardware profile file must be a YAML map.",
            "'" + yaml_path + "' root node is not a map.",
            false, false));
    }

    auto profile_node = root[profile_id];
    if (!profile_node || profile_node.IsNull()) {
        return Result<std::string>::err(InstallerError::make(
            ErrorCode::INTERNAL_CONFIG_ERROR,
            "Profile Not Found",
            "Hardware profile '" + profile_id + "' not found in " + yaml_path,
            "profile_id='" + profile_id + "' not in '" + yaml_path + "'",
            false, false));
    }

    // If the profile node is a scalar, return it directly.
    // If it's a map, serialize it back to a compact YAML string.
    if (profile_node.IsScalar()) {
        return Result<std::string>::ok(profile_node.as<std::string>());
    }

    // Map or sequence — re-emit as YAML
    try {
        YAML::Emitter emitter;
        emitter << profile_node;
        return Result<std::string>::ok(std::string(emitter.c_str()));
    } catch (const YAML::Exception& e) {
        return Result<std::string>::err(InstallerError::make(
            ErrorCode::INTERNAL_CONFIG_ERROR,
            "Profile Serialization Error",
            "Failed to serialize hardware profile: " + std::string(e.what()),
            e.what(),
            false, false));
    }
}

} // namespace installer
