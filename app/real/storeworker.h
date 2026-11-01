// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Michele Brami
#ifndef LM_REAL_STOREWORKER_H
#define LM_REAL_STOREWORKER_H

// Reads the store off the window's thread. A Qt SQL connection belongs
// to the thread that made it, and a read can wait while the store is busy (a compaction rebuilds the
// file), so the reads run in a thread of their own and the answers come back as signals carrying
// plain values. Pattern: the same facade-plus-worker shape as ServiceLink.
//
// The worker keeps a cache per Subject (the series loaded so far) and after each refresh sends the
// whole picture; copying about 40,000 points once a minute is cheap next to the queries it saves.

#include "storequeries.h"

#include <QHash>
#include <QMetaType>
#include <QObject>
#include <QThread>

namespace lm {
namespace real {

typedef QVector<QPair<qint64, qint64> > GapList;
typedef QList<ui::ThresholdHistoryEntry> ThresholdHistory;
} // namespace real
} // namespace lm

Q_DECLARE_METATYPE(lm::real::SubjectData)
Q_DECLARE_METATYPE(lm::real::GapList)
Q_DECLARE_METATYPE(lm::real::ThresholdHistory)

namespace lm {
namespace real {

class StoreWorkerImpl;

class StoreWorker : public QObject
{
    Q_OBJECT
public:
    StoreWorker(const QString &storePath, const QString &journalPath, QObject *parent = 0);
    ~StoreWorker();

    // The window the plot may ask for: from `fromMs` to now. Loads the Subject (first time) or
    // appends what is new (later); the answer is subjectLoaded().
    void load(const QString &subjectId, qint64 fromMs);
    void refresh(const QString &subjectId, qint64 fromMs);
    // The gaps of the service journal: gapsLoaded().
    void loadGaps();
    // The threshold edits of one patient from the journal: thresholdHistoryLoaded().
    void loadThresholdHistory(const QString &subjectId);

signals:
    void subjectLoaded(const QString &subjectId, const lm::real::SubjectData &data);
    void gapsLoaded(const lm::real::GapList &gaps);
    void thresholdHistoryLoaded(const QString &subjectId, bool ok, const lm::real::ThresholdHistory &entries);
    // The store could not be opened (not created yet): nothing is invented; the caller tries again
    // when the service says there is new data.
    void unavailable();

private:
    QThread m_thread;
    StoreWorkerImpl *m_impl;
};

} // namespace real
} // namespace lm

#endif
