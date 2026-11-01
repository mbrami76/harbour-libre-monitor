// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Michele Brami
#include "serviceruntime.h"

#include "daemon/commonports.h"
#include "daemon/maintenancejobs.h"
#include "guard/guard.h"

#include <QCoreApplication>
#include <QDBusConnection>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QTimer>

#include <cstdio>

#define LM_EMIT(call) do { if (m_adaptor) m_adaptor->call; } while (0)

namespace lm {

namespace {

LifecyclePorts portsOf(const ServicePlatform &p, ServiceCore *core, ServiceEventFanOut *events)
{
    LifecyclePorts l;
    l.clock = p.clock;
    l.names = p.names;
    l.storage = core;
    l.journal = core;
    l.notifier = p.notifier;
    l.wake = p.wake;
    l.approval = p.approval;
    l.scheduler = core;
    l.events = events;
    l.ingestLog = core;
    return l;
}

QString subjectKey(const QUuid &subject) { return subject.toString(); }

QString isoOrEmpty(const UtcTime &t) { return t.isValid() ? t.toIso() : QString(); }

// A Subject key as the other methods print it, or a null UUID for text that is not one.
QUuid parseSubject(const QString &text)
{
    const QUuid id(text);
    return id;
}


} // namespace

ServiceRuntime::ServiceRuntime(const RuntimeOptions &options, const ServicePlatform &platform,
                               QObject *parent)
    : ServiceObject(parent), m_options(options), m_platform(platform),
      m_reports(platform.clock, &m_events),
      m_core(options.core, platform, &m_events),
      m_lifecycle(portsOf(platform, &m_core, &m_events), options.lifecycle),
      m_highWaterGuard(&m_maintenance), m_activateAfterStop(false), m_dbusRegistered(false)
{
    // The version stamp check reads the stamp through the platform and writes its rare
    // journal entries through the core's journal, the same one the lifecycle uses.
    if (m_platform.stamp)
        m_version.reset(new VersionWatch(m_platform.stamp, options.appVersion, &m_core, platform.clock,
                                         platform.serviceSettings, options.versionWatch));

    // The lifecycle needs the scheduler's `approval-waiting` warning; the D-Bus side
    // needs every event; the maintenance runner needs the high-water mark.
    m_events.add(&m_lifecycle);
    m_events.add(this);
    m_events.add(&m_highWaterGuard);
    m_events.add(&m_reports);

    m_platform.wake->setHandler([this]() { onWake(); });

    connect(&m_maintenance, SIGNAL(progress(QString,QString,int,int,qlonglong,qlonglong,double)),
            this, SLOT(onMaintenanceProgress(QString,QString,int,int,qlonglong,qlonglong,double)));
    connect(&m_maintenance, SIGNAL(finished(QString,bool,QString,QString,QString)),
            this, SLOT(onMaintenanceFinished(QString,bool,QString,QString,QString)));

    m_stopTimer.setInterval(100);
    connect(&m_stopTimer, SIGNAL(timeout()), this, SLOT(onStopTick()));

    // A name taken away from us is the same as a stop. Only the real name holder can
    // report it; a fake in a test never does.
    DBusNameOwner *nameHolder = dynamic_cast<DBusNameOwner *>(m_platform.names);
    if (nameHolder)
        nameHolder->setLostHandler([this]() { stop(StopReason::nameLost()); });
}

ServiceRuntime::~ServiceRuntime()
{
    m_maintenance.cancelAndWait();
    m_events.remove(&m_lifecycle);
    m_events.remove(this);
    m_events.remove(&m_highWaterGuard);
    m_events.remove(&m_reports);
}

int ServiceRuntime::begin()
{
    QDBusConnection bus = QDBusConnection::sessionBus();
    if (!bus.isConnected())
        return ServiceExitBus;

    // The object is exported BEFORE the name is taken, so that the calls the bus has queued
    // for an activating service are answered the moment the name is ours.
    if (!m_dbusRegistered) {
        m_adaptor.reset(new ServiceAdaptor(this, this));
        m_dbusRegistered = bus.registerObject(m_options.identity.objectPath, this,
                                              QDBusConnection::ExportAdaptors);
        if (!m_dbusRegistered)
            return ServiceExitBus;
    }

    switch (m_lifecycle.start()) {
    case StartOutcome::Started:
        // A probe secret left over from a UI that died between storing and removing it.
        if (m_core.discovery())
            m_core.discovery()->cleanStaleProbe();
        // Do not disturb is watched from now on (only an instance that runs posts anything: a second
        // instance has stepped aside above). The watcher posts or withdraws the notice to match the
        // state it reads, and tells the resident notice.
        if (m_platform.dnd) {
            m_dnd.reset(new DoNotDisturbWatcher(m_platform.dnd, m_platform.presenter, [this](bool on) {
                m_core.setDoNotDisturb(on);
                m_lifecycle.refreshNotice();
            }));
            m_dnd->start();
        }
        return -1;
    case StartOutcome::NotIdle:
        return -1;
    case StartOutcome::NameTaken:
        return ServiceExitOk;           // REQ-ARC-09: step aside, nothing written
    case StartOutcome::OpenFailed:
        return ServiceExitOpenFailed;
    }
    return ServiceExitOpenFailed;
}

void ServiceRuntime::onWake()
{
    const int heartbeatsBefore = m_lifecycle.heartbeatsWritten();
    m_lifecycle.onWake();
    // the version stamp is compared at each heartbeat (one a minute), not at every
    // wake-up, so a wake-up stays cheap. A stop that begins here ends this wake-up's work.
    if (m_lifecycle.heartbeatsWritten() != heartbeatsBefore && checkVersionStamp())
        return;
    // Looks again for a stale probe secret that the locked secret store hid at the start.
    if (m_core.discovery())
        m_core.discovery()->tick();
    checkSilentChanges();
}

// A timed silent mode ends by itself inside the engine and raises no event, so the runtime looks at
// every wake-up and announces a change of state (the signal SilentChanged). The first look only
// records the state, so a service that starts with a mode already on says nothing.
void ServiceRuntime::checkSilentChanges()
{
    SilentControl *silent = m_core.silentControl();
    if (!silent)
        return;
    const QList<QUuid> subjects = m_core.subjectIds();
    for (int i = 0; i < subjects.size(); ++i) {
        const QUuid id = subjects.at(i);
        const bool on = silent->isOn(id);
        if (!m_silentSeen.contains(id)) {
            m_silentSeen.insert(id, on);
            continue;
        }
        if (m_silentSeen.value(id) != on) {
            m_silentSeen.insert(id, on);
            LM_EMIT(SilentChanged(subjectKey(id), on, on && silent->until(id).isValid() ? silent->until(id).toIso() : QString()));
            m_lifecycle.refreshNotice();
        }
    }
}

// ---- the version stamp ------------------------------------------

bool ServiceRuntime::checkVersionStamp()
{
    // Never during a stop or before the start: only a running service acts.
    const ServiceState st = m_lifecycle.state();
    if (!m_version || (st != ServiceState::Running && st != ServiceState::RunningAwaitingApproval))
        return false;
    QJsonObject extra;
    extra.insert(VersionCheckKey::appVersion(), m_version->ownVersion());
    switch (m_version->check()) {
    case VersionAction::None:
        return false;
    case VersionAction::StopUpgraded:
        // The stop record names both versions. The loop guard's time is written before the stop, so
        // the new process finds it even if this one dies during the stop.
        extra.insert(VersionCheckKey::stamp(), m_version->stampVersion());
        m_version->noteSelfRestart();
        m_activateAfterStop = true;
        stop(StopReason::upgraded(), extra);
        return true;
    case VersionAction::StopRemoved:
        stop(StopReason::packageRemoved(), extra);
        return true;
    }
    return false;
}

// ---- stop ----------------------------------------------------------------------------------

void ServiceRuntime::stop(const QString &reason)
{
    stop(reason, QJsonObject());
}

void ServiceRuntime::stop(const QString &reason, const QJsonObject &stopExtra)
{
    if (m_lifecycle.state() == ServiceState::Stopping || stopped())
        return;
    // A running compaction must end before the stores close; it cancels cleanly (rolled back,
    // the store as it was) and the worker is joined. This is part of the stop sequence's bounded time.
    m_maintenance.cancelAndWait();
    m_dnd.reset();                      // stops listening; the notice stays as the phone's state is
    // Discoveries in flight end with the service: their adapter processes are killed and the
    // callers get an answer instead of waiting for a reply that will never come.
    if (m_core.discovery())
        m_core.discovery()->cancelAll();
    m_lifecycle.requestStop(reason, stopExtra);
    m_stopTimer.start();
}

void ServiceRuntime::onStopTick()
{
    if (!m_lifecycle.stepStop())
        return;
    m_stopTimer.stop();
    // after a stop for an upgrade the stop record is written, the store and the journal
    // are closed and the name is released (stepStop), so the new binary can take it. One activation
    // of the name starts it; the call waits for the new process, then this one exits. A failure is
    // logged and nothing else is tried: the next ping of the add-on, a tap on the notice or the
    // window starts the service as after any stop.
    if (m_activateAfterStop) {
        m_activateAfterStop = false;
        QString how;
        const bool ok = m_platform.activator && m_platform.activator->activate(&how);
        if (!m_platform.activator)
            how = QStringLiteral("no-activator");
        fprintf(stderr, "libre-monitor: restart after an upgrade: %s (%s)\n", ok ? "started" : "not started",
                qPrintable(how));
    }
    if (m_options.exitOnStop)
        QCoreApplication::exit(ServiceExitOk);
}

void ServiceRuntime::onUnixSignal(int signalNumber)
{
    // The numbers are the Linux ones; the application runs on Linux only.
    switch (signalNumber) {
    case 15: stop(StopReason::term()); break;       // SIGTERM
    case 2: stop(StopReason::interrupt()); break;   // SIGINT
    case 1: stop(StopReason::hangup()); break;      // SIGHUP
    default: break;
    }
}

// ---- ServiceApi ---------------------------------------------------------------------------------

QString ServiceRuntime::apiStart()
{
    // The activation itself has already started the service (any call does); this only
    // reports the state, so a call to a running service does nothing harmful. The one
    // exception is deliberate: a tap on the notice (it calls Start) while connections wait
    // for the approval raises the system dialog again.
    m_lifecycle.raiseApprovalNow();
    return serviceStateName(m_lifecycle.state());
}

QString ServiceRuntime::apiStatusJson()
{
    const ServiceStatus s = m_lifecycle.status();
    QJsonObject service;
    service.insert(QStringLiteral("state"), serviceStateName(s.state));
    service.insert(QStringLiteral("notice"), noticeStateName(s.notice));
    service.insert(QStringLiteral("last_heartbeat"), s.lastHeartbeat.isValid() ? s.lastHeartbeat.toIso() : QString());
    service.insert(QStringLiteral("start_cause"), s.startCause);
    service.insert(QStringLiteral("waiting_connections"), QJsonArray::fromStringList(s.waitingConnections));
    service.insert(QStringLiteral("do_not_disturb"), m_core.doNotDisturb());
    // the version of the binary that runs (LM_APP_VERSION), so the window can tell
    // an older service from a stopped one. A new key: an older reader ignores it (format stays 1).
    service.insert(QStringLiteral("app_version"), m_options.appVersion);

    QJsonArray subjects;
    Scheduler *sch = m_core.scheduler();
    if (sch && s.state != ServiceState::Stopping) {
        const QList<QUuid> ids = sch->subjects();
        for (int i = 0; i < ids.size(); ++i) {
            const SubjectStatus st = sch->status(ids.at(i));
            QJsonObject o;
            o.insert(QStringLiteral("subject"), subjectKey(st.subject));
            o.insert(QStringLiteral("connection"), st.connection);
            o.insert(QStringLiteral("state"), scheduleStateName(st.state));
            o.insert(QStringLiteral("cause"), st.cause);
            o.insert(QStringLiteral("retry_at"), st.retryAt.isValid() ? st.retryAt.toIso() : QString());
            o.insert(QStringLiteral("last_success_at"), st.lastSuccessAt.isValid() ? st.lastSuccessAt.toIso() : QString());
            o.insert(QStringLiteral("last_contact_ok_at"), st.lastContactOkAt.isValid() ? st.lastContactOkAt.toIso() : QString());
            o.insert(QStringLiteral("stale"), st.stale);
            o.insert(QStringLiteral("stale_cause"), st.staleCause);
            o.insert(QStringLiteral("stale_detail"), st.staleDetail);
            o.insert(QStringLiteral("stale_since"), st.staleSince.isValid() ? st.staleSince.toIso() : QString());

            // Added; the format number stays 1 because a reader that
            // does not know these keys ignores them. Still opaque ids and numbers only: no name,
            // no source reference.
            const QString adapterId = m_core.connectionConfig(st.connection).adapterId;
            o.insert(QStringLiteral("adapter"), adapterId);
            o.insert(QStringLiteral("adapter_version"), ServiceCore::adapterVersion(adapterId));
            o.insert(QStringLiteral("report_proposed"), m_reports.proposed(st.connection));
            QJsonObject sensor;
            o.insert(QStringLiteral("sensor"), m_core.latestSensor(st.subject, &sensor)
                                                   ? QJsonValue(sensor) : QJsonValue(QJsonValue::Null));
            // The trend arrow of the newest reading, in memory only; null when there is none or
            // its reading is old. `trend_for` says which reading it belongs to.
            o.insert(QStringLiteral("trend_code"), st.hasTrend ? QJsonValue(st.trendCode)
                                                               : QJsonValue(QJsonValue::Null));
            o.insert(QStringLiteral("trend_for"), st.hasTrend ? st.trendFor.toIso() : QString());
            // The silent mode of the Subject: on, and until when (empty for no limit).
            SilentControl *silent = m_core.silentControl();
            const bool silentOn = silent && silent->isOn(st.subject);
            o.insert(QStringLiteral("silent"), silentOn);
            o.insert(QStringLiteral("silent_until"), silentOn ? isoOrEmpty(silent->until(st.subject)) : QString());
            subjects.append(o);
        }
    }

    QJsonObject maintenance;
    maintenance.insert(QStringLiteral("running"), m_maintenance.isRunning());
    maintenance.insert(QStringLiteral("kind"), m_maintenance.runningKind());

    QJsonObject root;
    root.insert(QStringLiteral("format"), 1);
    root.insert(QStringLiteral("service"), service);
    root.insert(QStringLiteral("subjects"), subjects);
    root.insert(QStringLiteral("maintenance"), maintenance);
    return QString::fromUtf8(QJsonDocument(root).toJson(QJsonDocument::Compact));
}

void ServiceRuntime::apiRetry(const QString &connection)
{
    if (m_core.scheduler() && m_lifecycle.state() != ServiceState::Stopping)
        m_core.scheduler()->userRetry(connection);
}

bool ServiceRuntime::apiReenable(const QString &connection)
{
    // The user acted on the connection: the open report proposal (REQ-GRD-15) is settled.
    m_reports.clear(connection);
    if (m_core.notices())
        m_core.notices()->reportSettled(connection);
    return m_core.guard() && m_core.guard()->reenable(connection);
}

bool ServiceRuntime::apiReenableVersion(const QString &adapter, const QString &version)
{
    m_reports.clearAll();
    return m_core.guard() && m_core.guard()->reenableVersion(adapter, version);
}

// The UI tells the service that the diagnostic report was written or declined. Only an
// open proposal is ended; the proposal can come back only after two new consecutive guard kills
// with no success between them, because ending it forgets the count (ReportProposalTracker).
// Nothing is stored.
bool ServiceRuntime::apiReportHandled(const QString &connection)
{
    if (!m_reports.proposed(connection))
        return false;
    m_reports.clear(connection);
    if (m_core.notices())
        m_core.notices()->reportSettled(connection);
    return true;
}

// The content of the diagnostic report for the developer (REQ-GRD-15), for the page that
// shows it to the user and writes the file when the user agrees. Answered only while a
// proposal is open, because that is the only moment the user is asked; the proposal is NOT ended
// here: the user may still decline, and the page says so with ReportHandled. The text is
// JSON Lines from DiagnosticCollector: versions, the command, the violations, counts and the held
// batch reduced to its shape, with no glucose value, name, reference, address or credential.
// Errors: {"format":1,"error":...} with unknown-connection (not in the configuration),
// not-proposed (no open proposal), no-report (a proposal without a noted guard kill: the service
// restarted or the kill was counted from the scheduler's state alone) or service-not-running.
QString ServiceRuntime::apiDiagnosticReport(const QString &connection)
{
    const ServiceState st = m_lifecycle.state();
    const auto error = [](const char *key) {
        return QStringLiteral("{\"format\":1,\"error\":\"%1\"}").arg(QLatin1String(key));
    };
    DiagnosticCollector *diagnostics = m_core.diagnostics();
    if (!diagnostics || (st != ServiceState::Running && st != ServiceState::RunningAwaitingApproval))
        return error("service-not-running");
    if (m_core.connectionConfig(connection).id.isEmpty())
        return error("unknown-connection");
    if (!m_reports.proposed(connection))
        return error("not-proposed");
    const QByteArray text = diagnostics->report(connection, m_options.appVersion);
    if (text.isEmpty())
        return error("no-report");
    return QString::fromUtf8(text);
}

void ServiceRuntime::apiCredentialsChanged(const QString &connection)
{
    if (m_core.scheduler())
        m_core.scheduler()->credentialsChanged(connection);
}

void ServiceRuntime::apiIntervalsChanged(const QString &connection)
{
    m_core.configurationChanged(connection);
}

QString ServiceRuntime::apiConfigChanged()
{
    return m_core.reloadConfiguration();
}

// ---- the alerts -------------------------------------------------------

AlertCoordinator *ServiceRuntime::runningAlerts()
{
    const ServiceState st = m_lifecycle.state();
    if (st != ServiceState::Running && st != ServiceState::RunningAwaitingApproval)
        return 0;
    return m_core.alerts();
}

namespace {

// A slot by its name ("low", "high", "missing"); false for anything else.
bool parseSlot(const QString &text, AlertSlot *slot)
{
    static const AlertSlot all[] = { AlertSlot::Low, AlertSlot::High, AlertSlot::Missing };
    for (size_t i = 0; i < sizeof(all) / sizeof(all[0]); ++i)
        if (alertSlotName(all[i]) == text) {
            *slot = all[i];
            return true;
        }
    return false;
}

QJsonObject thresholdsJson(const QUuid &subject, const Thresholds &t)
{
    QJsonObject o;
    o.insert(QStringLiteral("subject"), subject.toString());
    o.insert(QStringLiteral("ll"), t.ll);
    o.insert(QStringLiteral("l"), t.l);
    o.insert(QStringLiteral("h"), t.h);
    o.insert(QStringLiteral("hh"), t.hh);
    return o;
}

QJsonObject rangeJson(qint64 minMs, qint64 maxMs, qint64 defaultMs)
{
    QJsonObject o;
    o.insert(QStringLiteral("min"), double(minMs / 1000));
    o.insert(QStringLiteral("max"), double(maxMs / 1000));
    o.insert(QStringLiteral("default"), double(defaultMs / 1000));
    return o;
}

} // namespace

// What is going on, for a user interface that was closed (or is new). ONE call gives a page of the
// application everything it needs, so that none needs a second call:
//  - `alerts[]`: every slot that is active, acknowledged or snoozed (a pending alert is not shown),
//    with the level, the state, the acknowledged flag, the episode, when the alert and its current
//    level began (`level_since`) and when its first level did (`episode_since`), the limit crossed,
//    the newest reading's time for a no-data alert, when a snooze ends and the staleness cause;
//  - `thresholds[]`: for EVERY followed Subject the four limits in force and its silent mode (on, and
//    until when, empty for no limit);
//  - `thresholds[]` also carries `persistence_low_s`, `persistence_high_s` and `missing_limit_s`: the three
//    per-Subject times in force, in whole seconds, after the engine's clamping; and `limits` gives the
//    allowed `min`, `max` and `default` of each, in seconds (the keys of the configuration file have the same names);
//  - `do_not_disturb`: whether the phone will silence the alerts, so a page can say it.
// All limits are whole mg/dL, the screen converts them for display. No glucose value,
// no name and no comment is in it.
QString ServiceRuntime::apiAlerts()
{
    AlertCoordinator *alerts = runningAlerts();
    if (!alerts)
        return QStringLiteral("{\"format\":1,\"error\":\"service-not-running\"}");
    SilentControl *silent = m_core.silentControl();
    QJsonArray entries;
    const QVector<AlertView> views = alerts->alerts();
    for (int i = 0; i < views.size(); ++i) {
        const AlertView &v = views.at(i);
        if (v.state != AlertState::Active && v.state != AlertState::Acknowledged && v.state != AlertState::Snoozed)
            continue;
        QJsonObject o;
        o.insert(QStringLiteral("subject"), v.subject.toString());
        o.insert(QStringLiteral("slot"), alertSlotName(v.slot));
        o.insert(QStringLiteral("level"), alertTypeName(v.type));
        o.insert(QStringLiteral("state"), alertStateName(v.state));
        o.insert(QStringLiteral("acknowledged"), v.acknowledged);
        o.insert(QStringLiteral("episode"), v.episode.toString());
        o.insert(QStringLiteral("since"), isoOrEmpty(v.since));
        o.insert(QStringLiteral("level_since"), isoOrEmpty(v.levelSince.isValid() ? v.levelSince : v.since));
        o.insert(QStringLiteral("episode_since"), isoOrEmpty(v.episodeSince));
        o.insert(QStringLiteral("limit"), v.hasLimit ? QJsonValue(v.limit) : QJsonValue(QJsonValue::Null));
        o.insert(QStringLiteral("newest_reading_at"), isoOrEmpty(v.newestReadingAt));
        o.insert(QStringLiteral("snooze_until"), isoOrEmpty(v.snoozeUntil));
        o.insert(QStringLiteral("cause"), v.cause);
        entries.append(o);
    }
    QJsonArray thresholds;
    const QList<QUuid> subjects = m_core.subjectIds();
    for (int i = 0; i < subjects.size(); ++i) {
        QJsonObject o = thresholdsJson(subjects.at(i), alerts->thresholds(subjects.at(i)));
        const bool on = silent && silent->isOn(subjects.at(i));
        const ServiceCore::AlertKeyValues keys = m_core.alertKeysInForce(subjects.at(i));
        o.insert(QStringLiteral("persistence_low_s"), double(keys.persistenceLowS));
        o.insert(QStringLiteral("persistence_high_s"), double(keys.persistenceHighS));
        o.insert(QStringLiteral("missing_limit_s"), double(keys.missingLimitS));
        o.insert(QStringLiteral("silent"), on);
        o.insert(QStringLiteral("silent_until"), on ? isoOrEmpty(silent->until(subjects.at(i))) : QString());
        thresholds.append(o);
    }
    QJsonObject root;
    root.insert(QStringLiteral("format"), 1);
    root.insert(QStringLiteral("alerts"), entries);
    root.insert(QStringLiteral("thresholds"), thresholds);
    // The allowed range and the default of each per-Subject time, in whole seconds, so the thresholds
    // page can show them without knowing the engine.
    const AlertSettings &r = m_core.alertRanges();
    QJsonObject limits;
    limits.insert(QStringLiteral("persistence_low_s"), rangeJson(0, r.persistenceLowMaxMs, r.persistenceLowDefaultMs));
    limits.insert(QStringLiteral("persistence_high_s"), rangeJson(0, r.persistenceHighMaxMs, r.persistenceHighDefaultMs));
    limits.insert(QStringLiteral("missing_limit_s"), rangeJson(r.missingLimitMinMs, r.missingLimitMaxMs, r.missingLimitDefaultMs));
    root.insert(QStringLiteral("limits"), limits);
    root.insert(QStringLiteral("do_not_disturb"), m_core.doNotDisturb());
    return QString::fromUtf8(QJsonDocument(root).toJson(QJsonDocument::Compact));
}

QString ServiceRuntime::apiSetSilent(const QString &subject, int minutes)
{
    QJsonObject root;
    SilentControl *silent = m_core.silentControl();
    const QUuid id = parseSubject(subject);
    QString result;
    if (!runningAlerts())
        result = QStringLiteral("service-not-running");
    else if (!silent)
        result = QStringLiteral("unsupported");
    else if (!m_core.subjectIds().contains(id))
        result = QStringLiteral("unknown-subject");
    else if (minutes < kSilentUntilTomorrow || minutes > 366 * 24 * 60)
        result = QStringLiteral("bad-duration");
    else
        result = silent->set(id, minutes) ? QStringLiteral("done") : QStringLiteral("bad-duration");
    root.insert(QStringLiteral("result"), result);
    if (result == QLatin1String("done")) {
        root.insert(QStringLiteral("silent_until"), isoOrEmpty(silent->until(id)));
        m_silentSeen.insert(id, true);
        LM_EMIT(SilentChanged(subjectKey(id), true, isoOrEmpty(silent->until(id))));
        m_lifecycle.refreshNotice();
    }
    return QString::fromUtf8(QJsonDocument(root).toJson(QJsonDocument::Compact));
}

#ifdef LIBREMON_DEVSOURCE
// Development builds only: synthetic effects to the notifier, so every kind
// of notification can be seen and heard on the phone without glucose. Nothing here reaches the engine,
// the store or the source; the Subjects are four made-up identifiers (daemon/devfeed.h).
QString ServiceRuntime::apiDevInject(const QString &json)
{
    if (!runningAlerts() || !m_core.devFeed())
        return QStringLiteral("{\"ok\":false,\"error\":\"service-not-running\"}");
    const QString answer = m_core.devFeed()->apply(json);
    m_lifecycle.refreshNotice();            // the resident notice lists the synthetic alerts too
    return answer;
}
#endif

QString ServiceRuntime::apiEndSilent(const QString &subject)
{
    SilentControl *silent = m_core.silentControl();
    const QUuid id = parseSubject(subject);
    if (!runningAlerts())
        return QStringLiteral("service-not-running");
    if (!silent)
        return QStringLiteral("unsupported");
    if (!m_core.subjectIds().contains(id))
        return QStringLiteral("unknown-subject");
    if (!silent->end(id))
        return QStringLiteral("not-silent");
    m_silentSeen.insert(id, false);
    LM_EMIT(SilentChanged(subjectKey(id), false, QString()));
    m_lifecycle.refreshNotice();
    return QStringLiteral("done");
}

QString ServiceRuntime::apiAcknowledgeAlert(const QString &subject, const QString &slotName)
{
    AlertCoordinator *alerts = runningAlerts();
    if (!alerts)
        return QStringLiteral("service-not-running");
    AlertSlot slot;
    if (!parseSlot(slotName, &slot))
        return QStringLiteral("bad-slot");
    return actionResultName(alerts->acknowledge(parseSubject(subject), slot));
}

QString ServiceRuntime::apiSnoozeAlert(const QString &subject, const QString &slotName, int minutes)
{
    AlertCoordinator *alerts = runningAlerts();
    if (!alerts)
        return QStringLiteral("service-not-running");
    AlertSlot slot;
    if (!parseSlot(slotName, &slot))
        return QStringLiteral("bad-slot");
    return actionResultName(alerts->snooze(parseSubject(subject), slot, minutes));
}

// The answer is always JSON with a "result" word, so that a refusal can carry what the screen
// needs to explain it in words with numbers: the boundary at fault, the allowed ranges
// after `inconsistent`, the standard values after a missing or wrong challenge. There is no comment
// the change is recorded with its time, boundary, new and previous value only.
QString ServiceRuntime::apiSetThresholds(const QString &subject, int l, int h, int typedL, int typedH)
{
    QJsonObject root;
    AlertCoordinator *alerts = runningAlerts();
    if (!alerts) {
        root.insert(QStringLiteral("result"), QStringLiteral("service-not-running"));
        return QString::fromUtf8(QJsonDocument(root).toJson(QJsonDocument::Compact));
    }
    // Any number typed again (0 or more) is the caregiver's confirmation; -1 is "nothing typed".
    Confirmation confirmation;
    if (typedL >= 0 || typedH >= 0)
        confirmation = Confirmation::typed(typedL, typedH);
    EditOutcome out;
    const QUuid id = parseSubject(subject);
    alerts->setThresholds(id, l, h, confirmation, &out);
    root.insert(QStringLiteral("result"), editResultName(out.result));
    switch (out.result) {
    case EditResult::Accepted:
        root.insert(QStringLiteral("thresholds"), thresholdsJson(id, alerts->thresholds(id)));
        break;
    case EditResult::Inconsistent:
        root.insert(QStringLiteral("l_min"), out.lMin);
        root.insert(QStringLiteral("l_max"), out.lMax);
        root.insert(QStringLiteral("h_min"), out.hMin);
        root.insert(QStringLiteral("h_max"), out.hMax);
        break;
    case EditResult::NeedsConfirmation:
    case EditResult::ConfirmationMismatch:
        root.insert(QStringLiteral("global_l"), out.globalL);
        root.insert(QStringLiteral("global_h"), out.globalH);
        break;
    default:
        break;
    }
    if (out.hasOffending)
        root.insert(QStringLiteral("offending"), boundaryName(out.offending));
    return QString::fromUtf8(QJsonDocument(root).toJson(QJsonDocument::Compact));
}

// Asynchronous: the answer comes through `done` when the discovery is over (daemon/discovery.h).
// A service that is not running cannot discover; the answer says so instead of leaving the
// caller waiting for a reply.
void ServiceRuntime::apiDiscover(const QString &connection, const std::function<void(const QString &)> &done)
{
    const ServiceState st = m_lifecycle.state();
    DiscoveryService *discovery = m_core.discovery();
    if (!discovery || (st != ServiceState::Running && st != ServiceState::RunningAwaitingApproval)) {
        const QString json = DiscoveryService::errorAnswer(
            st == ServiceState::Stopping ? QStringLiteral("service-stopping") : QStringLiteral("service-not-running"));
        QTimer::singleShot(0, this, [done, json]() { done(json); });
        return;
    }
    discovery->discover(connection, done);
}

QString ServiceRuntime::apiStartMaintenance(const QString &kind, const QString &parametersJson)
{
    const ServiceState st = m_lifecycle.state();
    if (st != ServiceState::Running && st != ServiceState::RunningAwaitingApproval)
        return QStringLiteral("service-not-running");
    if (m_maintenance.isRunning())
        return QStringLiteral("maintenance-running");
    // Only one job at a time (above); the purge's Subject facts come from the configuration in force.
    MaintenanceContext context;
    context.storePath = m_core.storePath();
    context.journalPath = m_core.journalPath();
    context.freeSpace = m_options.compactionFreeSpace;
    context.purgeTarget = [this](const QUuid &subject) { return m_core.purgeTarget(subject); };
    context.openJournal = m_options.purgeJournalOpener;
    QString error;
    const MaintenanceRunner::Job job = buildMaintenanceJob(kind, parametersJson, context, &error);
    if (!job)
        return error;
    return m_maintenance.start(kind, job) ? QString() : QStringLiteral("maintenance-running");
}

void ServiceRuntime::apiCancelMaintenance()
{
    m_maintenance.cancel(CancelReason::user());
}

void ServiceRuntime::apiStop()
{
    // Answer the call first; the stop sequence runs after the reply has gone out.
    QMetaObject::invokeMethod(this, "stopRequested", Qt::QueuedConnection);
}

// ---- events to D-Bus ------------------------------------------------------------------------------


void ServiceRuntime::gapDetected(const UtcTime &from, const UtcTime &to)
{
    LM_EMIT(GapDetected(from.toIso(), to.toIso()));
}

void ServiceRuntime::subjectStateChanged(const SubjectStatus &s)
{
    LM_EMIT(StateChanged(subjectKey(s.subject), scheduleStateName(s.state), s.cause, s.connection));
}

void ServiceRuntime::stalenessChanged(const SubjectStatus &s)
{
    LM_EMIT(StalenessChanged(subjectKey(s.subject), s.stale, s.staleCause));
}

void ServiceRuntime::connectionWarning(const QString &connection, const QString &kind, const QString &detail)
{
    LM_EMIT(Warning(kind, connection, detail));
}

void ServiceRuntime::readingQueueHighWater(int n)
{
    LM_EMIT(Warning(QStringLiteral("reading-queue-high-water"), QString(), QStringLiteral("count=%1").arg(n)));
}

void ServiceRuntime::readingQueueFull(int n)
{
    LM_EMIT(Warning(QStringLiteral("reading-queue-full"), QString(), QStringLiteral("count=%1").arg(n)));
}

void ServiceRuntime::diagnosticReportProposed(const QString &connection)
{
    LM_EMIT(DiagnosticReportProposed(connection));
}

void ServiceRuntime::dataChanged(const QUuid &subject)
{
    LM_EMIT(DataChanged(subjectKey(subject)));
}

void ServiceRuntime::alertChanged(const QUuid &subject, const QString &slot, const QString &state)
{
    LM_EMIT(AlertChanged(subjectKey(subject), slot, state));
    m_lifecycle.refreshNotice();            // the resident notice lists the alerts
}

void ServiceRuntime::onMaintenanceProgress(const QString &kind, const QString &phase, int step,
                                           int steps, qlonglong done, qlonglong total, double fraction)
{
    LM_EMIT(MaintenanceProgress(kind, phase, step, steps, done, total, fraction));
}

void ServiceRuntime::onMaintenanceFinished(const QString &kind, bool ok, const QString &errorClass,
                                           const QString &message, const QString &cancelReason)
{
    const QString details = QString::fromUtf8(
        QJsonDocument(m_maintenance.lastResult().details).toJson(QJsonDocument::Compact));
    LM_EMIT(MaintenanceFinished(kind, ok, errorClass, message, cancelReason, details));
}

#undef LM_EMIT

} // namespace lm
