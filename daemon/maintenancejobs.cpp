// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Michele Brami
#include "maintenancejobs.h"

#include "store/journal.h"
#include "store/store.h"

#include <QJsonDocument>
#include <QJsonObject>

namespace lm {

namespace {

QString spaceWord(CompactionEstimate::Space s)
{
    switch (s) {
    case CompactionEstimate::SpaceEnough: return QStringLiteral("enough");
    case CompactionEstimate::SpaceShort: return QStringLiteral("short");
    case CompactionEstimate::SpaceUnknown: return QStringLiteral("unknown");
    }
    return QStringLiteral("unknown");
}

// The store tables and journal kinds a purge counts, always all of them (0 when nothing goes), so that
// the window shows one fixed list and an estimate and a result have the same keys.
const char *const kPurgeTables[] = { "record", "mark", "sensor", "quarantine", "ingest" };
const char *const kPurgeJournalKinds[] = { "alert", "threshold", "silent" };

QJsonObject countsOf(const QMap<QString, qint64> &m, const char *const *keys, int n)
{
    QJsonObject o;
    for (int i = 0; i < n; ++i)
        o.insert(QLatin1String(keys[i]), double(m.value(QLatin1String(keys[i]))));
    return o;
}

// The age the window asked for: a JSON whole number of months, 3, 6, 12 or 24 (REQ-STO-17). A string,
// a fraction or a missing key is refused like any other age, before anything starts.
bool readAge(const QJsonObject &p, int *months)
{
    const QJsonValue v = p.value(QStringLiteral("age_months"));
    if (!v.isDouble())
        return false;
    const double d = v.toDouble();
    if (d != double(int(d)) || !isAllowedPurgeAge(int(d)))
        return false;
    *months = int(d);
    return true;
}

MaintenanceRunner::Job buildCompactionJob(const QJsonObject &p, const MaintenanceContext &c, QString *error)
{
    FullResolutionWindow window = FullResolutionWindow::ofMonths(p.value(QStringLiteral("window_months")).toInt(3));
    if (p.contains(QStringLiteral("window_days")))
        window = FullResolutionWindow::ofDays(p.value(QStringLiteral("window_days")).toInt());
    if (!window.isAllowed(UtcTime::now())) {
        *error = QStringLiteral("parameter-window");        // REQ-STO-13: at least 14 days
        return MaintenanceRunner::Job();
    }
    const QString storePath = c.storePath;
    const FreeSpaceFn freeSpace = c.freeSpace;
    return [=](const ProgressFn &progress) -> MaintenanceResult {
        Store store;
        if (!store.open(storePath))
            return MaintenanceResult::failure(CompactionError::io(), QStringLiteral("open"));
        CompactionOptions o;
        o.window = window;
        o.progress = progress;
        o.freeSpace = freeSpace;
        // Decision: (REQ-STO-16): no deletion without the space check.
        o.proceedWhenFreeSpaceUnknown = false;
        const CompactionReport r = compactStore(store, o);
        MaintenanceResult result = r.ok ? MaintenanceResult::success()
                                        : MaintenanceResult::failure(r.failure.errorClass, r.failure.message);
        result.details = compactionEstimateDetails(r.estimate);
        if (r.ok) {
            result.details.insert(QStringLiteral("removed"), double(r.totalRemoved()));
            result.details.insert(QStringLiteral("bytes_freed"), double(r.bytesFreed));
        }
        return result;
    };
}

// The purge and its estimate. Everything the core needs besides the Subject and the age comes from the
// service's configuration (purgeTarget), never from the caller, so a window cannot widen the purge to
// another Subject's quarantine or connection log.
MaintenanceRunner::Job buildPurgeJob(bool dryRun, const QJsonObject &p, const MaintenanceContext &c,
                                     QString *error)
{
    const QString subjectText = p.value(QStringLiteral("subject")).toString();
    const QUuid subject(subjectText);
    if (subjectText.isEmpty() || subject.isNull()) {
        *error = QStringLiteral("parameter-subject");
        return MaintenanceRunner::Job();
    }
    int months = 0;
    if (!readAge(p, &months)) {
        *error = PurgeError::ageNotAllowed();
        return MaintenanceRunner::Job();
    }
    const PurgeTarget target = c.purgeTarget ? c.purgeTarget(subject) : PurgeTarget();
    if (!target.known) {
        *error = QStringLiteral("unknown-subject");
        return MaintenanceRunner::Job();
    }
    if (!dryRun) {
        // the typed label must equal the configured one exactly. A Subject without a label
        // can never match (an empty typed text would otherwise pass), so it is refused on its own key.
        if (target.label.isEmpty()) {
            *error = QStringLiteral("no-label");
            return MaintenanceRunner::Job();
        }
        const QJsonValue typed = p.value(QStringLiteral("typed_label"));
        if (!typed.isString() || typed.toString() != target.label) {
            *error = QStringLiteral("label-mismatch");
            return MaintenanceRunner::Job();
        }
    }

    PurgeOptions base;
    base.subject = subject;
    base.months = months;
    base.subjectRefs = target.subjectRefs;
    base.connections = target.soleConnections;
    base.freeSpace = c.freeSpace;
    // As for compaction: no deletion without the space check, no override.
    base.proceedWhenFreeSpaceUnknown = false;
    const QString storePath = c.storePath;
    const QString journalPath = c.journalPath;

    if (dryRun) {
        // Read-only connections: the estimate can never change a file, also by mistake.
        return [=](const ProgressFn &) -> MaintenanceResult {
            Store store;
            Journal journal;
            if (!store.openReadOnly(storePath) || !journal.openReadOnly(journalPath))
                return MaintenanceResult::failure(CompactionError::io(), QStringLiteral("open"));
            PurgeOptions o = base;
            o.now = UtcTime::now();
            PurgeEstimate e;
            QString err;
            if (!estimatePurge(store, journal, o, &e, &err))
                return MaintenanceResult::failure(CompactionError::io(), QStringLiteral("estimate"));
            MaintenanceResult result = MaintenanceResult::success();
            result.details = purgeEstimateDetails(e);
            return result;
        };
    }

    const std::function<bool(Journal &, const QString &)> openJournal = c.openJournal;
    return [=](const ProgressFn &progress) -> MaintenanceResult {
        Store store;
        Journal journal;
        // The journal is opened for writing: the Subject's alert, threshold and silent entries
        // before the cut-off go after the store part.
        const bool journalOpen = openJournal ? openJournal(journal, journalPath) : journal.open(journalPath);
        if (!store.open(storePath) || !journalOpen)
            return MaintenanceResult::failure(CompactionError::io(), QStringLiteral("open"));
        PurgeOptions o = base;
        o.now = UtcTime::now();
        o.progress = progress;
        const PurgeReport r = purgeSubject(store, journal, o);
        MaintenanceResult result = r.ok ? MaintenanceResult::success()
                                        : MaintenanceResult::failure(r.failure.errorClass, r.failure.message);
        result.details = purgeEstimateDetails(r.estimate);
        result.details.insert(QStringLiteral("store_part_done"), r.storePartDone);
        result.details.insert(QStringLiteral("removed"),
                              countsOf(r.removed, kPurgeTables, int(sizeof(kPurgeTables) / sizeof(kPurgeTables[0]))));
        result.details.insert(QStringLiteral("journal_removed"),
                              countsOf(r.journalRemoved, kPurgeJournalKinds,
                                       int(sizeof(kPurgeJournalKinds) / sizeof(kPurgeJournalKinds[0]))));
        result.details.insert(QStringLiteral("removed_total"), double(r.totalRemoved()));
        result.details.insert(QStringLiteral("bytes_freed"), double(r.bytesFreed));
        result.details.insert(QStringLiteral("journal_bytes_freed"), double(r.journalBytesFreed));
        return result;
    };
}

} // namespace

// Numbers travel as JSON doubles: exact up to 2^53 bytes, far beyond any phone's storage.
QJsonObject compactionEstimateDetails(const CompactionEstimate &e)
{
    QJsonObject o;
    o.insert(QStringLiteral("cutoff"), e.cutoff.isValid() ? e.cutoff.toIso() : QString());
    o.insert(QStringLiteral("records"), double(e.records));
    o.insert(QStringLiteral("quarantine"), double(e.quarantine));
    o.insert(QStringLiteral("ingest"), double(e.ingest));
    o.insert(QStringLiteral("bytes_estimated"), double(e.bytes));
    o.insert(QStringLiteral("store_bytes"), double(e.storeBytes));
    o.insert(QStringLiteral("rebuild_need_bytes"), double(e.rebuildNeedBytes));
    o.insert(QStringLiteral("free_bytes"), double(e.freeBytes));
    o.insert(QStringLiteral("space"), spaceWord(e.space));
    o.insert(QStringLiteral("missing_bytes"), double(e.missingBytes()));
    return o;
}

// The purge's estimate (REQ-STO-17): the rows per store table and the entries per journal
// kind that go, the bytes the two files are expected to give back (estimates), and the space
// check: the temporary space each rewrite needs, the larger one when both files share a directory
// (`needed_bytes`), the free space, the verdict and what is missing.
QJsonObject purgeEstimateDetails(const PurgeEstimate &e)
{
    QJsonObject o;
    o.insert(QStringLiteral("subject"), e.subject.toString());          // the opaque id the window sent
    o.insert(QStringLiteral("age_months"), e.months);
    o.insert(QStringLiteral("cutoff"), e.cutoff.isValid() ? e.cutoff.toIso() : QString());
    QMap<QString, qint64> rows;
    rows.insert(QStringLiteral("record"), e.records);
    rows.insert(QStringLiteral("mark"), e.marks);
    rows.insert(QStringLiteral("sensor"), e.sensors);
    rows.insert(QStringLiteral("quarantine"), e.quarantine);
    rows.insert(QStringLiteral("ingest"), e.ingest);
    o.insert(QStringLiteral("store_rows"), countsOf(rows, kPurgeTables, int(sizeof(kPurgeTables) / sizeof(kPurgeTables[0]))));
    o.insert(QStringLiteral("journal_entries"),
             countsOf(e.journal, kPurgeJournalKinds, int(sizeof(kPurgeJournalKinds) / sizeof(kPurgeJournalKinds[0]))));
    o.insert(QStringLiteral("total_rows"), double(e.totalRows()));
    o.insert(QStringLiteral("bytes_estimated"), double(e.bytes));
    o.insert(QStringLiteral("journal_bytes_estimated"), double(e.journalBytesFreed));
    o.insert(QStringLiteral("total_bytes_estimated"), double(e.totalBytesFreed()));
    o.insert(QStringLiteral("store_bytes"), double(e.storeBytes));
    o.insert(QStringLiteral("journal_file_bytes"), double(e.journalFileBytes));
    o.insert(QStringLiteral("rebuild_need_bytes"), double(e.rebuildNeedBytes));
    o.insert(QStringLiteral("journal_rebuild_need_bytes"), double(e.journalRebuildNeedBytes));
    o.insert(QStringLiteral("needed_bytes"), double(e.neededBytes()));
    o.insert(QStringLiteral("free_bytes"), double(e.freeBytes));
    o.insert(QStringLiteral("journal_free_bytes"), double(e.journalFreeBytes));
    o.insert(QStringLiteral("shared_directory"), e.sharedDirectory);
    o.insert(QStringLiteral("space"), spaceWord(e.space));
    o.insert(QStringLiteral("missing_bytes"), double(e.missingBytes()));
    return o;
}

MaintenanceRunner::Job buildMaintenanceJob(const QString &kind, const QString &parametersJson,
                                           const MaintenanceContext &context, QString *error)
{
    const QJsonDocument doc = QJsonDocument::fromJson(parametersJson.toUtf8());
    if (!doc.isObject()) {
        *error = QStringLiteral("parameters-not-json");
        return MaintenanceRunner::Job();
    }
    const QJsonObject p = doc.object();
    if (kind == MaintenanceKind::compaction())
        return buildCompactionJob(p, context, error);
    if (kind == MaintenanceKind::purgeEstimate())
        return buildPurgeJob(true, p, context, error);
    if (kind == MaintenanceKind::purge())
        return buildPurgeJob(false, p, context, error);
    // `export` and `reimport` were removed; an older window that still asks for them
    // gets the same answer as for any other unknown kind.
    *error = QStringLiteral("unknown-kind");
    return MaintenanceRunner::Job();
}

} // namespace lm
