// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Michele Brami
#ifndef LM_ADAPTERLINK_H
#define LM_ADAPTERLINK_H

// Session driver: the core's end of the link to one adapter process (
// REQ-ADP-06 to 12, REQ-ADM-01 to 04). It detects, counts and reports; it never retries,
// never sleeps and never decides policy. Boundary:.

#include "session/adapterregistry.h"
#include "session/linkchannel.h"
#include "session/linkpolicy.h"
#include "session/scrubber.h"
#include "protocol/envelope.h"

#include <QByteArray>
#include <QElapsedTimer>
#include <QHash>
#include <QObject>
#include <QSet>
#include <QTimer>

#include <functional>

namespace lm {

// Burst rule of REQ-ADM-03. PROVISIONAL, to be tuned with real
// data: at the completion of a fetch the command becomes `protocol-changed` only when at
// least kBurstMinRejected records were rejected and they are at least kBurstMinFraction of
// the records examined. A feed with fewer records never escalates here (even if every
// record was rejected): the rejections are counted per reason in the outcome, and the
// scheduler escalates after consecutive polls with rejections.
const int kBurstMinRejected = 3;
const double kBurstMinFraction = 0.5;

// Number of diagnostic lines the link keeps (stderr and `log`), newest last.
const int kMaxDiagnosticLines = 200;

struct Credentials
{
    QString username;
    QString password;
};

// What the caller needs beyond the CommandOutcome. `detail` is the adapter's text (or the
// driver's reason), already scrubbed; it is empty for a success.
struct CommandReport
{
    CommandOutcome outcome;
    QString detail;
};

// Wire names, for the command-line output, logs and tests.
QString errorClassName(ErrorClass errorClass);                  // "auth-failed", ...
bool errorClassFromName(const QString &name, ErrorClass *out);
QString violationKindName(ViolationKind kind);                  // "line-too-long", ...
// "none", "deadline", "guard", "exit", "driver-limit": who ended the process.
QString killedByName(CommandOutcome::KilledBy by);
// Same names as the store's validationReasonName(); kept apart so the driver needs no QtSql.
QString quarantineReasonName(Validation::Reason reason);        // "MissingSubject", ...
Severity severityOf(ViolationKind kind);

// Creates the other end for a built-in adapter. The link takes ownership.
typedef std::function<LinkChannel *(const AdapterEntry &)> ChannelFactory;

class AdapterLink : public QObject
{
    Q_OBJECT
public:
    typedef std::function<void(const StartResult &, const QString &detail)> StartCallback;
    typedef std::function<void(const CommandReport &)> DoneCallback;
    typedef std::function<void(const CommandReport &, const SessionGrant &)> AuthCallback;
    typedef std::function<void(const CommandReport &, const QList<SubjectInfo> &)> DiscoverCallback;
    typedef std::function<void(const CommandReport &, const FetchCompletion &)> FetchCallback;
    typedef std::function<void()> StopCallback;

    // The policy is not owned and must outlive the link.
    explicit AdapterLink(LinkPolicy *policy, QObject *parent = 0);
    ~AdapterLink();

    // Sinks are not owned; null discards the records.
    void setRecordSink(RecordSink *sink) { m_recordSink = sink; }
    void setQuarantineSink(QuarantineSink *sink) { m_quarantineSink = sink; }
    // Optional: gets the raw line of a batch the policy ended the adapter in the
    // middle of. Null discards it. Not owned.
    void setRejectedBatchSink(RejectedBatchSink *sink) { m_rejectedBatchSink = sink; }

    // How the other end is made. Without a factory, start() runs `program` with
    // `arguments` as a child process (arguments carry no secret, REQ-ADP-11).
    void setChannelFactory(const ChannelFactory &factory) { m_factory = factory; }
    void setProgram(const QString &program, const QStringList &arguments = QStringList());

    // Clock and skew tolerance of record validation (REQ-ADM-02).
    void setClock(const std::function<UtcTime()> &clock) { m_clock = clock; }
    void setClockSkewToleranceMs(qint64 ms) { m_skewMs = ms; }

    // One link, one command at a time. Every operation completes through its callback
    // exactly once; callbacks run in the event loop and may call the link again.
    void start(const QString &adapterId, const QString &connectionId,
               const QJsonObject &settings, const StartCallback &done);
    void authenticate(const Credentials &credentials, const QString &session,
                      const AuthCallback &done);
    void discoverSubjects(const DiscoverCallback &done);
    void fetchCurrent(const QString &subjectRef, const FetchCallback &done);
    // feeds empty = all feeds that can cover the range.
    void fetchRange(const QString &subjectRef, const UtcTime &from, const UtcTime &to,
                    const QStringList &feeds, const FetchCallback &done);
    // `shutdown`, wait up to the policy's end-of-session wait, then kill.
    void stop(const StopCallback &done = StopCallback());

    bool isAlive() const;
    LinkId linkId() const { return m_link; }

    // Hooks for a process policy such as REQ-ADP-13 (long-lived process with an idle
    // timeout): the policy calls stop() when idleMs() passes its limit. Nothing here
    // changes an adapter.
    bool isIdle() const { return m_state == Ready; }
    qint64 idleMs() const { return isIdle() ? m_lastActivity.elapsed() : 0; }

    // Scrubbed diagnostics (stderr and `log` lines), newest last, at most kMaxDiagnosticLines.
    QStringList diagnostics() const { return m_diagnostics; }

signals:
    // source is "stderr" or "log"; level is "debug", "info", "warn", "error" ("stderr" has
    // none). The text is scrubbed (REQ-ADP-12).
    void diagnostic(const QString &source, const QString &level, const QString &text);

private slots:
    void onReadable();
    void onChannelStarted();
    void onChannelFailed(const QString &reason);
    void onStderr(const QByteArray &data);
    void onHelloTimeout();
    void onDeadline();
    void onResume();
    void onMemoryCheck();

private:
    enum State { Idle, Spawning, WaitHello, WaitConfigure, Ready, Busy, Stopping, Closed };

    struct Pending
    {
        Pending() : active(false), type(MessageType::Reply), acceptsRecords(false) {}

        bool active;
        QString id;
        MessageType type;
        bool acceptsRecords;
        QElapsedTimer clock;
        CommandOutcome outcome;
        std::function<bool(const QJsonObject &)> onReply;   // false = malformed reply
        DoneCallback done;
    };

    // Output
    void send(MessageType type, const QString &id, const QString &correlation,
              const QJsonObject &payload = QJsonObject());
    void runCommand(MessageType type, QJsonObject payload, int deadlineMs, bool acceptsRecords,
                    const std::function<bool(const QJsonObject &)> &onReply,
                    const DoneCallback &done);
    void failImmediately(MessageType type, const DoneCallback &done, ErrorClass errorClass,
                         const QString &detail);
    void refuseStart(const StartCallback &done, StartResult::RefusalReason reason,
                     const QString &rejectReason, const QString &denyReason, const QString &detail);
    void configureDone(const CommandReport &report);
    void killChannel();
    // Reads the adapter's resident peak into m_peakResident (the highest value seen).
    void sampleMemory();

    // Input
    void processBuffer();
    void handleLine(const QByteArray &line, qint64 bytes);
    void handleHello(const Envelope &env);
    void handleLog(const Envelope &env);
    void handleRecords(const Envelope &env, qint64 bytes, const QByteArray &rawLine);
    void deliverRejectedBatch(const QByteArray &rawLine);
    void handleCompletion(const Envelope &env);
    bool applyAction(const Action &action);
    void applyBudgets(const Budgets &budgets);

    // Admission (REQ-ADM-01)
    bool admit(const Envelope &env, StartResult *result, QString *rejectReason, QString *detail);
    void rejectHello(const Envelope &env, const QString &reason, const QString &detail);

    // Records
    void processRecord(const QJsonObject &raw, const QString &feed, QVector<LinkRecord> *valid);
    void quarantine(const QJsonObject &raw, Validation::Reason reason, const QString &detail);

    // Endings
    void report(ViolationKind kind, const QString &detail, bool applyAnswer = true);
    bool ended(quint64 generation) const;
    void finishCommand(bool ok, ErrorClass errorClass, int retryAfterS,
                       CommandOutcome::KilledBy by, const QString &detail);
    void failStart(StartResult::RefusalReason reason, const QString &rejectReason,
                   const QString &detail);
    void terminate(CommandOutcome::KilledBy by, ErrorClass errorClass, const QString &detail,
                   StartResult::RefusalReason startReason);
    void onChannelFinished(LinkChannel *channel, int exitCode, bool crashed);
    void detachChannel();
    void beginClose(bool sendShutdown, const StopCallback &done);
    void addDiagnostic(const QString &source, const QString &level, const QString &text);
    UtcTime now() const;
    QString scrub(const QString &text) const { return m_scrubber.scrub(text); }

    LinkPolicy *m_policy;
    RecordSink *m_recordSink;
    QuarantineSink *m_quarantineSink;
    RejectedBatchSink *m_rejectedBatchSink;
    ChannelFactory m_factory;
    QString m_program;
    QStringList m_arguments;
    std::function<UtcTime()> m_clock;
    qint64 m_skewMs;

    State m_state;
    LinkId m_link;
    LinkLimits m_limits;
    Budgets m_budgets;
    QJsonObject m_settings;
    QStringList m_feeds;
    QList<FeedDeclaration> m_declared;
    StartResult m_admission;
    StartCallback m_startDone;
    quint64 m_generation;
    StopCallback m_stopDone;

    LinkChannel *m_channel;
    QHash<LinkChannel *, LinkId> m_detached;
    // What the driver knew of a process at the moment it let go of it (peak resident size,
    // ended for memory), kept until the process's exit is reported to the policy.
    struct DetachedExit
    {
        DetachedExit() : peakResidentBytes(-1), killedForMemory(false) {}
        qint64 peakResidentBytes;
        bool killedForMemory;
    };
    QHash<LinkChannel *, DetachedExit> m_detachedExit;
    QTimer m_memoryTimer;
    qint64 m_peakResident;          // highest resident size seen of the current process, -1 = none
    bool m_killedForMemory;
    QByteArray m_buffer;
    bool m_inputFailed;
    bool m_paused;
    QTimer m_helloTimer;
    QTimer m_deadlineTimer;
    QTimer m_resumeTimer;
    QTimer m_stopTimer;
    MessageIdGenerator m_ids;
    Pending m_pending;
    QSet<QString> m_completed;
    QString m_helloId;
    qint64 m_stderrBytes;
    bool m_stderrFlooded;
    qint64 m_lineBytes;     // size of the line being handled
    bool m_hasLastViolation;    // a violation was reported since the batch began
    ViolationKind m_lastViolation;
    Scrubber m_scrubber;
    QStringList m_diagnostics;
    QElapsedTimer m_lastActivity;
};

} // namespace lm

#endif
