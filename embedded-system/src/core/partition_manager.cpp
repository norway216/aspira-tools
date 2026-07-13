/**
 * @file partition_manager.cpp
 * @brief PartitionManager implementation using sgdisk via ProcessRunner.
 */

#include "partition_manager.h"
#include "installer/partition/ipartition_manager.h"
#include "installer/platform/iprocess_runner.h"

#include <string>
#include <vector>
#include <sstream>
#include <algorithm>
#include <cstring>
#include <regex>
#include <mutex>
#include <unistd.h>
#include <sys/stat.h>
#include <dirent.h>
#include <limits.h>
#include <cctype>
#include <cmath>

namespace installer {

// =============================================================================
// Construction / Destruction
// =============================================================================

PartitionManager::PartitionManager(IProcessRunner* proc_runner)
    : proc_runner_(proc_runner)
{
    // proc_runner is expected to be a valid, lifecycle-managed pointer.
    // No null-check here — it is the caller's responsibility.
}

PartitionManager::~PartitionManager() = default;

// =============================================================================
// Static helpers — GPT type codes
// =============================================================================

std::string PartitionManager::fs_type_to_sgdisk_code(FilesystemType fs_type)
{
    switch (fs_type) {
    case FilesystemType::VFAT:
        return "EF00";      // EFI System Partition
    case FilesystemType::EXT4:
        return "8300";      // Linux filesystem
    case FilesystemType::SquashFS:
        return "8300";      // Linux filesystem (read-only variant)
    case FilesystemType::Raw:
        return "8300";      // generic Linux filesystem
    case FilesystemType::Unknown:
    default:
        return "8300";
    }
}

FilesystemType PartitionManager::sgdisk_code_to_fs_type(const std::string& code)
{
    // EFI-related codes
    if (code == "EF00" || code == "EF02" || code == "0700") {
        return FilesystemType::VFAT;
    }
    // Linux filesystem codes (8300–83FF)
    if (code.size() == 4 && code[0] == '8' && code[1] == '3') {
        return FilesystemType::EXT4;
    }
    // Linux swap
    if (code == "8200") {
        return FilesystemType::Raw;
    }
    // Everything else
    return FilesystemType::Unknown;
}

// =============================================================================
// Static helpers — device naming
// =============================================================================

std::string PartitionManager::partition_suffix(const std::string& device_path)
{
    // Extract basename: /dev/mmcblk0 -> mmcblk0
    size_t slash = device_path.rfind('/');
    std::string name = (slash != std::string::npos)
                           ? device_path.substr(slash + 1)
                           : device_path;

    // If the base name ends with a digit (mmcblk0, nvme0n1, loop0),
    // the kernel uses a 'p' separator before the partition number.
    if (!name.empty() && std::isdigit(static_cast<unsigned char>(name.back()))) {
        return "p";
    }
    return "";
}

// =============================================================================
// Static helpers — block-device enumeration
// =============================================================================

std::vector<std::string> PartitionManager::enumerate_block_devices()
{
    std::vector<std::string> devices;

    DIR* dir = opendir("/sys/block");
    if (dir == nullptr) {
        return devices;
    }

    struct dirent* entry;
    while ((entry = readdir(dir)) != nullptr) {
        // Skip . and ..
        if (entry->d_name[0] == '.') {
            continue;
        }

        std::string name(entry->d_name);

        // Skip loop devices
        if (name.compare(0, 4, "loop") == 0) {
            continue;
        }
        // Skip RAM disks
        if (name.compare(0, 3, "ram") == 0) {
            continue;
        }
        // Skip device-mapper entries
        if (name.compare(0, 3, "dm-") == 0) {
            continue;
        }
        // Skip zram
        if (name.compare(0, 4, "zram") == 0) {
            continue;
        }

        // Check that a 'dev' file exists inside the sysfs directory
        // to confirm this is a real block device (not a virtual subsystem).
        std::string dev_file = "/sys/block/" + name + "/dev";
        struct stat st;
        if (stat(dev_file.c_str(), &st) != 0) {
            continue;
        }

        devices.push_back("/dev/" + name);
    }

    closedir(dir);
    return devices;
}

// =============================================================================
// Static helpers — size parsing
// =============================================================================

uint64_t PartitionManager::parse_size_to_mib(const std::string& size_str,
                                               const std::string& unit_str)
{
    // Parse the numeric part
    double value = 0.0;
    try {
        value = std::stod(size_str);
    } catch (const std::exception&) {
        return 0;
    }

    // Convert to MiB based on unit
    if (unit_str == "MiB" || unit_str == "M") {
        return static_cast<uint64_t>(std::round(value));
    } else if (unit_str == "GiB" || unit_str == "G") {
        return static_cast<uint64_t>(std::round(value * 1024.0));
    } else if (unit_str == "KiB" || unit_str == "K") {
        return static_cast<uint64_t>(std::round(value / 1024.0));
    } else if (unit_str == "TiB" || unit_str == "T") {
        return static_cast<uint64_t>(std::round(value * 1024.0 * 1024.0));
    } else if (unit_str == "B") {
        return static_cast<uint64_t>(std::round(value / (1024.0 * 1024.0)));
    } else if (unit_str == "sectors") {
        // 1 sector = 512 bytes; 1 MiB = 2048 sectors
        return static_cast<uint64_t>(std::round(value / 2048.0));
    }
    return 0;
}

// =============================================================================
// create_partition_table
// =============================================================================

Result<void> PartitionManager::create_partition_table(
    const std::string& device_path,
    const PartitionLayout& layout)
{
    std::lock_guard<std::mutex> lock(mutex_);

    // ---- Step 1: Zap all existing partition tables --------------------------
    {
        std::vector<std::string> args = {"sgdisk", "--zap-all", device_path};
        auto result = proc_runner_->run(args, std::chrono::seconds(30));

        if (!result.is_ok()) {
            return Result<void>::err(
                InstallerError::make(
                    ErrorCode::PARTITION_CREATE_FAILED,
                    "Partition Create Failed",
                    "Failed to clear existing partition table on " + device_path,
                    "sgdisk --zap-all process error: " + result.error().technical_message,
                    true));
        }

        const auto& proc = result.value();
        if (proc.exit_code != 0) {
            return Result<void>::err(
                InstallerError::make(
                    ErrorCode::PARTITION_CREATE_FAILED,
                    "Partition Create Failed",
                    "Failed to clear existing partition table on " + device_path,
                    "sgdisk --zap-all exit code " + std::to_string(proc.exit_code)
                        + ": " + proc.stderr_data,
                    true));
        }
    }

    // ---- Step 2: Build the combined sgdisk command --------------------------
    //
    // We use a single invocation of `sgdisk --clear -n ... -t ... -c ...`
    // to create the GPT table and all partitions atomically.  This avoids
    // intermediate states where the kernel might re-read a half-written table.

    std::vector<std::string> args;
    args.reserve(8 + layout.partitions.size() * 6);
    args.push_back("sgdisk");
    args.push_back("--clear");

    // Alignment hint (convert MiB to 512-byte sectors)
    if (layout.alignment_mib > 0) {
        uint64_t align_sectors = layout.alignment_mib * 2048;
        args.push_back("-a");
        args.push_back(std::to_string(align_sectors));
    }

    // Build partition entries
    int part_num = 1;
    for (const auto& spec : layout.partitions) {
        std::string num_str = std::to_string(part_num);

        // Size specification: +<size>M, or 0 for "fill remaining"
        std::string size_spec;
        if (spec.size_mib == 0) {
            size_spec = "0";
        } else {
            size_spec = "+" + std::to_string(spec.size_mib) + "M";
        }

        std::string type_code = fs_type_to_sgdisk_code(spec.filesystem);
        std::string gpt_name = spec.label.empty() ? spec.name : spec.label;

        // -n <partnum>:<start>:<end>
        args.push_back("-n");
        args.push_back(num_str + ":0:" + size_spec);

        // -t <partnum>:<type_code>
        args.push_back("-t");
        args.push_back(num_str + ":" + type_code);

        // -c <partnum>:<name>
        args.push_back("-c");
        args.push_back(num_str + ":" + gpt_name);

        ++part_num;
    }

    args.push_back(device_path);

    // ---- Step 3: Execute the partition-creation command ---------------------
    {
        auto result = proc_runner_->run(args, std::chrono::seconds(60));

        if (!result.is_ok()) {
            return Result<void>::err(
                InstallerError::make(
                    ErrorCode::PARTITION_CREATE_FAILED,
                    "Partition Create Failed",
                    "Failed to create partitions on " + device_path,
                    "sgdisk process error: " + result.error().technical_message,
                    true));
        }

        const auto& proc = result.value();
        if (proc.exit_code != 0) {
            return Result<void>::err(
                InstallerError::make(
                    ErrorCode::PARTITION_CREATE_FAILED,
                    "Partition Create Failed",
                    "Failed to create partitions on " + device_path,
                    "sgdisk exit code " + std::to_string(proc.exit_code)
                        + ": " + proc.stderr_data,
                    true));
        }
    }

    // ---- Step 4: Notify the kernel of the new partition table ---------------
    {
        std::vector<std::string> partprobe_args = {"partprobe", device_path};
        // Best-effort — failure here is not fatal; wait_for_partitions
        // will pick up any transient issue.
        proc_runner_->run(partprobe_args, std::chrono::seconds(15));
    }

    // ---- Step 5: Wait for device nodes to materialise -----------------------
    return wait_for_partitions(device_path, 10);
}

// =============================================================================
// read_partition_table
// =============================================================================

Result<std::vector<PartitionSpec>> PartitionManager::read_partition_table(
    const std::string& device_path)
{
    std::lock_guard<std::mutex> lock(mutex_);

    std::vector<std::string> args = {"sgdisk", "--print", device_path};
    auto result = proc_runner_->run(args, std::chrono::seconds(15));

    if (!result.is_ok()) {
        return Result<std::vector<PartitionSpec>>::err(
            InstallerError::make(
                ErrorCode::PARTITION_NOT_FOUND,
                "Partition Not Found",
                "Failed to read partition table from " + device_path,
                "sgdisk process error: " + result.error().technical_message,
                true));
    }

    const auto& proc = result.value();
    if (proc.exit_code != 0) {
        return Result<std::vector<PartitionSpec>>::err(
            InstallerError::make(
                ErrorCode::PARTITION_NOT_FOUND,
                "Partition Not Found",
                "Failed to read partition table from " + device_path,
                "sgdisk --print exit code " + std::to_string(proc.exit_code)
                    + ": " + proc.stderr_data,
                true));
    }

    return parse_sgdisk_output(proc.stdout_data);
}

// =============================================================================
// verify_partition_layout
// =============================================================================

Result<bool> PartitionManager::verify_partition_layout(
    const std::string& device_path,
    const PartitionLayout& expected)
{
    auto read_result = read_partition_table(device_path);
    if (!read_result.is_ok()) {
        return Result<bool>::err(read_result.take_error());
    }

    const auto& actual = read_result.value();

    // ---- Check 1: Partition count -------------------------------------------
    if (actual.size() != expected.partitions.size()) {
        return Result<bool>::ok(false);
    }

    // ---- Check 2: Individual partitions -------------------------------------
    for (size_t i = 0; i < actual.size(); ++i) {
        const auto& ap = actual[i];
        const auto& ep = expected.partitions[i];

        // Determine the expected GPT partition name
        std::string expected_name = ep.label.empty() ? ep.name : ep.label;

        // Compare partition name (GPT label).  Check both the 'name' and
        // 'label' fields of the actual spec since the parser populates
        // whichever was found.
        std::string actual_name = ap.label.empty() ? ap.name : ap.label;
        if (actual_name != expected_name && ap.name != ep.name) {
            return Result<bool>::ok(false);
        }

        // Check size within tolerance (±1 MiB for alignment variance)
        if (ep.size_mib > 0) {
            int64_t diff = static_cast<int64_t>(ap.size_mib)
                           - static_cast<int64_t>(ep.size_mib);
            if (std::abs(diff) > 1) {
                return Result<bool>::ok(false);
            }
        }

        // Check filesystem type codes match
        if (ap.filesystem != ep.filesystem) {
            return Result<bool>::ok(false);
        }
    }

    return Result<bool>::ok(true);
}

// =============================================================================
// get_partition_by_label
// =============================================================================

Result<std::string> PartitionManager::get_partition_by_label(
    const std::string& label)
{
    // ---- Strategy 1: /dev/disk/by-partlabel/<label> -------------------------
    std::string partlabel_path = "/dev/disk/by-partlabel/" + label;

    {
        struct stat st;
        if (stat(partlabel_path.c_str(), &st) == 0) {
            // Symlink exists — resolve it to the canonical device node
            char link_target[PATH_MAX];
            ssize_t len = readlink(partlabel_path.c_str(),
                                   link_target, sizeof(link_target) - 1);
            if (len != -1) {
                link_target[len] = '\0';
                std::string target(link_target);

                // readlink returns a relative name like "../../mmcblk0p2".
                // Prepend "/dev/" if the result is not already absolute.
                if (!target.empty() && target[0] != '/') {
                    // Resolve relative to the by-partlabel directory
                    std::string dir = "/dev/disk/by-partlabel/";
                    // Build absolute path: strip trailing "../" components
                    // Simplest approach: resolve using realpath()
                    char resolved[PATH_MAX];
                    if (realpath(partlabel_path.c_str(), resolved) != nullptr) {
                        return Result<std::string>::ok(std::string(resolved));
                    }
                    // Fallback: return the partlabel symlink itself
                    return Result<std::string>::ok(partlabel_path);
                }
                return Result<std::string>::ok(target);
            }
        }
    }

    // ---- Strategy 2: Try blkid as a quicker fallback ------------------------
    {
        std::vector<std::string> args = {
            "blkid", "-t", "PARTLABEL=" + label, "-o", "device"
        };
        auto result = proc_runner_->run(args, std::chrono::seconds(5));
        if (result.is_ok() && result.value().exit_code == 0) {
            std::string dev = result.value().stdout_data;
            // Trim trailing whitespace / newline
            while (!dev.empty() && (dev.back() == '\n' || dev.back() == '\r'
                                    || dev.back() == ' ')) {
                dev.pop_back();
            }
            if (!dev.empty()) {
                return Result<std::string>::ok(dev);
            }
        }
    }

    // ---- Strategy 3: Scan block devices via sgdisk --------------------------
    {
        std::vector<std::string> devices = enumerate_block_devices();
        for (const auto& dev : devices) {
            std::vector<std::string> args = {"sgdisk", "--print", dev};
            auto result = proc_runner_->run(args, std::chrono::seconds(10));

            if (!result.is_ok() || result.value().exit_code != 0) {
                // Skip devices without a valid GPT (or other errors)
                continue;
            }

            // Parse the output and look for a partition with the given name
            auto parse_result = parse_sgdisk_output(result.value().stdout_data);
            if (!parse_result.is_ok()) {
                continue;
            }

            for (size_t i = 0; i < parse_result.value().size(); ++i) {
                const auto& spec = parse_result.value()[i];
                std::string part_name = spec.label.empty() ? spec.name : spec.label;
                if (part_name == label) {
                    // Found it — construct the partition device path
                    int part_num = static_cast<int>(i) + 1;
                    std::string part_path = dev
                        + partition_suffix(dev)
                        + std::to_string(part_num);
                    return Result<std::string>::ok(part_path);
                }
            }
        }
    }

    // ---- Not found ----------------------------------------------------------
    return Result<std::string>::err(
        InstallerError::make(
            ErrorCode::PARTITION_NOT_FOUND,
            "Partition Not Found",
            "Partition with label '" + label + "' was not found on any block device.",
            "Checked /dev/disk/by-partlabel, blkid, and sgdisk scans of "
                "/sys/block devices.",
            false));
}

// =============================================================================
// wait_for_partitions
// =============================================================================

Result<void> PartitionManager::wait_for_partitions(
    const std::string& device_path,
    int timeout_sec)
{
    if (timeout_sec <= 0) {
        timeout_sec = 10;
    }

    // ---- Strategy 1: udevadm settle -----------------------------------------
    {
        std::vector<std::string> args = {
            "udevadm", "settle",
            "--timeout=" + std::to_string(timeout_sec)
        };
        auto result = proc_runner_->run(
            args,
            std::chrono::seconds(timeout_sec + 5));

        if (result.is_ok() && result.value().exit_code == 0) {
            return Result<void>::ok();
        }
        // Fall through to polling on failure
    }

    // ---- Strategy 2: Poll for partition device nodes ------------------------
    {
        std::string suffix = partition_suffix(device_path);
        int max_attempts = timeout_sec * 10; // 100 ms intervals

        for (int attempt = 0; attempt < max_attempts; ++attempt) {
            // Check for the existence of at least one partition (e.g. p1 / 1)
            std::string part_path = device_path + suffix + "1";
            struct stat st;
            if (stat(part_path.c_str(), &st) == 0 && S_ISBLK(st.st_mode)) {
                return Result<void>::ok();
            }
            usleep(100000); // 100 ms
        }
    }

    // ---- Timeout ------------------------------------------------------------
    return Result<void>::err(
        InstallerError::make(
            ErrorCode::INTERNAL_TIMEOUT,
            "Timeout",
            "Timed out waiting for partition device nodes to appear on "
                + device_path,
            "Neither udevadm settle succeeded nor did partition nodes "
                "appear within " + std::to_string(timeout_sec) + " seconds.",
            true));
}

// =============================================================================
// parse_sgdisk_output
// =============================================================================

Result<std::vector<PartitionSpec>> PartitionManager::parse_sgdisk_output(
    const std::string& output)
{
    std::vector<PartitionSpec> partitions;

    std::istringstream stream(output);
    std::string line;
    bool in_partition_table = false;

    while (std::getline(stream, line)) {
        // Trim leading whitespace
        size_t start = 0;
        while (start < line.size()
               && (line[start] == ' ' || line[start] == '\t')) {
            ++start;
        }

        if (start >= line.size()) {
            continue; // empty line
        }

        std::string trimmed = line.substr(start);

        // Look for the partition-table header line.
        //
        // Typical format:
        //   Number  Start (sector)    End (sector)  Size       Code  Name
        //
        // Some older versions use a slightly different layout, so we
        // match on "Number" at column 0 and the presence of "Code" and
        // "Name" somewhere in the line.
        if (!in_partition_table) {
            if (trimmed.compare(0, 6, "Number") == 0
                && trimmed.find("Code") != std::string::npos
                && trimmed.find("Name") != std::string::npos) {
                in_partition_table = true;
            }
            continue;
        }

        // After the header, every non-empty line is a partition entry
        // until we hit a blank line or end-of-file.

        // Split the line by whitespace
        std::vector<std::string> fields;
        {
            std::istringstream line_stream(trimmed);
            std::string field;
            while (line_stream >> field) {
                fields.push_back(field);
            }
        }

        // We need at least 6 fields:
        //   [0] Number  [1] Start  [2] End  [3] Size  [4] Unit  [5] Code  [6..] Name
        //
        // The Size field is split into two tokens by the istringstream
        // (e.g. "256.0" and "MiB"), so we actually need >= 7 fields.
        //
        // Some versions of sgdisk may output the size as a single
        // column with a space in between.  We try both interpretations.

        if (fields.size() < 5) {
            continue; // skip malformed lines
        }

        // --- Parse partition number ---
        int part_num = 0;
        try {
            part_num = std::stoi(fields[0]);
        } catch (const std::exception&) {
            continue;
        }

        // --- Determine where the size, unit, code, and name fields lie ---
        // Heuristic: look for a field that is a 4-hex-digit code (the type
        // code).  Everything after it is the partition name.

        size_t code_idx = std::string::npos;
        for (size_t i = 1; i < fields.size(); ++i) {
            const auto& f = fields[i];
            if (f.size() == 4
                && std::all_of(f.begin(), f.end(), [](char c) {
                       return std::isxdigit(static_cast<unsigned char>(c));
                   })) {
                code_idx = i;
                break;
            }
        }

        if (code_idx == std::string::npos || code_idx < 4) {
            continue; // cannot locate type code
        }

        // Fields layout:
        //   [0] Number
        //   [1] Start sector
        //   [2] End sector
        //   [3..code_idx-1] Size value + optional unit tokens
        //   [code_idx] Type code
        //   [code_idx+1..] Partition name

        // --- Parse size ---
        uint64_t size_mib = 0;

        // The tokens just before code_idx are: size_value, [unit]
        // Typically: fields[3] = "256.0", fields[4] = "MiB", fields[5] = code
        // So (code_idx - 1) is the unit, (code_idx - 2) is the value.
        if (code_idx >= 3) {
            std::string size_val = fields[code_idx - 2];
            std::string size_unit = (code_idx >= 4) ? fields[code_idx - 1] : "MiB";

            // Validate that size_val looks like a number
            bool looks_numeric = !size_val.empty()
                && std::all_of(size_val.begin(), size_val.end(), [](char c) {
                       return std::isdigit(static_cast<unsigned char>(c)) || c == '.';
                   });

            if (looks_numeric) {
                size_mib = parse_size_to_mib(size_val, size_unit);
            } else {
                // Layout might be different — try (code_idx - 1) as the
                // single size token (e.g. "256.0MiB" without a space).
                std::string combined = fields[code_idx - 1];
                // Split numeric prefix from alphabetic suffix
                size_t split = 0;
                while (split < combined.size()
                       && (std::isdigit(static_cast<unsigned char>(combined[split]))
                           || combined[split] == '.')) {
                    ++split;
                }
                if (split > 0 && split < combined.size()) {
                    std::string num_part = combined.substr(0, split);
                    std::string unit_part = combined.substr(split);
                    size_mib = parse_size_to_mib(num_part, unit_part);
                }
            }
        }

        // --- Parse type code ---
        std::string type_code = fields[code_idx];
        FilesystemType fs_type = sgdisk_code_to_fs_type(type_code);

        // --- Parse partition name ---
        std::string part_name;
        for (size_t i = code_idx + 1; i < fields.size(); ++i) {
            if (!part_name.empty()) {
                part_name += " ";
            }
            part_name += fields[i];
        }

        // --- Build PartitionSpec ---
        PartitionSpec spec;
        spec.name = part_name;
        spec.label = part_name;   // In GPT there is no separate label; the
                                  // partition name IS the PARTLABEL.
        spec.size_mib = size_mib;
        spec.filesystem = fs_type;

        partitions.push_back(std::move(spec));
    }

    if (!in_partition_table) {
        return Result<std::vector<PartitionSpec>>::err(
            InstallerError::make(
                ErrorCode::PARTITION_NOT_FOUND,
                "Partition Not Found",
                "No valid GPT partition table found in sgdisk output.",
                "Could not locate the 'Number ... Code ... Name' header line.",
                true));
    }

    return Result<std::vector<PartitionSpec>>::ok(std::move(partitions));
}

} // namespace installer
