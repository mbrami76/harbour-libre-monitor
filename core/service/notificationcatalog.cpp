// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Michele Brami
#include "service/notificationcatalog.h"

#include <QList>

namespace lm {

namespace {

// The category of everything the service posts, so the platform (and a later setting) can group
// it. A name of our own, not one of the platform's.
const char *const kCategory = "x-libre-monitor.alert";

// The system events (feedback). Names the phone's sound service knows; S9 and U2 saw each work.
const char *const kAlarm = "clock";                 // repeating tone, fades in, audible in the silent profile
const char *const kCalendar = "calendar";           // short; the author's choice for low and high
const char *const kWarning = "general_warning";     // short; no data and the approval notice

NoteStyle row(int urgency, int priority, const char *feedback, bool displayOn, bool banner,
              const char *icon, bool removable)
{
    NoteStyle s;
    s.urgency = urgency;
    s.priority = priority;
    s.feedback = QString::fromLatin1(feedback);
    s.displayOn = displayOn;
    s.banner = banner;
    s.icon = QString::fromLatin1(icon);
    s.removable = removable;
    return s;
}

} // namespace

// The table. Order of the enum; one row per kind
//   urgent low / urgent high: critical, priority 125, the alarm event until closed
//   low / high:               critical, priority 100, `calendar`, wakes the screen
//   no data:                  critical, priority 100, `general_warning`, wakes the screen
//   interruption:             page only (urgency 0), priority 50, no feedback
//   waiting for approval:     normal, priority 100, `general_warning` at each re-raise
//   guard disabled, report:   page only, priority 50
//   alerts will be silent:    normal, priority 50, no feedback, a banner (also under DND, U2b)
//   ended entries:            page only, priority 50, removable by the user
const NoteStyle &NotificationCatalog::style(NoteKind kind)
{
    static const NoteStyle table[] = {
        /* UrgentLow       */ row(2, 125, kAlarm,    true,  true,  "alert-ll",     false),
        /* UrgentHigh      */ row(2, 125, kAlarm,    true,  true,  "alert-hh",     false),
        /* Low             */ row(2, 100, kCalendar, true,  true,  "alert-l",      false),
        /* High            */ row(2, 100, kCalendar, true,  true,  "alert-h",      false),
        /* NoData          */ row(2, 100, kWarning,  true,  true,  "alert-nodata", false),
        /* Interruption    */ row(0,  50, "",        false, false, "notice-2",    false),
        /* WaitingApproval */ row(1, 100, kWarning,  false, true,  "notice-2",    false),
        /* GuardDisabled   */ row(0,  50, "",        false, false, "notice-3",    false),
        /* ReportProposed  */ row(0,  50, "",        false, false, "notice-2",    false),
        /* AlertsSilent    */ row(1,  50, "",        false, true,  "notice-1",    false),
        /* EndedUrgentLow  */ row(0,  50, "",        false, false, "ended-ll", true),
        /* EndedUrgentHigh */ row(0,  50, "",        false, false, "ended-hh", true),
        /* EndedLow        */ row(0,  50, "",        false, false, "ended-l",  true),
        /* EndedHigh       */ row(0,  50, "",        false, false, "ended-h",  true),
    };
    return table[static_cast<int>(kind)];
}

NotificationCatalog::NotificationCatalog(const QString &iconDirectory)
    : m_iconDirectory(iconDirectory)
{
    if (!m_iconDirectory.isEmpty() && !m_iconDirectory.endsWith(QLatin1Char('/')))
        m_iconDirectory += QLatin1Char('/');
}

NoteKind NotificationCatalog::alarmKindOf(AlertType type)
{
    switch (type) {
    case AlertType::LL: return NoteKind::UrgentLow;
    case AlertType::L: return NoteKind::Low;
    case AlertType::H: return NoteKind::High;
    case AlertType::HH: return NoteKind::UrgentHigh;
    case AlertType::Missing: return NoteKind::NoData;
    }
    return NoteKind::NoData;
}

bool NotificationCatalog::endedKindOf(AlertType type, NoteKind *kind)
{
    switch (type) {
    case AlertType::LL: *kind = NoteKind::EndedUrgentLow; return true;
    case AlertType::L: *kind = NoteKind::EndedLow; return true;
    case AlertType::H: *kind = NoteKind::EndedHigh; return true;
    case AlertType::HH: *kind = NoteKind::EndedUrgentHigh; return true;
    case AlertType::Missing: return false;
    }
    return false;
}

bool NotificationCatalog::isAlarm(NoteKind kind)
{
    return kind == NoteKind::UrgentLow || kind == NoteKind::UrgentHigh || kind == NoteKind::Low
        || kind == NoteKind::High || kind == NoteKind::NoData;
}

bool NotificationCatalog::isUrgentAlarm(NoteKind kind)
{
    return kind == NoteKind::UrgentLow || kind == NoteKind::UrgentHigh;
}

int NotificationCatalog::rankOf(NoteKind kind)
{
    switch (kind) {
    case NoteKind::UrgentLow: return 1;
    case NoteKind::UrgentHigh: return 2;
    case NoteKind::NoData: return 3;
    case NoteKind::Low: return 4;
    case NoteKind::High: return 5;
    default: return 0;
    }
}

QString NotificationCatalog::shortEvent(NoteKind kind)
{
    return QString::fromLatin1(kind == NoteKind::NoData ? kWarning : kCalendar);
}

// The icon of the resident notice follows its state: one mark
// while all is well, two marks when the monitor waits for the user or needs attention, three marks
// when it is stopped. One place, so that a new design is a new file and nothing else.
QString NotificationCatalog::residentIconPath(NoticeState state) const
{
    if (m_iconDirectory.isEmpty())
        return QString();
    QString file;
    switch (state) {
    case NoticeState::Active: file = QStringLiteral("notice-1"); break;
    case NoticeState::AwaitingApproval:
    case NoticeState::Attention: file = QStringLiteral("notice-2"); break;
    case NoticeState::Stopped: file = QStringLiteral("notice-3"); break;
    }
    return m_iconDirectory + file + QStringLiteral(".png");
}

NoteHints NotificationCatalog::hints(NoteKind kind, Sound sound) const
{
    const NoteStyle &s = style(kind);
    NoteHints h;
    h.urgency = s.urgency;
    h.priority = s.priority;
    h.displayOn = s.displayOn;
    h.banner = s.banner;
    h.removable = s.removable;
    h.category = QString::fromLatin1(kCategory);
    if (!m_iconDirectory.isEmpty() && !s.icon.isEmpty())
        h.iconPath = m_iconDirectory + s.icon + QStringLiteral(".png");
    switch (sound) {
    case Sound::Own: h.feedback = s.feedback; break;
    case Sound::Short: h.feedback = isAlarm(kind) ? shortEvent(kind) : s.feedback; break;
    case Sound::None: break;
    }
    return h;
}

const QList<NoteKind> &NotificationCatalog::all()
{
    static const QList<NoteKind> list = QList<NoteKind>()
        << NoteKind::UrgentLow << NoteKind::UrgentHigh << NoteKind::Low << NoteKind::High << NoteKind::NoData
        << NoteKind::Interruption << NoteKind::WaitingApproval << NoteKind::GuardDisabled
        << NoteKind::ReportProposed << NoteKind::AlertsSilent
        << NoteKind::EndedUrgentLow << NoteKind::EndedUrgentHigh << NoteKind::EndedLow << NoteKind::EndedHigh;
    return list;
}

QString NotificationCatalog::name(NoteKind kind)
{
    switch (kind) {
    case NoteKind::UrgentLow: return QStringLiteral("urgent-low");
    case NoteKind::UrgentHigh: return QStringLiteral("urgent-high");
    case NoteKind::Low: return QStringLiteral("low");
    case NoteKind::High: return QStringLiteral("high");
    case NoteKind::NoData: return QStringLiteral("no-data");
    case NoteKind::Interruption: return QStringLiteral("interruption");
    case NoteKind::WaitingApproval: return QStringLiteral("waiting-approval");
    case NoteKind::GuardDisabled: return QStringLiteral("guard-disabled");
    case NoteKind::ReportProposed: return QStringLiteral("report-proposed");
    case NoteKind::AlertsSilent: return QStringLiteral("alerts-silent");
    case NoteKind::EndedUrgentLow: return QStringLiteral("ended-urgent-low");
    case NoteKind::EndedUrgentHigh: return QStringLiteral("ended-urgent-high");
    case NoteKind::EndedLow: return QStringLiteral("ended-low");
    case NoteKind::EndedHigh: return QStringLiteral("ended-high");
    }
    return QString();
}

} // namespace lm
