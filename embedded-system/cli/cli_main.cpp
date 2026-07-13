/**
 * @file cli_main.cpp
 * @brief Entry point for the installer-cli command-line tool.
 *
 * Links only the IPC client library (not the full core service).
 * Commands:
 *   installer-cli device list
 *   installer-cli device info <device>
 *   installer-cli package verify <path>
 *   installer-cli install --package <path> --target <device> [--slot A|B]
 *   installer-cli backup create [--profile <name>]
 *   installer-cli restore --backup <path>
 *   installer-cli job status [--job-id <id>]
 *   installer-cli job cancel --job-id <id>
 *   installer-cli job list
 *   installer-cli logs [--tail <n>]
 *   installer-cli boot status
 *   installer-cli boot set-slot <A|B>
 *   installer-cli reboot
 *   installer-cli shutdown
 *   installer-cli ping
 *   installer-cli --help
 *   installer-cli --socket <path>
 *   installer-cli --json
 */

#include "cli_commands.h"

#include <iostream>
#include <cstring>
#include <spdlog/spdlog.h>
#include <spdlog/sinks/null_sink.h>

namespace {

void print_usage(const char* prog_name) {
    std::cout << "Embedded Linux Installer CLI Tool\n\n"
              << "Usage: " << prog_name << " [GLOBAL_OPTS] COMMAND [ARGS...]\n\n"
              << "Global Options:\n"
              << "  --socket PATH   Daemon socket path "
                 "(default: /var/run/installer.sock)\n"
              << "  --json          Output in JSON format\n"
              << "  --help, -h      Show this help\n\n"
              << "Commands:\n"
              << "  device list                        List block devices\n"
              << "  device info <device>               Show device details\n"
              << "  package verify <path>              Verify a .espkg file\n"
              << "  install --package <p> --target <d> Start installation\n"
              << "  backup create [--profile <n>]      Start backup job\n"
              << "  restore --backup <path>            Start restore job\n"
              << "  job status [--job-id <id>]         Get job status\n"
              << "  job cancel --job-id <id>           Cancel a job\n"
              << "  job list                           List all jobs\n"
              << "  logs [--tail <n>]                  Fetch recent logs\n"
              << "  boot status                        Show boot slot info\n"
              << "  boot set-slot <A|B>                Set next boot slot\n"
              << "  reboot                             Request system reboot\n"
              << "  shutdown                           Request system shutdown\n"
              << "  ping                               Check daemon liveness\n"
              << std::endl;
}

} // anonymous namespace

int main(int argc, char* argv[]) {
    using namespace installer::cli;

    if (argc < 2) {
        print_usage(argv[0]);
        return 1;
    }

    // ---- Parse global options ----
    CliOptions opts;
    int arg_idx = 1;

    for (; arg_idx < argc; ++arg_idx) {
        std::string a(argv[arg_idx]);

        if (a == "--help" || a == "-h") {
            print_usage(argv[0]);
            return 0;
        } else if (a == "--socket" && arg_idx + 1 < argc) {
            opts.socket_path = argv[++arg_idx];
        } else if (a == "--json") {
            opts.json_output = true;
        } else if (a == "--timeout" && arg_idx + 1 < argc) {
            opts.timeout = std::chrono::milliseconds(std::stoi(argv[++arg_idx]));
        } else {
            // End of global options
            break;
        }
    }

    if (arg_idx >= argc) {
        std::cerr << "Error: No command specified.\n";
        print_usage(argv[0]);
        return 1;
    }

    std::string command(argv[arg_idx++]);

    // Collect remaining arguments for the subcommand
    std::vector<std::string> cmd_args;
    for (; arg_idx < argc; ++arg_idx) {
        cmd_args.emplace_back(argv[arg_idx]);
    }

    // ---- Create a null logger for CLI (no log output) ----
    auto null_sink = std::make_shared<spdlog::sinks::null_sink_mt>();
    auto cli_logger = std::make_shared<spdlog::logger>("cli", null_sink);
    cli_logger->set_level(spdlog::level::off);

    // ---- Connect to daemon ----
    auto client_result = connect(opts, nullptr);
    if (client_result.is_err()) {
        print_error("Cannot connect to installer-core at " +
                    opts.socket_path + ": " +
                    client_result.error().user_message);
        print_error("Is the installer-core daemon running?");
        return 2;
    }

    auto& client = *client_result.value();

    // ---- Dispatch command ----
    int ret = 0;

    if (command == "device") {
        if (cmd_args.empty()) {
            print_error("Usage: device <list|info> ...");
            ret = 1;
        } else if (cmd_args[0] == "list") {
            ret = cmd_device_list(client, opts,
                                  {cmd_args.begin() + 1, cmd_args.end()});
        } else if (cmd_args[0] == "info") {
            ret = cmd_device_info(client, opts,
                                  {cmd_args.begin() + 1, cmd_args.end()});
        } else {
            print_error("Unknown device subcommand: " + cmd_args[0]);
            ret = 1;
        }
    } else if (command == "package") {
        if (cmd_args.empty() || cmd_args[0] != "verify") {
            print_error("Usage: package verify <path>");
            ret = 1;
        } else {
            ret = cmd_package_verify(client, opts,
                                     {cmd_args.begin() + 1, cmd_args.end()});
        }
    } else if (command == "install") {
        ret = cmd_install_start(client, opts, cmd_args);
    } else if (command == "backup") {
        if (cmd_args.empty() || cmd_args[0] != "create") {
            print_error("Usage: backup create [--profile <name>]");
            ret = 1;
        } else {
            ret = cmd_backup_create(client, opts,
                                    {cmd_args.begin() + 1, cmd_args.end()});
        }
    } else if (command == "restore") {
        ret = cmd_restore(client, opts, cmd_args);
    } else if (command == "job") {
        if (cmd_args.empty()) {
            print_error("Usage: job <status|cancel|list> ...");
            ret = 1;
        } else if (cmd_args[0] == "status") {
            ret = cmd_job_status(client, opts,
                                 {cmd_args.begin() + 1, cmd_args.end()});
        } else if (cmd_args[0] == "cancel") {
            ret = cmd_job_cancel(client, opts,
                                 {cmd_args.begin() + 1, cmd_args.end()});
        } else if (cmd_args[0] == "list") {
            ret = cmd_job_list(client, opts,
                               {cmd_args.begin() + 1, cmd_args.end()});
        } else {
            print_error("Unknown job subcommand: " + cmd_args[0]);
            ret = 1;
        }
    } else if (command == "logs") {
        ret = cmd_logs(client, opts, cmd_args);
    } else if (command == "boot") {
        if (cmd_args.empty()) {
            print_error("Usage: boot <status|set-slot> ...");
            ret = 1;
        } else if (cmd_args[0] == "status") {
            ret = cmd_boot_status(client, opts,
                                  {cmd_args.begin() + 1, cmd_args.end()});
        } else if (cmd_args[0] == "set-slot") {
            ret = cmd_boot_set_slot(client, opts,
                                    {cmd_args.begin() + 1, cmd_args.end()});
        } else {
            print_error("Unknown boot subcommand: " + cmd_args[0]);
            ret = 1;
        }
    } else if (command == "reboot") {
        ret = cmd_reboot(client, opts, cmd_args);
    } else if (command == "shutdown") {
        ret = cmd_shutdown(client, opts, cmd_args);
    } else if (command == "ping") {
        ret = cmd_ping(client, opts, cmd_args);
    } else {
        print_error("Unknown command: " + command);
        print_usage(argv[0]);
        ret = 1;
    }

    client.disconnect();
    return ret;
}
