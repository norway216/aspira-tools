/**
 * @file ifilesystem_manager.h
 * @brief Filesystem creation, mounting, and checking interface.
 *
 * Wraps mkfs, mount, umount, and fsck operations behind a uniform
 * interface. All operations that modify on-disk state return Result<void>
 * so callers are forced to handle errors.
 *
 * @see Architecture Doc §6.5
 */

#ifndef INSTALLER_FILESYSTEM_IFILESYSTEM_MANAGER_H
#define INSTALLER_FILESYSTEM_IFILESYSTEM_MANAGER_H

#include <string>

#include "installer/core/result.h"
#include "installer/core/types.h"

namespace installer {

/**
 * Filesystem operations interface.
 *
 * Provides a uniform abstraction over the standard Linux filesystem
 * tools (mkfs.*, mount, umount, fsck.*). Implementations may use
 * system calls (mount(2), etc.) directly or invoke the command-line
 * utilities via IProcessRunner.
 */
class IFilesystemManager {
public:
    virtual ~IFilesystemManager() = default;

    /**
     * Create a filesystem on a partition (i.e. format it).
     *
     * Destroys any existing data on the partition. The partition must
     * already exist in the partition table before this call.
     *
     * @param partition Absolute path to the partition device node
     *                  (e.g. /dev/mmcblk0p2).
     * @param fs_type   Filesystem type to create (VFAT, EXT4, etc.).
     * @param label     Optional volume label (empty string = no label).
     * @return Result<void> — ok on success,
     *         FILESYSTEM_FORMAT_FAILED on error.
     */
    virtual Result<void> format(const std::string& partition,
                                FilesystemType fs_type,
                                const std::string& label = "") = 0;

    /**
     * Mount a filesystem at the specified mount point.
     *
     * The mount point directory must already exist. By default the
     * filesystem type is auto-detected; pass an explicit type string
     * (e.g. "ext4", "vfat") to override.
     *
     * @param partition   Absolute path to the partition or device.
     * @param mount_point Absolute path to an existing directory.
     * @param fs_type     Filesystem type string for the -t flag
     *                    (empty = auto-detect).
     * @param flags       Mount flags (MS_RDONLY, MS_NOSUID, etc. as
     *                    defined in <sys/mount.h>). Zero = defaults.
     * @return Result<void> — ok when mounted,
     *         FILESYSTEM_MOUNT_FAILED on error.
     */
    virtual Result<void> mount(const std::string& partition,
                               const std::string& mount_point,
                               const std::string& fs_type = "",
                               int flags = 0) = 0;

    /**
     * Unmount a filesystem.
     *
     * @param mount_point Absolute path to the mount point to unmount.
     * @param force       If true, perform a lazy unmount (MNT_DETACH /
     *                    umount -l) even if the filesystem is busy.
     * @return Result<void> — ok when unmounted,
     *         FILESYSTEM_MOUNT_FAILED on error.
     */
    virtual Result<void> umount(const std::string& mount_point,
                                bool force = false) = 0;

    /**
     * Check (and optionally repair) a filesystem.
     *
     * Runs the appropriate fsck.* tool for the filesystem type detected
     * on the partition. This is used as a safety check after formatting
     * or before mounting a pre-existing filesystem.
     *
     * @param partition Absolute path to the partition device node.
     * @return Result<void> — ok if the filesystem is clean,
     *         FILESYSTEM_CHECK_FAILED if errors are detected.
     */
    virtual Result<void> check(const std::string& partition) = 0;

    /**
     * Test whether a path is currently a mount point.
     *
     * Checks /proc/mounts (or uses statfs) to determine whether the
     * given path is mounted. Returns false for paths that are not
     * mount points even if they reside on a mounted filesystem.
     *
     * @param path Absolute path to check.
     * @return true if the path is listed in /proc/mounts.
     */
    virtual bool is_mounted(const std::string& path) = 0;
};

} // namespace installer

#endif // INSTALLER_FILESYSTEM_IFILESYSTEM_MANAGER_H
