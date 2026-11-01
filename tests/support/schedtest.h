// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Michele Brami
#ifndef LM_SCHEDTEST_H
#define LM_SCHEDTEST_H

// Fakes for the scheduler tests: a scripted link that behaves like the session
// driver towards the policy, an in-memory secret store, settings store and reading store, and
// a small model of the source (one reading a minute, a history feed of five-minute points
// that lags). Everything is deterministic and driven by a manual clock: a test advances the
// clock, calls tick() and then pump(), which stands in for the Qt event loop by completing
// the asynchronous operations the logic started. No real time passes and no process runs.
//
// The fake link calls the REAL guard (mayStart, onSpawned, onAdmitted, onBatch,
// onCommandFinished, onExited) in the order the driver does, so the guard and the scheduler
// are exercised together.

#include "guard/guard.h"
#include "guard/journalstatelog.h"
#include "modtest.h"
#include "scheduler/connectionsession.h"
#include "scheduler/scheduler.h"

#include <QSet>
#include <QtTest>

#include <deque>

namespace lm {
namespace testing {

class FakeWorld;

// What a scripted command does.
struct Script
{
    Script() : ok(true), errorClass(ErrorClass::Transient), retryAfterS(-1),
               killedBy(CommandOutcome::NotKilled), quarantined(0) {}

    bool ok;
    ErrorClass errorClass;
    int retryAfterS;
    CommandOutcome::KilledBy killedBy;
    // Batches delivered while the command runs: feed -> records.
    QList<QPair<QString, QVector<LinkRecord> > > batches;
    int quarantined;                    // records reported as rejected (counts only)
    FetchCompletion completion;        // for authenticate, `sensor` carries the settings_update
};

class FakeLink : public LinkPort
{
public:
    FakeLink(FakeWorld *world, const QString &connection);
    ~FakeLink();

    void start(const QString &adapterId, const QString &connectionId, const QJsonObject &settings,
               const StartCallback &done);
    void authenticate(const Credentials &credentials, const QString &session, const AuthCallback &done);
    void discoverSubjects(const DiscoverCallback &done);
    void fetchCurrent(const QString &subjectRef, const FetchCallback &done);
    void fetchRange(const QString &subjectRef, const UtcTime &from, const UtcTime &to,
                    const QStringList &feeds, const FetchCallback &done);
    void stop(const StopCallback &done);
    bool isAlive() const { return m_alive; }
    LinkId linkId() const { return m_id; }
    // Test hook: the process dies on its own, as a crash between polls would.
    void kill() { die(true); }

    // What the last authenticate was given (the secret is never kept beyond this test fake).
    QString lastSessionOffered;
    bool lastCredentialsWereGiven;

private:
    CommandOutcome baseOutcome(const QString &type) const;
    void runFetch(const QString &type, const QString &subjectRef, const UtcTime &from,
                  const UtcTime &to, const QStringList &feeds, const FetchCallback &done);
    void die(bool crashed);

    FakeWorld *m_world;
    LinkId m_id;
    bool m_alive;
    int m_commandN;
};

class FakeFactory : public LinkFactory
{
public:
    explicit FakeFactory(FakeWorld *world) : m_world(world), created(0) {}
    LinkPort *create(const QString &connectionId);

    int created;
    FakeLink *last() const { return m_last; }
private:
    FakeWorld *m_world;
    FakeLink *m_last = 0;
};

class FakeSecrets : public SecretStore
{
public:
    explicit FakeSecrets(FakeWorld *world) : m_world(world), kind(SecretLoad::Ok), loads(0) {
        username = QStringLiteral("caregiver@example.invalid");
        password = QStringLiteral("not-a-real-password-1234");
        session = QStringLiteral("opaque-token-ABCDEF");
    }
    void load(const QString &connectionId, const LoadCallback &done);
    void saveSession(const QString &, const QString &s, const UtcTime &e) { session = s; expires = e; ++saves; }
    void clearSession(const QString &) { session.clear(); expires = UtcTime(); ++clears; }

    SecretLoad::Kind kind;
    QString username, password, session;
    UtcTime expires;
    int loads = 0, saves = 0, clears = 0;
    QList<qint64> loadTimes;
private:
    FakeWorld *m_world;
};

class FakeSettings : public ConnectionSettingsStore
{
public:
    QJsonObject get(const QString &c) { return data.value(c); }
    void merge(const QString &c, const QJsonObject &s)
    {
        QJsonObject o = data.value(c);
        for (QJsonObject::const_iterator it = s.begin(); it != s.end(); ++it)
            o.insert(it.key(), it.value());
        data[c] = o;
    }
    QMap<QString, QJsonObject> data;
};

class FakeReadings : public ReadingStore
{
public:
    FakeReadings() : busy(false), ingestCalls(0) {}

    UtcTime newestMeasurement(const QUuid &subject, const QString &source)
    {
        qint64 best = -1;
        const QList<Record> &l = stored[subject.toString() + source];
        for (int i = 0; i < l.size(); ++i)
            if (l.at(i).measuredAt.msecsSinceEpoch() > best) best = l.at(i).measuredAt.msecsSinceEpoch();
        return best >= 0 ? UtcTime::fromMSecsSinceEpoch(best) : UtcTime();
    }
    int ingest(const QUuid &subject, const LinkId &, const QVector<Record> &records)
    {
        ++ingestCalls;
        if (busy) return -1;
        int added = 0;
        for (int i = 0; i < records.size(); ++i) {
            QList<Record> &l = stored[subject.toString() + records.at(i).source];
            bool dup = false;
            for (int k = 0; k < l.size(); ++k)
                if (l.at(k).measuredAt == records.at(i).measuredAt) dup = true;
            if (!dup) { l.append(records.at(i)); ++added; }
        }
        return added;
    }
    void quarantine(const LinkId &, const QString &reason, const QString &, const QJsonObject &)
    { quarantined.append(reason); }
    bool latestSensor(const QUuid &, UtcTime *a, int *w)
    {
        if (!sensorActivated.isValid()) return false;
        *a = sensorActivated; *w = sensorWarmup; return true;
    }
    void noteSensor(const QUuid &, const QJsonObject &s) { sensors.append(s); }

    int count(const QUuid &subject, const QString &source) const
    { return stored.value(subject.toString() + source).size(); }

    QMap<QString, QList<Record> > stored;
    QStringList quarantined;
    QList<QJsonObject> sensors;
    UtcTime sensorActivated;
    int sensorWarmup = 60;
    bool busy;
    int ingestCalls;
};

// Everything one test needs. Members are public: a test reaches into them on purpose.
class FakeWorld
{
public:
    FakeWorld()
        : factory(this), secrets(this), keeper(&stateLog, &clock),
          ingest(&sink, GuardSettings().ingestQueueMax), guard(&clock, &keeper, &ingest),
          scheduler(0), linksSeen(0)
    {
        keeper.load();
    }
    ~FakeWorld() { delete scheduler; }

    // Builds the scheduler over the fakes. Call after setting `settings` and before start().
    void build()
    {
        delete scheduler;
        SchedulerPorts p;
        p.clock = &clock;
        p.random = &random;
        p.links = &factory;
        p.secrets = &secrets;
        p.settings = &connSettings;
        p.readings = &readings;
        p.state = &keeper;
        p.guard = &guard;
        p.ingest = &ingest;
        scheduler = new Scheduler(p, settings);
    }

    // Completes asynchronous operations until none is left (the event loop of the tests).
    void pump()
    {
        int guardLoop = 0;
        while (!pending.empty() && ++guardLoop < 10000) {
            std::function<void()> f = pending.front();
            pending.pop_front();
            f();
        }
        QVERIFY2(guardLoop < 10000, "pump did not settle");
    }
    // One step of a simulation: tick and settle.
    void step() { scheduler->tick(); pump(); }
    // Moves to the next wake-up (at most to `endMs`) and runs it. False when nothing is due.
    bool advanceToNextWake(qint64 endMs)
    {
        const UtcTime w = scheduler->nextWakeAt();
        qint64 target = w.isValid() ? w.msecsSinceEpoch() : endMs;
        if (target > endMs) target = endMs;
        if (target > clock.ms()) clock.setMs(target);
        step();
        return clock.ms() < endMs;
    }
    void runUntil(qint64 endMs)
    {
        int guardLoop = 0;
        while (clock.ms() < endMs && ++guardLoop < 200000)
            if (!advanceToNextWake(endMs)) break;
    }

    // The source model --------------------------------------------------------------------
    // The newest `current` reading published at `nowMs`: stamped on the minute, visible 7 s
    // later (5 to 11 s).
    qint64 currentStamp(qint64 nowMs) const
    {
        const qint64 visible = nowMs - 7000;
        return (visible / 60000) * 60000;
    }
    LinkRecord sample(const QString &subjectRef, const QString &feed, qint64 stampMs, double value = 110) const
    {
        LinkRecord lr;
        lr.subjectRef = subjectRef;
        lr.record.shape = Record::Sample;
        lr.record.metric = QStringLiteral("glucose");
        lr.record.value = value;
        lr.record.unit = QStringLiteral("mg/dL");
        lr.record.measuredAt = UtcTime::fromMSecsSinceEpoch(stampMs);
        lr.record.originalTimestamp = QStringLiteral("synthetic");
        lr.record.source = QStringLiteral("libre.") + feed;
        return lr;
    }

    // The default behaviour of the source; a test replaces the functions to inject faults.
    std::function<Script(const QString &type, const QString &subjectRef, const UtcTime &from,
                         const UtcTime &to, const QStringList &feeds)> fetchScript;
    std::function<Script()> authScript;
    std::function<int()> startBehavior;     // 0 admit, 1 spawn failed, 2 hello timeout, 3 reject
    QString adapterVersion = QStringLiteral("0.1.0");
    qint64 grantLifetimeMs = 180LL * 24 * 3600 * 1000;    // the source's token lasts about 180 days
    qint64 tokenExpiryMs = 0;                              // 0 = no token issued yet

    Script defaultFetch(const QString &type, const QString &subjectRef, const UtcTime &from,
                        const UtcTime &to, const QStringList &feeds)
    {
        Script s;
        s.completion.watermark = UtcTime::fromMSecsSinceEpoch(clock.ms());
        if (type == QLatin1String("fetch_current")) {
            QVector<LinkRecord> v;
            v.append(sample(subjectRef, QStringLiteral("current"), currentStamp(clock.ms())));
            s.batches.append(qMakePair(QStringLiteral("current"), v));
        } else if (type == QLatin1String("fetch_range")) {
            const bool graph = feeds.contains(QStringLiteral("graph"));
            QVector<LinkRecord> v;
            if (graph) {
                // Five-minute points up to 19 minutes ago, inside [from, to].
                const qint64 newest = ((clock.ms() - 19 * 60000) / 300000) * 300000;
                for (qint64 t = newest; t >= clock.ms() - 12LL * 3600 * 1000; t -= 300000)
                    if (t >= from.msecsSinceEpoch() && t <= to.msecsSinceEpoch())
                        v.prepend(sample(subjectRef, QStringLiteral("graph"), t));
                s.batches.append(qMakePair(QStringLiteral("graph"), v));
            }
        }
        return s;
    }

    // Records which commands ran and when (for the minimum-interval checks).
    struct CommandLogEntry { qint64 startMs; qint64 endMs; QString type; QString connection; };
    QList<CommandLogEntry> commands;
    QList<qint64> authStarts;

    // Parts ---------------------------------------------------------------------------------
    ManualClock clock;
    ScriptedRandom random;
    FakeFactory factory;
    FakeSecrets secrets;
    FakeSettings connSettings;
    FakeReadings readings;
    MemoryStateLog stateLog;
    RecordingIngestSink sink;
    StateKeeper keeper;
    IngestLog ingest;
    Guard guard;
    SchedulerSettings settings;
    Scheduler *scheduler;
    std::deque<std::function<void()> > pending;
    int linksSeen;
    QList<FakeLink *> links;
};

// ---- implementation (header only, test code) ---------------------------------------------

inline FakeLink::FakeLink(FakeWorld *world, const QString &connection)
    : lastCredentialsWereGiven(false), m_world(world), m_alive(false), m_commandN(0)
{
    m_id.connectionId = connection;
    m_id.sessionId = QStringLiteral("sess-%1").arg(++world->linksSeen);
    world->links.append(this);
}

inline FakeLink::~FakeLink() { m_world->links.removeAll(this); }

inline LinkPort *FakeFactory::create(const QString &connectionId)
{
    ++created;
    m_last = new FakeLink(m_world, connectionId);
    return m_last;
}

inline void FakeSecrets::load(const QString &, const LoadCallback &done)
{
    ++loads;
    loadTimes.append(m_world->clock.ms());
    m_world->pending.push_back([this, done]() {
        SecretLoad l;
        l.kind = kind;
        if (kind == SecretLoad::Ok) {
            l.credentials.username = username;
            l.credentials.password = password;
            l.session = session;
            l.expiresAt = expires;
        }
        done(l);
    });
}

inline void FakeLink::start(const QString &adapterId, const QString &connectionId, const QJsonObject &,
                            const StartCallback &done)
{
    m_id.adapterId = adapterId;
    m_id.connectionId = connectionId;
    m_world->pending.push_back([this, done]() {
        StartResult r;
        r.link = m_id;
        StartRequest req;
        req.connectionId = m_id.connectionId;
        req.adapterId = m_id.adapterId;
        const MayStartResult may = m_world->guard.mayStart(req);
        if (may.kind == MayStartResult::Denied) {
            r.refusal = StartResult::Denied;
            switch (may.reason) {
            case MayStartResult::DisabledByGuard: r.denyReason = "disabled-by-guard"; break;
            case MayStartResult::AdapterVersionDisabled: r.denyReason = "adapter-version-disabled"; break;
            case MayStartResult::CircuitOpen: r.denyReason = "circuit-open"; break;
            default: r.denyReason = "spawn-rate"; break;
            }
            done(r, QString());
            return;
        }
        const int behavior = m_world->startBehavior ? m_world->startBehavior() : 0;
        if (behavior == 1) { r.refusal = StartResult::SpawnFailed; done(r, QString()); return; }
        m_world->guard.onSpawned(m_id);
        if (behavior == 2) { r.refusal = StartResult::HelloTimeout; done(r, QString()); return; }
        if (behavior == 3) { r.refusal = StartResult::AdmissionRejected; r.rejectReason = "metric"; done(r, QString()); return; }
        m_id.adapterVersion = m_world->adapterVersion;
        r.link = m_id;
        const MayStartResult again = m_world->guard.onAdmitted(m_id);
        if (again.kind == MayStartResult::Denied) {
            r.refusal = StartResult::Denied;
            r.denyReason = again.reason == MayStartResult::DisabledByGuard ? "disabled-by-guard"
                                                                           : "adapter-version-disabled";
            ExitStatus e;
            m_world->guard.onExited(m_id, e);
            done(r, QString());
            return;
        }
        m_alive = true;
        r.kind = StartResult::Admitted;
        done(r, QString());
    });
}

inline CommandOutcome FakeLink::baseOutcome(const QString &type) const
{
    CommandOutcome o;
    o.link = m_id;
    o.commandType = type;
    o.commandId = QStringLiteral("c%1").arg(m_commandN);
    return o;
}

inline void FakeLink::die(bool crashed)
{
    if (!m_alive) return;
    m_alive = false;
    ExitStatus e;
    e.crashed = crashed;
    m_world->guard.onExited(m_id, e);
}

inline void FakeLink::authenticate(const Credentials &credentials, const QString &session,
                                   const AuthCallback &done)
{
    lastSessionOffered = session;
    lastCredentialsWereGiven = !credentials.username.isEmpty() && !credentials.password.isEmpty();
    ++m_commandN;
    m_world->authStarts.append(m_world->clock.ms());
    m_world->pending.push_back([this, done]() {
        Script s = m_world->authScript ? m_world->authScript() : Script();
        CommandReport rep;
        rep.outcome = baseOutcome(QStringLiteral("authenticate"));
        SessionGrant g;
        if (s.ok) {
            // A login with a stored token reuses it unchanged, as the source does (the same
            // ticket comes back); a fresh login issues a new token.
            if (lastSessionOffered.isEmpty() || m_world->tokenExpiryMs == 0) {
                g.session = QStringLiteral("opaque-token-NEW");
                m_world->tokenExpiryMs = m_world->clock.ms() + m_world->grantLifetimeMs;
            } else {
                g.session = lastSessionOffered;
            }
            g.expiresAt = UtcTime::fromMSecsSinceEpoch(m_world->tokenExpiryMs);
            g.settingsUpdate = s.completion.sensor;       // reused as the settings_update carrier
        } else {
            rep.outcome.result = CommandOutcome::Error;
            rep.outcome.errorClass = s.errorClass;
            rep.outcome.retryAfterS = s.retryAfterS;
            rep.outcome.killedBy = s.killedBy;
            if (s.killedBy != CommandOutcome::NotKilled) die(false);
        }
        m_world->guard.onCommandFinished(rep.outcome);
        done(rep, g);
    });
}

inline void FakeLink::discoverSubjects(const DiscoverCallback &done)
{
    ++m_commandN;
    m_world->pending.push_back([this, done]() {
        CommandReport rep;
        rep.outcome = baseOutcome(QStringLiteral("discover_subjects"));
        m_world->guard.onCommandFinished(rep.outcome);
        done(rep, QList<SubjectInfo>());
    });
}

inline void FakeLink::fetchCurrent(const QString &subjectRef, const FetchCallback &done)
{
    runFetch(QStringLiteral("fetch_current"), subjectRef, UtcTime(), UtcTime(), QStringList(), done);
}

inline void FakeLink::fetchRange(const QString &subjectRef, const UtcTime &from, const UtcTime &to,
                                 const QStringList &feeds, const FetchCallback &done)
{
    runFetch(QStringLiteral("fetch_range"), subjectRef, from, to, feeds, done);
}

inline void FakeLink::runFetch(const QString &type, const QString &subjectRef, const UtcTime &from,
                               const UtcTime &to, const QStringList &feeds, const FetchCallback &done)
{
    ++m_commandN;
    const qint64 started = m_world->clock.ms();
    m_world->pending.push_back([=]() {
        Script s = m_world->fetchScript ? m_world->fetchScript(type, subjectRef, from, to, feeds)
                                        : m_world->defaultFetch(type, subjectRef, from, to, feeds);
        CommandReport rep;
        rep.outcome = baseOutcome(type);
        bool killed = false;
        if (s.ok) {
            for (int i = 0; i < s.batches.size() && !killed; ++i) {
                const Action a = m_world->guard.onBatch(m_id, s.batches.at(i).first,
                                                        s.batches.at(i).second.size(), 100);
                if (a.kind == Action::Kill) {
                    killed = true;
                } else if (a.kind != Action::Drop) {
                    m_world->scheduler->recordSink()->accept(m_id, s.batches.at(i).first, s.batches.at(i).second);
                    rep.outcome.recordsValid += s.batches.at(i).second.size();
                }
            }
            if (s.quarantined > 0)
                rep.outcome.quarantinedByReason.insert(int(Validation::ValueOutOfRange), s.quarantined);
        }
        if (killed) {
            rep.outcome.result = CommandOutcome::Error;
            rep.outcome.errorClass = ErrorClass::ProtocolChanged;
            rep.outcome.killedBy = CommandOutcome::Guard;
            die(false);
        } else if (!s.ok) {
            rep.outcome.result = CommandOutcome::Error;
            rep.outcome.errorClass = s.errorClass;
            rep.outcome.retryAfterS = s.retryAfterS;
            rep.outcome.killedBy = s.killedBy;
            if (s.killedBy != CommandOutcome::NotKilled) die(s.killedBy == CommandOutcome::Exit);
        }
        m_world->commands.append(FakeWorld::CommandLogEntry{ started, m_world->clock.ms(), type, m_id.connectionId });
        m_world->guard.onCommandFinished(rep.outcome);
        done(rep, s.completion);
    });
}

inline void FakeLink::stop(const StopCallback &done)
{
    m_world->pending.push_back([this, done]() {
        if (m_alive) {
            m_alive = false;
            ExitStatus e;
            m_world->guard.onExited(m_id, e);
        }
        if (done) done();
    });
}

} // namespace testing
} // namespace lm

#endif
