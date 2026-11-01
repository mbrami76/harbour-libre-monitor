// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Michele Brami
#include "realmaintenancebackend.h"

#include "statusparser.h"
#include "store/compactor.h"
#include "store/journal.h"
#include "store/storagecheck.h"
#include "store/store.h"
#include "time/utctime.h"

#include <QDateTime>
#include <QFileInfo>
#include <QStorageInfo>

namespace lm {
namespace real {

namespace {
qint64 sizeWithLog(const QString &path)
{
    const QFileInfo f(path);
    if (!f.exists())
        return -1;
    const QFileInfo wal(path + QStringLiteral("-wal"));
    return f.size() + (wal.exists() ? wal.size() : 0);
}
}

ui::StorageFacts StorageReader::readFacts(const QString &storePath, const QString &journalPath, int windowMonths,
                                          qint64 nowMs)
{
    ui::StorageFacts f;
    f.windowMonths = windowMonths;
    f.storeBytes = sizeWithLog(storePath);
    f.journalBytes = sizeWithLog(journalPath);
    const QStorageInfo where(QFileInfo(storePath).absolutePath());
    f.freeBytes = where.isValid() ? where.bytesAvailable() : -1;
    {
        Journal journal;
        if (journal.openReadOnly(journalPath)) {
            StorageCheckResult r;
            if (lastStorageCheck(journal, &r)) {
                f.checkKnown = true;
                f.checkTimeMs = r.time.isValid() ? r.time.msecsSinceEpoch() : -1;
                f.checkLow = r.low;
            }
        }
    }
    Store store;
    if (!store.openReadOnly(storePath)) {
        f.error = QStringLiteral("store-unreadable");
        return f;
    }
    f.ok = true;
    const FullResolutionWindow window = windowMonths <= 0 ? FullResolutionWindow::ofDays(kMinimumFullResolutionDays)
                                                          : FullResolutionWindow::ofMonths(windowMonths);
    CompactionEstimate e;
    QString error;
    if (estimateCompaction(store, window, UtcTime::fromMSecsSinceEpoch(nowMs), &e, &error)) {
        f.estimateKnown = true;
        f.estimateBytes = e.bytes;
        f.estimateRecords = e.records + e.quarantine + e.ingest;
        f.cutoffMs = e.cutoff.isValid() ? e.cutoff.msecsSinceEpoch() : -1;
        // REQ-STO-16: the core's rebuild need (with its margin) and its comparison with
        // the free space, the same calculation the service's compaction checks before deleting anything.
        f.rebuildNeedBytes = e.rebuildNeedBytes;
        f.freeBytes = e.freeBytes;
        f.space = e.space == CompactionEstimate::SpaceEnough ? QStringLiteral("enough")
                : e.space == CompactionEstimate::SpaceShort ? QStringLiteral("short") : QStringLiteral("unknown");
        f.missingBytes = e.missingBytes();
    }
    return f;
}

void StorageReader::read(int windowMonths)
{
    emit factsRead(readFacts(m_storePath, m_journalPath, windowMonths, QDateTime::currentMSecsSinceEpoch()));
}

RealMaintenanceBackend::RealMaintenanceBackend(ServiceLink *link, const QString &storePath, const QString &journalPath,
                                               QObject *parent)
    : ui::MaintenanceBackend(parent), m_link(link), m_reader(new StorageReader(storePath, journalPath)), m_running(false)
{
    qRegisterMetaType<lm::ui::StorageFacts>();
    m_reader->moveToThread(&m_thread);
    connect(&m_thread, &QThread::finished, m_reader, &QObject::deleteLater);
    connect(m_reader, &StorageReader::factsRead, this, &ui::MaintenanceBackend::storageFactsLoaded);
    m_thread.start();

    connect(m_link, &ServiceLink::startMaintenanceAnswered, this, [this](bool ok, const QString &kind, const QString &result) {
        if (ok && result.isEmpty())
            setRunning(true, kind);
        emit maintenanceAnswered(ok, kind, result);
    });
    connect(m_link, &ServiceLink::maintenanceProgress, this,
            [this](const QString &kind, const QString &phase, int step, int steps, qlonglong done, qlonglong total, double fraction) {
        setRunning(true, kind);
        emit maintenanceProgress(kind, phase, step, steps, done, total, fraction);
    });
    connect(m_link, &ServiceLink::maintenanceFinished, this,
            [this](const QString &kind, bool ok, const QString &errorClass, const QString &message,
                   const QString &cancelReason, const QString &details) {
        setRunning(false, QString());
        emit maintenanceFinished(kind, ok, errorClass, message, cancelReason, details);
    });
    connect(m_link, &ServiceLink::warning, this, [this](const QString &kind, const QString &, const QString &detail) {
        if (kind == QLatin1String("reading-queue-high-water") || kind == QLatin1String("reading-queue-full"))
            emit maintenanceWarning(kind, detail);
    });
    // The Status() answers the window asks for anyway carry `maintenance.running`: a run started before
    // the window opened is seen without a call of our own.
    connect(m_link, &ServiceLink::statusAnswered, this, [this](bool ok, const QByteArray &json) {
        if (!ok)
            return;
        const ServiceReport r = parseServiceStatus(json);
        if (r.ok)
            setRunning(r.maintenanceRunning, r.maintenanceKind);
    });
    connect(m_link, &ServiceLink::reachableChanged, this, [this](bool reachable) {
        if (!reachable)
            setRunning(false, QString());           // a stopped service runs nothing
    });
    connect(m_link, &ServiceLink::diagnosticReportAnswered, this, &ui::MaintenanceBackend::diagnosticReportAnswered);
}

RealMaintenanceBackend::~RealMaintenanceBackend()
{
    m_thread.quit();
    m_thread.wait(8000);
}

void RealMaintenanceBackend::setRunning(bool running, const QString &kind)
{
    if (running == m_running && (!running || kind == m_kind))
        return;
    m_running = running;
    m_kind = running ? kind : QString();
    emit runningChanged();
}

void RealMaintenanceBackend::requestStorageFacts(int windowMonths)
{
    QMetaObject::invokeMethod(m_reader, "read", Qt::QueuedConnection, Q_ARG(int, windowMonths));
}

void RealMaintenanceBackend::startMaintenance(const QString &kind, const QString &parametersJson)
{
    m_link->startMaintenance(kind, parametersJson);
}

void RealMaintenanceBackend::cancelMaintenance() { m_link->cancelMaintenance(); }

void RealMaintenanceBackend::requestDiagnosticReport(const QString &connection) { m_link->diagnosticReport(connection); }

void RealMaintenanceBackend::reportHandled(const QString &connection)
{
    m_link->reportHandled(connection);
    m_link->refreshStatus();                // report_proposed is cleared by the service
}

} // namespace real
} // namespace lm
