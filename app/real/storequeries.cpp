// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Michele Brami
#include "storequeries.h"

#include "alerts/alertjournal.h"
#include "record/registry.h"
#include "store/journal.h"
#include "store/store.h"
#include "units/glucoseunits.h"

#include <algorithm>

namespace lm {
namespace real {

namespace {
const char *kCurrentSource = "libre.current";
const char *kHistorySource = "libre.graph";
}

bool toMgDl(double value, const QString &unit, double *mgdl)
{
    if (unit == QLatin1String("mg/dL")) { *mgdl = value; return true; }
    if (unit == QLatin1String("mmol/L")) { *mgdl = value * kMgDlPerMmolL; return true; }
    return false;
}

StoreQueries::StoreQueries() : m_store(new Store), m_journal(new Journal) {}

StoreQueries::~StoreQueries()
{
    close();
    delete m_journal;
    delete m_store;
}

bool StoreQueries::open(const QString &storePath, const QString &journalPath)
{
    close();
    if (!m_store->openReadOnly(storePath))
        return false;
    // The journal only matters for the gaps in the plot: a missing one is not fatal.
    m_journal->openReadOnly(journalPath);
    return true;
}

bool StoreQueries::isOpen() const { return m_store->isOpen(); }

void StoreQueries::close()
{
    m_store->close();
    m_journal->close();
}

namespace {

void append(QVector<ui::PlotPoint> *out, const QVector<Record> &records, qint64 *newest)
{
    for (int i = 0; i < records.size(); ++i) {
        double mg;
        if (!toMgDl(records.at(i).value, records.at(i).unit, &mg))
            continue;
        out->append(ui::PlotPoint(records.at(i).measuredAt.msecsSinceEpoch(), mg));
        if (newest && records.at(i).measuredAt.msecsSinceEpoch() > *newest)
            *newest = records.at(i).measuredAt.msecsSinceEpoch();
    }
}

} // namespace

// The most recently seen sensor of the patient from the store's `sensor` table, read-only (REQ-SEN-01,
//). The same choice as the service's Status() (daemon/servicecore.cpp
// `latestSensor`: the largest last-seen time), so the page and the service name the same sensor. The table
// is small (one row per sensor ever worn), so it is read whole.
static ui::SensorFacts latestSensorOf(Store *store, const QUuid &subject)
{
    ui::SensorFacts f;
    const QVector<Sensor> all = store->sensors();
    const Sensor *best = 0;
    for (int i = 0; i < all.size(); ++i)
        if (all.at(i).subject == subject && (!best || best->lastSeen < all.at(i).lastSeen))
            best = &all.at(i);
    if (!best)
        return f;
    f.known = true;
    f.id = best->sensorId;
    f.activatedMs = best->activatedAt.isValid() ? best->activatedAt.msecsSinceEpoch() : -1;
    f.warmupMinutes = best->warmupMinutes;
    f.firstSeenMs = best->firstSeen.isValid() ? best->firstSeen.msecsSinceEpoch() : -1;
    f.lastSeenMs = best->lastSeen.isValid() ? best->lastSeen.msecsSinceEpoch() : -1;
    return f;
}

SubjectData StoreQueries::load(const QUuid &subject, qint64 fromMs)
{
    SubjectData d;
    if (!m_store->isOpen())
        return d;
    d.loadedFromMs = fromMs;

    Record latest;
    if (m_store->latest(subject, metricGlucose(), QLatin1String(kCurrentSource), true, &latest)) {
        d.hasReading = true;
        d.measuredAtMs = latest.measuredAt.msecsSinceEpoch();
        d.value = latest.value;          // in its stored unit, for the big value
        d.unit = latest.unit;
    }
    RecordQuery q;
    q.subject = subject;
    q.metric = metricGlucose();
    q.effective = true;
    q.from = UtcTime::fromMSecsSinceEpoch(fromMs);
    q.source = QLatin1String(kCurrentSource);
    qint64 newestCurrent = 0;
    append(&d.current, m_store->records(q), &newestCurrent);
    q.source = QLatin1String(kHistorySource);
    append(&d.history, m_store->records(q), &d.newestHistoryMs);
    const SubjectStats st = m_store->subjectStats(subject);
    d.firstDataMs = st.first.isValid() ? st.first.msecsSinceEpoch() : 0;
    d.sensor = latestSensorOf(m_store, subject);
    return d;
}

void StoreQueries::refresh(const QUuid &subject, SubjectData *data)
{
    if (!m_store->isOpen())
        return;
    Record latest;
    if (m_store->latest(subject, metricGlucose(), QLatin1String(kCurrentSource), true, &latest)) {
        data->hasReading = true;
        data->measuredAtMs = latest.measuredAt.msecsSinceEpoch();
        data->value = latest.value;
        data->unit = latest.unit;
    }
    RecordQuery q;
    q.subject = subject;
    q.metric = metricGlucose();
    q.effective = true;
    // Strictly newer than what is held; the key rule of the store makes a repeat harmless anyway.
    const qint64 lastCur = data->current.isEmpty() ? data->loadedFromMs : data->current.last().tMs + 1;
    q.source = QLatin1String(kCurrentSource);
    q.from = UtcTime::fromMSecsSinceEpoch(lastCur);
    qint64 dummy = 0;
    append(&data->current, m_store->records(q), &dummy);
    const qint64 lastHis = data->history.isEmpty() ? data->loadedFromMs : data->history.last().tMs + 1;
    q.source = QLatin1String(kHistorySource);
    q.from = UtcTime::fromMSecsSinceEpoch(lastHis);
    append(&data->history, m_store->records(q), &data->newestHistoryMs);
    if (data->firstDataMs == 0) {
        const SubjectStats st = m_store->subjectStats(subject);
        data->firstDataMs = st.first.isValid() ? st.first.msecsSinceEpoch() : 0;
    }
    data->sensor = latestSensorOf(m_store, subject);
}

QVector<QPair<qint64, qint64> > StoreQueries::serviceGaps()
{
    QVector<QPair<qint64, qint64> > out;
    if (!m_journal->isOpen())
        return out;
    const QVector<JournalGap> gaps = m_journal->gaps();
    for (int i = 0; i < gaps.size(); ++i)
        if (gaps.at(i).from.isValid() && gaps.at(i).to.isValid())
            out.append(qMakePair(gaps.at(i).from.msecsSinceEpoch(), gaps.at(i).to.msecsSinceEpoch()));
    return out;
}

bool StoreQueries::thresholdHistory(const QString &journalPath, const QUuid &subject,
                                    QList<ui::ThresholdHistoryEntry> *out)
{
    out->clear();
    if (!m_journal->isOpen() && !m_journal->openReadOnly(journalPath))
        return false;
    const QVector<ThresholdChange> all = loadThresholdChanges(m_journal);
    QVector<ThresholdChange> mine;
    for (int i = 0; i < all.size(); ++i)
        if (all.at(i).subject == subject && all.at(i).time.isValid())
            mine.append(all.at(i));
    // The book's order (time, then id), so equal times never swap between two reads.
    std::sort(mine.begin(), mine.end(), [](const ThresholdChange &a, const ThresholdChange &b) {
        const qint64 ta = a.time.msecsSinceEpoch(), tb = b.time.msecsSinceEpoch();
        return ta != tb ? ta < tb : a.id.toString() < b.id.toString();
    });
    for (int i = 0; i < mine.size(); ++i) {
        ui::ThresholdHistoryEntry e;
        e.timeMs = mine.at(i).time.msecsSinceEpoch();
        e.boundary = boundaryName(mine.at(i).boundary);
        e.value = mine.at(i).value;
        e.previous = mine.at(i).previous;
        out->append(e);         // numbers and time only: no comment
    }
    return true;
}

} // namespace real
} // namespace lm
