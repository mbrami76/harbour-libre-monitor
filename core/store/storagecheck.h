// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Michele Brami
#ifndef LM_STORAGECHECK_H
#define LM_STORAGECHECK_H

#include "store/journal.h"
#include "store/store.h"

#include <functional>

namespace lm {

// The weekly storage check (REQ-STO-01, REQ-STO-02, REQ-SCH-14). It measures the size of the
// store and the journal, asks for the free space at the store's directory, compares them, and
// records the result in the service journal as a `storage-check` entry (provisional kind).
// The journal is pruned first (REQ-SCH-14: at the weekly check), so the size it
// records is the size after pruning.
//
// Scheduling belongs: it calls runStorageCheck from the running service, for
// example daily, and the function itself keeps to once a week. Showing the notice belongs to
// The user interface: it reads lastStorageCheck and points to compaction, with the estimate of what
// compaction would free and of the space its rebuild needs (compactor.h, estimateCompaction;
// REQ-STO-02, REQ-STO-13, REQ-STO-16).

const int kStorageCheckIntervalDays = 7;        // REQ-STO-01: once a week, not more often
const int kLowSpaceFactor = 3;                  // REQ-STO-02: free space below 3 times the size

struct StorageCheckOptions
{
    StorageCheckOptions() : prune(true), force(false) {}

    UtcTime now;                // invalid = the clock

    // Free bytes at the store's directory. Empty = ask the file system (QStorageInfo); a
    // negative answer means unknown: nothing is flagged low, and the entry says so.
    std::function<qint64(const QString &directory)> freeSpace;

    bool prune;                 // prune the journal first (REQ-SCH-14)
    // Run although the last check is less than a week old. Only for an action of the user
    // ("check now"); the service never sets it.
    bool force;
};

struct StorageCheckResult
{
    StorageCheckResult()
        : ok(false), skipped(false), recorded(false), freeKnown(false), low(false),
          storeBytes(0), journalBytes(0), freeBytes(-1), thresholdBytes(0), pruned(0) {}

    bool ok;                    // false: see error; nothing was recorded
    QString error;
    bool skipped;               // the last check is less than a week old: nothing was done
    bool recorded;              // the entry is in the journal

    UtcTime time;
    bool freeKnown;
    bool low;                   // free space below 3 times (store + journal): point to compaction
    qint64 storeBytes;          // the database and its log
    qint64 journalBytes;
    qint64 freeBytes;           // -1 when unknown
    qint64 thresholdBytes;      // 3 times the size of store and journal
    qint64 pruned;              // heartbeats the journal pruning removed
    QString pruneError;         // pruning failed (the check still ran)

    qint64 totalBytes() const { return storeBytes + journalBytes; }
};

// True if there is no check yet, or the last one is a week old or more (also when the clock
// was set back by more than a week).
bool storageCheckDue(Journal &journal, const UtcTime &now);

// Prunes the journal, measures, compares and records. "journal" and "store" must be open
// for writing.
StorageCheckResult runStorageCheck(Store &store, Journal &journal, const StorageCheckOptions &options);

// The result of the latest recorded check, read from the journal. False if there is none.
bool lastStorageCheck(Journal &journal, StorageCheckResult *result);

} // namespace lm

#endif
