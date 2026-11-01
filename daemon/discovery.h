// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Michele Brami
#ifndef LM_DISCOVERY_H
#define LM_DISCOVERY_H

// "Which people can this account follow?" for the user interface's set-up page: the D-Bus
// method `Discover(connection)`. One discovery logs in to the
// source through a short-lived adapter process, asks it for the Subjects the account may see,
// answers ONE JSON text and ends the process.
//
// Two kinds, one code path:
//  - a KNOWN connection (one in the configuration): its stored credentials are used, and its
//    stored session is offered to the adapter first so that no new login is made while the
//    scheduler's own session is alive;
//  - the PROBE, the reserved identifier `probe`: a login that is not saved anywhere yet. The
//    UI stores the user's entry under the secret name `probe` (the password may not travel in a
//    D-Bus message, REQ-SEC-01), calls Discover("probe") and the service treats it as a one-time
//    probe of the built-in adapter `libre`: it reads the secret, REMOVES it at once (the
//    credentials are only needed in memory from then on), stores nothing, keeps no session and
//    never counts as a connection for the guard's persistent state.
//
// Pattern: a service object that starts independent one-shot "runs", each a small state
// machine (secret -> start -> authenticate -> discover -> answer) with its own adapter link and
// its OWN guard. Why a guard of its own: the guard's policy (line and byte limits, the kill on a
// hard violation, the spawn rate) must apply to this adapter process like to any other (
// "through the guard's policy"), but its persistent state belongs to real connections. A run's
// guard keeps its state in memory and is dropped with the run, so a misbehaving adapter ends the
// run and answers `protocol-changed` without disabling a connection or an adapter version in the
// journal. The persistent state is only READ, before the run starts: a connection the guard has
// disabled, or an adapter version it has stopped, is not started again by a discovery.
//
// Why not the scheduler's session for a known connection: the session is busy polling, a second
// command would have to queue inside the state machine (a change there, outside this
// task), and the stored session blob already lets this run reuse the live login. Assumption
// the adapter accepts a still-valid stored session and then makes no new login, so a
// discovery for a known connection does not invalidate the scheduler's token. If the stored
// session is missing or near its end, a fresh login is made, as the scheduler would do itself.
//
// Limits (provisional): one discovery per connection key every 30 s and six per hour,
// and one at a time per key; beyond that the answer is `rate-limited` and nothing is sent to the
// source. A watchdog ends a run that does not answer within about a minute (`unreachable`).
// The counters are in memory: a restart of the service forgets them.
//
// What the answer never holds: a credential, a session or a token. Errors are classes only.
// The one thing it does carry is the Subjects' display names, which are personal data; it goes
// to the UI on the session bus of the same user and the service logs none of it.
//
// Answer shapes (the keys are the contract):
//   {"ok":true,"subjects":[{"subject_ref":..,"display_name":..,"unit":..,"unit_basis":..}]}
//   {"ok":false,"error":"<class>"}
// Classes: auth-failed, terms-must-be-accepted, rate-limited, unreachable,
// protocol-changed. Additional classes for states does not name (proposed):
// unknown-connection, credentials-missing, secrets-locked, stopped-by-guard, service-stopping,
// internal.

#include "guard/ports.h"
#include "guard/statekeeper.h"
#include "scheduler/schedulerports.h"
#include "session/adapterlink.h"
#include "daemon/provisioning.h"
#include "scheduler/schedulersettings.h"

#include <QHash>
#include <QList>
#include <QObject>
#include <QPointer>
#include <QSet>

#include <functional>

namespace lm {

// The reserved connection identifier of the probe. It can never be a configured
// connection: the configuration reader and the provisioning code refuse it.
inline QString probeConnectionId() { return QStringLiteral("probe"); }

struct DiscoverySettings
{
    DiscoverySettings()
        : minSpacingMs(30 * 1000), maxPerHour(6), timeoutMs(60 * 1000),
          staleCleanRetryMs(5 * 60 * 1000) {}

    qint64 minSpacingMs;        // between two discoveries of the same key
    int maxPerHour;             // per key
    qint64 timeoutMs;           // the watchdog: "within about a minute"
    qint64 staleCleanRetryMs;   // how often a locked probe secret is looked at again
};

// Everything a discovery needs from its surroundings; none of it is owned.
struct DiscoveryPorts
{
    DiscoveryPorts() : clock(0), secrets(0), writer(0), state(0) {}

    Clock *clock;
    SecretStore *secrets;               // the same store the scheduler reads
    CredentialWriter *writer;           // only remove() is used: the probe secret
    const StateKeeper *state;           // the PERSISTENT state, read only
    // The configured connection with this id; an empty `id` in the result means unknown.
    std::function<ConnectionConfig(const QString &)> connection;
    // The non-secret settings the adapter was last given for a connection (the regional host).
    std::function<QJsonObject(const QString &)> settings;
    // A fully configured link (program or channel, clock) for the adapter, without sinks: a
    // discovery receives no records. The policy is the run's own guard.
    std::function<AdapterLink *(const QString &adapterId, LinkPolicy *policy)> makeLink;
};

class DiscoveryService : public QObject
{
    Q_OBJECT
public:
    typedef std::function<void(const QString &json)> Done;

    DiscoveryService(const DiscoveryPorts &ports, const DiscoverySettings &settings = DiscoverySettings(),
                     QObject *parent = 0);
    ~DiscoveryService();

    // Starts a discovery of `connection` (a configured id, or `probe`). `done` is called exactly
    // once with the answer, from the event loop, never before this function has returned.
    void discover(const QString &connection, const Done &done);

    // Ends every running discovery now (their processes are killed) and answers each with
    // `service-stopping`. Used by the stop sequence.
    void cancelAll();
    int running() const { return m_runs.size(); }

    // The stale probe secret: a probe secret that is still there when the service starts
    // is left from a UI that died between storing it and removing it. This looks for it and
    // removes it; when the secret store is locked (after a phone restart) it is looked at again
    // at later ticks, at most every `staleCleanRetryMs`, until the store answers.
    void cleanStaleProbe();
    void tick();
    bool cleanupPending() const { return m_cleanPending; }

    // The answer texts, public for the tests.
    static QString errorAnswer(const QString &errorClass);
    static QString subjectsAnswer(const QList<SubjectInfo> &subjects);

private:
    class Run;
    friend class Run;

    bool admit(const QString &key);
    void note(const QString &key);
    void forget(Run *run);
    void tryCleanStale();
    void removeProbeSecret();

    DiscoveryPorts m_p;
    DiscoverySettings m_s;
    QList<Run *> m_runs;
    QHash<QString, QList<qint64> > m_starts;    // key -> start times (ms) within the last hour
    bool m_cleanPending;
    qint64 m_nextCleanMs;
    bool m_destroying;
};

} // namespace lm

#endif
