// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Michele Brami
#include "storeworker.h"

namespace lm {
namespace real {

class StoreWorkerImpl : public QObject
{
    Q_OBJECT
public:
    StoreWorkerImpl(const QString &storePath, const QString &journalPath)
        : m_storePath(storePath), m_journalPath(journalPath) {}

public slots:
    void load(const QString &id, qint64 fromMs)
    {
        if (!ensureOpen()) { emit unavailable(); return; }
        SubjectData d = m_queries.load(QUuid(id), fromMs);
        m_cache.insert(id, d);
        emit subjectLoaded(id, d);
    }
    void refresh(const QString &id, qint64 fromMs)
    {
        if (!m_cache.contains(id)) { load(id, fromMs); return; }
        if (!ensureOpen()) { emit unavailable(); return; }
        SubjectData &d = m_cache[id];
        m_queries.refresh(QUuid(id), &d);
        emit subjectLoaded(id, d);
    }
    void loadGaps()
    {
        if (!ensureOpen()) { emit unavailable(); return; }
        emit gapsLoaded(m_queries.serviceGaps());
    }
    void loadThresholdHistory(const QString &id)
    {
        ensureOpen();               // the journal can be read even when the store is not there yet
        ThresholdHistory entries;
        const bool ok = m_queries.thresholdHistory(m_journalPath, QUuid(id), &entries);
        emit thresholdHistoryLoaded(id, ok, entries);
    }

signals:
    void subjectLoaded(const QString &id, const lm::real::SubjectData &data);
    void gapsLoaded(const lm::real::GapList &gaps);
    void thresholdHistoryLoaded(const QString &id, bool ok, const lm::real::ThresholdHistory &entries);
    void unavailable();

private:
    bool ensureOpen()
    {
        if (m_queries.isOpen())
            return true;
        return m_queries.open(m_storePath, m_journalPath);
    }

    QString m_storePath, m_journalPath;
    StoreQueries m_queries;                     // created in this object's thread, used only there
    QHash<QString, SubjectData> m_cache;
};

StoreWorker::StoreWorker(const QString &storePath, const QString &journalPath, QObject *parent)
    : QObject(parent), m_impl(new StoreWorkerImpl(storePath, journalPath))
{
    qRegisterMetaType<lm::real::SubjectData>();
    qRegisterMetaType<lm::real::GapList>();
    qRegisterMetaType<lm::real::ThresholdHistory>();
    m_impl->moveToThread(&m_thread);
    connect(m_impl, &StoreWorkerImpl::subjectLoaded, this, &StoreWorker::subjectLoaded);
    connect(m_impl, &StoreWorkerImpl::gapsLoaded, this, &StoreWorker::gapsLoaded);
    connect(m_impl, &StoreWorkerImpl::thresholdHistoryLoaded, this, &StoreWorker::thresholdHistoryLoaded);
    connect(m_impl, &StoreWorkerImpl::unavailable, this, &StoreWorker::unavailable);
    connect(&m_thread, &QThread::finished, m_impl, &QObject::deleteLater);
    m_thread.start();
}

StoreWorker::~StoreWorker()
{
    m_thread.quit();
    m_thread.wait(8000);
}

void StoreWorker::load(const QString &id, qint64 fromMs)
{
    QMetaObject::invokeMethod(m_impl, "load", Qt::QueuedConnection, Q_ARG(QString, id), Q_ARG(qint64, fromMs));
}
void StoreWorker::refresh(const QString &id, qint64 fromMs)
{
    QMetaObject::invokeMethod(m_impl, "refresh", Qt::QueuedConnection, Q_ARG(QString, id), Q_ARG(qint64, fromMs));
}
void StoreWorker::loadGaps() { QMetaObject::invokeMethod(m_impl, "loadGaps", Qt::QueuedConnection); }
void StoreWorker::loadThresholdHistory(const QString &id)
{
    QMetaObject::invokeMethod(m_impl, "loadThresholdHistory", Qt::QueuedConnection, Q_ARG(QString, id));
}

} // namespace real
} // namespace lm

#include "storeworker.moc"
