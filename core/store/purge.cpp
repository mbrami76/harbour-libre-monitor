// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Michele Brami
//
// The purge of one Subject's old data and its dry run (purge.h). One scan decides what goes;
// the dry run and the real run differ only in what they do with a decision (count it, or
// count it and delete the row), so the estimate and the run cannot disagree.
#include "store/purge.h"

#include <QDateTime>
#include <QFileInfo>
#include <QSet>
#include <QStorageInfo>

namespace lm {

bool isAllowedPurgeAge(int months)
{
    return allowedPurgeAges().contains(months);
}

QList<int> allowedPurgeAges()
{
    return QList<int>() << 3 << 6 << 12 << 24;
}

UtcTime purgeCutoff(int months, const UtcTime &now)
{
    if (!isAllowedPurgeAge(months) || !now.isValid())
        return UtcTime();
    // Calendar months in UTC, as the full-resolution window of compaction counts them.
    const UtcTime c = FullResolutionWindow::ofMonths(months).cutoff(now);
    // REQ-STO-17: never anything inside the 14-day floor. True for every allowed age; checked
    // so that a change of the list can never break it.
    if (!c.isValid() || c.msecsSinceEpoch() > now.msecsSinceEpoch() - qint64(kMinimumFullResolutionDays) * 86400000LL)
        return UtcTime();
    return c;
}

namespace {

qint64 sum(const QMap<QString, qint64> &m)
{
    qint64 n = 0;
    for (QMap<QString, qint64>::const_iterator it = m.begin(); it != m.end(); ++it)
        n += it.value();
    return n;
}

} // namespace

qint64 PurgeEstimate::journalRows() const { return sum(journal); }

qint64 PurgeEstimate::neededBytes() const
{
    return qMax(rebuildNeedBytes, journalRows() > 0 ? journalRebuildNeedBytes : 0);
}

qint64 PurgeEstimate::missingBytes() const
{
    if (space != CompactionEstimate::SpaceShort)
        return 0;
    if (sharedDirectory)
        return neededBytes() - freeBytes;
    qint64 missing = 0;
    if (freeBytes >= 0) missing = qMax(missing, rebuildNeedBytes - freeBytes);
    if (journalRows() > 0 && journalFreeBytes >= 0) missing = qMax(missing, journalRebuildNeedBytes - journalFreeBytes);
    return missing;
}

qint64 PurgeReport::totalRemoved() const
{
    return sum(removed) + sum(journalRemoved);
}

namespace {

CompactionFailure failure(const QString &errorClass, const QString &message)
{
    CompactionFailure f;
    f.errorClass = errorClass;
    f.message = message;
    return f;
}

// What the scan does with a decision. The dry run counts only; the real run deletes.
struct Actions
{
    std::function<bool(const RecordKey &)> removeRecord;
    std::function<bool(const Mark &)> removeMark;
    std::function<bool(const Sensor &)> removeSensor;     // false: not removed (error or seen again)
    std::function<bool(const QUuid &)> removeQuarantine;
    std::function<bool(const QUuid &)> removeIngest;
};

struct Counters
{
    Counters() : records(0), marks(0), sensors(0), quarantine(0), ingest(0) {}
    qint64 records, marks, sensors, quarantine, ingest;
    qint64 total() const { return records + marks + sensors + quarantine + ingest; }
};

// The decisions, shared by the dry run and the run. Reads only through "reader".
CompactionFailure scan(Store &reader, const PurgeOptions &o, const UtcTime &cutoff, const Actions &actions,
                       ProgressReporter *progress, OperationProgress::Phase phase, bool useHook,
                       Counters *c)
{
    const qint64 cut = cutoff.msecsSinceEpoch();
    const QSet<QString> refs = QSet<QString>::fromList(o.subjectRefs);
    const QSet<QString> connections = QSet<QString>::fromList(o.connections);
    const qint64 total = reader.countRecordsOfSubjectBefore(o.subject, cutoff);
    qint64 done = 0;
    CompactionFailure stop;

    // One step of the scan: progress, the test hook, then the action; false stops the scan.
    auto step = [&](const std::function<bool()> &act, const char *what, qint64 *counter) {
        if (!stop.ok())
            return;
        if (useHook && o.hook && !o.hook(done)) {
            stop = failure(CompactionError::aborted(), QStringLiteral("stopped by the test hook"));
            return;
        }
        ++done;
        if (progress && ((done & 127) == 0) && !progress->report(phase, qMin(done, total), total)) {
            stop = failure(CompactionError::cancelled(), QStringLiteral("cancelled"));
            return;
        }
        if (act())
            ++*counter;
        else if (stop.ok() && qstrcmp(what, "sensor") != 0)
            stop = failure(CompactionError::io(), QStringLiteral("a %1 row could not be removed").arg(QLatin1String(what)));
    };

    if (progress && !progress->report(phase, 0, total, true))
        return failure(CompactionError::cancelled(), QStringLiteral("cancelled"));

    // Records of every feed, events, manual and derived records.
    bool ok = reader.forEachRecordOfSubjectBefore(o.subject, cutoff, [&](const Record &r) {
        const RecordKey k = r.key();
        step([&]() { return actions.removeRecord(k); }, "record", &c->records);
    });
    // Marks: the Subject's, made before the cut-off, on a record measured before it.
    ok = ok && reader.forEachMark([&](const Mark &m) {
        if (m.target.subject != o.subject || m.time.msecsSinceEpoch() >= cut || m.target.measuredAtMs >= cut)
            return;
        step([&]() { return actions.removeMark(m); }, "mark", &c->marks);
    });
    // Sensor records last seen before the cut-off.
    ok = ok && reader.forEachSensor([&](const Sensor &s) {
        if (s.subject != o.subject || s.lastSeen.msecsSinceEpoch() >= cut)
            return;
        step([&]() { return actions.removeSensor(s); }, "sensor", &c->sensors);
    });
    // Quarantine entries whose raw record names one of the Subject's references.
    if (!refs.isEmpty())
        ok = ok && reader.forEachQuarantine([&](const QuarantineEntry &e) {
            if (e.receivedAt.msecsSinceEpoch() >= cut)
                return;
            const QJsonValue ref = e.record.value(QStringLiteral("subject_ref"));
            if (!ref.isString() || !refs.contains(ref.toString()))
                return;
            step([&]() { return actions.removeQuarantine(e.id); }, "quarantine", &c->quarantine);
        });
    // Ingest-log entries of the connections that serve only this Subject.
    if (!connections.isEmpty())
        ok = ok && reader.forEachIngestEntry([&](const IngestEntry &e) {
            if (e.time.msecsSinceEpoch() >= cut)
                return;
            const QJsonValue conn = e.extra.value(QStringLiteral("connection"));
            if (!conn.isString() || !connections.contains(conn.toString()))
                return;
            step([&]() { return actions.removeIngest(e.id); }, "ingest-log", &c->ingest);
        });
    if (!stop.ok())
        return stop;
    if (!ok)
        return failure(CompactionError::io(), QStringLiteral("the store could not be read"));
    if (progress && !progress->report(phase, total, total, true))
        return failure(CompactionError::cancelled(), QStringLiteral("cancelled"));
    return CompactionFailure();
}

// Checks the arguments; an empty failure when they are good.
CompactionFailure checkArguments(const PurgeOptions &o, const UtcTime &now, UtcTime *cutoff)
{
    if (o.subject.isNull())
        return failure(PurgeError::noSubject(), QStringLiteral("no Subject was given"));
    *cutoff = purgeCutoff(o.months, now);
    if (!cutoff->isValid())
        return failure(PurgeError::ageNotAllowed(), QStringLiteral("the age must be 3, 6, 12 or 24 months"));
    return CompactionFailure();
}

// The one calculation of the estimate and of the run's space check (REQ-STO-16): the dry run
// over "reader", then the bytes freed, the temporary space needed and the verdict.
CompactionFailure dryRun(Store &reader, Journal &journal, const PurgeOptions &o, const UtcTime &cutoff,
                         qint64 storeBytes, qint64 freeBytes, ProgressReporter *progress, PurgeEstimate *e)
{
    Counters c;
    Actions count;
    count.removeRecord = [](const RecordKey &) { return true; };
    count.removeMark = [](const Mark &) { return true; };
    count.removeSensor = [](const Sensor &) { return true; };
    count.removeQuarantine = [](const QUuid &) { return true; };
    count.removeIngest = [](const QUuid &) { return true; };
    const CompactionFailure f = scan(reader, o, cutoff, count, progress, OperationProgress::CheckingSpace, false, &c);
    if (!f.ok())
        return f;
    e->subject = o.subject;
    e->months = o.months;
    e->cutoff = cutoff;
    e->records = c.records;
    e->marks = c.marks;
    e->sensors = c.sensors;
    e->quarantine = c.quarantine;
    e->ingest = c.ingest;
    e->storeBytes = storeBytes;
    e->bytes = estimateBytesOfRows(reader, storeBytes, c.total());
    e->rebuildNeedBytes = rebuildNeedFor(storeBytes, e->bytes);
    e->freeBytes = freeBytes >= 0 ? freeBytes : -1;

    // The journal's part: its entries of the three kinds, the bytes they take (entries
    // times the journal's average entry size) and the space to rewrite the journal's file.
    if (!journal.countSubjectEntriesBefore(o.subject, cutoff, &e->journal))
        return failure(CompactionError::io(), QStringLiteral("the journal could not be read"));
    e->journalFileBytes = journal.sizeBytes();
    const qint64 entries = journal.count();
    e->journalBytesFreed = entries > 0 ? qint64(double(e->journalRows()) * double(e->journalFileBytes) / double(entries)) : 0;
    e->journalRebuildNeedBytes = rebuildNeedFor(e->journalFileBytes, e->journalBytesFreed);
    const QString storeDir = QFileInfo(reader.path()).absolutePath();
    const QString journalDir = QFileInfo(journal.path()).absolutePath();
    e->sharedDirectory = journal.path().isEmpty() || storeDir == journalDir;
    if (e->sharedDirectory) {
        e->journalFreeBytes = e->freeBytes;
        e->space = spaceVerdict(freeBytes, e->neededBytes());
    } else {
        e->journalFreeBytes = o.freeSpace ? o.freeSpace(journalDir) : -1;
        if (!o.freeSpace) {
            const QStorageInfo info(journalDir);
            e->journalFreeBytes = info.isValid() ? info.bytesAvailable() : -1;
        }
        const CompactionEstimate::Space a = spaceVerdict(freeBytes, e->rebuildNeedBytes);
        const CompactionEstimate::Space b = e->journalRows() > 0
            ? spaceVerdict(e->journalFreeBytes, e->journalRebuildNeedBytes) : CompactionEstimate::SpaceEnough;
        e->space = (a == CompactionEstimate::SpaceShort || b == CompactionEstimate::SpaceShort) ? CompactionEstimate::SpaceShort
            : (a == CompactionEstimate::SpaceUnknown || b == CompactionEstimate::SpaceUnknown) ? CompactionEstimate::SpaceUnknown
            : CompactionEstimate::SpaceEnough;
    }
    return CompactionFailure();
}

// The journal's part, after the store's: one transaction of its own. A failure here
// leaves the journal unchanged and the store purged; the report says so and the purge can be run
// again (the store part then finds nothing).
PurgeReport finishJournal(Journal &journal, const UtcTime &cutoff, const PurgeOptions &o,
                          ProgressReporter *progress, PurgeReport *report)
{
    if (report->estimate.journalRows() > 0) {
        const qint64 before = journal.sizeBytes();
        if (!journal.purgeSubjectEntries(o.subject, cutoff, &report->journalRemoved)) {
            report->ok = false;
            report->failure = failure(CompactionError::io(),
                                      QStringLiteral("the store part is done; the journal part failed and can be repeated: ")
                                          + journal.lastError());
            return *report;
        }
        const qint64 after = journal.sizeBytes();
        report->journalBytesFreed = before > after ? before - after : 0;
    }
    report->ok = true;
    progress->report(OperationProgress::Finalizing, 1, 1, true);
    progress->report(OperationProgress::Done, 1, 1, true);
    return *report;
}

} // namespace

bool estimatePurge(Store &store, Journal &journal, const PurgeOptions &options, PurgeEstimate *estimate, QString *error)
{
    const UtcTime now = options.now.isValid() ? options.now : UtcTime::now();
    if (!store.isOpen() || !journal.isOpen()) {
        *error = QStringLiteral("the store or the journal is not open");
        return false;
    }
    UtcTime cutoff;
    CompactionFailure f = checkArguments(options, now, &cutoff);
    if (!f.ok()) {
        *error = f.message;
        return false;
    }
    PurgeEstimate e;
    f = dryRun(store, journal, options, cutoff, store.sizeBytes(), freeSpaceForStore(store, options.freeSpace), 0, &e);
    if (!f.ok()) {
        *error = f.message;
        return false;
    }
    *estimate = e;
    return true;
}

PurgeReport purgeSubject(Store &store, Journal &journal, const PurgeOptions &options)
{
    PurgeReport report;
    auto fail = [&](const CompactionFailure &f) {
        report.ok = false;
        report.failure = f;
        return report;
    };

    const UtcTime now = options.now.isValid() ? options.now : UtcTime::now();
    if (!store.isOpen() || store.path().isEmpty() || store.path() == QLatin1String(":memory:"))
        return fail(failure(CompactionError::io(), QStringLiteral("the store must be open on a file")));
    if (!journal.isOpen())
        return fail(failure(CompactionError::io(), QStringLiteral("the journal is not open")));
    UtcTime cutoff;
    CompactionFailure f = checkArguments(options, now, &cutoff);
    if (!f.ok())
        return fail(f);

    ProgressReporter progress(options.progress, OperationProgress::Purging);
    const qint64 sizeBefore = store.sizeBytes();

    // Decisions come from a read snapshot of their own; the deletions go through the writer.
    // The dry run and the run read the same snapshot, so they make the same decisions.
    Store reader;
    if (!reader.openReadOnly(store.path()) || !reader.beginSnapshot())
        return fail(failure(CompactionError::io(), QStringLiteral("the store cannot be read")));

    // REQ-STO-16: the space to rewrite the file is checked before anything is deleted.
    f = dryRun(reader, journal, options, cutoff, sizeBefore, freeSpaceForStore(store, options.freeSpace), &progress,
               &report.estimate);
    if (!f.ok())
        return fail(f);
    if (report.estimate.space == CompactionEstimate::SpaceShort)
        return fail(failure(CompactionError::insufficientSpace(),
                            QStringLiteral("rewriting the files needs %1 bytes of free space, %2 are missing")
                                .arg(report.estimate.neededBytes()).arg(report.estimate.missingBytes())));
    if (report.estimate.space == CompactionEstimate::SpaceUnknown && !options.proceedWhenFreeSpaceUnknown)
        return fail(failure(CompactionError::freeSpaceUnknown(),
                            QStringLiteral("the free space is unknown; rewriting the files needs %1 bytes")
                                .arg(report.estimate.neededBytes())));
    if (report.estimate.totalRows() == 0) {
        // Nothing to remove: nothing changes, not even the files.
        progress.report(OperationProgress::Done, 1, 1, true);
        report.ok = true;
        return report;
    }
    const qint64 storeRows = report.estimate.totalRows() - report.estimate.journalRows();
    if (storeRows == 0) {
        // The store part was done by an earlier run (or there is nothing in it): only the journal.
        reader.endSnapshot();
        if (!progress.report(OperationProgress::Finalizing, 0, 1, true))
            return fail(failure(CompactionError::cancelled(), QStringLiteral("cancelled")));
        report.storePartDone = true;
        return finishJournal(journal, cutoff, options, &progress, &report);
    }

    if (!store.beginPurge())
        return fail(failure(CompactionError::io(), QStringLiteral("the store cannot be opened for writing")));

    Actions actions;
    actions.removeRecord = [&](const RecordKey &k) { return store.removeRecord(k); };
    actions.removeMark = [&](const Mark &m) { return store.removeMark(m.id, cutoff); };
    bool sensorError = false;
    actions.removeSensor = [&](const Sensor &s) {
        const int n = store.removeSensor(s.subject, s.sensorId, cutoff);
        if (n < 0) sensorError = true;
        return n == 1;                  // 0: seen again after the snapshot, it stays
    };
    actions.removeQuarantine = [&](const QUuid &id) { return store.removeQuarantineEntry(id); };
    actions.removeIngest = [&](const QUuid &id) { return store.removeIngestEntry(id); };

    Counters c;
    f = scan(reader, options, cutoff, actions, &progress, OperationProgress::Purging, true, &c);
    reader.endSnapshot();
    if (f.ok() && sensorError)
        f = failure(CompactionError::io(), QStringLiteral("a sensor row could not be removed"));
    if (f.ok() && !progress.report(OperationProgress::Finalizing, 0, 1, true))
        f = failure(CompactionError::cancelled(), QStringLiteral("cancelled"));
    if (!f.ok()) {
        store.rollbackPurge();          // nothing changes
        return fail(f);
    }
    if (!store.commitPurge())
        return fail(failure(CompactionError::io(), QStringLiteral("the purge could not be committed")));

    report.storePartDone = true;
    if (c.records) report.removed.insert(QStringLiteral("record"), c.records);
    if (c.marks) report.removed.insert(QStringLiteral("mark"), c.marks);
    if (c.sensors) report.removed.insert(QStringLiteral("sensor"), c.sensors);
    if (c.quarantine) report.removed.insert(QStringLiteral("quarantine"), c.quarantine);
    if (c.ingest) report.removed.insert(QStringLiteral("ingest"), c.ingest);
    const qint64 sizeAfter = store.sizeBytes();
    report.bytesFreed = sizeBefore > sizeAfter ? sizeBefore - sizeAfter : 0;
    return finishJournal(journal, cutoff, options, &progress, &report);
}

} // namespace lm
