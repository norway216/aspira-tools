/**
 * @file ITransactionJournal.h
 * @brief Transaction journal for crash-safe operations.
 *
 * Every state transition of a long-running job is recorded atomically
 * so the system can resume after an unexpected power loss or crash.
 */

#ifndef INSTALLER_ITRANSACTIONJOURNAL_H
#define INSTALLER_ITRANSACTIONJOURNAL_H

#include "installer/core/types.h"
#include "installer/core/result.h"
#include <string>
#include <vector>

namespace installer {

class ITransactionJournal {
public:
    virtual ~ITransactionJournal() = default;

    virtual Result<void> begin(const std::string& transaction_id,
                               const std::string& operation) = 0;

    virtual Result<void> update(const JournalEntry& entry) = 0;

    virtual Result<void> commit(const std::string& transaction_id) = 0;

    virtual Result<void> abort(const std::string& transaction_id) = 0;

    virtual Result<JournalEntry> get(const std::string& transaction_id) = 0;

    virtual Result<std::vector<JournalEntry>> find_incomplete() = 0;

    virtual Result<std::vector<JournalEntry>> list_all() = 0;
};

} // namespace installer

#endif // INSTALLER_ITRANSACTIONJOURNAL_H
