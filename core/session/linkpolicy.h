// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Michele Brami
#ifndef LM_LINKPOLICY_H
#define LM_LINKPOLICY_H

// Boundary between the session driver (the link side) and the guard and
// scheduler (the policy side). Content is a proposal;
// names may still change.
//
// No type below carries record content, a name, a credential, a token or a glucose
// value, except SessionGrant, which is secret, lives in memory only and is never logged.
// One thread, the Qt event loop: callbacks run in the driver's thread and must not block.

#include "record/record.h"
#include "time/utctime.h"

#include <QByteArray>
#include <QJsonObject>
#include <QList>
#include <QMap>
#include <QString>
#include <QStringList>
#include <QVector>

namespace lm {

// ---- Common types ------------------------------------------

struct LinkId
{
    QString connectionId;       // opaque, given by the caller
    QString adapterId;
    QString adapterVersion;     // from hello
    QString sessionId;          // new for every process start
};

enum class Severity { Soft, Hard };     // fixed by

enum class ViolationKind {
    LineTooLong, MalformedFraming, UnknownType, BadHeader,
    UndeclaredMetric, UndeclaredUnit, UndeclaredFeed,
    UnmatchedCorrelation, DuplicateCompletion, NonProtocolOutput,
    BatchTooLarge, RateAboveRegistry, StderrFlood, RecordsSentMismatch,
    BudgetExceeded              // messages or bytes per second, records per window
};

// The six classes (REQ-ADP-10).
enum class ErrorClass {
    AuthFailed, TermsMustBeAccepted, RateLimited, Transient, ProtocolChanged, InvalidCommand
};

// Answer of the policy to a driver event. The driver applies it at once.
struct Action
{
    enum Kind { Proceed, Drop, Pause, Kill };

    Action() : kind(Proceed), pauseMs(0) {}
    static Action proceed() { return Action(); }
    static Action drop() { Action a; a.kind = Drop; return a; }
    static Action pause(int ms) { Action a; a.kind = Pause; a.pauseMs = ms; return a; }
    static Action kill() { Action a; a.kind = Kill; return a; }

    Kind kind;
    int pauseMs;                // Pause only: stop reading, backpressure
};

struct Violation
{
    Violation() : kind(ViolationKind::MalformedFraming), severity(Severity::Soft) {}

    LinkId link;
    ViolationKind kind;
    Severity severity;
    QString detail;             // names the limit or field, never content
    UtcTime time;
};

// ---- Link side: results ------------------------------------

// Opaque secret. Memory only, never logged, stored by the caller in the secret store.
struct SessionGrant
{
    QString session;
    UtcTime expiresAt;                  // invalid (default) = none
    QJsonObject settingsUpdate;         // non-secret settings the caller shall keep; may be empty
};

struct SubjectInfo
{
    QString subjectRef;         // opaque to the core
    QString displayName;        // personal data: screen and configuration only
    QString unit;               // empty = undetermined
    QString unitBasis;          // code, relation, magnitude, none (REQ-DAT-13)
};

struct FeedCoverage
{
    QString feed;
    UtcTime from;
    UtcTime to;
};

// What the completion of a fetch carries; records themselves go to the sinks.
struct FetchCompletion
{
    FetchCompletion() : recordsSent(0), hasTrend(false), trend(0) {}

    qint64 recordsSent;         // the count the adapter claims
    UtcTime watermark;          // invalid = nothing returned
    QList<FeedCoverage> coverage;
    QJsonObject sensor;         // as in contract 5.5; empty when absent
    // The source's trend arrow of the reading the completion of `fetch_current` is about
    // an integer the core does not interpret. `hasTrend` is false
    // when the adapter sent none, sent null, or the command is not `fetch_current`. The
    // caller keeps it in memory with the reading's measurement time (the watermark) and
    // never stores it.
    bool hasTrend;
    int trend;
};

struct FeedDeclaration
{
    QString metric;
    QStringList units;
};

struct Budgets
{
    Budgets() : maxRecordsPerBatch(500), maxLineBytes(1048576) {}

    int maxRecordsPerBatch;     // ceiling 500
    int maxLineBytes;           // ceiling 1048576
};

// Result of start().
struct StartResult
{
    enum Kind { Admitted, Refused };
    enum RefusalReason { None, Denied, SpawnFailed, HelloTimeout, AdmissionRejected };

    StartResult() : kind(Refused), refusal(None) {}

    Kind kind;
    LinkId link;
    // Admitted:
    Budgets budgets;                    // effective: the lower of request and ceiling
    QStringList feeds;
    QList<FeedDeclaration> metrics;
    // Refused:
    RefusalReason refusal;
    QString denyReason;                 // Denied: the policy's reason (MayStartResult)
    QString rejectReason;               // AdmissionRejected: reject reason of contract 5.2
};

struct CommandOutcome
{
    enum Result { Ok, Error };
    // Who ended the process. Guard: the policy ordered a kill (an Action). DriverLimit: the
    // driver's own kill at a line over max_line_bytes. Deadline: hello or command deadline.
    // Exit: the adapter ended on its own.
    enum KilledBy { NotKilled, Deadline, Guard, Exit, DriverLimit };

    CommandOutcome()
        : result(Ok), errorClass(ErrorClass::Transient), retryAfterS(-1), killedBy(NotKilled),
          durationMs(0), linesRead(0), bytesRead(0), batches(0), recordsReceived(0),
          recordsValid(0), softViolations(0), hardViolations(0) {}

    LinkId link;
    QString commandId;
    QString commandType;                // wire name of the command
    Result result;
    ErrorClass errorClass;              // Error only
    int retryAfterS;                    // -1 = the adapter gave none
    KilledBy killedBy;
    qint64 durationMs;
    qint64 linesRead;
    qint64 bytesRead;
    qint64 batches;
    qint64 recordsReceived;
    qint64 recordsValid;
    QMap<int, qint64> quarantinedByReason;  // key: Validation::Reason
    int softViolations;
    int hardViolations;
};

// A record that passed validateRecord. The adapter sends subject_ref only, so
// record.subject stays null; the caller maps subjectRef to its Subject UUID.
struct LinkRecord
{
    QString subjectRef;
    Record record;
};

// ---- Link side: sinks (the driver stores nothing) --------------------------------------

class RecordSink
{
public:
    virtual ~RecordSink() {}
    // Valid records in arrival order, one call per protocol batch.
    virtual void accept(const LinkId &link, const QString &feed,
                        const QVector<LinkRecord> &records) = 0;
};

class QuarantineSink
{
public:
    virtual ~QuarantineSink() {}
    // reason is the name of the Validation::Reason; rawRecord is the object as received,
    // unmodified (REQ-DAT-08).
    virtual void reject(const LinkId &link, const QString &reason, const QString &detail,
                        const QJsonObject &rawRecord) = 0;
};

// Receives the protocol line of a batch that the guard ended the adapter for (REQ-GRD-13).
// Optional: without a sink the line is discarded. Called at most once per kill, after
// the process was ended. None of the records of that batch was delivered to the RecordSink;
// records quarantined before the kill stay in the QuarantineSink. The caller keeps the line
// in memory only, never on disk without the user's action, and builds the diagnostic report
// of REQ-GRD-15 from it (core/session/diagnosticreport.h).
class RejectedBatchSink
{
public:
    virtual ~RejectedBatchSink() {}
    // rawLine is the line exactly as received, without the line end. killedBy is always
    // Guard today. hasViolation tells whether the kill answered a violation (then `kind` is
    // the last one reported while the batch was handled) or the policy's own onBatch check
    // (then `kind` is meaningless).
    virtual void rejected(const LinkId &link, const QByteArray &rawLine,
                          CommandOutcome::KilledBy killedBy, bool hasViolation,
                          ViolationKind kind) = 0;
};

// ---- Policy side --------------------------------------------

struct LinkLimits
{
    LinkLimits()
        : maxLineBytes(1048576), maxRecordsPerBatch(500), helloTimeoutMs(10000),
          shutdownWaitMs(2000), authenticateDeadlineMs(30000),
          discoverSubjectsDeadlineMs(30000), fetchCurrentDeadlineMs(15000),
          fetchRangeDeadlineMs(60000), maxStderrBytes(65536),
          memoryLimitBytes(0), cpuTimeLimitS(0), fileDescriptorLimit(0),
          residentLimitBytes(0), memoryCheckIntervalMs(5000) {}

    int maxLineBytes;                   // ceiling 1048576
    int maxRecordsPerBatch;             // ceiling 500
    int helloTimeoutMs;                 // 10 s
    int shutdownWaitMs;                 // 2 s
    int authenticateDeadlineMs;         // defaults of contract section 4
    int discoverSubjectsDeadlineMs;
    int fetchCurrentDeadlineMs;
    int fetchRangeDeadlineMs;
    qint64 maxStderrBytes;              // cap on standard-error bytes kept
    // Optional OS limits applied at spawn where the platform allows; 0 = none
    // (REQ-GRD-01, REQ-GRD-02).
    qint64 memoryLimitBytes;
    int cpuTimeLimitS;
    int fileDescriptorLimit;
    // The limit on the adapter's RESIDENT size, watched by the driver while the process runs
    // 0 = none. Above it the process is ended and the command is a driver-limit kill.
    // It is a separate number from `memoryLimitBytes` (the address space) on purpose: the
    // address space of a healthy adapter is large and a limit on it is not used.
    qint64 residentLimitBytes;
    // How often the driver reads the size. A leak of about 1.4 MB in 11 hours does not need
    // speed; this only bounds how long a runaway adapter can grow before it is stopped.
    int memoryCheckIntervalMs;
};

struct StartRequest
{
    QString connectionId;
    QString adapterId;
    QString adapterVersion;             // empty before hello
};

struct MayStartResult
{
    enum Kind { Allowed, Denied };
    enum Reason { None, DisabledByGuard, AdapterVersionDisabled, CircuitOpen, SpawnRate };

    MayStartResult() : kind(Allowed), reason(None) {}
    static MayStartResult allowed() { return MayStartResult(); }
    static MayStartResult denied(Reason r) { MayStartResult m; m.kind = Denied; m.reason = r; return m; }

    Kind kind;
    Reason reason;
};

struct ExitStatus
{
    ExitStatus() : exitCode(0), crashed(false), peakResidentBytes(-1), killedForMemory(false) {}

    int exitCode;
    bool crashed;                       // killed by a signal or crashed
    // The highest resident size the driver saw, in bytes, -1 when it never could read it
    // (an in-memory end, a process that ended before the first look). The guard logs it with
    // the end of the session, so a slow growth shows over days without a profiler.
    qint64 peakResidentBytes;
    // The driver ended the process because it passed the resident limit.
    bool killedForMemory;
};

class LinkPolicy
{
public:
    virtual ~LinkPolicy() {}

    virtual LinkLimits limits() const = 0;
    virtual MayStartResult mayStart(const StartRequest &request) = 0;

    virtual void onSpawned(const LinkId &link) = 0;
    // Second policy check, after `hello` was admitted: only now is the adapter version
    // known (link.adapterVersion; it is empty at mayStart). A denial ends the start as
    // Refused(denied) and the process is stopped; no command can follow.
    virtual MayStartResult onAdmitted(const LinkId &link) = 0;
    virtual void onExited(const LinkId &link, const ExitStatus &status) = 0;

    // Sizes and counts only, never content.
    virtual Action onLine(const LinkId &link, qint64 bytes, const UtcTime &time) = 0;
    virtual Action onBatch(const LinkId &link, const QString &feed, int records,
                           qint64 bytes) = 0;
    virtual Action onStderr(const LinkId &link, qint64 bytes) = 0;

    // Answers like the other callbacks, so a hard violation can end the process at once,
    // also when the adapter goes quiet. For a violation Proceed and Drop are the same (the
    // offending message is already discarded); Pause stops reading; Kill ends the command
    // with protocol-changed and killed_by = guard.
    virtual Action onViolation(const Violation &violation) = 0;
    virtual void onCommandFinished(const CommandOutcome &outcome) = 0;
};

// Default policy for the command-line program: the contract defaults,
// proceed everywhere except that a hard violation is answered with kill (the driver itself
// also kills at a line over max_line_bytes and at a missed deadline), and a record of the
// violations for display. Defined in
// core/session/nullpolicy.cpp.
class NullPolicy : public LinkPolicy
{
public:
    NullPolicy();

    LinkLimits limits() const;
    MayStartResult mayStart(const StartRequest &request);
    void onSpawned(const LinkId &link);
    MayStartResult onAdmitted(const LinkId &link);
    void onExited(const LinkId &link, const ExitStatus &status);
    Action onLine(const LinkId &link, qint64 bytes, const UtcTime &time);
    Action onBatch(const LinkId &link, const QString &feed, int records, qint64 bytes);
    Action onStderr(const LinkId &link, qint64 bytes);
    Action onViolation(const Violation &violation);
    void onCommandFinished(const CommandOutcome &outcome);

    // Violations seen so far, in order; a hard one kills the command at once (Action::kill)
    // and the command-line run ends with exit 21.
    QList<Violation> violations() const { return m_violations; }
    bool sawHardViolation() const;

private:
    QList<Violation> m_violations;
};

} // namespace lm

#endif
