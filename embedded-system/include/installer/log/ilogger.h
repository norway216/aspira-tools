/**
 * @file ilogger.h
 * @brief Structured JSON Lines logger interface.
 *
 * Provides a pure virtual interface for logging operations.
 * Implementations write structured JSON Lines (one JSON object per line)
 * to a configurable log file with per-entry metadata including timestamp,
 * level, component, message, and an optional job_id for correlation.
 *
 * @see Architecture Doc §18
 */

#ifndef INSTALLER_LOG_ILOGGER_H
#define INSTALLER_LOG_ILOGGER_H

#include <string>

#include "installer/core/types.h"

namespace installer {

/**
 * Structured JSON Lines logger interface.
 *
 * All log output is formatted as one JSON object per line (JSON Lines /
 * newline-delimited JSON). Each line contains at minimum: timestamp, level,
 * component, and message. An optional job_id field links log entries to a
 * specific installation job for traceability.
 *
 * Thread-safety: implementations must be safe for concurrent use from
 * multiple threads.
 */
class ILogger {
public:
    virtual ~ILogger() = default;

    /**
     * Log a free-form message at the given severity level.
     *
     * @param level     Severity level (Debug, Info, Warn, Error).
     * @param component Name of the originating module (e.g. "ImageWriter").
     * @param message   Human-readable log message.
     * @param job_id    Optional job identifier for cross-referencing log entries
     *                  with a specific installation job. Empty string = no job.
     */
    virtual void log(LogLevel level,
                     const std::string& component,
                     const std::string& message,
                     const std::string& job_id = "") = 0;

    /**
     * Log a structured event with a machine-readable event name and
     * associated JSON fields.
     *
     * Unlike log(), which takes free-form text, this method emits a
     * structured event record suitable for automated monitoring and
     * analysis.
     *
     * @param level       Severity level.
     * @param component   Name of the originating module.
     * @param event_name  Machine-readable event identifier
     *                    (e.g. "write_started", "verify_failed").
     * @param json_fields JSON object string containing event-specific
     *                    key/value pairs (default: empty object "{}").
     */
    virtual void log_event(LogLevel level,
                           const std::string& component,
                           const std::string& event_name,
                           const std::string& json_fields = "{}") = 0;

    /**
     * Set (or change) the output log file path.
     *
     * If a log file is already open, it is flushed and closed before the
     * new file is opened. A subsequent call to log() writes to the new
     * location.
     *
     * @param path Absolute filesystem path to the log file.
     */
    virtual void set_log_file(const std::string& path) = 0;

    /**
     * Flush all buffered log entries to persistent storage.
     *
     * Calls fsync() (or platform equivalent) so that all log data written
     * up to this point is durable on disk. Useful before a potentially
     * risky operation where log integrity is critical.
     */
    virtual void flush() = 0;

    // Convenience methods with shorter names
    void debug(const std::string& msg) { log(LogLevel::Debug, "", msg); }
    void info(const std::string& msg)  { log(LogLevel::Info,  "", msg); }
    void warn(const std::string& msg)  { log(LogLevel::Warn,  "", msg); }
    void error(const std::string& msg) { log(LogLevel::Error, "", msg); }
};

} // namespace installer

#endif // INSTALLER_LOG_ILOGGER_H
