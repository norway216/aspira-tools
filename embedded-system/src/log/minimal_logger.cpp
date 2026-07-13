/**
 * @file minimal_logger.cpp
 * @brief Minimal StructuredLogger implementation — zero external deps.
 */
#include "src/log/structured_logger.h"
#include <iostream>
#include <mutex>

namespace installer {

struct StructuredLogger::Impl {
    std::mutex mtx;
};

StructuredLogger::StructuredLogger() : pimpl_(std::make_unique<Impl>()) {}
StructuredLogger::~StructuredLogger() = default;

void StructuredLogger::log(LogLevel level, const std::string& component,
                           const std::string& message, const std::string& job_id) {
    std::lock_guard<std::mutex> lock(pimpl_->mtx);
    const char* lvl = "INFO";
    if (level == LogLevel::Debug) lvl = "DEBUG";
    else if (level == LogLevel::Warn) lvl = "WARN";
    else if (level == LogLevel::Error) lvl = "ERROR";
    std::cout << "[" << lvl << "] [" << component << "] " << message;
    if (!job_id.empty()) std::cout << " [job=" << job_id << "]";
    std::cout << std::endl;
}

void StructuredLogger::log_event(LogLevel level, const std::string& component,
                                  const std::string& event_name, const std::string& json_fields) {
    log(level, component, "event=" + event_name + " data=" + json_fields);
}

void StructuredLogger::set_log_file(const std::string&) { /* no-op */ }
void StructuredLogger::flush() { std::cout << std::flush; }

} // namespace installer
