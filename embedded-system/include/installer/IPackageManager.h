/**
 * @file IPackageManager.h
 * @brief Package opening, manifest parsing, and validation interface.
 */

#ifndef INSTALLER_IPACKAGEMANAGER_H
#define INSTALLER_IPACKAGEMANAGER_H

#include "installer/core/types.h"
#include "installer/core/result.h"
#include <string>

namespace installer {

class IPackageManager {
public:
    virtual ~IPackageManager() = default;

    virtual Result<void> open(const std::string& package_path) = 0;
    virtual Result<Manifest> load_manifest() = 0;
    virtual Result<void> verify_payload_hash(const std::string& payload_name) = 0;
    virtual Result<void> extract_payload(const std::string& payload_name,
                                         const std::string& target_path,
                                         ProgressCallback progress,
                                         CancellationToken& cancel) = 0;
    virtual Result<uint64_t> get_payload_size(const std::string& payload_name) = 0;
    virtual void close() = 0;
};

} // namespace installer

#endif // INSTALLER_IPACKAGEMANAGER_H
