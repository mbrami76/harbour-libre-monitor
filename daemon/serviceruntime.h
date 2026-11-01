// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Michele Brami
#ifndef LM_SERVICERUNTIME_H
#define LM_SERVICERUNTIME_H

// The service process, assembled (REQ-ARC-07 to 10): the lifecycle
// the pipeline wiring (ServiceCore), the D-Bus object, the maintenance
// runner and the Unix signals, around a Qt event loop. Pattern: composition root. Nothing
// here decides anything about polling or trust; it connects the parts and turns external
// events (a wake-up, a D-Bus call, a signal) into lifecycle calls.
//
// The Qt event loop runs on the main thread, which is the only thread that touches the
// lifecycle, the scheduler, the guard and the main store connection. The one other thread is
// the maintenance worker (core/service/maintenancerunner.h). Time comes from the platform's
// wake-up source, never from a timer of this class; the one QTimer here drives only the short
// stop sequence, while the phone is awake for a shutdown (lifecycle.h, stepStop).
//
// Assumptions: the D-Bus name is owned only by the first instance (REQ-ARC-09); a second
// instance finds out in begin() and exits before it writes anything.

#include "daemon/dbusservice.h"
#include "daemon/serviceidentity.h"
#include "daemon/servicecore.h"
#include "daemon/serviceplatform.h"
#include "service/donotdisturb.h"
#include "service/maintenancerunner.h"
#include "service/reportproposal.h"
#include "service/servicelifecycle.h"
#include "service/versionwatch.h"
#include "store/compactor.h"

#include <QHash>
#include <QObject>
#include <QTimer>

#include <memory>

namespace lm {

struct RuntimeOptions
{
    RuntimeOptions() : exitOnStop(true) {}

    ServiceIdentity identity;
    CoreOptions core;
    LifecycleSettings lifecycle;
    // The loop guard of the version stamp check.
    VersionWatchSettings versionWatch;
    // This binary's LM_APP_VERSION. The version stamp is compared with it, and Status() reports it
    // as `service.app_version`.
    QString appVersion;
    // Free bytes at a directory for the space check of compaction and purge (REQ-STO-16); empty = the
    // file system. Tests inject a short or an unknown answer. (The name is older than the purge.)
    FreeSpaceFn compactionFreeSpace;
    // How the purge job opens the journal for writing; empty = Journal::open. Tests open it
    // read-only so that the journal part fails after the store part and the repeat can be shown.
    std::function<bool(Journal &, const QString &)> purgeJournalOpener;
    // Tests keep the event loop; the process quits when the stop sequence is done.
    bool exitOnStop;
};

// Exit codes of the service process.
enum ServiceExit {
    ServiceExitOk = 0,          // an orderly stop, or a second instance that stepped aside
    ServiceExitOpenFailed = 70, // the journal, the store or the configuration did not open
    ServiceExitBus = 71         // no session bus
};

class ServiceRuntime : public ServiceObject, public ServiceApi, public ServiceEventSink
{
    Q_OBJECT
public:
    // The platform and its objects are not owned and must outlive the runtime.
    ServiceRuntime(const RuntimeOptions &options, const ServicePlatform &platform,
                   QObject *parent = 0);
    ~ServiceRuntime();

    // Takes the name, opens the pipeline, writes the start record and arms the first wake-up
    // Returns -1 when the service is running, else the exit code of the
    // process: ServiceExitOk for a second instance (it wrote nothing), ServiceExitOpenFailed
    // for.
    int begin();

    // The stop sequence is driven from here after a signal, a D-Bus Stop or a lost name.
    void stop(const QString &reason);
    // The same, with extra keys for the stop record (the versions of an `upgraded` stop).
    void stop(const QString &reason, const QJsonObject &stopExtra);
    ServiceLifecycle *lifecycle() { return &m_lifecycle; }
    // The version stamp check; null when the platform has no stamp reader.
    VersionWatch *versionWatch() { return m_version.get(); }
    ServiceCore *core() { return &m_core; }
    // The fan-out the scheduler reports to. Public so that a test can send an event as the
    // scheduler would; nothing else outside the runtime needs it.
    ServiceEventFanOut *events() { return &m_events; }
    MaintenanceRunner *maintenance() { return &m_maintenance; }
    bool stopped() const { return m_lifecycle.state() == ServiceState::NotRunning; }

    // ServiceApi
    QString apiStart();
    QString apiStatusJson();
    void apiRetry(const QString &connection);
    bool apiReenable(const QString &connection);
    bool apiReenableVersion(const QString &adapter, const QString &version);
    bool apiReportHandled(const QString &connection);
    QString apiDiagnosticReport(const QString &connection);
    void apiCredentialsChanged(const QString &connection);
    void apiIntervalsChanged(const QString &connection);
    QString apiConfigChanged();
    QString apiAlerts();
    QString apiAcknowledgeAlert(const QString &subject, const QString &slot);
    QString apiSnoozeAlert(const QString &subject, const QString &slot, int minutes);
#ifdef LIBREMON_DEVSOURCE
    QString apiDevInject(const QString &json);
#endif
    QString apiSetSilent(const QString &subject, int minutes);
    QString apiEndSilent(const QString &subject);
    QString apiSetThresholds(const QString &subject, int l, int h, int typedL, int typedH);
    void apiDiscover(const QString &connection, const std::function<void(const QString &)> &done);
    QString apiStartMaintenance(const QString &kind, const QString &parametersJson);
    void apiCancelMaintenance();
    void apiStop();

    // ServiceEventSink: forwards to the D-Bus signals
    void gapDetected(const UtcTime &from, const UtcTime &to);
    void subjectStateChanged(const SubjectStatus &status);
    void stalenessChanged(const SubjectStatus &status);
    void connectionWarning(const QString &connection, const QString &kind, const QString &detail);
    void readingQueueHighWater(int queuedRecords);
    void readingQueueFull(int droppedRecords);
    void diagnosticReportProposed(const QString &connection);
    void dataChanged(const QUuid &subject);
    void alertChanged(const QUuid &subject, const QString &slot, const QString &state);
    // Raised by the runtime itself: a mode that ran out is not an event of the engine.
    void checkSilentChanges();

public slots:
    // Unix signal numbers, from UnixSignalForwarder.
    void onUnixSignal(int signalNumber);

private slots:
    void stopRequested() { stop(StopReason::requested()); }
    void onWake();
    void onStopTick();
    void onMaintenanceProgress(const QString &kind, const QString &phase, int step, int steps,
                               qlonglong done, qlonglong total, double fraction);
    void onMaintenanceFinished(const QString &kind, bool ok, const QString &errorClass,
                               const QString &message, const QString &cancelReason);

private:
    void exitWith(int code);
    // After a heartbeat: compares the version stamp with this binary's version and stops for an
    // upgrade or a removal. True when a stop began.
    bool checkVersionStamp();
    // The coordinator when the service is running (alerts are only answered then), else null.
    AlertCoordinator *runningAlerts();

    RuntimeOptions m_options;
    ServicePlatform m_platform;
    ServiceEventFanOut m_events;
    // Derives `report_proposed` of Status() from the scheduler's state events (REQ-GRD-15).
    ReportProposalTracker m_reports;
    ServiceCore m_core;
    ServiceLifecycle m_lifecycle;
    MaintenanceRunner m_maintenance;
    MaintenanceHighWaterGuard m_highWaterGuard;
    std::unique_ptr<ServiceAdaptor> m_adaptor;
    // Watches Do not disturb once the service runs; null where the platform has no source.
    std::unique_ptr<DoNotDisturbWatcher> m_dnd;
    // The version stamp check; exists only when the platform gives a stamp reader.
    std::unique_ptr<VersionWatch> m_version;
    // Set when the stop is for an upgrade: once the stop record is written and the name released,
    // the platform's activator starts the new binary.
    bool m_activateAfterStop;
    QTimer m_stopTimer;
    bool m_dbusRegistered;
    QHash<QUuid, bool> m_silentSeen;        // the silent state last announced per Subject
};

} // namespace lm

#endif
