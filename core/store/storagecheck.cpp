// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Michele Brami
#include "store/storagecheck.h"

#include <QFileInfo>
#include <QStorageInfo>

namespace lm {

namespace {

const qint64 kDayMs = 86400000LL;

StorageCheckResult fromEntry(const JournalEntry &e)
{
    StorageCheckResult r;
    r.ok = true;
    r.recorded = true;
    r.time = e.wallTime;
    r.storeBytes = qint64(e.extra.value(QStringLiteral("store_bytes")).toDouble());
    r.journalBytes = qint64(e.extra.value(QStringLiteral("journal_bytes")).toDouble());
    r.thresholdBytes = qint64(e.extra.value(QStringLiteral("threshold_bytes")).toDouble());
    r.freeKnown = e.extra.contains(QStringLiteral("free_bytes"));
    r.freeBytes = r.freeKnown ? qint64(e.extra.value(QStringLiteral("free_bytes")).toDouble()) : -1;
    r.low = e.extra.value(QStringLiteral("low")).toBool();
    r.pruned = qint64(e.extra.value(QStringLiteral("pruned")).toDouble());
    return r;
}

} // namespace

bool storageCheckDue(Journal &journal, const UtcTime &now)
{
    const QVector<JournalEntry> checks = journal.entries(JournalKind::storageCheck());
    if (checks.isEmpty() || !now.isValid())
        return true;
    const qint64 last = checks.last().wallTime.msecsSinceEpoch();
    const qint64 interval = qint64(kStorageCheckIntervalDays) * kDayMs;
    const qint64 t = now.msecsSinceEpoch();
    // A last check in the future by less than a week is the clock being a little off: not due.
    // By more than a week the clock was set back: due.
    return t - last >= interval || last - t >= interval;
}

StorageCheckResult runStorageCheck(Store &store, Journal &journal, const StorageCheckOptions &options)
{
    StorageCheckResult r;
    if (!store.isOpen() || !journal.isOpen()) {
        r.error = QStringLiteral("the store and the journal must be open");
        return r;
    }
    const UtcTime now = options.now.isValid() ? options.now : UtcTime::now();
    r.time = now;

    if (!options.force && !storageCheckDue(journal, now)) {
        r.ok = true;
        r.skipped = true;
        return r;
    }

    // REQ-SCH-14: the journal is pruned at the weekly check. A failure does not stop the check.
    if (options.prune) {
        const PruneResult p = journal.pruneJournal(now);
        if (p.ok)
            r.pruned = p.removed;
        else
            r.pruneError = journal.lastError();
    }

    // The size of the store: all collections, the ingest log and the journal (REQ-STO-02).
    r.storeBytes = store.sizeBytes();
    r.journalBytes = journal.sizeBytes();
    r.thresholdBytes = qint64(kLowSpaceFactor) * r.totalBytes();

    const QString directory = QFileInfo(store.path()).absolutePath();
    const qint64 free = options.freeSpace ? options.freeSpace(directory)
                                          : QStorageInfo(directory).bytesAvailable();
    r.freeKnown = free >= 0;
    r.freeBytes = r.freeKnown ? free : -1;
    r.low = r.freeKnown && free < r.thresholdBytes;

    JournalEntry e;
    e.kind = JournalKind::storageCheck();
    e.wallTime = now;
    e.extra.insert(QStringLiteral("store_bytes"), double(r.storeBytes));
    e.extra.insert(QStringLiteral("journal_bytes"), double(r.journalBytes));
    if (r.freeKnown)
        e.extra.insert(QStringLiteral("free_bytes"), double(r.freeBytes));
    e.extra.insert(QStringLiteral("threshold_bytes"), double(r.thresholdBytes));
    e.extra.insert(QStringLiteral("low"), r.low);
    if (r.pruned > 0)
        e.extra.insert(QStringLiteral("pruned"), double(r.pruned));
    if (!journal.append(e)) {
        r.error = journal.lastError();
        return r;
    }
    r.recorded = true;
    r.ok = true;
    return r;
}

bool lastStorageCheck(Journal &journal, StorageCheckResult *result)
{
    const QVector<JournalEntry> checks = journal.entries(JournalKind::storageCheck());
    if (checks.isEmpty())
        return false;
    *result = fromEntry(checks.last());
    return true;
}

} // namespace lm
