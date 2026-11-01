// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Michele Brami
#include "discovery.h"

#include "guard/guard.h"
#include "guard/ingestlog.h"
#include "guard/statelog.h"
#include "session/adapterregistry.h"

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QTimer>

namespace lm {

namespace {

// A discovery writes no ingest-log entries of its own: the real log belongs to the store and
// to real connections. The run's guard needs a sink to log into, so it gets one that drops.
class DroppingIngestSink : public IngestLogSink
{
public:
    bool append(const IngestLogEntry &) { return true; }
};

// The session blob of a known connection is offered to the adapter, so that it can reuse the
// live login instead of making a new one, unless it is about to end (then a fresh login is
// what the scheduler would make too).
const qint64 kSessionMinRemainingMs = 60 * 1000;

QString classOfOutcome(const CommandOutcome &o)
{
    // A kill by the guard or by the driver's own line limit is the adapter misbehaving, not
    // the source answering: it reads as a changed protocol, whatever class the driver set.
    if (o.killedBy == CommandOutcome::Guard || o.killedBy == CommandOutcome::DriverLimit)
        return QStringLiteral("protocol-changed");
    switch (o.errorClass) {
    case ErrorClass::AuthFailed: return QStringLiteral("auth-failed");
    case ErrorClass::TermsMustBeAccepted: return QStringLiteral("terms-must-be-accepted");
    case ErrorClass::RateLimited: return QStringLiteral("rate-limited");
    case ErrorClass::Transient: return QStringLiteral("unreachable");
    case ErrorClass::ProtocolChanged: return QStringLiteral("protocol-changed");
    case ErrorClass::InvalidCommand: break;
    }
    return QStringLiteral("internal");      // a defect of the core, not something the user can fix
}

QString classOfRefusal(const StartResult &r)
{
    switch (r.refusal) {
    case StartResult::Denied:
        if (r.denyReason == QLatin1String("circuit-open") || r.denyReason == QLatin1String("spawn-rate"))
            return QStringLiteral("rate-limited");
        return QStringLiteral("stopped-by-guard");
    case StartResult::AdmissionRejected:
        return QStringLiteral("protocol-changed");
    case StartResult::SpawnFailed:
    case StartResult::HelloTimeout:
    case StartResult::None:
    default:
        return QStringLiteral("unreachable");
    }
}

} // namespace

// ---- one run ------------------------------------------------------------------------------

class DiscoveryService::Run : public QObject
{
public:
    Run(DiscoveryService *service, const QString &key, bool probe, const ConnectionConfig &config,
        const Done &done)
        : QObject(service), m_owner(service), m_key(key), m_probe(probe), m_config(config), m_done(done),
          m_keeper(&m_log, service->m_p.clock), m_ingest(&m_sink, 16),
          m_guard(service->m_p.clock, &m_keeper, &m_ingest), m_link(0), m_finished(false)
    {
        m_keeper.load();
        m_watchdog.setSingleShot(true);
        QObject::connect(&m_watchdog, &QTimer::timeout, this, [this]() {
            finish(DiscoveryService::errorAnswer(QStringLiteral("unreachable")));
        });
    }

    const QString &key() const { return m_key; }

    void begin()
    {
        m_watchdog.start(int(m_owner->m_s.timeoutMs));
        const QString secretId = m_probe ? probeConnectionId() : m_config.id;
        m_owner->m_p.secrets->load(secretId, [this](const SecretLoad &load) { loaded(load); });
    }

    // Answers `errorClass` and ends the adapter process at once (the stop sequence, or the
    // holder going away). Safe to call after the run has finished.
    void cancel(const QString &errorClass)
    {
        finish(DiscoveryService::errorAnswer(errorClass));
        delete m_link;      // the destructor kills the process; no callback can follow
        m_link = 0;
    }

private:
    void loaded(const SecretLoad &load)
    {
        if (m_finished)
            return;
        switch (load.kind) {
        case SecretLoad::Ok:
            m_creds = load.credentials;
            // A known connection offers its stored session; the probe never has one.
            if (!m_probe && !load.session.isEmpty()
                && (!load.expiresAt.isValid()
                    || load.expiresAt.msecsSinceEpoch() > m_owner->m_p.clock->now().msecsSinceEpoch()
                                                           + kSessionMinRemainingMs))
                m_session = load.session;
            // The probe's login is only needed in memory from here on: take it out of the
            // secret store at the earliest moment, so it is gone whatever happens next
            // A failure to remove is not fatal; the next start tries again.
            if (m_probe)
                m_owner->removeProbeSecret();
            startLink();
            return;
        case SecretLoad::Missing:
            finish(DiscoveryService::errorAnswer(QStringLiteral("credentials-missing")));
            return;
        case SecretLoad::Locked:
            finish(DiscoveryService::errorAnswer(QStringLiteral("secrets-locked")));
            return;
        case SecretLoad::Error:
        default:
            finish(DiscoveryService::errorAnswer(QStringLiteral("internal")));
            return;
        }
    }

    void startLink()
    {
        const QString adapterId = m_probe ? QStringLiteral("libre") : m_config.adapterId;
        m_link = m_owner->m_p.makeLink(adapterId, &m_guard);
        if (!m_link) {
            wipe();
            finish(DiscoveryService::errorAnswer(QStringLiteral("internal")));
            return;
        }
        m_link->setParent(this);
        const QJsonObject settings = m_probe || !m_owner->m_p.settings ? QJsonObject()
                                                                      : m_owner->m_p.settings(m_config.id);
        m_link->start(adapterId, m_key, settings, [this](const StartResult &result, const QString &) {
            if (m_finished)
                return;
            if (result.kind != StartResult::Admitted) {
                wipe();
                finish(DiscoveryService::errorAnswer(classOfRefusal(result)));
                return;
            }
            // The credentials go to the adapter in this one call and are wiped here at once.
            const Credentials creds = m_creds;
            const QString session = m_session;
            wipe();
            m_link->authenticate(creds, session, [this](const CommandReport &report, const SessionGrant &) {
                // The grant is dropped on purpose: a discovery keeps no session and a
                // known connection's stored session is the scheduler's to renew.
                if (m_finished)
                    return;
                if (report.outcome.result != CommandOutcome::Ok) {
                    finish(DiscoveryService::errorAnswer(classOfOutcome(report.outcome)));
                    return;
                }
                m_guard.beginCommand(m_link->linkId(), QStringLiteral("discover_subjects"), 1, 1.0);
                m_link->discoverSubjects([this](const CommandReport &r, const QList<SubjectInfo> &subjects) {
                    if (m_finished)
                        return;
                    if (r.outcome.result != CommandOutcome::Ok) {
                        finish(DiscoveryService::errorAnswer(classOfOutcome(r.outcome)));
                        return;
                    }
                    finish(DiscoveryService::subjectsAnswer(subjects));
                });
            });
        });
    }

    void wipe()
    {
        // Best effort, as in the session manager: this object does not keep the secret longer
        // than the call that needs it.
        m_creds.username.fill(QLatin1Char('\0'));
        m_creds.password.fill(QLatin1Char('\0'));
        m_creds = Credentials();
        m_session.fill(QLatin1Char('\0'));
        m_session.clear();
    }

    void finish(const QString &json)
    {
        if (m_finished)
            return;
        m_finished = true;
        m_watchdog.stop();
        wipe();
        const Done done = m_done;
        m_done = Done();
        m_owner->forget(this);
        // The process ends politely (`shutdown`, then killed after the policy's wait). The run
        // is deleted when that is done, or after a safety delay, never inside a link callback.
        if (m_link)
            m_link->stop([this]() { deleteLater(); });
        else
            deleteLater();
        QTimer::singleShot(10000, this, SLOT(deleteLater()));
        if (done)
            done(json);
    }

    DiscoveryService *m_owner;
    QString m_key;
    bool m_probe;
    ConnectionConfig m_config;
    Done m_done;

    // The run's own guard and what it needs, in construction order (see the file comment).
    MemoryStateLog m_log;
    StateKeeper m_keeper;
    DroppingIngestSink m_sink;
    IngestLog m_ingest;
    Guard m_guard;

    AdapterLink *m_link;
    Credentials m_creds;
    QString m_session;
    QTimer m_watchdog;
    bool m_finished;
};

// ---- the service --------------------------------------------------------------------------

DiscoveryService::DiscoveryService(const DiscoveryPorts &ports, const DiscoverySettings &settings,
                                   QObject *parent)
    : QObject(parent), m_p(ports), m_s(settings), m_cleanPending(false), m_nextCleanMs(0),
      m_destroying(false)
{
}

DiscoveryService::~DiscoveryService()
{
    // The runs are children of this object and go with it; each destructor ends its process.
    m_destroying = true;
}

QString DiscoveryService::errorAnswer(const QString &errorClass)
{
    QJsonObject o;
    o.insert(QStringLiteral("ok"), false);
    o.insert(QStringLiteral("error"), errorClass);
    return QString::fromUtf8(QJsonDocument(o).toJson(QJsonDocument::Compact));
}

QString DiscoveryService::subjectsAnswer(const QList<SubjectInfo> &subjects)
{
    QJsonArray list;
    for (int i = 0; i < subjects.size(); ++i) {
        QJsonObject s;
        s.insert(QStringLiteral("subject_ref"), subjects.at(i).subjectRef);
        s.insert(QStringLiteral("display_name"), subjects.at(i).displayName);
        s.insert(QStringLiteral("unit"), subjects.at(i).unit);             // "" = undetermined
        s.insert(QStringLiteral("unit_basis"), subjects.at(i).unitBasis);
        list.append(s);
    }
    QJsonObject o;
    o.insert(QStringLiteral("ok"), true);
    o.insert(QStringLiteral("subjects"), list);
    return QString::fromUtf8(QJsonDocument(o).toJson(QJsonDocument::Compact));
}

// Only starts that were really sent to the source count: an answer that never reached it
// (unknown connection, stopped by the guard) must not eat the allowance.
bool DiscoveryService::admit(const QString &key)
{
    const qint64 now = m_p.clock->now().msecsSinceEpoch();
    QList<qint64> &starts = m_starts[key];
    while (!starts.isEmpty() && starts.first() <= now - 3600LL * 1000)
        starts.removeFirst();
    if (!starts.isEmpty() && now - starts.last() < m_s.minSpacingMs)
        return false;
    return starts.size() < m_s.maxPerHour;
}

void DiscoveryService::note(const QString &key)
{
    m_starts[key].append(m_p.clock->now().msecsSinceEpoch());
}

void DiscoveryService::forget(Run *run)
{
    m_runs.removeAll(run);
}

void DiscoveryService::removeProbeSecret()
{
    if (!m_p.writer)
        return;
    QString ignored;
    m_p.writer->remove(probeConnectionId(), &ignored);
}

void DiscoveryService::discover(const QString &connection, const Done &done)
{
    const bool probe = connection == probeConnectionId();
    // Every refusal below is answered from the event loop, never inside this call, so a caller
    // can rely on one order of events whatever the outcome.
    auto refuse = [this, probe, done](const QString &errorClass) {
        // A refused probe still takes its secret out: the UI stored it for this call.
        if (probe)
            removeProbeSecret();
        const QString json = errorAnswer(errorClass);
        QTimer::singleShot(0, this, [done, json]() { if (done) done(json); });
    };

    ConnectionConfig config;
    if (probe) {
        if (!m_p.writer) {          // the secret could not be taken out again: do not take it in
            refuse(QStringLiteral("internal"));
            return;
        }
    } else {
        config = m_p.connection ? m_p.connection(connection) : ConnectionConfig();
        if (connection.isEmpty() || config.id != connection) {
            refuse(QStringLiteral("unknown-connection"));
            return;
        }
    }

    // What the persistent state says is read before anything is sent to the source.
    const QString adapterId = probe ? QStringLiteral("libre") : config.adapterId;
    const AdapterEntry *entry = AdapterRegistry::instance().find(adapterId);
    if (!entry) {
        refuse(QStringLiteral("internal"));
        return;
    }
    if (m_p.state) {
        if ((!probe && m_p.state->connectionDisabled(config.id))
            || m_p.state->versionDisabled(entry->id, entry->version)) {
            refuse(QStringLiteral("stopped-by-guard"));
            return;
        }
    }

    // One at a time per key, and the allowance of the key (a login repeated in a loop
    // is a risk to the account).
    for (int i = 0; i < m_runs.size(); ++i)
        if (m_runs.at(i)->key() == connection) {
            refuse(QStringLiteral("rate-limited"));
            return;
        }
    if (!admit(connection)) {
        refuse(QStringLiteral("rate-limited"));
        return;
    }
    note(connection);

    Run *run = new Run(this, connection, probe, config, done);
    m_runs.append(run);
    // The secret store may answer at once; the run then finishes before begin() returns, which
    // is why the answer is posted and not called (see Run::finish and refuse above).
    QTimer::singleShot(0, run, [run]() { run->begin(); });
}

void DiscoveryService::cancelAll()
{
    const QList<Run *> runs = m_runs;
    for (int i = 0; i < runs.size(); ++i)
        runs.at(i)->cancel(QStringLiteral("service-stopping"));
}

// ---- the stale probe secret -----------------------------------------------------------

void DiscoveryService::cleanStaleProbe()
{
    m_cleanPending = true;
    m_nextCleanMs = m_p.clock->now().msecsSinceEpoch() + m_s.staleCleanRetryMs;
    tryCleanStale();
}

void DiscoveryService::tick()
{
    if (!m_cleanPending)
        return;
    const qint64 now = m_p.clock->now().msecsSinceEpoch();
    if (now < m_nextCleanMs)
        return;
    m_nextCleanMs = now + m_s.staleCleanRetryMs;
    tryCleanStale();
}

void DiscoveryService::tryCleanStale()
{
    if (!m_p.secrets || !m_p.writer) {
        m_cleanPending = false;
        return;
    }
    m_p.secrets->load(probeConnectionId(), [this](const SecretLoad &load) {
        switch (load.kind) {
        case SecretLoad::Missing:
            m_cleanPending = false;                 // nothing stale
            break;
        case SecretLoad::Ok: {
            QString error;
            if (m_p.writer->remove(probeConnectionId(), &error))
                m_cleanPending = false;             // otherwise it is tried again at a later tick
            break;
        }
        case SecretLoad::Locked:                    // after a phone restart: ask again later
        case SecretLoad::Error:
        default:
            break;
        }
    });
}

} // namespace lm
