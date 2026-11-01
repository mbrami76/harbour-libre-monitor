// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Michele Brami
#include "scheduler/connectionsession.h"

#include <QRegExp>

namespace lm {

namespace {

const qint64 kNever = -1;

// Gap that doubles with each consecutive failure, up to a cap (the login retry gap).
qint64 doubled(qint64 base, int n, qint64 cap)
{
    qint64 v = base;
    for (int i = 0; i < n && v < cap; ++i)
        v *= 2;
    return v < cap ? v : cap;
}

// A host name as a regional endpoint may be: letters, digits, dots, hyphens; no scheme, port,
// path or credentials. An adapter is untrusted input.
bool plausibleHost(const QString &host)
{
    static const QRegExp re(QStringLiteral("^[A-Za-z0-9][A-Za-z0-9.-]{0,252}$"));
    return re.exactMatch(host);
}

// Keeps the smallest non-negative time seen so far.
void keepMin(qint64 *best, qint64 t)
{
    if (t >= 0 && (*best < 0 || t < *best))
        *best = t;
}

} // namespace

QString sessionStateName(SessionState s)
{
    switch (s) {
    case SessionState::Closed: return QStringLiteral("closed");
    case SessionState::Loading: return QStringLiteral("loading");
    case SessionState::AwaitingApproval: return QStringLiteral("awaiting-approval");
    case SessionState::Starting: return QStringLiteral("starting");
    case SessionState::Authenticating: return QStringLiteral("authenticating");
    case SessionState::Ready: return QStringLiteral("ready");
    case SessionState::Running: return QStringLiteral("running");
    case SessionState::Renewing: return QStringLiteral("renewing");
    case SessionState::Relogin: return QStringLiteral("relogin");
    case SessionState::Stopping: return QStringLiteral("stopping");
    case SessionState::Attention: return QStringLiteral("attention");
    case SessionState::Denied: return QStringLiteral("denied");
    }
    return QString();
}

ConnectionSession::ConnectionSession(const ConnectionConfig &config, const SchedulerPorts &ports,
                                     const SchedulerSettings &settings)
    : m_config(config), m_ports(ports), m_s(settings), m_listener(0),
      m_state(SessionState::Closed), m_link(0), m_generation(1), m_hasRequest(false),
      m_reloginsUsed(0), m_hasCreds(false), m_lastAuthStartMs(kNever), m_lastAuthEndMs(kNever),
      m_authNotBeforeMs(0), m_lastCommandEndMs(kNever), m_linkStartedMs(0),
      m_lastRenewTryMs(kNever), m_authFailN(0), m_secretRetryN(0), m_secretRetryAtMs(0),
      m_approvalSinceMs(0), m_suspectedRetryAtMs(0), m_suspectedTries(0),
      m_renewalRefused(false), m_offeredFresh(false), m_useStoredToken(false), m_resumeAtMs(0)
{
    // A connection that was disabled by the guard before a restart starts denied.
    if (m_ports.state->connectionDisabled(m_config.id))
        m_state = SessionState::Denied;
}

ConnectionSession::~ConnectionSession()
{
    wipeCredentials();
    delete m_link;
    for (int i = 0; i < m_dead.size(); ++i)
        delete m_dead.at(i).link;
}

bool ConnectionSession::busy() const
{
    if (m_hasRequest)
        return true;
    switch (m_state) {
    case SessionState::Loading: case SessionState::Starting: case SessionState::Authenticating:
    case SessionState::Running: case SessionState::Renewing: case SessionState::Relogin:
    case SessionState::Stopping:
        return true;
    default:
        return false;
    }
}

UtcTime ConnectionSession::gateOpenAt() const
{
    qint64 t = 0;
    if (m_lastCommandEndMs != kNever && m_lastCommandEndMs + m_s.minFetchGapMs > t)
        t = m_lastCommandEndMs + m_s.minFetchGapMs;
    if (m_lastAuthEndMs != kNever && m_lastAuthEndMs + m_s.authToFetchGapMs > t)
        t = m_lastAuthEndMs + m_s.authToFetchGapMs;
    return t > 0 ? at(t) : UtcTime();
}

UtcTime ConnectionSession::approvalRetryAt() const
{
    return m_state == SessionState::AwaitingApproval ? at(m_secretRetryAtMs) : UtcTime();
}

UtcTime ConnectionSession::suspectedRetryAt() const
{
    return m_state == SessionState::Attention && m_suspectedRetryAtMs > 0
        ? at(m_suspectedRetryAtMs) : UtcTime();
}

// ---- helpers -----------------------------------------------------------------------------

void ConnectionSession::wipeCredentials()
{
    // Best effort: QString may have been copied elsewhere; the point is that this object does
    // not keep the secret longer than the call that needs it.
    m_creds.username.fill(QLatin1Char('\0'));
    m_creds.password.fill(QLatin1Char('\0'));
    m_creds = Credentials();
    m_hasCreds = false;
}

void ConnectionSession::waitThen(qint64 resumeMs, const std::function<void()> &fn)
{
    m_resumeAtMs = resumeMs;
    m_resume = fn;
}

void ConnectionSession::finish(const SessionResult &result)
{
    const Done d = m_done;
    m_done = Done();
    m_hasRequest = false;
    if (d)
        d(result);
}

void ConnectionSession::fail(SessionResult::Reason reason, const UtcTime &retryAt,
                             const QString &detail, int retryAfterS, const QString &attentionCause)
{
    SessionResult r;
    r.kind = SessionResult::Failed;
    r.reason = reason;
    r.retryAt = retryAt;
    r.detail = detail;
    r.retryAfterS = retryAfterS;
    r.attentionCause = attentionCause;
    finish(r);
}

void ConnectionSession::raiseAttention(const QString &cause, const QString &detail)
{
    m_ports.state->setAttention(m_config.id, cause, detail, m_config.adapterId,
                                m_link ? m_link->linkId().adapterVersion : QString());
    m_state = SessionState::Attention;
    m_suspectedRetryAtMs = 0;
    if (cause == StateCause::authFailedSuspected())
        m_suspectedRetryAtMs = nowMs() + m_s.authSuspectedRetryMs;
}

void ConnectionSession::clearAttention()
{
    if (m_ports.state->attention(m_config.id).active())
        m_ports.state->clearConnection(m_config.id, QStringLiteral("attention ended"));
    m_suspectedRetryAtMs = 0;
    if (m_listener)
        m_listener->sessionAttentionCleared(m_config.id);
}

void ConnectionSession::retireLink()
{
    if (!m_link)
        return;
    DeadLink d;
    d.link = m_link;
    d.stopped = false;
    m_link = 0;
    m_dead.append(d);
    // Stop, then delete later from tick(): never inside a callback of the link itself.
    LinkPort *link = d.link;
    link->stop([this, link]() {
        for (int i = 0; i < m_dead.size(); ++i)
            if (m_dead[i].link == link)
                m_dead[i].stopped = true;
    });
}

void ConnectionSession::reapLinks()
{
    for (int i = 0; i < m_dead.size();) {
        if (m_dead.at(i).stopped || !m_dead.at(i).link->isAlive()) {
            delete m_dead.at(i).link;
            m_dead.removeAt(i);
        } else {
            ++i;
        }
    }
}

// ---- running a command ---------------------------------------

void ConnectionSession::run(const SessionRequest &request, const Done &done)
{
    m_request = request;
    m_done = done;
    m_hasRequest = true;
    const qint64 now = nowMs();

    // A connection that needs the caregiver, or that the guard denies, starts nothing.
    if (m_ports.state->connectionDisabled(m_config.id)) {
        m_state = SessionState::Denied;
        fail(SessionResult::DeniedDisabled);
        return;
    }
    const AttentionState a = m_ports.state->attention(m_config.id);
    if (a.active()) {
        m_state = SessionState::Attention;
        fail(SessionResult::Attention, UtcTime(), a.detail, -1, a.cause);
        return;
    }
    switch (m_state) {
    case SessionState::Ready:
        execute();
        break;
    case SessionState::AwaitingApproval:
        if (now < m_secretRetryAtMs) {
            fail(SessionResult::CredentialsLocked, at(m_secretRetryAtMs));
            break;
        }
        beginLoad(Initial);
        break;
    case SessionState::Denied:
        fail(SessionResult::DeniedDisabled);
        break;
    case SessionState::Closed:
        beginLoad(Initial);
        break;
    default:
        fail(SessionResult::Transient, UtcTime(), QStringLiteral("session busy"));
        break;
    }
}

void ConnectionSession::beginLoad(AuthMode mode)
{
    if (mode == Initial)
        m_state = SessionState::Loading;
    const quint64 gen = m_generation;
    m_ports.secrets->load(m_config.id, [this, mode, gen](const SecretLoad &load) {
        onLoaded(mode, load, gen);
    });
}

void ConnectionSession::onLoaded(AuthMode mode, const SecretLoad &load, quint64 generation)
{
    if (generation != m_generation)
        return;
    const qint64 now = nowMs();
    switch (load.kind) {
    case SecretLoad::Ok:
        m_creds = load.credentials;
        m_hasCreds = true;
        m_storedSession = load.session;
        if (load.expiresAt.isValid() || m_storedSession.isEmpty())
            m_expiresAt = load.expiresAt;
        m_secretRetryN = 0;
        if (mode == Initial) {
            startLink();
        } else {
            // Re-login and renewal run on the live link, with a fresh login (no stored blob).
            beginAuth(mode, QString());
        }
        return;

    case SecretLoad::Locked:
        // Not an error of the source and not a guard matter. Wait for the approval,
        // retry slowly: each access is what raises the system notification (SA9).
        if (mode == Renew) {
            m_lastRenewTryMs = now;
            m_state = SessionState::Ready;
            return;
        }
        if (mode == Relogin_)
            retireLink();
        if (m_state != SessionState::AwaitingApproval || m_secretRetryN == 0)
            m_approvalSinceMs = now;
        m_state = SessionState::AwaitingApproval;
        m_secretRetryAtMs = now + doubled(m_s.secretRetryMs, m_secretRetryN, m_s.secretRetryCapMs);
        ++m_secretRetryN;
        if (m_listener)
            m_listener->sessionApprovalWaiting(m_config.id, at(m_approvalSinceMs));
        fail(SessionResult::CredentialsLocked, at(m_secretRetryAtMs));
        return;

    case SecretLoad::Missing:
        if (mode == Renew) {
            m_lastRenewTryMs = now;
            m_state = SessionState::Ready;
            return;
        }
        if (mode == Relogin_)
            retireLink();
        raiseAttention(StateCause::credentialsMissing(), QStringLiteral("no credentials stored"));
        fail(SessionResult::Attention, UtcTime(), QString(), -1, StateCause::credentialsMissing());
        return;

    case SecretLoad::Error:
    default:
        if (mode == Renew) {
            m_lastRenewTryMs = now;
            m_state = SessionState::Ready;
            return;
        }
        if (mode == Relogin_)
            retireLink();
        m_state = SessionState::Closed;
        fail(SessionResult::SecretStoreError);
        return;
    }
}

void ConnectionSession::startLink()
{
    m_state = SessionState::Starting;
    if (!m_link)
        m_link = m_ports.links->create(m_config.id);
    const quint64 gen = m_generation;
    m_link->start(m_config.adapterId, m_config.id, m_ports.settings->get(m_config.id),
                  [this, gen](const StartResult &result, const QString &) { onStarted(result, gen); });
}

void ConnectionSession::onStarted(const StartResult &result, quint64 generation)
{
    if (generation != m_generation)
        return;
    if (result.kind == StartResult::Admitted) {
        m_linkStartedMs = nowMs();
        // A token inside its renewal window is not offered to the adapter, which then logs in
        // afresh: the renewal rides on the start.
        QString session = m_storedSession;
        m_offeredFresh = false;
        if (!m_useStoredToken && m_expiresAt.isValid()
            && m_expiresAt.msecsSinceEpoch() - m_s.renewBeforeMs <= nowMs()) {
            session.clear();
            m_offeredFresh = true;
        }
        m_useStoredToken = false;
        beginAuth(Initial, session);
        return;
    }
    // Refused: the process is not running; the link object is retired without a stop.
    retireLink();
    switch (result.refusal) {
    case StartResult::Denied:
        if (result.denyReason == QLatin1String("disabled-by-guard")) {
            m_state = SessionState::Denied;
            fail(SessionResult::DeniedDisabled);
        } else if (result.denyReason == QLatin1String("adapter-version-disabled")) {
            m_state = SessionState::Denied;
            fail(SessionResult::DeniedVersion);
        } else if (result.denyReason == QLatin1String("circuit-open")) {
            m_state = SessionState::Closed;
            fail(SessionResult::DeniedCircuit, m_ports.guard->circuitReopenAt(m_config.id));
        } else {
            m_state = SessionState::Closed;
            fail(SessionResult::DeniedSpawnRate, m_ports.guard->spawnReadyAt(m_config.id));
        }
        return;
    case StartResult::AdmissionRejected:
        raiseAttention(StateCause::adapterRejected(), result.rejectReason);
        fail(SessionResult::Attention, UtcTime(), result.rejectReason, -1,
             StateCause::adapterRejected());
        return;
    case StartResult::HelloTimeout:
        m_ports.guard->noteStartFailure(m_config.id);
        m_state = SessionState::Closed;
        fail(SessionResult::HelloTimeout);
        return;
    case StartResult::SpawnFailed:
    default:
        m_ports.guard->noteStartFailure(m_config.id);
        m_state = SessionState::Closed;
        fail(SessionResult::SpawnFailed);
        return;
    }
}

// ---- the login -----------------------------------------------

void ConnectionSession::beginAuth(AuthMode mode, const QString &session)
{
    // Minimum intervals between logins (REQ-SCH-05): after the previous login started, and
    // after the retry gap that follows a failed one.
    qint64 earliest = m_authNotBeforeMs;
    if (m_lastAuthStartMs != kNever && m_lastAuthStartMs + m_s.minAuthGapMs > earliest)
        earliest = m_lastAuthStartMs + m_s.minAuthGapMs;
    if (mode == Initial)
        m_state = SessionState::Authenticating;
    else if (mode == Relogin_)
        m_state = SessionState::Relogin;
    else
        m_state = SessionState::Renewing;
    if (nowMs() < earliest) {
        waitThen(earliest, [this, mode, session]() { sendAuth(mode, session); });
        return;
    }
    sendAuth(mode, session);
}

void ConnectionSession::sendAuth(AuthMode mode, const QString &session)
{
    if (!m_link || !m_hasCreds) {
        wipeCredentials();
        m_state = SessionState::Closed;
        if (m_hasRequest)
            fail(SessionResult::Transient, UtcTime(), QStringLiteral("no link for the login"));
        return;
    }
    m_lastAuthStartMs = nowMs();
    const quint64 gen = m_generation;
    const Credentials creds = m_creds;
    wipeCredentials();          // the only copy left is the one handed to the call
    m_link->authenticate(creds, session,
                         [this, mode, gen](const CommandReport &report, const SessionGrant &grant) {
                             onAuth(mode, report, grant, gen);
                         });
}

void ConnectionSession::applyGrant(const SessionGrant &grant)
{
    if (!grant.session.isEmpty()) {
        m_ports.secrets->saveSession(m_config.id, grant.session, grant.expiresAt);
        m_storedSession = grant.session;
        m_expiresAt = grant.expiresAt;
    }
    // settings_update: only a bare host name under a known key is kept (REQ-SRC-02); the rest
    // is dropped and noted by key name only, never by value.
    if (!grant.settingsUpdate.isEmpty()) {
        QJsonObject accepted;
        QStringList rejected;
        for (QJsonObject::const_iterator it = grant.settingsUpdate.begin();
             it != grant.settingsUpdate.end(); ++it) {
            if (it.key() == QLatin1String("region_host") && it.value().isString()
                && plausibleHost(it.value().toString()))
                accepted.insert(it.key(), it.value());
            else
                rejected.append(it.key());
        }
        if (!accepted.isEmpty())
            m_ports.settings->merge(m_config.id, accepted);
        if (!rejected.isEmpty()) {
            rejected.sort();
            IngestLogEntry e;
            e.time = m_ports.clock->now();
            e.event = QStringLiteral("settings-update-rejected");
            e.adapter = m_config.adapterId;
            e.connection = m_config.id;
            e.detail = QStringLiteral("keys: ") + rejected.join(QLatin1Char(','));
            m_ports.ingest->append(e);
        }
    }
}

void ConnectionSession::onAuth(AuthMode mode, const CommandReport &report, const SessionGrant &grant,
                               quint64 generation)
{
    if (generation != m_generation)
        return;
    const qint64 now = nowMs();
    m_lastAuthEndMs = now;
    if (report.outcome.result == CommandOutcome::Ok) {
        applyGrant(grant);
        m_authFailN = 0;
        m_authNotBeforeMs = 0;
        m_suspectedTries = 0;
        m_renewalRefused = false;
        if (mode == Initial)
            m_reloginsUsed = 0;
        m_state = SessionState::Ready;
        if (mode == Renew)
            return;
        // The first command waits the login-to-fetch gap (REQ-SCH-05).
        waitThen(now + m_s.authToFetchGapMs, [this]() { execute(); });
        return;
    }
    onAuthError(mode, report.outcome);
}

void ConnectionSession::renewalFailed()
{
    // The old token still works, so monitoring goes on; only a warning is raised.
    m_lastRenewTryMs = nowMs();
    m_renewalRefused = true;
    if (m_link && m_link->isAlive()) {
        m_state = SessionState::Ready;
    } else {
        retireLink();
        m_state = SessionState::Closed;
    }
    if (m_listener)
        m_listener->sessionRenewalRefused(m_config.id);
}

void ConnectionSession::onAuthError(AuthMode mode, const CommandOutcome &o)
{
    const qint64 now = nowMs();
    if (mode == Renew) {
        renewalFailed();
        return;
    }
    // A fresh login that was only meant to renew a token near its end is refused: the old
    // token is still valid for days, so it is offered once more instead of stopping the
    // connection early (the same reasoning as for the live link). A warning is raised.
    if (mode == Initial && o.errorClass == ErrorClass::AuthFailed && m_offeredFresh
        && !m_storedSession.isEmpty() && m_expiresAt.isValid() && m_expiresAt.msecsSinceEpoch() > now) {
        m_offeredFresh = false;
        m_useStoredToken = true;
        m_renewalRefused = true;
        if (m_listener)
            m_listener->sessionRenewalRefused(m_config.id);
        retireLink();
        beginLoad(Initial);
        return;
    }
    retireLink();           // a failed login leaves no usable link

    switch (o.errorClass) {
    case ErrorClass::AuthFailed:
        // The source refused the credentials. Never retried by itself.
        raiseAttention(StateCause::authFailed(), QStringLiteral("login refused"));
        fail(SessionResult::Attention, UtcTime(), QString(), -1, StateCause::authFailed());
        return;

    case ErrorClass::Transient: {
        // A wrong password may be reported as transient, so failures are
        // counted; after kAuthTransientMax the refusal is only *suspected* and retried slowly.
        ++m_authFailN;
        if (m_authFailN >= m_s.authTransientMax) {
            raiseAttention(StateCause::authFailedSuspected(), QStringLiteral("login never succeeds"));
            fail(SessionResult::Attention, UtcTime(), QString(), -1, StateCause::authFailedSuspected());
            return;
        }
        const qint64 gap = doubled(m_s.authRetryGapMs, m_authFailN - 1, m_s.authRetryCapMs);
        m_authNotBeforeMs = now + gap;
        m_state = SessionState::Closed;
        fail(SessionResult::Transient, at(now + gap), QStringLiteral("login failed"));
        return;
    }

    case ErrorClass::RateLimited:
        m_state = SessionState::Closed;
        fail(SessionResult::RateLimited, UtcTime(), QString(), o.retryAfterS);
        return;

    case ErrorClass::TermsMustBeAccepted:
        raiseAttention(StateCause::terms(), QStringLiteral("terms must be accepted"));
        fail(SessionResult::Attention, UtcTime(), QString(), -1, StateCause::terms());
        return;

    case ErrorClass::ProtocolChanged:
        if (o.killedBy == CommandOutcome::Guard || o.killedBy == CommandOutcome::DriverLimit) {
            // The guard's doing, not the source's: it decides the trip; here only a backoff.
            m_state = m_ports.state->connectionDisabled(m_config.id) ? SessionState::Denied
                                                                      : SessionState::Closed;
            fail(m_state == SessionState::Denied ? SessionResult::DeniedDisabled
                                                 : SessionResult::Transient,
                 UtcTime(), QStringLiteral("killed by the guard during the login"));
            return;
        }
        raiseAttention(StateCause::protocolChanged(), QStringLiteral("login reply not understood"));
        fail(SessionResult::Attention, UtcTime(), QString(), -1, StateCause::protocolChanged());
        return;

    case ErrorClass::InvalidCommand:
    default:
        m_state = SessionState::Closed;
        fail(SessionResult::InvalidCommand);
        return;
    }
}

// ---- executing the command ----------------------------------------------------------------

void ConnectionSession::execute()
{
    if (!m_hasRequest)
        return;
    if (!m_link || !m_link->isAlive()) {
        // The process ended between polls. Start over without telling the schedule.
        retireLink();
        m_state = SessionState::Closed;
        beginLoad(Initial);
        return;
    }
    m_state = SessionState::Running;
    const LinkId id = m_link->linkId();
    const quint64 gen = m_generation;
    const SessionRequest r = m_request;
    switch (r.type) {
    case SessionRequest::Current:
        m_ports.guard->beginCommand(id, QStringLiteral("fetch_current"), r.subjects, 1.0);
        m_link->fetchCurrent(r.subjectRef, [this, gen](const CommandReport &rep, const FetchCompletion &c) {
            onCommandDone(rep, c, QList<SubjectInfo>(), gen);
        });
        break;
    case SessionRequest::Range:
        m_ports.guard->beginCommand(id, QStringLiteral("fetch_range"), r.subjects, r.spanMinutes);
        m_link->fetchRange(r.subjectRef, r.from, r.to, r.feeds,
                           [this, gen](const CommandReport &rep, const FetchCompletion &c) {
                               onCommandDone(rep, c, QList<SubjectInfo>(), gen);
                           });
        break;
    case SessionRequest::Discover:
        m_ports.guard->beginCommand(id, QStringLiteral("discover_subjects"), 1, 1.0);
        m_link->discoverSubjects([this, gen](const CommandReport &rep, const QList<SubjectInfo> &list) {
            onCommandDone(rep, FetchCompletion(), list, gen);
        });
        break;
    }
}

void ConnectionSession::onCommandDone(const CommandReport &report, const FetchCompletion &completion,
                                      const QList<SubjectInfo> &subjects, quint64 generation)
{
    if (generation != m_generation)
        return;
    m_lastCommandEndMs = nowMs();
    const CommandOutcome &o = report.outcome;
    const bool alive = m_link && m_link->isAlive();

    if (o.result == CommandOutcome::Ok) {
        m_reloginsUsed = 0;
        if (alive) {
            m_state = SessionState::Ready;
        } else {
            retireLink();
            m_state = SessionState::Closed;
        }
        SessionResult r;
        r.kind = SessionResult::Done;
        r.report = report;
        r.completion = completion;
        r.subjects = subjects;
        finish(r);
        return;
    }

    if (o.errorClass == ErrorClass::AuthFailed && o.killedBy == CommandOutcome::NotKilled) {
        // (REQ-SCH-04): one re-login, then the command once more; a second
        // rejection stops the connection for the caregiver.
        if (m_reloginsUsed < m_s.reloginMax) {
            ++m_reloginsUsed;
            m_state = SessionState::Relogin;
            beginLoad(Relogin_);
            return;
        }
        retireLink();
        raiseAttention(StateCause::authFailed(), QStringLiteral("token rejected after the re-login"));
        fail(SessionResult::Attention, UtcTime(), QString(), -1, StateCause::authFailed());
        return;
    }

    // Any other error is the schedule's to judge.
    if (alive && o.killedBy == CommandOutcome::NotKilled) {
        m_state = SessionState::Ready;
    } else {
        retireLink();
        m_state = m_ports.state->connectionDisabled(m_config.id) ? SessionState::Denied
                                                                  : SessionState::Closed;
    }
    SessionResult r;
    r.kind = SessionResult::Done;
    r.report = report;
    finish(r);
}

// ---- timers and events --------------------------------------------------------------------

void ConnectionSession::renewNow()
{
    m_lastRenewTryMs = nowMs();
    m_state = SessionState::Renewing;
    beginLoad(Renew);
}

void ConnectionSession::tick(qint64 earliestDueMs)
{
    reapLinks();
    const qint64 now = nowMs();

    if (m_resume && now >= m_resumeAtMs) {
        const std::function<void()> fn = m_resume;
        m_resume = std::function<void()>();
        fn();
        return;
    }

    // Slow retry of a login that is only suspected to be refused.
    if (m_state == SessionState::Attention && m_suspectedRetryAtMs > 0 && now >= m_suspectedRetryAtMs) {
        if (m_s.authSuspectedRetryMax == 0 || m_suspectedTries < m_s.authSuspectedRetryMax) {
            ++m_suspectedTries;
            m_authFailN = m_s.authTransientMax - 1;     // one failure suspects again at once
            clearAttention();
            m_state = SessionState::Closed;
        } else {
            m_suspectedRetryAtMs = 0;                   // out of retries: wait for the user
        }
        return;
    }

    if (m_state != SessionState::Ready || busy())
        return;

    // Proactive renewal of a token near its expiry; a refusal is only a warning.
    if (m_expiresAt.isValid() && now >= m_expiresAt.msecsSinceEpoch() - m_s.renewBeforeMs
        && (m_lastRenewTryMs == kNever || now - m_lastRenewTryMs >= m_s.renewRetryMs)) {
        renewNow();
        return;
    }

    // Idle close and daily recycle (REQ-ADP-13), only between commands.
    const bool nothingSoon = earliestDueMs < 0 || earliestDueMs - now > m_s.linkIdleCloseMs;
    const bool tooOld = now - m_linkStartedMs >= m_s.linkMaxAgeMs;
    if (nothingSoon || tooOld) {
        m_state = SessionState::Stopping;
        retireLink();
        m_state = SessionState::Closed;
    }
}

UtcTime ConnectionSession::nextWakeAt(qint64 earliestDueMs) const
{
    qint64 best = -1;
    const qint64 now = nowMs();
    if (m_resume)
        keepMin(&best, m_resumeAtMs);
    if (m_state == SessionState::AwaitingApproval)
        keepMin(&best, m_secretRetryAtMs);
    if (m_state == SessionState::Attention && m_suspectedRetryAtMs > 0)
        keepMin(&best, m_suspectedRetryAtMs);
    if (m_state == SessionState::Ready && !busy()) {
        if (m_expiresAt.isValid()) {
            qint64 t = m_expiresAt.msecsSinceEpoch() - m_s.renewBeforeMs;
            if (m_lastRenewTryMs != kNever && m_lastRenewTryMs + m_s.renewRetryMs > t)
                t = m_lastRenewTryMs + m_s.renewRetryMs;
            keepMin(&best, t < now ? now : t);
        }
        // The idle close is due at once when nothing is wanted within the idle time; if a
        // command is soon due it never becomes due by waiting, so no wake-up is needed.
        if (earliestDueMs < 0 || earliestDueMs - now > m_s.linkIdleCloseMs)
            keepMin(&best, now);
        keepMin(&best, m_linkStartedMs + m_s.linkMaxAgeMs);
    }
    return best >= 0 ? at(best) : UtcTime();
}

void ConnectionSession::secretsAvailable()
{
    if (m_state == SessionState::AwaitingApproval) {
        m_secretRetryAtMs = nowMs();
        m_secretRetryN = 0;
    }
}

void ConnectionSession::credentialsChanged()
{
    // New credentials mean a new login, so the stored token is dropped and a live link
    // (which holds the old session) is ended. Only an attention about the credentials ends
    // here; other causes (terms, a changed source) wait for their own retry.
    const QString cause = m_ports.state->attention(m_config.id).cause;
    if (cause == StateCause::authFailed() || cause == StateCause::authFailedSuspected()
        || cause == StateCause::credentialsMissing())
        clearAttention();
    m_ports.secrets->clearSession(m_config.id);
    m_storedSession.clear();
    m_expiresAt = UtcTime();
    m_authFailN = 0;
    m_reloginsUsed = 0;
    m_authNotBeforeMs = 0;
    m_suspectedTries = 0;
    if ((m_state == SessionState::Attention && !m_ports.state->attention(m_config.id).active())
        || m_state == SessionState::AwaitingApproval) {
        m_state = SessionState::Closed;
    } else if (m_state == SessionState::Ready && !busy()) {
        retireLink();
        m_state = SessionState::Closed;
    }
}

void ConnectionSession::userRetry()
{
    if (m_state == SessionState::AwaitingApproval) {
        secretsAvailable();
        return;
    }
    clearAttention();
    if (m_state == SessionState::Attention)
        m_state = SessionState::Closed;
}

void ConnectionSession::guardStopped()
{
    ++m_generation;                 // a late outcome of the killed process is ignored
    m_resume = std::function<void()>();
    wipeCredentials();
    retireLink();
    m_state = SessionState::Denied;
    if (m_hasRequest)
        fail(SessionResult::DeniedDisabled);
}

void ConnectionSession::reenabled()
{
    if (m_state == SessionState::Denied)
        m_state = SessionState::Closed;
}

void ConnectionSession::shutdown()
{
    ++m_generation;
    m_resume = std::function<void()>();
    m_done = Done();
    m_hasRequest = false;
    wipeCredentials();
    retireLink();
    m_state = SessionState::Closed;
}

} // namespace lm
