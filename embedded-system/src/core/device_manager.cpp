/**
 * @file device_manager.cpp
 * @brief DeviceManager implementation — sysfs-based block device enumeration.
 *
 * All device discovery is performed by reading /sys/block and /proc/mounts
 * directly; no dependency on libudev.
 */

#include "device_manager.h"

#include "installer/IDeviceManager.h"
#include "installer/core/result.h"
#include "installer/core/types.h"

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <cstring>
#include <dirent.h>
#include <fcntl.h>
#include <fstream>
#include <functional>
#include <memory>
#include <mutex>
#include <sstream>
#include <string>
#include <sys/stat.h>
#include <sys/sysmacros.h>
#include <thread>
#include <unistd.h>
#include <vector>

namespace installer {

// =========================================================================
//  Anonymous helpers — file-static, not exposed in the header
// =========================================================================
namespace {

// ---- Error factory ---------------------------------------------------------

InstallerError make_error(const char* code, const std::string& msg) {
    return InstallerError::make(code, "Device Error", msg, msg, false);
}

// ---- String utilities ------------------------------------------------------

std::string trim(const std::string& s) {
    if (s.empty()) return s;
    size_t start = 0;
    while (start < s.size() && (s[start] == ' '  || s[start] == '\t' ||
                                s[start] == '\n' || s[start] == '\r')) {
        ++start;
    }
    size_t end = s.size();
    while (end > start && (s[end - 1] == ' '  || s[end - 1] == '\t' ||
                           s[end - 1] == '\n' || s[end - 1] == '\r')) {
        --end;
    }
    return s.substr(start, end - start);
}

uint64_t parse_uint64(const std::string& s) {
    if (s.empty()) return 0;
    const char* str = s.c_str();
    char* end = nullptr;
    unsigned long long val = ::strtoull(str, &end, 10);
    if (end == str) return 0;          // no conversion
    return static_cast<uint64_t>(val);
}

// ---- File-system helpers ---------------------------------------------------

/**
 * Read the first line of @p path, trim whitespace, return it.
 * Returns an empty string on any error (missing file, permission, etc.).
 */
std::string read_sysfs_file(const std::string& path) {
    std::ifstream ifs(path);
    if (!ifs.is_open()) return {};
    std::string line;
    if (!std::getline(ifs, line)) return {};
    return trim(line);
}

bool dir_exists(const std::string& path) {
    struct stat st;
    if (::stat(path.c_str(), &st) != 0) return false;
    return S_ISDIR(st.st_mode);
}

/**
 * Resolve a path that may contain symlinks to its canonical absolute path.
 * Returns the original path on failure.
 */
std::string resolve_symlink(const std::string& path) {
    char* resolved = ::realpath(path.c_str(), nullptr);
    if (!resolved) return path;
    std::string result(resolved);
    ::free(resolved);
    return result;
}

dev_t get_device_id(const std::string& path) {
    struct stat st;
    if (::stat(path.c_str(), &st) != 0) return 0;
    return st.st_rdev;   // device ID for block/char special files
}

// ---- Device name parsing ---------------------------------------------------

/**
 * Strip "/dev/" prefix and any partition suffix.
 *
 * Examples:
 *   /dev/sda      -> sda
 *   /dev/sda1     -> sda
 *   /dev/mmcblk0  -> mmcblk0
 *   /dev/mmcblk0p1 -> mmcblk0
 *   /dev/nvme0n1  -> nvme0n1
 *   /dev/nvme0n1p1 -> nvme0n1
 */
std::string base_device_name(const std::string& device_path) {
    std::string name = device_path;

    // Strip "/dev/" prefix
    const std::string prefix = "/dev/";
    if (name.size() > prefix.size() &&
        name.compare(0, prefix.size(), prefix) == 0) {
        name = name.substr(prefix.size());
    }

    if (name.empty()) return name;

    // mmcblk* and nvme* use 'p' as partition separator
    if (name.size() >= 6 && name.compare(0, 6, "mmcblk") == 0) {
        auto pos = name.rfind('p');
        if (pos != std::string::npos && pos + 1 < name.size() &&
            std::isdigit(static_cast<unsigned char>(name[pos + 1]))) {
            return name.substr(0, pos);
        }
        return name;
    }

    if (name.size() >= 4 && name.compare(0, 4, "nvme") == 0) {
        auto pos = name.rfind('p');
        if (pos != std::string::npos && pos + 1 < name.size() &&
            std::isdigit(static_cast<unsigned char>(name[pos + 1]))) {
            return name.substr(0, pos);
        }
        return name;
    }

    // sd*, hd*, vd* etc. — partition suffix is trailing digits
    while (!name.empty() && std::isdigit(static_cast<unsigned char>(name.back()))) {
        name.pop_back();
    }
    return name;
}

// ---- Device type classification --------------------------------------------

DeviceType determine_device_type(const std::string& name,
                                 bool removable,
                                 const std::string& /*type_file_path*/) {
    // eMMC
    if (name.size() >= 6 && name.compare(0, 6, "mmcblk") == 0) {
        return DeviceType::eMMC;
    }

    // NVMe
    if (name.size() >= 4 && name.compare(0, 4, "nvme") == 0) {
        return DeviceType::NVMe;
    }

    // sd* devices — distinguish SATA from USB by the removable flag
    if (name.size() >= 2 && name.compare(0, 2, "sd") == 0) {
        return removable ? DeviceType::USB : DeviceType::SATA;
    }

    return DeviceType::Unknown;
}

// ---- System-disk detection -------------------------------------------------

/**
 * Parse /proc/mounts, locate the entry whose mount-point is "/",
 * resolve symlinks, and return the canonical device path.
 * Returns an empty string when the root device cannot be determined.
 */
std::string get_system_disk_from_mounts() {
    std::ifstream mounts("/proc/mounts");
    if (!mounts.is_open()) return {};

    std::string line;
    while (std::getline(mounts, line)) {
        // Format: device mountpoint fstype options ...
        std::istringstream iss(line);
        std::string device, mount_point;
        if (!(iss >> device >> mount_point)) continue;

        if (mount_point == "/") {
            // Resolve the device node — /proc/mounts may report a symlink
            // (e.g. /dev/root, /dev/disk/by-uuid/...)
            std::string resolved = resolve_symlink(device);
            return resolved;
        }
    }
    return {};
}

/**
 * Check whether @p device_path is the system disk by comparing
 * device numbers obtained via stat().
 */
bool check_is_system_disk_by_devid(const std::string& device_path) {
    std::string sys_dev = get_system_disk_from_mounts();
    if (sys_dev.empty()) return false;

    dev_t sys_id  = get_device_id(sys_dev);
    dev_t cand_id = get_device_id(device_path);

    if (sys_id == 0 || cand_id == 0) return false;
    return sys_id == cand_id;
}

// ---- Directory iteration guard ---------------------------------------------

/**
 * RAII wrapper around DIR* from opendir().
 */
class ScopedDir {
public:
    explicit ScopedDir(const std::string& path) : dir_(::opendir(path.c_str())) {}
    ~ScopedDir() { if (dir_) ::closedir(dir_); }

    ScopedDir(const ScopedDir&) = delete;
    ScopedDir& operator=(const ScopedDir&) = delete;

    DIR* get() const { return dir_; }
    explicit operator bool() const { return dir_ != nullptr; }

private:
    DIR* dir_;
};

} // anonymous namespace

// =========================================================================
//  Construction / Destruction
// =========================================================================

DeviceManager::DeviceManager() = default;

DeviceManager::~DeviceManager() {
    // Ensure the monitor thread is stopped before destruction.
    if (monitoring_active_.load(std::memory_order_acquire)) {
        monitoring_active_.store(false, std::memory_order_release);
        if (monitor_thread_.joinable()) {
            monitor_thread_.join();
        }
    }
}

// =========================================================================
//  IDeviceManager — scan()
// =========================================================================

std::vector<BlockDeviceInfo> DeviceManager::scan() {
    auto result = scan_devices();
    if (result.is_ok()) {
        return result.take_value();
    }
    return {};
}

// =========================================================================
//  scan_devices()
// =========================================================================

Result<std::vector<BlockDeviceInfo>> DeviceManager::scan_devices() {
    ScopedDir dir("/sys/block");
    if (!dir) {
        return Result<std::vector<BlockDeviceInfo>>::err(
            make_error(ErrorCode::DEVICE_IO_ERROR,
                       "Cannot open /sys/block: " + std::string(::strerror(errno))));
    }

    // Resolve system disk once for the whole scan.
    std::string sys_disk = get_system_disk_from_mounts();

    std::vector<BlockDeviceInfo> devices;
    struct dirent* entry = nullptr;

    while ((entry = ::readdir(dir.get())) != nullptr) {
        std::string name(entry->d_name);

        // Skip . and ..
        if (name == "." || name == "..") continue;

        // Skip entries without a "device" subdirectory (virtual devices:
        // loop, ram, dm-*, zram, etc.)
        std::string device_dir = "/sys/block/" + name + "/device";
        if (!dir_exists(device_dir)) continue;

        BlockDeviceInfo info = build_device_info(name);
        if (info.size_bytes == 0 && info.path.empty()) {
            // Completely unresolvable — skip
            continue;
        }

        // Mark system disk via device-number comparison.
        if (!sys_disk.empty()) {
            // Resolve /dev/<name> to compare with sys_disk
            std::string cand_path = "/dev/" + name;
            dev_t cand_id = get_device_id(cand_path);

            // Also check whether the system disk is a partition *on* this
            // device: resolve the base device of sys_disk and compare.
            std::string sys_base_path = "/dev/" + base_device_name(sys_disk);
            dev_t sys_base_id = get_device_id(sys_base_path);

            if (cand_id != 0 && sys_base_id != 0 && cand_id == sys_base_id) {
                info.is_system_disk = true;
            }
        }

        // Mark installer media.
        info.is_installer_media = is_installer_media("/dev/" + name);

        devices.push_back(std::move(info));
    }

    return Result<std::vector<BlockDeviceInfo>>::ok(std::move(devices));
}

// =========================================================================
//  build_device_info() — single-device sysfs reader
// =========================================================================

BlockDeviceInfo DeviceManager::build_device_info(const std::string& name) {
    BlockDeviceInfo info;
    info.path = "/dev/" + name;

    std::string base = "/sys/block/" + name;

    // ---- size (512-byte sectors -> bytes) ----
    std::string size_str = read_sysfs_file(base + "/size");
    uint64_t sectors = parse_uint64(size_str);
    info.size_bytes = sectors * 512;

    // ---- model ----
    // Prefer device/model; fall back to device/name or the kernel name.
    info.model = read_sysfs_file(base + "/device/model");
    if (info.model.empty()) {
        info.model = read_sysfs_file(base + "/device/name");
    }
    if (info.model.empty()) {
        info.model = name;   // last resort
    }

    // ---- serial ----
    info.serial = read_sysfs_file(base + "/device/serial");

    // ---- removable ----
    std::string rem_str = read_sysfs_file(base + "/removable");
    info.removable = (rem_str == "1");

    // ---- read-only ----
    std::string ro_str = read_sysfs_file(base + "/ro");
    info.read_only = (ro_str == "1");

    // ---- sector sizes ----
    std::string log_sec = read_sysfs_file(base + "/queue/logical_block_size");
    if (!log_sec.empty()) {
        info.logical_sector_size = static_cast<uint32_t>(parse_uint64(log_sec));
    }

    std::string phys_sec = read_sysfs_file(base + "/queue/physical_block_size");
    if (!phys_sec.empty()) {
        info.physical_sector_size = static_cast<uint32_t>(parse_uint64(phys_sec));
    }

    // ---- device type ----
    std::string type_file = base + "/device/type";
    info.type = determine_device_type(name, info.removable, type_file);

    return info;
}

// =========================================================================
//  get_device_info()
// =========================================================================

Result<BlockDeviceInfo> DeviceManager::get_device_info(const std::string& device_path) {
    if (device_path.empty()) {
        return Result<BlockDeviceInfo>::err(
            make_error(ErrorCode::DEVICE_NOT_FOUND, "Empty device path"));
    }

    // Extract base device name (strip /dev/ and partition suffix).
    std::string name = base_device_name(device_path);

    // Verify the sysfs entry exists.
    std::string sysfs_dir = "/sys/block/" + name;
    if (!dir_exists(sysfs_dir)) {
        return Result<BlockDeviceInfo>::err(
            make_error(ErrorCode::DEVICE_NOT_FOUND,
                       "Device not found in sysfs: " + name));
    }

    BlockDeviceInfo info = build_device_info(name);

    // Override path with the caller-supplied path (may include partition).
    info.path = device_path;

    // Mark system disk.
    info.is_system_disk = check_is_system_disk_by_devid(device_path);
    if (!info.is_system_disk) {
        // Also check the base device.
        std::string base_path = "/dev/" + name;
        info.is_system_disk = check_is_system_disk_by_devid(base_path);
    }

    // Mark installer media.
    info.is_installer_media = is_installer_media(device_path);

    return Result<BlockDeviceInfo>::ok(std::move(info));
}

// =========================================================================
//  is_safe_target()
// =========================================================================

bool DeviceManager::is_safe_target(const std::string& device_path) {
    if (device_path.empty()) return false;

    // ---- 1. Reject the system disk ----
    std::string sys_disk = get_system_disk_from_mounts();
    if (!sys_disk.empty()) {
        std::string cand_base = base_device_name(device_path);
        std::string sys_base  = base_device_name(sys_disk);

        if (cand_base == sys_base) {
            return false;
        }

        // Also compare via device numbers for robustness.
        // Check if the candidate base device matches the system-disk base device.
        std::string cand_dev = "/dev/" + cand_base;
        std::string sys_dev  = "/dev/" + sys_base;
        dev_t cand_id = get_device_id(cand_dev);
        dev_t sys_id  = get_device_id(sys_dev);
        if (cand_id != 0 && sys_id != 0 && cand_id == sys_id) {
            return false;
        }
    }

    // ---- 2. Get device info for remaining checks ----
    auto result = get_device_info(device_path);
    if (result.is_err()) {
        // Can't determine — assume unsafe.
        return false;
    }
    const auto& info = result.value();

    // ---- 3. Reject read-only devices ----
    if (info.read_only) return false;

    // ---- 4. Reject installer media ----
    if (info.is_installer_media) return false;

    // ---- 5. Require sufficient size ----
    if (info.size_bytes == 0) return false;

    return true;
}

// =========================================================================
//  get_system_disk()
// =========================================================================

std::string DeviceManager::get_system_disk() {
    return get_system_disk_from_mounts();
}

// =========================================================================
//  wait_for_device()
// =========================================================================

Result<void> DeviceManager::wait_for_device(const std::string& device_path,
                                            int timeout_ms) {
    if (device_path.empty()) {
        return Result<void>::err(
            make_error(ErrorCode::DEVICE_NOT_FOUND, "Empty device path"));
    }

    constexpr int kPollIntervalMs = 100;
    auto start = std::chrono::steady_clock::now();

    while (true) {
        struct stat st;
        if (::stat(device_path.c_str(), &st) == 0 && S_ISBLK(st.st_mode)) {
            return Result<void>::ok();
        }

        auto now = std::chrono::steady_clock::now();
        auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                           now - start)
                           .count();

        if (elapsed >= timeout_ms) {
            return Result<void>::err(
                make_error(ErrorCode::DEVICE_HOTPLUG_TIMEOUT,
                           "Device " + device_path +
                               " did not appear within " +
                               std::to_string(timeout_ms) + " ms"));
        }

        std::this_thread::sleep_for(
            std::chrono::milliseconds(kPollIntervalMs));
    }
}

// =========================================================================
//  Installer-media helpers
// =========================================================================

void DeviceManager::set_installer_media_device(const std::string& device_path) {
    std::lock_guard<std::mutex> lock(mutex_);
    installer_media_device_ = device_path;
}

bool DeviceManager::is_installer_media(const std::string& device_path) const {
    std::lock_guard<std::mutex> lock(mutex_);
    if (installer_media_device_.empty()) return false;

    std::string cand_base  = base_device_name(device_path);
    std::string media_base = base_device_name(installer_media_device_);

    return cand_base == media_base;
}

// =========================================================================
//  Hotplug monitor
// =========================================================================

void DeviceManager::monitor_loop() {
    // Snapshot the current device list.
    {
        std::lock_guard<std::mutex> lock(mutex_);
        previous_device_names_.clear();
    }

    // Build the initial baseline.
    {
        ScopedDir dir("/sys/block");
        if (dir) {
            struct dirent* entry = nullptr;
            std::lock_guard<std::mutex> lock(mutex_);
            while ((entry = ::readdir(dir.get())) != nullptr) {
                std::string name(entry->d_name);
                if (name == "." || name == "..") continue;
                std::string device_dir = "/sys/block/" + name + "/device";
                if (!dir_exists(device_dir)) continue;
                previous_device_names_.push_back(name);
            }
            std::sort(previous_device_names_.begin(),
                      previous_device_names_.end());
        }
    }

    HotplugCallback cb;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        cb = hotplug_callback_;
    }

    // Polling loop.
    while (monitoring_active_.load(std::memory_order_acquire)) {
        std::this_thread::sleep_for(std::chrono::seconds(1));

        // Collect current device names.
        std::vector<std::string> current_names;
        {
            ScopedDir dir("/sys/block");
            if (dir) {
                struct dirent* entry = nullptr;
                while ((entry = ::readdir(dir.get())) != nullptr) {
                    std::string name(entry->d_name);
                    if (name == "." || name == "..") continue;
                    std::string device_dir =
                        "/sys/block/" + name + "/device";
                    if (!dir_exists(device_dir)) continue;
                    current_names.push_back(name);
                }
            }
        }
        std::sort(current_names.begin(), current_names.end());

        // Diff against previous snapshot.
        std::vector<std::string> added;
        std::vector<std::string> removed;

        {
            std::lock_guard<std::mutex> lock(mutex_);

            // Added: in current but not in previous.
            std::set_difference(
                current_names.begin(), current_names.end(),
                previous_device_names_.begin(),
                previous_device_names_.end(),
                std::back_inserter(added));

            // Removed: in previous but not in current.
            std::set_difference(
                previous_device_names_.begin(),
                previous_device_names_.end(),
                current_names.begin(), current_names.end(),
                std::back_inserter(removed));

            previous_device_names_ = std::move(current_names);
        }

        // Fire callbacks outside the lock.
        if (cb) {
            for (const auto& name : added) {
                BlockDeviceInfo info = build_device_info(name);
                // Mark installer media.
                info.is_installer_media =
                    is_installer_media("/dev/" + name);
                cb(info, /*added=*/true);
            }
            for (const auto& name : removed) {
                BlockDeviceInfo info;
                info.path = "/dev/" + name;
                info.model = name;
                cb(info, /*added=*/false);
            }
        }
    }
}

int DeviceManager::start_hotplug_monitor(HotplugCallback callback) {
    std::lock_guard<std::mutex> lock(mutex_);

    if (monitoring_active_.load(std::memory_order_acquire)) {
        return -1;   // already running
    }

    hotplug_callback_ = std::move(callback);
    monitoring_active_.store(true, std::memory_order_release);

    int handle = ++monitor_handle_counter_;

    monitor_thread_ = std::thread(&DeviceManager::monitor_loop, this);

    return handle;
}

Result<void> DeviceManager::stop_hotplug_monitor(int handle) {
    {
        std::lock_guard<std::mutex> lock(mutex_);

        if (!monitoring_active_.load(std::memory_order_acquire)) {
            return Result<void>::err(
                make_error(ErrorCode::INTERNAL_INVALID_STATE,
                           "Hotplug monitor is not active"));
        }

        // The handle is a simple cookie; for a single-monitor design any
        // valid handle is accepted.
        (void)handle;

        monitoring_active_.store(false, std::memory_order_release);
    }
    // Lock released here — joining with the lock held would deadlock
    // because the monitor loop also acquires mutex_.

    if (monitor_thread_.joinable()) {
        try {
            monitor_thread_.join();
        } catch (const std::system_error& e) {
            return Result<void>::err(
                make_error(ErrorCode::INTERNAL_ERROR,
                           std::string("Failed to join monitor thread: ") +
                               e.what()));
        }
    }

    return Result<void>::ok();
}

} // namespace installer
