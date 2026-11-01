// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Michele Brami
#ifndef LM_MAINTENANCEJOBS_H
#define LM_MAINTENANCEJOBS_H

// Builds the job of a StartMaintenance call: the
// function the maintenance worker thread runs. The application neither exports nor
// imports data. The kinds are:
//   compaction      thins old samples of every Subject (REQ-STO-05 to 07, 13, 16), deletes permanently
//   purge-estimate  the dry run of a purge: changes nothing, reports what a purge would remove
//   purge           removes everything of ONE Subject older than an age (REQ-STO-17),
//                   in the store and the Subject's alert, threshold and silent journal entries
// Pattern: factory; the runner (core/service) knows nothing about the store. Only one job runs at a
// time (the runner refuses a second start with `maintenance-running`).
//
// Each job opens its OWN connections inside the worker thread (a Qt SQL connection belongs to the
// thread that opened it) and so holds the write lock while it deletes and rebuilds. The service's
// main connection then gets SQLITE_BUSY, the store reports "busy" and the scheduler holds readings in
// its bounded queue, so monitoring is never interrupted (REQ-STO-15); at the queue's high-water mark
// the run is cancelled (MaintenanceHighWaterGuard) and rolls back. Alerts do not depend on the store.
//
// Parameters travel as a JSON object (`s`) so the D-Bus signature never changes; other keys are
// ignored. The service writes nothing outside its own data directory.
//   compaction      { "window_months": 3 }   or   { "window_days": 30 }   (at least 14 days)
//   purge-estimate  { "subject": "{uuid}", "age_months": 3 }             (3, 6, 12 or 24)
//   purge           { "subject": "{uuid}", "age_months": 3, "typed_label": "<what the user typed>" }
//
// The purge gate: the service refuses a purge whose `typed_label` is not EXACTLY the
// Subject's label in the service's own configuration (no trimming, no case folding). It guards against a
// stray or repeated request (an old call replayed, a window that sends the wrong Subject); it is NOT
// authentication: anything that can call the bus can also read the configuration. A Subject without a
// label cannot be purged (`no-label`): the user gives it one first. The estimate needs no label. The label
// and the typed text are never logged, written to the journal or sent back.
//
// The purge's other inputs come from the configuration, never from the caller: the
// Subject's source reference (to find its quarantine entries, which hold no Subject id) and the
// connections that serve that Subject ALONE (the ingest log is kept per connection; a connection that
// also serves another Subject is left out, so its log entries stay).
//
// Start errors (the answer of StartMaintenance; nothing has started):
//   every kind:   parameters-not-json, unknown-kind, (and from the runtime: service-not-running,
//                 maintenance-running)
//   compaction:   parameter-window
//   purge, purge-estimate: parameter-subject (not a Subject id), unknown-subject (not in the
//                 configuration), age-not-allowed (not 3, 6, 12 or 24)
//   purge:        no-label, label-mismatch
//
// Space (REQ-STO-16): the core checks before it deletes anything whether the rewrite of the file(s) fits
// in the free space. A short space is refused (`insufficient-space`, with the missing bytes), and an
// UNKNOWN free space is refused too (`free-space-unknown`): the service never deletes without the check,
// there is no override.
//
// Result details (MaintenanceFinished `detailsJson`; numbers and keys only, never a value, a name or a
// label), also on a refusal, so the window can explain it with numbers:
//   compaction: cutoff (UTC ISO), records, quarantine, ingest, bytes_estimated, store_bytes,
//     rebuild_need_bytes, free_bytes (-1 unknown), space ("enough", "short", "unknown"), missing_bytes;
//     after a run: removed, bytes_freed.
//   purge and purge-estimate: purgeEstimateDetails below; a purge adds removed {per table},
//     journal_removed {per kind}, removed_total, bytes_freed, journal_bytes_freed, store_part_done.
// Error classes (the job's result): compaction: io, window-too-short, insufficient-space,
// free-space-unknown, aborted, cancelled. Purge: the same (window-too-short apart) plus age-not-allowed
// and no-subject from the core. A purge whose journal part failed answers `io` with store_part_done true:
// the store part is done, the journal is unchanged and the same purge can simply be started again
// (it is idempotent). Phase keys: checking-space, compacting or purging, finalizing, done.
// No purge record is written anywhere.

#include "service/maintenancerunner.h"
#include "store/compactor.h"
#include "store/purge.h"

#include <QStringList>
#include <QUuid>

#include <functional>

namespace lm {

class Journal;

// What the configuration says about the Subject of a purge (ServiceCore::purgeTarget).
struct PurgeTarget
{
    PurgeTarget() : known(false) {}
    bool known;                 // the Subject is in the configuration
    QString label;              // its label as configured (trimmed by the reader); empty = none
    QStringList subjectRefs;    // its source reference(s), for quarantine entries
    QStringList soleConnections;    // connections that serve this Subject and no other
};

// What a job needs from the service. purgeTarget is called on the MAIN thread while the job is built;
// freeSpace and openJournal are called in the worker thread.
struct MaintenanceContext
{
    QString storePath;
    QString journalPath;
    FreeSpaceFn freeSpace;                                   // empty = the file system
    std::function<PurgeTarget(const QUuid &)> purgeTarget;   // empty = no Subject known
    // Opens the journal for the purge's writing; empty = Journal::open. Tests open it read-only to make
    // the journal part fail after the store part (the repeat).
    std::function<bool(Journal &, const QString &)> openJournal;
};

// Empty `error` and a valid job on success; otherwise an error key (above) and an empty job.
MaintenanceRunner::Job buildMaintenanceJob(const QString &kind, const QString &parametersJson,
                                           const MaintenanceContext &context, QString *error);

// The estimates as the result details write them (above). Public so a test states the same keys.
QJsonObject compactionEstimateDetails(const CompactionEstimate &e);
QJsonObject purgeEstimateDetails(const PurgeEstimate &e);

} // namespace lm

#endif
