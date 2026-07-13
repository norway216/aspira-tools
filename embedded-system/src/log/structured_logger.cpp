/**
 * @file structured_logger.cpp
 * @brief Implementation of StructuredLogger using spdlog and JSON Lines output.
 *
 * Internal design:
 *  - Two spdlog sinks are created:
 *      1. A rotating_file_sink_mt for all log levels.
 *      2. A stdout_color_sink_mt for WARN and above.
 *  - A custom formatter is used that expects the caller to pass a
 *    pre-formatted JSON string as the log message; the formatter appends
 *    a newline.
 *  - A std::mutex guards the JSON construction + spdlog call so that
 *    multi-line JSON is never interleaved.
 */

#include "structured_logger.h"

#include "installer/core/result.h"
#include "installer/core/types.h"

#include <spdlog/sinks/rotating_file_sink.h>
#include <spdlog/sinks/stdout_color_sinks.h>
#include <spdlog/spdlog.h>

#include <chrono>
#include <ctime>
#include <iomanip>
#include <mutex>
#include <sstream>
#include <stdexcept>

namespace installer {

// =============================================================================
// Custom spdlog formatter — outputs the payload as-is plus a newline.
//
// The StructuredLogger::Impl pre-formats the entire JSON line and passes it
// as the log message.  This formatter simply emits that payload followed by
// a line-feed.  This way we avoid fighting spdlog's built-in pattern syntax
// and can produce exactly the JSON we want.
// =============================================================================

class JsonLinesFormatter : public spdlog::formatter {
public:
    void format(const spdlog::details::log_msg& msg,
                spdlog::memory_buf_t& dest) override {
        dest.append(msg.payload.data(), msg.payload.data() + msg.payload.size());
        dest.push_back('\n');     // JSON Lines — one object per physical line
    }

    std::unique_ptr<spdlog::formatter> clone() const override {
        return std::make_unique<JsonLinesFormatter>();
    }
};

// =============================================================================
// PIMPL — hides spdlog headers and state from the public header.
// =============================================================================

struct StructuredLogger::Impl {
    std::shared_ptr<spdlog::logger> logger;
    std::mutex mtx;                       // serialises log() / log_event() calls
    bool file_sink_active = false;

    Impl() {
        // Start with a stderr-only logger so log output is never lost.
        auto stderr_sink = std::make_shared<spdlog::sinks::stdout_color_sink_mt>();
        stderr_sink->set_level(spdlog::level::warn);

        logger = std::make_shared<spdlog::logger>("installer", stderr_sink);
        logger->set_formatter(std::make_unique<JsonLinesFormatter>());
        logger->set_level(spdlog::level::debug);   // accept all; sinks filter
        logger->flush_on(spdlog::level::err);      // auto-flush on errors
    }
};

// =============================================================================
// Helpers
// =============================================================================

namespace {

/** Map our LogLevel enum to spdlog::level::level_enum. */
spdlog::level::level_enum to_spdlog_level(LogLevel lv) {
    switch (lv) {
    case LogLevel::Debug: return spdlog::level::debug;
    case LogLevel::Info:  return spdlog::level::info;
    case LogLevel::Warn:  return spdlog::level::warn;
    case LogLevel::Error: return spdlog::level::err;
    }
    return spdlog::level::info;
}

/** Return the current time as an ISO-8601 UTC string. */
std::string iso8601_utc_now() {
    using namespace std::chrono;
    auto now = system_clock::now();
    auto tt = system_clock::to_time_t(now);
    auto ms = duration_cast<milliseconds>(now.time_since_epoch()) % 1000;

    std::tm tm;
    ::gmtime_r(&tt, &tm);   // thread-safe gmtime

    std::ostringstream oss;
    oss << std::put_time(&tm, "%Y-%m-%dT%H:%M:%S")
        << '.' << std::setfill('0') << std::setw(3) << ms.count() << 'Z';
    return oss.str();
}

/** Minimal JSON string escaping. */
std::string json_escape(const std::string& s) {
    std::string out;
    out.reserve(s.size() + 8);
    for (char ch : s) {
        switch (ch) {
        case '"':  out += "\\\""; break;
        case '\\': out += "\\\\"; break;
        case '\n': out += "\\n";  break;
        case '\r': out += "\\r";  break;
        case '\t': out += "\\t";  break;
        default:
            if (static_cast<unsigned char>(ch) < 0x20) {
                out += "\\u00";
                out += "0123456789abcdef"[(ch >> 4) & 0xf];
                out += "0123456789abcdef"[ch & 0xf];
            } else {
                out += ch;
            }
            break;
        }
    }
    return out;
}

/** Build the JSON Line string for a log entry. */
std::string build_json_line(LogLevel level,
                            const std::string& component,
                            const std::string& job_id,
                            const std::string& event_name,
                            const std::string& message) {
    std::ostringstream oss;
    oss << "{\"time\":\"" << iso8601_utc_now()
        << "\",\"level\":\"" << log_level_name(level)
        << "\",\"job\":\"" << json_escape(job_id)
        << "\",\"component\":\"" << json_escape(component)
        << "\",\"event\":\"" << json_escape(event_name)
        << "\",\"message\":\"" << json_escape(message)
        << "\"}";
    return oss.str();
}

} // anonymous namespace

// =============================================================================
// StructuredLogger
// =============================================================================

StructuredLogger::StructuredLogger()
    : pimpl_(std::make_unique<Impl>()) {}

StructuredLogger::~StructuredLogger() {
    flush();
}

void StructuredLogger::log(LogLevel level,
                           const std::string& component,
                           const std::string& message,
                           const std::string& job_id) {
    // Pre-format the JSON line under the lock so spdlog simply emits it.
    std::string json_line = build_json_line(level, component, job_id, "", message);

    {
        std::lock_guard<std::mutex> lock(pimpl_->mtx);
        pimpl_->logger->log(to_spdlog_level(level), "{}", json_line);
    }
}

void StructuredLogger::log_event(LogLevel level,
                                 const std::string& component,
                                 const std::string& event_name,
                                 const std::string& json_fields) {
    std::string json_line =
        build_json_line(level, component, "", event_name, json_fields);

    {
        std::lock_guard<std::mutex> lock(pimpl_->mtx);
        pimpl_->logger->log(to_spdlog_level(level), "{}", json_line);
    }
}

void StructuredLogger::set_log_file(const std::string& path) {
    // Default rotation: 10 MB, keep 5 files
    return set_log_file_with_rotation(path, 10, 5);
}

void StructuredLogger::set_log_file_with_rotation(
    const std::string& path, size_t max_size_mb, int max_files) {
    try {
        // Build a rotating file sink
        auto file_sink = std::make_shared<spdlog::sinks::rotating_file_sink_mt>(
            path, max_size_mb * 1024 * 1024, max_files);
        file_sink->set_level(spdlog::level::debug); // accept all levels

        // Reuse the existing stderr sink or make a fresh one
        auto stderr_sink = std::make_shared<spdlog::sinks::stdout_color_sink_mt>();
        stderr_sink->set_level(spdlog::level::warn);

        spdlog::sinks_init_list sinks = {file_sink, stderr_sink};

        auto new_logger = std::make_shared<spdlog::logger>("installer",
                                                           sinks.begin(),
                                                           sinks.end());
        new_logger->set_formatter(std::make_unique<JsonLinesFormatter>());
        new_logger->set_level(spdlog::level::debug);
        new_logger->flush_on(spdlog::level::err);

        {
            std::lock_guard<std::mutex> lock(pimpl_->mtx);
            // Flush the old logger before replacing it
            if (pimpl_->logger) {
                pimpl_->logger->flush();
            }
            pimpl_->logger = new_logger;
            pimpl_->file_sink_active = true;
        }

        return;
    } catch (const spdlog::spdlog_ex& e) {
        std::cerr << "[ERROR] Log file configuration failed" << std::endl; return; // InstallerError::make(
            ErrorCode::INTERNAL_CONFIG_ERROR,
            "Log File Error",
            "Failed to set log file: " + std::string(e.what()),
            e.what(),
            false, false));
    } catch (const std::exception& e) {
        std::cerr << "[ERROR] Log file configuration failed" << std::endl; return; // InstallerError::make(
            ErrorCode::INTERNAL_ERROR,
            "Log Initialization Failed",
            "Unexpected error setting up log file: " + std::string(e.what()),
            e.what(),
            false, false));
    }
}

void StructuredLogger::flush() {
    std::lock_guard<std::mutex> lock(pimpl_->mtx);
    if (pimpl_->logger) {
        pimpl_->logger->flush();
    }
}

} // namespace installer
