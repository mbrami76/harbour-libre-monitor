// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Michele Brami
#ifndef LM_NOTIFICATIONCATALOG_H
#define LM_NOTIFICATIONCATALOG_H

// THE CATALOGUE of the notifications: one
// row per kind of notification, saying how loud, how urgent and how long it stays, which
// system sound it asks for, whether it wakes the screen and which icon it carries.
//
// Why one table. What the phone does with a notification was measured, not documented (
// U2, U2b), and a system release can change it. Everything that follows from those measurements
// is a number or a name in THIS file: a release that renames a sound event, or a decision that
// changes a priority, is a change here and nowhere else. The words of the notifications are in
// notificationtexts.*, which is the other half of the catalogue; the bookkeeping of ids, the
// arbiter of the long tone and the reminders are in alertnotifier.*, and none of them decides a
// hint.
//
// Pattern: a constant table behind accessors, plus a small class that turns a row into the
// hints a backend posts. Pure: no clock, no Qt event loop, no platform. Tests read every row.
//
// What the rows rest on (register MA-S9-xx, MA-U2-xx, MA-U2b-xx): no feedback hint means no sound
// or vibration whatever the urgency; `clock` is the system alarm (a repeating tone that fades in
// over about 20 s, audible in the silent profile, until the notification is closed); `calendar`
// and `general_warning` are short events (sound and vibration in the normal profile, vibration
// only in the silent profile); nothing sounds under Do not disturb; critical urgency (2) wakes
// the locked screen also under Do not disturb, urgency 1 does not; urgency 0 gives a page entry
// and no banner; the icon is a PNG with its own colours read from the application's folder and
// must never be tinted (`x-nemo-color` is never used).

#include "alerts/alerttypes.h"
#include "service/serviceports.h"

#include <QString>

namespace lm {

// Every kind the service posts, alarms first. The "Ended" kinds are the page-only entries left
// by a glucose alert that cleared while nobody had acknowledged it.
enum class NoteKind {
    UrgentLow, UrgentHigh, Low, High, NoData,
    Interruption, WaitingApproval, GuardDisabled, ReportProposed, AlertsSilent,
    EndedUrgentLow, EndedUrgentHigh, EndedLow, EndedHigh
};

// What a row asks of the phone.
struct NoteStyle
{
    NoteStyle() : urgency(1), priority(50), displayOn(false), banner(true), removable(false) {}

    int urgency;            // 0 page only, 1 normal, 2 critical (wakes the locked screen also under DND)
    int priority;           // `x-nemo-priority`; 100 and above also puts an icon and a counter on the lock screen
    QString feedback;       // the system event of the first post and of a reminder; "" = none
    bool displayOn;         // `x-nemo-display-on`: wake the screen without critical urgency
    bool banner;            // a banner (preview texts) as well as the page entry
    QString icon;           // file name without extension, in the icon folder
    bool removable;         // `x-nemo-user-removable`: only the "ended" entries ask for it
};

// What a backend needs to post one notification with these hints. Built by NotificationCatalog.
struct NoteHints
{
    NoteHints() : urgency(1), priority(50), displayOn(false), banner(true), removable(false) {}

    int urgency;
    int priority;
    QString feedback;       // "" = no sound and no vibration
    bool displayOn;
    bool banner;            // false = the preview texts stay empty and urgency is 0 (page only)
    QString iconPath;       // absolute path of the PNG, "" = none
    bool removable;
    QString category;       // one for everything the service posts
};

// Which sound a notification asks for at this moment. The
// arbiter decides; the catalogue only names the events.
enum class Sound {
    Own,        // the kind's own feedback event (the long `clock` for urgent low and urgent high)
    Short,      // the short event of the kind, never the long tone (a demoted alarm that gives its sound once)
    None        // no feedback hint: silent mode, a quiet update, a demoted alarm over a ringing tone
};

class NotificationCatalog
{
public:
    // `iconDirectory` is the folder with the icon PNGs (".../icons/"), with or without a trailing
    // slash; empty = no icon file is named (the platform shows the application's own icon).
    explicit NotificationCatalog(const QString &iconDirectory = QString());

    static const NoteStyle &style(NoteKind kind);

    // The alarm kind of an alert type, and the "ended" kind of the four glucose types (false for
    // no data: it leaves no ended entry).
    static NoteKind alarmKindOf(AlertType type);
    static bool endedKindOf(AlertType type, NoteKind *kind);
    static bool isAlarm(NoteKind kind);
    static bool isUrgentAlarm(NoteKind kind);
    // The strict rank: 1 urgent low, 2 urgent high, 3 no data, 4 low, 5 high; 0 for a
    // kind that is not an alarm.
    static int rankOf(NoteKind kind);
    // The short event of an alarm kind: `calendar` for the four glucose kinds (the author's choice by ear, U2b), `general_warning` for no data.
    static QString shortEvent(NoteKind kind);

    NoteHints hints(NoteKind kind, Sound sound) const;

    // The icon file of the resident notice in a state ("" without an icon folder).
    QString residentIconPath(NoticeState state) const;

    // A stable word for a kind, for logs and tests.
    static QString name(NoteKind kind);
    static const QList<NoteKind> &all();

private:
    QString m_iconDirectory;
};

} // namespace lm

#endif
