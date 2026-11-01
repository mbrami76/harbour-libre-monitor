// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Michele Brami
#ifndef LM_REAL_STOREQUERIES_H
#define LM_REAL_STOREQUERIES_H

// What the interface reads from the store and the journal, through connections that can never
// write (Store::openReadOnly, Journal::openReadOnly). The service is the
// only writer (REQ-ARC-03); WAL lets these reads run while it ingests.
//
// Pattern: a small synchronous class. A Qt SQL connection belongs to one thread, so the object
// that owns one must live where it is used: the worker thread of RealBackend creates its own
// StoreQueries. Nothing here knows about threads.
//
// What it returns: the newest `current` reading in its stored unit (for the big value), and the
// two glucose feeds of the last `windowMs` as exact mg/dL points (the stored value converted by the
// documented factor, never from a rounded one, REQ-DAT-12) for the plot. Readings in a unit other
// than mg/dL and mmol/L are left out: they were quarantined at ingest and cannot be here, so
// this is only a guard.
//
// Assumption: source tags are "libre.current" and "libre.graph" (REQ-DAT-09), the metric is
// "glucose". Another adapter would need its own tags (REQ-ARC-06).

#include "ui/backend.h"
#include "ui/plotreduce.h"

#include <QPair>
#include <QString>
#include <QUuid>
#include <QVector>

namespace lm {

class Store;
class Journal;

namespace real {

struct SubjectData
{
    SubjectData() : hasReading(false), measuredAtMs(0), value(0), firstDataMs(0), loadedFromMs(0),
                    newestHistoryMs(0) {}

    bool hasReading;
    qint64 measuredAtMs;
    double value;
    QString unit;
    QVector<ui::PlotPoint> current, history;    // sorted by time, exact mg/dL
    qint64 firstDataMs;                         // the oldest stored reading of the Subject; 0 = none
    qint64 loadedFromMs;                        // the series start here (the window asked for)
    qint64 newestHistoryMs;
    ui::SensorFacts sensor;                     // the most recently seen sensor (REQ-SEN-01), read-only
};

class StoreQueries
{
public:
    StoreQueries();
    ~StoreQueries();

    // Opens both read-only; false if the store cannot be opened (not created yet, or damaged).
    bool open(const QString &storePath, const QString &journalPath);
    bool isOpen() const;
    void close();

    // The whole picture of one Subject for the window [fromMs, now].
    SubjectData load(const QUuid &subject, qint64 fromMs);
    // Only what is newer than `data` already holds, appended to it (after DataChanged).
    void refresh(const QUuid &subject, SubjectData *data);

    // The periods in which the service was not running, from the journal (REQ-SCH-10).
    QVector<QPair<qint64, qint64> > serviceGaps();

    // The threshold edits of one patient, oldest first, from the journal's `threshold` lines read-only
    // A change carries no
    // comment; an entry written by an older version may still hold one, which is not read. Opens the journal alone when the store is not there
    // yet. False when the journal cannot be read.
    bool thresholdHistory(const QString &journalPath, const QUuid &subject, QList<ui::ThresholdHistoryEntry> *out);

private:
    StoreQueries(const StoreQueries &);
    StoreQueries &operator=(const StoreQueries &);
    Store *m_store;
    Journal *m_journal;
};

// The exact conversion of a stored value to mg/dL; false for an unknown unit.
bool toMgDl(double value, const QString &unit, double *mgdl);

} // namespace real
} // namespace lm

#endif
