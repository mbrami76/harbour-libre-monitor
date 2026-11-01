// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Michele Brami
#ifndef LM_CONNECTIONSESSION_H
#define LM_CONNECTIONSESSION_H

// The session manager of one connection (REQ-SCH-08): it owns the
// adapter link, the login, the token and its expiry, the regional endpoint, and the
// bookkeeping that keeps the source from being hammered (minimum intervals, REQ-SCH-05). The
// Subject schedules ask it to run one command and get back either the command's outcome or
// the reason why it could not run.
//
// Pattern: an explicit state machine whose transitions are advanced by asynchronous callbacks (secret store, link) and by tick(). It never
// sleeps and never reads the system clock: waits are stored as a time and resumed by tick(),
// so a test moves a manual clock and calls tick().
//
// Secrets: credentials are read from the secret store for one call and wiped after it is
// sent; they and the token are never written to a log, a state entry or a message
// (REQ-SEC-01, REQ-SEC-05, REQ-ADP-11).
// Assumptions about the source and the link are SA1 to SA11 of the document: a wrong password
// may arrive as `transient`, the terms reply and a regional redirect were never
// observed, whether a new login invalidates an older token is not known.

#include "guard/guard.h"
#include "scheduler/schedulerports.h"
#include "scheduler/schedulersettings.h"

#include <QList>

namespace lm {

// Everything the scheduler wires together (not owned).
struct SchedulerPorts
{
    SchedulerPorts()
        : clock(0), random(0), links(0), secrets(0), settings(0), readings(0), state(0),
          guard(0), ingest(0) {}

    Clock *clock;
    RandomSource *random;
    LinkFactory *links;
    SecretStore *secrets;
    ConnectionSettingsStore *settings;
    ReadingStore *readings;
    StateKeeper *state;
    Guard *guard;
    IngestLog *ingest;
};

enum class SessionState {
    Closed, Loading, AwaitingApproval, Starting, Authenticating, Ready, Running,
    Renewing, Relogin, Stopping, Attention, Denied
};
QString sessionStateName(SessionState state);

// One command asked of the session.
struct SessionRequest
{
    enum Type { Current, Range, Discover };

    SessionRequest() : type(Current), subjects(1), spanMinutes(1) {}

    Type type;
    QString subjectRef;
    UtcTime from;
    UtcTime to;
    QStringList feeds;
    int subjects;               // for the guard's expectation
    double spanMinutes;
};

struct SessionResult
{
    enum Kind { Done, Failed };
    // Why a command could not run (the Failed kind). Mirrors ConnFailed of the document.
    // `Attention` means the caregiver must act; the cause is in `attentionCause`
    // (a StateCause: auth-failed, terms, protocol-changed, credentials-missing, ...).
    enum Reason {
        None, CredentialsLocked, SecretStoreError, SpawnFailed, HelloTimeout,
        DeniedDisabled, DeniedVersion, DeniedCircuit, DeniedSpawnRate,
        RateLimited, Transient, InvalidCommand, Attention
    };

    SessionResult() : kind(Failed), reason(None), retryAfterS(-1) {}

    Kind kind;
    // Done: the report of the command (its outcome may still be an error of the source).
    CommandReport report;
    FetchCompletion completion;
    QList<SubjectInfo> subjects;
    // Failed:
    Reason reason;
    QString attentionCause;     // Attention only
    UtcTime retryAt;            // when trying again makes sense, if known
    int retryAfterS;            // the adapter's hint, -1 = none
    QString detail;
};

// Told about changes the Subject schedules cannot see from a command result.
class SessionListener
{
public:
    virtual ~SessionListener() {}
    // The attention state of the connection ended by itself (a slow retry of a suspected
    // refusal) or by the user (retry, new credentials).
    virtual void sessionAttentionCleared(const QString &connection) = 0;
    // Locked credentials: waiting for the user's approval since `since` (REQ-ALR-08).
    virtual void sessionApprovalWaiting(const QString &connection, const UtcTime &since) = 0;
    // The renewal of a token that is still valid was refused: probably new credentials. A
    // warning only; monitoring goes on.
    virtual void sessionRenewalRefused(const QString &connection) = 0;
};

class ConnectionSession
{
public:
    typedef std::function<void(const SessionResult &)> Done;

    ConnectionSession(const ConnectionConfig &config, const SchedulerPorts &ports,
                      const SchedulerSettings &settings);
    ~ConnectionSession();

    void setListener(SessionListener *listener) { m_listener = listener; }

    SessionState state() const { return m_state; }
    // True while a command, a login or a renewal is in progress: no new command may start.
    bool busy() const;
    // Earliest instant the next command may start (REQ-SCH-05): after the previous command
    // by the minimum gap, and after a login by the login-to-fetch gap.
    UtcTime gateOpenAt() const;

    // Runs one command. Only when !busy(). Completes `done` exactly once.
    void run(const SessionRequest &request, const Done &done);

    // Advances waits, renewal, idle close and daily recycle. `earliestDueMs` is the earliest
    // time any Subject of the connection wants a command (-1 = none).
    void tick(qint64 earliestDueMs);
    UtcTime nextWakeAt(qint64 earliestDueMs) const;

    // Events from outside.
    void secretsAvailable();        // The app tells that the approval was given
    void credentialsChanged();
    void userRetry();
    void guardStopped();            // The guard denies this connection
    void reenabled();
    void shutdown();

    bool renewalRefused() const { return m_renewalRefused; }
    // Valid while waiting for the user's approval: when the next access is made.
    UtcTime approvalRetryAt() const;
    // Valid while a suspected login refusal waits for its slow retry.
    UtcTime suspectedRetryAt() const;
    // Time of the latest login attempt that started, for the minimum-interval tests.
    qint64 lastAuthStartMs() const { return m_lastAuthStartMs; }

private:
    enum AuthMode { Initial, Relogin_, Renew };

    struct DeadLink
    {
        LinkPort *link;
        bool stopped;
    };

    qint64 nowMs() const { return m_ports.clock->now().msecsSinceEpoch(); }
    UtcTime at(qint64 ms) const { return UtcTime::fromMSecsSinceEpoch(ms); }

    void beginLoad(AuthMode mode);
    void onLoaded(AuthMode mode, const SecretLoad &load, quint64 generation);
    void startLink();
    void onStarted(const StartResult &result, quint64 generation);
    void beginAuth(AuthMode mode, const QString &session);
    void sendAuth(AuthMode mode, const QString &session);
    void onAuth(AuthMode mode, const CommandReport &report, const SessionGrant &grant,
                quint64 generation);
    void onAuthError(AuthMode mode, const CommandOutcome &outcome);
    void applyGrant(const SessionGrant &grant);
    void execute();
    void onCommandDone(const CommandReport &report, const FetchCompletion &completion,
                       const QList<SubjectInfo> &subjects, quint64 generation);

    void retireLink();
    void reapLinks();
    void fail(SessionResult::Reason reason, const UtcTime &retryAt = UtcTime(),
              const QString &detail = QString(), int retryAfterS = -1,
              const QString &attentionCause = QString());
    void renewalFailed();
    void finish(const SessionResult &result);
    void raiseAttention(const QString &cause, const QString &detail);
    void clearAttention();
    void wipeCredentials();
    void waitThen(qint64 resumeMs, const std::function<void()> &fn);
    void renewNow();

    ConnectionConfig m_config;
    SchedulerPorts m_ports;
    SchedulerSettings m_s;
    SessionListener *m_listener;

    SessionState m_state;
    LinkPort *m_link;
    QList<DeadLink> m_dead;
    quint64 m_generation;           // bumped on shutdown and guard stop: late callbacks are ignored

    // The request in progress.
    bool m_hasRequest;
    SessionRequest m_request;
    Done m_done;
    int m_reloginsUsed;

    // The token.
    Credentials m_creds;
    bool m_hasCreds;
    QString m_storedSession;
    UtcTime m_expiresAt;

    // Bookkeeping for the minimum intervals and the timers.
    qint64 m_lastAuthStartMs;
    qint64 m_lastAuthEndMs;
    qint64 m_authNotBeforeMs;
    qint64 m_lastCommandEndMs;
    qint64 m_linkStartedMs;
    qint64 m_lastRenewTryMs;
    int m_authFailN;
    int m_secretRetryN;
    qint64 m_secretRetryAtMs;
    qint64 m_approvalSinceMs;
    qint64 m_suspectedRetryAtMs;
    int m_suspectedTries;
    bool m_renewalRefused;
    bool m_offeredFresh;            // the last start passed no token because it is near its expiry
    bool m_useStoredToken;          // the next start offers the stored token after a refused renewal

    // A wait for a gap, resumed by tick().
    qint64 m_resumeAtMs;
    std::function<void()> m_resume;
};

} // namespace lm

#endif
