// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Michele Brami
#ifndef LM_DIAGNOSTICCOLLECTOR_H
#define LM_DIAGNOSTICCOLLECTOR_H

// What the service needs to answer `DiagnosticReport(connection)`: after the guard ended an adapter twice in a row, the user is
// offered a text file for the developer, and the user interface asks the service for its
// content. The guard keeps only counters and scores; nothing in the service kept the batch the adapter
// was killed in, nor the command and the violations of that run. This class keeps them, in memory
// only, per connection, for the LAST guard kill:
//   - the raw protocol line of the batch the adapter was ended in (REQ-GRD-13), when the kill fell in
//     the middle of a batch (the driver hands it over through RejectedBatchSink);
//   - the adapter's id and version, the command's wire name, the counts of that command, and the
//     violations reported during it (kind and short detail, which name a limit or a field and never
//     carry content).
// A command that ends without a guard kill and without error discards what was held for its
// connection (: "a successful retry discards the held batch"). Nothing is stored on
// disk and a restart forgets everything, as the proposal itself (core/service/reportproposal.h).
//
// Pattern: decorator. It IS the link policy the driver talks to (core/session/linkpolicy.h) and passes
// every call unchanged to the real one (the guard), noting on the way what it needs; the guard's
// answers are returned as they are, so the guard's behaviour cannot change. It is also the
// RejectedBatchSink of every link. One instance serves all the links of the service; calls arrive on
// the service's one event-loop thread.
//
// The text it gives back is made by the pure function (core/session/diagnosticreport.h,
// a whitelist anonymiser) and then STRIPPED OF EVERY MEASUREMENT VALUE: says the answer holds no
// glucose value, name, reference, address or credential. buildDiagnosticReport keeps measurement
// values on purpose ("the problem cannot be understood without them"), so the two decisions
// differ; this class follows the newer one and replaces each value by its type, "number".

#include "session/diagnosticreport.h"
#include "session/linkpolicy.h"

#include <QByteArray>
#include <QHash>
#include <QList>
#include <QString>

namespace lm {

class DiagnosticCollector : public LinkPolicy, public RejectedBatchSink
{
public:
    // `inner` is the real policy (the guard); not owned, must outlive this object.
    explicit DiagnosticCollector(LinkPolicy *inner) : m_inner(inner) {}

    // ---- LinkPolicy: every call goes to the inner policy unchanged -------------------------
    LinkLimits limits() const { return m_inner->limits(); }
    MayStartResult mayStart(const StartRequest &request) { return m_inner->mayStart(request); }
    void onSpawned(const LinkId &link) { m_inner->onSpawned(link); }
    MayStartResult onAdmitted(const LinkId &link) { return m_inner->onAdmitted(link); }
    void onExited(const LinkId &link, const ExitStatus &status) { m_inner->onExited(link, status); }
    Action onLine(const LinkId &link, qint64 bytes, const UtcTime &time) { return m_inner->onLine(link, bytes, time); }
    Action onBatch(const LinkId &link, const QString &feed, int records, qint64 bytes)
    {
        return m_inner->onBatch(link, feed, records, bytes);
    }
    Action onStderr(const LinkId &link, qint64 bytes) { return m_inner->onStderr(link, bytes); }
    // Notes the violation for the command in progress on that connection, then asks the guard.
    Action onViolation(const Violation &violation);
    // Ends the notes of the command: a guard kill keeps them as the connection's held report, a
    // clean success discards what was held, anything else (an error the adapter reported, a
    // deadline) leaves the held report as it was.
    void onCommandFinished(const CommandOutcome &outcome);

    // ---- RejectedBatchSink ----------------------------------------------------------------------
    void rejected(const LinkId &link, const QByteArray &rawLine, CommandOutcome::KilledBy killedBy,
                  bool hasViolation, ViolationKind kind);

    // True when a guard kill of the connection was noted and not yet discarded.
    bool hasReport(const QString &connection) const { return m_held.contains(connection); }
    // Forgets what is held for the connection (a re-enable, a success).
    void discard(const QString &connection) { m_held.remove(connection); m_running.remove(connection); }

    // The anonymised report of the last guard kill of the connection as JSON Lines (one object per
    // line, each ending in a newline), with every measurement value replaced by its type. Empty when
    // nothing is held. `applicationVersion` is the package's (LM_APP_VERSION).
    QByteArray report(const QString &connection, const QString &applicationVersion) const;

    // The same stripping as report() applies, for any text made by buildDiagnosticReport: a record's
    // `value` and its payload's `value` become "number" when they are numbers. Public for the tests.
    static QByteArray withoutMeasurements(const QByteArray &jsonLines);

private:
    struct Run
    {
        Run() : hasBatch(false) {}
        LinkId link;
        QList<DiagnosticViolation> violations;
        bool hasBatch;
        QByteArray batch;
    };
    struct Held
    {
        Run run;
        CommandOutcome outcome;
    };

    LinkPolicy *m_inner;
    QHash<QString, Run> m_running;     // per connection: the command in progress
    QHash<QString, Held> m_held;       // per connection: the last guard kill
};

} // namespace lm

#endif
