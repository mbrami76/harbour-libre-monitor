// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Michele Brami
#include "guard/guard.h"

#include "record/registry.h"
#include "session/adapterlink.h"        // violationKindName, severityOf

#include <cmath>

namespace lm {

namespace {

// Wire name of a violation kind, for the ingest-log detail. Names a limit, never content.
QString kindName(ViolationKind kind) { return violationKindName(kind); }

QString sevName(Severity s) { return s == Severity::Hard ? QStringLiteral("hard") : QStringLiteral("soft"); }

QString actionName(const Action &a)
{
    switch (a.kind) {
    case Action::Drop: return QStringLiteral("drop");
    case Action::Pause: return QStringLiteral("pause");
    case Action::Kill: return QStringLiteral("kill");
    default: return QStringLiteral("proceed");
    }
}

} // namespace

Guard::Guard(Clock *clock, StateKeeper *state, IngestLog *ingest, const GuardSettings &settings)
    : m_clock(clock), m_state(state), m_ingest(ingest), m_settings(settings), m_listener(0) {}

UtcTime Guard::nowFor(const UtcTime &given) const
{
    return given.isValid() ? given : m_clock->now();
}

Guard::Conn &Guard::conn(const QString &connectionId, const QString &adapterId)
{
    QHash<QString, Conn>::iterator it = m_conns.find(connectionId);
    if (it == m_conns.end()) {
        Conn c;
        c.score = ViolationScore(m_settings.decayHalfLifeMs);
        c.lines.configure(m_settings.lineBucketCap, m_settings.lineRefillPerS);
        c.bytes.configure(m_settings.byteBucketCap, m_settings.byteRefillPerS);
        c.spawn.configure(m_settings.spawnBucketCap, 1000.0 / double(m_settings.spawnRefillMs));
        it = m_conns.insert(connectionId, c);
    }
    if (!adapterId.isEmpty())
        it->adapterId = adapterId;
    return it.value();
}

void Guard::log(const QString &connection, const QString &adapter, const QString &version,
                const QString &event, const QString &detail)
{
    IngestLogEntry e;
    e.time = m_clock->now();
    e.event = event;
    e.adapter = adapter;
    e.adapterVersion = version;
    e.connection = connection;
    e.detail = detail;
    m_ingest->append(e);
}

void Guard::log(const LinkId &link, const QString &event, const QString &detail)
{
    IngestLogEntry e;
    e.time = m_clock->now();
    e.event = event;
    e.adapter = link.adapterId;
    e.adapterVersion = link.adapterVersion;
    e.sessionId = link.sessionId;
    e.connection = link.connectionId;
    e.detail = detail;
    m_ingest->append(e);
}

LinkLimits Guard::limits() const
{
    LinkLimits l;       // the contract defaults for lines, batches, deadlines (REQ-GRD-01)
    l.memoryLimitBytes = m_settings.memoryLimitBytes;
    l.cpuTimeLimitS = m_settings.cpuTimeLimitS;
    l.fileDescriptorLimit = m_settings.fileDescriptorLimit;
    l.residentLimitBytes = m_settings.residentLimitBytes;
    return l;
}

// ---- admission ---------------------------------------------------------------------------

MayStartResult Guard::mayStart(const StartRequest &request)
{
    Conn &c = conn(request.connectionId, request.adapterId);
    const UtcTime now = m_clock->now();

    if (m_state->connectionDisabled(request.connectionId))
        return MayStartResult::denied(MayStartResult::DisabledByGuard);
    if (!c.observedVersion.isEmpty() && m_state->versionDisabled(request.adapterId, c.observedVersion))
        return MayStartResult::denied(MayStartResult::AdapterVersionDisabled);

    if (c.circuitOpen) {
        // Open until the time is up; then exactly one trial start is allowed. Its command
        // closes the circuit (onCommandFinished) or reopens it for twice as long (addFailure).
        if (now.msecsSinceEpoch() < c.circuitUntilMs || c.trial)
            return MayStartResult::denied(MayStartResult::CircuitOpen);
        c.trial = true;
    }
    if (!c.spawn.has(now, 1.0))
        return MayStartResult::denied(MayStartResult::SpawnRate);
    c.spawn.take(now, 1.0);
    return MayStartResult::allowed();
}

void Guard::onSpawned(const LinkId &link)
{
    Conn &c = conn(link.connectionId, link.adapterId);
    // Buckets are per link: a new process starts with full buckets and no open episode.
    c.lines.configure(m_settings.lineBucketCap, m_settings.lineRefillPerS);
    c.bytes.configure(m_settings.byteBucketCap, m_settings.byteRefillPerS);
    c.lineEpisode = c.byteEpisode = false;
    c.hasExpect = false;
}

MayStartResult Guard::onAdmitted(const LinkId &link)
{
    Conn &c = conn(link.connectionId, link.adapterId);
    c.observedVersion = link.adapterVersion;
    if (m_state->connectionDisabled(link.connectionId))
        return MayStartResult::denied(MayStartResult::DisabledByGuard);
    if (m_state->versionDisabled(link.adapterId, link.adapterVersion))
        return MayStartResult::denied(MayStartResult::AdapterVersionDisabled);
    // The pair session-start / session-end tags the session: the records ingested between
    // them by this adapter are the ones of this session (REQ-GRD-10).
    log(link, QStringLiteral("session-start"), QStringLiteral("admitted"));
    return MayStartResult::allowed();
}

void Guard::onExited(const LinkId &link, const ExitStatus &status)
{
    Conn &c = conn(link.connectionId, link.adapterId);
    QString how = status.crashed ? QStringLiteral("crashed")
                                 : QStringLiteral("exit %1").arg(status.exitCode);
    // The adapter's highest resident size goes to the log with every end of a session, a stop
    // and a kill alike: a number, no value of the user's, and enough to see a slow leak
    // over days. A kill for memory says so.
    if (status.killedForMemory)
        how += QStringLiteral(", killed for memory");
    if (status.peakResidentBytes >= 0)
        how += QStringLiteral(", peak_resident_bytes=%1").arg(status.peakResidentBytes);
    log(link, QStringLiteral("session-end"), how);
    if (status.crashed || status.exitCode != 0)
        addFailure(link.connectionId, link.adapterId, link.adapterVersion, link.sessionId);
    c.hasExpect = false;
}

// ---- budgets -----------------------------------------------------------------------------

Action Guard::onLine(const LinkId &link, qint64 bytes, const UtcTime &time)
{
    Conn &c = conn(link.connectionId, link.adapterId);
    const UtcTime now = nowFor(time);

    // A bucket that has refilled to the re-arm fraction may report a new episode.
    if (c.lineEpisode && c.lines.fraction(now) >= m_settings.bucketRearmFraction)
        c.lineEpisode = false;
    if (c.byteEpisode && c.bytes.fraction(now) >= m_settings.bucketRearmFraction)
        c.byteEpisode = false;

    const qint64 waitLines = c.lines.take(now, 1.0);
    const qint64 waitBytes = c.bytes.take(now, double(bytes));
    if (waitLines <= 0 && waitBytes <= 0)
        return Action::proceed();

    const qint64 wait = waitLines > waitBytes ? waitLines : waitBytes;
    const int pauseMs = int(wait > m_settings.pauseMaxMs ? m_settings.pauseMaxMs : wait);
    Action answer = Action::pause(pauseMs);

    // One violation per episode, not per line, so a long pause does not inflate the score.
    if (waitLines > 0 && !c.lineEpisode) {
        c.lineEpisode = true;
        log(link, QStringLiteral("pause"), QStringLiteral("line bucket empty, %1 ms").arg(pauseMs));
        const Action a = violate(link, ViolationKind::BudgetExceeded, Severity::Soft,
                                 QStringLiteral("protocol lines per second"), now);
        if (a.kind == Action::Kill) answer = a;
    }
    if (waitBytes > 0 && !c.byteEpisode) {
        c.byteEpisode = true;
        log(link, QStringLiteral("pause"), QStringLiteral("byte bucket empty, %1 ms").arg(pauseMs));
        const Action a = violate(link, ViolationKind::BudgetExceeded, Severity::Soft,
                                 QStringLiteral("bytes per second"), now);
        if (a.kind == Action::Kill) answer = a;
    }
    return answer;
}

void Guard::beginCommand(const LinkId &link, const QString &commandType, int subjects,
                         double spanMinutes)
{
    Conn &c = conn(link.connectionId, link.adapterId);
    c.hasExpect = true;
    c.commandType = commandType;
    c.subjects = subjects > 0 ? subjects : 1;
    c.spanMinutes = spanMinutes > 1 ? spanMinutes : 1;
    c.feedRecords.clear();
    c.cmdTotal = 0;
    c.rateFlagged = false;
    c.capFlagged = false;
}

Action Guard::onBatch(const LinkId &link, const QString &feed, int records, qint64 /*bytes*/)
{
    Conn &c = conn(link.connectionId, link.adapterId);
    const UtcTime now = m_clock->now();
    Action answer = Action::proceed();

    // (a) Plausible rate against the registry (REQ-GRD-01). Only for sample feeds: the
    // logbook holds events, bound by the absolute caps below. Without an expectation (the
    // command-line program) only the absolute caps apply.
    if (c.hasExpect && feed != QLatin1String("logbook")) {
        const MetricDef *glucose = MetricRegistry::instance().find(QStringLiteral("glucose"));
        const double perMinute = glucose ? glucose->maxSamplesPerMinute : 2.0;
        const double allowance = std::ceil(perMinute * c.spanMinutes * c.subjects) + m_settings.rateSlack;
        c.feedRecords[feed] += records;
        if (double(c.feedRecords.value(feed)) > allowance) {
            if (!c.rateFlagged) {
                c.rateFlagged = true;
                log(link, QStringLiteral("drop"), QStringLiteral("records above the registry rate"));
                const Action a = violate(link, ViolationKind::RateAboveRegistry, Severity::Soft,
                                         QStringLiteral("records above the registry rate"), now);
                if (a.kind == Action::Kill) return a;
            }
            answer = Action::drop();
        }
    }

    // (b) Absolute caps: per command and per rolling window.
    c.cmdTotal += records;
    if (c.cmdTotal > m_settings.recordsPerCommandMax) {
        if (!c.capFlagged) {
            c.capFlagged = true;
            log(link, QStringLiteral("drop"), QStringLiteral("records per command"));
            const Action a = violate(link, ViolationKind::BudgetExceeded, Severity::Soft,
                                     QStringLiteral("records per command"), now);
            if (a.kind == Action::Kill) return a;
        }
        answer = Action::drop();
    }
    const qint64 nowMs = now.msecsSinceEpoch();
    c.window.append(qMakePair(nowMs, records));
    qint64 sum = 0;
    for (int i = 0; i < c.window.size();) {
        if (c.window.at(i).first < nowMs - m_settings.recordsWindowMs) {
            c.window.removeAt(i);
        } else {
            sum += c.window.at(i).second;
            ++i;
        }
    }
    if (sum > m_settings.recordsWindowMax) {
        if (!c.windowOver) {
            c.windowOver = true;
            log(link, QStringLiteral("drop"), QStringLiteral("records per window"));
            const Action a = violate(link, ViolationKind::BudgetExceeded, Severity::Soft,
                                     QStringLiteral("records per window"), now);
            if (a.kind == Action::Kill) return a;
        }
        answer = Action::drop();
    } else {
        c.windowOver = false;
    }
    return answer;
}

Action Guard::onStderr(const LinkId &, qint64)
{
    // The driver caps standard error and reports `stderr-flood` once as a violation; there is
    // nothing further to decide per chunk.
    return Action::proceed();
}

// ---- violations, trip --------------------------------------------------------------------

Action Guard::onViolation(const Violation &v)
{
    return violate(v.link, v.kind, v.severity, v.detail, v.time);
}

Action Guard::violate(const LinkId &link, ViolationKind kind, Severity severity,
                      const QString &detail, const UtcTime &when)
{
    Conn &c = conn(link.connectionId, link.adapterId);
    const UtcTime now = nowFor(when);
    const double weight = m_settings.weightOf(kind);
    const double score = c.score.add(now, weight);

    Action answer = Action::proceed();
    if (severity == Severity::Hard)
        answer = Action::kill();                // the process cannot be trusted any more
    else if (kind == ViolationKind::BatchTooLarge || kind == ViolationKind::RateAboveRegistry)
        answer = Action::drop();

    const bool crossing = score >= m_settings.tripLimit && !m_state->connectionDisabled(link.connectionId);
    if (crossing)
        answer = Action::kill();

    log(link, QStringLiteral("violation"),
        QStringLiteral("kind=%1 severity=%2 weight=%3 score=%4 action=%5 %6")
            .arg(kindName(kind), sevName(severity)).arg(weight, 0, 'f', 1).arg(score, 0, 'f', 1)
            .arg(actionName(answer), detail).trimmed());

    if (crossing)
        trip(link, c, score);
    return answer;
}

void Guard::trip(const LinkId &link, Conn &c, double score)
{
    const UtcTime now = m_clock->now();
    m_state->disableConnection(link.connectionId, link.adapterId, link.adapterVersion,
                               StateCause::score(),
                               QStringLiteral("score %1 reached the limit %2")
                                   .arg(score, 0, 'f', 1).arg(m_settings.tripLimit, 0, 'f', 1));
    log(link, QStringLiteral("disabled"),
        QStringLiteral("connection: score %1 >= limit %2").arg(score, 0, 'f', 1)
            .arg(m_settings.tripLimit, 0, 'f', 1));
    if (m_listener)
        m_listener->guardTripped(link.connectionId);

    // Per-version trip (REQ-GRD-07): the same adapter version tripping on enough different
    // connections within the window disables the version everywhere. An empty version (the
    // violation came before `hello`) cannot be counted.
    if (!link.adapterVersion.isEmpty() && !m_state->versionDisabled(link.adapterId, link.adapterVersion)) {
        const int n = m_state->connectionsTrippedWithin(link.adapterId, link.adapterVersion, now,
                                                        m_settings.versionTripWindowMs);
        if (n >= m_settings.versionTripConnections) {
            m_state->disableVersion(link.adapterId, link.adapterVersion,
                                    QStringLiteral("%1 connections tripped").arg(n));
            log(link, QStringLiteral("disabled"),
                QStringLiteral("adapter version: %1 connections tripped").arg(n));
            if (m_listener)
                m_listener->guardVersionTripped(link.adapterId, link.adapterVersion);
        }
    }
    (void)c;
}

// ---- circuit breaker ---------------------------------------------------------------------

void Guard::noteStartFailure(const QString &connection)
{
    Conn &c = conn(connection);
    addFailure(connection, c.adapterId, c.observedVersion, QString());
}

void Guard::addFailure(const QString &connection, const QString &adapter, const QString &version,
                       const QString &sessionId)
{
    Conn &c = conn(connection, adapter);
    const qint64 nowMs = m_clock->now().msecsSinceEpoch();
    if (!sessionId.isEmpty()) {
        if (c.countedSessions.contains(sessionId))
            return;                     // the command and the exit of one process count once
        c.countedSessions.insert(sessionId);
    }
    if (c.trial) {
        // The trial after reopening failed: reopen for twice as long, up to the cap.
        c.trial = false;
        c.circuitLengthMs = qMin(c.circuitLengthMs * 2, m_settings.circuitOpenCapMs);
        c.circuitUntilMs = nowMs + c.circuitLengthMs;
        log(connection, adapter, version, QStringLiteral("circuit-open"),
            QStringLiteral("trial failed, open %1 s").arg(c.circuitLengthMs / 1000));
        return;
    }
    if (c.circuitOpen)
        return;
    c.failures.append(nowMs);
    while (!c.failures.isEmpty() && c.failures.first() < nowMs - m_settings.circuitWindowMs)
        c.failures.removeFirst();
    if (c.failures.size() >= m_settings.circuitFailures) {
        c.circuitOpen = true;
        c.circuitLengthMs = m_settings.circuitOpenMs;
        c.circuitUntilMs = nowMs + c.circuitLengthMs;
        const int n = c.failures.size();
        c.failures.clear();
        log(connection, adapter, version, QStringLiteral("circuit-open"),
            QStringLiteral("%1 failures in %2 s, open %3 s").arg(n)
                .arg(m_settings.circuitWindowMs / 1000).arg(c.circuitLengthMs / 1000));
    }
}

void Guard::closeCircuit(const QString &connection, const QString &adapter, const QString &version,
                         bool announce)
{
    Conn &c = conn(connection, adapter);
    const bool was = c.circuitOpen;
    c.circuitOpen = false;
    c.trial = false;
    c.circuitLengthMs = 0;
    c.circuitUntilMs = 0;
    c.failures.clear();
    if (was && announce)
        log(connection, adapter, version, QStringLiteral("circuit-close"),
            QStringLiteral("a command succeeded"));
}

void Guard::onCommandFinished(const CommandOutcome &o)
{
    Conn &c = conn(o.link.connectionId, o.link.adapterId);
    c.hasExpect = false;
    if (o.result == CommandOutcome::Ok) {
        // Only a fetch that ended ok proves the adapter works: a login that
        // succeeds between two failed fetches must not clear the failure count, or a process
        // that dies at every fetch would never open the circuit.
        if (!o.commandType.startsWith(QLatin1String("fetch")))
            return;
        closeCircuit(o.link.connectionId, o.link.adapterId, o.link.adapterVersion, true);
        return;
    }
    // Only failures of the process count for the circuit breaker; an error class such as
    // `rate-limited` or `invalid-command` is about the source or the core, not the adapter.
    if (o.killedBy != CommandOutcome::NotKilled)
        addFailure(o.link.connectionId, o.link.adapterId, o.link.adapterVersion, o.link.sessionId);
}

// ---- manual re-enable --------------------------------------------------------------------

bool Guard::reenable(const QString &connection)
{
    if (!m_state->connectionDisabled(connection))
        return false;
    Conn &c = conn(connection);
    const QString version = m_state->lastVersionOf(connection);
    m_state->clearConnection(connection, QStringLiteral("re-enabled by the user"));
    c.score.reset();
    closeCircuit(connection, c.adapterId, version, false);
    // No comment is required; the acknowledgement itself is what the log must hold.
    log(connection, c.adapterId, version, QStringLiteral("acknowledgement"),
        QStringLiteral("connection re-enabled"));
    if (m_listener)
        m_listener->guardReenabled(connection);
    return true;
}

bool Guard::reenableVersion(const QString &adapter, const QString &adapterVersion)
{
    if (!m_state->versionDisabled(adapter, adapterVersion))
        return false;
    m_state->clearVersion(adapter, adapterVersion, QStringLiteral("re-enabled by the user"));
    log(QString(), adapter, adapterVersion, QStringLiteral("acknowledgement"),
        QStringLiteral("adapter version re-enabled"));
    if (m_listener)
        m_listener->guardVersionReenabled(adapter, adapterVersion);
    return true;
}

// ---- queries -----------------------------------------------------------------------------

double Guard::scoreOf(const QString &connection) const
{
    QHash<QString, Conn>::const_iterator it = m_conns.constFind(connection);
    return it == m_conns.constEnd() ? 0.0 : it->score.value(m_clock->now());
}

UtcTime Guard::circuitReopenAt(const QString &connection) const
{
    QHash<QString, Conn>::const_iterator it = m_conns.constFind(connection);
    if (it == m_conns.constEnd() || !it->circuitOpen)
        return UtcTime();
    return UtcTime::fromMSecsSinceEpoch(it->circuitUntilMs);
}

UtcTime Guard::spawnReadyAt(const QString &connection) const
{
    QHash<QString, Conn>::const_iterator it = m_conns.constFind(connection);
    if (it == m_conns.constEnd())
        return UtcTime();
    const UtcTime now = m_clock->now();
    const qint64 ms = it->spawn.msUntil(now, 1.0);
    return ms > 0 ? UtcTime::fromMSecsSinceEpoch(now.msecsSinceEpoch() + ms) : UtcTime();
}

} // namespace lm
