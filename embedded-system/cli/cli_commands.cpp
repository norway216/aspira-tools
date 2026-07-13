/**
 * @file cli_commands.cpp
 * @brief Implementation of CLI commands.
 */

#include "cli_commands.h"
#include "src/ipc/ipc_client.h"
#include "src/ipc/ipc_protocol.h"

#include <algorithm>
#include <iomanip>
#include <iostream>
#include <sstream>

namespace installer {
namespace cli {

// =========================================================================
//  Helpers
// =========================================================================

Result<std::unique_ptr<IIPCClient>> connect(const CliOptions& opts,
                                            std::shared_ptr<ILogger> logger) {
    auto client = std::make_unique<ipc::UnixSocketJsonRpcClient>(std::move(logger));
    auto result = client->connect(opts.socket_path);
    if (result.is_err()) {
        return Result<std::unique_ptr<IIPCClient>>::err(result.take_error());
    }
    return Result<std::unique_ptr<IIPCClient>>::ok(std::move(client));
}

void print_error(const std::string& message) {
    std::cerr << "Error: " << message << std::endl;
}

void print_result(const nlohmann::json& result, const CliOptions& opts) {
    if (opts.json_output) {
        std::cout << result.dump(2) << std::endl;
        return;
    }

    if (result.is_array()) {
        print_table(result);
    } else if (result.is_object()) {
        // Print key-value pairs
        for (auto& [key, value] : result.items()) {
            if (value.is_boolean()) {
                std::cout << std::left << std::setw(24) << key
                          << (value.get<bool>() ? "yes" : "no") << "\n";
            } else if (value.is_string()) {
                std::cout << std::left << std::setw(24) << key
                          << value.get<std::string>() << "\n";
            } else {
                std::cout << std::left << std::setw(24) << key
                          << value.dump() << "\n";
            }
        }
    } else {
        std::cout << result.dump(2) << std::endl;
    }
}

void print_table(const nlohmann::json& array) {
    if (!array.is_array() || array.empty()) {
        std::cout << "(empty)\n";
        return;
    }

    // Collect all keys from all objects
    std::vector<std::string> keys;
    if (array[0].is_object()) {
        for (auto& [key, _] : array[0].items()) {
            keys.push_back(key);
        }
    }

    // Calculate column widths
    std::vector<size_t> widths(keys.size(), 0);
    for (size_t i = 0; i < keys.size(); ++i) {
        widths[i] = keys[i].size();
    }
    for (const auto& row : array) {
        if (!row.is_object()) continue;
        for (size_t i = 0; i < keys.size(); ++i) {
            std::string val;
            if (row.contains(keys[i])) {
                const auto& v = row[keys[i]];
                if (v.is_string()) {
                    val = v.get<std::string>();
                } else {
                    val = v.dump();
                }
            }
            widths[i] = std::max(widths[i], val.size());
        }
    }

    // Print header
    for (size_t i = 0; i < keys.size(); ++i) {
        std::cout << std::left << std::setw(static_cast<int>(widths[i] + 2))
                  << keys[i];
    }
    std::cout << "\n";

    // Print separator
    for (size_t w : widths) {
        std::cout << std::string(w, '-') << "  ";
    }
    std::cout << "\n";

    // Print rows
    for (const auto& row : array) {
        if (!row.is_object()) continue;
        for (size_t i = 0; i < keys.size(); ++i) {
            std::string val;
            if (row.contains(keys[i])) {
                const auto& v = row[keys[i]];
                if (v.is_string()) {
                    val = v.get<std::string>();
                } else {
                    val = v.dump();
                }
            }
            std::cout << std::left << std::setw(static_cast<int>(widths[i] + 2))
                      << val;
        }
        std::cout << "\n";
    }
}

// =========================================================================
//  Internal helper: call a method and handle errors
// =========================================================================

namespace {

int call_and_print(IIPCClient& client, const CliOptions& opts,
                   const std::string& method, const nlohmann::json& params) {
    auto result = client.call(method, params, opts.timeout);
    if (result.is_err()) {
        print_error(result.error().user_message);
        return 1;
    }
    print_result(result.value(), opts);
    return 0;
}

} // anonymous namespace

// =========================================================================
//  Commands
// =========================================================================

int cmd_device_list(IIPCClient& client, const CliOptions& opts,
                    const std::vector<std::string>& /*args*/) {
    return call_and_print(client, opts, ipc::methods::LIST_DEVICES,
                          nlohmann::json::object());
}

int cmd_device_info(IIPCClient& client, const CliOptions& opts,
                    const std::vector<std::string>& args) {
    if (args.empty()) {
        print_error("Usage: device info <device>");
        return 1;
    }
    nlohmann::json params;
    params["device"] = args[0];
    return call_and_print(client, opts, ipc::methods::GET_DEVICE_INFO, params);
}

int cmd_package_verify(IIPCClient& client, const CliOptions& opts,
                       const std::vector<std::string>& args) {
    if (args.empty()) {
        print_error("Usage: package verify <path>");
        return 1;
    }
    nlohmann::json params;
    params["package"] = args[0];
    return call_and_print(client, opts, ipc::methods::VERIFY_PACKAGE, params);
}

int cmd_install_start(IIPCClient& client, const CliOptions& opts,
                      const std::vector<std::string>& args) {
    std::string package;
    std::string target;
    std::string slot = "A";

    // Simple positional or --flag parsing for the subcommand
    for (size_t i = 0; i < args.size(); ++i) {
        if (args[i] == "--package" && i + 1 < args.size()) {
            package = args[++i];
        } else if (args[i] == "--target" && i + 1 < args.size()) {
            target = args[++i];
        } else if (args[i] == "--slot" && i + 1 < args.size()) {
            slot = args[++i];
        } else if (package.empty()) {
            package = args[i];
        } else if (target.empty()) {
            target = args[i];
        }
    }

    if (package.empty() || target.empty()) {
        print_error("Usage: install --package <path> --target <device> [--slot A|B]");
        return 1;
    }

    nlohmann::json params;
    params["package"] = package;
    params["target"]  = target;
    params["slot"]    = slot;
    return call_and_print(client, opts, ipc::methods::START_INSTALL, params);
}

int cmd_backup_create(IIPCClient& client, const CliOptions& opts,
                      const std::vector<std::string>& args) {
    std::string profile = "default";
    for (size_t i = 0; i < args.size(); ++i) {
        if (args[i] == "--profile" && i + 1 < args.size()) {
            profile = args[++i];
        }
    }
    nlohmann::json params;
    params["profile"] = profile;
    return call_and_print(client, opts, ipc::methods::START_BACKUP, params);
}

int cmd_restore(IIPCClient& client, const CliOptions& opts,
                const std::vector<std::string>& args) {
    std::string backup;
    for (size_t i = 0; i < args.size(); ++i) {
        if (args[i] == "--backup" && i + 1 < args.size()) {
            backup = args[++i];
        } else if (backup.empty()) {
            backup = args[i];
        }
    }
    if (backup.empty()) {
        print_error("Usage: restore --backup <path>");
        return 1;
    }
    nlohmann::json params;
    params["backup"] = backup;
    return call_and_print(client, opts, ipc::methods::START_RESTORE, params);
}

int cmd_job_status(IIPCClient& client, const CliOptions& opts,
                   const std::vector<std::string>& args) {
    std::string job_id;
    for (size_t i = 0; i < args.size(); ++i) {
        if (args[i] == "--job-id" && i + 1 < args.size()) {
            job_id = args[++i];
        }
    }
    nlohmann::json params;
    params["job_id"] = job_id;
    return call_and_print(client, opts, ipc::methods::GET_JOB_STATUS, params);
}

int cmd_job_cancel(IIPCClient& client, const CliOptions& opts,
                   const std::vector<std::string>& args) {
    std::string job_id;
    for (size_t i = 0; i < args.size(); ++i) {
        if (args[i] == "--job-id" && i + 1 < args.size()) {
            job_id = args[++i];
        }
    }
    if (job_id.empty()) {
        print_error("Usage: job cancel --job-id <id>");
        return 1;
    }
    nlohmann::json params;
    params["job_id"] = job_id;
    return call_and_print(client, opts, ipc::methods::CANCEL_JOB, params);
}

int cmd_job_list(IIPCClient& client, const CliOptions& opts,
                 const std::vector<std::string>& /*args*/) {
    return call_and_print(client, opts, ipc::methods::LIST_JOBS,
                          nlohmann::json::object());
}

int cmd_logs(IIPCClient& client, const CliOptions& opts,
             const std::vector<std::string>& args) {
    int tail = 50;
    for (size_t i = 0; i < args.size(); ++i) {
        if (args[i] == "--tail" && i + 1 < args.size()) {
            tail = std::stoi(args[++i]);
        }
    }
    nlohmann::json params;
    params["tail"] = tail;
    return call_and_print(client, opts, ipc::methods::GET_LOGS, params);
}

int cmd_boot_status(IIPCClient& client, const CliOptions& opts,
                    const std::vector<std::string>& /*args*/) {
    return call_and_print(client, opts, ipc::methods::BOOT_STATUS,
                          nlohmann::json::object());
}

int cmd_boot_set_slot(IIPCClient& client, const CliOptions& opts,
                      const std::vector<std::string>& args) {
    if (args.empty() || (args[0] != "A" && args[0] != "B")) {
        print_error("Usage: boot set-slot <A|B>");
        return 1;
    }
    nlohmann::json params;
    params["slot"] = args[0];
    return call_and_print(client, opts, ipc::methods::BOOT_SET_SLOT, params);
}

int cmd_reboot(IIPCClient& client, const CliOptions& opts,
               const std::vector<std::string>& /*args*/) {
    return call_and_print(client, opts, ipc::methods::REBOOT,
                          nlohmann::json::object());
}

int cmd_shutdown(IIPCClient& client, const CliOptions& opts,
                 const std::vector<std::string>& /*args*/) {
    return call_and_print(client, opts, ipc::methods::SHUTDOWN,
                          nlohmann::json::object());
}

int cmd_ping(IIPCClient& client, const CliOptions& opts,
             const std::vector<std::string>& /*args*/) {
    return call_and_print(client, opts, ipc::methods::PING,
                          nlohmann::json::object());
}

} // namespace cli
} // namespace installer
