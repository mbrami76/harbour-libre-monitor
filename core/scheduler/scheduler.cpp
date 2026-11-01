// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Michele Brami
#include "scheduler/scheduler.h"

#include <QJsonValue>

namespace lm {

namespace {

const char *kFeedNames[kFeedCount] = { "current", "graph", "logbook" };

// How often a failed write (state log, ingest log, reading queue) is tried again.
const qint64 kRetryPersistMs = 5000;

QString subjectKey(const QString &connection, const QUuid &subject)
{
    // Attention that concerns one Subject only is kept under a key of its own, so that the
    // connection's own state is not touched.
    return connection + QLatin1Char('/') + subject.toString();
}

bool isSubjectCause(const QString &cause) { return cause == StateCause::subjectUnavailable(); }

} // namespace

QString scheduleStateName(ScheduleState s)
{
    switch (s) {
    case ScheduleState::Idle: return QStringLiteral("idle");
    case ScheduleState::Fetching: return QStringLiteral("fetching");
    case ScheduleState::Backoff: return QStringLiteral("backoff");
    case ScheduleState::NeedsAttention: return QStringLiteral("needs-attention");
    case ScheduleState::DisabledByGuard: return QStringLiteral("disabled-by-guard");
    }
    return QString();
}

QString feedName(int feed)
{
    return feed >= 0 && feed < kFeedCount ? QLatin1String(kFeedNames[feed]) : QString();
}

Scheduler::Scheduler(const SchedulerPorts &ports, const SchedulerSettings &settings)
    : m_ports(ports), m_s(settings), m_listener(0), m_recordSink(this), m_quarantineSink(this),
      m_queuedRecords(0), m_startedAtMs(0), m_serviceDownMs(-1), m_contactOkSinceStart(false),
      m_started(false)
{
    m_ports.guard->setListener(this);
}

Scheduler::~Scheduler()
{
    for (int i = 0; i < m_conns.size(); ++i) {
        for (int j = 0; j < m_conns.at(i)->subjects.size(); ++j)
            delete m_conns.at(i)->subjects.at(j);
        delete m_conns.at(i)->session;
        delete m_conns.at(i);
    }
}

void Scheduler::addConnection(const ConnectionConfig &config)
{
    Conn *c = new Conn;
    c->cfg = config;
    c->session = new ConnectionSession(config, m_ports, m_s);
    c->session->setListener(this);
    for (int i = 0; i < config.subjects.size(); ++i) {
        Subj *s = new Subj;
        s->cfg = config.subjects.at(i);
        s->conn = c;
        c->subjects.append(s);
    }
    m_conns.append(c);
}

void Scheduler::setCurrentAdapterVersion(const QString &adapterId, const QString &version)
{
    m_adapterVersions.insert(adapterId, version);
}

Scheduler::Conn *Scheduler::connection(const QString &id) const
{
    for (int i = 0; i < m_conns.size(); ++i)
        if (m_conns.at(i)->cfg.id == id)
            return m_conns.at(i);
    return 0;
}

Scheduler::Subj *Scheduler::subjectOf(const QUuid &id) const
{
    for (int i = 0; i < m_conns.size(); ++i)
        for (int j = 0; j < m_conns.at(i)->subjects.size(); ++j)
            if (m_conns.at(i)->subjects.at(j)->cfg.id == id)
                return m_conns.at(i)->subjects.at(j);
    return 0;
}

// ---- intervals ---------------------------------------------------------------------------

qint64 Scheduler::nominalMs(const Conn &c, int feed) const
{
    switch (feed) {
    case FeedCurrent: return c.cfg.currentIntervalMs >= 0 ? c.cfg.currentIntervalMs : m_s.currentIntervalMs;
    case FeedGraph: return c.cfg.graphIntervalMs >= 0 ? c.cfg.graphIntervalMs : m_s.graphIntervalMs;
    default: return c.cfg.logbookIntervalMs >= 0 ? c.cfg.logbookIntervalMs : m_s.logbookIntervalMs;
    }
}

qint64 Scheduler::effectiveMs(const Conn &c, const Subj &s, int feed, qint64 now) const
{
    return effectiveIntervalMs(nominalMs(c, feed), c.rateFloorMs, feed == FeedCurrent, now,
                               s.lastReadingMs, m_s);
}

// First run of a feed after a start or a re-enable: the planned time of the feed, but
// not before the settle time plus a random part of the spread, so connections do not start
// in step.
qint64 Scheduler::firstRunMs(const Conn &c, const Subj &s, int feed, qint64 now)
{
    if (nominalMs(c, feed) <= 0)
        return -1;                      // a feed with interval 0 is switched off (logbook)
    qint64 due = now + m_s.startSettleMs + qint64(m_ports.random->next() * double(m_s.startSpreadMs));
    if (feed == FeedCurrent && s.lastReadingMs >= 0) {
        const NextRun r = nextCurrentRun(now, effectiveMs(c, s, FeedCurrent, now), true,
                                         s.lastReadingMs, 0, m_s);
        if (r.atMs > due)
            due = r.atMs;
    }
    return due;
}

// ---- start and stop ----------------------------------------------------------------------

void Scheduler::start(const UtcTime &serviceDownSince)
{
    const qint64 now = nowMs();
    m_started = true;
    m_startedAtMs = now;
    m_contactOkSinceStart = false;
    m_serviceDownMs = serviceDownSince.isValid() ? now - serviceDownSince.msecsSinceEpoch() : -1;

    for (int i = 0; i < m_conns.size(); ++i) {
        Conn &c = *m_conns.at(i);
        const AttentionState att = m_ports.state->attention(c.cfg.id);
        const UtcTime hold = m_ports.state->holdUntil(c.cfg.id);

        // a connection stopped by `protocol-changed` tries once more after the adapter
        // changed (an update is the usual cure). Only when the service tells the version.
        bool probe = false;
        if (att.active() && att.cause == StateCause::protocolChanged()) {
            const QString recorded = m_ports.state->attentionVersion(c.cfg.id);
            const QString current = m_adapterVersions.value(c.cfg.adapterId);
            if (!recorded.isEmpty() && !current.isEmpty() && recorded != current) {
                m_ports.state->clearConnection(c.cfg.id, QStringLiteral("adapter version changed"));
                probe = true;
            }
        }

        for (int j = 0; j < c.subjects.size(); ++j) {
            Subj &s = *c.subjects.at(j);
            const UtcTime newest = m_ports.readings->newestMeasurement(
                s.cfg.id, c.cfg.adapterId + QLatin1Char('.') + feedName(FeedCurrent));
            s.lastReadingMs = newest.isValid() ? newest.msecsSinceEpoch() : -1;
            UtcTime activated;
            int warmup = -1;
            if (m_ports.readings->latestSensor(s.cfg.id, &activated, &warmup) && activated.isValid() && warmup >= 0)
                s.sensorReadyMs = activated.msecsSinceEpoch() + qint64(warmup) * 60 * 1000;
            s.staleSinceMs = now;

            const AttentionState subjAtt = m_ports.state->attention(subjectKey(c.cfg.id, s.cfg.id));
            if (m_ports.state->connectionDisabled(c.cfg.id)) {
                setState(s, ScheduleState::DisabledByGuard, QStringLiteral("connection"));
            } else if (att.active() && !probe) {
                setState(s, ScheduleState::NeedsAttention, att.cause);
            } else if (subjAtt.active()) {
                setState(s, ScheduleState::NeedsAttention, subjAtt.cause);
            } else if (hold.isValid() && hold.msecsSinceEpoch() > now) {
                setState(s, ScheduleState::Backoff, StateCause::rateLimited(), hold.msecsSinceEpoch());
            } else {
                for (int f = 0; f < kFeedCount; ++f)
                    s.nextDueMs[f] = firstRunMs(c, s, f, now);
                setState(s, ScheduleState::Idle, QString());
            }
            refreshStaleness(s, now);
        }
    }
}

void Scheduler::shutdown()
{
    for (int i = 0; i < m_conns.size(); ++i)
        m_conns.at(i)->session->shutdown();
    m_started = false;
}

// ---- the tick ----------------------------------------------------------------------------

void Scheduler::tick()
{
    if (!m_started)
        return;
    const qint64 now = nowMs();
    m_ports.state->retryPending();
    m_ports.ingest->flush();
    flushReadings();

    for (int i = 0; i < m_conns.size(); ++i) {
        Conn &c = *m_conns.at(i);
        c.session->tick(earliestDueMs(c));
        expireTimers(c, now);
        dispatch(c, now);
        for (int j = 0; j < c.subjects.size(); ++j)
            refreshStaleness(*c.subjects.at(j), now);
    }
}

void Scheduler::expireTimers(Conn &c, qint64 now)
{
    // The hold of a rate limit ends by itself; its persistent entry is cleared with it.
    const UtcTime hold = m_ports.state->holdUntil(c.cfg.id);
    if (hold.isValid() && hold.msecsSinceEpoch() <= now)
        m_ports.state->clearConnection(c.cfg.id, QStringLiteral("hold ended"));

    // The raised polling floor is halved after a calm day (REQ-SCH-13).
    if (c.rateFloorMs > 0 && now - c.lastRateLimitMs >= m_s.rateCalmMs) {
        c.rateFloorMs /= 2;
        if (c.rateFloorMs <= nominalMs(c, FeedCurrent))
            c.rateFloorMs = 0;
        c.lastRateLimitMs = now;
        c.lastHoldMs = 0;
    }

    for (int j = 0; j < c.subjects.size(); ++j) {
        Subj &s = *c.subjects.at(j);
        if (s.state == ScheduleState::Backoff && s.retryAtMs >= 0 && now >= s.retryAtMs) {
            toIdleDue(s, now);
        } else if (s.state == ScheduleState::NeedsAttention && s.cause == QLatin1String("credentials-locked")) {
            const UtcTime retry = c.session->approvalRetryAt();
            if (!retry.isValid() || now >= retry.msecsSinceEpoch())
                toIdleDue(s, now);
        }
    }
}

void Scheduler::dispatch(Conn &c, qint64 now)
{
    if (c.session->busy() || c.inflight)
        return;
    const UtcTime gate = c.session->gateOpenAt();
    if (gate.isValid() && gate.msecsSinceEpoch() > now)
        return;

    // The earliest due feed of any Idle Subject; `current` before `graph` before `logbook`,
    // ties by the order of the Subjects.
    Subj *best = 0;
    int bestFeed = -1;
    qint64 bestDue = 0;
    for (int j = 0; j < c.subjects.size(); ++j) {
        Subj &s = *c.subjects.at(j);
        if (s.state != ScheduleState::Idle)
            continue;
        for (int f = 0; f < kFeedCount; ++f) {
            if (s.nextDueMs[f] < 0 || s.nextDueMs[f] > now)
                continue;
            if (!best || s.nextDueMs[f] < bestDue || (s.nextDueMs[f] == bestDue && f < bestFeed)) {
                best = &s;
                bestFeed = f;
                bestDue = s.nextDueMs[f];
            }
        }
    }
    if (best)
        startCommand(c, *best, bestFeed, now);
}

void Scheduler::startCommand(Conn &c, Subj &s, int feed, qint64 now)
{
    SessionRequest req;
    req.subjectRef = s.cfg.subjectRef;
    req.subjects = 1;
    if (feed == FeedCurrent) {
        req.type = SessionRequest::Current;
    } else {
        // The graph and the logbook ask from the newest stored record of their feed, so a
        // gap is filled by the regular poll and nothing is asked twice. With
        // nothing stored the whole window the source offers is asked once.
        req.type = SessionRequest::Range;
        const UtcTime newest = m_ports.readings->newestMeasurement(
            s.cfg.id, c.cfg.adapterId + QLatin1Char('.') + feedName(feed));
        const qint64 window = feed == FeedGraph ? m_s.backfillWindowMs : m_s.logbookWindowMs;
        qint64 from = newest.isValid() ? newest.msecsSinceEpoch() : now - window;
        if (from >= now)
            from = now - 1;
        req.from = at(from);
        req.to = at(now);
        req.feeds = QStringList() << feedName(feed);
        req.spanMinutes = double(now - from) / 60000.0;
    }
    c.inflight = &s;
    c.newReading = false;
    c.lateRecords = 0;
    s.inflightFeed = feed;
    setState(s, ScheduleState::Fetching, QString());
    Conn *cp = &c;
    Subj *sp = &s;
    c.session->run(req, [this, cp, sp, feed](const SessionResult &r) { onResult(cp, sp, feed, r); });
}

// ---- results -----------------------------------------------------------------------------

void Scheduler::onResult(Conn *c, Subj *s, int feed, const SessionResult &r)
{
    c->inflight = 0;
    s->inflightFeed = -1;
    if (s->state != ScheduleState::Fetching)
        return;
    const qint64 now = nowMs();
    if (r.kind == SessionResult::Failed)
        onConnFailed(*c, *s, r, now);
    else if (r.report.outcome.result == CommandOutcome::Ok)
        onOutcomeOk(*c, *s, feed, r, now);
    else
        onOutcomeError(*c, *s, r, now);
    refreshStaleness(*s, now);
}

void Scheduler::onConnFailed(Conn &c, Subj &s, const SessionResult &r, qint64 now)
{
    const qint64 notBefore = r.retryAt.isValid() ? r.retryAt.msecsSinceEpoch() : -1;
    switch (r.reason) {
    case SessionResult::CredentialsLocked:
        // Not persisted: the lock ends by itself. Every Subject of the connection waits.
        for (int j = 0; j < c.subjects.size(); ++j)
            if (c.subjects.at(j)->state != ScheduleState::DisabledByGuard)
                setState(*c.subjects.at(j), ScheduleState::NeedsAttention,
                         QStringLiteral("credentials-locked"));
        return;
    case SessionResult::Attention:
        attentionForConnection(c, r.attentionCause, r.detail, false);
        return;
    case SessionResult::DeniedDisabled:
        disableConnection(c, m_ports.state->connectionDisabled(c.cfg.id) ? QStringLiteral("connection")
                                                                         : QStringLiteral("adapter-version"));
        return;
    case SessionResult::DeniedVersion:
        disableConnection(c, QStringLiteral("adapter-version"));
        return;
    case SessionResult::DeniedCircuit:
        setState(s, ScheduleState::Backoff, QStringLiteral("circuit-open"),
                 notBefore >= 0 ? notBefore : now + m_s.backoffBaseMs);
        return;
    case SessionResult::DeniedSpawnRate:
        setState(s, ScheduleState::Backoff, QStringLiteral("spawn-rate"),
                 notBefore >= 0 ? notBefore : now + m_s.backoffBaseMs);
        return;
    case SessionResult::RateLimited:
        rateLimited(c, r.retryAfterS, now);
        return;
    case SessionResult::InvalidCommand: {
        ++c.invalidN;
        if (c.invalidN >= m_s.invalidCommandMax) {
            attentionForConnection(c, StateCause::internal(), QStringLiteral("invalid command"), true);
            return;
        }
        transientBackoff(c, s, -1, -1, QStringLiteral("internal"), now);
        return;
    }
    case SessionResult::SecretStoreError:
    case SessionResult::SpawnFailed:
    case SessionResult::HelloTimeout:
    case SessionResult::Transient:
    default:
        transientBackoff(c, s, -1, notBefore, QStringLiteral("transient"), now);
        return;
    }
}

void Scheduler::onOutcomeOk(Conn &c, Subj &s, int feed, const SessionResult &r, qint64 now)
{
    const CommandOutcome &o = r.report.outcome;
    s.lastContactOkMs = now;
    m_contactOkSinceStart = true;
    s.backoffN = 0;
    s.subjectFailN = 0;
    s.firstFailMs = -1;
    c.invalidN = 0;
    c.lastSuccessBySubject.insert(subjectKey(c.cfg.id, s.cfg.id), now);

    // The sensor object of the completion feeds the warm-up cause (REQ-SEN-01).
    if (!r.completion.sensor.isEmpty()) {
        m_ports.readings->noteSensor(s.cfg.id, r.completion.sensor);
        const UtcTime activated = UtcTime::fromIso(r.completion.sensor.value(QStringLiteral("activated_at")).toString());
        const QJsonValue warm = r.completion.sensor.value(QStringLiteral("warmup_minutes"));
        if (activated.isValid() && warm.isDouble())
            s.sensorReadyMs = activated.msecsSinceEpoch() + qint64(warm.toDouble()) * 60 * 1000;
    }

    if (feed == FeedCurrent)
        noteTrend(s, r.completion);

    // Consecutive polls with rejected records count as one `protocol-changed` (REQ-SCH-04).
    qint64 rejected = 0;
    for (QMap<int, qint64>::const_iterator it = o.quarantinedByReason.begin();
         it != o.quarantinedByReason.end(); ++it)
        rejected += it.value();
    if (rejected > 0)
        ++s.rejectStreak;
    else
        s.rejectStreak = 0;

    // The next run. Only the `current` feed follows the sensor timestamp.
    if (feed == FeedCurrent) {
        const NextRun nr = nextCurrentRun(now, effectiveMs(c, s, FeedCurrent, now), c.newReading,
                                          s.lastReadingMs, s.recheckN, m_s);
        s.nextDueMs[FeedCurrent] = nr.atMs;
        s.recheckN = nr.recheckN;
    } else {
        s.nextDueMs[feed] = nominalMs(c, feed) > 0 ? now + effectiveMs(c, s, feed, now) : -1;
    }

    if (s.rejectStreak >= m_s.rejectPollsMax) {
        s.rejectStreak = 0;
        attentionForConnection(c, StateCause::protocolChanged(),
                               QStringLiteral("records rejected in consecutive polls"), true,
                               o.link.adapterVersion);
        return;
    }
    setState(s, ScheduleState::Idle, QString());
}

void Scheduler::onOutcomeError(Conn &c, Subj &s, const SessionResult &r, qint64 now)
{
    const CommandOutcome &o = r.report.outcome;

    // A kill by the guard or the driver is a violation, not a message from the source (L4).
    if (o.killedBy == CommandOutcome::Guard || o.killedBy == CommandOutcome::DriverLimit) {
        if (m_ports.state->connectionDisabled(c.cfg.id))
            disableConnection(c, QStringLiteral("connection"));
        else
            transientBackoff(c, s, -1, -1, QStringLiteral("guard-kill"), now);
        return;
    }

    switch (o.errorClass) {
    case ErrorClass::RateLimited:
        rateLimited(c, o.retryAfterS, now);
        return;
    case ErrorClass::TermsMustBeAccepted:
        attentionForConnection(c, StateCause::terms(), QStringLiteral("terms must be accepted"), true,
                               o.link.adapterVersion);
        return;
    case ErrorClass::ProtocolChanged:
        attentionForConnection(c, StateCause::protocolChanged(), QStringLiteral("source reply not understood"),
                               true, o.link.adapterVersion);
        return;
    case ErrorClass::InvalidCommand:
        ++c.invalidN;
        if (c.invalidN >= m_s.invalidCommandMax)
            attentionForConnection(c, StateCause::internal(), QStringLiteral("invalid command"), true,
                                   o.link.adapterVersion);
        else
            transientBackoff(c, s, -1, -1, QStringLiteral("internal"), now);
        return;
    case ErrorClass::AuthFailed:
    case ErrorClass::Transient:
    default:
        break;
    }

    // Transient (also a missed deadline or an unexpected exit; the circuit breaker counted
    // those in the guard). A Subject that fails again and again while another Subject of the
    // same connection succeeds has probably lost its share:.
    ++s.subjectFailN;
    if (s.firstFailMs < 0)
        s.firstFailMs = now;
    if (s.subjectFailN >= m_s.subjectFailMax && otherSubjectSucceededSince(c, s, s.firstFailMs)) {
        m_ports.state->setAttention(subjectKey(c.cfg.id, s.cfg.id), StateCause::subjectUnavailable(),
                                    QStringLiteral("repeated failures while other Subjects succeed"),
                                    c.cfg.adapterId, o.link.adapterVersion);
        setState(s, ScheduleState::NeedsAttention, StateCause::subjectUnavailable());
        return;
    }
    transientBackoff(c, s, o.retryAfterS, -1, QStringLiteral("transient"), now);
}

bool Scheduler::otherSubjectSucceededSince(const Conn &c, const Subj &s, qint64 sinceMs) const
{
    const QString self = subjectKey(c.cfg.id, s.cfg.id);
    for (QHash<QString, qint64>::const_iterator it = c.lastSuccessBySubject.begin();
         it != c.lastSuccessBySubject.end(); ++it)
        if (it.key() != self && it.value() > sinceMs)
            return true;
    return false;
}

void Scheduler::transientBackoff(Conn &, Subj &s, int retryAfterS, qint64 notBeforeMs,
                                 const QString &cause, qint64 now)
{
    const qint64 delay = backoffDelayMs(s.backoffN, retryAfterS, m_s, m_ports.random->next());
    qint64 retryAt = now + delay;
    if (notBeforeMs > retryAt)
        retryAt = notBeforeMs;
    ++s.backoffN;
    setState(s, ScheduleState::Backoff, cause, retryAt);
}

void Scheduler::rateLimited(Conn &c, int retryAfterS, qint64 now)
{
    // REQ-SCH-04: stop that connection for a long period; REQ-SCH-13: and poll more slowly
    // from then on. The hold doubles with each rate limit until the calm period has passed.
    qint64 hold = c.lastHoldMs > 0 ? c.lastHoldMs * 2 : m_s.rateHoldInitialMs;
    const qint64 hint = retryAfterS > 0 ? qint64(retryAfterS) * 1000 : 0;
    if (hint > hold)
        hold = hint;
    if (hold > m_s.rateHoldCapMs)
        hold = m_s.rateHoldCapMs;
    c.lastHoldMs = hold;
    c.lastRateLimitMs = now;
    const qint64 base = c.rateFloorMs > nominalMs(c, FeedCurrent) ? c.rateFloorMs : nominalMs(c, FeedCurrent);
    qint64 floor = base * 2 > hint ? base * 2 : hint;
    if (floor > m_s.rateFloorCapMs)
        floor = m_s.rateFloorCapMs;
    c.rateFloorMs = floor;

    const qint64 until = now + hold;
    m_ports.state->setHold(c.cfg.id, at(until),
                           QStringLiteral("retry after %1 s").arg(hold / 1000));
    IngestLogEntry e;
    e.time = m_ports.clock->now();
    e.event = QStringLiteral("rate-limited");
    e.adapter = c.cfg.adapterId;
    e.connection = c.cfg.id;
    e.detail = QStringLiteral("hold %1 s, polling floor %2 s").arg(hold / 1000).arg(floor / 1000);
    m_ports.ingest->append(e);
    for (int j = 0; j < c.subjects.size(); ++j) {
        Subj &s = *c.subjects.at(j);
        if (s.state != ScheduleState::DisabledByGuard && s.state != ScheduleState::NeedsAttention)
            setState(s, ScheduleState::Backoff, StateCause::rateLimited(), until);
    }
    if (m_listener)
        m_listener->connectionWarning(c.cfg.id, QStringLiteral("rate-limited"), e.detail);
}

void Scheduler::attentionForConnection(Conn &c, const QString &cause, const QString &detail,
                                       bool persist, const QString &version)
{
    if (persist)
        m_ports.state->setAttention(c.cfg.id, cause, detail, c.cfg.adapterId, version);
    for (int j = 0; j < c.subjects.size(); ++j) {
        Subj &s = *c.subjects.at(j);
        if (s.state != ScheduleState::DisabledByGuard && !(s.state == ScheduleState::NeedsAttention
                                                         && isSubjectCause(s.cause)))
            setState(s, ScheduleState::NeedsAttention, cause);
    }
    if (cause == StateCause::protocolChanged() && m_listener)
        m_listener->connectionWarning(c.cfg.id, QStringLiteral("protocol-changed"), detail);
}

void Scheduler::disableConnection(Conn &c, const QString &cause)
{
    for (int j = 0; j < c.subjects.size(); ++j)
        setState(*c.subjects.at(j), ScheduleState::DisabledByGuard, cause);
}

// ---- state changes and events ------------------------------------------------------------

void Scheduler::setState(Subj &s, ScheduleState state, const QString &cause, qint64 retryAtMs)
{
    const bool changed = s.state != state || s.cause != cause;
    s.state = state;
    s.cause = cause;
    s.retryAtMs = retryAtMs;
    if (changed)
        notifyState(s);
}

void Scheduler::toIdleDue(Subj &s, qint64 dueMs)
{
    // The `current` feed runs as soon as the gate allows; the others keep their time unless
    // they never had one. A feed that is already overdue stays overdue.
    for (int f = 0; f < kFeedCount; ++f) {
        if (nominalMs(*s.conn, f) <= 0)
            continue;
        if (s.nextDueMs[f] < 0)
            s.nextDueMs[f] = dueMs;
        else if (f == FeedCurrent && s.nextDueMs[f] > dueMs)
            s.nextDueMs[f] = dueMs;
    }
    setState(s, ScheduleState::Idle, QString());
}

void Scheduler::clearSubjectAttention(Subj &s)
{
    const QString key = subjectKey(s.conn->cfg.id, s.cfg.id);
    if (m_ports.state->attention(key).active())
        m_ports.state->clearConnection(key, QStringLiteral("attention ended"));
}

void Scheduler::notifyState(const Subj &s)
{
    if (m_listener)
        m_listener->subjectStateChanged(statusOf(s));
}

// ---- events from the user and the app ----------------------------------------------------

void Scheduler::userRetry(const QString &connection)
{
    Conn *c = this->connection(connection);
    if (!c)
        return;
    const qint64 now = nowMs();
    c->session->userRetry();                // clears the attention; the callback wakes the Subjects
    if (m_ports.state->holdUntil(c->cfg.id).isValid())
        m_ports.state->clearConnection(c->cfg.id, QStringLiteral("retry by the user"));
    c->lastHoldMs = 0;
    c->invalidN = 0;
    for (int j = 0; j < c->subjects.size(); ++j) {
        Subj &s = *c->subjects.at(j);
        if (s.state == ScheduleState::Backoff || s.state == ScheduleState::NeedsAttention) {
            if (isSubjectCause(s.cause))
                clearSubjectAttention(s);
            s.backoffN = 0;
            s.subjectFailN = 0;
            s.firstFailMs = -1;
            toIdleDue(s, now);
        }
    }
}

void Scheduler::credentialsChanged(const QString &connection)
{
    Conn *c = this->connection(connection);
    if (c)
        c->session->credentialsChanged();   // through sessionAttentionCleared
}

void Scheduler::secretsAvailable(const QString &connection)
{
    Conn *c = this->connection(connection);
    if (!c)
        return;
    c->session->secretsAvailable();
    const qint64 now = nowMs();
    for (int j = 0; j < c->subjects.size(); ++j) {
        Subj &s = *c->subjects.at(j);
        if (s.state == ScheduleState::NeedsAttention && s.cause == QLatin1String("credentials-locked"))
            toIdleDue(s, now);
    }
}

void Scheduler::intervalsChanged(const QString &connection, const ConnectionConfig &config)
{
    Conn *c = this->connection(connection);
    if (!c)
        return;
    c->cfg.currentIntervalMs = config.currentIntervalMs;
    c->cfg.graphIntervalMs = config.graphIntervalMs;
    c->cfg.logbookIntervalMs = config.logbookIntervalMs;
    const qint64 now = nowMs();
    for (int j = 0; j < c->subjects.size(); ++j) {
        Subj &s = *c->subjects.at(j);
        for (int f = 0; f < kFeedCount; ++f) {
            if (nominalMs(*c, f) <= 0) {
                s.nextDueMs[f] = -1;
            } else if (f == FeedCurrent && s.lastReadingMs >= 0) {
                s.nextDueMs[f] = nextCurrentRun(now, effectiveMs(*c, s, f, now), true, s.lastReadingMs, 0, m_s).atMs;
            } else if (s.nextDueMs[f] < 0 || s.nextDueMs[f] > now + effectiveMs(*c, s, f, now)) {
                s.nextDueMs[f] = now + effectiveMs(*c, s, f, now);
            }
        }
    }
}

// ---- events from the guard and the session -----------------------------------------------

void Scheduler::guardTripped(const QString &connection)
{
    Conn *c = this->connection(connection);
    if (!c)
        return;
    disableConnection(*c, QStringLiteral("connection"));
    c->inflight = 0;
    c->session->guardStopped();
}

void Scheduler::guardVersionTripped(const QString &adapter, const QString &)
{
    for (int i = 0; i < m_conns.size(); ++i) {
        Conn &c = *m_conns.at(i);
        if (c.cfg.adapterId != adapter)
            continue;
        // The built-in adapters of one application share one version, so every connection of
        // the adapter stops.
        if (!m_ports.state->connectionDisabled(c.cfg.id))
            disableConnection(c, QStringLiteral("adapter-version"));
        c.inflight = 0;
        c.session->guardStopped();
    }
}

void Scheduler::guardReenabled(const QString &connection)
{
    Conn *c = this->connection(connection);
    if (!c)
        return;
    c->session->reenabled();
    const qint64 now = nowMs();
    for (int j = 0; j < c->subjects.size(); ++j) {
        Subj &s = *c->subjects.at(j);
        if (s.state == ScheduleState::DisabledByGuard) {
            for (int f = 0; f < kFeedCount; ++f)
                s.nextDueMs[f] = firstRunMs(*c, s, f, now);
            s.backoffN = 0;
            setState(s, ScheduleState::Idle, QString());
        }
    }
}

void Scheduler::guardVersionReenabled(const QString &adapter, const QString &)
{
    const qint64 now = nowMs();
    for (int i = 0; i < m_conns.size(); ++i) {
        Conn &c = *m_conns.at(i);
        if (c.cfg.adapterId != adapter || m_ports.state->connectionDisabled(c.cfg.id))
            continue;
        c.session->reenabled();
        for (int j = 0; j < c.subjects.size(); ++j) {
            Subj &s = *c.subjects.at(j);
            if (s.state == ScheduleState::DisabledByGuard && s.cause == QLatin1String("adapter-version")) {
                for (int f = 0; f < kFeedCount; ++f)
                    s.nextDueMs[f] = firstRunMs(c, s, f, now);
                setState(s, ScheduleState::Idle, QString());
            }
        }
    }
}

void Scheduler::sessionAttentionCleared(const QString &connection)
{
    Conn *c = this->connection(connection);
    if (!c)
        return;
    const qint64 now = nowMs();
    for (int j = 0; j < c->subjects.size(); ++j) {
        Subj &s = *c->subjects.at(j);
        if (s.state == ScheduleState::NeedsAttention && !isSubjectCause(s.cause)
            && s.cause != QLatin1String("credentials-locked")) {
            s.backoffN = 0;
            toIdleDue(s, now);
        }
    }
}

void Scheduler::sessionApprovalWaiting(const QString &connection, const UtcTime &since)
{
    if (m_listener)
        m_listener->connectionWarning(connection, QStringLiteral("approval-waiting"), since.toIso());
}

void Scheduler::sessionRenewalRefused(const QString &connection)
{
    if (m_listener)
        m_listener->connectionWarning(connection, QStringLiteral("renewal-refused"), QString());
}

// ---- wake-up -----------------------------------------------------------------------------

qint64 Scheduler::earliestDueMs(const Conn &c) const
{
    qint64 best = -1;
    for (int j = 0; j < c.subjects.size(); ++j) {
        const Subj &s = *c.subjects.at(j);
        if (s.state == ScheduleState::Idle) {
            for (int f = 0; f < kFeedCount; ++f)
                if (s.nextDueMs[f] >= 0 && (best < 0 || s.nextDueMs[f] < best))
                    best = s.nextDueMs[f];
        } else if (s.state == ScheduleState::Backoff && s.retryAtMs >= 0 && (best < 0 || s.retryAtMs < best)) {
            best = s.retryAtMs;
        }
    }
    return best;
}

UtcTime Scheduler::nextWakeAt() const
{
    if (!m_started)
        return UtcTime();
    const qint64 now = nowMs();
    qint64 best = -1;
    struct Keep { static void min(qint64 *b, qint64 t) { if (t >= 0 && (*b < 0 || t < *b)) *b = t; } };

    for (int i = 0; i < m_conns.size(); ++i) {
        const Conn &c = *m_conns.at(i);
        const UtcTime gate = c.session->gateOpenAt();
        const qint64 gateMs = gate.isValid() ? gate.msecsSinceEpoch() : 0;
        for (int j = 0; j < c.subjects.size(); ++j) {
            const Subj &s = *c.subjects.at(j);
            if (s.state == ScheduleState::Idle && !c.session->busy() && !c.inflight) {
                for (int f = 0; f < kFeedCount; ++f)
                    if (s.nextDueMs[f] >= 0)
                        Keep::min(&best, s.nextDueMs[f] > gateMs ? s.nextDueMs[f] : gateMs);
            } else if (s.state == ScheduleState::Backoff) {
                Keep::min(&best, s.retryAtMs);
            } else if (s.state == ScheduleState::NeedsAttention && s.cause == QLatin1String("credentials-locked")) {
                const UtcTime t = c.session->approvalRetryAt();
                if (t.isValid())
                    Keep::min(&best, t.msecsSinceEpoch());
            }
            // The moment this Subject becomes stale, so that the report is not late.
            if (!s.stale) {
                const qint64 ref = s.lastReadingMs >= 0 ? s.lastReadingMs : m_startedAtMs;
                Keep::min(&best, ref + qint64(m_s.staleFactor) * effectiveMs(c, s, FeedCurrent, now) + 1);
            }
        }
        const UtcTime w = c.session->nextWakeAt(earliestDueMs(c));
        if (w.isValid())
            Keep::min(&best, w.msecsSinceEpoch());
        if (c.rateFloorMs > 0)
            Keep::min(&best, c.lastRateLimitMs + m_s.rateCalmMs);
        const UtcTime hold = m_ports.state->holdUntil(c.cfg.id);
        if (hold.isValid())
            Keep::min(&best, hold.msecsSinceEpoch());
    }
    if (m_ports.state->pendingWrites() > 0 || m_ports.ingest->queued() > 0 || !m_queue.isEmpty())
        Keep::min(&best, now + kRetryPersistMs);
    if (best < 0)
        return UtcTime();
    return at(best < now ? now : best);
}

// ---- staleness (REQ-SCH-06, REQ-SCH-07) ---------------------------------------------------

void Scheduler::refreshStaleness(Subj &s, qint64 now)
{
    const Conn &c = *s.conn;
    StalenessInput in;
    in.nowMs = now;
    in.lastSuccessMs = s.lastReadingMs;                 // the `current` feed only
    in.lastContactOkMs = s.lastContactOkMs;
    in.effCurrentMs = nominalMs(c, FeedCurrent) > c.rateFloorMs ? nominalMs(c, FeedCurrent) : c.rateFloorMs;
    in.staleFactor = m_s.staleFactor;
    in.disabledByGuard = s.state == ScheduleState::DisabledByGuard;
    in.credentialsLocked = s.state == ScheduleState::NeedsAttention && s.cause == QLatin1String("credentials-locked");
    in.needsAttention = s.state == ScheduleState::NeedsAttention && !in.credentialsLocked;
    in.attentionDetail = s.cause;
    in.inBackoff = s.state == ScheduleState::Backoff;
    in.backoffDetail = s.cause;
    in.serviceDownMs = m_serviceDownMs;
    in.contactOkSinceStart = m_contactOkSinceStart;
    in.sensorKnown = s.sensorReadyMs >= 0;
    in.sensorReadyMs = s.sensorReadyMs;
    in.startedAtMs = m_startedAtMs;

    const StalenessResult r = computeStaleness(in);
    if (r.stale != s.stale || r.cause != s.staleCause || r.detail != s.staleDetail) {
        s.stale = r.stale;
        s.staleCause = r.cause;
        s.staleDetail = r.detail;
        s.staleSinceMs = now;
        if (m_listener)
            m_listener->stalenessChanged(statusOf(s));
    }
}

// Keeps the trend arrow of a `current` completion with the time of its reading. It
// is only an annotation of the newest stored reading, so it is kept only when the completion's
// watermark IS that reading: a reading the core rejected (quarantined) or an older one that the
// source repeats must not give its arrow to a newer reading. A completion with no reading
// (invalid watermark) changes nothing. The arrow is never stored: after a restart there is none
// until the first poll.
void Scheduler::noteTrend(Subj &s, const FetchCompletion &completion)
{
    if (!completion.watermark.isValid())
        return;
    const qint64 readingMs = completion.watermark.msecsSinceEpoch();
    if (readingMs != s.lastReadingMs)
        return;
    s.trendForMs = readingMs;
    s.hasTrend = completion.hasTrend;
    s.trend = completion.trend;
}

SubjectStatus Scheduler::statusOf(const Subj &s) const
{
    SubjectStatus st;
    st.subject = s.cfg.id;
    st.connection = s.conn->cfg.id;
    st.state = s.state;
    st.cause = s.cause;
    st.retryAt = at(s.retryAtMs);
    for (int f = 0; f < kFeedCount; ++f)
        st.nextDue[f] = at(s.nextDueMs[f]);
    st.backoffN = s.backoffN;
    st.lastSuccessAt = at(s.lastReadingMs);
    st.lastContactOkAt = at(s.lastContactOkMs);
    st.stale = s.stale;
    st.staleCause = s.staleCause;
    st.staleDetail = s.staleDetail;
    st.staleSince = at(s.staleSinceMs);

    // The arrow goes out only while its reading is fresh: the same limit as the staleness of
    // the `current` feed (kStaleFactor times the effective interval).
    const Conn &c = *s.conn;
    const qint64 effMs = nominalMs(c, FeedCurrent) > c.rateFloorMs ? nominalMs(c, FeedCurrent) : c.rateFloorMs;
    if (s.hasTrend && s.trendForMs >= 0 && nowMs() - s.trendForMs <= m_s.staleFactor * effMs) {
        st.hasTrend = true;
        st.trendCode = s.trend;
        st.trendFor = at(s.trendForMs);
    }
    return st;
}

QList<QUuid> Scheduler::subjects() const
{
    QList<QUuid> out;
    for (int i = 0; i < m_conns.size(); ++i)
        for (int j = 0; j < m_conns.at(i)->subjects.size(); ++j)
            out.append(m_conns.at(i)->subjects.at(j)->cfg.id);
    return out;
}

SubjectStatus Scheduler::status(const QUuid &subject) const
{
    const Subj *s = subjectOf(subject);
    return s ? statusOf(*s) : SubjectStatus();
}

SessionState Scheduler::sessionState(const QString &connection) const
{
    const Conn *c = this->connection(connection);
    return c ? c->session->state() : SessionState::Closed;
}

// ---- records -----------------------------------------------------------------------------

void Scheduler::acceptRecords(const LinkId &link, const QString &feed, const QVector<LinkRecord> &records)
{
    Conn *c = connection(link.connectionId);
    if (!c)
        return;
    // Records are grouped by Subject (a command is for one, but the adapter names each).
    QMap<QString, QVector<Record> > bySubject;
    QMap<QString, Subj *> owners;
    QMap<QString, qint64> newest;
    for (int i = 0; i < records.size(); ++i) {
        Subj *s = 0;
        for (int j = 0; j < c->subjects.size(); ++j)
            if (c->subjects.at(j)->cfg.subjectRef == records.at(i).subjectRef)
                s = c->subjects.at(j);
        if (!s)
            continue;                   // a Subject we do not follow: not stored
        Record r = records.at(i).record;
        r.subject = s->cfg.id;
        const QString key = s->cfg.id.toString();
        if (!newest.contains(key))
            newest.insert(key, s->lastReadingMs);
        if (feed == QLatin1String("current")) {
            // REQ-SCH-03: a live reading is stored only if newer than the last one. An
            // equal or older one is the source repeating itself, not an error: counted, dropped.
            if (r.measuredAt.msecsSinceEpoch() <= newest.value(key)) {
                ++c->lateRecords;
                continue;
            }
            newest[key] = r.measuredAt.msecsSinceEpoch();
        }
        bySubject[key].append(r);
        owners[key] = s;
    }
    for (QMap<QString, QVector<Record> >::const_iterator it = bySubject.begin(); it != bySubject.end(); ++it) {
        Subj *s = owners.value(it.key());
        const int n = it.value().size();
        const int added = m_ports.readings->ingest(s->cfg.id, link, it.value());
        if (added < 0) {
            // The store is busy (REQ-STO-15): keep the readings in a bounded queue.
            if (m_queuedRecords + n > m_s.readingQueueMax) {
                IngestLogEntry e;
                e.time = m_ports.clock->now();
                e.event = QStringLiteral("readings-lost");
                e.adapter = link.adapterId;
                e.adapterVersion = link.adapterVersion;
                e.sessionId = link.sessionId;
                e.connection = link.connectionId;
                e.detail = QStringLiteral("%1 readings dropped, queue full").arg(n);
                m_ports.ingest->append(e);
                if (m_listener)
                    m_listener->readingQueueFull(n);
                continue;
            }
            PendingBatch b;
            b.subject = s->cfg.id;
            b.link = link;
            b.records = it.value();
            m_queue.append(b);
            m_queuedRecords += n;
        }
        if (feed == QLatin1String("current")) {
            s->lastReadingMs = newest.value(it.key());
            c->newReading = true;
        }
    }
}

void Scheduler::rejectRecord(const LinkId &link, const QString &reason, const QString &detail,
                             const QJsonObject &raw)
{
    m_ports.readings->quarantine(link, reason, detail, raw);
}

void Scheduler::flushReadings()
{
    while (!m_queue.isEmpty()) {
        const PendingBatch &b = m_queue.first();
        if (m_ports.readings->ingest(b.subject, b.link, b.records) < 0)
            return;
        m_queuedRecords -= b.records.size();
        m_queue.removeFirst();
    }
}

} // namespace lm
