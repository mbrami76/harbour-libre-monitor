// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Michele Brami
#include "service/notificationtexts.h"

#include <QCoreApplication>
#include <QDateTime>
#include <QDir>
#include <QFileInfo>
#include <QStringList>

// The English source of every text is the `//%` comment above its qtTrId call: that is where
// lupdate reads it. The Italian texts are approved and live in
// app/translations/harbour-libre-monitor-it.ts with the user interface's.

namespace lm {

namespace {

QString nl(const QStringList &lines) { return lines.join(QLatin1Char('\n')); }

} // namespace

NotificationTexts::NotificationTexts(const TimeFormat &timeFormat, const QLocale &locale)
{
    if (timeFormat) {
        m_time = timeFormat;
    } else {
        // The language's own short form (24-hour or not), in the phone's time zone.
        m_time = [locale](const UtcTime &t) {
            return locale.toString(QDateTime::fromMSecsSinceEpoch(t.msecsSinceEpoch()).time(), QLocale::ShortFormat);
        };
    }
}

int NotificationTexts::minutesBetween(const UtcTime &from, const UtcTime &to)
{
    if (!from.isValid() || !to.isValid())
        return 0;
    const qint64 ms = to.msecsSinceEpoch() - from.msecsSinceEpoch();
    return ms <= 0 ? 0 : int(ms / 60000);
}

QString NotificationTexts::joined(const QStringList &lines) { return nl(lines); }

QString NotificationTexts::causeText(const QString &key)
{
    if (key == QLatin1String("source-unreachable")) return qtTrId("ui-stale-unreachable");
    if (key == QLatin1String("no-new-readings")) return qtTrId("ui-stale-no-new");
    if (key == QLatin1String("sensor-warm-up")) return qtTrId("ui-stale-warmup");
    if (key == QLatin1String("disabled-by-guard")) return qtTrId("ui-stale-guard");
    if (key == QLatin1String("service-not-running")) return qtTrId("ui-stale-service-gap");
    if (key == QLatin1String("credentials-locked")) return qtTrId("ui-stale-locked");
    if (key == QLatin1String("needs-attention") || key.endsWith(QLatin1String("-failed")))
        return qtTrId("ui-attn-internal");
    return qtTrId("ui-stale-other");
}

// The five trend glyphs, the only place they are written: the
// plain Unicode characters down, down-right, approximately-equal, up-right, up, each followed by the
// text variation selector U+FE0E. Without the selector some fonts draw an arrow as a coloured emoji
// picture; with it the glyph stays plain text in the colour of the line. Why plain characters and not
// the source's own picture codes: the notification body is plain text. Assumption (register): the
// phone's font draws all five; they are checked in an expanded banner.
QString NotificationTexts::trendArrow(int code)
{
    ushort glyph = 0;
    switch (code) {
    case 1: glyph = 0x2193; break;      // falling fast
    case 2: glyph = 0x2198; break;      // falling slowly
    case 3: glyph = 0x2248; break;      // steady
    case 4: glyph = 0x2197; break;      // rising slowly
    case 5: glyph = 0x2191; break;      // rising fast
    default: return QString();          // an unknown code gives no glyph
    }
    QString t = QString(QChar(glyph));
    t += QChar(0xFE0E);
    return t;
}

QString NotificationTexts::valueText(double value, const QString &unit, int trendCode)
{
    const bool mmol = unit == QLatin1String("mmol/L");
    QString t = mmol ? QString::number(value, 'f', 1) : QString::number(qRound(value));
    t += QLatin1Char(' ') + unit;
    const QString arrow = trendArrow(trendCode);
    if (!arrow.isEmpty())
        t += QLatin1Char(' ') + arrow;
    return t;
}

QString NotificationTexts::limitText(int mgDl, const QString &unit)
{
    if (unit == QLatin1String("mmol/L"))
        return QString::number(mgDl / 18.0156, 'f', 1);
    return QString::number(mgDl);
}

// The alarm texts. Line 1: the label, and with the setting the value, unit and trend after it.
// Line 2: the limit crossed and since when (or, for no data, the last reading and the cause).
// Line 3: where to acknowledge: a tap on the entry in the Events page, because a tap on the banner does not
// acknowledge. The reminder re-posts the notification, so `minutes` is current.
NoteText NotificationTexts::alarm(NoteKind kind, const AlarmTextInput &in) const
{
    NoteText t;
    switch (kind) {
    //% "VERY LOW GLUCOSE"
    case NoteKind::UrgentLow: t.summary = qtTrId("note-urgent-low"); break;
    //% "LOW GLUCOSE"
    case NoteKind::Low: t.summary = qtTrId("note-low"); break;
    //% "HIGH GLUCOSE"
    case NoteKind::High: t.summary = qtTrId("note-high"); break;
    //% "VERY HIGH GLUCOSE"
    case NoteKind::UrgentHigh: t.summary = qtTrId("note-urgent-high"); break;
    //% "NO GLUCOSE DATA"
    default: t.summary = qtTrId("note-no-data"); break;
    }

    QString line1 = in.label;
    if (in.showValue && in.haveValue)
        line1 += QString::fromUtf8(" \xC2\xB7 ") + valueText(in.value, in.unit, in.trendCode);

    QString line2;
    struct { QString unit; } u = { in.unit.isEmpty() ? QStringLiteral("mg/dL") : in.unit };
    if (kind == NoteKind::NoData) {
        //% "No reading since %1 (%n min): %2."
        line2 = qtTrId("note-line-nodata", minutesBetween(in.facts.lastReadingAt, in.now))
                    .arg(time(in.facts.lastReadingAt), causeText(in.facts.cause));
    } else {
        const int minutes = minutesBetween(in.facts.levelSince, in.now);
        const QString limit = limitText(in.facts.limit, u.unit);
        if (kind == NoteKind::UrgentLow || kind == NoteKind::Low) {
            //% "Below %1 %2 since %3 (%n min)."
            line2 = qtTrId("note-line-below", minutes).arg(limit, u.unit, time(in.facts.levelSince));
        } else {
            //% "Above %1 %2 since %3 (%n min)."
            line2 = qtTrId("note-line-above", minutes).arg(limit, u.unit, time(in.facts.levelSince));
        }
    }
    //% "In Events, tap this entry to acknowledge."
    t.body = nl(QStringList() << line1 << line2 << qtTrId("note-line-ack"));
    return t;
}

NoteText NotificationTexts::ended(NoteKind kind, const QString &label, const EffectFacts &facts) const
{
    NoteText t;
    switch (kind) {
    //% "VERY LOW GLUCOSE ENDED"
    case NoteKind::EndedUrgentLow: t.summary = qtTrId("note-ended-urgent-low"); break;
    //% "LOW GLUCOSE ENDED"
    case NoteKind::EndedLow: t.summary = qtTrId("note-ended-low"); break;
    //% "HIGH GLUCOSE ENDED"
    case NoteKind::EndedHigh: t.summary = qtTrId("note-ended-high"); break;
    //% "VERY HIGH GLUCOSE ENDED"
    default: t.summary = qtTrId("note-ended-urgent-high"); break;
    }
    //% "The alert ended at %1 after %n min."
    const QString l2 = qtTrId("note-ended-l1", minutesBetween(facts.episodeSince, facts.clearedAt)).arg(time(facts.clearedAt));
    //% "You did not acknowledge it."
    t.body = nl(QStringList() << label << l2 << qtTrId("note-ended-l2"));
    return t;
}

NoteText NotificationTexts::waitingForApproval(const QString &label) const
{
    NoteText t;
    //% "APPROVE NEW PATIENT"
    t.summary = qtTrId("note-approval");
    //% "%1 is waiting to be monitored."
    const QString l1 = qtTrId("note-approval-l1").arg(label);
    //% "No alerts until you approve in the app."
    t.body = nl(QStringList() << l1 << qtTrId("note-approval-l2"));
    return t;
}

NoteText NotificationTexts::interruption(const UtcTime &from, const UtcTime &to) const
{
    NoteText t;
    //% "GLUCOSE MONITOR WAS DOWN"
    t.summary = qtTrId("note-interruption");
    //% "No readings were collected from %1 to %2."
    const QString l1 = qtTrId("note-interruption-l1").arg(time(from), time(to));
    //% "Alerts could not fire during that time."
    t.body = nl(QStringList() << l1 << qtTrId("note-interruption-l2"));
    return t;
}

NoteText NotificationTexts::guardDisabled(const QString &connectionLabel, int attempts, const QString &causeKey) const
{
    NoteText t;
    //% "GLUCOSE SOURCE DISABLED"
    t.summary = qtTrId("note-guard");
    //% "%1 was disabled after %n failed attempts."
    const QString l1 = qtTrId("note-guard-l1", attempts).arg(connectionLabel);
    //% "Cause: %1. Re-enable it in the app."
    t.body = nl(QStringList() << l1 << qtTrId("note-guard-l2").arg(causeText(causeKey)));
    return t;
}

NoteText NotificationTexts::reportProposed() const
{
    NoteText t;
    //% "REVIEW DIAGNOSTIC REPORT"
    t.summary = qtTrId("note-report");
    //% "A diagnostic report was prepared after repeated problems."
    const QString l1 = qtTrId("note-report-l1");
    //% "Open the app to review it."
    t.body = nl(QStringList() << l1 << qtTrId("note-report-l2"));
    return t;
}

NoteText NotificationTexts::alertsWillBeSilent() const
{
    NoteText t;
    //% "GLUCOSE ALERTS SILENT"
    t.summary = qtTrId("note-silent");
    //% "Do not disturb is on: alerts will show, but without sound or vibration."
    const QString l1 = qtTrId("note-silent-l1");
    //% "Turn it off if you want to be woken."
    t.body = nl(QStringList() << l1 << qtTrId("note-silent-l2"));
    return t;
}

// ---- the resident notice ------------------------------------------------------------------------

// "<label> very low", "<label> high (acknowledged)": the label and the level word, never a value.
QString NotificationTexts::alertWord(NoteKind kind, const QString &label, bool acknowledged)
{
    QString w;
    switch (kind) {
    //% "%1 very low"
    case NoteKind::UrgentLow: w = qtTrId("note-list-urgent-low").arg(label); break;
    //% "%1 very high"
    case NoteKind::UrgentHigh: w = qtTrId("note-list-urgent-high").arg(label); break;
    //% "%1 low"
    case NoteKind::Low: w = qtTrId("note-list-low").arg(label); break;
    //% "%1 high"
    case NoteKind::High: w = qtTrId("note-list-high").arg(label); break;
    //% "%1 no data"
    default: w = qtTrId("note-list-no-data").arg(label); break;
    }
    //% "%1 (acknowledged)"
    return acknowledged ? qtTrId("note-list-acked").arg(w) : w;
}

NoteText NotificationTexts::residentActive(const ResidentInput &in) const
{
    NoteText t;
    //% "GLUCOSE MONITOR ACTIVE"
    t.summary = qtTrId("note-res-active");
    //% "Updated %1 · %n patients monitored."
    const QString l1 = qtTrId("note-res-active-l1", in.patients).arg(time(in.updated));
    QString l2;
    if (in.alerts.isEmpty()) {
        //% "No active alerts."
        l2 = qtTrId("note-res-no-alerts");
    } else {
        QStringList words;
        for (int i = 0; i < in.alerts.size(); ++i)
            words << alertWord(in.alerts.at(i).kind, in.alerts.at(i).label, in.alerts.at(i).acknowledged);
        //% "Alerts: %1."
        l2 = qtTrId("note-res-alerts").arg(words.join(QStringLiteral(", ")));
    }
    QStringList lines;
    lines << l1 << l2;
    QStringList l3;
    if (!in.silentLabels.isEmpty()) {
        //% "Silent: %1."
        l3 << qtTrId("note-res-silent").arg(in.silentLabels.join(QStringLiteral(", ")));
    }
    if (in.doNotDisturb) {
        //% "Do not disturb is on."
        l3 << qtTrId("note-res-dnd");
    }
    if (!l3.isEmpty())
        lines << l3.join(QLatin1Char(' '));
    t.body = nl(lines);
    return t;
}

NoteText NotificationTexts::residentAwaitingApproval(const ResidentInput &in) const
{
    // The same words as the notice that asks for the approval.
    return waitingForApproval(in.connectionLabel);
}

NoteText NotificationTexts::residentAttention(const ResidentInput &in) const
{
    NoteText t;
    //% "MONITOR NEEDS ATTENTION"
    t.summary = qtTrId("note-res-attention");
    //% "Monitoring cannot start: %1."
    const QString l1 = qtTrId("note-res-attention-l1").arg(causeText(in.cause));
    //% "Open the app to fix it."
    t.body = nl(QStringList() << l1 << qtTrId("note-res-attention-l2"));
    return t;
}

NoteText NotificationTexts::residentStopped(const ResidentInput &in) const
{
    NoteText t;
    //% "GLUCOSE ALERTS STOPPED"
    t.summary = qtTrId("note-res-stopped");
    //% "Monitoring was stopped at %1."
    const QString l1 = qtTrId("note-res-stopped-l1").arg(time(in.stoppedAt));
    //% "No readings are collected and no alerts will fire."
    const QString l2 = qtTrId("note-res-stopped-l2");
    //% "Open the app to start it again."
    t.body = nl(QStringList() << l1 << l2 << qtTrId("note-res-stopped-l3"));
    return t;
}

// ---- the translator ------------------------------------------------------------------------------

NotificationTranslator::~NotificationTranslator() { unload(); }

void NotificationTranslator::unload()
{
    if (m_specificInstalled)
        QCoreApplication::removeTranslator(&m_specific);
    if (m_baseInstalled)
        QCoreApplication::removeTranslator(&m_base);
    m_specificInstalled = m_baseInstalled = false;
}

QString NotificationTranslator::load(const QString &directory, const QString &baseName, const QString &language)
{
    unload();
    QString lang = language.trimmed();
    if (lang.isEmpty())
        lang = QLocale::system().name();            // "it_IT"
    // English first (the source and the fallback), then the language on top of it.
    if (m_base.load(baseName, directory)) {
        QCoreApplication::installTranslator(&m_base);
        m_baseInstalled = true;
    }
    m_language = QStringLiteral("en");
    const QString primary = lang.section(QLatin1Char('_'), 0, 0).section(QLatin1Char('-'), 0, 0).toLower();
    if (primary != QLatin1String("en") && !primary.isEmpty()) {
        if (m_specific.load(baseName + QLatin1Char('-') + lang, directory)
            || m_specific.load(baseName + QLatin1Char('-') + primary, directory)) {
            QCoreApplication::installTranslator(&m_specific);
            m_specificInstalled = true;
            m_language = primary;
        }
    }
    return m_language;
}

} // namespace lm
