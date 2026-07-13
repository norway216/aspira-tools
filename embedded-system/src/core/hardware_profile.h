/**
 * @file hardware_profile.h
 * @brief HardwareProfileManager — loads profiles from config, matches against
 *        current hardware, and provides partition layout / bootloader config.
 *
 * Hardware profiles describe known target platforms (Raspberry Pi 4, generic
 * x86_64, etc.) and are defined in a YAML-like config file.  At runtime the
 * manager detects the current system and selects the best-matching profile.
 */

#ifndef INSTALLER_CORE_HARDWARE_PROFILE_H
#define INSTALLER_CORE_HARDWARE_PROFILE_H

#include "installer/core/types.h"
#include "installer/core/result.h"

#include <map>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

namespace installer {

/**
 * A single hardware profile definition.
 *
 * Each profile describes a known target platform: architecture requirements,
 * device paths, partition layout, bootloader, and other platform-specific
 * metadata.
 */
struct HardwareProfile {
    std::string name;                       // e.g. "raspberry_pi_4"
    std::string display_name;               // e.g. "Raspberry Pi 4"
    std::string architecture;               // e.g. "aarch64", "armv7l", "x86_64"
    uint64_t min_disk_size_bytes = 0;       // Minimum storage required
    std::vector<std::string> device_candidates;   // e.g. ["/dev/mmcblk0"]
    std::string partition_layout;           // Reference to a PartitionLayout name
    std::string bootloader_type;            // "u-boot", "grub", "barebox"
    std::string boot_device;                // e.g. "/dev/mmcblk0p1"
    std::vector<std::string> required_kernel_modules;
    std::map<std::string, std::string> extra;     // Extension key-value pairs

    /** Device-tree compatible string (optional, for DT-based platforms). */
    std::optional<std::string> dt_compatible;
};

/**
 * Hardware profile management.
 *
 * Loads hardware profiles from a YAML-like config file (or built-in defaults),
 * matches the current system against those profiles, and provides access to
 * the associated partition layout and bootloader configuration.
 *
 * Thread-safe: all public methods lock the internal mutex.
 */
class HardwareProfileManager {
public:
    HardwareProfileManager();
    ~HardwareProfileManager() = default;

    // ---- Loading -----------------------------------------------------------

    /**
     * Load profiles and partition layouts from a config file at @p config_path.
     * If the file cannot be opened, built-in default profiles are loaded instead.
     */
    Result<void> load_profiles(const std::string& config_path);

    /**
     * Load profiles and partition layouts from an in-memory YAML string.
     */
    Result<void> load_profiles_from_string(const std::string& yaml_content);

    /**
     * Add a single profile programmatically (e.g. at runtime).
     */
    void add_profile(const HardwareProfile& profile);

    // ---- Matching ----------------------------------------------------------

    /**
     * Detect the current system (architecture, disk size, device paths) and
     * return the best-matching profile.
     */
    Result<HardwareProfile> match_current_hardware() const;

    /**
     * Match explicitly provided hardware parameters against the loaded profiles.
     * Returns the highest-scoring match, or an error if nothing matches.
     */
    Result<HardwareProfile> match_hardware(const std::string& architecture,
                                           uint64_t disk_size_bytes,
                                           const std::vector<std::string>& device_paths) const;

    // ---- Lookup ------------------------------------------------------------

    /** Retrieve a profile by its unique name. */
    Result<HardwareProfile> get_profile(const std::string& name) const;

    /** Return a copy of all currently loaded profiles. */
    std::vector<HardwareProfile> get_all_profiles() const;

    /**
     * Look up the PartitionLayout referenced by @p profile's partition_layout
     * field.  Returns an error if the layout name is not found.
     */
    Result<PartitionLayout> get_partition_layout(const HardwareProfile& profile) const;

    // ---- Bootloader --------------------------------------------------------

    /** Bootloader configuration derived from a profile. */
    struct BootloaderConfig {
        std::string type;                               // "u-boot", "grub", ...
        std::string boot_device;
        std::map<std::string, std::string> env_vars;    // Platform env vars
    };

    /**
     * Build bootloader configuration from @p profile.  Adds sensible default
     * environment variables depending on the bootloader type.
     */
    Result<BootloaderConfig> get_bootloader_config(const HardwareProfile& profile) const;

    // ---- System detection (static helpers) ---------------------------------

    /** Return the machine architecture string (calls uname(2)). */
    static std::string detect_architecture();

    /**
     * Read the device-tree compatible string from
     * /proc/device-tree/compatible.  Returns an empty string on non-DT
     * systems or when the file is unavailable.
     */
    static std::string detect_dt_compatible();

private:
    // ---- YAML parsing ------------------------------------------------------

    /**
     * Parse a YAML string that contains a top-level "profiles" list and an
     * optional "partition_layouts" map.  Returns the vector of parsed
     * HardwareProfile objects.
     *
     * Internally delegates to file-scope helpers in the .cpp translation unit.
     */
    Result<std::vector<HardwareProfile>> parse_yaml_profiles(const std::string& yaml);

    // ---- Built-in defaults ------------------------------------------------

    /**
     * Populate profiles_ and layout_templates_ with hard-coded fallback
     * profiles (Raspberry Pi 4, generic x86_64) and their associated
     * partition layouts.  Called when no config file is available.
     */
    void load_builtin_profiles();

    // ---- Static helpers ----------------------------------------------------

    /** Determine size (bytes) of the system disk mounted at "/". */
    static uint64_t get_system_disk_size();

    /** Enumerate block device paths present in /sys/block. */
    static std::vector<std::string> get_system_devices();

    // ---- State -------------------------------------------------------------

    std::vector<HardwareProfile> profiles_;
    std::map<std::string, PartitionLayout> layout_templates_;
    mutable std::mutex mutex_;
};

} // namespace installer

#endif // INSTALLER_CORE_HARDWARE_PROFILE_H
