/**
 * @file hardware_profile.cpp
 * @brief Implementation of HardwareProfileManager: config parsing, hardware
 *        matching, and system detection.
 *
 * Includes a hand-rolled YAML-like parser that handles the structured subset
 * of YAML used by the hardware profile config file.  This avoids pulling in a
 * full YAML library for a single config file whose format is tightly
 * controlled.
 */

// <atomic> must precede hardware_profile.h — types.h uses std::atomic but
// does not include the header itself (pre-existing issue).
#include <atomic>

#include "hardware_profile.h"

#include <algorithm>
#include <cctype>
#include <cstring>
#include <dirent.h>
#include <fstream>
#include <mutex>
#include <sstream>
#include <sys/utsname.h>
#include <unistd.h>

namespace installer {

// ============================================================================
// Anonymous namespace — YAML parser internals
// ============================================================================

namespace {

// ---------------------------------------------------------------------------
// YAMLValue tree node type
// ---------------------------------------------------------------------------

struct YAMLValue {
    enum Type { Null, String, Int, Bool, List, Map };

    Type type = Null;
    std::string str_val;
    int64_t int_val = 0;
    bool bool_val = false;
    std::vector<YAMLValue> list_val;
    std::map<std::string, YAMLValue> map_val;
};

using YAMLMap = std::map<std::string, YAMLValue>;

// ---------------------------------------------------------------------------
// Generic utilities
// ---------------------------------------------------------------------------

std::string ltrim(const std::string& s) {
    size_t start = 0;
    while (start < s.size() && (s[start] == ' ' || s[start] == '\t')) {
        ++start;
    }
    return s.substr(start);
}

std::string rtrim(const std::string& s) {
    size_t end = s.size();
    while (end > 0 && (s[end - 1] == ' ' || s[end - 1] == '\t' || s[end - 1] == '\r')) {
        --end;
    }
    return s.substr(0, end);
}

std::string trim_str(const std::string& s) {
    return ltrim(rtrim(s));
}

bool starts_with(const std::string& s, const std::string& prefix) {
    return s.size() >= prefix.size() && s.compare(0, prefix.size(), prefix) == 0;
}

std::vector<std::string> split_lines(const std::string& text) {
    std::vector<std::string> lines;
    std::istringstream stream(text);
    std::string line;
    while (std::getline(stream, line)) {
        // Normalise CRLF
        if (!line.empty() && line.back() == '\r') {
            line.pop_back();
        }
        lines.push_back(std::move(line));
    }
    return lines;
}

// ---------------------------------------------------------------------------
// YAML tokenizer
// ---------------------------------------------------------------------------

struct LineToken {
    int indent;             // number of 2-space indentation levels
    std::string content;    // trimmed content (without leading spaces)
};

/**
 * Convert raw YAML text into indentation-aware tokens.
 *
 * Rules:
 *  - Empty lines and comment-only lines (first non-space char is '#') are
 *    skipped.
 *  - Indentation is measured in 2-space increments.  Odd indentation is
 *    rounded down.
 *  - Trailing inline comments (" #...") are stripped unless inside double
 *    quotes.
 */
std::vector<LineToken> tokenize_yaml(const std::string& yaml) {
    std::vector<LineToken> tokens;
    auto raw_lines = split_lines(yaml);

    for (auto& raw : raw_lines) {
        // Count leading spaces
        size_t space_count = 0;
        while (space_count < raw.size() && raw[space_count] == ' ') {
            ++space_count;
        }

        std::string content = raw.substr(space_count);

        // Skip empty lines and comments
        if (content.empty() || content[0] == '#') {
            continue;
        }

        // Strip trailing inline comment (outside of double quotes)
        // Look for " #" that is not inside quotes
        bool in_quotes = false;
        for (size_t i = 0; i + 1 < content.size(); ++i) {
            if (content[i] == '"' && (i == 0 || content[i - 1] != '\\')) {
                in_quotes = !in_quotes;
            }
            if (!in_quotes && content[i] == ' ' && content[i + 1] == '#') {
                content = trim_str(content.substr(0, i));
                break;
            }
        }

        int indent = static_cast<int>(space_count / 2);
        tokens.push_back({indent, trim_str(content)});
    }

    return tokens;
}

// ---------------------------------------------------------------------------
// YAML scalar parser
// ---------------------------------------------------------------------------

/**
 * Parse a single scalar value string into a YAMLValue.
 * Handles quoted strings, booleans, integers, and bare (unquoted) strings.
 */
YAMLValue parse_scalar(const std::string& s) {
    YAMLValue val;
    std::string trimmed = trim_str(s);

    if (trimmed.empty()) {
        val.type = YAMLValue::String;
        return val;
    }

    // Quoted string
    if (trimmed.size() >= 2 && trimmed.front() == '"' && trimmed.back() == '"') {
        val.type = YAMLValue::String;
        val.str_val = trimmed.substr(1, trimmed.size() - 2);
        return val;
    }

    // Booleans
    if (trimmed == "true" || trimmed == "yes" || trimmed == "True" || trimmed == "Yes") {
        val.type = YAMLValue::Bool;
        val.bool_val = true;
        return val;
    }
    if (trimmed == "false" || trimmed == "no" || trimmed == "False" || trimmed == "No") {
        val.type = YAMLValue::Bool;
        val.bool_val = false;
        return val;
    }

    // Integer: optional leading '-', then all digits
    bool is_int = !trimmed.empty();
    for (size_t i = (trimmed[0] == '-' ? 1 : 0); i < trimmed.size(); ++i) {
        if (!std::isdigit(static_cast<unsigned char>(trimmed[i]))) {
            is_int = false;
            break;
        }
    }
    if (is_int && trimmed != "-") {
        val.type = YAMLValue::Int;
        val.int_val = std::stoll(trimmed);
        return val;
    }

    // Fall-through: bare string
    val.type = YAMLValue::String;
    val.str_val = trimmed;
    return val;
}

// ---------------------------------------------------------------------------
// Forward declarations for recursive YAML parsing
// ---------------------------------------------------------------------------

YAMLValue parse_value(const std::vector<LineToken>& lines, size_t& pos,
                      int base_indent);

YAMLValue parse_map(const std::vector<LineToken>& lines, size_t& pos,
                    int base_indent);

YAMLValue parse_list(const std::vector<LineToken>& lines, size_t& pos,
                     int base_indent);

// ---------------------------------------------------------------------------
// parse_value — dispatches to parse_map or parse_list based on the current line
// ---------------------------------------------------------------------------

YAMLValue parse_value(const std::vector<LineToken>& lines, size_t& pos,
                      int base_indent) {
    if (pos >= lines.size()) {
        return YAMLValue();   // Null
    }

    // Skip lines whose indent is less than base_indent — end of this block
    if (lines[pos].indent < base_indent) {
        return YAMLValue();
    }

    // Lines with indent > base_indent are a structural error; skip and return
    if (lines[pos].indent > base_indent) {
        ++pos;
        return YAMLValue();
    }

    // dispatch
    if (starts_with(lines[pos].content, "- ")) {
        return parse_list(lines, pos, base_indent);
    }

    return parse_map(lines, pos, base_indent);
}

// ---------------------------------------------------------------------------
// parse_map — reads key: value pairs at @p base_indent
// ---------------------------------------------------------------------------

YAMLValue parse_map(const std::vector<LineToken>& lines, size_t& pos,
                    int base_indent) {
    YAMLValue result;
    result.type = YAMLValue::Map;

    while (pos < lines.size()) {
        const auto& line = lines[pos];

        // End conditions
        if (line.indent < base_indent) break;            // back to parent
        if (line.indent > base_indent) break;            // child block
        if (starts_with(line.content, "- ")) break;      // list belongs to parent

        // Extract key: value
        size_t colon = line.content.find(':');
        if (colon == std::string::npos) {
            // Malformed line — skip
            ++pos;
            continue;
        }

        std::string key   = trim_str(line.content.substr(0, colon));
        std::string rest  = trim_str(line.content.substr(colon + 1));
        ++pos;  // consume this line

        if (rest.empty()) {
            // Value is on subsequent indented lines
            result.map_val[key] = parse_value(lines, pos, base_indent + 1);
        } else {
            result.map_val[key] = parse_scalar(rest);
        }
    }

    return result;
}

// ---------------------------------------------------------------------------
// parse_list — reads "- " items at @p base_indent
// ---------------------------------------------------------------------------

YAMLValue parse_list(const std::vector<LineToken>& lines, size_t& pos,
                     int base_indent) {
    YAMLValue result;
    result.type = YAMLValue::List;

    while (pos < lines.size()) {
        const auto& line = lines[pos];

        // End conditions
        if (line.indent < base_indent) break;
        if (line.indent > base_indent) break;
        if (!starts_with(line.content, "- ")) break;     // no more list items

        std::string item = trim_str(line.content.substr(2));
        ++pos;  // consume the "- " line

        size_t colon = item.find(':');
        if (colon != std::string::npos) {
            // "- key: value" (or "- key:") — list of maps
            std::string item_key  = trim_str(item.substr(0, colon));
            std::string item_rest = trim_str(item.substr(colon + 1));

            YAMLValue item_map;
            item_map.type = YAMLValue::Map;

            if (item_rest.empty()) {
                // "- key:" — value nested on next lines
                item_map.map_val[item_key] = parse_value(lines, pos, base_indent + 1);
            } else {
                // "- key: value" — starts a map object
                item_map.map_val[item_key] = parse_scalar(item_rest);
            }

            // Consume remaining keys of this map at indent+1
            // (e.g. after "- name: rpi4" come "  display_name: ..." lines)
            YAMLValue rest_map = parse_map(lines, pos, base_indent + 1);
            for (auto& [k, v] : rest_map.map_val) {
                item_map.map_val[k] = std::move(v);
            }

            result.list_val.push_back(std::move(item_map));
        } else {
            // "- value" — scalar list item
            result.list_val.push_back(parse_scalar(item));
        }
    }

    return result;
}

// ---------------------------------------------------------------------------
// YAML map access helpers
// ---------------------------------------------------------------------------

std::string get_str(const YAMLMap& m, const std::string& key,
                    const std::string& def = "") {
    auto it = m.find(key);
    if (it == m.end()) return def;
    if (it->second.type == YAMLValue::String) return it->second.str_val;
    if (it->second.type == YAMLValue::Int)    return std::to_string(it->second.int_val);
    if (it->second.type == YAMLValue::Bool)   return it->second.bool_val ? "true" : "false";
    return def;
}

int64_t get_int(const YAMLMap& m, const std::string& key, int64_t def = 0) {
    auto it = m.find(key);
    if (it == m.end()) return def;
    if (it->second.type == YAMLValue::Int) return it->second.int_val;
    if (it->second.type == YAMLValue::String) {
        try { return std::stoll(it->second.str_val); }
        catch (...) { return def; }
    }
    return def;
}

bool get_bool(const YAMLMap& m, const std::string& key, bool def = false) {
    auto it = m.find(key);
    if (it == m.end()) return def;
    if (it->second.type == YAMLValue::Bool) return it->second.bool_val;
    if (it->second.type == YAMLValue::String) {
        const auto& s = it->second.str_val;
        if (s == "true" || s == "yes" || s == "True" || s == "Yes") return true;
        if (s == "false" || s == "no" || s == "False" || s == "No") return false;
    }
    return def;
}

std::vector<std::string> get_str_list(const YAMLMap& m, const std::string& key) {
    auto it = m.find(key);
    if (it == m.end()) return {};
    std::vector<std::string> result;
    if (it->second.type == YAMLValue::List) {
        for (const auto& item : it->second.list_val) {
            if (item.type == YAMLValue::String) {
                result.push_back(item.str_val);
            } else if (item.type == YAMLValue::Int) {
                result.push_back(std::to_string(item.int_val));
            }
        }
    }
    return result;
}

std::optional<std::string> get_optional_str(const YAMLMap& m,
                                             const std::string& key) {
    auto it = m.find(key);
    if (it == m.end()) return std::nullopt;
    if (it->second.type == YAMLValue::String) {
        if (it->second.str_val.empty()) return std::nullopt;
        return it->second.str_val;
    }
    return std::nullopt;
}

// ---------------------------------------------------------------------------
// Filesystem type parsing
// ---------------------------------------------------------------------------

FilesystemType parse_filesystem_type(const std::string& s) {
    if (s == "vfat"   || s == "fat32" || s == "fat16" || s == "fat")  return FilesystemType::VFAT;
    if (s == "ext4"   || s == "ext3"  || s == "ext2")                 return FilesystemType::EXT4;
    if (s == "squashfs")                                              return FilesystemType::SquashFS;
    if (s == "raw"    || s == "none")                                 return FilesystemType::Raw;
    return FilesystemType::Unknown;
}

// ---------------------------------------------------------------------------
// Partition layout parser (from YAML map)
// ---------------------------------------------------------------------------

Result<PartitionLayout> parse_layout_yaml(const std::string& name, const YAMLMap& obj) {
    PartitionLayout layout;
    layout.name          = name;
    layout.table_type    = get_str(obj, "table_type", "gpt");
    layout.alignment_mib = static_cast<uint32_t>(get_int(obj, "alignment_mib", 4));

    auto parts_it = obj.find("partitions");
    if (parts_it != obj.end() && parts_it->second.type == YAMLValue::List) {
        for (const auto& part_val : parts_it->second.list_val) {
            if (part_val.type != YAMLValue::Map) continue;

            PartitionSpec spec;
            spec.name       = get_str(part_val.map_val, "name");
            spec.size_mib   = static_cast<uint64_t>(get_int(part_val.map_val, "size_mib", 0));
            spec.filesystem = parse_filesystem_type(
                                  get_str(part_val.map_val, "filesystem", "ext4"));
            spec.label      = get_str(part_val.map_val, "label");
            layout.partitions.push_back(spec);
        }
    }

    return Result<PartitionLayout>::ok(layout);
}

// ---------------------------------------------------------------------------
// HardwareProfile parsing (from YAML map)
// ---------------------------------------------------------------------------

Result<HardwareProfile> parse_profile_yaml(const YAMLMap& obj) {
    HardwareProfile profile;

    profile.name         = get_str(obj, "name");
    profile.display_name = get_str(obj, "display_name", profile.name);
    profile.architecture = get_str(obj, "architecture");

    if (profile.name.empty() || profile.architecture.empty()) {
        return Result<HardwareProfile>::err(InstallerError::make(
            ErrorCode::INTERNAL_CONFIG_ERROR,
            "Invalid Profile",
            "Hardware profile is missing required 'name' or 'architecture' field.",
            "name=" + profile.name + " arch=" + profile.architecture));
    }

    // min_disk_size_gb in config -> min_disk_size_bytes in struct
    int64_t min_disk_gb = get_int(obj, "min_disk_size_gb", 0);
    profile.min_disk_size_bytes = static_cast<uint64_t>(min_disk_gb) *
                                  1024ULL * 1024ULL * 1024ULL;

    profile.device_candidates       = get_str_list(obj, "device_candidates");
    profile.partition_layout        = get_str(obj, "partition_layout");
    profile.bootloader_type         = get_str(obj, "bootloader");
    profile.boot_device             = get_str(obj, "boot_device");
    profile.required_kernel_modules = get_str_list(obj, "kernel_modules");
    profile.dt_compatible           = get_optional_str(obj, "dt_compatible");

    // Collect any extra keys not already consumed into 'extra'
    static const std::vector<std::string> known_keys = {
        "name", "display_name", "architecture", "min_disk_size_gb",
        "device_candidates", "partition_layout", "bootloader", "boot_device",
        "kernel_modules", "dt_compatible"
    };
    for (const auto& [k, v] : obj) {
        if (std::find(known_keys.begin(), known_keys.end(), k) == known_keys.end()) {
            if (v.type == YAMLValue::String) {
                profile.extra[k] = v.str_val;
            } else if (v.type == YAMLValue::Int) {
                profile.extra[k] = std::to_string(v.int_val);
            } else if (v.type == YAMLValue::Bool) {
                profile.extra[k] = v.bool_val ? "true" : "false";
            }
        }
    }

    return Result<HardwareProfile>::ok(profile);
}

} // anonymous namespace

// ============================================================================
// HardwareProfileManager — public API
// ============================================================================

HardwareProfileManager::HardwareProfileManager() {
    load_builtin_profiles();
}

// ---------------------------------------------------------------------------
// Loading
// ---------------------------------------------------------------------------

Result<void> HardwareProfileManager::load_profiles(const std::string& config_path) {
    std::ifstream file(config_path);
    if (!file.is_open()) {
        // Config file unavailable — fall back to built-ins (already loaded in
        // the constructor, but reconstruct to get a clean state).
        std::lock_guard<std::mutex> lock(mutex_);
        profiles_.clear();
        layout_templates_.clear();
        load_builtin_profiles();
        return Result<void>::ok();
    }

    std::ostringstream buffer;
    buffer << file.rdbuf();
    return load_profiles_from_string(buffer.str());
}

Result<void> HardwareProfileManager::load_profiles_from_string(
        const std::string& yaml_content) {

    auto tokens = tokenize_yaml(yaml_content);
    if (tokens.empty()) {
        std::lock_guard<std::mutex> lock(mutex_);
        profiles_.clear();
        layout_templates_.clear();
        load_builtin_profiles();
        return Result<void>::ok();
    }

    size_t pos = 0;
    YAMLValue root = parse_map(tokens, pos, 0);

    std::lock_guard<std::mutex> lock(mutex_);

    // ---- Parse profiles ----------------------------------------------------
    auto profiles_it = root.map_val.find("profiles");
    if (profiles_it != root.map_val.end() &&
        profiles_it->second.type == YAMLValue::List) {

        profiles_.clear();
        for (const auto& item : profiles_it->second.list_val) {
            if (item.type != YAMLValue::Map) continue;

            auto parsed = parse_profile_yaml(item.map_val);
            if (parsed.is_ok()) {
                profiles_.push_back(std::move(parsed.value()));
            }
        }
    }

    // ---- Parse partition layouts -------------------------------------------
    auto layouts_it = root.map_val.find("partition_layouts");
    if (layouts_it != root.map_val.end() &&
        layouts_it->second.type == YAMLValue::Map) {

        for (const auto& [layout_name, layout_val] : layouts_it->second.map_val) {
            if (layout_val.type != YAMLValue::Map) continue;

            auto parsed_layout = parse_layout_yaml(layout_name, layout_val.map_val);
            if (parsed_layout.is_ok()) {
                layout_templates_[layout_name] = std::move(parsed_layout.value());
            }
        }
    }

    // If no profiles were parsed successfully, fall back to built-ins.
    if (profiles_.empty()) {
        profiles_.clear();
        layout_templates_.clear();
        load_builtin_profiles();
    }

    return Result<void>::ok();
}

void HardwareProfileManager::add_profile(const HardwareProfile& profile) {
    std::lock_guard<std::mutex> lock(mutex_);
    profiles_.push_back(profile);
}

// ---------------------------------------------------------------------------
// Matching
// ---------------------------------------------------------------------------

Result<HardwareProfile> HardwareProfileManager::match_current_hardware() const {
    std::string arch              = detect_architecture();
    uint64_t disk_size            = get_system_disk_size();
    std::vector<std::string> devs = get_system_devices();
    return match_hardware(arch, disk_size, devs);
}

Result<HardwareProfile> HardwareProfileManager::match_hardware(
        const std::string& architecture,
        uint64_t disk_size_bytes,
        const std::vector<std::string>& device_paths) const {

    std::lock_guard<std::mutex> lock(mutex_);

    if (profiles_.empty()) {
        return Result<HardwareProfile>::err(InstallerError::make(
            ErrorCode::DEVICE_NOT_FOUND,
            "No Hardware Profiles",
            "No hardware profiles are loaded. Provide a config file or call "
            "add_profile() before matching.",
            ""));
    }

    std::string dt_compat = detect_dt_compatible();

    struct Candidate {
        const HardwareProfile* profile;
        int score;
    };
    std::vector<Candidate> candidates;

    for (const auto& p : profiles_) {
        // ---- Architecture filter ----
        if (p.architecture != architecture) continue;

        // ---- Disk size filter ----
        if (p.min_disk_size_bytes > 0 && disk_size_bytes > 0 &&
            disk_size_bytes < p.min_disk_size_bytes) {
            continue;
        }

        int score = 0;

        // ---- Device candidate bonus ----
        for (const auto& cand : p.device_candidates) {
            for (const auto& dev : device_paths) {
                // Exact match or substring match (e.g. /dev/mmcblk0 matches
                // /dev/mmcblk0p1 if users list the whole-disk path).
                if (dev == cand || dev.find(cand) != std::string::npos ||
                    cand.find(dev) != std::string::npos) {
                    score += 15;
                    break;
                }
            }
        }

        // ---- Device-tree compatible bonus ----
        if (p.dt_compatible.has_value() && !p.dt_compatible->empty() &&
            !dt_compat.empty()) {
            if (dt_compat.find(p.dt_compatible.value()) != std::string::npos) {
                score += 75;
            }
        }

        candidates.push_back({&p, score});
    }

    // ---- Return best candidate ----
    if (!candidates.empty()) {
        std::sort(candidates.begin(), candidates.end(),
                  [](const Candidate& a, const Candidate& b) {
                      return a.score > b.score;
                  });
        return Result<HardwareProfile>::ok(*candidates[0].profile);
    }

    // ---- Relaxed fallback: match on architecture only ----
    for (const auto& p : profiles_) {
        if (p.architecture == architecture) {
            return Result<HardwareProfile>::ok(p);
        }
    }

    return Result<HardwareProfile>::err(InstallerError::make(
        ErrorCode::DEVICE_NOT_FOUND,
        "No Matching Hardware Profile",
        "Could not find a hardware profile matching architecture '" +
            architecture + "'.",
        ""));
}

// ---------------------------------------------------------------------------
// Lookup
// ---------------------------------------------------------------------------

Result<HardwareProfile> HardwareProfileManager::get_profile(
        const std::string& name) const {
    std::lock_guard<std::mutex> lock(mutex_);

    for (const auto& p : profiles_) {
        if (p.name == name) {
            return Result<HardwareProfile>::ok(p);
        }
    }

    return Result<HardwareProfile>::err(InstallerError::make(
        ErrorCode::INTERNAL_CONFIG_ERROR,
        "Profile Not Found",
        "No hardware profile found with name: " + name,
        "name=" + name));
}

std::vector<HardwareProfile> HardwareProfileManager::get_all_profiles() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return profiles_;
}

Result<PartitionLayout> HardwareProfileManager::get_partition_layout(
        const HardwareProfile& profile) const {
    std::lock_guard<std::mutex> lock(mutex_);

    auto it = layout_templates_.find(profile.partition_layout);
    if (it != layout_templates_.end()) {
        return Result<PartitionLayout>::ok(it->second);
    }

    return Result<PartitionLayout>::err(InstallerError::make(
        ErrorCode::INTERNAL_CONFIG_ERROR,
        "Partition Layout Not Found",
        "Partition layout '" + profile.partition_layout +
            "' referenced by profile '" + profile.name + "' was not found.",
        "profile=" + profile.name +
            " layout=" + profile.partition_layout));
}

// ---------------------------------------------------------------------------
// Bootloader config
// ---------------------------------------------------------------------------

Result<HardwareProfileManager::BootloaderConfig>
HardwareProfileManager::get_bootloader_config(
        const HardwareProfile& profile) const {

    std::lock_guard<std::mutex> lock(mutex_);

    BootloaderConfig config;
    config.type        = profile.bootloader_type;
    config.boot_device = profile.boot_device;

    // Sensible default environment variables per bootloader type.
    if (profile.bootloader_type == "u-boot") {
        config.env_vars["boot_target"]    = "mmc";
        config.env_vars["boot_part"]      = "1";
        config.env_vars["bootargs_otp"]   = "console=tty1 rootwait";
    } else if (profile.bootloader_type == "grub") {
        config.env_vars["prefix"]         = "(hd0,gpt1)/boot/grub";
        config.env_vars["root"]           = "hd0,gpt2";
    } else if (profile.bootloader_type == "barebox") {
        config.env_vars["global.boot.default"]        = "mmc";
        config.env_vars["global.linux.bootargs.base"] = "console=tty1 rootwait";
    }

    return Result<BootloaderConfig>::ok(config);
}

// ---------------------------------------------------------------------------
// System detection (static)
// ---------------------------------------------------------------------------

std::string HardwareProfileManager::detect_architecture() {
    struct utsname buf;
    if (uname(&buf) != 0) {
        return "unknown";
    }
    return std::string(buf.machine);
}

std::string HardwareProfileManager::detect_dt_compatible() {
    std::ifstream file("/proc/device-tree/compatible", std::ios::binary);
    if (!file.is_open()) {
        return "";
    }

    std::string content((std::istreambuf_iterator<char>(file)),
                         std::istreambuf_iterator<char>());
    if (content.empty()) {
        return "";
    }

    // The compatible file contains null-terminated strings concatenated
    // together.  The first string (up to the first NUL byte) is the most
    // specific compatible identifier.
    size_t null_pos = content.find('\0');
    if (null_pos != std::string::npos) {
        return content.substr(0, null_pos);
    }
    return content;
}

uint64_t HardwareProfileManager::get_system_disk_size() {
    // 1. Find the root device from /proc/mounts
    std::ifstream mounts("/proc/mounts");
    if (!mounts.is_open()) return 0;

    std::string root_device;
    {
        std::string line;
        while (std::getline(mounts, line)) {
            std::istringstream iss(line);
            std::string device, mountpoint;
            if (iss >> device >> mountpoint) {
                if (mountpoint == "/") {
                    root_device = device;
                    break;
                }
            }
        }
    }

    if (root_device.empty()) return 0;

    // 2. Strip leading "/dev/" to get the kernel device name
    std::string dev_name = root_device;
    if (starts_with(dev_name, "/dev/")) {
        dev_name = dev_name.substr(5);
    }
    if (dev_name.empty()) return 0;

    // 3. If it is a partition, extract the base block-device name.
    //
    //    mmcblk0p1  ->  mmcblk0
    //    nvme0n1p2  ->  nvme0n1
    //    sda2       ->  sda
    //    loop0p1    ->  loop0
    //
    //    Strategy: if the name starts with a known prefix that uses 'p' as a
    //    partition separator (mmcblk, nvme, loop), strip the 'pN' suffix.
    //    Otherwise strip trailing digits only.
    {
        bool uses_p_separator = starts_with(dev_name, "mmcblk") ||
                                starts_with(dev_name, "nvme")   ||
                                starts_with(dev_name, "loop");

        if (uses_p_separator) {
            // Find the last 'p' that is followed by digits only
            size_t last_p = dev_name.rfind('p');
            if (last_p != std::string::npos && last_p + 1 < dev_name.size()) {
                bool all_digits = true;
                for (size_t i = last_p + 1; i < dev_name.size(); ++i) {
                    if (!std::isdigit(static_cast<unsigned char>(dev_name[i]))) {
                        all_digits = false;
                        break;
                    }
                }
                if (all_digits) {
                    dev_name = dev_name.substr(0, last_p);
                }
            }
        } else {
            // sdX, hdX, vdX, xvdX — strip trailing digits
            while (!dev_name.empty() &&
                   std::isdigit(static_cast<unsigned char>(dev_name.back()))) {
                dev_name.pop_back();
            }
        }
    }

    // 4. Read size from /sys/block/<dev_name>/size (in 512-byte sectors)
    std::string size_path = "/sys/block/" + dev_name + "/size";
    std::ifstream size_file(size_path);
    if (!size_file.is_open()) return 0;

    uint64_t sectors = 0;
    size_file >> sectors;

    return sectors * 512;
}

std::vector<std::string> HardwareProfileManager::get_system_devices() {
    std::vector<std::string> devices;

    DIR* dir = opendir("/sys/block");
    if (!dir) return devices;

    struct dirent* entry;
    while ((entry = readdir(dir)) != nullptr) {
        // Skip ".", "..", and non-directory entries
        if (entry->d_name[0] == '.') continue;
        if (entry->d_type != DT_DIR && entry->d_type != DT_LNK) continue;

        std::string dev_path = "/dev/" + std::string(entry->d_name);
        // Only include if the device node actually exists
        if (access(dev_path.c_str(), F_OK) == 0) {
            devices.push_back(dev_path);
        }
    }
    closedir(dir);

    return devices;
}

// ---------------------------------------------------------------------------
// YAML parsing (private method)
// ---------------------------------------------------------------------------

Result<std::vector<HardwareProfile>> HardwareProfileManager::parse_yaml_profiles(
        const std::string& yaml) {

    auto tokens = tokenize_yaml(yaml);
    if (tokens.empty()) {
        return Result<std::vector<HardwareProfile>>::ok({});
    }

    size_t pos = 0;
    YAMLValue root = parse_map(tokens, pos, 0);

    std::vector<HardwareProfile> profiles;

    auto profiles_it = root.map_val.find("profiles");
    if (profiles_it != root.map_val.end() &&
        profiles_it->second.type == YAMLValue::List) {

        for (const auto& item : profiles_it->second.list_val) {
            if (item.type != YAMLValue::Map) continue;

            auto parsed = parse_profile_yaml(item.map_val);
            if (parsed.is_ok()) {
                profiles.push_back(std::move(parsed.value()));
            }
        }
    }

    return Result<std::vector<HardwareProfile>>::ok(profiles);
}

// ---------------------------------------------------------------------------
// Built-in defaults
// ---------------------------------------------------------------------------

void HardwareProfileManager::load_builtin_profiles() {
    // ---- Profile: Raspberry Pi 4 -------------------------------------------
    {
        HardwareProfile p;
        p.name                = "raspberry_pi_4";
        p.display_name        = "Raspberry Pi 4";
        p.architecture        = "aarch64";
        p.min_disk_size_bytes = 8ULL * 1024 * 1024 * 1024;   // 8 GiB
        p.device_candidates   = {"/dev/mmcblk0", "/dev/mmcblk1"};
        p.partition_layout    = "ab_standard_v1";
        p.bootloader_type     = "u-boot";
        p.boot_device         = "/dev/mmcblk0p1";
        p.required_kernel_modules = {"bcm2835", "vc4"};
        p.dt_compatible       = "raspberrypi,4-model-b";
        profiles_.push_back(std::move(p));
    }

    // ---- Profile: Generic x86_64 -------------------------------------------
    {
        HardwareProfile p;
        p.name                = "generic_x86_64";
        p.display_name        = "Generic x86_64";
        p.architecture        = "x86_64";
        p.min_disk_size_bytes = 16ULL * 1024 * 1024 * 1024;  // 16 GiB
        p.device_candidates   = {"/dev/sda", "/dev/nvme0n1"};
        p.partition_layout    = "ab_x86_v1";
        p.bootloader_type     = "grub";
        p.boot_device         = "/dev/sda1";
        profiles_.push_back(std::move(p));
    }

    // ---- Layout: ab_standard_v1 (ARM / eMMC / SD) --------------------------
    {
        PartitionLayout layout;
        layout.name          = "ab_standard_v1";
        layout.table_type    = "gpt";
        layout.alignment_mib = 4;
        layout.partitions    = {
            {"boot",     256,  FilesystemType::VFAT, "boot"},
            {"rootfs_a", 2048, FilesystemType::EXT4, "rootfs_a"},
            {"rootfs_b", 2048, FilesystemType::EXT4, "rootfs_b"},
            {"data",     0,    FilesystemType::EXT4, "data"},
        };
        layout_templates_[layout.name] = layout;
    }

    // ---- Layout: ab_x86_v1 (x86 / SATA / NVMe) -----------------------------
    {
        PartitionLayout layout;
        layout.name          = "ab_x86_v1";
        layout.table_type    = "gpt";
        layout.alignment_mib = 4;
        layout.partitions    = {
            {"efi",      512,  FilesystemType::VFAT, "efi"},
            {"rootfs_a", 4096, FilesystemType::EXT4, "rootfs_a"},
            {"rootfs_b", 4096, FilesystemType::EXT4, "rootfs_b"},
            {"data",     0,    FilesystemType::EXT4, "data"},
        };
        layout_templates_[layout.name] = layout;
    }
}

} // namespace installer
