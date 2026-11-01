// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Michele Brami
#ifndef LM_NOTIFICATIONTEXTS_H
#define LM_NOTIFICATIONTEXTS_H

// THE WORDS OF THE NOTIFICATIONS (REQ-ALR-10).
// The other half of the catalogue (notificationcatalog.h holds the hints).
//
// What the rules are: the summary is short, in capitals, says what the notification is about to
// someone who does not know the application, and carries NO label and NO application name; the
// body carries the details in two or three lines; the banner and the page show the same summary
// and the same body. Levels are named in words only. Line 1 of an alarm is the Subject's short
// label, and only with the setting "Show name and value in alerts" the newest value, its unit and
// its trend after it. Plain text: nothing here is markup, and a `<` or `&` in a label is shown as it is.
//
// How they are translated: every text is an identifier-based translation (`qtTrId`, the same
// scheme as the user interface), in the SAME translation files, so a new
// language is one more file for both (REQ-UI-13). English is the source and is written beside
// each use as a `//%` comment, for `lupdate`. The service installs the translator of the
// application's language (NotificationTranslator) and changes it at `ConfigChanged` without a
// restart. Plurals use the translation system's own forms (`%n`); times use the language's own
// short time format. The cause of a missing-data alert is the user interface's own text of the
// same staleness cause (REQ-SCH-07, the ids `ui-stale-*`), so the two say the same thing.
//
// Pure: no clock (the caller passes `now`), no platform. A test checks every text in both
// languages, with and without the value, and that no value, name or digit leaks into a text
// that must not have one.

#include "service/effectfacts.h"
#include "service/notificationcatalog.h"

#include <QLocale>
#include <QString>
#include <QTranslator>

#include <functional>

namespace lm {

// A summary and a body: what the page shows, and what the banner shows (the same).
struct NoteText
{
    QString summary;
    QString body;       // lines joined by "\n"
};

// What an alarm text needs besides the effect's facts.
struct AlarmTextInput
{
    AlarmTextInput() : haveValue(false), showValue(false), trendCode(-1) {}

    QString label;              // the Subject's short label (or its position), never a name
    bool showValue;             // the setting "Show name and value in alerts" (off by default)
    bool haveValue;             // a newest reading could be read from the store
    double value;
    QString unit;               // as stored: "mg/dL" or "mmol/L"
    int trendCode;              // 1 to 5, -1 none
    EffectFacts facts;
    UtcTime now;
};

// One entry of the resident notice's list of alerts.
struct ResidentAlert
{
    ResidentAlert() : kind(NoteKind::Low), acknowledged(false) {}
    NoteKind kind;
    QString label;
    bool acknowledged;
};

struct ResidentInput
{
    ResidentInput() : patients(0), doNotDisturb(false) {}

    UtcTime updated;
    int patients;
    QList<ResidentAlert> alerts;
    QStringList silentLabels;
    bool doNotDisturb;
    QString connectionLabel;    // awaiting approval: the label of the first waiting connection
    QString cause;              // attention: the cause key
    UtcTime stoppedAt;
};

class NotificationTexts
{
public:
    // `timeFormat` turns an instant into the text of the time; the default is the language's own
    // short time format in the phone's time zone. A test passes a fixed zone.
    typedef std::function<QString(const UtcTime &)> TimeFormat;
    explicit NotificationTexts(const TimeFormat &timeFormat = TimeFormat(),
                               const QLocale &locale = QLocale());

    NoteText alarm(NoteKind kind, const AlarmTextInput &in) const;
    // The page-only entry of an alert that cleared unacknowledged.
    NoteText ended(NoteKind kind, const QString &label, const EffectFacts &facts) const;
    NoteText waitingForApproval(const QString &label) const;
    NoteText interruption(const UtcTime &from, const UtcTime &to) const;
    NoteText guardDisabled(const QString &connectionLabel, int attempts, const QString &causeKey) const;
    NoteText reportProposed() const;
    NoteText alertsWillBeSilent() const;

    // The resident notice, four states.
    NoteText residentActive(const ResidentInput &in) const;
    NoteText residentAwaitingApproval(const ResidentInput &in) const;
    NoteText residentAttention(const ResidentInput &in) const;
    NoteText residentStopped(const ResidentInput &in) const;

    // The text of a staleness cause key (REQ-SCH-07), the user interface's own words.
    static QString causeText(const QString &staleCauseKey);
    // The arrow of a trend code (1 falling fast ... 5 rising fast); "" for none.
    static QString trendArrow(int trendCode);
    // "52 mg/dL" with an arrow; a mmol/L value keeps one decimal.
    static QString valueText(double value, const QString &unit, int trendCode);
    // A limit in whole mg/dL shown in the Subject's unit.
    static QString limitText(int mgDl, const QString &unit);

    QString time(const UtcTime &t) const { return m_time(t); }

private:
    static int minutesBetween(const UtcTime &from, const UtcTime &to);
    static QString joined(const QStringList &lines);
    static QString alertWord(NoteKind kind, const QString &label, bool acknowledged);

    TimeFormat m_time;
};

// Installs the translator of the notification texts for a language and removes the previous one.
// `language` is a code such as "it" or "en"; empty means the phone's language. The files are the
// application's own (`<base>.qm` for English, `<base>-<language>.qm` for the others) in
// `directory`; English is also the fallback when a language has no file. Returns the code that
// was loaded ("en" when it fell back). The service keeps one of these for its lifetime.
class NotificationTranslator
{
public:
    NotificationTranslator() : m_language(QStringLiteral("en")) {}
    ~NotificationTranslator();

    QString load(const QString &directory, const QString &baseName, const QString &language);
    QString language() const { return m_language; }

private:
    void unload();

    QTranslator m_base;
    QTranslator m_specific;
    bool m_baseInstalled = false;
    bool m_specificInstalled = false;
    QString m_language;
};

} // namespace lm

#endif
