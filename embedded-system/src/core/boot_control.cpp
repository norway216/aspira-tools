/**
 * @file boot_control.cpp
 * @brief Implementation of BootControl using U-Boot fw_printenv/fw_setenv.
 *
 * Design:
 *  - All U-Boot environment reads go through fw_printenv -c <config>.
 *  - Single-variable writes use fw_setenv -c <config> <key> <value>.
 *  - Batch writes use fw_setenv -c <config> -s <script_file> for
 *    transactional (or near-transactional) updates.
 *  - The active slot is cached after the first read to avoid repeated
 *    subprocess invocations.
 *  - All public methods are thread-safe (guarded by mutex_).
 *
 * NEVER writes to the MTD device directly — all access goes through the
 * upstream U-Boot tools to guarantee format compatibility.
 */

#include "boot_control.h"

#include "installer/platform/iprocess_runner.h"

#include <algorithm>
#include <cctype>
#include <cerrno>
#include <chrono>
#include <cstring>
#include <map>
#include <sstream>
#include <string>
#include <unistd.h>
#include <vector>

namespace installer {

// =============================================================================
// Anonymous helpers
// =============================================================================

namespace {

/**
 * Build a uniform InstallerError for boot-control operations.
 *
 * @param code     One of the ErrorCode::BOOT_* constants (or INTERNAL_ERROR).
 * @param title    Short human-readable title.
 * @param user_msg Message suitable for display to the end user.
 * @param tech_msg Optional technical detail for logs / debugging.
 */
InstallerError make_error(const std::string& code,
                          const std::string& title,
                          const std::string& user_msg,
                          const std::string& tech_msg = "") {
    return InstallerError::make(code, title, user_msg, tech_msg);
}

/**
 * Trim leading and trailing whitespace from a string (in-place).
 * Whitespace includes space, tab, carriage-return, and newline.
 */
void trim_inplace(std::string& s) {
    // Left trim
    s.erase(s.begin(), std::find_if(s.begin(), s.end(), [](unsigned char ch) {
        return !std::isspace(ch);
    }));
    // Right trim
    s.erase(std::find_if(s.rbegin(), s.rend(), [](unsigned char ch) {
        return !std::isspace(ch);
    }).base(), s.end());
}

/**
 * Remove a file, ignoring errors (best-effort cleanup).
 */
void remove_file_quiet(const std::string& path) {
    ::unlink(path.c_str());
}

} // anonymous namespace

// =============================================================================
// Construction / Destruction
// =============================================================================

BootControl::BootControl(IProcessRunner* proc_runner,
                         const std::string& fw_env_config)
    : proc_runner_(proc_runner)
    , fw_env_config_(fw_env_config)
{
    // proc_runner_ may be validated lazily on first use, but a null pointer
    // here almost certainly indicates a programming error early in startup.
    // We do not throw (project convention: no exceptions), but callers
    // should ensure the pointer is valid before constructing.
}

BootControl::~BootControl() = default;

// =============================================================================
// Static helpers
// =============================================================================

std::pair<std::string, std::string>
BootControl::parse_env_line(const std::string& line) {
    auto pos = line.find('=');
    if (pos == std::string::npos) {
        // No '=' found — treat the whole line as a key with an empty value
        std::string key = line;
        trim_inplace(key);
        return {key, ""};
    }

    std::string key   = line.substr(0, pos);
    std::string value = line.substr(pos + 1);

    trim_inplace(key);
    trim_inplace(value);

    return {key, value};
}

Result<std::string>
BootControl::validate_and_normalize_slot(const std::string& slot) {
    if (slot.empty()) {
        return Result<std::string>::err(make_error(
            ErrorCode::BOOT_SLOT_INVALID,
            "Invalid Slot",
            "Slot name is empty.",
            "slot argument is an empty string"));
    }

    // Case-insensitive: accept "a", "A", "b", "B"
    std::string normalized = slot;
    std::transform(normalized.begin(), normalized.end(), normalized.begin(),
                   [](unsigned char c) { return static_cast<char>(std::toupper(c)); });

    if (normalized != "A" && normalized != "B") {
        return Result<std::string>::err(make_error(
            ErrorCode::BOOT_SLOT_INVALID,
            "Invalid Slot",
            "Slot must be 'A' or 'B', got '" + slot + "'.",
            "slot value '" + slot + "' is not a valid A/B slot identifier"));
    }

    return Result<std::string>::ok(normalized);
}

// =============================================================================
// Internal helpers
// =============================================================================

Result<ProcessResult>
BootControl::run_fw_tool(const std::vector<std::string>& args,
                         int timeout_ms) {
    if (proc_runner_ == nullptr) {
        return Result<ProcessResult>::err(make_error(
            ErrorCode::INTERNAL_ERROR,
            "Process Runner Unavailable",
            "The process runner is not initialised.",
            "proc_runner_ is null"));
    }

    CancellationToken cancel;   // not cancellable from outside
    return proc_runner_->run(args,
                             std::chrono::milliseconds(timeout_ms),
                             &cancel);
}

Result<std::map<std::string, std::string>>
BootControl::read_all_vars() {
    // Build argument list: fw_printenv -c <config>
    std::vector<std::string> args;
    args.reserve(3);
    args.push_back("fw_printenv");
    args.push_back("-c");
    args.push_back(fw_env_config_);

    auto result = run_fw_tool(args, 5000);
    if (!result.is_ok()) {
        return Result<std::map<std::string, std::string>>::err(
            result.take_error());
    }

    const auto& proc = result.value();

    // fw_printenv exits non-zero when the environment is inaccessible
    if (proc.exit_code != 0) {
        return Result<std::map<std::string, std::string>>::err(make_error(
            ErrorCode::INTERNAL_ERROR,
            "Boot Environment Read Failed",
            "Failed to read U-Boot environment variables.",
            "fw_printenv exited with code " + std::to_string(proc.exit_code)
                + ": " + proc.stderr_data));
    }

    if (proc.timed_out) {
        return Result<std::map<std::string, std::string>>::err(make_error(
            ErrorCode::INTERNAL_TIMEOUT,
            "Boot Environment Read Timeout",
            "Timed out while reading U-Boot environment.",
            "fw_printenv timed out after 5000 ms"));
    }

    // Parse each line of stdout into the map
    std::map<std::string, std::string> vars;
    std::istringstream stream(proc.stdout_data);
    std::string line;

    while (std::getline(stream, line)) {
        // Skip completely empty lines (blank lines in output)
        if (line.empty()) {
            continue;
        }

        auto [key, value] = parse_env_line(line);

        // Skip lines with an empty key (should not happen with valid output)
        if (key.empty()) {
            continue;
        }

        vars[key] = value;
    }

    // If the map is empty but the tool succeeded, that is suspicious
    // (a valid U-Boot environment always has at least some variables).
    // We treat it as a soft warning rather than an error — the caller
    // will use defaults for every field.

    return Result<std::map<std::string, std::string>>::ok(std::move(vars));
}

Result<void>
BootControl::write_vars(const std::map<std::string, std::string>& vars) {
    if (vars.empty()) {
        // Nothing to write — succeed trivially
        return Result<void>::ok();
    }

    // Build the script content: one "key=value" per line
    std::string script;
    for (const auto& [key, value] : vars) {
        script += key;
        script += '=';
        script += value;
        script += '\n';
    }

    // Create a unique temporary file using mkstemp to avoid collisions
    // when write_vars() is called concurrently.
    std::string temp_template = "/tmp/fw_env_batch_XXXXXX";
    std::vector<char> temp_buf(temp_template.begin(), temp_template.end());
    temp_buf.push_back('\0');

    int fd = ::mkstemp(temp_buf.data());
    if (fd < 0) {
        return Result<void>::err(make_error(
            ErrorCode::BOOT_ENV_WRITE_FAILED,
            "Temporary File Error",
            "Failed to create temporary file for U-Boot environment script.",
            "mkstemp failed: " + std::string(std::strerror(errno))));
    }

    // Write script content to the fd
    ssize_t written = ::write(fd, script.data(), script.size());
    ::close(fd);

    if (written < 0 || static_cast<size_t>(written) != script.size()) {
        std::string temp_path(temp_buf.data());
        remove_file_quiet(temp_path);
        return Result<void>::err(make_error(
            ErrorCode::BOOT_ENV_WRITE_FAILED,
            "Temporary File Error",
            "Failed to write temporary U-Boot environment script.",
            "write to " + temp_path + " failed: " + std::string(std::strerror(errno))));
    }

    std::string temp_path(temp_buf.data());

    // Run fw_setenv -c <config> -s <temp_file>
    std::vector<std::string> args;
    args.reserve(5);
    args.push_back("fw_setenv");
    args.push_back("-c");
    args.push_back(fw_env_config_);
    args.push_back("-s");
    args.push_back(temp_path);

    auto result = run_fw_tool(args, 5000);

    // Always remove the temporary file, even on error
    remove_file_quiet(temp_path);

    if (!result.is_ok()) {
        return Result<void>::err(result.take_error());
    }

    const auto& proc = result.value();

    if (proc.exit_code != 0) {
        return Result<void>::err(make_error(
            ErrorCode::BOOT_ENV_WRITE_FAILED,
            "Boot Environment Write Failed",
            "Failed to write U-Boot environment variables.",
            "fw_setenv -s exited with code " + std::to_string(proc.exit_code)
                + ": " + proc.stderr_data));
    }

    if (proc.timed_out) {
        return Result<void>::err(make_error(
            ErrorCode::INTERNAL_TIMEOUT,
            "Boot Environment Write Timeout",
            "Timed out while writing U-Boot environment.",
            "fw_setenv -s timed out after 5000 ms"));
    }

    return Result<void>::ok();
}

// =============================================================================
// IBootControl interface
// =============================================================================

Result<BootEnv>
BootControl::read_boot_env() {
    auto vars_result = read_all_vars();
    if (!vars_result.is_ok()) {
        return Result<BootEnv>::err(vars_result.take_error());
    }

    const auto& vars = vars_result.value();

    BootEnv env;

    // ---- active_slot (default "A") ----
    auto it = vars.find("active_slot");
    if (it != vars.end() && !it->second.empty()) {
        env.active_slot = it->second;
    } else {
        env.active_slot = "A";
    }

    // ---- next_slot (default "") ----
    it = vars.find("next_slot");
    if (it != vars.end()) {
        env.next_slot = it->second;
    }

    // ---- upgrade_pending ("1" -> true, otherwise false) ----
    it = vars.find("upgrade_pending");
    if (it != vars.end()) {
        env.upgrade_pending = (it->second == "1");
    }

    // ---- boot_attempts_left (parse int, default 0) ----
    it = vars.find("boot_attempts_left");
    if (it != vars.end() && !it->second.empty()) {
        try {
            env.boot_attempts_left = std::stoi(it->second);
        } catch (const std::exception&) {
            env.boot_attempts_left = 0;
        }
    }

    // ---- slot_a_good ("0" -> false, otherwise true, default true) ----
    it = vars.find("slot_a_good");
    if (it != vars.end()) {
        env.slot_a_good = (it->second != "0");
    }

    // ---- slot_b_good ("0" -> false, otherwise true, default true) ----
    it = vars.find("slot_b_good");
    if (it != vars.end()) {
        env.slot_b_good = (it->second != "0");
    }

    // Cache the active slot for fast access via get_active_slot()
    {
        std::lock_guard<std::mutex> lock(mutex_);
        cache_active_slot_ = env.active_slot;
    }

    return Result<BootEnv>::ok(std::move(env));
}

Result<void>
BootControl::write_boot_env(const BootEnv& env) {
    // Delegates to the batch-write path
    return commit_boot_env(env);
}

Result<void>
BootControl::set_active_slot(const std::string& slot) {
    auto normalized = validate_and_normalize_slot(slot);
    if (!normalized.is_ok()) {
        return Result<void>::err(normalized.take_error());
    }

    return write_boot_var("active_slot", normalized.value());
}

Result<void>
BootControl::set_upgrade_pending(bool pending) {
    return write_boot_var("upgrade_pending", pending ? "1" : "0");
}

Result<void>
BootControl::mark_slot_good(const std::string& slot) {
    auto normalized = validate_and_normalize_slot(slot);
    if (!normalized.is_ok()) {
        return Result<void>::err(normalized.take_error());
    }

    const std::string& s = normalized.value();

    // Write slot_X_good = 1
    std::string var_name = (s == "A") ? "slot_a_good" : "slot_b_good";
    auto result = write_boot_var(var_name, "1");
    if (!result.is_ok()) {
        return result;
    }

    // Reset boot_attempts_left to a reasonable default (3) so the
    // bootloader does not fall back unnecessarily on the next boot.
    return write_boot_var("boot_attempts_left", "3");
}

Result<std::string>
BootControl::get_inactive_slot() {
    auto active = get_active_slot();
    if (!active.is_ok()) {
        return active;   // propagate error
    }

    const std::string& a = active.value();

    if (a == "A") {
        return Result<std::string>::ok("B");
    }
    if (a == "B") {
        return Result<std::string>::ok("A");
    }

    // If the active slot is something unexpected, fall back to "B".
    // This should be rare — active_slot is validated/coerced during reads.
    return Result<std::string>::ok("B");
}

// =============================================================================
// Extended API
// =============================================================================

Result<void>
BootControl::write_boot_var(const std::string& key,
                            const std::string& value) {
    // Build argument list: fw_setenv -c <config> <key> <value>
    std::vector<std::string> args;
    args.reserve(5);
    args.push_back("fw_setenv");
    args.push_back("-c");
    args.push_back(fw_env_config_);
    args.push_back(key);
    args.push_back(value);

    auto result = run_fw_tool(args, 5000);
    if (!result.is_ok()) {
        return Result<void>::err(result.take_error());
    }

    const auto& proc = result.value();

    if (proc.exit_code != 0) {
        return Result<void>::err(make_error(
            ErrorCode::BOOT_ENV_WRITE_FAILED,
            "Boot Variable Write Failed",
            "Failed to write '" + key + "' to U-Boot environment.",
            "fw_setenv " + key + " exited with code "
                + std::to_string(proc.exit_code)
                + ": " + proc.stderr_data));
    }

    if (proc.timed_out) {
        return Result<void>::err(make_error(
            ErrorCode::INTERNAL_TIMEOUT,
            "Boot Variable Write Timeout",
            "Timed out writing '" + key + "' to U-Boot environment.",
            "fw_setenv " + key + " timed out after 5000 ms"));
    }

    // Update the active-slot cache when the active_slot variable changes
    if (key == "active_slot") {
        std::lock_guard<std::mutex> lock(mutex_);
        cache_active_slot_ = value;
    }

    return Result<void>::ok();
}

Result<void>
BootControl::commit_boot_env(const BootEnv& env) {
    std::map<std::string, std::string> vars;

    vars["active_slot"]        = env.active_slot;
    vars["next_slot"]          = env.next_slot;
    vars["upgrade_pending"]    = env.upgrade_pending ? "1" : "0";
    vars["boot_attempts_left"] = std::to_string(env.boot_attempts_left);
    vars["slot_a_good"]        = env.slot_a_good ? "1" : "0";
    vars["slot_b_good"]        = env.slot_b_good ? "1" : "0";

    auto result = write_vars(vars);

    // Update the active-slot cache on success
    if (result.is_ok()) {
        std::lock_guard<std::mutex> lock(mutex_);
        cache_active_slot_ = env.active_slot;
    }

    return result;
}

Result<void>
BootControl::set_next_boot_slot(const std::string& slot) {
    auto normalized = validate_and_normalize_slot(slot);
    if (!normalized.is_ok()) {
        return Result<void>::err(normalized.take_error());
    }

    const std::string& s = normalized.value();

    // Write next_slot
    auto result = write_boot_var("next_slot", s);
    if (!result.is_ok()) {
        return result;
    }

    // Mark that an upgrade/switch is pending so the bootloader knows to
    // perform the slot switch and decrement boot_attempts_left.
    return write_boot_var("upgrade_pending", "1");
}

Result<std::string>
BootControl::get_active_slot() {
    // Fast path: return cached value
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!cache_active_slot_.empty()) {
            return Result<std::string>::ok(cache_active_slot_);
        }
    }

    // Slow path: read the environment and populate the cache
    auto env_result = read_boot_env();
    if (!env_result.is_ok()) {
        return Result<std::string>::err(env_result.take_error());
    }

    return Result<std::string>::ok(env_result.value().active_slot);
}

} // namespace installer
