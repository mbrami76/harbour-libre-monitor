// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Michele Brami
#ifndef LM_UI_MAINTENANCEBACKEND_H
#define LM_UI_MAINTENANCEBACKEND_H

// What the storage page and the diagnostic report dialog get their facts from and send their
// requests to.
//
// Pattern: a port, header-only like accountbackend.h and alertsbackend.h, with two implementations: the
// real one (app/real/realmaintenancebackend.h: the service over D-Bus for the work, the store and the
// journal READ-ONLY in a thread of their own for the facts) and a scripted one for development builds and
// the host tests (fakemaintenancebackend.h). Requests return at once; every answer is a signal.
//
// Where the work happens (REQ-STO-15): compaction, the only maintenance kind (no export,
// no import), runs IN THE SERVICE, on its worker thread, started by StartMaintenance and stopped by
// CancelMaintenance; the service keeps polling meanwhile. The interface only reads: the size of the store,
// the last weekly check and the ESTIMATE of what compaction would free, with the rebuild need and the free
// space compared with it, come from the core's own dry run (core/store/compactor.h `estimateCompaction`,
// which changes nothing) run in the interface's process on a read-only connection, as
// 8.2 says: the same calculation the service's compaction checks before it deletes anything (REQ-STO-16).
// No call of the service is made for them.

#include <QObject>
#include <QString>
#include <QtGlobal>

namespace lm {
namespace ui {

// The facts of the storage page, read-only. Sizes in bytes; -1 = not known.
struct StorageFacts
{
    StorageFacts() : ok(false), windowMonths(3), storeBytes(-1), journalBytes(-1), freeBytes(-1),
                     checkKnown(false), checkTimeMs(-1), checkLow(false), estimateKnown(false),
                     estimateBytes(0), estimateRecords(0), cutoffMs(-1), rebuildNeedBytes(-1), missingBytes(0) {}

    bool ok;                    // false: the store could not be read (error says why)
    QString error;              // "store-unreadable"
    int windowMonths;           // the window these figures are for; 0 = the shortest, 14 days (REQ-STO-13)
    qint64 storeBytes;          // the database file and its write-ahead log, now
    qint64 journalBytes;        // the service journal, now
    qint64 freeBytes;           // free space where the store is, now
    // The service's last weekly check (REQ-STO-01, REQ-STO-02), from the journal.
    bool checkKnown;
    qint64 checkTimeMs;
    bool checkLow;              // free space below three times the size at that check: point to compaction
    // REQ-STO-13: the dry-run estimate for the window (rows that would go times their average size).
    bool estimateKnown;
    qint64 estimateBytes;
    qint64 estimateRecords;     // readings, quarantine and ingest entries together
    qint64 cutoffMs;            // older than this would be thinned
    // REQ-STO-16: the free space the rebuild after the deletion needs (what remains plus the core's margin,
    // `CompactionEstimate::rebuildNeedBytes`), and the core's comparison of the free space with it:
    // "enough", "short" (then `missingBytes` is what is missing) or "unknown" (the free space could not be
    // read; the service then refuses, with no override). Taken from the core, never computed here.
    qint64 rebuildNeedBytes;
    QString space;
    qint64 missingBytes;
};

class MaintenanceBackend : public QObject
{
    Q_OBJECT
public:
    explicit MaintenanceBackend(QObject *parent = 0) : QObject(parent) {}

    // The facts for a window (0 = 14 days, else whole months): storageFactsLoaded().
    virtual void requestStorageFacts(int windowMonths) = 0;
    // StartMaintenance(kind, parametersJson) of the service: maintenanceAnswered(ok, kind, result), then
    // the progress and the end as maintenanceProgress / maintenanceFinished.
    virtual void startMaintenance(const QString &kind, const QString &parametersJson) = 0;
    virtual void cancelMaintenance() = 0;
    // A maintenance run is going on (Status() `maintenance.running`, or a progress signal), and which kind.
    virtual bool maintenanceRunning() const = 0;
    virtual QString maintenanceKind() const = 0;
    // DiagnosticReport(connection): diagnosticReportAnswered(ok, connection, text). Does not end the
    // proposal; reportHandled() does.
    virtual void requestDiagnosticReport(const QString &connection) = 0;
    virtual void reportHandled(const QString &connection) = 0;

signals:
    void storageFactsLoaded(const lm::ui::StorageFacts &facts);
    void maintenanceAnswered(bool ok, const QString &kind, const QString &result);
    void maintenanceProgress(const QString &kind, const QString &phase, int step, int steps,
                             qlonglong done, qlonglong total, double fraction);
    void maintenanceFinished(const QString &kind, bool ok, const QString &errorClass, const QString &message,
                             const QString &cancelReason, const QString &detailsJson);
    // The service's warnings that concern maintenance: "reading-queue-high-water" (the run was stopped to protect the readings) and "reading-queue-full" with "count=N" (readings were lost).
    void maintenanceWarning(const QString &kind, const QString &detail);
    void runningChanged();
    void diagnosticReportAnswered(bool ok, const QString &connection, const QString &text);
};

} // namespace ui
} // namespace lm

#endif
