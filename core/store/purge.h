// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Michele Brami
#ifndef LM_PURGE_H
#define LM_PURGE_H

#include "store/compactor.h"
#include "store/journal.h"
#include "store/progress.h"
#include "store/store.h"

#include <QMap>
#include <QString>
#include <QStringList>
#include <QUuid>

#include <functional>

namespace lm {

// Purge of one Subject's old data (REQ-STO-17). The user chooses, by hand and for
// one Subject at a time, an age of 3, 6, 12 or 24 calendar months; everything of that Subject
// in the store older than that age is deleted for good, in one transaction. It is irreversible
// and, by design, leaves no record anywhere. The confirmation with the
// typed label belongs to the window and the service; this function only does the work.
//
// What goes (all with a time strictly before the cut-off; rows at or after it are never touched):
//   - table `record`: every record of the Subject measured before the cut-off: readings of every
//     feed (current and graph), logbook entries and other events, manual and derived records;
//   - table `mark`: the Subject's marks whose own time AND whose target's measurement time are
//     before the cut-off. A mark made after the cut-off stays even when its record goes (it then
//     points at nothing, which the effective view ignores); an old mark on a newer record stays,
//     because removing it would change how that newer record is shown;
//   - table `sensor`: the Subject's sensor records last seen before the cut-off (a sensor still
//     seen after the cut-off stays whole; the check is made again at the delete, because the
//     service widens a sensor row in place);
//   - table `quarantine`: entries received before the cut-off whose raw record names one of the
//     caller's `subjectRefs` (the adapter's `subject_ref`; the store holds no mapping from a
//     source reference to a Subject, REQ-SUB-01, so only the caller, who reads the configuration,
//     can say which references are the Subject's). No references: no quarantine entry goes;
//   - table `ingest_log`: entries before the cut-off whose `connection` is one of the caller's
//     `connections`. The ingest log is kept per connection and session, not per Subject; the
//     caller passes only connections that serve this Subject alone.
//     No connections: no ingest-log entry goes.
//   - the service journal (a file of its own): the Subject's `alert`, `threshold` and
//     `silent` entries with a wall time before the cut-off (Journal::purgeSubjectEntries, the
//     only exception to the journal's append-only rule).
// What stays, deliberately: the Subject's own row (table `subject`) and its configuration
// (outside the store); every other journal entry (heartbeat, start, stop, prune,
// connection-state, storage-check, version-check: service events, not the Subject's data);
// the compaction records and the reimport records of schema 1 (they describe the store, not
// one Subject); every row and entry of any other Subject.
//
// Two files, two transactions: the store's rows go first, then the journal's
// entries. If the second part fails the report says so (`storePartDone`) and the purge can be
// run again: it is idempotent, the store part then finds nothing more and the journal part
// does its work.
//
// Pattern: the same as compaction (compactor.h). Decisions come from a read snapshot taken at
// the start, so a row written by the service while the purge runs is never looked at and
// never removed. The dry run (estimatePurge) and the run make the same calculation over that
// snapshot: what would go, the bytes it frees (an estimate), the temporary space needed to
// rewrite the files afterwards (REQ-STO-16, wording) and the verdict on the free space.
// The two files are rewritten one after the other, so the space needed is the larger of the
// two needs when they share a directory; otherwise each is checked where its file is.
// The run refuses before anything is deleted when the space is short, or unknown and the
// caller did not accept that. After the commit the file is rewritten (VACUUM) so the space
// really goes back. The transaction blocks writers to the store while it runs (REQ-STO-15:
// the service holds its readings in a queue meanwhile).

// The ages a user may choose (REQ-STO-17), in calendar months.
bool isAllowedPurgeAge(int months);
QList<int> allowedPurgeAges();          // 3, 6, 12, 24

// The cut-off for "older than `months` calendar months before `now`" (UTC, the day clamped to
// the length of the month); invalid for an age that is not allowed or an invalid time. It is
// never later than 14 days before `now` (REQ-STO-13's floor holds for every allowed age).
UtcTime purgeCutoff(int months, const UtcTime &now);

// Error class of a purge refused for its arguments; the others are CompactionError's
// (io, insufficient-space, free-space-unknown, aborted, cancelled).
namespace PurgeError {
inline QString ageNotAllowed() { return QStringLiteral("age-not-allowed"); }    // not 3, 6, 12 or 24 months
inline QString noSubject() { return QStringLiteral("no-subject"); }             // a null Subject id
}

// What the purge would remove, and the space it needs. Changes nothing.
struct PurgeEstimate
{
    PurgeEstimate()
        : months(0), records(0), marks(0), sensors(0), quarantine(0), ingest(0), bytes(0),
          storeBytes(0), rebuildNeedBytes(0), freeBytes(-1), journalBytesFreed(0), journalFileBytes(0),
          journalRebuildNeedBytes(0), journalFreeBytes(-1), sharedDirectory(true), space(CompactionEstimate::SpaceUnknown) {}

    QUuid subject;
    int months;
    UtcTime cutoff;
    qint64 records;             // record rows: readings of every feed, events, manual, derived
    qint64 marks;
    qint64 sensors;
    qint64 quarantine;
    qint64 ingest;
    qint64 bytes;               // estimate of the bytes freed in the store (rows times the average row size)
    qint64 storeBytes;          // the database file and its write-ahead log now
    qint64 rebuildNeedBytes;    // temporary space needed to rewrite the store's file (rebuildNeedFor)
    qint64 freeBytes;           // free space at the store's directory now; -1 unknown

    QMap<QString, qint64> journal;  // journal entries that would go, per kind (alert, threshold, silent)
    qint64 journalBytesFreed;   // estimate, entries times the journal's average entry size
    qint64 journalFileBytes;    // the journal file and its log now
    qint64 journalRebuildNeedBytes;   // temporary space needed to rewrite the journal's file
    qint64 journalFreeBytes;    // free space at the journal's directory; -1 unknown

    bool sharedDirectory;       // both files in one directory: the needs do not add up, the larger counts
    CompactionEstimate::Space space;  // the verdict for both rewrites

    qint64 journalRows() const;
    qint64 totalRows() const { return records + marks + sensors + quarantine + ingest + journalRows(); }
    qint64 totalBytesFreed() const { return bytes + journalBytesFreed; }
    // The temporary space the purge needs: the larger need when both files share a directory.
    qint64 neededBytes() const;
    qint64 missingBytes() const;
};

struct PurgeOptions
{
    PurgeOptions() : months(0), proceedWhenFreeSpaceUnknown(false) {}

    QUuid subject;
    int months;                     // 3, 6, 12 or 24
    UtcTime now;                    // invalid = the clock
    QStringList subjectRefs;        // the Subject's source references, for quarantine (see above)
    QStringList connections;        // connections that serve only this Subject, for the ingest log
    FreeSpaceFn freeSpace;          // see compactor.h
    bool proceedWhenFreeSpaceUnknown;
    ProgressFn progress;            // phases checking-space, purging, finalizing, done
    // Test hook: called before each row the deleting pass removes, with the number done so far.
    // Returning false aborts with class "aborted" and rolls everything back.
    std::function<bool(qint64 done)> hook;
};

struct PurgeReport
{
    PurgeReport() : ok(false), storePartDone(false), bytesFreed(0), journalBytesFreed(0) {}

    bool ok;
    // When not ok: if storePartDone is false nothing changed anywhere; if it is true the store
    // part was committed and the journal part failed (it changed nothing): run the purge again.
    CompactionFailure failure;
    bool storePartDone;
    PurgeEstimate estimate;         // the dry run the space check used (also on a refusal)
    QMap<QString, qint64> removed;  // store: record, mark, sensor, quarantine, ingest (only those removed)
    QMap<QString, qint64> journalRemoved;   // journal: alert, threshold, silent (only those removed)
    qint64 bytesFreed;              // store file size before minus after (database and log)
    qint64 journalBytesFreed;       // the same for the journal file

    qint64 totalRemoved() const;
};

// The dry run over "store" and "journal" (open for reading or writing). False with *error for a
// bad age or Subject or a file that cannot be read. Only `subject`, `months`, `now`,
// `subjectRefs`, `connections` and `freeSpace` of the options are used.
bool estimatePurge(Store &store, Journal &journal, const PurgeOptions &options, PurgeEstimate *estimate, QString *error);

// "store" and "journal" must be open for writing on files. Opens a read-only connection of its
// own for the store's decisions.
PurgeReport purgeSubject(Store &store, Journal &journal, const PurgeOptions &options);

} // namespace lm

#endif
