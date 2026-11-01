// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Michele Brami
#include "realbackend.h"

#include "daemon/dbusservice.h"
#include "ui/uiconstants.h"

namespace lm {
namespace real {

namespace {
// Signals of the service that arrive in bursts (a poll changes state, staleness and data within a
// moment) become one request after this pause.
const int kStatusCoalesceMs = 200;
const int kDataCoalesceMs = 500;
}

RealBackend::RealBackend(ServiceLink *link, const QString &configPath, const QString &storePath,
                         const QString &journalPath, QObject *parent)
    : ui::Backend(parent), m_link(link), m_store(storePath, journalPath), m_configPath(configPath),
      m_haveReport(false), m_answered(false), m_reachable(false), m_callFailed(false), m_olderByCall(false)
{
    m_statusTimer.setSingleShot(true);
    m_statusTimer.setInterval(kStatusCoalesceMs);
    m_dataTimer.setSingleShot(true);
    m_dataTimer.setInterval(kDataCoalesceMs);
    connect(&m_statusTimer, &QTimer::timeout, this, &RealBackend::flushStatus);
    connect(&m_dataTimer, &QTimer::timeout, this, &RealBackend::flushData);

    connect(m_link, &ServiceLink::startAnswered, this, &RealBackend::onStartAnswered);
    connect(m_link, &ServiceLink::statusAnswered, this, &RealBackend::onStatusAnswered);
    connect(m_link, &ServiceLink::reachableChanged, this, &RealBackend::onReachableChanged);
    connect(m_link, &ServiceLink::callFailed, this, &RealBackend::onCallFailed);
    connect(m_link, &ServiceLink::dataChanged, this, &RealBackend::onDataChanged);
    // Any state, staleness or warning signal means the picture changed: ask for it once.
    // The arguments of the signals are not used: any of them only means "the picture changed", and the
    // one Status() call after the pause tells everything (the connection of StateChanged, is
    // there for tools that act on one connection).
    connect(m_link, &ServiceLink::stateChanged, this, [this](const QString &, const QString &, const QString &, const QString &) { onStatusSignal(); });
    connect(m_link, &ServiceLink::stalenessChanged, this, [this](const QString &, bool, const QString &) { onStatusSignal(); });
    connect(m_link, &ServiceLink::warning, this, [this](const QString &, const QString &, const QString &) { onStatusSignal(); });
    connect(m_link, &ServiceLink::diagnosticReportProposed, this, [this](const QString &) { onStatusSignal(); });
    connect(&m_store, &StoreWorker::subjectLoaded, this, &RealBackend::onSubjectLoaded);
    connect(&m_store, &StoreWorker::gapsLoaded, this, &RealBackend::onGapsLoaded);
    connect(&m_store, &StoreWorker::thresholdHistoryLoaded, this, &ui::Backend::thresholdHistoryLoaded);
}

qint64 RealBackend::windowFromMs() const
{
    // The plot can show at most kUiZoomMaxMs: that is as far back as anything is loaded.
    return nowMs() - ui::kUiZoomMaxMs;
}

void RealBackend::begin()
{
    m_config.load(m_configPath);
    // REQ-UI-11: opening the UI starts the service if it is not running. A cold start of the UI
    // does not start it by itself, this call does.
    m_link->start();
    loadAll();
    emit changed();
}

void RealBackend::reloadConfiguration()
{
    m_config.load(m_configPath);
    loadAll();
    emit changed();
}

void RealBackend::loadAll()
{
    const QList<ConfigSubject> subs = configSubjects();
    for (int i = 0; i < subs.size(); ++i)
        m_store.load(subs.at(i).id, windowFromMs());
    m_store.loadGaps();
}

void RealBackend::refreshAll()
{
    // Not polling: this runs when the window comes back to the front, to catch up on signals missed
    // while the UI was in the background.
    m_link->refreshStatus();
    const QList<ConfigSubject> subs = configSubjects();
    for (int i = 0; i < subs.size(); ++i)
        m_store.refresh(subs.at(i).id, windowFromMs());
    m_store.loadGaps();
}

QList<ConfigSubject> RealBackend::configSubjects() const
{
    QList<ConfigSubject> out;
    const QList<ui::ConnectionEntry> conns = m_config.connections();
    for (int i = 0; i < conns.size(); ++i)
        for (int j = 0; j < conns.at(i).subjects.size(); ++j) {
            const ui::SubjectEntry &s = conns.at(i).subjects.at(j);
            ConfigSubject c;
            c.id = s.id;
            c.name = s.name;
            c.label = s.label;
            c.connection = conns.at(i).id;
            out.append(c);
        }
    return out;
}

QList<ui::SubjectSnapshot> RealBackend::subjects() const
{
    return buildSnapshots(configSubjects(), m_haveReport && m_reachable ? &m_report : 0, m_data, m_gaps, nowMs());
}

ui::ServiceStatus RealBackend::serviceStatus() const
{
    const ServiceReport *report = m_haveReport ? &m_report : 0;
    // A newer version wins over an "unknown method" (a newer service may have dropped a method); else an
    // "unknown method" means an older build even when the versions look the same.
    VersionRelation relation = serviceVersionRelation(report, m_ownVersion);
    if (m_olderByCall && relation != VersionRelation::Newer)
        relation = VersionRelation::Older;
    return serviceStatusOf(report, m_answered, m_reachable, m_callFailed, relation);
}

void RealBackend::setOwnVersion(const QString &version)
{
    m_ownVersion = version;
    emit changed();
}

// the client says why a call failed just before the call's answer. "Unknown method" (or interface,
// or object) means a service is there but does not have what this window calls: an older build. Any
// method counts (Alerts is the usual one, from a 0.1-1 service); the other errors (no service, no reply)
// are not about the version and change nothing here. Only names are read, never logged.
void RealBackend::onCallFailed(const QString &, const QString &errorName)
{
    if (!ServiceClient::isOlderServiceError(errorName) || m_olderByCall)
        return;
    m_olderByCall = true;
    emit changed();
}

qint64 RealBackend::lastHeartbeatMs() const
{
    return m_haveReport && m_report.lastHeartbeatMs > 0 ? m_report.lastHeartbeatMs : 0;
}

void RealBackend::onStartAnswered(bool ok, const QString &)
{
    if (!ok) {
        // The call failed (the service could not be started or did not answer): say so, do not guess.
        m_callFailed = true;
        m_answered = true;
        emit changed();
        return;
    }
    m_callFailed = false;
    m_link->refreshStatus();
}

void RealBackend::onStatusAnswered(bool ok, const QByteArray &json)
{
    m_answered = true;
    if (!ok) {
        m_callFailed = true;
        emit changed();
        return;
    }
    const ServiceReport r = parseServiceStatus(json);
    if (!r.ok) {
        // An answer this version cannot read (a newer service): treat as a failed call, so the strip
        // says so instead of showing a guess (docs 8.3).
        m_callFailed = true;
        emit changed();
        return;
    }
    m_callFailed = false;
    m_report = r;
    m_haveReport = true;
    m_reachable = true;
    emit changed();
}

void RealBackend::onReachableChanged(bool reachable)
{
    m_reachable = reachable;
    // The process that answered "unknown method" is gone with its name: the next one is judged afresh
    // (by its Status() and by the next call).
    if (!reachable)
        m_olderByCall = false;
    if (reachable) {
        m_link->refreshStatus();        // the service (re)appeared: catch up
        loadAll();
    }
    emit changed();
}

void RealBackend::onSubjectLoaded(const QString &id, const SubjectData &data)
{
    m_data.insert(id, data);
    emit changed();
}

void RealBackend::onGapsLoaded(const QVector<QPair<qint64, qint64> > &gaps)
{
    m_gaps = gaps;
    emit changed();
}

void RealBackend::onDataChanged(const QString &subject)
{
    if (!m_dirty.contains(subject))
        m_dirty.append(subject);
    if (!m_dataTimer.isActive())
        m_dataTimer.start();
}

void RealBackend::onStatusSignal()
{
    if (!m_statusTimer.isActive())
        m_statusTimer.start();
}

void RealBackend::flushStatus() { m_link->refreshStatus(); }

void RealBackend::flushData()
{
    const QStringList dirty = m_dirty;
    m_dirty.clear();
    for (int i = 0; i < dirty.size(); ++i)
        m_store.refresh(dirty.at(i), windowFromMs());
    m_store.loadGaps();
}

void RealBackend::retry(const QString &subjectId)
{
    const QList<ConfigSubject> subs = configSubjects();
    for (int i = 0; i < subs.size(); ++i)
        if (subs.at(i).id == subjectId) {
            m_link->retry(subs.at(i).connection);       // REQ-GRD-14
            return;
        }
}

void RealBackend::reenable(const QString &subjectId)
{
    const SubjectReport *r = m_haveReport ? m_report.subject(subjectId) : 0;
    QString connection;
    const QList<ConfigSubject> subs = configSubjects();
    for (int i = 0; i < subs.size(); ++i)
        if (subs.at(i).id == subjectId)
            connection = subs.at(i).connection;
    if (connection.isEmpty())
        return;
    // A stopped adapter VERSION is re-enabled by version (REQ-GRD-08); the connection
    // otherwise. The adapter and version come with the richer Status(); without them the
    // connection is re-enabled.
    if (r && r->cause == QLatin1String("adapter-version") && !r->adapter.isEmpty() && !r->adapterVersion.isEmpty())
        m_link->reenableVersion(r->adapter, r->adapterVersion);
    else
        m_link->reenable(connection);
}

void RealBackend::reportHandled(const QString &subjectId)
{
    const QList<ConfigSubject> subs = configSubjects();
    for (int i = 0; i < subs.size(); ++i)
        if (subs.at(i).id == subjectId) {
            m_link->reportHandled(subs.at(i).connection);
            m_link->refreshStatus();            // report_proposed is cleared by the service
            return;
        }
}

void RealBackend::startMonitoring() { m_link->start(); }

// Only the Stop button of the "older service" strip calls this: the user stops the old
// process; its name then loses its holder, the strip shows "not running" with Start, and Start activates the
// installed (new) binary. Nothing here starts it again by itself.
void RealBackend::stopMonitoring() { m_link->stop(); }

// Read from the journal in the store's thread, read-only; asked when the thresholds page opens
// and after an accepted edit, never on a timer.
void RealBackend::requestThresholdHistory(const QString &subjectId) { m_store.loadThresholdHistory(subjectId); }

void RealBackend::askAgain()
{
    // A call to the service re-raises the system approval dialog while a connection waits for it
    // (: `Start` does).
    m_link->start();
}

} // namespace real
} // namespace lm
