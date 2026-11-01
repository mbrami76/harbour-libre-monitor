// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Michele Brami
#include "session/adapterlink.h"

#include "jsonl/jsonlcodec.h"
#include "record/registry.h"
#include "session/processchannel.h"

#include <QJsonArray>
#include <QRegularExpression>
#include <QUuid>

#include <cmath>
#include <limits>
#include <memory>

namespace lm {

namespace {

// Subject UUID used only while validating: the adapter sends subject_ref, the mapping to
// a Subject belongs to the caller. It never leaves the driver.
const char *kValidationSubject = "{00000000-0000-4000-8000-000000000001}";

QString k(const char *s) { return QString::fromLatin1(s); }

bool toInteger(const QJsonValue &value, qint64 *out)
{
    if (!value.isDouble())
        return false;
    const double d = value.toDouble();
    if (!(d == std::floor(d)) || std::fabs(d) > 9007199254740992.0)
        return false;
    *out = static_cast<qint64>(d);
    return true;
}

QString newSessionId()
{
    QString id = QUuid::createUuid().toString();
    id.remove(QLatin1Char('{'));
    id.remove(QLatin1Char('}'));
    return id;
}

const char *denyReasonName(MayStartResult::Reason reason)
{
    switch (reason) {
    case MayStartResult::DisabledByGuard: return "disabled-by-guard";
    case MayStartResult::AdapterVersionDisabled: return "adapter-version-disabled";
    case MayStartResult::CircuitOpen: return "circuit-open";
    case MayStartResult::SpawnRate: return "spawn-rate";
    default: return "denied";
    }
}

int minPositive(int a, int b) { return a < b ? a : b; }

} // namespace

// ---- Names -----------------------------------------------------------------------------

QString errorClassName(ErrorClass errorClass)
{
    switch (errorClass) {
    case ErrorClass::AuthFailed: return k("auth-failed");
    case ErrorClass::TermsMustBeAccepted: return k("terms-must-be-accepted");
    case ErrorClass::RateLimited: return k("rate-limited");
    case ErrorClass::Transient: return k("transient");
    case ErrorClass::ProtocolChanged: return k("protocol-changed");
    case ErrorClass::InvalidCommand: return k("invalid-command");
    }
    return QString();
}

bool errorClassFromName(const QString &name, ErrorClass *out)
{
    static const ErrorClass all[] = {
        ErrorClass::AuthFailed, ErrorClass::TermsMustBeAccepted, ErrorClass::RateLimited,
        ErrorClass::Transient, ErrorClass::ProtocolChanged, ErrorClass::InvalidCommand
    };
    for (size_t i = 0; i < sizeof(all) / sizeof(all[0]); ++i) {
        if (errorClassName(all[i]) == name) {
            if (out)
                *out = all[i];
            return true;
        }
    }
    return false;
}

QString violationKindName(ViolationKind kind)
{
    switch (kind) {
    case ViolationKind::LineTooLong: return k("line-too-long");
    case ViolationKind::MalformedFraming: return k("malformed-framing");
    case ViolationKind::UnknownType: return k("unknown-type");
    case ViolationKind::BadHeader: return k("bad-header");
    case ViolationKind::UndeclaredMetric: return k("undeclared-metric");
    case ViolationKind::UndeclaredUnit: return k("undeclared-unit");
    case ViolationKind::UndeclaredFeed: return k("undeclared-feed");
    case ViolationKind::UnmatchedCorrelation: return k("unmatched-correlation");
    case ViolationKind::DuplicateCompletion: return k("duplicate-completion");
    case ViolationKind::NonProtocolOutput: return k("non-protocol-output");
    case ViolationKind::BatchTooLarge: return k("batch-too-large");
    case ViolationKind::RateAboveRegistry: return k("rate-above-registry");
    case ViolationKind::StderrFlood: return k("stderr-flood");
    case ViolationKind::RecordsSentMismatch: return k("records-sent-mismatch");
    case ViolationKind::BudgetExceeded: return k("budget-exceeded");
    }
    return QString();
}

QString killedByName(CommandOutcome::KilledBy by)
{
    switch (by) {
    case CommandOutcome::NotKilled: return k("none");
    case CommandOutcome::Deadline: return k("deadline");
    case CommandOutcome::Guard: return k("guard");
    case CommandOutcome::Exit: return k("exit");
    case CommandOutcome::DriverLimit: return k("driver-limit");
    }
    return QString();
}

QString quarantineReasonName(Validation::Reason reason)
{
    switch (reason) {
    case Validation::Ok: return k("Ok");
    case Validation::MissingSubject: return k("MissingSubject");
    case Validation::UnknownMetric: return k("UnknownMetric");
    case Validation::ShapeMismatch: return k("ShapeMismatch");
    case Validation::MissingUnit: return k("MissingUnit");
    case Validation::UnitNotAccepted: return k("UnitNotAccepted");
    case Validation::ValueNotFinite: return k("ValueNotFinite");
    case Validation::ValueOutOfRange: return k("ValueOutOfRange");
    case Validation::InvalidPayload: return k("InvalidPayload");
    case Validation::InvalidDuration: return k("InvalidDuration");
    case Validation::MissingMeasurementTime: return k("MissingMeasurementTime");
    case Validation::MeasurementTimeInFuture: return k("MeasurementTimeInFuture");
    case Validation::MissingIngestTime: return k("MissingIngestTime");
    case Validation::MissingOriginalTimestamp: return k("MissingOriginalTimestamp");
    case Validation::MissingSource: return k("MissingSource");
    case Validation::MissingEntryId: return k("MissingEntryId");
    case Validation::MissingRuleVersion: return k("MissingRuleVersion");
    case Validation::UndeclaredMetric: return k("UndeclaredMetric");
    case Validation::UndeclaredUnit: return k("UndeclaredUnit");
    case Validation::UndeclaredFeed: return k("UndeclaredFeed");
    case Validation::MalformedMeasurementTime: return k("MalformedMeasurementTime");
    }
    return QString();
}

// Contract-v0 section 7. BudgetExceeded is not in the table; soft until the scheduler says
// otherwise (the driver never emits it).
Severity severityOf(ViolationKind kind)
{
    switch (kind) {
    case ViolationKind::BatchTooLarge:
    case ViolationKind::RateAboveRegistry:
    case ViolationKind::StderrFlood:
    case ViolationKind::RecordsSentMismatch:
    case ViolationKind::BudgetExceeded:
        return Severity::Soft;
    default:
        return Severity::Hard;
    }
}

// ---- Construction ----------------------------------------------------------------------

AdapterLink::AdapterLink(LinkPolicy *policy, QObject *parent)
    : QObject(parent), m_policy(policy), m_recordSink(0), m_quarantineSink(0), m_rejectedBatchSink(0),
      m_skewMs(kDefaultClockSkewToleranceMs), m_state(Idle), m_generation(0), m_channel(0),
      m_peakResident(-1), m_killedForMemory(false),
      m_inputFailed(false), m_paused(false), m_ids(QStringLiteral("c")),
      m_stderrBytes(0), m_stderrFlooded(false), m_lineBytes(0),
      m_hasLastViolation(false), m_lastViolation(ViolationKind::MalformedFraming)
{
    m_helloTimer.setSingleShot(true);
    m_deadlineTimer.setSingleShot(true);
    m_resumeTimer.setSingleShot(true);
    m_stopTimer.setSingleShot(true);
    connect(&m_memoryTimer, &QTimer::timeout, this, &AdapterLink::onMemoryCheck);
    connect(&m_helloTimer, &QTimer::timeout, this, &AdapterLink::onHelloTimeout);
    connect(&m_deadlineTimer, &QTimer::timeout, this, &AdapterLink::onDeadline);
    connect(&m_resumeTimer, &QTimer::timeout, this, &AdapterLink::onResume);
    connect(&m_stopTimer, &QTimer::timeout, this, [this]() {
        const StopCallback done = m_stopDone;
        m_stopDone = StopCallback();
        terminate(CommandOutcome::Guard, ErrorClass::Transient,
                  QStringLiteral("the adapter did not end in time"), StartResult::SpawnFailed);
        if (done)
            done();
    });
    m_lastActivity.start();
}

AdapterLink::~AdapterLink()
{
    // No callbacks from here on.
    m_helloTimer.stop();
    m_deadlineTimer.stop();
    m_resumeTimer.stop();
    m_stopTimer.stop();
    m_memoryTimer.stop();
    if (m_channel) {
        m_channel->disconnect(this);
        m_channel->kill();
    }
}

void AdapterLink::setProgram(const QString &program, const QStringList &arguments)
{
    m_program = program;
    m_arguments = arguments;
}

UtcTime AdapterLink::now() const
{
    return m_clock ? m_clock() : UtcTime::now();
}

bool AdapterLink::isAlive() const
{
    return m_channel && m_channel->isRunning() && m_state != Closed && m_state != Idle;
}

// ---- Starting --------------------------------------------------------------------------

void AdapterLink::refuseStart(const StartCallback &done, StartResult::RefusalReason reason,
                              const QString &rejectReason, const QString &denyReason,
                              const QString &detail)
{
    StartResult r;
    r.kind = StartResult::Refused;
    r.link = m_link;
    r.refusal = reason;
    r.rejectReason = rejectReason;
    r.denyReason = denyReason;
    if (!done)
        return;
    QTimer::singleShot(0, this, [done, r, detail]() { done(r, detail); });
}

void AdapterLink::start(const QString &adapterId, const QString &connectionId,
                        const QJsonObject &settings, const StartCallback &done)
{
    if (m_state != Idle && m_state != Closed) {
        refuseStart(done, StartResult::SpawnFailed, QString(), QString(),
                    QStringLiteral("the link is already in use"));
        return;
    }

    ++m_generation;
    m_link = LinkId();
    m_link.connectionId = connectionId;
    m_link.adapterId = adapterId;
    m_link.sessionId = newSessionId();
    m_buffer.clear();
    m_inputFailed = false;
    m_paused = false;
    m_ids = MessageIdGenerator(QStringLiteral("c"));
    m_completed.clear();
    m_pending = Pending();
    m_stderrBytes = 0;
    m_stderrFlooded = false;
    m_diagnostics.clear();
    m_feeds.clear();
    m_declared.clear();
    m_admission = StartResult();
    m_settings = settings;
    m_stopDone = StopCallback();
    m_startDone = done;

    const AdapterEntry *entry = AdapterRegistry::instance().find(adapterId);
    if (!entry) {
        m_state = Closed;
        m_startDone = StartCallback();
        refuseStart(done, StartResult::AdmissionRejected, QStringLiteral("unknown_adapter"),
                    QString(), QStringLiteral("the adapter is not in the built-in registry"));
        return;
    }

    StartRequest request;
    request.connectionId = connectionId;
    request.adapterId = adapterId;
    const MayStartResult may = m_policy->mayStart(request);
    if (may.kind == MayStartResult::Denied) {
        m_state = Closed;
        m_startDone = StartCallback();
        refuseStart(done, StartResult::Denied, QString(),
                    QString::fromLatin1(denyReasonName(may.reason)),
                    QStringLiteral("the policy does not allow a start now"));
        return;
    }

    m_limits = m_policy->limits();
    m_peakResident = -1;
    m_killedForMemory = false;
    m_budgets.maxLineBytes = m_limits.maxLineBytes;
    m_budgets.maxRecordsPerBatch = m_limits.maxRecordsPerBatch;

    LinkChannel *channel = 0;
    if (m_factory) {
        channel = m_factory(*entry);
    } else if (!m_program.isEmpty()) {
        ProcessChannel *process = new ProcessChannel(m_program, m_arguments);
        OsLimits os;
        os.memoryBytes = m_limits.memoryLimitBytes;
        os.cpuSeconds = m_limits.cpuTimeLimitS;
        os.fileDescriptors = m_limits.fileDescriptorLimit;
        process->setOsLimits(os);
        channel = process;
    }
    if (!channel) {
        m_state = Closed;
        m_startDone = StartCallback();
        refuseStart(done, StartResult::SpawnFailed, QString(), QString(),
                    QStringLiteral("no adapter program is set"));
        return;
    }

    channel->setParent(this);
    m_channel = channel;
    // Every signal is tied to the channel it came from and ignored once that channel is no
    // longer the current one. A process that was killed or has ended may still
    // deliver a last notification, some stderr or some output; if the link has meanwhile been
    // started again, such a leftover must never be taken for news from the new process
    // (it would move the new start on, or be counted against the new link's budgets).
    connect(channel, &LinkChannel::started, this,
            [this, channel]() { if (channel == m_channel) onChannelStarted(); });
    connect(channel, &LinkChannel::failedToStart, this,
            [this, channel](const QString &reason) { if (channel == m_channel) onChannelFailed(reason); });
    connect(channel, &LinkChannel::stderrData, this,
            [this, channel](const QByteArray &data) { if (channel == m_channel) onStderr(data); });
    connect(channel, &LinkChannel::finished, this,
            [this, channel](int exitCode, bool crashed) { onChannelFinished(channel, exitCode, crashed); });
    connect(channel->fromAdapter(), &QIODevice::readyRead, this,
            [this, channel]() { if (channel == m_channel) onReadable(); });

    m_state = Spawning;
    m_helloTimer.start(m_limits.helloTimeoutMs);
    channel->open();
}

void AdapterLink::onChannelStarted()
{
    if (m_state != Spawning)
        return;
    m_state = WaitHello;
    // From here the adapter's size is watched. Only when a limit is set: a policy that
    // sets none (a test, a null policy) pays for no timer and no /proc reads.
    if (m_limits.residentLimitBytes > 0 && m_limits.memoryCheckIntervalMs > 0)
        m_memoryTimer.start(m_limits.memoryCheckIntervalMs);
    m_policy->onSpawned(m_link);
    // Whatever the adapter already wrote (its hello, usually) was kept back by onReadable().
    processBuffer();
}

void AdapterLink::onChannelFailed(const QString &reason)
{
    if (m_state != Spawning)
        return;
    LinkChannel *channel = m_channel;
    m_channel = 0;
    m_state = Closed;
    m_helloTimer.stop();
    if (channel)
        channel->deleteLater();
    StartCallback cb = m_startDone;
    m_startDone = StartCallback();
    StartResult r;
    r.kind = StartResult::Refused;
    r.link = m_link;
    r.refusal = StartResult::SpawnFailed;
    if (cb)
        cb(r, scrub(reason));
}

void AdapterLink::sampleMemory()
{
    if (!m_channel)
        return;
    const qint64 now = m_channel->peakResidentBytes();
    if (now > m_peakResident)
        m_peakResident = now;
}

// The resident-size limit. The adapter is untrusted and lives for a day;
// a leak or a runaway must not eat the phone's memory. The check is the parent's, from the
// outside, and the ending is the driver's own rule, like a line over the length limit: the
// command ends as a driver-limit kill, not as a guard kill, and the guard hears of the
// exit with the peak size and the reason (ExitStatus).
void AdapterLink::onMemoryCheck()
{
    sampleMemory();
    if (m_limits.residentLimitBytes <= 0 || m_peakResident <= m_limits.residentLimitBytes)
        return;
    m_killedForMemory = true;
    terminate(CommandOutcome::DriverLimit, ErrorClass::ProtocolChanged,
              QStringLiteral("the adapter passed the resident memory limit"), StartResult::SpawnFailed);
}

void AdapterLink::onHelloTimeout()
{
    terminate(CommandOutcome::Deadline, ErrorClass::Transient,
              QStringLiteral("no hello within the time limit"), StartResult::HelloTimeout);
}

// ---- Output ----------------------------------------------------------------------------

void AdapterLink::send(MessageType type, const QString &id, const QString &correlation,
                       const QJsonObject &payload)
{
    if (!m_channel)
        return;
    const QJsonObject message = buildMessage(type, id, correlation, payload);
    const QByteArray line = encodeJsonLine(message);
    if (line.isEmpty())
        return;     // the core's own message over 1 MiB: not possible with the commands of v0
    m_channel->toAdapter()->write(line);
}

void AdapterLink::runCommand(MessageType type, QJsonObject payload, int deadlineMs,
                             bool acceptsRecords,
                             const std::function<bool(const QJsonObject &)> &onReply,
                             const DoneCallback &done)
{
    Pending p;
    p.active = true;
    p.type = type;
    p.id = m_ids.next();
    p.acceptsRecords = acceptsRecords;
    p.onReply = onReply;
    p.done = done;
    p.outcome.link = m_link;
    p.outcome.commandId = p.id;
    p.outcome.commandType = messageTypeName(type);
    p.clock.start();
    m_pending = p;
    if (type != MessageType::Configure)
        payload.insert(QStringLiteral("deadline_ms"), deadlineMs);
    m_deadlineTimer.start(deadlineMs);
    send(type, p.id, QString(), payload);
}

void AdapterLink::failImmediately(MessageType type, const DoneCallback &done,
                                  ErrorClass errorClass, const QString &detail)
{
    CommandReport report;
    report.outcome.link = m_link;
    report.outcome.commandType = messageTypeName(type);
    report.outcome.result = CommandOutcome::Error;
    report.outcome.errorClass = errorClass;
    report.detail = detail;
    m_policy->onCommandFinished(report.outcome);
    if (done)
        QTimer::singleShot(0, this, [done, report]() { done(report); });
}

void AdapterLink::authenticate(const Credentials &credentials, const QString &session,
                               const AuthCallback &done)
{
    DoneCallback fail = [done](const CommandReport &r) { if (done) done(r, SessionGrant()); };
    if (m_state != Ready) {
        failImmediately(MessageType::Authenticate, fail, ErrorClass::InvalidCommand,
                        QStringLiteral("the link is not ready for a command"));
        return;
    }
    m_scrubber.add(credentials.username);
    m_scrubber.add(credentials.password);
    m_scrubber.add(session);

    QJsonObject creds;
    creds.insert(QStringLiteral("username"), credentials.username);
    creds.insert(QStringLiteral("password"), credentials.password);
    QJsonObject payload;
    payload.insert(QStringLiteral("credentials"), creds);
    payload.insert(QStringLiteral("session"),
                   session.isEmpty() ? QJsonValue(QJsonValue::Null) : QJsonValue(session));

    std::shared_ptr<SessionGrant> grant(new SessionGrant);
    m_state = Busy;
    runCommand(MessageType::Authenticate, payload, m_limits.authenticateDeadlineMs, false,
               [this, grant](const QJsonObject &reply) {
                   const QJsonValue s = reply.value(QStringLiteral("session"));
                   if (!s.isString())
                       return false;
                   grant->session = s.toString();
                   m_scrubber.add(grant->session);
                   const QJsonValue exp = reply.value(QStringLiteral("session_expires"));
                   if (exp.isString()) {
                       grant->expiresAt = UtcTime::fromIso(exp.toString());
                       if (!grant->expiresAt.isValid())
                           return false;
                   } else if (!exp.isNull() && !exp.isUndefined()) {
                       return false;
                   }
                   const QJsonValue upd = reply.value(QStringLiteral("settings_update"));
                   if (upd.isObject())
                       grant->settingsUpdate = upd.toObject();
                   else if (!upd.isNull() && !upd.isUndefined())
                       return false;
                   return true;
               },
               [done, grant](const CommandReport &r) {
                   if (done)
                       done(r, r.outcome.result == CommandOutcome::Ok ? *grant : SessionGrant());
               });
}

void AdapterLink::discoverSubjects(const DiscoverCallback &done)
{
    DoneCallback fail = [done](const CommandReport &r) { if (done) done(r, QList<SubjectInfo>()); };
    if (m_state != Ready) {
        failImmediately(MessageType::DiscoverSubjects, fail, ErrorClass::InvalidCommand,
                        QStringLiteral("the link is not ready for a command"));
        return;
    }
    std::shared_ptr<QList<SubjectInfo> > subjects(new QList<SubjectInfo>);
    m_state = Busy;
    runCommand(MessageType::DiscoverSubjects, QJsonObject(), m_limits.discoverSubjectsDeadlineMs,
               false,
               [this, subjects](const QJsonObject &reply) {
                   const QJsonValue list = reply.value(QStringLiteral("subjects"));
                   if (!list.isArray())
                       return false;
                   const QJsonArray array = list.toArray();
                   for (int i = 0; i < array.size(); ++i) {
                       if (!array.at(i).isObject())
                           return false;
                       const QJsonObject o = array.at(i).toObject();
                       SubjectInfo s;
                       if (!o.value(QStringLiteral("subject_ref")).isString()
                           || !o.value(QStringLiteral("display_name")).isString())
                           return false;
                       s.subjectRef = o.value(QStringLiteral("subject_ref")).toString();
                       s.displayName = o.value(QStringLiteral("display_name")).toString();
                       const QJsonValue unit = o.value(QStringLiteral("unit"));
                       if (unit.isString())
                           s.unit = unit.toString();
                       else if (!unit.isNull() && !unit.isUndefined())
                           return false;
                       const QJsonValue basis = o.value(QStringLiteral("unit_basis"));
                       if (!basis.isString())
                           return false;
                       s.unitBasis = basis.toString();
                       if (s.subjectRef.isEmpty()
                           || (s.unitBasis != QLatin1String("code")
                               && s.unitBasis != QLatin1String("relation")
                               && s.unitBasis != QLatin1String("magnitude")
                               && s.unitBasis != QLatin1String("none")))
                           return false;
                       subjects->append(s);
                   }
                   for (int i = 0; i < subjects->size(); ++i)
                       m_scrubber.add(subjects->at(i).displayName, 3);     // personal data
                   return true;
               },
               [done, subjects](const CommandReport &r) {
                   if (done)
                       done(r, r.outcome.result == CommandOutcome::Ok ? *subjects : QList<SubjectInfo>());
               });
}

namespace {

// The part of a fetch completion that does not depend on the counters of the command.
bool parseFetchReply(const QJsonObject &reply, FetchCompletion *out)
{
    qint64 sent = 0;
    if (!toInteger(reply.value(QStringLiteral("records_sent")), &sent) || sent < 0)
        return false;
    out->recordsSent = sent;

    const QJsonValue wm = reply.value(QStringLiteral("watermark"));
    if (wm.isString()) {
        out->watermark = UtcTime::fromIso(wm.toString());
        if (!out->watermark.isValid())
            return false;
    } else if (!wm.isNull() && !wm.isUndefined()) {
        return false;
    }

    const QJsonValue cov = reply.value(QStringLiteral("coverage"));
    if (!cov.isArray())
        return false;
    const QJsonArray array = cov.toArray();
    for (int i = 0; i < array.size(); ++i) {
        if (!array.at(i).isObject())
            return false;
        const QJsonObject o = array.at(i).toObject();
        FeedCoverage c;
        if (!o.value(QStringLiteral("feed")).isString()
            || !o.value(QStringLiteral("from")).isString()
            || !o.value(QStringLiteral("to")).isString())
            return false;
        c.feed = o.value(QStringLiteral("feed")).toString();
        c.from = UtcTime::fromIso(o.value(QStringLiteral("from")).toString());
        c.to = UtcTime::fromIso(o.value(QStringLiteral("to")).toString());
        if (!c.from.isValid() || !c.to.isValid())
            return false;
        out->coverage.append(c);
    }

    const QJsonValue sensor = reply.value(QStringLiteral("sensor"));
    if (sensor.isObject())
        out->sensor = sensor.toObject();
    else if (!sensor.isNull() && !sensor.isUndefined())
        return false;

    // The trend arrow: an integer or null, nothing else. The adapter is untrusted, so
    // a value of another type, a fraction or one outside the range of an int makes the reply
    // malformed, like a bad sensor object. The number itself is never interpreted here.
    const QJsonValue trend = reply.value(QStringLiteral("trend"));
    if (!trend.isNull() && !trend.isUndefined()) {
        qint64 n = 0;
        if (!toInteger(trend, &n) || n < std::numeric_limits<int>::min()
            || n > std::numeric_limits<int>::max())
            return false;
        out->hasTrend = true;
        out->trend = int(n);
    }
    return true;
}

} // namespace

void AdapterLink::fetchCurrent(const QString &subjectRef, const FetchCallback &done)
{
    DoneCallback fail = [done](const CommandReport &r) { if (done) done(r, FetchCompletion()); };
    if (m_state != Ready) {
        failImmediately(MessageType::FetchCurrent, fail, ErrorClass::InvalidCommand,
                        QStringLiteral("the link is not ready for a command"));
        return;
    }
    QJsonObject payload;
    payload.insert(QStringLiteral("subject_ref"), subjectRef);
    std::shared_ptr<FetchCompletion> completion(new FetchCompletion);
    m_state = Busy;
    runCommand(MessageType::FetchCurrent, payload, m_limits.fetchCurrentDeadlineMs, true,
               [completion](const QJsonObject &reply) { return parseFetchReply(reply, completion.get()); },
               [done, completion](const CommandReport &r) {
                   if (done)
                       done(r, r.outcome.result == CommandOutcome::Ok ? *completion : FetchCompletion());
               });
}

void AdapterLink::fetchRange(const QString &subjectRef, const UtcTime &from, const UtcTime &to,
                             const QStringList &feeds, const FetchCallback &done)
{
    DoneCallback fail = [done](const CommandReport &r) { if (done) done(r, FetchCompletion()); };
    if (m_state != Ready) {
        failImmediately(MessageType::FetchRange, fail, ErrorClass::InvalidCommand,
                        QStringLiteral("the link is not ready for a command"));
        return;
    }
    if (!from.isValid() || !to.isValid() || !(from < to)) {
        failImmediately(MessageType::FetchRange, fail, ErrorClass::InvalidCommand,
                        QStringLiteral("the range is not valid (from must be before to)"));
        return;
    }
    QJsonObject payload;
    payload.insert(QStringLiteral("subject_ref"), subjectRef);
    payload.insert(QStringLiteral("from"), from.toIso());
    payload.insert(QStringLiteral("to"), to.toIso());
    if (!feeds.isEmpty()) {
        QJsonArray array;
        for (int i = 0; i < feeds.size(); ++i)
            array.append(feeds.at(i));
        payload.insert(QStringLiteral("feeds"), array);
    }
    std::shared_ptr<FetchCompletion> completion(new FetchCompletion);
    m_state = Busy;
    runCommand(MessageType::FetchRange, payload, m_limits.fetchRangeDeadlineMs, true,
               [completion](const QJsonObject &reply) { return parseFetchReply(reply, completion.get()); },
               [done, completion](const CommandReport &r) {
                   if (done)
                       done(r, r.outcome.result == CommandOutcome::Ok ? *completion : FetchCompletion());
               });
}

// ---- Input -----------------------------------------------------------------------------

void AdapterLink::onReadable()
{
    if (!m_channel || m_inputFailed)
        return;
    m_buffer.append(m_channel->fromAdapter()->readAll());
    // the cause of the false "hello outside the start" violation. A process
    // is up when its first output arrives, but the platform reports "started" through a
    // different notification, and the event loop may deliver the output first (both become
    // ready in one pass; a busy loop makes that likely). The output is the answer to the
    // spawn, so it must not be judged before the start is acknowledged: keep the bytes and
    // let onChannelStarted() process them, in order, once the state is WaitHello. A buffer
    // already over the line limit is processed at once so the length rule still applies.
    if (m_state == Spawning && m_buffer.size() <= m_budgets.maxLineBytes)
        return;
    processBuffer();
}

void AdapterLink::onResume()
{
    m_paused = false;
    processBuffer();
}

void AdapterLink::processBuffer()
{
    const quint64 generation = m_generation;
    while (!m_paused && !m_inputFailed && generation == m_generation
           && m_state != Closed && m_state != Idle && m_channel) {
        const int nl = m_buffer.indexOf('\n');
        const int length = nl < 0 ? m_buffer.size() : nl;
        if (length > m_budgets.maxLineBytes) {
            m_inputFailed = true;
            m_buffer.clear();
            // The driver kills here by its own rule, whatever the policy answers: the
            // answer is ignored so that killed_by says driver-limit, not guard. If the
            // policy had already killed on an earlier event, terminate() does nothing.
            report(ViolationKind::LineTooLong, QStringLiteral("line over max_line_bytes"), false);
            terminate(CommandOutcome::DriverLimit, ErrorClass::ProtocolChanged,
                      QStringLiteral("a line was over the length limit"), StartResult::SpawnFailed);
            return;
        }
        if (nl < 0)
            break;
        const QByteArray line = m_buffer.left(nl);
        m_buffer.remove(0, nl + 1);
        handleLine(line, nl + 1);
    }
}

bool AdapterLink::applyAction(const Action &action)
{
    switch (action.kind) {
    case Action::Drop:
        return false;
    case Action::Kill:
        terminate(CommandOutcome::Guard, ErrorClass::ProtocolChanged,
                  QStringLiteral("ended by the guard"), StartResult::SpawnFailed);
        return false;
    case Action::Pause:
        m_paused = true;
        m_resumeTimer.start(action.pauseMs > 0 ? action.pauseMs : 1);
        return true;
    default:
        return true;
    }
}

void AdapterLink::handleLine(const QByteArray &rawLine, qint64 bytes)
{
    QByteArray line = rawLine;
    if (line.endsWith('\r'))
        line.chop(1);
    if (line.trimmed().isEmpty())
        return;

    if (m_pending.active) {
        ++m_pending.outcome.linesRead;
        m_pending.outcome.bytesRead += bytes;
    }
    m_lineBytes = bytes;
    if (!applyAction(m_policy->onLine(m_link, bytes, now())))
        return;

    JsonLinesDecoder decoder(m_budgets.maxLineBytes + 16);
    const QVector<JsonLinesItem> items = decoder.feed(line + '\n');
    if (items.isEmpty())
        return;
    const JsonLinesItem &item = items.at(0);
    if (item.kind != JsonLinesItem::Message) {
        // Something that does not even start like a JSON object is not a protocol line.
        if (!line.trimmed().startsWith('{'))
            report(ViolationKind::NonProtocolOutput, QStringLiteral("output that is not a protocol line"));
        else
            report(ViolationKind::MalformedFraming, QStringLiteral("line is not valid JSON or UTF-8"));
        return;
    }

    Envelope env;
    EnvelopeError error;
    if (!parseEnvelope(item.message, &env, &error)) {
        report(error.code == EnvelopeError::UnknownType ? ViolationKind::UnknownType
                                                        : ViolationKind::BadHeader,
               error.detail);
        return;
    }

    switch (env.type) {
    case MessageType::Hello:
        if (m_state == WaitHello)
            handleHello(env);
        else
            report(ViolationKind::UnknownType, QStringLiteral("hello outside the start"));
        break;
    case MessageType::Log:
        handleLog(env);
        break;
    case MessageType::Records:
    case MessageType::Reply:
    case MessageType::Error: {
        if (!m_pending.active || env.correlationId != m_pending.id) {
            if (env.type != MessageType::Records && m_completed.contains(env.correlationId))
                report(ViolationKind::DuplicateCompletion, QStringLiteral("second completion for a command"));
            else
                report(ViolationKind::UnmatchedCorrelation,
                       QStringLiteral("correlation_id matches no open command"));
            break;
        }
        if (env.type == MessageType::Records)
            handleRecords(env, bytes, rawLine);
        else
            handleCompletion(env);
        break;
    }
    default:
        report(ViolationKind::UnknownType, QStringLiteral("message type not allowed from an adapter"));
        break;
    }
}

void AdapterLink::handleLog(const Envelope &env)
{
    const QJsonValue level = env.fields.value(QStringLiteral("level"));
    const QJsonValue text = env.fields.value(QStringLiteral("text"));
    const QString l = level.toString();
    if (!level.isString() || !text.isString()
        || (l != QLatin1String("debug") && l != QLatin1String("info")
            && l != QLatin1String("warn") && l != QLatin1String("error"))) {
        report(ViolationKind::MalformedFraming, QStringLiteral("log payload"));
        return;
    }
    addDiagnostic(QStringLiteral("log"), l, scrub(text.toString()));
}

void AdapterLink::addDiagnostic(const QString &source, const QString &level, const QString &text)
{
    m_diagnostics.append(source + QLatin1Char(' ') + level + QLatin1String(": ") + text);
    while (m_diagnostics.size() > kMaxDiagnosticLines)
        m_diagnostics.removeFirst();
    emit diagnostic(source, level, text);
}

void AdapterLink::onStderr(const QByteArray &data)
{
    const qint64 size = data.size();
    const bool proceed = applyAction(m_policy->onStderr(m_link, size));
    m_stderrBytes += size;
    if (m_stderrBytes > m_limits.maxStderrBytes) {
        if (!m_stderrFlooded) {
            m_stderrFlooded = true;
            report(ViolationKind::StderrFlood, QStringLiteral("standard error over the byte cap"));
        }
        return;
    }
    if (!proceed)
        return;
    const QStringList lines = QString::fromUtf8(data).split(QLatin1Char('\n'), QString::SkipEmptyParts);
    for (int i = 0; i < lines.size(); ++i)
        addDiagnostic(QStringLiteral("stderr"), QString(), scrub(lines.at(i).trimmed()));
}

// ---- Admission (REQ-ADM-01) ------------------------------------------------------------

bool AdapterLink::admit(const Envelope &env, StartResult *result, QString *rejectReason,
                        QString *detail)
{
    const QJsonObject &f = env.fields;
    const AdapterRegistry &registry = AdapterRegistry::instance();
    *rejectReason = QStringLiteral("bad_manifest");

    const QJsonValue adapter = f.value(QStringLiteral("adapter"));
    if (!adapter.isString()) {
        *detail = QStringLiteral("adapter");
        return false;
    }
    if (adapter.toString() != m_link.adapterId || !registry.find(adapter.toString())) {
        *rejectReason = QStringLiteral("unknown_adapter");
        *detail = QStringLiteral("adapter is not the one requested or not in the registry");
        return false;
    }

    const QJsonValue av = f.value(QStringLiteral("adapter_version"));
    if (!av.isString() || av.toString().isEmpty()) {
        *detail = QStringLiteral("adapter_version");
        return false;
    }
    qint64 contract = 0;
    if (!toInteger(f.value(QStringLiteral("contract_version")), &contract)) {
        *detail = QStringLiteral("contract_version");
        return false;
    }
    if (contract != kProtocolVersion) {
        *rejectReason = QStringLiteral("version");
        *detail = QStringLiteral("contract_version is not supported");
        return false;
    }

    // Metrics: shape first, then the registry (REQ-ADM-01).
    const QJsonValue metrics = f.value(QStringLiteral("metrics"));
    if (!metrics.isArray() || metrics.toArray().isEmpty()) {
        *detail = QStringLiteral("metrics");
        return false;
    }
    QList<FeedDeclaration> declared;
    const QJsonArray metricArray = metrics.toArray();
    for (int i = 0; i < metricArray.size(); ++i) {
        if (!metricArray.at(i).isObject()) {
            *detail = QStringLiteral("metrics");
            return false;
        }
        const QJsonObject m = metricArray.at(i).toObject();
        if (!m.value(QStringLiteral("metric")).isString() || !m.value(QStringLiteral("units")).isArray()) {
            *detail = QStringLiteral("metrics");
            return false;
        }
        FeedDeclaration d;
        d.metric = m.value(QStringLiteral("metric")).toString();
        const QJsonArray units = m.value(QStringLiteral("units")).toArray();
        for (int u = 0; u < units.size(); ++u) {
            if (!units.at(u).isString()) {
                *detail = QStringLiteral("metrics");
                return false;
            }
            d.units << units.at(u).toString();
        }
        for (int j = 0; j < declared.size(); ++j) {
            if (declared.at(j).metric == d.metric) {
                *detail = QStringLiteral("metrics: a metric is declared twice");
                return false;
            }
        }
        declared.append(d);
    }
    for (int i = 0; i < declared.size(); ++i) {
        const MetricDef *def = MetricRegistry::instance().find(declared.at(i).metric);
        if (!def) {
            *rejectReason = QStringLiteral("metric");
            *detail = QStringLiteral("a declared metric is not in the registry");
            return false;
        }
        for (int u = 0; u < declared.at(i).units.size(); ++u) {
            const QString &unit = declared.at(i).units.at(u);
            const bool accepted = def->shape == MetricDef::Sample
                ? def->findUnit(unit) != 0
                : MetricRegistry::instance().isKnownUnit(unit);
            if (!accepted) {
                *rejectReason = QStringLiteral("unit");
                *detail = QStringLiteral("a declared unit is not accepted by the registry");
                return false;
            }
        }
    }

    // Feeds.
    const QJsonValue feedsValue = f.value(QStringLiteral("feeds"));
    if (!feedsValue.isArray() || feedsValue.toArray().isEmpty()) {
        *detail = QStringLiteral("feeds");
        return false;
    }
    QStringList feeds;
    static const QRegularExpression feedName(QStringLiteral("^[A-Za-z0-9_-]{1,32}$"));
    const QJsonArray feedArray = feedsValue.toArray();
    for (int i = 0; i < feedArray.size(); ++i) {
        if (!feedArray.at(i).isString()) {
            *detail = QStringLiteral("feeds");
            return false;
        }
        const QString feed = feedArray.at(i).toString();
        if (!feedName.match(feed).hasMatch() || feeds.contains(feed)) {
            *rejectReason = QStringLiteral("feed");
            *detail = QStringLiteral("a feed name is invalid or declared twice");
            return false;
        }
        feeds << feed;
    }

    // Budgets: the lower of request and ceiling; a request above the ceiling is rejected.
    Budgets budgets;
    budgets.maxLineBytes = m_limits.maxLineBytes;
    budgets.maxRecordsPerBatch = m_limits.maxRecordsPerBatch;
    const QJsonValue requested = f.value(QStringLiteral("budgets"));
    if (!requested.isNull() && !requested.isUndefined()) {
        if (!requested.isObject()) {
            *detail = QStringLiteral("budgets");
            return false;
        }
        const QJsonObject b = requested.toObject();
        const char *keys[] = { "max_records_per_batch", "max_line_bytes" };
        int *targets[] = { &budgets.maxRecordsPerBatch, &budgets.maxLineBytes };
        for (int i = 0; i < 2; ++i) {
            const QString key = QString::fromLatin1(keys[i]);
            if (!b.contains(key))
                continue;
            qint64 v = 0;
            if (!toInteger(b.value(key), &v) || v < 1 || v > *targets[i]) {
                *rejectReason = QStringLiteral("budget");
                *detail = key + QStringLiteral(" is not valid or is above the ceiling");
                return false;
            }
            *targets[i] = minPositive(static_cast<int>(v), *targets[i]);
        }
    }

    m_link.adapterVersion = av.toString();
    m_feeds = feeds;
    m_declared = declared;
    m_budgets = budgets;

    result->kind = StartResult::Admitted;
    result->link = m_link;
    result->budgets = budgets;
    result->feeds = feeds;
    result->metrics = declared;
    return true;
}

void AdapterLink::rejectHello(const Envelope &env, const QString &reason, const QString &detail)
{
    QJsonObject payload;
    payload.insert(QStringLiteral("reason"), reason);
    payload.insert(QStringLiteral("detail"), detail);
    send(MessageType::Reject, m_ids.next(), env.id, payload);

    const StartCallback cb = m_startDone;
    m_startDone = StartCallback();
    StartResult r;
    r.kind = StartResult::Refused;
    r.link = m_link;
    r.refusal = StartResult::AdmissionRejected;
    r.rejectReason = reason;
    beginClose(false, StopCallback());
    if (cb)
        cb(r, detail);
}

void AdapterLink::handleHello(const Envelope &env)
{
    m_helloTimer.stop();
    m_helloId = env.id;
    StartResult admitted;
    QString rejectReason, detail;
    if (!admit(env, &admitted, &rejectReason, &detail)) {
        rejectHello(env, rejectReason, detail);
        return;
    }
    m_admission = admitted;

    // Second policy check: only now is the adapter version known. A denial
    // ends the process the polite way (end-of-session message, wait, kill) and completes
    // the start as Refused(denied); the link is Stopping, so no command can follow.
    const MayStartResult again = m_policy->onAdmitted(m_link);
    if (again.kind == MayStartResult::Denied) {
        const StartCallback cb = m_startDone;
        m_startDone = StartCallback();
        StartResult r;
        r.kind = StartResult::Refused;
        r.link = m_link;
        r.refusal = StartResult::Denied;
        r.denyReason = QString::fromLatin1(denyReasonName(again.reason));
        beginClose(true, StopCallback());
        if (cb)
            cb(r, QStringLiteral("the policy does not allow this adapter version"));
        return;
    }
    send(MessageType::Accept, m_ids.next(), env.id);

    m_state = WaitConfigure;
    QJsonObject payload;
    payload.insert(QStringLiteral("settings"), m_settings);
    runCommand(MessageType::Configure, payload, m_limits.helloTimeoutMs, false,
               std::function<bool(const QJsonObject &)>(),
               [this](const CommandReport &r) { configureDone(r); });
}

void AdapterLink::configureDone(const CommandReport &report)
{
    if (m_state != WaitConfigure)
        return;
    if (report.outcome.result != CommandOutcome::Ok) {
        terminate(CommandOutcome::NotKilled, report.outcome.errorClass,
                  QStringLiteral("configure failed"), StartResult::SpawnFailed);
        return;
    }
    m_state = Ready;
    m_lastActivity.restart();
    const StartCallback cb = m_startDone;
    m_startDone = StartCallback();
    if (cb)
        cb(m_admission, QString());
}

// ---- Records ---------------------------------------------------------------------------

// One `records` message. `rawLine` is the protocol line exactly as received (no line end);
// it is kept only to hand it to the RejectedBatchSink when the process is ended in the
// middle of this batch (REQ-GRD-13). The adapter is not trusted for such a batch:
// nothing of it reaches the RecordSink, but what was quarantined before the kill stays.
void AdapterLink::handleRecords(const Envelope &env, qint64 bytes, const QByteArray &rawLine)
{
    const QJsonValue feedValue = env.fields.value(QStringLiteral("feed"));
    const QJsonValue recordsValue = env.fields.value(QStringLiteral("records"));
    if (!m_pending.acceptsRecords) {
        report(ViolationKind::UnmatchedCorrelation, QStringLiteral("records outside a fetch"));
        return;
    }
    if (!feedValue.isString() || !recordsValue.isArray()) {
        // No usable `feed` or `records`: not a batch we can reason about (a batch without a
        // `feed` key stays a framing violation), so there is nothing to hand over.
        report(ViolationKind::MalformedFraming, QStringLiteral("records payload"));
        return;
    }
    const QString feed = feedValue.toString();
    const QJsonArray array = recordsValue.toArray();
    const quint64 generation = m_generation;
    m_hasLastViolation = false;
    if (!m_feeds.contains(feed)) {
        // A batch for a feed that `hello` did not declare (as for a single record in):
        // every record is quarantined as received, with UndeclaredFeed, and the
        // violation is reported once for the batch. Quarantine first, so the entries exist
        // and are counted even if the policy kills on the violation. Entries that are not
        // objects cannot be quarantined (the sink takes the object as received) and are
        // left out; the one violation covers them. The batch size and the policy's batch
        // check are not applied: the feed is unknown, there is nothing to account against.
        ++m_pending.outcome.batches;
        m_pending.outcome.recordsReceived += array.size();
        for (int i = 0; i < array.size(); ++i)
            if (array.at(i).isObject())
                quarantine(array.at(i).toObject(), Validation::UndeclaredFeed, k("feed"));
        report(ViolationKind::UndeclaredFeed, QStringLiteral("batch feed not declared in hello"));
        if (ended(generation))
            deliverRejectedBatch(rawLine);
        return;
    }
    if (array.size() > m_budgets.maxRecordsPerBatch) {
        report(ViolationKind::BatchTooLarge, QStringLiteral("batch above max_records_per_batch"));
        if (ended(generation)) {
            deliverRejectedBatch(rawLine);
            return;
        }
    }
    if (!applyAction(m_policy->onBatch(m_link, feed, array.size(), bytes))) {
        // Drop leaves the link alive and delivers nothing; Kill ends it: only then is
        // there a rejected batch to hand over.
        if (ended(generation))
            deliverRejectedBatch(rawLine);
        return;
    }

    ++m_pending.outcome.batches;
    m_pending.outcome.recordsReceived += array.size();
    QVector<LinkRecord> valid;
    for (int i = 0; i < array.size(); ++i) {
        if (!array.at(i).isObject()) {
            report(ViolationKind::MalformedFraming, QStringLiteral("a record is not an object"));
            if (ended(generation)) {
                deliverRejectedBatch(rawLine);
                return;
            }
            continue;
        }
        processRecord(array.at(i).toObject(), feed, &valid);
        // The policy may have ended the adapter because of this record. Whatever was
        // valid before it in the same batch is discarded with the rest: an adapter that
        // was killed for breaking the contract is not trusted for the data it sent in
        // that batch, and the command has already finished with its outcome.
        if (ended(generation)) {
            deliverRejectedBatch(rawLine);
            return;
        }
    }
    m_pending.outcome.recordsValid += valid.size();
    if (!valid.isEmpty() && m_recordSink)
        m_recordSink->accept(m_link, feed, valid);
}

// Hands the held line to the optional sink, once. Called only after the link ended inside
// handleRecords, which in practice means the policy answered Kill (a deadline or an exit
// cannot fire in the middle of a handler on the single event-loop thread).
void AdapterLink::deliverRejectedBatch(const QByteArray &rawLine)
{
    if (m_rejectedBatchSink)
        m_rejectedBatchSink->rejected(m_link, rawLine, CommandOutcome::Guard,
                                      m_hasLastViolation, m_lastViolation);
}

void AdapterLink::quarantine(const QJsonObject &raw, Validation::Reason reason, const QString &detail)
{
    ++m_pending.outcome.quarantinedByReason[static_cast<int>(reason)];
    if (m_quarantineSink)
        m_quarantineSink->reject(m_link, quarantineReasonName(reason), detail, raw);
}

void AdapterLink::processRecord(const QJsonObject &raw, const QString &feed, QVector<LinkRecord> *valid)
{
    Q_UNUSED(feed);
    typedef Validation V;
    const QString kMetric = k("metric"), kSource = k("source"), kUnit = k("unit"),
                  kPayload = k("payload");

    if (!raw.value(kMetric).isString()) {
        quarantine(raw, V::UnknownMetric, k("metric"));
        return;
    }
    const QString metric = raw.value(kMetric).toString();
    const FeedDeclaration *decl = 0;
    for (int i = 0; i < m_declared.size(); ++i)
        if (m_declared.at(i).metric == metric)
            decl = &m_declared.at(i);
    // A record that names something the adapter did not declare in `hello` is two things
    // at once: a hard violation of the contract, and a record that was not
    // accepted, so it is quarantined as received. The quarantine entry comes first so that
    // it is delivered, and counted in the outcome, even if the policy kills on the
    // violation.
    if (!decl) {
        quarantine(raw, V::UndeclaredMetric, k("metric"));
        report(ViolationKind::UndeclaredMetric, QStringLiteral("record metric not declared in hello"));
        return;
    }

    const QString shapeName = raw.value(k("shape")).toString();
    Record r;
    if (shapeName == QLatin1String("sample"))
        r.shape = Record::Sample;
    else if (shapeName == QLatin1String("event"))
        r.shape = Record::Event;
    else {
        quarantine(raw, V::ShapeMismatch, k("shape"));
        return;
    }

    if (!raw.value(kSource).isString() || raw.value(kSource).toString().isEmpty()) {
        quarantine(raw, V::MissingSource, k("source"));
        return;
    }
    r.source = raw.value(kSource).toString();
    const QString prefix = m_link.adapterId + QLatin1Char('.');
    if (!r.source.startsWith(prefix) || !m_feeds.contains(r.source.mid(prefix.size()))) {
        quarantine(raw, V::UndeclaredFeed, k("source"));
        report(ViolationKind::UndeclaredFeed, QStringLiteral("record source feed not declared in hello"));
        return;
    }

    QString unit;
    if (r.shape == Record::Sample) {
        if (!raw.value(kUnit).isString() || raw.value(kUnit).toString().isEmpty()) {
            quarantine(raw, V::MissingUnit, k("unit"));
            return;
        }
        unit = raw.value(kUnit).toString();
        if (!raw.value(k("value")).isDouble()) {
            quarantine(raw, V::ValueNotFinite, k("value"));
            return;
        }
        r.value = raw.value(k("value")).toDouble();
    } else {
        if (!raw.value(kPayload).isObject()) {
            quarantine(raw, V::InvalidPayload, k("payload"));
            return;
        }
        r.payload = raw.value(kPayload).toObject();
        if (r.payload.value(kUnit).isString())
            unit = r.payload.value(kUnit).toString();
        const QJsonValue dur = raw.value(k("duration_ms"));
        if (!dur.isNull() && !dur.isUndefined()) {
            qint64 d = 0;
            if (!toInteger(dur, &d)) {
                quarantine(raw, V::InvalidDuration, k("duration"));
                return;
            }
            r.durationMs = d;
        }
    }
    if (!unit.isEmpty() && !decl->units.contains(unit)) {
        quarantine(raw, V::UndeclaredUnit, k("unit"));
        report(ViolationKind::UndeclaredUnit, QStringLiteral("record unit not declared in hello"));
        return;
    }
    r.unit = unit;
    r.metric = metric;

    if (!raw.value(k("subject_ref")).isString() || raw.value(k("subject_ref")).toString().isEmpty()) {
        quarantine(raw, V::MissingSubject, k("subject_ref"));
        return;
    }
    const QString subjectRef = raw.value(k("subject_ref")).toString();

    if (!raw.value(k("measured_at")).isString()) {
        quarantine(raw, V::MissingMeasurementTime, k("measured_at"));
        return;
    }
    // Present but not strict ISO 8601 UTC (REQ-DAT-02, REQ-DAT-11): a different reason from
    // an absent time, so that a source that changed its format can be told from one that
    // dropped the field.
    r.measuredAt = UtcTime::fromIso(raw.value(k("measured_at")).toString());
    if (!r.measuredAt.isValid()) {
        quarantine(raw, V::MalformedMeasurementTime, k("measured_at"));
        return;
    }
    if (!raw.value(k("original_timestamp")).isString()
        || raw.value(k("original_timestamp")).toString().isEmpty()) {
        quarantine(raw, V::MissingOriginalTimestamp, k("original_timestamp"));
        return;
    }
    r.originalTimestamp = raw.value(k("original_timestamp")).toString();

    const UtcTime t = now();
    r.ingestedAt = t;
    r.origin = Origin::Adapter;
    r.subject = QUuid(QString::fromLatin1(kValidationSubject));
    const Validation v = validateRecord(r, t, m_skewMs);
    if (!v.ok()) {
        quarantine(raw, v.reason, v.detail);
        return;
    }
    LinkRecord out;
    out.subjectRef = subjectRef;
    out.record = r;
    out.record.subject = QUuid();
    valid->append(out);
}

// ---- Completion ------------------------------------------------------------------------

void AdapterLink::handleCompletion(const Envelope &env)
{
    if (env.type == MessageType::Error) {
        ErrorClass cls = ErrorClass::ProtocolChanged;
        QString detail = env.fields.value(QStringLiteral("detail")).toString();
        if (!errorClassFromName(env.fields.value(QStringLiteral("class")).toString(), &cls)) {
            cls = ErrorClass::ProtocolChanged;
            detail = QStringLiteral("the adapter sent an unknown error class");
        }
        qint64 retry = -1;
        const QJsonValue r = env.fields.value(QStringLiteral("retry_after_s"));
        if (!toInteger(r, &retry) || retry < 0)
            retry = -1;
        finishCommand(false, cls, static_cast<int>(retry), CommandOutcome::NotKilled, detail);
        return;
    }

    if (m_pending.onReply && !m_pending.onReply(env.fields)) {
        finishCommand(false, ErrorClass::ProtocolChanged, -1, CommandOutcome::NotKilled,
                      QStringLiteral("the reply is malformed"));
        return;
    }

    if (m_pending.acceptsRecords) {
        qint64 claimed = 0;
        toInteger(env.fields.value(QStringLiteral("records_sent")), &claimed);
        if (claimed != m_pending.outcome.recordsReceived) {
            const quint64 generation = m_generation;
            report(ViolationKind::RecordsSentMismatch,
                   QStringLiteral("records_sent differs from the records received"));
            if (ended(generation))
                return;
        }

        // Burst rule (REQ-ADM-03; provisional thresholds). It needs
        // at least kBurstMinRejected rejected records, so a feed with fewer records never
        // escalates here, even if all of them were rejected: the rejections stay counted
        // per reason in the outcome and the scheduler escalates after consecutive polls.
        qint64 quarantined = 0;
        const QMap<int, qint64> &q = m_pending.outcome.quarantinedByReason;
        for (QMap<int, qint64>::const_iterator it = q.begin(); it != q.end(); ++it)
            quarantined += it.value();
        const qint64 examined = quarantined + m_pending.outcome.recordsValid;
        if (quarantined >= kBurstMinRejected
            && static_cast<double>(quarantined) >= kBurstMinFraction * examined) {
            finishCommand(false, ErrorClass::ProtocolChanged, -1, CommandOutcome::NotKilled,
                          QStringLiteral("a burst of records was rejected"));
            return;
        }
    }
    finishCommand(true, ErrorClass::Transient, -1, CommandOutcome::NotKilled, QString());
}

// True when the link was closed (or closed and started again) since `generation` was read:
// a policy answer or a callback may end the adapter in the middle of a handler.
bool AdapterLink::ended(quint64 generation) const
{
    return generation != m_generation || m_state == Closed || m_state == Idle;
}

void AdapterLink::finishCommand(bool ok, ErrorClass errorClass, int retryAfterS,
                                CommandOutcome::KilledBy by, const QString &detail)
{
    if (!m_pending.active)
        return;
    Pending p = m_pending;
    m_pending = Pending();
    m_deadlineTimer.stop();
    m_completed.insert(p.id);

    p.outcome.result = ok ? CommandOutcome::Ok : CommandOutcome::Error;
    p.outcome.errorClass = errorClass;
    p.outcome.retryAfterS = retryAfterS;
    p.outcome.killedBy = by;
    p.outcome.durationMs = p.clock.elapsed();

    if (m_state == Busy)
        m_state = Ready;
    m_lastActivity.restart();

    if (p.type != MessageType::Configure)
        m_policy->onCommandFinished(p.outcome);
    CommandReport rep;
    rep.outcome = p.outcome;
    rep.detail = ok ? QString() : scrub(detail);
    if (p.done)
        p.done(rep);
}

// Reports one violation to the policy and applies its answer at once. Kill ends the command with protocol-changed and killed_by guard, also
// when the adapter has gone quiet, because the answer is applied here and not at the next
// message. Pause stops reading. Proceed and Drop are the same for a violation: the
// offending message is already discarded by the caller. After this call the link may be
// closed; callers that go on working with the same message must check `ended()`.
// `applyAnswer` is false only for the driver's own kill at an over-long line.
void AdapterLink::report(ViolationKind kind, const QString &detail, bool applyAnswer)
{
    Violation v;
    v.link = m_link;
    v.kind = kind;
    v.severity = severityOf(kind);
    v.detail = detail;
    v.time = now();
    if (m_pending.active) {
        if (v.severity == Severity::Hard)
            ++m_pending.outcome.hardViolations;
        else
            ++m_pending.outcome.softViolations;
    }
    // Remembered so that a kill in the middle of a batch can tell the RejectedBatchSink which
    // violation led to it.
    m_hasLastViolation = true;
    m_lastViolation = kind;
    const Action answer = m_policy->onViolation(v);
    if (applyAnswer)
        applyAction(answer);
}

void AdapterLink::onDeadline()
{
    if (!m_pending.active)
        return;
    const bool starting = m_state == WaitConfigure;
    terminate(CommandOutcome::Deadline, ErrorClass::Transient,
              QStringLiteral("the command deadline expired"),
              starting ? StartResult::HelloTimeout : StartResult::SpawnFailed);
}

// ---- Ending ----------------------------------------------------------------------------

void AdapterLink::detachChannel()
{
    if (!m_channel)
        return;
    m_detached.insert(m_channel, m_link);
    DetachedExit info;
    info.peakResidentBytes = m_peakResident;
    info.killedForMemory = m_killedForMemory;
    m_detachedExit.insert(m_channel, info);
    m_channel = 0;
}

void AdapterLink::killChannel()
{
    LinkChannel *channel = m_channel;
    detachChannel();
    if (channel) {
        channel->disconnect(channel->fromAdapter(), 0, this, 0);
        channel->kill();
    }
}

void AdapterLink::terminate(CommandOutcome::KilledBy by, ErrorClass errorClass,
                            const QString &detail, StartResult::RefusalReason startReason)
{
    m_helloTimer.stop();
    m_deadlineTimer.stop();
    m_resumeTimer.stop();
    m_stopTimer.stop();
    m_memoryTimer.stop();
    m_paused = false;
    const State previous = m_state;
    if (previous == Closed || previous == Idle)
        return;
    m_state = Closed;

    const StopCallback stopDone = m_stopDone;
    m_stopDone = StopCallback();
    const bool starting = previous == Spawning || previous == WaitHello || previous == WaitConfigure;
    const StartCallback startDone = m_startDone;
    m_startDone = StartCallback();
    const StartResult::RefusalReason reason = startReason;
    LinkId link = m_link;
    const bool hadPending = m_pending.active;
    if (starting)
        m_pending = Pending();

    sampleMemory();         // a last look while the process is still there
    killChannel();

    if (starting) {
        StartResult r;
        r.kind = StartResult::Refused;
        r.link = link;
        r.refusal = reason;
        if (startDone)
            startDone(r, scrub(detail));
    } else if (hadPending) {
        finishCommand(false, errorClass, -1, by, detail);
    }
    if (stopDone)
        stopDone();
}

void AdapterLink::onChannelFinished(LinkChannel *channel, int exitCode, bool crashed)
{
    ExitStatus status;
    status.exitCode = exitCode;
    status.crashed = crashed;

    if (channel == m_channel)
        onReadable();       // the last output may still be in the pipe

    if (channel != m_channel) {
        const LinkId link = m_detached.take(channel);
        const DetachedExit info = m_detachedExit.take(channel);
        status.peakResidentBytes = info.peakResidentBytes;
        status.killedForMemory = info.killedForMemory;
        m_policy->onExited(link, status);
        channel->deleteLater();
        return;
    }

    // The adapter ended on its own.
    m_helloTimer.stop();
    m_deadlineTimer.stop();
    m_resumeTimer.stop();
    m_stopTimer.stop();
    m_memoryTimer.stop();
    status.peakResidentBytes = m_peakResident;
    m_policy->onExited(m_link, status);
    m_channel = 0;
    channel->deleteLater();
    const State previous = m_state;
    m_state = Closed;
    const StopCallback stopDone = m_stopDone;
    m_stopDone = StopCallback();

    if (previous == Spawning || previous == WaitHello || previous == WaitConfigure) {
        m_pending = Pending();
        const StartCallback cb = m_startDone;
        m_startDone = StartCallback();
        StartResult r;
        r.kind = StartResult::Refused;
        r.link = m_link;
        r.refusal = StartResult::SpawnFailed;
        if (cb)
            cb(r, QStringLiteral("the adapter ended before it was ready"));
    } else if (m_pending.active) {
        finishCommand(false, ErrorClass::Transient, -1, CommandOutcome::Exit,
                      QStringLiteral("the adapter process ended during the command"));
    }
    if (stopDone)
        stopDone();
}

void AdapterLink::beginClose(bool sendShutdown, const StopCallback &done)
{
    m_stopDone = done;
    sampleMemory();         // the size at the orderly end, while the process answers
    m_state = Stopping;
    m_helloTimer.stop();
    m_deadlineTimer.stop();
    if (sendShutdown)
        send(MessageType::Shutdown, m_ids.next(), QString());
    m_stopTimer.start(m_limits.shutdownWaitMs);
}

void AdapterLink::stop(const StopCallback &done)
{
    if (m_state == Idle || m_state == Closed) {
        if (done)
            QTimer::singleShot(0, this, [done]() { done(); });
        return;
    }
    if (m_state == Stopping) {
        // Already ending: the new callback replaces nothing, it runs with the first one.
        const StopCallback first = m_stopDone;
        m_stopDone = [first, done]() { if (first) first(); if (done) done(); };
        return;
    }

    const bool starting = m_state == Spawning || m_state == WaitHello || m_state == WaitConfigure;
    if (starting) {
        terminate(CommandOutcome::NotKilled, ErrorClass::Transient,
                  QStringLiteral("stopped before the adapter was ready"), StartResult::SpawnFailed);
        if (done)
            QTimer::singleShot(0, this, [done]() { done(); });
        return;
    }
    if (m_pending.active)
        finishCommand(false, ErrorClass::Transient, -1, CommandOutcome::NotKilled,
                      QStringLiteral("stopped during the command"));
    if (m_state == Closed || m_state == Idle) {     // a callback ended the link
        if (done)
            QTimer::singleShot(0, this, [done]() { done(); });
        return;
    }
    beginClose(true, done);
}

} // namespace lm
