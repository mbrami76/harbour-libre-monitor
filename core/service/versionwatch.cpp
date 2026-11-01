// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Michele Brami
#include "service/versionwatch.h"

#include <QRegularExpression>

namespace lm {

VersionWatch::VersionWatch(VersionStampReader *reader, const QString &ownVersion,
                           ServiceJournal *journal, ServiceClock *clock,
                           ServiceSettingsStore *settings, const VersionWatchSettings &s)
    : m_reader(reader), m_own(ownVersion), m_journal(journal), m_clock(clock),
      m_settings(settings), m_s(s), m_doubtNoted(false), m_suppressNoted(false)
{
}

// RPM allows letters, digits and . + ~ _ in Version and Release, joined here by one '-'. Anything
// else (an empty file, two lines, binary garbage, a very long text) is not a version: it is treated
// as a doubtful read, never as "another version", so a damaged file cannot make the service stop.
bool VersionWatch::looksLikeVersion(const QString &text)
{
    static const QRegularExpression re(QStringLiteral("^[0-9A-Za-z.+~_-]{1,64}$"));
    return re.match(text.trimmed()).hasMatch();
}

VersionAction VersionWatch::check()
{
    const StampRead r = m_reader->read();

    if (r.kind == StampRead::Missing) {
        // The package's folder no longer holds the stamp: the package was removed (rpm -e). The
        // caller stops; nothing starts again (the activation file is gone with the package anyway).
        m_lastStamp.clear();
        return VersionAction::StopRemoved;
    }

    if (r.kind == StampRead::Unreadable || !looksLikeVersion(r.text)) {
        // Never stop monitoring on a doubtful read. One entry per episode, so a file
        // that stays damaged does not fill the journal at one entry a minute.
        if (!m_doubtNoted) {
            note(VersionCheckResult::stampUnreadable(), QString());
            m_doubtNoted = true;
        }
        return VersionAction::None;
    }

    const QString stamp = r.text.trimmed();
    m_lastStamp = stamp;
    m_doubtNoted = false;

    if (stamp == m_own) {
        m_suppressNoted = false;
        return VersionAction::None;
    }

    // Another version is installed. At most one self-restart in restartGuardS. A
    // second difference within that time means the restart did not bring the stamp's version (a
    // broken or half-finished install, or a stamp that does not belong to the binary): restarting
    // again would only interrupt monitoring once a minute.
    if (restartedRecently()) {
        if (!m_suppressNoted) {
            note(VersionCheckResult::restartSuppressed(), stamp);
            m_suppressNoted = true;
        }
        return VersionAction::None;
    }
    return VersionAction::StopUpgraded;
}

void VersionWatch::noteSelfRestart()
{
    m_restartAt = m_clock->now();
    if (m_settings)
        m_settings->setValue(restartKey(), m_restartAt.toIso());
}

// The time of the last self-restart, from this process or from the settings the previous process
// wrote. A wall clock that was set back makes the difference negative: within one guard period
// that still counts as recent (the safe side: no restart), beyond it the guard is over.
bool VersionWatch::restartedRecently() const
{
    UtcTime at = m_restartAt;
    if (!at.isValid() && m_settings)
        at = UtcTime::fromIso(m_settings->value(restartKey()));
    if (!at.isValid())
        return false;
    const qint64 elapsedMs = m_clock->now().msecsSinceEpoch() - at.msecsSinceEpoch();
    const qint64 guardMs = qint64(m_s.restartGuardS) * 1000;
    return elapsedMs > -guardMs && elapsedMs < guardMs;
}

void VersionWatch::note(const QString &result, const QString &stamp)
{
    if (!m_journal)
        return;
    JournalEntry e;
    e.kind = JournalKind::versionCheck();
    e.wallTime = m_clock->now();
    e.extra.insert(VersionCheckKey::result(), result);
    e.extra.insert(VersionCheckKey::appVersion(), m_own);
    if (!stamp.isEmpty())
        e.extra.insert(VersionCheckKey::stamp(), stamp);
    m_journal->append(e);       // a failed write must not change what the service does
}

} // namespace lm
