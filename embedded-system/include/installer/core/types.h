/**
 * @file types.h
 * @brief Central type definitions for the Embedded Linux Installer.
 *
 * All shared enums, error codes, POD structs, and callback typedefs
 * are defined here to avoid circular dependencies and ensure
 * consistency across all modules.
 */

#ifndef INSTALLER_CORE_TYPES_H
#define INSTALLER_CORE_TYPES_H

#include <atomic>
#include <chrono>
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace installer {

/* =========================================================================
 *  Error Codes (Architecture Doc §17)
 * =========================================================================
 *
 *  E1xxx — Device & Hardware
 *  E2xxx — Package
 *  E3xxx — Partition & Filesystem
 *  E4xxx — Image Write
 *  E5xxx — Backup
 *  E6xxx — Restore
 *  E7xxx — Bootloader
 *  E8xxx — Network
 *  E9xxx — Internal Error
 * ========================================================================= */

struct ErrorCode {
    // ---- E1xxx: Device & Hardware ----
    static constexpr const char* DEVICE_NOT_FOUND       = "E1001";
    static constexpr const char* DEVICE_READ_ONLY       = "E1002";
    static constexpr const char* DEVICE_CAPACITY_LOW    = "E1003";
    static constexpr const char* DEVICE_IO_ERROR        = "E1004";
    static constexpr const char* DEVICE_NOT_SAFE_TARGET = "E1005";
    static constexpr const char* DEVICE_BUSY            = "E1006";
    static constexpr const char* DEVICE_MEDIA_REMOVED   = "E1007";
    static constexpr const char* DEVICE_HOTPLUG_TIMEOUT = "E1008";

    // ---- E2xxx: Package ----
    static constexpr const char* PACKAGE_INVALID_FORMAT  = "E2001";
    static constexpr const char* PACKAGE_SIGNATURE_FAIL  = "E2002";
    static constexpr const char* PACKAGE_HW_INCOMPATIBLE = "E2003";
    static constexpr const char* PACKAGE_HASH_MISMATCH   = "E2004";
    static constexpr const char* PACKAGE_MANIFEST_ERROR  = "E2005";
    static constexpr const char* PACKAGE_VERSION_REFUSED = "E2006";
    static constexpr const char* PACKAGE_CORRUPTED       = "E2007";

    // ---- E3xxx: Partition & Filesystem ----
    static constexpr const char* PARTITION_CREATE_FAILED  = "E3001";
    static constexpr const char* FILESYSTEM_FORMAT_FAILED = "E3002";
    static constexpr const char* FILESYSTEM_MOUNT_FAILED  = "E3003";
    static constexpr const char* FILESYSTEM_CHECK_FAILED  = "E3004";
    static constexpr const char* PARTITION_NOT_FOUND      = "E3005";
    static constexpr const char* PARTITION_LAYOUT_MISMATCH = "E3006";

    // ---- E4xxx: Image Write ----
    static constexpr const char* IMAGE_WRITE_FAILED    = "E4001";
    static constexpr const char* IMAGE_VERIFY_FAILED   = "E4002";
    static constexpr const char* IMAGE_WRITE_TIMEOUT   = "E4003";
    static constexpr const char* IMAGE_SOURCE_CORRUPT  = "E4004";
    static constexpr const char* IMAGE_DECOMPRESS_FAIL = "E4005";

    // ---- E5xxx: Backup ----
    static constexpr const char* BACKUP_SPACE_LOW     = "E5001";
    static constexpr const char* BACKUP_MEDIA_REMOVED = "E5002";
    static constexpr const char* BACKUP_CONSISTENCY   = "E5003";
    static constexpr const char* BACKUP_VERIFY_FAILED = "E5004";

    // ---- E6xxx: Restore ----
    static constexpr const char* RESTORE_VERSION_MISMATCH = "E6001";
    static constexpr const char* RESTORE_DEVICE_MISMATCH  = "E6002";
    static constexpr const char* RESTORE_SPACE_LOW        = "E6003";
    static constexpr const char* RESTORE_VERIFY_FAILED    = "E6004";
    static constexpr const char* RESTORE_ROLLBACK_FAILED  = "E6005";

    // ---- E7xxx: Bootloader ----
    static constexpr const char* BOOT_ENV_WRITE_FAILED  = "E7001";
    static constexpr const char* BOOT_SLOT_INVALID       = "E7002";
    static constexpr const char* BOOT_MARK_FAILED        = "E7003";

    // ---- E8xxx: Network ----
    static constexpr const char* NETWORK_DOWNLOAD_FAILED = "E8001";
    static constexpr const char* NETWORK_TIMEOUT         = "E8002";

    // ---- E9xxx: Internal ----
    static constexpr const char* INTERNAL_ERROR         = "E9001";
    static constexpr const char* INTERNAL_TIMEOUT       = "E9002";
    static constexpr const char* INTERNAL_CANCELLED     = "E9003";
    static constexpr const char* INTERNAL_INVALID_STATE = "E9004";
    static constexpr const char* INTERNAL_CONFIG_ERROR  = "E9005";
};

/**
 * Structured error information as defined in Architecture Doc §17.
 */
struct InstallerError {
    std::string code;               // e.g. "E4001"
    std::string title;
    std::string user_message;
    std::string technical_message;
    std::string component;
    std::string operation;
    bool retryable = false;
    bool reboot_required = false;

    static InstallerError make(const std::string& code,
                               const std::string& title,
                               const std::string& user_msg,
                               const std::string& tech_msg = "",
                               bool retryable = false,
                               bool reboot = false) {
        return InstallerError{code, title, user_msg, tech_msg, "", "", retryable, reboot};
    }
};

/* =========================================================================
 *  Cancellation Token
 * ========================================================================= */

/**
 * Thread-safe cancellation token.
 * Passed by reference to all long-running operations.
 */
class CancellationToken {
public:
    CancellationToken() = default;
    ~CancellationToken() = default;

    // Non-copyable, movable
    CancellationToken(const CancellationToken&) = delete;
    CancellationToken& operator=(const CancellationToken&) = delete;
    CancellationToken(CancellationToken&&) = delete;
    CancellationToken& operator=(CancellationToken&&) = delete;

    bool is_cancelled() const { return cancelled_.load(std::memory_order_acquire); }
    void cancel() { cancelled_.store(true, std::memory_order_release); }
    void reset() { cancelled_.store(false, std::memory_order_release); }

private:
    std::atomic<bool> cancelled_{false};
};

/* =========================================================================
 *  Progress Reporting
 * ========================================================================= */

struct ProgressInfo {
    int percent = 0;                // 0–100
    std::string step_description;
    std::string current_file;
    uint64_t bytes_processed = 0;
    uint64_t bytes_total = 0;
    double speed_bytes_per_sec = 0.0;
};

using ProgressCallback = std::function<void(const ProgressInfo&)>;

/* =========================================================================
 *  Job Types (Architecture Doc §7)
 * ========================================================================= */

enum class JobType {
    InstallSystem,
    UpgradeSystem,
    BackupData,
    RestoreData,
    RepairSystem,
    VerifyPackage,
    ExportLogs
};

const char* job_type_name(JobType type);

enum class JobState {
    Idle,
    Preparing,
    Running,
    Paused,
    Cancelling,
    Completed,
    Failed,
    Recoverable,
    RebootRequired
};

const char* job_state_name(JobState state);

/* =========================================================================
 *  Block Device Info (Architecture Doc §6.1)
 * ========================================================================= */

enum class DeviceType {
    Unknown,
    eMMC,
    NVMe,
    SATA,
    USB,
    SD
};

struct BlockDeviceInfo {
    std::string path;                   // e.g. /dev/mmcblk0
    std::string model;
    std::string serial;
    uint64_t size_bytes = 0;
    uint32_t logical_sector_size = 512;
    uint32_t physical_sector_size = 512;
    bool removable = false;
    bool read_only = false;
    bool is_system_disk = false;
    bool is_installer_media = false;
    DeviceType type = DeviceType::Unknown;
};

/* =========================================================================
 *  Manifest & Package (Architecture Doc §9)
 * ========================================================================= */

struct PayloadEntry {
    std::string name;               // e.g. "kernel_b"
    std::string file;               // relative path in package
    std::string target;             // e.g. "kernel_inactive"
    std::string type;               // "raw", "ext4_zstd", "tar_zst"
    uint64_t size = 0;
    uint64_t uncompressed_size = 0;
    std::string sha256;
};

struct Manifest {
    int format_version = 1;
    std::string package_id;
    std::string product;
    std::string version;
    std::string build_id;
    std::string architecture;
    std::vector<std::string> hardware_profiles;
    std::string min_installer_version;
    uint64_t min_disk_size_bytes = 0;
    bool allow_downgrade = false;
    std::vector<PayloadEntry> payloads;
};

/* =========================================================================
 *  Logging (Architecture Doc §18)
 * ========================================================================= */

enum class LogLevel {
    Debug = 0,
    Info  = 1,
    Warn  = 2,
    Error = 3
};

const char* log_level_name(LogLevel level);

/* =========================================================================
 *  Partition & Filesystem (Architecture Doc §6.4, §6.5)
 * ========================================================================= */

enum class FilesystemType {
    Unknown,
    VFAT,
    EXT4,
    SquashFS,
    Raw
};

struct PartitionSpec {
    std::string name;               // e.g. "boot", "rootfs_a"
    uint64_t size_mib = 0;          // 0 = fill remaining
    FilesystemType filesystem = FilesystemType::EXT4;
    std::string label;
};

struct PartitionLayout {
    std::string name;               // e.g. "ab_standard_v1"
    std::string table_type;         // "gpt" or "mbr"
    uint32_t alignment_mib = 4;
    std::vector<PartitionSpec> partitions;
};

/* =========================================================================
 *  Boot Control (Architecture Doc §6.9)
 * ========================================================================= */

struct BootEnv {
    std::string active_slot;        // "A" or "B"
    std::string next_slot;
    bool upgrade_pending = false;
    int boot_attempts_left = 0;
    bool slot_a_good = true;
    bool slot_b_good = true;
};

/* =========================================================================
 *  Transaction Journal (Architecture Doc §6.11)
 * ========================================================================= */

enum class JournalState {
    None,
    VerifyPackage,
    CheckTarget,
    PreparePartitions,
    WriteBootloader,
    WriteKernel,
    WriteRootfs,
    RestoreConfig,
    VerifyTarget,
    ConfigureBoot,
    Finalize,
    Complete,
    Aborted
};

const char* journal_state_name(JournalState state);

struct JournalEntry {
    std::string transaction_id;             // UUID
    std::string operation;                  // "system_install", "backup", "restore"
    JournalState state = JournalState::None;
    int progress = 0;
    std::string target_device;
    std::string target_slot;                // "A" or "B"
    std::string package_version;
    std::string started_at;                 // ISO 8601
    std::string last_update_at;
    bool safe_to_resume = true;
    std::vector<std::string> completed_steps;
};

/* =========================================================================
 *  Job Context — passed through all job steps
 * ========================================================================= */

// Forward declarations for dependency injection
class IDeviceManager;
class IPackageManager;
class IImageWriter;
class IPartitionManager;
class IFilesystemManager;
class IBootControl;
class ISecurityManager;
class ITransactionJournal;
class ILogger;
class IProcessRunner;

struct JobContext {
    std::string job_id;
    std::string package_path;
    std::string target_device;
    std::string target_slot;        // "A" or "B"
    std::string backup_profile;
    std::string destination_path;

    // Injected services (non-owning pointers)
    IDeviceManager* device_mgr = nullptr;
    IPackageManager* package_mgr = nullptr;
    IImageWriter* image_writer = nullptr;
    IPartitionManager* part_mgr = nullptr;
    IFilesystemManager* fs_mgr = nullptr;
    IBootControl* boot_ctrl = nullptr;
    ISecurityManager* sec_mgr = nullptr;
    ITransactionJournal* journal = nullptr;
    ILogger* logger = nullptr;
    IProcessRunner* proc_runner = nullptr;
};

} // namespace installer

#endif // INSTALLER_CORE_TYPES_H
