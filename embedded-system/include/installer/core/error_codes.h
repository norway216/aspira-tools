/**
 * @file error_codes.h
 * @brief Error code registry with human-readable mappings.
 *
 * Provides centralized lookup of error codes to user-friendly
 * and technical messages. Follows the error code hierarchy
 * defined in Architecture Doc §17.
 */

#ifndef INSTALLER_CORE_ERROR_CODES_H
#define INSTALLER_CORE_ERROR_CODES_H

#include "installer/core/types.h"
#include <unordered_map>
#include <string>

namespace installer {

/**
 * Get a human-readable description for an error code.
 */
const char* error_code_description(const std::string& code);

/**
 * Determine if an error is retryable based on its code.
 */
bool error_code_is_retryable(const std::string& code);

/**
 * Determine if an error requires a reboot.
 */
bool error_code_needs_reboot(const std::string& code);

} // namespace installer

#endif // INSTALLER_CORE_ERROR_CODES_H
