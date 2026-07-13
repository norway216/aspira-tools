/**
 * @file itransaction_journal.h
 * @brief Transaction journal for power-loss recovery.
 *
 * Records the progress of long-running installation operations so that
 * they can be safely resumed after an unexpected power loss or crash.
 * Every write to the journal is atomic (temp file -> fsync -> rename ->
 * fsync parent directory) so that the on-disk state is never torn.
 *
 * @see Architecture Doc §6.11, §19
 */

#ifndef INSTALLER_JOURNAL_ITRANSACTION_JOURNAL_H
#define INSTALLER_JOURNAL_ITRANSACTION_JOURNAL_H

#include <string>
#include <vector>

#include "installer/core/result.h"
#include "installer/core/types.h"

namespace installer {

/**
 * Transaction journal for power-loss recovery.
 *
 * Each installation operation (system_install, backup, restore) is
 * tracked as a JournalEntry that records the current state, progress
 * percentage, completed steps, and whether the transaction is safe to
 * resume. After a power loss, find_incomplete() returns any transaction
 * that was interrupted, and the job manager can resume from the last
 * completed step.
 *
 * Atomicity guarantee: every write to the journal is performed as:
 *   1. Write to a temporary file.
 *   2. fsync() the temporary file.
 *   3. rename() the temporary file over the real journal file.
 *   4. fsync() the parent directory.
 * This ensures the on-disk state is never partially written.
 */
class ITransactionJournal {
public:
    virtual ~ITransactionJournal() = default;

    /**
     * Start a new transaction and record its initial journal entry.
     *
     * Generates a new UUID for the transaction and persists the entry
     * with state = None and progress = 0.
     *
     * @param entry The initial JournalEntry with at minimum operation,
     *              target_device, target_slot, package_version, and
     *              started_at populated. The transaction_id will be
     *              set by this method.
     * @return Result<void> — ok when the entry is safely on disk.
     */
    virtual Result<void> begin_transaction(const JournalEntry& entry) = 0;

    /**
     * Update the state and progress of an in-flight transaction.
     *
     * Called at the beginning of each high-level phase (VerifyPackage,
     * CheckTarget, PreparePartitions, etc.) to record progress.
     *
     * @param tx_id    Transaction identifier (UUID returned by
     *                 begin_transaction).
     * @param state    New journal state for this phase.
     * @param progress Overall progress percentage (0–100).
     * @return Result<void> — ok when the update is safely on disk.
     */
    virtual Result<void> update_state(const std::string& tx_id,
                                      JournalState state,
                                      int progress) = 0;

    /**
     * Mark a named step as completed within the transaction.
     *
     * Steps are finer-grained than states — a single JournalState may
     * encompass several steps. The completed_steps list in the journal
     * entry allows precise resumption from the exact point of failure.
     *
     * @param tx_id     Transaction identifier.
     * @param step_name Human-readable step name (e.g. "write_bootloader",
     *                  "sync_partition_table").
     * @return Result<void>.
     */
    virtual Result<void> mark_step_complete(
        const std::string& tx_id,
        const std::string& step_name) = 0;

    /**
     * Finalise a transaction as successfully completed.
     *
     * Sets the state to Complete and writes the final entry. After this
     * call, the transaction will no longer appear in find_incomplete().
     *
     * @param tx_id Transaction identifier.
     * @return Result<void>.
     */
    virtual Result<void> commit_transaction(const std::string& tx_id) = 0;

    /**
     * Mark a transaction as aborted.
     *
     * Sets the state to Aborted. Aborted transactions are not resumed
     * and can be garbage-collected.
     *
     * @param tx_id Transaction identifier.
     * @return Result<void>.
     */
    virtual Result<void> abort_transaction(const std::string& tx_id) = 0;

    /**
     * Find all transactions that were interrupted before completion.
     *
     * Returns every JournalEntry whose state is not Complete or Aborted.
     * The caller inspects safe_to_resume on each entry to decide whether
     * to attempt resumption.
     *
     * @return A list of incomplete journal entries, ordered by
     *         started_at (oldest first). Returns an empty vector if
     *         no incomplete transactions exist.
     */
    virtual std::vector<JournalEntry> find_incomplete() = 0;

    /**
     * Retrieve a specific transaction by its identifier.
     *
     * @param tx_id Transaction identifier.
     * @return The JournalEntry, or an error if tx_id is not found.
     */
    virtual Result<JournalEntry> get_transaction(
        const std::string& tx_id) = 0;
};

} // namespace installer

#endif // INSTALLER_JOURNAL_ITRANSACTION_JOURNAL_H
