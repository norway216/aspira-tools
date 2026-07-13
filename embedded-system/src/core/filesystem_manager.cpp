/**
 * @file filesystem_manager.cpp
 * @brief Implementation of FilesystemManager using system tools via
 *        ProcessRunner and direct mount/umount syscalls.
 *
 * Concurrency: all public methods acquire a mutex so the manager is safe
 * to use from multiple threads.  The underlying ProcessRunner is expected
 * to be thread-safe as well (it forks independent child processes).
 */

#include "filesystem_manager.h"

#include "installer/platform/iprocess_runner.h"
#include "common/file_utils.h"

#include <algorithm>
#include <cctype>
#include <cerrno>
#include <cstring>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

#include <fcntl.h>
#include <sys/mount.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

namespace installer {

// =============================================================================
// Internal helpers
// =============================================================================

namespace {

/**
 * Build an InstallerError from the current errno value.
 */
InstallerError make_errno_error(const std::string& error_code,
                                const std::string& title,
                                const std::string& user_msg,
                                const std::string& context,
                                bool retryable = true) {
    const char* sys_msg = ::strerror(errno);
    std::string tech = context + " (errno=" + std::to_string(errno) +
                       ": " + sys_msg + ")";
    return InstallerError::make(error_code, title, user_msg, tech, retryable, false);
}

/**
 * Build an InstallerError from an arbitrary string message (no errno).
 */
InstallerError make_error(const std::string& error_code,
                          const std::string& title,
                          const std::string& user_msg,
                          const std::string& tech_msg,
                          bool retryable = false) {
    return InstallerError::make(error_code, title, user_msg, tech_msg, retryable, false);
}

/**
 * Build a ProcessResult error from a failed subprocess invocation.
 */
InstallerError make_process_error(const std::string& error_code,
                                  const std::string& title,
                                  const std::string& user_msg,
                                  const ProcessResult& proc_result,
                                  bool retryable = true) {
    std::string tech = user_msg + " (exit_code=" +
                       std::to_string(proc_result.exit_code) + ")";
    if (!proc_result.stderr_data.empty()) {
        tech += " stderr: " + proc_result.stderr_data;
    }
    return InstallerError::make(error_code, title, user_msg, tech, retryable, false);
}

/**
 * Recursively create a directory path (equivalent to mkdir -p).
 *
 * This is a local fallback used when the file_utils.h ensure_directory()
 * is not directly available or for standalone use.  In practice we delegate
 * to ensure_directory() which handles partial paths correctly.
 */
Result<void> create_directory_recursive(const std::string& path) {
    return ensure_directory(path);
}

/**
 * Convert a VFAT label to its canonical form:
 *  - Truncated to 11 characters
 *  - Uppercased
 *  - Invalid characters replaced (though mkfs.vfat does this anyway)
 */
std::string sanitize_vfat_label(const std::string& label) {
    std::string out;
    out.reserve(11);
    for (char c : label) {
        if (out.size() >= 11) break;
        // VFAT label: only allow alphanumeric, underscore, and a few others.
        // We uppercased the input and let mkfs.vfat reject truly invalid chars.
        out.push_back(static_cast<char>(std::toupper(static_cast<unsigned char>(c))));
    }
    return out;
}

/**
 * Determine the parent directory of a path.
 *
 * Examples:
 *   /mnt/point  -> /mnt
 *   /mnt/       -> /mnt
 *   /           -> /
 *   point       -> .
 */
std::string parent_directory(const std::string& path) {
    if (path.empty()) return ".";
    // Strip trailing slashes
    std::string p = path;
    while (p.size() > 1 && p.back() == '/') p.pop_back();

    size_t pos = p.rfind('/');
    if (pos == 0) {
        return "/";
    }
    if (pos == std::string::npos) {
        return ".";
    }
    return p.substr(0, pos);
}

} // anonymous namespace

// =============================================================================
// FilesystemManager
// =============================================================================

FilesystemManager::FilesystemManager(IProcessRunner* proc_runner)
    : proc_runner_(proc_runner) {}

FilesystemManager::~FilesystemManager() = default;

// =============================================================================
// fs_type_to_string
// =============================================================================

std::string FilesystemManager::fs_type_to_string(FilesystemType fs_type) {
    switch (fs_type) {
        case FilesystemType::EXT4:     return "ext4";
        case FilesystemType::VFAT:     return "vfat";
        case FilesystemType::SquashFS: return "squashfs";
        case FilesystemType::Raw:
        case FilesystemType::Unknown:
        default:
            return "";
    }
}

// =============================================================================
// format
// =============================================================================

Result<void> FilesystemManager::format(const std::string& device_path,
                                       FilesystemType fs_type,
                                       const std::string& label) {
    std::lock_guard<std::mutex> lock(mutex_);

    std::vector<std::string> args;
    std::string tool_name;

    switch (fs_type) {
        // -----------------------------------------------------------------
        // EXT4
        // -----------------------------------------------------------------
        case FilesystemType::EXT4: {
            tool_name = "mkfs.ext4";
            args = {"mkfs.ext4", "-F"};   // -F = force (even if already has a fs)
            if (!label.empty()) {
                args.push_back("-L");
                args.push_back(label);
            }
            args.push_back(device_path);
            break;
        }

        // -----------------------------------------------------------------
        // VFAT (FAT32)
        // -----------------------------------------------------------------
        case FilesystemType::VFAT: {
            tool_name = "mkfs.vfat";
            args = {"mkfs.vfat", "-F", "32"};   // FAT32
            std::string vfat_label = sanitize_vfat_label(label);
            if (!vfat_label.empty()) {
                args.push_back("-n");
                args.push_back(vfat_label);
            }
            args.push_back(device_path);
            break;
        }

        // -----------------------------------------------------------------
        // SquashFS — read-only, cannot be formatted
        // -----------------------------------------------------------------
        case FilesystemType::SquashFS:
            return Result<void>::err(make_error(
                ErrorCode::FILESYSTEM_FORMAT_FAILED,
                "Format Not Supported",
                "SquashFS is a read-only filesystem and cannot be formatted.",
                "Cannot format SquashFS on device " + device_path,
                false));

        // -----------------------------------------------------------------
        // Raw — no formatting needed
        // -----------------------------------------------------------------
        case FilesystemType::Raw:
            return Result<void>::ok();

        // -----------------------------------------------------------------
        // Unknown / unhandled
        // -----------------------------------------------------------------
        case FilesystemType::Unknown:
        default:
            return Result<void>::err(make_error(
                ErrorCode::FILESYSTEM_FORMAT_FAILED,
                "Unknown Filesystem Type",
                "Cannot format: the filesystem type is unknown or unsupported.",
                "Unknown FilesystemType for device " + device_path,
                false));
    }

    // ---- Execute the mkfs tool ----
    auto run_result = proc_runner_->run(args, std::chrono::seconds(60));
    if (run_result.is_err()) {
        return Result<void>::err(run_result.take_error());
    }

    const auto& proc = run_result.value();

    if (proc.timed_out) {
        return Result<void>::err(make_error(
            ErrorCode::FILESYSTEM_FORMAT_FAILED,
            "Format Timed Out",
            "Formatting " + device_path + " timed out after 60 seconds.",
            tool_name + " timed out on " + device_path,
            true));
    }

    if (proc.cancelled) {
        return Result<void>::err(make_error(
            ErrorCode::INTERNAL_CANCELLED,
            "Format Cancelled",
            "Format operation was cancelled.",
            tool_name + " was cancelled for " + device_path,
            false));
    }

    if (proc.exit_code != 0) {
        return Result<void>::err(make_process_error(
            ErrorCode::FILESYSTEM_FORMAT_FAILED,
            "Format Failed",
            "Failed to format " + device_path + " as " +
                fs_type_to_string(fs_type) + ".",
            proc, true));
    }

    return Result<void>::ok();
}

// =============================================================================
// mount
// =============================================================================

Result<void> FilesystemManager::mount(const std::string& device_path,
                                      const std::string& mount_point,
                                      const std::string& fs_type,
                                      unsigned long mount_flags) {
    std::lock_guard<std::mutex> lock(mutex_);

    // ---- Create the mount point directory if it does not exist ----
    auto dir_result = create_directory_recursive(mount_point);
    if (dir_result.is_err()) {
        return Result<void>::err(make_error(
            ErrorCode::FILESYSTEM_MOUNT_FAILED,
            "Mount Point Creation Failed",
            "Failed to create mount point directory: " + mount_point + ".",
            "ensure_directory(\"" + mount_point + "\") failed",
            true));
    }

    // ---- Resolve the filesystem type argument ----
    const char* fstype_arg = nullptr;
    std::string fstype_storage;

    if (!fs_type.empty()) {
        // Caller explicitly specified a type string — use it as-is.
        fstype_storage = fs_type;
        fstype_arg = fstype_storage.c_str();
    }
    // If fs_type is empty, pass nullptr so the kernel auto-detects.

    // ---- Perform the mount(2) syscall ----
    int ret = ::mount(device_path.c_str(),
                      mount_point.c_str(),
                      fstype_arg,
                      mount_flags,
                      nullptr);    // data — not used for most filesystems

    if (ret != 0) {
        const char* sys_msg = ::strerror(errno);
        std::string fstype_desc = fstype_arg ? fstype_arg : "auto";
        return Result<void>::err(make_errno_error(
            ErrorCode::FILESYSTEM_MOUNT_FAILED,
            "Mount Failed",
            "Failed to mount " + device_path + " at " + mount_point +
                " (" + fstype_desc + "): " + sys_msg,
            "mount(\"" + device_path + "\", \"" + mount_point + "\", " +
                fstype_desc + ", flags=" + std::to_string(mount_flags) + ")",
            true));
    }

    return Result<void>::ok();
}

// =============================================================================
// umount
// =============================================================================

Result<void> FilesystemManager::umount(const std::string& mount_point,
                                       bool lazy) {
    std::lock_guard<std::mutex> lock(mutex_);

    int flags = lazy ? MNT_DETACH : 0;

    int ret = ::umount2(mount_point.c_str(), flags);
    if (ret != 0) {
        const char* sys_msg = ::strerror(errno);
        std::string mode = lazy ? "lazy" : "normal";
        return Result<void>::err(make_errno_error(
            ErrorCode::FILESYSTEM_MOUNT_FAILED,
            "Unmount Failed",
            "Failed to unmount " + mount_point + " (" + mode + "): " + sys_msg,
            "umount2(\"" + mount_point + "\", flags=" + std::to_string(flags) + ")",
            true));
    }

    return Result<void>::ok();
}

// =============================================================================
// check
// =============================================================================

Result<bool> FilesystemManager::check(const std::string& device_path,
                                      FilesystemType fs_type,
                                      bool repair) {
    std::lock_guard<std::mutex> lock(mutex_);

    std::vector<std::string> args;
    std::string tool_name;

    switch (fs_type) {
        // -----------------------------------------------------------------
        // EXT4 — use e2fsck
        // -----------------------------------------------------------------
        case FilesystemType::EXT4: {
            tool_name = "e2fsck";
            args.push_back("e2fsck");
            if (repair) {
                args.push_back("-p");    // automatic repair (preen)
            } else {
                args.push_back("-n");    // read-only: answer no to all questions
            }
            args.push_back(device_path);
            break;
        }

        // -----------------------------------------------------------------
        // VFAT — use fsck.vfat (dosfsck)
        // -----------------------------------------------------------------
        case FilesystemType::VFAT: {
            tool_name = "fsck.vfat";
            args.push_back("fsck.vfat");
            if (repair) {
                args.push_back("-a");    // automatically repair
            } else {
                args.push_back("-n");    // no write — check only
            }
            args.push_back(device_path);
            break;
        }

        // -----------------------------------------------------------------
        // SquashFS — read-only, always considered clean
        // -----------------------------------------------------------------
        case FilesystemType::SquashFS:
            return Result<bool>::ok(true);

        // -----------------------------------------------------------------
        // Raw / Unknown — nothing meaningful to check
        // -----------------------------------------------------------------
        case FilesystemType::Raw:
        case FilesystemType::Unknown:
        default:
            return Result<bool>::ok(true);
    }

    // ---- Execute the fsck tool ----
    auto run_result = proc_runner_->run(args, std::chrono::seconds(60));
    if (run_result.is_err()) {
        return Result<bool>::err(run_result.take_error());
    }

    const auto& proc = run_result.value();

    if (proc.timed_out) {
        return Result<bool>::err(make_error(
            ErrorCode::FILESYSTEM_CHECK_FAILED,
            "Filesystem Check Timed Out",
            "Filesystem check timed out for " + device_path + " after 60 seconds.",
            tool_name + " timed out on " + device_path,
            true));
    }

    if (proc.cancelled) {
        return Result<bool>::err(make_error(
            ErrorCode::INTERNAL_CANCELLED,
            "Check Cancelled",
            "Filesystem check was cancelled.",
            tool_name + " was cancelled for " + device_path,
            false));
    }

    // ---- Interpret fsck exit code (bitmask per the man page) ----
    int code = proc.exit_code;

    // 0 — No errors
    if (code == 0) {
        return Result<bool>::ok(true);
    }

    // Bits 2 (code & 4): uncorrected errors remain
    if (code & 4) {
        // Uncorrected errors — the filesystem is not clean
        return Result<bool>::ok(false);
    }

    // Bit 3 and beyond (code >= 8): operational error, usage error, cancelled, etc.
    // These indicate the *check tool itself* failed, not the filesystem.
    if (code >= 8) {
        return Result<bool>::err(make_process_error(
            ErrorCode::FILESYSTEM_CHECK_FAILED,
            "Filesystem Check Error",
            "The filesystem check tool encountered an error on " +
                device_path + ".",
            proc, (code & 8) ? true : false));   // operational errors may be retryable
    }

    // Remaining cases: code & 1 (errors corrected) or code & 2 (reboot needed).
    // The filesystem is clean (or was repaired).
    return Result<bool>::ok(true);
}

// =============================================================================
// is_mounted
// =============================================================================

Result<bool> FilesystemManager::is_mounted(const std::string& path) {
    std::lock_guard<std::mutex> lock(mutex_);

    // ---- Strategy 1: parse /proc/mounts ----
    // Format: device mountpoint fstype options dump pass
    std::ifstream mounts("/proc/mounts");
    if (!mounts.is_open()) {
        return Result<bool>::err(make_errno_error(
            ErrorCode::INTERNAL_ERROR,
            "Cannot Read Mount Table",
            "Unable to open /proc/mounts to check mount status.",
            "open(\"/proc/mounts\")",
            false));
    }

    std::string line;
    while (std::getline(mounts, line)) {
        // Skip empty lines
        if (line.empty()) continue;

        std::istringstream iss(line);
        std::string device, mountpoint;
        if (iss >> device >> mountpoint) {
            if (device == path || mountpoint == path) {
                return Result<bool>::ok(true);
            }
        }
    }
    mounts.close();

    // ---- Strategy 2: stat() heuristic ----
    // If path is a directory on a different device than its parent,
    // it is very likely a mount point that /proc/mounts may not show
    // (e.g. certain bind-mount or overlay scenarios).
    struct stat path_stat;
    if (::stat(path.c_str(), &path_stat) == 0) {
        // Don't check the root filesystem — everything lives on it.
        if (path == "/") {
            return Result<bool>::ok(false);
        }

        std::string parent = parent_directory(path);

        struct stat parent_stat;
        if (::stat(parent.c_str(), &parent_stat) == 0) {
            if (path_stat.st_dev != parent_stat.st_dev) {
                return Result<bool>::ok(true);
            }
        }
    }

    return Result<bool>::ok(false);
}

// =============================================================================
// ScopedMount
// =============================================================================

ScopedMount::ScopedMount(IFilesystemManager* fs_mgr,
                         const std::string& device_path,
                         const std::string& mount_point,
                         const std::string& fs_type,
                         unsigned long mount_flags)
    : fs_mgr_(fs_mgr)
    , mount_point_(mount_point)
    , mounted_(false)
{
    if (!fs_mgr_) return;
    auto result = fs_mgr_->mount(device_path, mount_point, fs_type, mount_flags);
    mounted_ = result.is_ok();
}

ScopedMount::~ScopedMount() {
    if (mounted_ && fs_mgr_) {
        // Lazy unmount on destruction — we don't want to block or throw
        // during stack unwinding.
        fs_mgr_->umount(mount_point_, true);
        mounted_ = false;
    }
}

ScopedMount::ScopedMount(ScopedMount&& other) noexcept
    : fs_mgr_(other.fs_mgr_)
    , mount_point_(std::move(other.mount_point_))
    , mounted_(other.mounted_)
{
    other.fs_mgr_   = nullptr;
    other.mounted_  = false;
}

ScopedMount& ScopedMount::operator=(ScopedMount&& other) noexcept {
    if (this != &other) {
        // Unmount our current mount before taking ownership of other's.
        if (mounted_ && fs_mgr_) {
            fs_mgr_->umount(mount_point_, true);
        }
        fs_mgr_       = other.fs_mgr_;
        mount_point_  = std::move(other.mount_point_);
        mounted_      = other.mounted_;
        other.fs_mgr_  = nullptr;
        other.mounted_ = false;
    }
    return *this;
}

Result<void> ScopedMount::unmount() {
    if (!mounted_ || !fs_mgr_) {
        return Result<void>::ok();
    }
    auto result = fs_mgr_->umount(mount_point_);
    if (result.is_ok()) {
        mounted_ = false;
    }
    return result;
}

} // namespace installer
