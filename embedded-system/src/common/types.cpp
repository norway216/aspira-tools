/**
 * @file types.cpp
 * @brief Implementations of enum-to-string conversion functions declared in types.h.
 */

#include "installer/core/types.h"

namespace installer {

const char* job_type_name(JobType type) {
    switch (type) {
    case JobType::InstallSystem:  return "InstallSystem";
    case JobType::UpgradeSystem:  return "UpgradeSystem";
    case JobType::BackupData:     return "BackupData";
    case JobType::RestoreData:    return "RestoreData";
    case JobType::RepairSystem:   return "RepairSystem";
    case JobType::VerifyPackage:  return "VerifyPackage";
    case JobType::ExportLogs:      return "ExportLogs";
    }
    return "Unknown";
}

const char* job_state_name(JobState state) {
    switch (state) {
    case JobState::Idle:           return "Idle";
    case JobState::Preparing:      return "Preparing";
    case JobState::Running:        return "Running";
    case JobState::Paused:         return "Paused";
    case JobState::Cancelling:     return "Cancelling";
    case JobState::Completed:      return "Completed";
    case JobState::Failed:         return "Failed";
    case JobState::Recoverable:    return "Recoverable";
    case JobState::RebootRequired: return "RebootRequired";
    }
    return "Unknown";
}

const char* log_level_name(LogLevel level) {
    switch (level) {
    case LogLevel::Debug: return "DEBUG";
    case LogLevel::Info:  return "INFO";
    case LogLevel::Warn:  return "WARN";
    case LogLevel::Error: return "ERROR";
    }
    return "UNKNOWN";
}

const char* journal_state_name(JournalState state) {
    switch (state) {
    case JournalState::None:              return "None";
    case JournalState::VerifyPackage:     return "VerifyPackage";
    case JournalState::CheckTarget:       return "CheckTarget";
    case JournalState::PreparePartitions: return "PreparePartitions";
    case JournalState::WriteBootloader:   return "WriteBootloader";
    case JournalState::WriteKernel:       return "WriteKernel";
    case JournalState::WriteRootfs:       return "WriteRootfs";
    case JournalState::RestoreConfig:     return "RestoreConfig";
    case JournalState::VerifyTarget:      return "VerifyTarget";
    case JournalState::ConfigureBoot:     return "ConfigureBoot";
    case JournalState::Finalize:          return "Finalize";
    case JournalState::Complete:          return "Complete";
    case JournalState::Aborted:           return "Aborted";
    }
    return "Unknown";
}

} // namespace installer
