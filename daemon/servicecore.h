// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Michele Brami
#ifndef LM_SERVICECORE_H
#define LM_SERVICECORE_H

// The wiring of the monitoring pipeline inside the service: the store and the journal of
// The store, the guard and the scheduler, the session driver, and the
// platform ports. It is built in open() and not in the constructor because the lifecycle must
// be able to say "the journal or the store did not open" before anything exists that
// depends on them.
//
// Pattern: facade. To the lifecycle this one object is the storage (open, close, weekly
// check), the journal and the scheduler; behind it is the object graph.
// It owns what it creates in a fixed order and destroys it in the reverse order.
//
// The Qt SQL rule it respects: a database connection belongs to the thread that opened it.
// Everything here runs on the main thread; the maintenance worker (maintenancerunner.h) opens
// connections of its own.
//
// Assumptions: the data directory is private to the application (Sailjail gives it); the
// configuration file is written by the UI and may not exist yet (then there are no
// connections and the service idles); a connection's adapter is a built-in one.

#include "alerts/alertcoordinator.h"
#include "daemon/devfeed.h"
#include "daemon/discovery.h"
#include "daemon/maintenancejobs.h"
#include "daemon/serviceconfig.h"
#include "daemon/serviceplatform.h"
#include "service/diagnosticcollector.h"
#include "service/journallog.h"
#include "service/schedulerhost.h"
#include "service/noticewatcher.h"
#include "service/serviceevents.h"
#include "service/serviceports.h"
#include "service/silentcontrol.h"
#include "store/journal.h"
#include "store/store.h"

#include <QObject>
#include <QString>

#include <memory>

namespace lm {

class Guard;
class IngestLog;
class JournalStateLog;
class StateKeeper;
class StoreIngestLogSink;
class StoreReadingStore;

// How to start the adapter process of a connection.
struct AdapterLaunch
{
    QString program;
    QStringList arguments;
};

struct CoreOptions
{
    CoreOptions() {}

    QString dataDirectory;      // store.db, journal.db and settings live here
    QString configPath;         // the configuration file; may be absent
    // The adapter command for an adapter identifier. In the packaged application:
    // applicationFilePath() with {"--adapter", id}; on the PC: the host executable.
    std::function<AdapterLaunch(const QString &adapterId)> adapterLaunch;
    // How the other end of an adapter link is made. Empty in production: the link starts the
    // process named by `adapterLaunch`. A test sets it to answer from memory, so the whole
    // pipeline (driver, guard, scheduler, discovery) runs against a scripted adapter without a
    // process (the same seam the driver has, session/adapterlink.h).
    ChannelFactory channelFactory;
    // The limits of `Discover` (daemon/discovery.h); a test shortens the watchdog.
    DiscoverySettings discovery;
    // The constants of the alert engine. The default is the provisional set; the
    // host executable may scale the durations for the integration run (scaledAlertSettings).
    AlertSettings alerts;
    // Where the compiled translations of the application are (".../translations") and their base
    // name; the notification texts are read from them (REQ-ALR-10). Empty directory = English ids
    // are shown as the translation system gives them (a test or a PC run without files).
    QString translationsDir;
    QString translationBase = QStringLiteral("harbour-libre-monitor");
    // The folder with the notification icon PNGs (".../icons/"); empty = none is named.
    QString iconDirectory;
    // Silent mode of the Subjects. Null in the packaged service: the coordinator answers it
    // (core/service/silentcontrol.h). A test may supply a fake; not owned.
    SilentControl *silent = nullptr;
};

class ServiceCore : public ServiceStorage, public ServiceJournal, public SchedulerHost,
                    public IngestLogSink
{
public:
    // The platform and the events are not owned and must outlive the core.
    ServiceCore(const CoreOptions &options, const ServicePlatform &platform,
                ServiceEventFanOut *events);
    ~ServiceCore();

    // ServiceStorage
    bool open(QString *cause);
    void close();
    bool storageCheckDue(const UtcTime &now);
    void runStorageCheck(const UtcTime &now);

    // ServiceJournal
    bool lastLifecycleEntry(JournalEntry *entry);
    bool hasStartRecord();
    bool append(const JournalEntry &entry);

    // SchedulerHost
    void start(const UtcTime &serviceDownSince);
    void tick();
    UtcTime nextWakeAt() const;
    void shutdown();
    bool linksEnded() const;
    int queuedReadings() const;
    int flushQueuedReadings();
    void secretsAvailable(const QString &connectionId);

    // IngestLogSink: the lifecycle writes `readings-lost` through the same sink as the guard.
    bool append(const IngestLogEntry &entry);

    // For the runtime: the commands of the D-Bus interface and the status.
    Scheduler *scheduler() { return m_scheduler.get(); }
    Guard *guard() { return m_guard.get(); }
    // What the diagnostic report of a connection is made from; null while closed.
    DiagnosticCollector *diagnostics() { return m_diagnostics.get(); }
    QString dataDirectory() const { return m_options.dataDirectory; }
    QString storePath() const;
    QString journalPath() const;
    // Re-reads the configuration after the UI changed it and applies new intervals.
    void configurationChanged(const QString &connectionId);
    // The whole file changed (D-Bus ConfigChanged). Applies new intervals of the
    // connections the scheduler already has. Returns "" when nothing more is needed,
    // "restart-needed" when connections or Subjects were added, removed or changed (the
    // scheduler cannot add or drop them while running), or "config-invalid"
    // when the file does not parse (the service keeps what it had).
    QString reloadConfiguration();
    ConnectionConfig connectionConfig(const QString &id) const;
    QStringList connectionIds() const;
    // The Subjects the service follows, in configuration order (for Alerts()).
    QList<QUuid> subjectIds() const { return m_config.subjectOrder; }
    // What the configuration in force says about the Subject of a purge (REQ-STO-17):
    // whether it is known, its label (for the typed-label gate), its source reference and the
    // connections that serve it alone. Read from the loaded configuration, which ConfigChanged keeps
    // current for labels; never from the caller.
    PurgeTarget purgeTarget(const QUuid &subject) const;
    // The three per-Subject alert times IN FORCE, in whole seconds, after the engine's clamping (for
    // Alerts(): the thresholds page shows what the engine uses, not what the file asked for).
    struct AlertKeyValues { qint64 persistenceLowS, persistenceHighS, missingLimitS; };
    AlertKeyValues alertKeysInForce(const QUuid &subject) const;
    // The engine's allowed ranges and defaults (core/alerts/alertsettings.h), for Alerts().
    const AlertSettings &alertRanges() const { return m_options.alerts; }

    // One-time discovery of the Subjects of an account; null until open().
    DiscoveryService *discovery() { return m_discovery.get(); }
    // The alert coordinator; null until open().
    AlertCoordinator *alerts() { return m_alerts.get(); }
    SilentControl *silentControl() { return m_options.silent ? m_options.silent : m_silent.get(); }
    // The notices that come from the service's own events (approval, guard, report); null until open().
    NoticeWatcher *notices() { return m_notices.get(); }
    // Do not disturb is on (set by the watcher); the resident notice says so.
    void setDoNotDisturb(bool on) { m_doNotDisturb = on; }
    bool doNotDisturb() const { return m_doNotDisturb; }
    // The facts of the moment for the resident notice (see core/service/residentnotice.h).
    ResidentInput residentFacts() const;
#ifdef LIBREMON_DEVSOURCE
    // The developer-only feed of synthetic effects; null until open().
    DevFeed *devFeed() { return m_devFeed.get(); }
#endif
    QString connectionLabel(const QString &connection) const;
    // The sensor most recently seen for a Subject, as the completion's `sensor` object (`id`,
    // `activated_at`, `warmup_minutes`; each only when known), or false when none is stored.
    // Read from the store on the main thread, for Status().
    bool latestSensor(const QUuid &subject, QJsonObject *sensor);
    // The version of a built-in adapter as the service will run it (the registry's);
    // empty for an unknown adapter.
    static QString adapterVersion(const QString &adapterId);

private:
    // Hands the Subjects' short labels from the configuration to the notification texts.
    void publishLabels();
    // The language and the privacy setting of the notification texts (configuration file keys).
    void applyNotificationSettings();
    // The settings the alert engine gets for one Subject of a connection (file keys or defaults).
    AlertSubjectSettings alertSettingsFor(const ConnectionConfig &c, const QUuid &subject) const;

    CoreOptions m_options;
    ServicePlatform m_platform;
    ServiceEventFanOut *m_events;

    // In construction order; destroyed in reverse.
    Store m_store;
    Journal m_journal;
    std::unique_ptr<JournalLifecycleLog> m_lifecycleLog;
    std::unique_ptr<JournalStateLog> m_stateLog;
    std::unique_ptr<StateKeeper> m_keeper;
    std::unique_ptr<StoreIngestLogSink> m_ingestSink;
    std::unique_ptr<IngestLog> m_ingest;
    std::unique_ptr<Guard> m_guard;
    // Stands between every adapter link and the guard (a decorator: the guard's answers are
    // unchanged) and keeps the last guard kill per connection for the diagnostic report.
    std::unique_ptr<DiagnosticCollector> m_diagnostics;
    std::unique_ptr<StoreReadingStore> m_readings;
    std::unique_ptr<NotifyingReadingStore> m_notifying;
    std::unique_ptr<LinkFactory> m_baseLinks;
    std::unique_ptr<TrackingLinkFactory> m_links;
    std::unique_ptr<Scheduler> m_scheduler;
    std::unique_ptr<SchedulerHostAdapter> m_host;
    std::unique_ptr<DiscoveryService> m_discovery;
    // The alerts: listens to the same events as the UI, reads the newest `current`
    // reading from the store, writes its journal lines to the same journal. Destroyed before the
    // store and the journal it uses.
    std::unique_ptr<StoreAlertReadings> m_alertReadings;
    // Between the coordinator and the platform's notifier: tells the event sinks (the D-Bus signal
    // AlertChanged) what changed. Destroyed after the coordinator that calls it.
    std::unique_ptr<NoticeWatcher> m_notices;
    std::unique_ptr<AlertValueSource> m_valueSource;
    std::unique_ptr<EffectFactsReader> m_effectFacts;
    std::unique_ptr<SilentControl> m_silent;
    NotificationTranslator m_translator;
    std::unique_ptr<AlertChangeRelay> m_alertRelay;
    std::unique_ptr<AlertCoordinator> m_alerts;
    ServiceConfig m_config;
    bool m_open;
    bool m_doNotDisturb = false;
    std::unique_ptr<ResidentFactsSource> m_residentFacts;
#ifdef LIBREMON_DEVSOURCE
    std::unique_ptr<DevFeed> m_devFeed;
#endif
};

} // namespace lm

#endif
