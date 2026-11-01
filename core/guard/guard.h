// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Michele Brami
#ifndef LM_GUARD_H
#define LM_GUARD_H

// The guard (REQ-GRD-01 to 07, 10, 11): the
// policy side of the link between the core and one adapter process. The driver
// reports facts (a line arrived, a violation happened) and applies the answer at once; the
// guard decides what they weigh and what to do. It defends against faults and resource
// abuse, NOT against hostile code (REQ-GRD-12).
//
// Pattern: policy object behind the LinkPolicy interface, with one small state record per
// connection. The reaction ladder of REQ-GRD-03 is built from the answers it can give:
//   1 backpressure   -> Action::pause      (a token bucket is empty)
//   2 drop and count -> Action::drop       (a batch over its allowance)
//   3 quarantine     -> done by the driver and the store, not a violation
//   4 kill + backoff -> Action::kill       (any hard violation; the scheduler backs off)
//   5 circuit breaker-> mayStart denies    (repeated process-level failures, closes by itself)
//   6 trip           -> disabled-by-guard  (score at the limit; persistent, never automatic)
// Assumptions: the severity of a violation is the contract's; the
// weights, the decay and every limit are provisional and live in GuardSettings; one
// thread, the Qt event loop; the version of an adapter is known only after `hello`
// so mayStart can use only the version seen earlier in this run.

#include "guard/guardsettings.h"
#include "guard/ingestlog.h"
#include "guard/ports.h"
#include "guard/statekeeper.h"
#include "guard/violationscore.h"
#include "session/linkpolicy.h"

#include <QHash>
#include <QList>
#include <QMap>
#include <QSet>

namespace lm {

// Told when the guard changes the state of a connection or an adapter version, so the
// scheduler can move the schedules.
class GuardListener
{
public:
    virtual ~GuardListener() {}
    virtual void guardTripped(const QString &connection) = 0;
    virtual void guardVersionTripped(const QString &adapter, const QString &adapterVersion) = 0;
    virtual void guardReenabled(const QString &connection) = 0;
    virtual void guardVersionReenabled(const QString &adapter, const QString &adapterVersion) = 0;
};

class Guard : public LinkPolicy
{
public:
    // None of the pointers is owned; all must outlive the guard.
    Guard(Clock *clock, StateKeeper *state, IngestLog *ingest,
          const GuardSettings &settings = GuardSettings());

    void setListener(GuardListener *listener) { m_listener = listener; }
    const GuardSettings &settings() const { return m_settings; }

    // ---- LinkPolicy (called by the driver) ---------------------------------------------
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

    // ---- called by the scheduler (same module, same thread) ----------------------------
    // Tells the guard what the next command should return, because onBatch carries no
    // command, Subject or metric. `spanMinutes` is the length of the range
    // asked for (1 for the current value).
    void beginCommand(const LinkId &link, const QString &commandType, int subjects,
                      double spanMinutes);
    // A start that failed before a link existed (spawn failure, hello timeout) counts toward
    // the circuit breaker like any process-level failure.
    void noteStartFailure(const QString &connection);

    // Manual re-enabling, as logic only (REQ-GRD-08): clears the persistent state,
    // resets the score and writes the acknowledgement to the ingest log. False when the
    // connection (or version) was not disabled.
    bool reenable(const QString &connection);
    bool reenableVersion(const QString &adapter, const QString &adapterVersion);

    // ---- queries -----------------------------------------------------------------------
    bool isDisabled(const QString &connection) const { return m_state->connectionDisabled(connection); }
    double scoreOf(const QString &connection) const;
    // Invalid when the circuit is closed.
    UtcTime circuitReopenAt(const QString &connection) const;
    // When the spawn bucket holds one token; invalid if it does already.
    UtcTime spawnReadyAt(const QString &connection) const;

private:
    struct Conn
    {
        Conn() : score(), lineEpisode(false), byteEpisode(false), circuitOpen(false),
                 circuitUntilMs(0), circuitLengthMs(0), trial(false), hasExpect(false),
                 subjects(1), spanMinutes(1), cmdTotal(0), rateFlagged(false), capFlagged(false),
                 windowOver(false) {}

        QString adapterId;
        QString observedVersion;        // seen in this run only (see the class comment)
        ViolationScore score;
        TokenBucket lines;
        TokenBucket bytes;
        TokenBucket spawn;
        bool lineEpisode;
        bool byteEpisode;
        QList<qint64> failures;
        bool circuitOpen;
        qint64 circuitUntilMs;
        qint64 circuitLengthMs;
        bool trial;
        QSet<QString> countedSessions;  // sessions whose failure the circuit already has
        // The command in flight.
        bool hasExpect;
        QString commandType;
        int subjects;
        double spanMinutes;
        QMap<QString, qint64> feedRecords;
        qint64 cmdTotal;
        bool rateFlagged;
        bool capFlagged;
        QList<QPair<qint64, int> > window;  // (time, records) within recordsWindowMs
        bool windowOver;
    };

    Conn &conn(const QString &connectionId, const QString &adapterId = QString());
    UtcTime nowFor(const UtcTime &given) const;
    void log(const LinkId &link, const QString &event, const QString &detail);
    void log(const QString &connection, const QString &adapter, const QString &version,
             const QString &event, const QString &detail);
    Action violate(const LinkId &link, ViolationKind kind, Severity severity,
                   const QString &detail, const UtcTime &when);
    void trip(const LinkId &link, Conn &c, double score);
    void addFailure(const QString &connection, const QString &adapter, const QString &version,
                    const QString &sessionId);
    void closeCircuit(const QString &connection, const QString &adapter, const QString &version,
                      bool announce);

    Clock *m_clock;
    StateKeeper *m_state;
    IngestLog *m_ingest;
    GuardSettings m_settings;
    GuardListener *m_listener;
    QHash<QString, Conn> m_conns;
};

} // namespace lm

#endif
