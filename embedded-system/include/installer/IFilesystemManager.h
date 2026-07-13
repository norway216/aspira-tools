/**
 * @file IFilesystemManager.h
 * @brief Filesystem creation, mounting, and checking interface.
 */

#ifndef INSTALLER_IFILESYSTEMMANAGER_H
#define INSTALLER_IFILESYSTEMMANAGER_H

#include "installer/core/types.h"
#include "installer/core/result.h"
#include <string>

namespace installer {

class IFilesystemManager {
public:
    virtual ~IFilesystemManager() = default;

    virtual Result<void> format(const std::string& partition,
                                FilesystemType fs_type,
                                const std::string& label = "") = 0;

    virtual Result<void> mount(const std::string& partition,
                               const std::string& mount_point,
                               const std::string& options = "") = 0;

    virtual Result<void> unmount(const std::string& mount_point) = 0;

    virtual Result<void> check(const std::string& partition) = 0;

    virtual Result<void> resize(const std::string& partition,
                                uint64_t new_size_bytes) = 0;
};

} // namespace installer

#endif // INSTALLER_IFILESYSTEMMANAGER_H
