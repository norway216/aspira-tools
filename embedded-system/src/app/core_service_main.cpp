/**
 * @file core_service_main.cpp
 * @brief Main entry point for the installer-core daemon.
 *
 * Parses command-line arguments, creates all services via ServiceSetup,
 * starts the IPC server, and waits for a termination signal.
 */

#include "service_setup.h"

#include <csignal>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <string>
#include <thread>
#include <unistd.h>

namespace {

// ---------------------------------------------------------------------------
// Signal handling
// ---------------------------------------------------------------------------

volatile sig_atomic_t g_shutdown_requested = 0;

void signal_handler(int signum) {
    g_shutdown_requested = 1;
}

bool install_signal_handlers() {
    struct sigaction sa{};
    sa.sa_handler = signal_handler;
    sa.sa_flags = 0;
    sigemptyset(&sa.sa_mask);

    if (sigaction(SIGTERM, &sa, nullptr) < 0) {
        std::cerr << "Failed to install SIGTERM handler: "
                  << std::strerror(errno) << std::endl;
        return false;
    }
    if (sigaction(SIGINT, &sa, nullptr) < 0) {
        std::cerr << "Failed to install SIGINT handler: "
                  << std::strerror(errno) << std::endl;
        return false;
    }

    // Ignore SIGPIPE so we don't crash if a client disconnects
    signal(SIGPIPE, SIG_IGN);

    return true;
}

// ---------------------------------------------------------------------------
// Command-line parsing
// ---------------------------------------------------------------------------

void print_usage(const char* prog_name) {
    std::cout << "Usage: " << prog_name << " [OPTIONS]\n"
              << "Embedded Linux Installer Core Service\n\n"
              << "Options:\n"
              << "  --config <path>   Path to installer.yaml\n"
              << "                     (default: /etc/installer/installer.yaml)\n"
              << "  --socket <path>    Unix domain socket path\n"
              << "                     (default: /var/run/installer.sock)\n"
              << "  --help             Show this help message\n"
              << std::endl;
}

struct CommandLineOptions {
    std::string config_path = "/etc/installer/installer.yaml";
    std::string socket_path = "/var/run/installer.sock";
};

CommandLineOptions parse_args(int argc, char* argv[]) {
    CommandLineOptions opts;

    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];

        if (arg == "--help" || arg == "-h") {
            print_usage(argv[0]);
            std::exit(0);
        } else if (arg == "--config" && i + 1 < argc) {
            opts.config_path = argv[++i];
        } else if (arg == "--socket" && i + 1 < argc) {
            opts.socket_path = argv[++i];
        } else {
            std::cerr << "Unknown option: " << arg << std::endl;
            print_usage(argv[0]);
            std::exit(1);
        }
    }

    return opts;
}

} // anonymous namespace

// ============================================================================
//  main
// ============================================================================

int main(int argc, char* argv[]) {
    // Parse command line
    auto opts = parse_args(argc, argv);

    // Install signal handlers
    if (!install_signal_handlers()) {
        return 1;
    }

    std::cout << "installer-core: starting up..." << std::endl;
    std::cout << "  Config: " << opts.config_path << std::endl;
    std::cout << "  Socket: " << opts.socket_path << std::endl;

    // Create all services via the composition root
    auto result = installer::ServiceSetup::create(opts.config_path);
    if (result.is_err()) {
        std::cerr << "FATAL: Failed to create services: "
                  << result.error().user_message << std::endl;
        std::cerr << "  Code: " << result.error().code << std::endl;
        if (!result.error().technical_message.empty()) {
            std::cerr << "  Detail: " << result.error().technical_message
                      << std::endl;
        }
        return 1;
    }

    auto services = result.take_value();

    // Start the IPC server
    auto start_result = services.ipc_server->start(opts.socket_path);
    if (start_result.is_err()) {
        std::cerr << "FATAL: Failed to start IPC server: "
                  << start_result.error().user_message << std::endl;
        installer::ServiceSetup::shutdown(services);
        return 1;
    }

    services.logger->log(installer::LogLevel::Info,
                         "main",
                         "installer-core ready on " + opts.socket_path);

    std::cout << "installer-core: ready on " << opts.socket_path << std::endl;

    // ---- Main loop: wait for shutdown signal ----
    while (!g_shutdown_requested) {
        // Sleep in short intervals to remain responsive to signals
        std::this_thread::sleep_for(std::chrono::milliseconds(500));

        // Check if IPC server is still running
        if (!services.ipc_server->is_running()) {
            services.logger->log(installer::LogLevel::Error,
                                 "main",
                                 "IPC server stopped unexpectedly");
            break;
        }
    }

    std::cout << "\ninstaller-core: shutting down..." << std::endl;

    // Graceful shutdown
    installer::ServiceSetup::shutdown(services);

    std::cout << "installer-core: goodbye." << std::endl;
    return 0;
}
