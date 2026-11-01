// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Michele Brami
#ifndef LM_COMPACTOR_H
#define LM_COMPACTOR_H

#include "store/progress.h"
#include "store/store.h"

#include <QMap>
#include <QString>

#include <functional>

namespace lm {

// Compaction (REQ-STO-05 to 07, REQ-STO-12, REQ-STO-13, REQ-STO-16): bounds the
// growth of the store. It runs only after the user has seen the estimate and confirmed that
// the removed samples cannot be restored by the application (REQ-STO-05; the confirmation is
// the caller's). Compaction deletes permanently: there is no archive file any more
// so a row is chosen for removal by the rule alone.
//
// What it removes, when the row is older than the full-resolution window:
//   - adapter samples of the `current` feed, thinned by the rule of REQ-STO-07 (below),
//   - derived records (they can be recomputed),
//   - quarantine and ingest-log entries.
// It never removes samples of the `graph` (history) feed, marks, sensor records, events
// (logbook entries), manual records, records of a kind the registry does not thin
// (REQ-STO-12), the journal, or any compaction record. The database
// itself refuses every other delete (store.h).
//
// Thinning rule (REQ-STO-07, which replaced the minimum/maximum/middle rule).
// Time is divided into 5-minute slots aligned to UTC (hh:00, hh:05, ...; the registry gives
// the length and the thinned feeds per kind). Only the `current` feed (about one sample a
// minute) is thinned: for the adapter samples of one Subject, kind, source tag and unit in a
// slot, only the sample nearest to the start of the slot is kept (ties: the earlier one, by
// key order); a slot with a single sample keeps it. It was accepted that a short dip or
// peak inside a 5-minute slot of old data can disappear; the `graph` feed (one point
// every 5 minutes) keeps the history in full. Samples with another unit are never mixed
// the window is applied to whole slots, so a slot that crosses the end of the window
// is left alone.
//
// Space first (REQ-STO-16). After the deletion the database file is rebuilt (VACUUM) so that
// the space really goes back to the phone; the rebuild writes a new copy of what remains, so
// it needs free space of about that size. Before anything is deleted, compaction works out
// what it would remove (the dry run, the same calculation as estimateCompaction) and refuses
// without changing the store when the free space is short, or unknown and the caller did not
// accept that. The estimate the user saw and the check the run makes come from one function,
// so the estimate can never promise what the run then refuses.
//
// Decisions come from a read snapshot of a connection of its own, so rows ingested by the
// service while compaction runs are never considered and never removed; they wait for the next
// run. One transaction holds the deletions and the compaction record (written only if
// something was removed); afterwards the file is rebuilt and the write-ahead log emptied. The
// transaction and the rebuild block writers to the store for their duration (REQ-STO-15: the
// service holds its readings in a queue meanwhile).

// The version of the thinning rule, written into every compaction record: "1" was the rule of
// (30-minute slots, minimum, maximum and middle), "2" is the rule.
const char *const kThinningRuleVersion = "2";

const int kMinimumFullResolutionDays = 14;      // REQ-STO-13: newer data is never thinned

// The full-resolution window: default 3 calendar months; the user may change it but not
// below 14 days (REQ-STO-13).
struct FullResolutionWindow
{
    FullResolutionWindow() : months(3), days(0) {}

    static FullResolutionWindow ofMonths(int m) { FullResolutionWindow w; w.months = m; return w; }
    static FullResolutionWindow ofDays(int d) { FullResolutionWindow w; w.months = 0; w.days = d; return w; }

    int months;
    int days;           // if above 0 it is used instead of the months

    // The start of the window: records measured before this are old.
    UtcTime cutoff(const UtcTime &now) const;
    // False if the window is shorter than 14 days (or not set).
    bool isAllowed(const UtcTime &now) const;
};

// Error classes of a refused or failed compaction: stable keys that the service passes on and
// the window translates. In every case the store is as it was before.
namespace CompactionError {
inline QString io() { return QStringLiteral("io"); }                                // the store could not be read or written
inline QString windowTooShort() { return QStringLiteral("window-too-short"); }      // below 14 days (REQ-STO-13)
inline QString insufficientSpace() { return QStringLiteral("insufficient-space"); } // REQ-STO-16: the rebuild would not fit
inline QString freeSpaceUnknown() { return QStringLiteral("free-space-unknown"); }  // REQ-STO-16: no answer, caller did not accept
inline QString aborted() { return QStringLiteral("aborted"); }                      // the test hook stopped the run
inline QString cancelled() { return QStringLiteral("cancelled"); }                  // the progress callback asked to stop
}

// A failure: its class and a message that never repeats record content.
struct CompactionFailure
{
    bool ok() const { return errorClass.isEmpty(); }

    QString errorClass;
    QString message;
};

// Free bytes at a directory; a negative answer means unknown. An empty function asks the file
// system (QStorageInfo). Tests inject their own.
typedef std::function<qint64(const QString &directory)> FreeSpaceFn;

// Margin on top of what remains for the rebuild (REQ-STO-16): a tenth of what
// remains, at least 1 MiB. The rebuild writes every remaining page again through the
// write-ahead log, and pages are not filled completely; the margin covers that overhead.
const int kRebuildMarginPercent = 10;
const qint64 kRebuildMarginMinimumBytes = 1024 * 1024;

// REQ-STO-13 and REQ-STO-16: what compaction would free for a window and whether the rebuild
// after it would fit, from a dry run over the store. Changes nothing. The freed bytes are an
// estimate: the number of rows times the average size of a row; the UI labels them so.
struct CompactionEstimate
{
    // The free space compared with the rebuild need: enough (free >= need, the boundary is
    // allowed), short, or unknown (the file system gave no answer).
    enum Space { SpaceEnough, SpaceShort, SpaceUnknown };

    CompactionEstimate()
        : records(0), quarantine(0), ingest(0), bytes(0), storeBytes(0), rebuildNeedBytes(0),
          freeBytes(-1), space(SpaceUnknown) {}

    UtcTime cutoff;
    qint64 records;             // record rows that would be removed (samples and derived)
    qint64 quarantine;
    qint64 ingest;
    qint64 bytes;               // estimate of the bytes freed
    qint64 storeBytes;          // the database file and its write-ahead log now
    qint64 rebuildNeedBytes;    // free space the rebuild needs: what remains plus the margin
    qint64 freeBytes;           // free space at the store's directory now; -1 unknown
    Space space;

    qint64 totalRows() const { return records + quarantine + ingest; }
    // The free space that is missing when it is short (0 otherwise): what the user would have
    // to free, or to win with a shorter window.
    qint64 missingBytes() const { return space == SpaceShort ? rebuildNeedBytes - freeBytes : 0; }
};

// The rebuild need for a store of "storeBytes" of which about "freedBytes" go: what remains
// plus the margin above. Public so the UI and the tests state the same rule.
qint64 rebuildNeedFor(qint64 storeBytes, qint64 freedBytes);

// The parts of the space rule (REQ-STO-16) that compaction and the purge (purge.h) share, so
// both state the same estimate and the same verdict:
//   the free bytes at the directory of the store's file (-1 unknown: no file, or no answer),
qint64 freeSpaceForStore(const Store &store, const FreeSpaceFn &freeSpace);
//   the bytes that "rows" rows of the store take: rows times the average size of a row of the
//   tables that hold almost all the bytes (record, quarantine, ingest log, mark, sensor),
qint64 estimateBytesOfRows(Store &reader, qint64 storeBytes, qint64 rows);
//   the verdict: enough when free >= need, short below, unknown for a negative free space.
CompactionEstimate::Space spaceVerdict(qint64 freeBytes, qint64 needBytes);

// The dry run of REQ-STO-13 over "store" (open for reading or writing), for "window" at "now"
// (invalid = the clock). "freeSpace" answers for the directory of the store's file. False with
// *error when the store cannot be read or the window is below 14 days.
bool estimateCompaction(Store &store, const FullResolutionWindow &window, const UtcTime &now,
                        CompactionEstimate *estimate, QString *error,
                        const FreeSpaceFn &freeSpace = FreeSpaceFn());

struct CompactionOptions
{
    CompactionOptions() : proceedWhenFreeSpaceUnknown(false) {}

    UtcTime now;                    // time of the compaction record and of the window; invalid = the clock
    FullResolutionWindow window;
    FreeSpaceFn freeSpace;          // see FreeSpaceFn
    // REQ-STO-16: when the free space is unknown the run refuses with "free-space-unknown",
    // unless the caller decided to go on without the check (for example after asking the user).
    bool proceedWhenFreeSpaceUnknown;
    ProgressFn progress;            // phases and cancel, see progress.h
    // Test hook: called for each old record scanned by the deleting pass, with the number
    // scanned so far. Returning false aborts with class "aborted" and rolls everything back.
    std::function<bool(qint64 scanned)> hook;
};

struct CompactionReport
{
    CompactionReport() : ok(false), derivedRemoved(0), slotCount(0), kept(0), bytesFreed(0) {}

    bool ok;
    CompactionFailure failure;      // when not ok the store is unchanged

    // The dry run the space check used (also filled when the space check refused the run).
    CompactionEstimate estimate;

    UtcTime cutoff;                 // the window started here
    QMap<QString, qint64> removed;  // record, quarantine, ingest (only those removed)
    qint64 derivedRemoved;          // part of "record": derived records
    qint64 slotCount;               // slots of adapter samples that were thinned or checked
    qint64 kept;                    // samples kept in those slots
    qint64 bytesFreed;              // file size before minus after (database and log)

    qint64 totalRemoved() const;
};

// "store" must be open for writing (Store::open) with a file behind it. Compaction opens a
// read-only connection of its own for the decisions. Phases: checking-space (the dry run),
// compacting, finalizing (commit and rebuild), done.
CompactionReport compactStore(Store &store, const CompactionOptions &options);

} // namespace lm

#endif
