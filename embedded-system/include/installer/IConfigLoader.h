/**
 * @file IConfigLoader.h
 * @brief Configuration loading interface.
 */

#ifndef INSTALLER_ICONFIGLOADER_H
#define INSTALLER_ICONFIGLOADER_H

#include "installer/core/types.h"
#include "installer/core/result.h"
#include <string>

namespace installer {

class IConfigLoader {
public:
    virtual ~IConfigLoader() = default;

    /**
     * Load configuration from a YAML file.
     */
    virtual Result<void> load(const std::string& config_path) = 0;

    /**
     * Get a string value by dotted key path, e.g. "installer.log_dir".
     */
    virtual Result<std::string> get_string(const std::string& key) const = 0;

    /**
     * Get an integer value by dotted key path.
     */
    virtual Result<int64_t> get_int(const std::string& key) const = 0;

    /**
     * Get a boolean value by dotted key path.
     */
    virtual Result<bool> get_bool(const std::string& key) const = 0;

    /**
     * Get a list of strings by dotted key path.
     */
    virtual Result<std::vector<std::string>> get_string_list(
        const std::string& key) const = 0;

    /**
     * Check if a key exists.
     */
    virtual bool has_key(const std::string& key) const = 0;
};

} // namespace installer

#endif // INSTALLER_ICONFIGLOADER_H
