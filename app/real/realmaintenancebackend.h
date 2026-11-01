// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Michele Brami
#ifndef LM_REAL_REALMAINTENANCEBACKEND_H
#define LM_REAL_REALMAINTENANCEBACKEND_H

// The real MaintenanceBackend (app/ui/maintenancebackend.h and 9).
//
// The work goes to the service: StartMaintenance, CancelMaintenance, DiagnosticReport and ReportHandled
// over the asynchronous client (ServiceLink); the progress and the end come back as the service's
// signals. Whether a run is going on is read from the Status() answers the window already asks for (on
// the service's signals and when the window comes to the front) and from the progress signals: no call
// of its own, nothing polls (REQ-ARC-03).
//
// The facts are read HERE, in the interface's own process, by `StorageReader` in a thread of its own,
// on read-only connections that it opens for each reading and closes afterwards: the file sizes, the
// free space (QStorageInfo), the last weekly check from the journal (core/store/storagecheck.h
// `lastStorageCheck`) and the dry-run estimate of what compaction would free for the chosen window
// (core/store/compactor.h `estimateCompaction`, which changes nothing). The rebuild need of REQ-STO-16 is
// the store's size now minus that estimate (the core's estimate does not carry the need yet; the
// service's compaction checks the space itself before it deletes anything).

#include "servicelink.h"
#include "ui/maintenancebackend.h"

#include <QMetaType>
#include <QThread>

Q_DECLARE_METATYPE(lm::ui::StorageFacts)

namespace lm {
namespace real {

// Reads the storage facts read-only; lives in the backend's own thread.
class StorageReader : public QObject
{
    Q_OBJECT
public:
    StorageReader(const QString &storePath, const QString &journalPath) : m_storePath(storePath), m_journalPath(journalPath) {}
    // The work itself, synchronous; public so a host test can call it without a thread.
    static ui::StorageFacts readFacts(const QString &storePath, const QString &journalPath, int windowMonths, qint64 nowMs);
public slots:
    void read(int windowMonths);
signals:
    void factsRead(const lm::ui::StorageFacts &facts);
private:
    QString m_storePath, m_journalPath;
};

class RealMaintenanceBackend : public ui::MaintenanceBackend
{
    Q_OBJECT
public:
    RealMaintenanceBackend(ServiceLink *link, const QString &storePath, const QString &journalPath, QObject *parent = 0);
    ~RealMaintenanceBackend();

    void requestStorageFacts(int windowMonths);
    void startMaintenance(const QString &kind, const QString &parametersJson);
    void cancelMaintenance();
    bool maintenanceRunning() const { return m_running; }
    QString maintenanceKind() const { return m_kind; }
    void requestDiagnosticReport(const QString &connection);
    void reportHandled(const QString &connection);

private:
    void setRunning(bool running, const QString &kind);

    ServiceLink *m_link;
    QThread m_thread;
    StorageReader *m_reader;
    bool m_running;
    QString m_kind;
};

} // namespace real
} // namespace lm

#endif
