// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Michele Brami
#ifndef LM_VERSIONWATCH_H
#define LM_VERSIONWATCH_H

// The service notices that its own package was upgraded or removed.
//
// Why it is needed: a Harbour package may run no install scripts (Harbour FAQ 2.11), so
// nothing stops the running service when the package is upgraded or removed. The old process kept
// running for days on the author's phone with the old behaviour. The
// process cannot look at its own binary either: Sailjail gives it a private copy of /usr/bin that is
// never marked as replaced. What it CAN see is the package's folder
// /usr/share/<package>/, which is a live view of the installed files inside the sandbox. the packaging's
// package therefore installs a stamp file `version` there holding the package's Version-Release, the
// same text as this binary's LM_APP_VERSION.
//
// The rule, checked at every heartbeat while the service runs:
// - the stamp equals our own version: nothing to do;
// - the stamp holds ANOTHER version: a newer (or older) package is installed. Stop in order with the
//   reason `upgraded` and let the caller start the new binary by one activation of the D-Bus name;
// - the stamp is missing: the package was removed. Stop in order with the reason `package-removed`
//   and start nothing;
// - the stamp cannot be read, is empty or does not look like a version: a doubtful read. Monitoring
//   is never stopped on a doubtful read; one journal entry says so (one per episode, not per beat).
// A loop guard: if the service restarted itself for an upgrade less than 10 minutes ago and still
// sees a different stamp (the new binary is not the one the stamp names, for example a broken
// install), it does not restart again; it writes one journal entry instead and keeps monitoring.
// The time of the last self-restart is kept in the service's settings, because the guard has to
// work across the restart it guards.
//
// Pattern: pure logic behind ports (the stamp reader, the journal, the clock, the settings), so every
// case is a host test with fakes (tests/tst_versionwatch). It decides; the runtime acts (stop,
// activation), because the stop also has to end maintenance and discoveries (ServiceRuntime::stop).
//
// Assumptions (assumption register): a package replaces the stamp with a rename,
// so a running service never reads a half-written file or sees it missing during an upgrade; the
// sandbox shows the change at once (measured); a version is short
// text of letters, digits and . + ~ _ - (the RPM rules for Version and Release).

#include "service/serviceports.h"

#include <QString>

namespace lm {

// What reading the stamp file gave. `text` is the raw content (at most a few hundred bytes) when
// the file was read; the watch decides whether it looks like a version.
struct StampRead
{
    enum Kind { Present, Missing, Unreadable };
    StampRead() : kind(Unreadable) {}
    static StampRead present(const QString &text) { StampRead r; r.kind = Present; r.text = text; return r; }
    static StampRead missing() { StampRead r; r.kind = Missing; return r; }
    static StampRead unreadable() { StampRead r; r.kind = Unreadable; return r; }

    Kind kind;
    QString text;
};

// Port: reads the package's version stamp. The real one reads /usr/share/<package>/version
// (daemon/commonports.h, FileVersionStamp); tests use a scripted one.
class VersionStampReader
{
public:
    virtual ~VersionStampReader() {}
    virtual StampRead read() = 0;
};

// Port: starts the service again by one activation of its D-Bus name, after the old process has
// released the name. The real one pings the name as the add-on does and falls back to
// the bus's StartServiceByName (daemon/commonports.h, DBusSelfActivator). `how` names what worked or
// why it failed, as a short word for the process log; never a name or a path.
class ServiceActivator
{
public:
    virtual ~ServiceActivator() {}
    virtual bool activate(QString *how) = 0;
};

enum class VersionAction {
    None,           // keep running
    StopUpgraded,   // another version is installed: stop (reason `upgraded`) and activate the name
    StopRemoved     // the package is gone: stop (reason `package-removed`), start nothing
};

struct VersionWatchSettings
{
    VersionWatchSettings() : restartGuardS(600) {}
    // At most one self-restart for an upgrade in this time (10 minutes).
    int restartGuardS;
};

// Keys of the journal entry `version-check` (is withdrawn).
namespace VersionCheckKey {
inline QString result() { return QStringLiteral("result"); }            // see VersionCheckResult
inline QString appVersion() { return QStringLiteral("app_version"); }   // this binary's version
inline QString stamp() { return QStringLiteral("stamp"); }              // the stamp's version, when it looks like one
}
namespace VersionCheckResult {
inline QString stampUnreadable() { return QStringLiteral("stamp-unreadable"); }
inline QString restartSuppressed() { return QStringLiteral("restart-suppressed"); }
}

class VersionWatch
{
public:
    // The settings key that holds the wall time (ISO) of the last self-restart for an upgrade.
    static QString restartKey() { return QStringLiteral("version.self_restart_at"); }

    // Nothing is owned. `ownVersion` is this binary's LM_APP_VERSION. `settings` may be null (then the
    // loop guard lasts only as long as this process, which a test may want).
    VersionWatch(VersionStampReader *reader, const QString &ownVersion, ServiceJournal *journal,
                 ServiceClock *clock, ServiceSettingsStore *settings,
                 const VersionWatchSettings &s = VersionWatchSettings());

    // One check; call it at each heartbeat while the service runs and never during a stop.
    VersionAction check();

    // The caller is about to stop for an upgrade and activate the name: remember the time for the
    // loop guard, here and in the settings.
    void noteSelfRestart();

    // The stamp's version of the last check, when it looked like a version (for the stop record).
    QString stampVersion() const { return m_lastStamp; }
    QString ownVersion() const { return m_own; }

    // True when `text` (one line, surrounding white space ignored) looks like an RPM Version-Release.
    static bool looksLikeVersion(const QString &text);

private:
    bool restartedRecently() const;
    void note(const QString &result, const QString &stamp);

    VersionStampReader *m_reader;
    QString m_own;
    ServiceJournal *m_journal;
    ServiceClock *m_clock;
    ServiceSettingsStore *m_settings;
    VersionWatchSettings m_s;
    QString m_lastStamp;
    UtcTime m_restartAt;        // in this process; the settings keep it across the restart
    bool m_doubtNoted;          // one journal entry per episode of doubtful reads
    bool m_suppressNoted;       // one journal entry per episode of suppressed restarts
};

} // namespace lm

#endif
