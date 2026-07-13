/**
 * @file cli_commands.h
 * @brief CLI command implementations for the installer CLI tool.
 *
 * Each command connects to the installer-core daemon via IPC,
 * calls the appropriate JSON-RPC method, and prints the result.
 */

#ifndef INSTALLER_CLI_COMMANDS_H
#define INSTALLER_CLI_COMMANDS_H

#include "installer/IIPCClient.h"
#include "installer/ILogger.h"
#include "installer/core/result.h"
#include <memory>
#include <string>
#include <vector>

namespace installer {
namespace cli {

/**
 * Global CLI options that apply to all commands.
 */
struct CliOptions {
    std::string socket_path = "/var/run/installer.sock";
    std::chrono::milliseconds timeout{5000};
    bool json_output = false;
};

/**
 * Connect to the daemon, returning a ready client.
 */
Result<std::unique_ptr<IIPCClient>> connect(const CliOptions& opts,
                                            std::shared_ptr<ILogger> logger);

/**
 * Print an error to stderr.
 */
void print_error(const std::string& message);

/**
 * Print a JSON value; if opts.json_output is true print raw JSON,
 * otherwise pretty-print as a table or formatted text.
 */
void print_result(const nlohmann::json& result, const CliOptions& opts);

/**
 * Print a horizontal table from a JSON array of objects.
 * Keys are taken from the first object.
 */
void print_table(const nlohmann::json& array);

// ---- Command functions ----

int cmd_device_list(IIPCClient& client, const CliOptions& opts,
                    const std::vector<std::string>& args);

int cmd_device_info(IIPCClient& client, const CliOptions& opts,
                    const std::vector<std::string>& args);

int cmd_package_verify(IIPCClient& client, const CliOptions& opts,
                       const std::vector<std::string>& args);

int cmd_install_start(IIPCClient& client, const CliOptions& opts,
                      const std::vector<std::string>& args);

int cmd_backup_create(IIPCClient& client, const CliOptions& opts,
                      const std::vector<std::string>& args);

int cmd_restore(IIPCClient& client, const CliOptions& opts,
                const std::vector<std::string>& args);

int cmd_job_status(IIPCClient& client, const CliOptions& opts,
                   const std::vector<std::string>& args);

int cmd_job_cancel(IIPCClient& client, const CliOptions& opts,
                   const std::vector<std::string>& args);

int cmd_job_list(IIPCClient& client, const CliOptions& opts,
                 const std::vector<std::string>& args);

int cmd_logs(IIPCClient& client, const CliOptions& opts,
             const std::vector<std::string>& args);

int cmd_boot_status(IIPCClient& client, const CliOptions& opts,
                    const std::vector<std::string>& args);

int cmd_boot_set_slot(IIPCClient& client, const CliOptions& opts,
                      const std::vector<std::string>& args);

int cmd_reboot(IIPCClient& client, const CliOptions& opts,
               const std::vector<std::string>& args);

int cmd_shutdown(IIPCClient& client, const CliOptions& opts,
                 const std::vector<std::string>& args);

int cmd_ping(IIPCClient& client, const CliOptions& opts,
             const std::vector<std::string>& args);

} // namespace cli
} // namespace installer

#endif // INSTALLER_CLI_COMMANDS_H
