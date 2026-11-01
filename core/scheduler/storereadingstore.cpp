// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Michele Brami
#include "scheduler/storereadingstore.h"

namespace lm {

namespace {

// The metric a source tag carries: the logbook feed holds events, every other feed glucose.
QString metricOfSource(const QString &source)
{
    return source.endsWith(QLatin1String(".logbook")) ? QStringLiteral("logbook_entry")
                                                      : QStringLiteral("glucose");
}

} // namespace

UtcTime StoreReadingStore::newestMeasurement(const QUuid &subject, const QString &source)
{
    Record r;
    // Raw view (effective = false): marks that ignore a record must not make the scheduler ask
    // for it again.
    if (m_store->latest(subject, metricOfSource(source), source, false, &r))
        return r.measuredAt;
    return UtcTime();
}

IngestContext StoreReadingStore::contextFor(const LinkId &link) const
{
    IngestContext c;
    c.now = m_clock->now();
    c.adapter = link.adapterId;
    c.adapterVersion = link.adapterVersion;
    c.sessionId = link.sessionId;
    return c;
}

int StoreReadingStore::ingest(const QUuid &subject, const LinkId &link, const QVector<Record> &records)
{
    QVector<IngestItem> items;
    for (int i = 0; i < records.size(); ++i) {
        IngestItem item;
        item.record = records.at(i);
        item.record.subject = subject;
        // The adapter does not send the ingest time; the core sets it from its own clock
        // (contract 5.6, REQ-DAT-02).
        if (!item.record.ingestedAt.isValid())
            item.record.ingestedAt = m_clock->now();
        items.append(item);
    }
    const IngestResult r = m_store->ingest(items, contextFor(link));
    return r.ok ? r.added : -1;         // not ok: nothing was written, the caller keeps the batch
}

void StoreReadingStore::quarantine(const LinkId &link, const QString &reason, const QString &detail,
                                   const QJsonObject &rawRecord)
{
    Validation::Reason why;
    if (!validationReasonFromName(reason, &why))
        why = Validation::InvalidPayload;
    IngestItem item;
    item.raw = rawRecord;
    item.rejected.reason = why;
    item.rejected.detail = detail;
    QVector<IngestItem> items;
    items.append(item);
    m_store->ingest(items, contextFor(link));
}

bool StoreReadingStore::latestSensor(const QUuid &subject, UtcTime *activatedAt, int *warmupMinutes)
{
    const QVector<Sensor> all = m_store->sensors();
    const Sensor *best = 0;
    for (int i = 0; i < all.size(); ++i)
        if (all.at(i).subject == subject && (!best || best->lastSeen < all.at(i).lastSeen))
            best = &all.at(i);
    if (!best || !best->activatedAt.isValid() || best->warmupMinutes < 0)
        return false;
    *activatedAt = best->activatedAt;
    *warmupMinutes = best->warmupMinutes;
    return true;
}

void StoreReadingStore::noteSensor(const QUuid &subject, const QJsonObject &sensor)
{
    Sensor s;
    s.subject = subject;
    s.sensorId = sensor.value(QStringLiteral("id")).toString();
    if (s.sensorId.isEmpty())
        return;
    s.activatedAt = UtcTime::fromIso(sensor.value(QStringLiteral("activated_at")).toString());
    const QJsonValue warm = sensor.value(QStringLiteral("warmup_minutes"));
    s.warmupMinutes = warm.isDouble() ? int(warm.toDouble()) : -1;
    s.firstSeen = s.lastSeen = m_clock->now();
    m_store->noteSensor(s);
}

bool StoreIngestLogSink::append(const IngestLogEntry &e)
{
    IngestEntry x;
    x.time = e.time;
    x.event = e.event;
    x.adapter = e.adapter;
    x.adapterVersion = e.adapterVersion;
    x.sessionId = e.sessionId;
    x.detail = e.detail;
    if (!e.connection.isEmpty())
        x.extra.insert(QStringLiteral("connection"), e.connection);
    return m_store->appendIngestEntry(x);
}

} // namespace lm
