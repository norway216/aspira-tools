/**
 * @file transaction_journal.cpp
 * @brief Implementation of the power-loss-safe transaction journal.
 *
 * Atomicity guarantee:
 *   1. Serialize all entries to JSON.
 *   2. Write to journal.json.partial.
 *   3. fsync() the partial file.
 *   4. rename() partial over target.
 *   5. fsync() the containing directory.
 */

#include "src/journal/transaction_journal.h"

#include <cerrno>
#include <chrono>
#include <cstdio>
#include <ctime>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <fstream>
#include <sstream>
#include <iomanip>

#include <nlohmann/json.hpp>

using json = nlohmann::json;

namespace installer {

namespace {

// RAII file-descriptor wrapper.
class ScopedFd {
public:
    explicit ScopedFd(int fd) : fd_(fd) {}
    ~ScopedFd() { if (fd_ >= 0) close(fd_); }
    ScopedFd(const ScopedFd&) = delete;
    ScopedFd& operator=(const ScopedFd&) = delete;
    ScopedFd(ScopedFd&& o) noexcept : fd_(o.fd_) { o.fd_ = -1; }
    ScopedFd& operator=(ScopedFd&& o) noexcept {
        if (this != &o) { if (fd_ >= 0) close(fd_); fd_ = o.fd_; o.fd_ = -1; }
        return *this;
    }
    int get() const { return fd_; }
    bool valid() const { return fd_ >= 0; }
private:
    int fd_ = -1;
};

// ---- JournalState serialization ----
std::string to_string(JournalState s) {
    switch (s) {
        case JournalState::None:               return "none";
        case JournalState::VerifyPackage:      return "verify_package";
        case JournalState::CheckTarget:        return "check_target";
        case JournalState::PreparePartitions:  return "prepare_partitions";
        case JournalState::WriteBootloader:    return "write_bootloader";
        case JournalState::WriteKernel:        return "write_kernel";
        case JournalState::WriteRootfs:        return "write_rootfs";
        case JournalState::RestoreConfig:      return "restore_config";
        case JournalState::VerifyTarget:       return "verify_target";
        case JournalState::ConfigureBoot:      return "configure_boot";
        case JournalState::Finalize:           return "finalize";
        case JournalState::Complete:           return "complete";
        case JournalState::Aborted:            return "aborted";
    }
    return "unknown";
}

JournalState from_string(const std::string& s) {
    if (s == "none")               return JournalState::None;
    if (s == "verify_package")     return JournalState::VerifyPackage;
    if (s == "check_target")       return JournalState::CheckTarget;
    if (s == "prepare_partitions") return JournalState::PreparePartitions;
    if (s == "write_bootloader")   return JournalState::WriteBootloader;
    if (s == "write_kernel")       return JournalState::WriteKernel;
    if (s == "write_rootfs")       return JournalState::WriteRootfs;
    if (s == "restore_config")     return JournalState::RestoreConfig;
    if (s == "verify_target")      return JournalState::VerifyTarget;
    if (s == "configure_boot")     return JournalState::ConfigureBoot;
    if (s == "finalize")           return JournalState::Finalize;
    if (s == "complete")           return JournalState::Complete;
    if (s == "aborted")            return JournalState::Aborted;
    return JournalState::None;
}

json entry_to_json(const JournalEntry& e) {
    return {
        {"transaction_id",  e.transaction_id},
        {"operation",       e.operation},
        {"state",           to_string(e.state)},
        {"progress",        e.progress},
        {"target_device",   e.target_device},
        {"target_slot",     e.target_slot},
        {"package_version", e.package_version},
        {"started_at",      e.started_at},
        {"last_update_at",  e.last_update_at},
        {"safe_to_resume",  e.safe_to_resume},
        {"completed_steps", e.completed_steps}
    };
}

JournalEntry entry_from_json(const json& j) {
    JournalEntry e;
    e.transaction_id   = j.value("transaction_id", "");
    e.operation        = j.value("operation", "");
    e.state            = from_string(j.value("state", "none"));
    e.progress         = j.value("progress", 0);
    e.target_device    = j.value("target_device", "");
    e.target_slot      = j.value("target_slot", "");
    e.package_version  = j.value("package_version", "");
    e.started_at       = j.value("started_at", "");
    e.last_update_at   = j.value("last_update_at", "");
    e.safe_to_resume   = j.value("safe_to_resume", true);
    if (j.contains("completed_steps") && j["completed_steps"].is_array()) {
        for (const auto& s : j["completed_steps"])
            e.completed_steps.push_back(s.get<std::string>());
    }
    return e;
}

std::string now_iso8601() {
    auto now = std::chrono::system_clock::now();
    auto tt = std::chrono::system_clock::to_time_t(now);
    auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
        now.time_since_epoch()) % 1000;
    std::tm tm_buf;
    localtime_r(&tt, &tm_buf);
    std::ostringstream oss;
    oss << std::put_time(&tm_buf, "%Y-%m-%dT%H:%M:%S")
        << '.' << std::setfill('0') << std::setw(3) << ms.count()
        << std::put_time(&tm_buf, "%z");
    return oss.str();
}

} // anonymous namespace

// ============================================================================
//  Construction
// ============================================================================

TransactionJournal::TransactionJournal(const std::string& journal_dir,
                                       std::shared_ptr<ILogger> logger)
    : impl_(std::make_unique<Impl>())
{
    impl_->journal_dir = journal_dir;
    impl_->logger = std::move(logger);

    static const char* comp = "TransactionJournal";
    impl_->logger->log(LogLevel::Info, comp,
                       "Initializing journal_dir=" + journal_dir);

    // Ensure journal directory exists.
    struct stat st;
    if (stat(journal_dir.c_str(), &st) != 0 && errno == ENOENT) {
        if (mkdir(journal_dir.c_str(), 0755) != 0) {
            impl_->logger->log(LogLevel::Error, comp,
                               "Failed to create journal_dir: " +
                               std::string(strerror(errno)));
        }
    }

    auto result = load_from_disk();
    if (!result.is_ok()) {
        impl_->logger->log(LogLevel::Error, comp,
                           "Failed to load journal: " +
                           result.error().technical_message);
    }

    auto incomplete = find_incomplete();
    if (incomplete.is_ok() && !incomplete.value().empty()) {
        impl_->logger->log(LogLevel::Warn, comp,
                           "Found " +
                           std::to_string(incomplete.value().size()) +
                           " incomplete transaction(s) on startup");
        for (const auto& e : incomplete.value()) {
            impl_->logger->log(LogLevel::Warn, comp,
                               "  tx_id=" + e.transaction_id +
                               " state=" + journal_state_name(e.state) +
                               " progress=" + std::to_string(e.progress) +
                               "% safe=" + (e.safe_to_resume ? "true" : "false"));
        }
    } else {
        impl_->logger->log(LogLevel::Info, comp,
                           "No incomplete transactions found");
    }
}

TransactionJournal::~TransactionJournal() = default;

// ============================================================================
//  ITransactionJournal interface
// ============================================================================

Result<void> TransactionJournal::begin(const std::string& transaction_id,
                                        const std::string& operation) {
    std::unique_lock<std::shared_mutex> lock(impl_->mutex);

    for (const auto& e : impl_->entries) {
        if (e.transaction_id == transaction_id) {
            return Result<void>::err(InstallerError::make(
                ErrorCode::INTERNAL_ERROR,
                "Duplicate Transaction",
                "A transaction with this ID already exists",
                "tx_id=" + transaction_id));
        }
    }

    JournalEntry entry;
    entry.transaction_id = transaction_id;
    entry.operation = operation;
    entry.state = JournalState::None;
    entry.progress = 0;
    entry.safe_to_resume = true;
    entry.started_at = now_iso8601();
    entry.last_update_at = entry.started_at;
    impl_->entries.push_back(entry);

    impl_->logger->log(LogLevel::Info, "TransactionJournal",
                       "begin: " + operation + " target=" + entry.target_device,
                       transaction_id);

    return atomic_save();
}

Result<void> TransactionJournal::update(const JournalEntry& entry) {
    std::unique_lock<std::shared_mutex> lock(impl_->mutex);

    auto* existing = find_entry(entry.transaction_id);
    if (!existing) {
        return Result<void>::err(InstallerError::make(
            ErrorCode::INTERNAL_INVALID_STATE,
            "Transaction Not Found",
            "No transaction with the given ID",
            "tx_id=" + entry.transaction_id));
    }

    // Preserve started_at from existing entry if not set in update.
    if (entry.started_at.empty())
        const_cast<JournalEntry&>(entry).started_at = existing->started_at;

    *existing = entry;
    existing->last_update_at = now_iso8601();

    impl_->logger->log(LogLevel::Debug, "TransactionJournal",
                       "update: state=" + std::string(journal_state_name(entry.state)) +
                       " progress=" + std::to_string(entry.progress),
                       entry.transaction_id);

    return atomic_save();
}

Result<void> TransactionJournal::commit(const std::string& transaction_id) {
    std::unique_lock<std::shared_mutex> lock(impl_->mutex);

    auto* entry = find_entry(transaction_id);
    if (!entry) {
        return Result<void>::err(InstallerError::make(
            ErrorCode::INTERNAL_INVALID_STATE,
            "Transaction Not Found",
            "No transaction with the given ID",
            "tx_id=" + transaction_id));
    }

    entry->state = JournalState::Complete;
    entry->progress = 100;
    entry->last_update_at = now_iso8601();

    impl_->logger->log(LogLevel::Info, "TransactionJournal",
                       "commit: transaction completed successfully",
                       transaction_id);

    return atomic_save();
}

Result<void> TransactionJournal::abort(const std::string& transaction_id) {
    std::unique_lock<std::shared_mutex> lock(impl_->mutex);

    auto* entry = find_entry(transaction_id);
    if (!entry) {
        return Result<void>::err(InstallerError::make(
            ErrorCode::INTERNAL_INVALID_STATE,
            "Transaction Not Found",
            "No transaction with the given ID",
            "tx_id=" + transaction_id));
    }

    entry->state = JournalState::Aborted;
    entry->last_update_at = now_iso8601();

    impl_->logger->log(LogLevel::Warn, "TransactionJournal",
                       "abort: transaction aborted", transaction_id);

    return atomic_save();
}

Result<JournalEntry> TransactionJournal::get(const std::string& transaction_id) {
    std::shared_lock<std::shared_mutex> lock(impl_->mutex);

    auto* entry = find_entry(transaction_id);
    if (!entry) {
        return Result<JournalEntry>::err(InstallerError::make(
            ErrorCode::INTERNAL_INVALID_STATE,
            "Transaction Not Found",
            "No transaction with the given ID",
            "tx_id=" + transaction_id));
    }
    return Result<JournalEntry>::ok(*entry);
}

Result<std::vector<JournalEntry>> TransactionJournal::find_incomplete() {
    std::shared_lock<std::shared_mutex> lock(impl_->mutex);

    std::vector<JournalEntry> result;
    for (const auto& e : impl_->entries) {
        if (e.state != JournalState::Complete &&
            e.state != JournalState::Aborted) {
            result.push_back(e);
        }
    }
    return Result<std::vector<JournalEntry>>::ok(std::move(result));
}

Result<std::vector<JournalEntry>> TransactionJournal::list_all() {
    std::shared_lock<std::shared_mutex> lock(impl_->mutex);
    return Result<std::vector<JournalEntry>>::ok(impl_->entries);
}

// ============================================================================
//  Convenience methods
// ============================================================================

Result<void> TransactionJournal::update_state(const std::string& tx_id,
                                               JournalState state,
                                               int progress) {
    std::unique_lock<std::shared_mutex> lock(impl_->mutex);

    auto* entry = find_entry(tx_id);
    if (!entry) {
        return Result<void>::err(InstallerError::make(
            ErrorCode::INTERNAL_INVALID_STATE,
            "Transaction Not Found",
            "No transaction with the given ID",
            "tx_id=" + tx_id));
    }

    entry->state = state;
    entry->progress = progress;
    entry->last_update_at = now_iso8601();

    impl_->logger->log(LogLevel::Debug, "TransactionJournal",
                       "update_state: state=" +
                       std::string(journal_state_name(state)) +
                       " progress=" + std::to_string(progress), tx_id);

    return atomic_save();
}

Result<void> TransactionJournal::mark_step_complete(const std::string& tx_id,
                                                     const std::string& step_name) {
    std::unique_lock<std::shared_mutex> lock(impl_->mutex);

    auto* entry = find_entry(tx_id);
    if (!entry) {
        return Result<void>::err(InstallerError::make(
            ErrorCode::INTERNAL_INVALID_STATE,
            "Transaction Not Found",
            "No transaction with the given ID",
            "tx_id=" + tx_id));
    }

    for (const auto& s : entry->completed_steps) {
        if (s == step_name) return Result<void>::ok();  // already marked
    }

    entry->completed_steps.push_back(step_name);
    entry->last_update_at = now_iso8601();

    impl_->logger->log(LogLevel::Info, "TransactionJournal",
                       "mark_step_complete: " + step_name, tx_id);

    return atomic_save();
}

// ============================================================================
//  Private helpers
// ============================================================================

Result<void> TransactionJournal::atomic_save() {
    json root = json::array();
    for (const auto& e : impl_->entries) {
        root.push_back(entry_to_json(e));
    }

    std::string serialized = root.dump(2);
    std::string path = journal_path();
    std::string partial_path = path + ".partial";

    // Step 1: Open partial file.
    ScopedFd fd(open(partial_path.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644));
    if (!fd.valid()) {
        return Result<void>::err(InstallerError::make(
            ErrorCode::INTERNAL_ERROR,
            "Journal Write Error",
            "Failed to open partial journal file",
            "path=" + partial_path + " errno=" + std::to_string(errno)));
    }

    // Step 2: Write all data.
    const char* data = serialized.c_str();
    size_t remaining = serialized.size();
    while (remaining > 0) {
        ssize_t written = write(fd.get(), data, remaining);
        if (written < 0) {
            if (errno == EINTR) continue;
            return Result<void>::err(InstallerError::make(
                ErrorCode::INTERNAL_ERROR,
                "Journal Write Error",
                "Failed to write partial journal file",
                "path=" + partial_path + " errno=" + std::to_string(errno)));
        }
        data += written;
        remaining -= static_cast<size_t>(written);
    }

    // Step 3: fsync partial.
    if (fsync(fd.get()) != 0) {
        return Result<void>::err(InstallerError::make(
            ErrorCode::INTERNAL_ERROR,
            "Journal Sync Error",
            "Failed to fsync partial journal file",
            "path=" + partial_path + " errno=" + std::to_string(errno)));
    }

    // Step 4: Close fd, then rename.
    fd = ScopedFd(-1);

    if (rename(partial_path.c_str(), path.c_str()) != 0) {
        return Result<void>::err(InstallerError::make(
            ErrorCode::INTERNAL_ERROR,
            "Journal Rename Error",
            "Failed to rename partial journal file",
            "from=" + partial_path + " to=" + path +
            " errno=" + std::to_string(errno)));
    }

    // Step 5: fsync directory.
    ScopedFd dir_fd(open(impl_->journal_dir.c_str(), O_RDONLY | O_DIRECTORY));
    if (dir_fd.valid()) {
        fsync(dir_fd.get());
    }

    return Result<void>::ok();
}

std::string TransactionJournal::journal_path() const {
    if (impl_->journal_dir.empty()) return "journal.json";
    if (impl_->journal_dir.back() == '/')
        return impl_->journal_dir + "journal.json";
    return impl_->journal_dir + "/journal.json";
}

Result<void> TransactionJournal::load_from_disk() {
    std::string path = journal_path();
    std::ifstream ifs(path);
    if (!ifs.is_open()) {
        impl_->logger->log(LogLevel::Info, "TransactionJournal",
                           "No existing journal file at " + path);
        return Result<void>::ok();
    }

    try {
        json root;
        ifs >> root;

        if (!root.is_array()) {
            return Result<void>::err(InstallerError::make(
                ErrorCode::INTERNAL_CONFIG_ERROR,
                "Journal Parse Error",
                "Journal file is not a JSON array", "path=" + path));
        }

        impl_->entries.clear();
        for (const auto& je : root) {
            impl_->entries.push_back(entry_from_json(je));
        }

        impl_->logger->log(LogLevel::Info, "TransactionJournal",
                           "Loaded " + std::to_string(impl_->entries.size()) +
                           " entries from " + path);
    } catch (const json::exception& e) {
        return Result<void>::err(InstallerError::make(
            ErrorCode::INTERNAL_CONFIG_ERROR,
            "Journal Parse Error",
            "Failed to parse journal JSON",
            "path=" + path + " error=" + std::string(e.what())));
    }

    return Result<void>::ok();
}

JournalEntry* TransactionJournal::find_entry(const std::string& tx_id) {
    for (auto& e : impl_->entries) {
        if (e.transaction_id == tx_id) return &e;
    }
    return nullptr;
}

} // namespace installer
