// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Michele Brami
#ifndef LM_STOREREADINGSTORE_H
#define LM_STOREREADINGSTORE_H

// The scheduler's view of the measurement store and the sink for ingest-log
// entries, both over a Store. Pattern: adapter. The store is append-only and knows nothing of
// adapters or schedules; these two classes translate between the scheduler's narrow ports and
// the store's API. The store is not owned and must outlive them.

#include "guard/ports.h"
#include "scheduler/schedulerports.h"
#include "store/store.h"

namespace lm {

class StoreReadingStore : public ReadingStore
{
public:
    // `clock` gives the time used for validation and receipt (IngestContext::now).
    StoreReadingStore(Store *store, Clock *clock) : m_store(store), m_clock(clock) {}

    UtcTime newestMeasurement(const QUuid &subject, const QString &source);
    int ingest(const QUuid &subject, const LinkId &link, const QVector<Record> &records);
    void quarantine(const LinkId &link, const QString &reason, const QString &detail,
                    const QJsonObject &rawRecord);
    bool latestSensor(const QUuid &subject, UtcTime *activatedAt, int *warmupMinutes);
    void noteSensor(const QUuid &subject, const QJsonObject &sensor);

private:
    IngestContext contextFor(const LinkId &link) const;

    Store *m_store;
    Clock *m_clock;
};

// Writes ingest-log entries to the store (REQ-GRD-10, REQ-GRD-11). The connection id travels
// as the extra key `connection`, which the archive carries unchanged.
class StoreIngestLogSink : public IngestLogSink
{
public:
    explicit StoreIngestLogSink(Store *store) : m_store(store) {}
    bool append(const IngestLogEntry &entry);

private:
    Store *m_store;
};

} // namespace lm

#endif
