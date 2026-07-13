/**
 * @file iboot_control.h
 * @brief A/B boot slot management interface.
 *
 * Controls the U-Boot (or equivalent) boot environment for A/B
 * seamless updates. Manages active/inactive slot selection, slot
 * health marking, upgrade-pending flags, and recovery boot requests.
 *
 * @see Architecture Doc §6.9, §24
 */

#ifndef INSTALLER_BOOT_IBOOT_CONTROL_H
#define INSTALLER_BOOT_IBOOT_CONTROL_H

#include <string>

#include "installer/core/result.h"
#include "installer/core/types.h"

namespace installer {

/**
 * A/B boot slot management interface.
 *
 * Wraps the platform's boot environment (U-Boot fw_env on eMMC boot
 * partitions, EFI variables, or a raw environment block) to provide
 * atomic A/B slot control. The BootEnv struct captures the full state
 * of the boot environment at a point in time.
 *
 * Implementations must guarantee that commit_boot_env() produces a
 * durable write (fsync + rename on the underlying storage).
 */
class IBootControl {
public:
    virtual ~IBootControl() = default;

    /**
     * Read the complete boot environment.
     *
     * Returns a snapshot of all boot-control variables: active slot,
     * next-slot override, upgrade-pending flag, boot attempt counters,
     * and per-slot health (good/bad).
     *
     * @return A populated BootEnv structure, or BOOT_ENV_WRITE_FAILED
     *         if the environment cannot be read.
     */
    virtual Result<BootEnv> get_boot_env() = 0;

    /**
     * Set the currently active boot slot.
     *
     * Updates the "active" variable in the boot environment. This
     * typically happens after a successful boot into the new slot.
     *
     * @param slot Slot identifier ("A" or "B").
     * @return Result<void> — ok on success, BOOT_SLOT_INVALID if
     *         the slot identifier is not recognised.
     */
    virtual Result<void> set_active_slot(const std::string& slot) = 0;

    /**
     * Set the slot that will be booted on the NEXT reboot.
     *
     * This is a one-shot override — the bootloader typically resets
     * this variable after consuming it. Used to direct a reboot into
     * a specific slot for upgrade or recovery.
     *
     * @param slot Slot identifier ("A" or "B").
     * @return Result<void>.
     */
    virtual Result<void> set_next_slot(const std::string& slot) = 0;

    /**
     * Mark a slot as successfully booted ("good").
     *
     * Resets the boot-attempt counter for the slot and marks it as
     * healthy. Should be called by userspace after the system has
     * fully initialised following a successful boot into that slot.
     *
     * @param slot Slot identifier ("A" or "B").
     * @return Result<void> — ok on success, BOOT_MARK_FAILED on error.
     */
    virtual Result<void> mark_slot_good(const std::string& slot) = 0;

    /**
     * Set or clear the upgrade-pending flag.
     *
     * When true, the bootloader knows that an upgrade is in progress
     * and may alter its boot-attempt / fallback behaviour accordingly.
     *
     * @param pending true to set the flag, false to clear it.
     * @return Result<void>.
     */
    virtual Result<void> set_upgrade_pending(bool pending) = 0;

    /**
     * Get the identifier of the currently active boot slot.
     *
     * Convenience method that reads only the active-slot variable
     * without fetching the entire BootEnv.
     *
     * @return "A" or "B", or an error.
     */
    virtual Result<std::string> get_current_slot() = 0;

    /**
     * Request that the bootloader enter recovery mode on the next boot.
     *
     * Sets the appropriate bootloader variable to trigger recovery
     * (e.g. a ramdisk-based recovery image or a dedicated recovery
     * partition).
     *
     * @return Result<void>.
     */
    virtual Result<void> request_recovery_boot() = 0;

    /**
     * Commit all pending boot environment changes to persistent storage.
     *
     * Writes the complete environment atomically to the underlying
     * storage medium and ensures durability (fsync). Until this method
     * is called, changes made via the other methods may only exist
     * in memory.
     *
     * @return Result<void> — ok when written and sync'd,
     *         BOOT_ENV_WRITE_FAILED on error.
     */
    virtual Result<void> commit_boot_env() = 0;
};

} // namespace installer

#endif // INSTALLER_BOOT_IBOOT_CONTROL_H
