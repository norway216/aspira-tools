/**
 * @file boot_control.h
 * @brief BootControl implementation of IBootControl using U-Boot fw_printenv/fw_setenv.
 *
 * Manages the U-Boot environment variables that control A/B boot slot
 * selection, upgrade pending flags, and boot attempt counters.
 * Communicates with the U-Boot environment via the fw_printenv and
 * fw_setenv command-line tools, executed through IProcessRunner.
 */

#ifndef INSTALLER_CORE_BOOT_CONTROL_H
#define INSTALLER_CORE_BOOT_CONTROL_H

#include "installer/boot/iboot_control.h"
#include "installer/core/types.h"
#include "installer/core/result.h"
#include "installer/platform/iprocess_runner.h"
#include <string>
#include <map>
#include <mutex>

namespace installer {

class BootControl : public IBootControl {
public:
    /**
     * Construct a BootControl instance.
     *
     * @param proc_runner   Process runner for executing fw_printenv/fw_setenv.
     *                      Must outlive this object. Must not be null.
     * @param fw_env_config Path to the U-Boot fw_env.config file describing
     *                      the MTD device and environment offsets.
     */
    explicit BootControl(IProcessRunner* proc_runner,
                         const std::string& fw_env_config = "/etc/fw_env.config");
    ~BootControl() override;

    // ---- IBootControl interface ----

    Result<BootEnv> get_boot_env() override;
    Result<void> write_boot_env(const BootEnv& env);
    Result<void> set_active_slot(const std::string& slot) override;
    Result<void> set_upgrade_pending(bool pending) override;
    Result<void> mark_slot_good(const std::string& slot) override;
    Result<std::string> get_inactive_slot();
    Result<std::string> get_current_slot() override;
    Result<void> set_next_slot(const std::string& slot) override;
    Result<void> request_recovery_boot() override;
    Result<void> commit_boot_env() override;

    // ---- Extended API ----

    /**
     * Write a single U-Boot environment variable immediately.
     *
     * Calls `fw_setenv -c <config> <key> <value>`.
     * Updates the internal active-slot cache when @p key is "active_slot".
     */
    Result<void> write_boot_var(const std::string& key, const std::string& value);

    /**
     * Commit an entire BootEnv to the U-Boot environment in a batch
     * (script mode).  Semantically equivalent to write_boot_env().
     */
    Result<void> commit_boot_env(const BootEnv& env);

    /**
     * Return the currently active slot ("A" or "B").
     *
     * Returns the cached value when available; otherwise reads the
     * U-Boot environment and caches the result.
     */
    Result<std::string> get_active_slot();

private:
    /**
     * Run fw_printenv and parse its stdout into a key-value map.
     *
     * Each line of output is parsed via parse_env_line().
     * Returns an error when the tool is unavailable or the output
     * cannot be read.
     */
    Result<std::map<std::string, std::string>> read_all_vars();

    /**
     * Write a batch of variables via fw_setenv script mode.
     *
     * Writes key=value pairs to a temporary file and invokes
     * `fw_setenv -c <config> -s <temp_file>`.  The temporary file
     * is always removed before this function returns.
     */
    Result<void> write_vars(const std::map<std::string, std::string>& vars);

    /**
     * Parse a single "key=value" line from fw_printenv output.
     *
     * Splits on the first '=' character and trims whitespace from
     * both key and value.  Lines without '=' are treated as
     * key=<empty>.
     */
    static std::pair<std::string, std::string> parse_env_line(const std::string& line);

    /**
     * Run a fw_printenv or fw_setenv command with the given arguments.
     *
     * @param args       Command and arguments; args[0] is the executable.
     * @param timeout_ms Timeout in milliseconds (default 5000).
     * @return           The populated ProcessResult, or an error if the
     *                   process runner is unavailable.
     */
    Result<ProcessResult> run_fw_tool(const std::vector<std::string>& args,
                                      int timeout_ms = 5000);

    /**
     * Validate that @p slot is "A" or "B" (case-insensitive).
     * Returns the normalized uppercase slot on success, or an error.
     */
    static Result<std::string> validate_and_normalize_slot(const std::string& slot);

    IProcessRunner* proc_runner_;
    std::string fw_env_config_;
    std::string cache_active_slot_;   // guarded by mutex_
    mutable std::mutex mutex_;
};

} // namespace installer

#endif // INSTALLER_CORE_BOOT_CONTROL_H
