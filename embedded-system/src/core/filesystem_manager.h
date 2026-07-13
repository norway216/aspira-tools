/**
 * @file filesystem_manager.h
 * @brief FilesystemManager — format, mount, check, and query filesystems
 *        using system tools via ProcessRunner and direct syscalls.
 */

#ifndef INSTALLER_CORE_FILESYSTEM_MANAGER_H
#define INSTALLER_CORE_FILESYSTEM_MANAGER_H

#include "installer/filesystem/ifilesystem_manager.h"
#include "installer/core/types.h"
#include "installer/core/result.h"
#include <string>
#include <mutex>

namespace installer {

class IProcessRunner;

class FilesystemManager : public IFilesystemManager {
public:
    explicit FilesystemManager(IProcessRunner* proc_runner);
    ~FilesystemManager() override;

    Result<void> format(const std::string& device_path,
                        FilesystemType fs_type,
                        const std::string& label = "") override;

    Result<void> mount(const std::string& partition,
                       const std::string& mount_point,
                       const std::string& fs_type = "",
                       int flags = 0) override;

    Result<void> umount(const std::string& mount_point, bool force = false) override;

    Result<void> check(const std::string& partition) override;

    bool is_mounted(const std::string& path) override;

private:
    // Convert FilesystemType to string for mount/format
    static std::string fs_type_to_string(FilesystemType fs_type);

    IProcessRunner* proc_runner_;
    mutable std::mutex mutex_;
};

// RAII class: mounts on construction, unmounts on destruction
class ScopedMount {
public:
    /**
     * Construct a ScopedMount by mounting @p device_path at @p mount_point.
     *
     * The mount is attempted immediately.  Callers MUST check is_mounted()
     * after construction to verify the mount succeeded — mount failures
     * cannot be propagated through the constructor.
     */
    ScopedMount(IFilesystemManager* fs_mgr, const std::string& device_path,
                const std::string& mount_point, const std::string& fs_type = "",
                int mount_flags = 0);
    ~ScopedMount();

    // Non-copyable, movable
    ScopedMount(const ScopedMount&) = delete;
    ScopedMount& operator=(const ScopedMount&) = delete;
    ScopedMount(ScopedMount&& other) noexcept;
    ScopedMount& operator=(ScopedMount&& other) noexcept;

    bool is_mounted() const { return mounted_; }
    const std::string& mount_point() const { return mount_point_; }

    /**
     * Explicitly unmount before destruction.  After a successful call
     * is_mounted() returns false and the destructor becomes a no-op.
     *
     * @return Ok on success, or FILESYSTEM_MOUNT_FAILED on error.
     */
    Result<void> unmount();

private:
    IFilesystemManager* fs_mgr_ = nullptr;
    std::string mount_point_;
    bool mounted_ = false;
};

} // namespace installer

#endif // INSTALLER_CORE_FILESYSTEM_MANAGER_H
