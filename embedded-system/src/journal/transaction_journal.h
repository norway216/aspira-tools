/**
 * @file transaction_journal.h
 * @brief Power-loss-safe transaction journal backed by an atomic JSON file.
 *
 * Every mutation is written via write-to-.partial -> fsync -> rename -> fsync(dir)
 * to guarantee that the journal is never left in a corrupted state, even if
 * power is lost mid-write.
 */

#ifndef INSTALLER_TRANSACTION_JOURNAL_H
#define INSTALLER_TRANSACTION_JOURNAL_H

#include "installer/ITransactionJournal.h"
#include "installer/log/ilogger.h"

#include <memory>
#include <mutex>
#include <shared_mutex>
#include <string>
#include <vector>

namespace installer {

class TransactionJournal : public ITransactionJournal {
public:
    explicit TransactionJournal(const std::string& journal_dir,
                                std::shared_ptr<ILogger> logger);

    ~TransactionJournal() override;

    // ---- ITransactionJournal interface ----

    Result<void> begin(const std::string& transaction_id,
                       const std::string& operation) override;

    Result<void> update(const JournalEntry& entry) override;

    Result<void> commit(const std::string& transaction_id) override;

    Result<void> abort(const std::string& transaction_id) override;

    Result<JournalEntry> get(const std::string& transaction_id) override;

    Result<std::vector<JournalEntry>> find_incomplete() override;

    Result<std::vector<JournalEntry>> list_all() override;

    // ---- Convenience methods (kept for BaseJob compatibility) ----

    // Update progress and state for a transaction.
    Result<void> update_state(const std::string& tx_id,
                              JournalState state, int progress);

    // Mark a named step as complete.
    Result<void> mark_step_complete(const std::string& tx_id,
                                    const std::string& step_name);

private:
    // Write all in-memory entries to journal.json atomically.
    Result<void> atomic_save();

    // Full path to journal.json.
    std::string journal_path() const;

    // Load existing entries from disk. Called once during construction.
    Result<void> load_from_disk();

    // Find an entry by transaction_id; returns nullptr if not found.
    JournalEntry* find_entry(const std::string& tx_id);

    // Update timestamp helper.
    static std::string now_iso8601();

    struct Impl {
        std::string journal_dir;
        std::vector<JournalEntry> entries;
        std::shared_ptr<ILogger> logger;
        mutable std::shared_mutex mutex;
    };
    std::unique_ptr<Impl> impl_;
};

} // namespace installer

#endif // INSTALLER_TRANSACTION_JOURNAL_H
