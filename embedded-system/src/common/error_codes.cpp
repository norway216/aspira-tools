/**
 * @file error_codes.cpp
 * @brief Implementation of error code description and retryability lookups.
 *
 * Maps all ErrorCode namespace constants (E1001–E9005) to
 * human-readable descriptions, retryable flags, and reboot-required flags
 * as specified in Architecture Doc §17.
 */

#include "installer/core/error_codes.h"
#include <unordered_map>

namespace installer {
namespace {

// ---------------------------------------------------------------------------
// Internal registry entry
// ---------------------------------------------------------------------------

struct ErrorMeta {
    const char* description;
    bool retryable;
    bool reboot_required;
};

// ---------------------------------------------------------------------------
// Static registry — populated once at static-init time
// ---------------------------------------------------------------------------

const std::unordered_map<std::string, ErrorMeta> kErrorRegistry = {

    // ===================================================================
    // E1xxx — Device & Hardware
    // ===================================================================

    {ErrorCode::DEVICE_NOT_FOUND,
     {"Device not found. Verify the target device is connected and powered on.",
      true, false}},
    {ErrorCode::DEVICE_READ_ONLY,
     {"Device is read-only. Check the write-protect switch or filesystem mount flags.",
      false, false}},
    {ErrorCode::DEVICE_CAPACITY_LOW,
     {"Device capacity is too low. The target disk is smaller than the minimum "
      "required by the package.",
      false, false}},
    {ErrorCode::DEVICE_IO_ERROR,
     {"Device I/O error. The storage media may be failing or the cable may be loose.",
      true, false}},
    {ErrorCode::DEVICE_NOT_SAFE_TARGET,
     {"Device is not a safe target. Refusing to overwrite the system disk or "
      "installer media. Select a different device.",
      false, false}},
    {ErrorCode::DEVICE_BUSY,
     {"Device is busy. Close all applications using the device and try again.",
      true, false}},
    {ErrorCode::DEVICE_MEDIA_REMOVED,
     {"Storage media was unexpectedly removed during the operation. "
      "Reconnect the device and retry.",
      false, false}},
    {ErrorCode::DEVICE_HOTPLUG_TIMEOUT,
     {"Device hotplug detection timed out. The expected device did not appear "
      "within the required time window.",
      true, false}},

    // ===================================================================
    // E2xxx — Package
    // ===================================================================

    {ErrorCode::PACKAGE_INVALID_FORMAT,
     {"Package format is invalid or unsupported. Verify the file is a valid "
      "installer package.",
      false, false}},
    {ErrorCode::PACKAGE_SIGNATURE_FAIL,
     {"Package signature verification failed. The package may have been tampered "
      "with or corrupted during download.",
      false, false}},
    {ErrorCode::PACKAGE_HW_INCOMPATIBLE,
     {"Package is incompatible with this hardware. Check the product name and "
      "hardware revision.",
      false, false}},
    {ErrorCode::PACKAGE_HASH_MISMATCH,
     {"Package hash mismatch. The data integrity check failed — the file may be "
      "incomplete or corrupted.",
      true, false}},
    {ErrorCode::PACKAGE_MANIFEST_ERROR,
     {"Package manifest is missing, malformed, or contains invalid entries.",
      false, false}},
    {ErrorCode::PACKAGE_VERSION_REFUSED,
     {"Package version has been refused. Downgrades may be disabled, or the "
      "version is older than the minimum required.",
      false, false}},
    {ErrorCode::PACKAGE_CORRUPTED,
     {"Package content is corrupted. Re-download the package or verify the "
      "source media.",
      false, false}},

    // ===================================================================
    // E3xxx — Partition & Filesystem
    // ===================================================================

    {ErrorCode::PARTITION_CREATE_FAILED,
     {"Failed to create partition on the target device. The partition table may "
      "be locked or the device may be in use.",
      true, false}},
    {ErrorCode::FILESYSTEM_FORMAT_FAILED,
     {"Failed to format the filesystem. The partition may be too small, or the "
      "required tools (mkfs.*) may be missing.",
      true, false}},
    {ErrorCode::FILESYSTEM_MOUNT_FAILED,
     {"Failed to mount the filesystem. Check that the required kernel modules "
      "are loaded and the mount point exists.",
      true, false}},
    {ErrorCode::FILESYSTEM_CHECK_FAILED,
     {"Filesystem integrity check failed. The filesystem may be corrupted and "
      "may need manual repair.",
      false, false}},
    {ErrorCode::PARTITION_NOT_FOUND,
     {"Expected partition was not found on the target device. Verify the "
      "partition layout.",
      false, false}},
    {ErrorCode::PARTITION_LAYOUT_MISMATCH,
     {"Partition layout does not match the expected configuration. The target "
      "device may have been previously partitioned differently.",
      false, false}},

    // ===================================================================
    // E4xxx — Image Write
    // ===================================================================

    {ErrorCode::IMAGE_WRITE_FAILED,
     {"Failed to write image data to the device. The storage may be full or "
      "experiencing hardware errors.",
      true, false}},
    {ErrorCode::IMAGE_VERIFY_FAILED,
     {"Image verification failed. The data written to the device does not match "
      "the source. The target media may be unreliable.",
      true, false}},
    {ErrorCode::IMAGE_WRITE_TIMEOUT,
     {"Image write operation timed out. The storage device may be too slow or "
      "unresponsive.",
      true, false}},
    {ErrorCode::IMAGE_SOURCE_CORRUPT,
     {"Image source data is corrupted. The source file integrity check failed.",
      false, false}},
    {ErrorCode::IMAGE_DECOMPRESS_FAIL,
     {"Image decompression failed. The compressed payload may be truncated or "
      "corrupted.",
      true, false}},

    // ===================================================================
    // E5xxx — Backup
    // ===================================================================

    {ErrorCode::BACKUP_SPACE_LOW,
     {"Not enough free space on the backup destination. Free up space or use a "
      "larger storage device.",
      false, false}},
    {ErrorCode::BACKUP_MEDIA_REMOVED,
     {"Backup media was unexpectedly removed during the backup operation. "
      "Reconnect and retry.",
      false, false}},
    {ErrorCode::BACKUP_CONSISTENCY,
     {"Backup consistency check failed. The backup may be incomplete or "
      "inconsistent.",
      true, false}},
    {ErrorCode::BACKUP_VERIFY_FAILED,
     {"Backup verification failed. The backed-up data does not match the "
      "original source.",
      true, false}},

    // ===================================================================
    // E6xxx — Restore
    // ===================================================================

    {ErrorCode::RESTORE_VERSION_MISMATCH,
     {"Restore version mismatch. The backup was created for a different software "
      "version and is not compatible.",
      false, false}},
    {ErrorCode::RESTORE_DEVICE_MISMATCH,
     {"Restore device mismatch. The backup was created on different hardware "
      "and cannot be restored to this device.",
      false, false}},
    {ErrorCode::RESTORE_SPACE_LOW,
     {"Not enough free space on the restore target. The target device is smaller "
      "than the backup data requires.",
      false, false}},
    {ErrorCode::RESTORE_VERIFY_FAILED,
     {"Restore verification failed. The restored data does not match the backup.",
      true, false}},
    {ErrorCode::RESTORE_ROLLBACK_FAILED,
     {"Restore rollback failed. The system may be in an inconsistent state. "
      "A system reinstall is recommended.",
      false, true}},

    // ===================================================================
    // E7xxx — Bootloader
    // ===================================================================

    {ErrorCode::BOOT_ENV_WRITE_FAILED,
     {"Failed to write boot environment data. The bootloader configuration may "
      "be incomplete. Reboot and retry.",
      true, true}},
    {ErrorCode::BOOT_SLOT_INVALID,
     {"Invalid boot slot specified. The requested boot slot does not exist in "
      "the partition layout.",
      false, false}},
    {ErrorCode::BOOT_MARK_FAILED,
     {"Failed to mark the boot slot as active. The bootloader may not have been "
      "properly installed.",
      true, false}},

    // ===================================================================
    // E8xxx — Network
    // ===================================================================

    {ErrorCode::NETWORK_DOWNLOAD_FAILED,
     {"Network download failed. Check your internet connection and try again.",
      true, false}},
    {ErrorCode::NETWORK_TIMEOUT,
     {"Network operation timed out. The server may be unreachable or the "
      "connection may be too slow.",
      true, false}},

    // ===================================================================
    // E9xxx — Internal
    // ===================================================================

    {ErrorCode::INTERNAL_ERROR,
     {"An unexpected internal error occurred. Please check the logs for details.",
      false, false}},
    {ErrorCode::INTERNAL_TIMEOUT,
     {"Internal operation timed out. The system may be overloaded. Retry after a "
      "short wait.",
      true, false}},
    {ErrorCode::INTERNAL_CANCELLED,
     {"Operation was cancelled by user request.",
      false, false}},
    {ErrorCode::INTERNAL_INVALID_STATE,
     {"The installer is in an invalid internal state. A reboot is required to "
      "recover.",
      false, true}},
    {ErrorCode::INTERNAL_CONFIG_ERROR,
     {"Configuration error. One or more installer configuration settings are "
      "missing or invalid. Check the configuration files.",
      false, false}},
};

} // anonymous namespace

// =============================================================================
// Public API
// =============================================================================

const char* error_code_description(const std::string& code) {
    auto it = kErrorRegistry.find(code);
    if (it != kErrorRegistry.end()) {
        return it->second.description;
    }
    return "An unknown error occurred. Error code not recognized.";
}

bool error_code_is_retryable(const std::string& code) {
    auto it = kErrorRegistry.find(code);
    if (it != kErrorRegistry.end()) {
        return it->second.retryable;
    }
    return false;
}

bool error_code_needs_reboot(const std::string& code) {
    auto it = kErrorRegistry.find(code);
    if (it != kErrorRegistry.end()) {
        return it->second.reboot_required;
    }
    return false;
}

} // namespace installer
