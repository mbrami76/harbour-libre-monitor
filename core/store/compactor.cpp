// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Michele Brami
//
// Compaction and its dry run (compactor.h). One scan decides what goes; the dry run and the
// real run differ only in what they do with a decision (count it, or count it and delete the
// row), so the estimate and the run cannot disagree on the rule.
#include "store/compactor.h"

#include "record/registry.h"

#include <QDateTime>
#include <QFileInfo>
#include <QStorageInfo>


namespace lm {

// ---- window ---------------------------------------------------------------------------------

UtcTime FullResolutionWindow::cutoff(const UtcTime &now) const
{
    if (!now.isValid())
        return UtcTime();
    if (days > 0)
        return UtcTime::fromMSecsSinceEpoch(now.msecsSinceEpoch() - qint64(days) * 86400000LL);
    if (months <= 0)
        return UtcTime();
    // Calendar months in UTC: QDate::addMonths clamps the day to the length of the month.
    const QDateTime t = QDateTime::fromMSecsSinceEpoch(now.msecsSinceEpoch(), Qt::UTC);
    return UtcTime::fromMSecsSinceEpoch(QDateTime(t.date().addMonths(-months), t.time(), Qt::UTC).toMSecsSinceEpoch());
}

bool FullResolutionWindow::isAllowed(const UtcTime &now) const
{
    const UtcTime c = cutoff(now);
    return c.isValid()
        && c.msecsSinceEpoch() <= now.msecsSinceEpoch() - qint64(kMinimumFullResolutionDays) * 86400000LL;
}

qint64 CompactionReport::totalRemoved() const
{
    qint64 n = 0;
    for (QMap<QString, qint64>::const_iterator it = removed.begin(); it != removed.end(); ++it)
        n += it.value();
    return n;
}

qint64 rebuildNeedFor(qint64 storeBytes, qint64 freedBytes)
{
    const qint64 remaining = qMax<qint64>(0, storeBytes - qMax<qint64>(0, freedBytes));
    const qint64 margin = qMax<qint64>(kRebuildMarginMinimumBytes, remaining * kRebuildMarginPercent / 100);
    return remaining + margin;
}

namespace {

CompactionFailure failure(const QString &errorClass, const QString &message)
{
    CompactionFailure f;
    f.errorClass = errorClass;
    f.message = message;
    return f;
}

// Floor division that also works for times before 1970, so that slot numbers are aligned to
// UTC whatever the sign.
qint64 floorDiv(qint64 a, qint64 b)
{
    qint64 q = a / b;
    if ((a % b != 0) && ((a < 0) != (b < 0)))
        --q;
    return q;
}

} // namespace

qint64 freeSpaceForStore(const Store &store, const FreeSpaceFn &freeSpace)
{
    const QString path = store.path();
    if (path.isEmpty() || path == QLatin1String(":memory:"))
        return -1;
    const QString directory = QFileInfo(path).absolutePath();
    if (freeSpace)
        return freeSpace(directory);
    const QStorageInfo info(directory);
    return info.isValid() ? info.bytesAvailable() : -1;
}

qint64 estimateBytesOfRows(Store &reader, qint64 storeBytes, qint64 rows)
{
    const qint64 all = reader.rowCount(QStringLiteral("record")) + reader.rowCount(QStringLiteral("quarantine"))
        + reader.rowCount(QStringLiteral("ingest")) + reader.rowCount(QStringLiteral("mark"))
        + reader.rowCount(QStringLiteral("sensor"));
    const double average = all > 0 ? double(storeBytes) / double(all) : 0.0;
    return qint64(double(rows) * average);
}

CompactionEstimate::Space spaceVerdict(qint64 freeBytes, qint64 needBytes)
{
    if (freeBytes < 0)
        return CompactionEstimate::SpaceUnknown;
    return freeBytes >= needBytes ? CompactionEstimate::SpaceEnough : CompactionEstimate::SpaceShort;
}

namespace {

// ---- the scan --------------------------------------------------------------------------------

// What the scan does with a decision. The dry run counts only; the real run deletes.
struct Actions
{
    std::function<bool(const RecordKey &)> removeRecord;
    std::function<bool(const QUuid &)> removeQuarantine;
    std::function<bool(const QUuid &)> removeIngest;
};

struct Counters
{
    Counters() : records(0), derived(0), quarantine(0), ingest(0), slotCount(0), kept(0) {}

    qint64 records, derived, quarantine, ingest, slotCount, kept;
    UtcTime earliest;               // of the removed rows

    void note(const UtcTime &t)
    {
        if (!earliest.isValid() || t < earliest)
            earliest = t;
    }
};

struct Sample
{
    qint64 t;
    RecordKey key;
};

// Chooses the sample of one slot to keep (REQ-STO-07): the one nearest to the start of
// the slot, which is the earliest, since every sample of the slot lies at or after its start;
// of samples with the same time the first in key order (the earlier one). A slot with a
// single sample keeps it. Pure; tested through the public behaviour of compactStore.
int chooseKept(const QVector<Sample> &s)
{
    int keep = 0;
    for (int i = 1; i < s.size(); ++i)
        if (s[i].t < s[keep].t)             // strict: a tie stays with the earlier one in key order
            keep = i;
    return keep;
}

// Collects the thinnable samples of a slot and thins it when the slot is complete. The stream
// is in key order (Subject, kind, time), so a slot is finished when the next sample belongs
// to another slot, Subject or kind; only one slot is held in memory at a time. Within the
// slot, samples are grouped by source tag (adapter and feed) and unit, so units never mix.
class Thinner
{
public:
    Thinner(const Actions &actions, Counters *counters) : m_actions(actions), m_counters(counters), m_slot(0), m_open(false), m_failed(false) {}

    bool failed() const { return m_failed; }

    void add(const Record &r, qint64 slotMs)
    {
        const qint64 slot = floorDiv(r.measuredAt.msecsSinceEpoch(), slotMs);
        if (m_open && (r.subject != m_subject || r.metric != m_metric || slot != m_slot))
            flush();
        m_open = true;
        m_subject = r.subject;
        m_metric = r.metric;
        m_slot = slot;
        Sample s;
        s.t = r.measuredAt.msecsSinceEpoch();
        s.key = r.key();
        m_groups[r.source + QLatin1Char('\n') + r.unit].append(s);
    }

    void flush()
    {
        for (QMap<QString, QVector<Sample> >::iterator it = m_groups.begin(); it != m_groups.end(); ++it) {
            const QVector<Sample> &samples = it.value();
            const int keep = chooseKept(samples);
            ++m_counters->slotCount;
            for (int i = 0; i < samples.size(); ++i) {
                if (i == keep) {
                    ++m_counters->kept;
                } else if (!m_failed) {
                    if (m_actions.removeRecord(samples[i].key)) {
                        ++m_counters->records;
                        m_counters->note(UtcTime::fromMSecsSinceEpoch(samples[i].t));
                    } else {
                        m_failed = true;
                    }
                }
            }
        }
        m_groups.clear();
        m_open = false;
    }

private:
    Actions m_actions;
    Counters *m_counters;
    QUuid m_subject;
    QString m_metric;
    qint64 m_slot;
    bool m_open;
    bool m_failed;
    QMap<QString, QVector<Sample> > m_groups;
};

// The feed part of a source tag "<adapter>.<feed>" (REQ-DAT-09); empty when there is none.
QString feedOf(const QString &source)
{
    const int dot = source.indexOf(QLatin1Char('.'));
    return dot < 0 ? QString() : source.mid(dot + 1);
}

// The decisions, shared by the dry run and the real run. "phase" is the progress phase this
// pass reports under (checking-space for the dry run, compacting for the real one).
CompactionFailure scan(Store &reader, const Actions &actions, const UtcTime &cutoff,
                       ProgressReporter *progress, OperationProgress::Phase phase,
                       const std::function<bool(qint64)> &hook, Counters *counters)
{
    const MetricRegistry &registry = MetricRegistry::instance();
    const qint64 total = reader.countRecordsOlderThan(cutoff);
    qint64 scanned = 0;
    CompactionFailure stop;
    Thinner thinner(actions, counters);

    if (progress && !progress->report(phase, 0, total, true))
        return failure(CompactionError::cancelled(), QStringLiteral("cancelled"));

    const bool read = reader.forEachRecordOlderThan(cutoff, [&](const Record &r) {
        if (!stop.ok() || thinner.failed())
            return;
        ++scanned;
        if (hook && !hook(scanned)) {
            stop = failure(CompactionError::aborted(), QStringLiteral("stopped by the test hook"));
            return;
        }
        if (progress && ((scanned & 127) == 0 || scanned == total)
            && !progress->report(phase, scanned, total)) {
            stop = failure(CompactionError::cancelled(), QStringLiteral("cancelled"));
            return;
        }

        if (r.origin == Origin::Derived) {
            // Recomputable from the samples, so removed when old (REQ-STO-06).
            if (actions.removeRecord(r.key())) {
                ++counters->records;
                ++counters->derived;
                counters->note(r.measuredAt);
            } else {
                stop = failure(CompactionError::io(), QStringLiteral("a record could not be removed"));
            }
            return;
        }
        if (r.origin != Origin::Adapter || r.shape != Record::Sample)
            return;                                     // manual records and events stay in full
        const ThinningRule rule = registry.thinningFor(r.metric);
        if (!rule.thinsFeed(feedOf(r.source)))
            return;                                     // other feeds and unknown kinds are never thinned (REQ-STO-07, REQ-STO-12)
        const qint64 slotMs = qint64(rule.slotMinutes) * 60000;
        // Whole slots only: the one that crosses the end of the window is left alone.
        const qint64 alignedCutoff = floorDiv(cutoff.msecsSinceEpoch(), slotMs) * slotMs;
        if (r.measuredAt.msecsSinceEpoch() >= alignedCutoff)
            return;
        thinner.add(r, slotMs);
    });
    thinner.flush();

    if (!stop.ok())
        return stop;
    if (!read || thinner.failed())
        return failure(CompactionError::io(), QStringLiteral("the store could not be read or written"));

    // Quarantine and the ingest log older than the window (REQ-STO-06).
    bool ok = reader.forEachQuarantine([&](const QuarantineEntry &e) {
        if (!stop.ok() || e.receivedAt.msecsSinceEpoch() >= cutoff.msecsSinceEpoch())
            return;
        if (actions.removeQuarantine(e.id)) {
            ++counters->quarantine;
            counters->note(e.receivedAt);
        } else {
            stop = failure(CompactionError::io(), QStringLiteral("a quarantine entry could not be removed"));
        }
    });
    ok = ok && reader.forEachIngestEntry([&](const IngestEntry &e) {
        if (!stop.ok() || e.time.msecsSinceEpoch() >= cutoff.msecsSinceEpoch())
            return;
        if (actions.removeIngest(e.id)) {
            ++counters->ingest;
            counters->note(e.time);
        } else {
            stop = failure(CompactionError::io(), QStringLiteral("an ingest-log entry could not be removed"));
        }
    });
    if (!stop.ok())
        return stop;
    if (!ok)
        return failure(CompactionError::io(), QStringLiteral("the store could not be read"));
    return CompactionFailure();
}

// The one calculation of REQ-STO-13 and REQ-STO-16: the dry run over "reader", then the bytes
// freed (rows times the average row size of the store), the rebuild need and the verdict on
// the free space. estimateCompaction and compactStore both call it, so what the user is shown
// is what the run checks.
CompactionFailure dryRun(Store &reader, const UtcTime &cutoff, qint64 storeBytes, qint64 freeBytes,
                         ProgressReporter *progress, CompactionEstimate *e)
{
    Counters counters;
    Actions count;
    count.removeRecord = [](const RecordKey &) { return true; };
    count.removeQuarantine = [](const QUuid &) { return true; };
    count.removeIngest = [](const QUuid &) { return true; };
    const CompactionFailure f = scan(reader, count, cutoff, progress, OperationProgress::CheckingSpace,
                                     std::function<bool(qint64)>(), &counters);
    if (!f.ok())
        return f;

    e->cutoff = cutoff;
    e->records = counters.records;
    e->quarantine = counters.quarantine;
    e->ingest = counters.ingest;
    e->storeBytes = storeBytes;
    e->bytes = estimateBytesOfRows(reader, storeBytes, e->totalRows());
    e->rebuildNeedBytes = rebuildNeedFor(storeBytes, e->bytes);
    e->freeBytes = freeBytes >= 0 ? freeBytes : -1;
    e->space = spaceVerdict(freeBytes, e->rebuildNeedBytes);
    return CompactionFailure();
}

} // namespace

bool estimateCompaction(Store &store, const FullResolutionWindow &window, const UtcTime &now,
                        CompactionEstimate *estimate, QString *error, const FreeSpaceFn &freeSpace)
{
    const UtcTime t = now.isValid() ? now : UtcTime::now();
    if (!store.isOpen()) {
        *error = QStringLiteral("the store is not open");
        return false;
    }
    if (!window.isAllowed(t)) {
        *error = QStringLiteral("the full-resolution window is shorter than 14 days");
        return false;
    }
    CompactionEstimate e;
    const CompactionFailure f = dryRun(store, window.cutoff(t), store.sizeBytes(), freeSpaceForStore(store, freeSpace), 0, &e);
    if (!f.ok()) {
        *error = f.message;
        return false;
    }
    *estimate = e;
    return true;
}

CompactionReport compactStore(Store &store, const CompactionOptions &options)
{
    CompactionReport report;
    auto fail = [&](const CompactionFailure &f) {
        report.ok = false;
        report.failure = f;
        return report;
    };

    const UtcTime now = options.now.isValid() ? options.now : UtcTime::now();
    if (!store.isOpen() || store.path().isEmpty() || store.path() == QLatin1String(":memory:"))
        return fail(failure(CompactionError::io(), QStringLiteral("the store must be open on a file")));
    if (!options.window.isAllowed(now))
        return fail(failure(CompactionError::windowTooShort(), QStringLiteral("the full-resolution window is shorter than 14 days")));
    report.cutoff = options.window.cutoff(now);

    ProgressReporter progress(options.progress);
    const qint64 sizeBefore = store.sizeBytes();

    // Decisions come from a read snapshot of their own; the deletions go through the writer.
    // The dry run and the real run read the same snapshot, so they make the same decisions.
    Store reader;
    if (!reader.openReadOnly(store.path()) || !reader.beginSnapshot())
        return fail(failure(CompactionError::io(), QStringLiteral("the store cannot be read")));

    // REQ-STO-16: the space for the rebuild is checked before anything is deleted.
    CompactionFailure f = dryRun(reader, report.cutoff, sizeBefore, freeSpaceForStore(store, options.freeSpace),
                                 &progress, &report.estimate);
    if (!f.ok())
        return fail(f);
    if (report.estimate.space == CompactionEstimate::SpaceShort)
        return fail(failure(CompactionError::insufficientSpace(),
                            QStringLiteral("the rebuild needs %1 bytes of free space, %2 are free")
                                .arg(report.estimate.rebuildNeedBytes).arg(report.estimate.freeBytes)));
    if (report.estimate.space == CompactionEstimate::SpaceUnknown && !options.proceedWhenFreeSpaceUnknown)
        return fail(failure(CompactionError::freeSpaceUnknown(),
                            QStringLiteral("the free space is unknown; the rebuild needs %1 bytes")
                                .arg(report.estimate.rebuildNeedBytes)));

    if (!store.beginCompaction())
        return fail(failure(CompactionError::io(), QStringLiteral("the store cannot be opened for writing")));

    Actions actions;
    actions.removeRecord = [&](const RecordKey &k) { return store.removeRecord(k); };
    actions.removeQuarantine = [&](const QUuid &id) { return store.removeQuarantineEntry(id); };
    actions.removeIngest = [&](const QUuid &id) { return store.removeIngestEntry(id); };

    Counters counters;
    f = scan(reader, actions, report.cutoff, &progress, OperationProgress::Compacting, options.hook, &counters);
    reader.endSnapshot();

    const qint64 removedRows = counters.records + counters.quarantine + counters.ingest;
    if (f.ok() && removedRows > 0) {
        // The record of the run (REQ-STO-06): time range, counts removed, rule version. Never removed.
        CompactionEntry entry;
        entry.time = now;
        entry.rangeFrom = counters.earliest;
        entry.rangeTo = report.cutoff;
        if (report.cutoff < entry.rangeFrom)
            entry.rangeTo = entry.rangeFrom;
        if (counters.records) entry.removed.insert(QStringLiteral("record"), double(counters.records));
        if (counters.quarantine) entry.removed.insert(QStringLiteral("quarantine"), double(counters.quarantine));
        if (counters.ingest) entry.removed.insert(QStringLiteral("ingest"), double(counters.ingest));
        entry.ruleVersion = QString::fromLatin1(kThinningRuleVersion);
        if (!store.recordCompaction(entry))
            f = failure(CompactionError::io(), QStringLiteral("the compaction record could not be written"));
    }
    if (f.ok() && !progress.report(OperationProgress::Finalizing, 0, 1, true))
        f = failure(CompactionError::cancelled(), QStringLiteral("cancelled"));
    if (!f.ok() || removedRows == 0) {
        store.rollbackCompaction();         // an error, or nothing to remove: nothing changes
        if (!f.ok())
            return fail(f);
    } else if (!store.commitCompaction()) {
        return fail(failure(CompactionError::io(), QStringLiteral("the compaction could not be committed")));
    }

    report.ok = true;
    if (counters.records) report.removed.insert(QStringLiteral("record"), counters.records);
    if (counters.quarantine) report.removed.insert(QStringLiteral("quarantine"), counters.quarantine);
    if (counters.ingest) report.removed.insert(QStringLiteral("ingest"), counters.ingest);
    report.derivedRemoved = counters.derived;
    report.slotCount = counters.slotCount;
    report.kept = counters.kept;
    const qint64 sizeAfter = store.sizeBytes();
    report.bytesFreed = sizeBefore > sizeAfter ? sizeBefore - sizeAfter : 0;
    progress.report(OperationProgress::Finalizing, 1, 1, true);
    progress.report(OperationProgress::Done, 1, 1, true);
    return report;
}

} // namespace lm
