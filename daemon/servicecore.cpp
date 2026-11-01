// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Michele Brami
#include "servicecore.h"

#include "guard/guard.h"
#include "guard/ingestlog.h"
#include "guard/journalstatelog.h"
#include "guard/statekeeper.h"
#include "scheduler/adapterlinkport.h"
#include "scheduler/storereadingstore.h"
#include "session/adapterregistry.h"
#include "store/storagecheck.h"

#include <algorithm>

#include <QDir>
#include <QFile>

namespace lm {

namespace {

// The newest stored reading of a Subject and its trend, for the notification texts' optional value
// Read from the store and the scheduler at posting time, so the
// effects carry no value and no new port is needed.
class ServiceValueSource : public AlertValueSource
{
public:
    ServiceValueSource(ServiceCore *core, AlertReadingSource *readings) : m_core(core), m_readings(readings) {}
    bool newest(const QUuid &subject, double *value, QString *unit, int *trendCode, UtcTime *measuredAt) const
    {
        QString source;
        const QStringList ids = m_core->connectionIds();
        for (int i = 0; i < ids.size() && source.isEmpty(); ++i) {
            const ConnectionConfig c = m_core->connectionConfig(ids.at(i));
            for (int j = 0; j < c.subjects.size(); ++j)
                if (c.subjects.at(j).id == subject)
                    source = c.adapterId + QStringLiteral(".current");
        }
        if (source.isEmpty() || !m_readings->newestCurrent(subject, source, value, unit, measuredAt))
            return false;
        *trendCode = -1;
        if (m_core->scheduler()) {
            const SubjectStatus st = m_core->scheduler()->status(subject);
            if (st.hasTrend)
                *trendCode = st.trendCode;
        }
        return true;
    }

private:
    ServiceCore *m_core;
    AlertReadingSource *m_readings;
};

class CoreResidentFacts : public ResidentFactsSource
{
public:
    explicit CoreResidentFacts(const ServiceCore *core) : m_core(core) {}
    ResidentInput facts() const { return m_core->residentFacts(); }

private:
    const ServiceCore *m_core;
};

} // namespace

ServiceCore::ServiceCore(const CoreOptions &options, const ServicePlatform &platform,
                         ServiceEventFanOut *events)
    : m_options(options), m_platform(platform), m_events(events), m_open(false)
{
}

ServiceCore::~ServiceCore()
{
    close();
}

QString ServiceCore::storePath() const { return m_options.dataDirectory + QStringLiteral("/store.db"); }
QString ServiceCore::journalPath() const { return m_options.dataDirectory + QStringLiteral("/journal.db"); }

// ---- ServiceStorage -----------------------------------------------------------------------

bool ServiceCore::open(QString *cause)
{
    if (m_open)
        return true;
    if (!QDir().mkpath(m_options.dataDirectory)) {
        *cause = QStringLiteral("store-open-failed");
        return false;
    }
    // The journal first: it is the evidence of what the service did, and the notice for a
    // failure to open the store is still worth a journal entry in the next run.
    if (!m_journal.open(journalPath())) {
        *cause = QStringLiteral("journal-open-failed");
        return false;
    }
    if (!m_store.open(storePath())) {
        m_journal.close();
        *cause = QStringLiteral("store-open-failed");
        return false;
    }

    // A missing configuration file is a normal first run: no connections, nothing to poll.
    // A file that exists but is not valid is a failure the user must see: nothing is started.
    if (QFile::exists(m_options.configPath)) {
        m_config = loadServiceConfig(m_options.configPath);
        if (!m_config.ok) {
            m_store.close();
            m_journal.close();
            *cause = QStringLiteral("config-invalid");
            return false;
        }
    } else {
        m_config = ServiceConfig();
        m_config.ok = true;
    }

    // The wiring, in the order given there.
    m_lifecycleLog.reset(new JournalLifecycleLog(&m_journal));
    m_stateLog.reset(new JournalStateLog(&m_journal));
    m_keeper.reset(new StateKeeper(m_stateLog.get(), m_platform.clock));
    m_keeper->load();
    m_ingestSink.reset(new StoreIngestLogSink(&m_store));
    m_ingest.reset(new IngestLog(m_ingestSink.get(), GuardSettings().ingestQueueMax));
    m_guard.reset(new Guard(m_platform.clock, m_keeper.get(), m_ingest.get()));
    m_diagnostics.reset(new DiagnosticCollector(m_guard.get()));
    m_readings.reset(new StoreReadingStore(&m_store, m_platform.clock));
    m_notifying.reset(new NotifyingReadingStore(m_readings.get(), m_events));

    // The maker builds one fully configured link per connection, on demand. The scheduler
    // exists by the time it is first called, so it is looked up then, not captured now.
    m_baseLinks.reset(new AdapterLinkFactory([this](const QString &connectionId) -> AdapterLink * {
        // The policy is the collector, which hands every call to the guard unchanged and keeps
        // what the diagnostic report needs; it also takes the batch a guard kill interrupted.
        AdapterLink *link = new AdapterLink(m_diagnostics.get());
        link->setRecordSink(m_scheduler->recordSink());
        link->setQuarantineSink(m_scheduler->quarantineSink());
        link->setRejectedBatchSink(m_diagnostics.get());
        Clock *clock = m_platform.clock;
        link->setClock([clock]() { return clock->now(); });
        const ConnectionConfig cfg = connectionConfig(connectionId);
        const AdapterLaunch launch = m_options.adapterLaunch(cfg.adapterId);
        link->setProgram(launch.program, launch.arguments);
        if (m_options.channelFactory)
            link->setChannelFactory(m_options.channelFactory);
        return link;
    }));
    m_links.reset(new TrackingLinkFactory(m_baseLinks.get()));

    SchedulerPorts p;
    p.clock = m_platform.clock;
    p.random = m_platform.random;
    p.links = m_links.get();
    p.secrets = m_platform.secrets;
    p.settings = m_platform.connectionSettings;
    p.readings = m_notifying.get();
    p.state = m_keeper.get();
    p.guard = m_guard.get();
    p.ingest = m_ingest.get();
    m_scheduler.reset(new Scheduler(p));
    m_scheduler->setListener(m_events);

    for (int i = 0; i < m_config.connections.size(); ++i) {
        m_scheduler->addConnection(m_config.connections.at(i));
        // The adapter's own version, from the registry: lets a connection stopped by
        // `protocol-changed` try once more after an update.
        const AdapterEntry *e = AdapterRegistry::instance().find(m_config.connections.at(i).adapterId);
        if (e)
            m_scheduler->setCurrentAdapterVersion(e->id, e->version);
    }
    m_host.reset(new SchedulerHostAdapter(m_scheduler.get(), m_links.get()));

    // The one-time discovery of Subjects for the set-up page. It reads the persistent
    // guard state (never writes it) and starts its own short-lived adapter links, built here
    // like the scheduler's but without sinks: a discovery receives no records.
    DiscoveryPorts dp;
    dp.clock = m_platform.clock;
    dp.secrets = m_platform.secrets;
    dp.writer = m_platform.credentials;
    dp.state = m_keeper.get();
    dp.connection = [this](const QString &id) { return connectionConfig(id); };
    dp.settings = [this](const QString &id) {
        return m_platform.connectionSettings ? m_platform.connectionSettings->get(id) : QJsonObject();
    };
    dp.makeLink = [this](const QString &adapterId, LinkPolicy *policy) -> AdapterLink * {
        AdapterLink *link = new AdapterLink(policy);
        Clock *clock = m_platform.clock;
        link->setClock([clock]() { return clock->now(); });
        const AdapterLaunch launch = m_options.adapterLaunch(adapterId);
        link->setProgram(launch.program, launch.arguments);
        if (m_options.channelFactory)
            link->setChannelFactory(m_options.channelFactory);
        return link;
    };
    m_discovery.reset(new DiscoveryService(dp, m_options.discovery));

    // The alerts. Every configured Subject is watched; its `current` source tag
    // is "<adapter>.current" (REQ-DAT-09). The per-Subject persistence and missing-data limit are
    // the engine's defaults until the configuration file carries them.
    m_alertReadings.reset(new StoreAlertReadings(&m_store));
    m_alertRelay.reset(new AlertChangeRelay(m_platform.alerts, m_events));
    m_valueSource.reset(new ServiceValueSource(this, m_alertReadings.get()));
    m_alerts.reset(new AlertCoordinator(&m_journal, m_alertReadings.get(), m_alertRelay.get(),
                                        m_platform.clock, m_options.alerts));
    publishLabels();
    for (int i = 0; i < m_config.connections.size(); ++i) {
        const ConnectionConfig &c = m_config.connections.at(i);
        for (int j = 0; j < c.subjects.size(); ++j)
            m_alerts->addSubject(c.subjects.at(j).id, alertSettingsFor(c, c.subjects.at(j).id));
    }
    m_events->add(m_alerts.get());

    // The notifier needs what only the core knows: the coordinator's thresholds for the limits, the
    // newest reading for the optional value and for the last reading of a no-data alert, and the
    // acknowledgement a deletion by the user stands for.
    m_effectFacts.reset(new EngineEffectFacts);
    // Silent mode is the (the coordinator); a test may replace it by a fake in the options.
    m_silent.reset(new CoordinatorSilentControl(m_alerts.get()));
    if (AlertNotificationPresenter *p = m_platform.presenter) {
        p->setFactsReader(m_effectFacts.get());
        p->setValueSource(m_valueSource.get());
        AlertCoordinator *alerts = m_alerts.get();
#ifdef LIBREMON_DEVSOURCE
        // The developer-only feed: synthetic effects through the same notifier. It reads its own facts for
        // the synthetic Subjects and the effect facts above for everything else.
        m_devFeed.reset(new DevFeed(p, m_platform.labels, m_platform.clock, m_effectFacts.get(),
                                    [this](bool on) { m_doNotDisturb = on; }, m_events));
        p->setFactsReader(m_devFeed.get());
        DevFeed *dev = m_devFeed.get();
        p->setAcknowledgeHandler([alerts, dev](const QUuid &subject, AlertSlot slot) {
            if (DevFeed::isSynthetic(subject))
                dev->acknowledge(subject, slot);
            else
                alerts->acknowledge(subject, slot);
        });
#else
        p->setAcknowledgeHandler([alerts](const QUuid &subject, AlertSlot slot) { alerts->acknowledge(subject, slot); });
#endif
        p->setConnectionInfo([this](const QString &connection) {
                                 QStringList ids;
#ifdef LIBREMON_DEVSOURCE
                                 if (connection == QLatin1String("dev")) {       // the synthetic connection of the feed
                                     ids << DevFeed::subjectId(1).toString() << DevFeed::subjectId(2).toString();
                                     return ids;
                                 }
#endif
                                 const ConnectionConfig c = connectionConfig(connection);
                                 for (int i = 0; i < c.subjects.size(); ++i)
                                     ids << c.subjects.at(i).id.toString();
                                 return ids;
                             },
                             [this](const QString &connection) { return connectionLabel(connection); });
        m_notices.reset(new NoticeWatcher(p));
        m_events->add(m_notices.get());
    }
    applyNotificationSettings();
    if (m_platform.resident) {
        m_residentFacts.reset(new CoreResidentFacts(this));
        m_platform.resident->setSource(m_residentFacts.get());
    }

    m_open = true;
    return true;
}

void ServiceCore::close()
{
    if (!m_open)
        return;
    // Reverse order of construction. The discovery goes first (its runs end their processes);
    // then the scheduler: it owns links that call into the guard and the stores while they are
    // deleted.
    m_discovery.reset();
#ifdef LIBREMON_DEVSOURCE
    m_devFeed.reset();
#endif
    if (m_platform.resident)
        m_platform.resident->setSource(0);
    m_residentFacts.reset();
    if (m_notices) {
        m_events->remove(m_notices.get());
        m_notices.reset();
    }
    if (m_platform.presenter) {
        m_platform.presenter->setConnectionInfo(AlertNotificationPresenter::SubjectsOfConnection(), AlertNotificationPresenter::ConnectionLabel());
        m_platform.presenter->setAcknowledgeHandler(AlertNotificationPresenter::AcknowledgeHandler());
        m_platform.presenter->setFactsReader(0);
        m_platform.presenter->setValueSource(0);
    }
    if (m_alerts)
        m_events->remove(m_alerts.get());
    m_alerts.reset();
    m_alertRelay.reset();
    m_effectFacts.reset();
    m_valueSource.reset();
    m_alertReadings.reset();
    m_host.reset();
    m_scheduler.reset();
    m_links.reset();
    m_baseLinks.reset();
    m_notifying.reset();
    m_readings.reset();
    m_diagnostics.reset();
    m_guard.reset();
    m_ingest.reset();
    m_ingestSink.reset();
    m_keeper.reset();
    m_stateLog.reset();
    m_lifecycleLog.reset();
    m_store.close();
    m_journal.close();
    m_open = false;
}

bool ServiceCore::storageCheckDue(const UtcTime &now)
{
    return m_open && lm::storageCheckDue(m_journal, now);
}

void ServiceCore::runStorageCheck(const UtcTime &now)
{
    if (!m_open)
        return;
    StorageCheckOptions o;
    o.now = now;
    // The check keeps to once a week by itself and prunes the journal first (REQ-SCH-14). A
    // failure is recorded in its result and never stops monitoring.
    lm::runStorageCheck(m_store, m_journal, o);
}

// ---- ServiceJournal -------------------------------------------------------------------------

bool ServiceCore::lastLifecycleEntry(JournalEntry *entry) { return m_lifecycleLog->lastLifecycleEntry(entry); }
bool ServiceCore::hasStartRecord() { return m_lifecycleLog->hasStartRecord(); }
bool ServiceCore::append(const JournalEntry &entry) { return m_lifecycleLog->append(entry); }

bool ServiceCore::append(const IngestLogEntry &entry)
{
    return m_ingestSink && m_ingestSink->append(entry);
}

// ---- SchedulerHost --------------------------------------------------------------------------

void ServiceCore::start(const UtcTime &since)
{
    m_host->start(since);
    m_alerts->start();      // after the scheduler: its first status events are already cached
    // The restore has delivered its notifications; what the last run left up for an alert that no
    // longer exists is taken down.
    if (m_platform.presenter)
        m_platform.presenter->closeOrphans();
}

void ServiceCore::tick()
{
    m_host->tick();
    if (m_alerts)
        m_alerts->tick();
}

// The earlier of the scheduler's and the alerts' next due time: the service wakes for whichever
// comes first (the engine's due times are what makes a reminder or a snooze end on time).
UtcTime ServiceCore::nextWakeAt() const
{
    const UtcTime a = m_host ? m_host->nextWakeAt() : UtcTime();
    const UtcTime b = m_alerts ? m_alerts->nextWakeAt() : UtcTime();
    if (!a.isValid()) return b;
    if (!b.isValid()) return a;
    return b < a ? b : a;
}
void ServiceCore::shutdown() { if (m_host) m_host->shutdown(); }
bool ServiceCore::linksEnded() const { return !m_host || m_host->linksEnded(); }
int ServiceCore::queuedReadings() const { return m_host ? m_host->queuedReadings() : 0; }
int ServiceCore::flushQueuedReadings() { return m_host ? m_host->flushQueuedReadings() : 0; }
void ServiceCore::secretsAvailable(const QString &c) { if (m_host) m_host->secretsAvailable(c); }

// ---- reading for Status() -------------------------------------------------------------------

QString ServiceCore::adapterVersion(const QString &adapterId)
{
    const AdapterEntry *e = AdapterRegistry::instance().find(adapterId);
    return e ? e->version : QString();
}

bool ServiceCore::latestSensor(const QUuid &subject, QJsonObject *sensor)
{
    if (!m_open)
        return false;
    const QVector<Sensor> all = m_store.sensors();
    const Sensor *best = 0;
    for (int i = 0; i < all.size(); ++i)
        if (all.at(i).subject == subject && (!best || best->lastSeen < all.at(i).lastSeen))
            best = &all.at(i);
    if (!best)
        return false;
    QJsonObject o;
    o.insert(QStringLiteral("id"), best->sensorId);
    if (best->activatedAt.isValid())
        o.insert(QStringLiteral("activated_at"), best->activatedAt.toIso());
    if (best->warmupMinutes >= 0)
        o.insert(QStringLiteral("warmup_minutes"), best->warmupMinutes);
    *sensor = o;
    return true;
}

// ---- configuration --------------------------------------------------------------------------

ConnectionConfig ServiceCore::connectionConfig(const QString &id) const
{
    for (int i = 0; i < m_config.connections.size(); ++i)
        if (m_config.connections.at(i).id == id)
            return m_config.connections.at(i);
    return ConnectionConfig();
}

// A Subject is in exactly one connection (the configuration reader refuses a Subject listed twice), so
// its source reference is the one of that entry. The ingest log is per connection: only a connection
// whose one Subject is this one may have its log purged; otherwise the entries stay.
// Assumption: a source reference the configuration no longer names (changed by a restart-needed edit)
// is not known any more, so quarantine entries that carry an older reference stay.
PurgeTarget ServiceCore::purgeTarget(const QUuid &subject) const
{
    PurgeTarget t;
    for (int i = 0; i < m_config.connections.size(); ++i) {
        const ConnectionConfig &c = m_config.connections.at(i);
        bool serves = false;
        for (int j = 0; j < c.subjects.size(); ++j) {
            if (c.subjects.at(j).id != subject)
                continue;
            serves = true;
            if (!t.subjectRefs.contains(c.subjects.at(j).subjectRef))
                t.subjectRefs << c.subjects.at(j).subjectRef;
        }
        if (!serves)
            continue;
        t.known = true;
        if (c.subjects.size() == 1)
            t.soleConnections << c.id;
    }
    t.label = m_config.subjectLabels.value(subject);
    return t;
}

QStringList ServiceCore::connectionIds() const
{
    QStringList ids;
    for (int i = 0; i < m_config.connections.size(); ++i)
        ids << m_config.connections.at(i).id;
    return ids;
}

// The UI changed the intervals (D-Bus IntervalsChanged): re-read the file and apply the new
// intervals of that connection. Adding or removing connections or Subjects is not applied
// while running; that needs a restart of the service.
void ServiceCore::configurationChanged(const QString &connectionId)
{
    if (!m_open || !QFile::exists(m_options.configPath))
        return;
    const ServiceConfig fresh = loadServiceConfig(m_options.configPath);
    if (!fresh.ok)
        return;     // keep running with what was valid at start
    for (int i = 0; i < fresh.connections.size(); ++i) {
        const ConnectionConfig &c = fresh.connections.at(i);
        if (c.id != connectionId)
            continue;
        for (int j = 0; j < m_config.connections.size(); ++j)
            if (m_config.connections.at(j).id == connectionId) {
                ConnectionConfig merged = m_config.connections.at(j);
                merged.currentIntervalMs = c.currentIntervalMs;
                merged.graphIntervalMs = c.graphIntervalMs;
                merged.logbookIntervalMs = c.logbookIntervalMs;
                m_config.connections[j] = merged;
                m_scheduler->intervalsChanged(connectionId, merged);
            }
    }
}

// What the alert engine needs about one Subject from the configuration file: the
// connection's `current` interval for the staleness, and the Subject's own persistence and missing-data
// times when the file has them (`alert_persistence_low_s`, `alert_persistence_high_s`, `missing_limit_s`,
// whole seconds), else the engine's defaults. The values are not clamped here: the engine clamps them to
// its range (AlertEngine::sanitized), the one place that range is defined, and the same numbers are
// reported by effectiveAlertKeys() for the thresholds page.
AlertSubjectSettings ServiceCore::alertSettingsFor(const ConnectionConfig &c, const QUuid &subject) const
{
    AlertSubjectSettings s;
    s.currentSource = c.adapterId + QStringLiteral(".current");
    s.currentIntervalMs = c.currentIntervalMs >= 0 ? c.currentIntervalMs : SchedulerSettings().currentIntervalMs;
    s.staleFactor = SchedulerSettings().staleFactor;
    s.persistenceLowMs = m_options.alerts.persistenceLowDefaultMs;
    s.persistenceHighMs = m_options.alerts.persistenceHighDefaultMs;
    s.missingLimitMs = m_options.alerts.missingLimitDefaultMs;
    const QHash<QUuid, SubjectAlertKeys>::const_iterator k = m_config.subjectAlertKeys.constFind(subject);
    if (k != m_config.subjectAlertKeys.constEnd()) {
        if (k->persistenceLowS >= 0) s.persistenceLowMs = k->persistenceLowS * 1000;
        if (k->persistenceHighS >= 0) s.persistenceHighMs = k->persistenceHighS * 1000;
        if (k->missingLimitS >= 0) s.missingLimitMs = k->missingLimitS * 1000;
    }
    return s;
}

ServiceCore::AlertKeyValues ServiceCore::alertKeysInForce(const QUuid &subject) const
{
    ConnectionConfig conn;
    for (int i = 0; i < m_config.connections.size(); ++i)
        for (int j = 0; j < m_config.connections.at(i).subjects.size(); ++j)
            if (m_config.connections.at(i).subjects.at(j).id == subject)
                conn = m_config.connections.at(i);
    const AlertSubjectSettings s = alertSettingsFor(conn, subject);
    const AlertSettings &r = m_options.alerts;
    AlertKeyValues v;
    // The same clamping as AlertEngine::sanitized, so that the page sees what the engine uses.
    v.persistenceLowS = qBound<qint64>(0, s.persistenceLowMs, r.persistenceLowMaxMs) / 1000;
    v.persistenceHighS = qBound<qint64>(0, s.persistenceHighMs, r.persistenceHighMaxMs) / 1000;
    v.missingLimitS = qBound(r.missingLimitMinMs, s.missingLimitMs, r.missingLimitMaxMs) / 1000;
    return v;
}

// A file read at ConfigChanged: the intervals of known connections are applied at once; any
// other difference (a connection or a Subject added, removed or pointing at another source
// reference) cannot be applied to a running scheduler, so the caller is told to restart the
// service. The comparison looks at ids and references only, never prints them.
QString ServiceCore::reloadConfiguration()
{
    if (!m_open)
        return QStringLiteral("config-invalid");
    ServiceConfig fresh;
    if (QFile::exists(m_options.configPath)) {
        fresh = loadServiceConfig(m_options.configPath);
        if (!fresh.ok)
            return QStringLiteral("config-invalid");
    } else {
        fresh.ok = true;
    }

    bool restart = fresh.connections.size() != m_config.connections.size();
    for (int i = 0; !restart && i < fresh.connections.size(); ++i) {
        const ConnectionConfig &f = fresh.connections.at(i);
        const ConnectionConfig old = connectionConfig(f.id);
        if (old.id != f.id || old.adapterId != f.adapterId || old.subjects.size() != f.subjects.size()) {
            restart = true;
            break;
        }
        for (int j = 0; j < f.subjects.size(); ++j)
            if (old.subjects.at(j).id != f.subjects.at(j).id
                || old.subjects.at(j).subjectRef != f.subjects.at(j).subjectRef)
                restart = true;
    }
    if (restart)
        return QStringLiteral("restart-needed");
    for (int i = 0; i < fresh.connections.size(); ++i)
        configurationChanged(fresh.connections.at(i).id);
    // A new label takes effect at the next notification; nothing else about it needs a restart.
    m_config.subjectOrder = fresh.subjectOrder;
    m_config.subjectLabels = fresh.subjectLabels;
    m_config.connectionLabels = fresh.connectionLabels;
    m_config.language = fresh.language;
    m_config.alertsShowValue = fresh.alertsShowValue;
    m_config.subjectAlertKeys = fresh.subjectAlertKeys;
    // The per-Subject alert times take effect now, without a restart (a key removed from the file brings
    // the default back). The engine judges again only for a value that really changed.
    if (m_alerts) {
        for (int i = 0; i < m_config.connections.size(); ++i) {
            const ConnectionConfig &c = m_config.connections.at(i);
            for (int j = 0; j < c.subjects.size(); ++j)
                m_alerts->updateSubjectSettings(c.subjects.at(j).id, alertSettingsFor(c, c.subjects.at(j).id));
        }
    }
    publishLabels();
    applyNotificationSettings();
    return QString();
}

// The language of the notification texts and the privacy setting come from the configuration file
// and are applied at start and at every ConfigChanged, without a restart (REQ-ALR-10).
void ServiceCore::applyNotificationSettings()
{
    if (!m_platform.presenter)
        return;
    if (!m_options.translationsDir.isEmpty())
        m_translator.load(m_options.translationsDir, m_options.translationBase, m_config.language);
    const QLocale locale = m_config.language.isEmpty() ? QLocale::system() : QLocale(m_config.language);
    m_platform.presenter->setTexts(NotificationTexts(NotificationTexts::TimeFormat(), locale));
    m_platform.presenter->setShowValue(m_config.alertsShowValue);
}

// The label the guard notice uses for a connection: the optional `label` of the connection in the
// configuration file, else its opaque id. Never an account name or an e-mail address.
// The facts of the moment for the resident notice: the alerts that are active or acknowledged (a
// snoozed one is not listed), in the order of the rank and then by label; the Subjects in silent
// mode; Do not disturb; the number of Subjects. Labels and level words only, never a value.
ResidentInput ServiceCore::residentFacts() const
{
    ResidentInput in;
    in.patients = m_config.subjectOrder.size();
    in.doNotDisturb = m_doNotDisturb;
    if (m_alerts) {
        const QVector<AlertView> views = m_alerts->alerts();
        QList<ResidentAlert> list;
        for (int i = 0; i < views.size(); ++i) {
            const AlertView &v = views.at(i);
            if (v.state != AlertState::Active && v.state != AlertState::Acknowledged)
                continue;
            ResidentAlert a;
            a.kind = NotificationCatalog::alarmKindOf(v.type);
            a.label = m_platform.labels ? m_platform.labels->labelOf(v.subject) : QStringLiteral("?");
            a.acknowledged = v.state == AlertState::Acknowledged;
            list << a;
        }
        std::stable_sort(list.begin(), list.end(), [](const ResidentAlert &x, const ResidentAlert &y) {
            const int rx = NotificationCatalog::rankOf(x.kind), ry = NotificationCatalog::rankOf(y.kind);
            return rx != ry ? rx < ry : x.label < y.label;
        });
        in.alerts = list;
    }
#ifdef LIBREMON_DEVSOURCE
    // The synthetic alerts of the developer feed, listed and sorted with the real ones.
    if (m_devFeed) {
        in.alerts += m_devFeed->residentAlerts();
        std::stable_sort(in.alerts.begin(), in.alerts.end(), [](const ResidentAlert &x, const ResidentAlert &y) {
            const int rx = NotificationCatalog::rankOf(x.kind), ry = NotificationCatalog::rankOf(y.kind);
            return rx != ry ? rx < ry : x.label < y.label;
        });
        in.silentLabels += m_devFeed->silentLabels();
    }
#endif
    if (const_cast<ServiceCore *>(this)->silentControl())
        for (int i = 0; i < m_config.subjectOrder.size(); ++i)
            if (const_cast<ServiceCore *>(this)->silentControl()->isOn(m_config.subjectOrder.at(i)))
                in.silentLabels << (m_platform.labels ? m_platform.labels->labelOf(m_config.subjectOrder.at(i)) : QStringLiteral("?"));
    return in;
}

QString ServiceCore::connectionLabel(const QString &connection) const
{
    const QString label = m_config.connectionLabels.value(connection);
    return label.isEmpty() ? connection : label;
}

void ServiceCore::publishLabels()
{
    if (m_platform.labels)
        m_platform.labels->set(m_config.subjectOrder, m_config.subjectLabels);
}

} // namespace lm
