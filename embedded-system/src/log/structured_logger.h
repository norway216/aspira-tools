/**
 * @file structured_logger.h
 * @brief JSON Lines logger implementation using spdlog.
 *
 * Outputs every log entry as a single-line JSON object.
 * Thread-safe: all public methods are protected by an internal mutex.
 */

#ifndef INSTALLER_LOG_STRUCTURED_LOGGER_H
#define INSTALLER_LOG_STRUCTURED_LOGGER_H

#include "installer/log/ilogger.h"
#include <cstddef>
#include <memory>

namespace installer {

/**
 * StructuredLogger — ILogger implementation backed by spdlog.
 *
 * Features:
 *  - JSON Lines output (one JSON object per line)
 *  - Dual sinks: rotating file (all levels) + stderr (WARN and above)
 *  - Log rotation support (max file size + max file count)
 *  - Thread-safe via internal mutex
 *
 * Usage:
 *   StructuredLogger logger;
 *   logger.set_log_file("/var/log/installer/installer.log");
 *   logger.log(LogLevel::Info, "MyComponent", "Hello");
 */
class StructuredLogger : public ILogger {
public:
    StructuredLogger();
    ~StructuredLogger() override;

    // ---- ILogger interface ----

    void log(LogLevel level, const std::string& component,
             const std::string& message,
             const std::string& job_id = "") override;

    void log_event(LogLevel level, const std::string& component,
                   const std::string& event_name,
                   const std::string& json_fields = "{}") override;

    Result<void> set_log_file(const std::string& path) override;

    void flush() override;

    // ---- Extended API ----

    /**
     * Set the log file with automatic rotation.
     *
     * @param path         Absolute path to the log file.
     * @param max_size_mb  Rotate when the file exceeds this size (default 10 MB).
     * @param max_files    Keep at most this many rotated files (default 5).
     * @return             Ok on success.
     */
    Result<void> set_log_file_with_rotation(const std::string& path,
                                            size_t max_size_mb = 10,
                                            int max_files = 5);

private:
    struct Impl;
    std::unique_ptr<Impl> pimpl_;
};

} // namespace installer

#endif // INSTALLER_LOG_STRUCTURED_LOGGER_H
