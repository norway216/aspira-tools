/**
 * @file ipc_protocol.h
 * @brief JSON-RPC 2.0 method and event name constants.
 *
 * All method names that the installer-core daemon exposes and all
 * event/signal names that it pushes to clients are defined here as
 * constexpr string constants to avoid typos and enable IDE completion.
 */

#ifndef INSTALLER_IPC_PROTOCOL_H
#define INSTALLER_IPC_PROTOCOL_H

namespace installer {
namespace ipc {

// ---- JSON-RPC method names exposed by the daemon ----
namespace methods {
    constexpr const char* LIST_DEVICES      = "device.list";
    constexpr const char* GET_DEVICE_INFO   = "device.info";
    constexpr const char* VERIFY_PACKAGE    = "package.verify";
    constexpr const char* START_INSTALL     = "install.start";
    constexpr const char* START_BACKUP      = "backup.start";
    constexpr const char* START_RESTORE     = "restore.start";
    constexpr const char* CANCEL_JOB        = "job.cancel";
    constexpr const char* GET_JOB_STATUS    = "job.status";
    constexpr const char* GET_JOB_PROGRESS  = "job.progress";
    constexpr const char* LIST_JOBS         = "job.list";
    constexpr const char* GET_LOGS          = "logs.get";
    constexpr const char* BOOT_STATUS       = "boot.status";
    constexpr const char* BOOT_SET_SLOT     = "boot.set_slot";
    constexpr const char* REBOOT            = "system.reboot";
    constexpr const char* SHUTDOWN          = "system.shutdown";
    constexpr const char* PING              = "ping";
} // namespace methods

// ---- JSON-RPC event/signal names pushed from daemon to clients ----
namespace events {
    constexpr const char* JOB_STATE_CHANGED    = "job.state_changed";
    constexpr const char* JOB_PROGRESS_CHANGED = "job.progress_changed";
    constexpr const char* JOB_LOG              = "job.log";
    constexpr const char* DEVICE_ADDED         = "device.added";
    constexpr const char* DEVICE_REMOVED       = "device.removed";
} // namespace events

} // namespace ipc
} // namespace installer

#endif // INSTALLER_IPC_PROTOCOL_H
