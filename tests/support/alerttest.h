// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Michele Brami
#ifndef LM_ALERTTEST_H
#define LM_ALERTTEST_H

// Test support for the alert engine: a scripted rig around AlertEngine with a manual
// clock and repeatable ids, and a tiny script language so that a test row reads as "what happened"
// and "what the engine answered", the table-driven style
//
// Time in a script is SECONDS after a fixed start (the engine itself works in milliseconds, so a
// line may show 180.001 where a limit is "older than" 180 s). Every effect the engine returns is
// turned into one text line, so a row compares a list of strings:
//
//   J <t> <slot> <ALERT> <state> <event>        a journal entry (the transition)
//   N <t> <slot> <ALERT>[ cause=<c>]            a new notification (sound)
//   R <t> <slot> <ALERT> #<n>                   a repeat of an unanswered one
//   U <t> <slot> <ALERT> <state>[ cause=<c>]    replaced in place, silently
//   W <t> <slot> <ALERT>                        taken down
//   I <from> <to>                               the interruption notice
//   A <t> ack|snz <slot> <result>               a refused user action (a done one shows its effects)
//
// Script lines (one per line, '#' starts a comment):
//   r <t> <mg/dL>                    a reading at time t (measured and arriving at t)
//   rr <from> <to> <step> <mg/dL>    the same reading every <step> seconds
//   rm <t> <mmol/L>                  a reading stored in mmol/L
//   t <t>                            a tick
//   ack <t> low|high|missing         the user acknowledges
//   snz <t> low|high|missing <min>   the user snoozes
//   st <t> <cause|-> <lastReading|-> the scheduler's staleness information
//   th <t> <l> <h>                   the Subject's L and H thresholds change (mg/dL, no checks)
//   cfg <t> <pl> <ph> <ml>           persistence L, persistence H, missing limit, in minutes
//   gap <t> <from> <to>              the service started after a gap
//   silent <t> <minutes>             silent mode on for that long (0 = without limit)
//   unsilent <t>                     silent mode off at the user's request
// A notification line ends with " silent" when the effect carries the silent flag.

#include "alerts/alertengine.h"
#include "alerts/silentbook.h"

#include <QStringList>
#include <QtTest>

namespace lm {
namespace testing {

const qint64 kAlertTestStartMs = 1768000000000LL;       // 2026-01-09, as the other manual clocks

inline UtcTime alertTime(double seconds)
{
    return UtcTime::fromMSecsSinceEpoch(kAlertTestStartMs + qint64(seconds * 1000.0 + (seconds < 0 ? -0.5 : 0.5)));
}

inline QUuid alertTestSubject(int n = 1)
{
    static const QUuid ns("{6ba7b810-9dad-11d1-80b4-00c04fd430c8}");
    return QUuid::createUuidV5(ns, QStringLiteral("alert-subject-%1").arg(n));
}

class AlertRig
{
public:
    explicit AlertRig(const AlertSettings &settings = AlertSettings())
        : m_ids(0), m_engine(settings, makeIds(&m_ids)), m_subject(alertTestSubject())
    {
        m_engine.setSilentBook(&m_silentBook);
        m_engine.setSubject(m_subject, m_cfg, alertTime(0));
    }

    SilentBook &silentBook() { return m_silentBook; }
    // Silent mode on from `t` for `minutes` (0 = without limit), as the coordinator does it: the
    // book first, then the one Update per active unacknowledged alert if the mode was not on.
    void setSilent(double t, int minutes)
    {
        const UtcTime now = alertTime(t);
        const bool was = m_silentBook.isSilent(m_subject, now);
        m_silentBook.start(m_subject, minutes > 0 ? alertTime(t + minutes * 60.0) : UtcTime(), now);
        if (!was)
            log(m_engine.onSilentStarted(m_subject, now));
    }
    void endSilent(double t) { m_silentBook.end(m_subject, alertTime(t)); }

    AlertEngine &engine() { return m_engine; }
    const QUuid &subject() const { return m_subject; }
    SubjectAlertConfig &config() { return m_cfg; }
    const QVector<AlertRecord> &records() const { return m_records; }
    // Every effect returned so far (journal effects included), in order, for tests that look at the
    // facts an effect carries and not only at the text line.
    const AlertEffects &effects() const { return m_effects; }
    // The last effect of a kind for a slot, or an effect with an invalid time when there is none.
    AlertEffect lastEffect(AlertEffect::Kind kind, AlertSlot slot) const
    {
        for (int i = m_effects.size() - 1; i >= 0; --i)
            if (m_effects.at(i).kind == kind && m_effects.at(i).slot == slot)
                return m_effects.at(i);
        return AlertEffect();
    }

    // ---- inputs, each one logs what the engine answered ----------------------------------------
    void reading(double t, double value, const QString &unit = QStringLiteral("mg/dL"))
    {
        log(m_engine.onReading(m_subject, value, unit, alertTime(t), alertTime(t)));
    }
    void tick(double t) { log(m_engine.tick(alertTime(t))); }
    void staleness(double t, const QString &cause, double lastReading = -1e9)
    {
        StalenessInfo info;
        info.cause = cause;
        if (lastReading > -1e8)
            info.lastReadingAt = alertTime(lastReading);
        log(m_engine.onStaleness(m_subject, info, alertTime(t)));
    }
    void configure(double t, const SubjectAlertConfig &c)
    {
        m_cfg = c;
        log(m_engine.setSubject(m_subject, c, alertTime(t)));
    }
    void acknowledge(double t, AlertSlot slot)
    {
        const ActionOutcome o = m_engine.acknowledge(m_subject, slot, alertTime(t));
        log(o.effects);
        action(t, "ack", slot, o.result);
    }
    void snooze(double t, AlertSlot slot, int minutes)
    {
        const ActionOutcome o = m_engine.snooze(m_subject, slot, minutes, alertTime(t));
        log(o.effects);
        action(t, "snz", slot, o.result);
    }
    void serviceStart(double t, double from, double to)
    {
        log(m_engine.onServiceStart(alertTime(from), alertTime(to), alertTime(t)));
    }

    // ---- results -----------------------------------------------------------------------------
    // Returns the lines logged since the last call, optionally only those whose first letter is in
    // `kinds` (for example "JN" leaves out reminders and updates), and clears the log.
    QStringList take(const QString &kinds = QString())
    {
        QStringList out;
        for (int i = 0; i < m_log.size(); ++i)
            if (kinds.isEmpty() || kinds.contains(m_log.at(i).at(0)))
                out.append(m_log.at(i));
        m_log.clear();
        return out;
    }

    AlertView view(AlertSlot slot) const { return m_engine.view(m_subject, slot); }

    // "low:LL:active", or "-" for a normal slot: the compact form tables compare.
    QString shape(AlertSlot slot) const
    {
        const AlertView v = view(slot);
        if (v.state == AlertState::Normal)
            return QStringLiteral("-");
        return alertSlotName(slot) + QLatin1Char(':') + alertTypeName(v.type) + QLatin1Char(':')
            + alertStateName(v.state);
    }

    // Runs a script (see the top of this file) and returns the log.
    QStringList run(const QString &script, const QString &kinds = QString())
    {
        const QStringList lines = script.split(QLatin1Char('\n'));
        for (int i = 0; i < lines.size(); ++i) {
            QString line = lines.at(i);
            const int hash = line.indexOf(QLatin1Char('#'));
            if (hash >= 0) line.truncate(hash);
            const QStringList w = line.simplified().split(QLatin1Char(' '), QString::SkipEmptyParts);
            if (w.isEmpty()) continue;
            step(w);
        }
        return take(kinds);
    }

    static QString fmt(const UtcTime &t)
    {
        const qint64 ms = t.msecsSinceEpoch() - kAlertTestStartMs;
        if (ms % 1000 == 0)
            return QString::number(ms / 1000);
        QString s = QString::number(double(ms) / 1000.0, 'f', 3);
        while (s.endsWith(QLatin1Char('0'))) s.chop(1);
        return s;
    }

private:
    static IdFactory makeIds(int *counter)
    {
        // Repeatable ids: the n-th id asked for is always the same UUID.
        return [counter]() {
            static const QUuid ns("{6ba7b810-9dad-11d1-80b4-00c04fd430c8}");
            return QUuid::createUuidV5(ns, QStringLiteral("alert-id-%1").arg(++*counter));
        };
    }

    static AlertSlot slotNamed(const QString &s)
    {
        AlertSlot slot = AlertSlot::Low;
        const bool ok = alertSlotFromName(s, &slot);
        Q_ASSERT(ok);
        Q_UNUSED(ok);
        return slot;
    }

    void action(double t, const char *what, AlertSlot slot, ActionResult r)
    {
        if (r != ActionResult::Done)
            m_log.append(QStringLiteral("A %1 %2 %3 %4").arg(fmtSeconds(t), QLatin1String(what),
                                                            alertSlotName(slot), actionResultName(r)));
    }
    static QString fmtSeconds(double t) { return fmt(alertTime(t)); }

    void step(const QStringList &w)
    {
        const QString &c = w.at(0);
        if (c == QLatin1String("r")) reading(w.at(1).toDouble(), w.at(2).toDouble());
        else if (c == QLatin1String("rm")) reading(w.at(1).toDouble(), w.at(2).toDouble(), QStringLiteral("mmol/L"));
        else if (c == QLatin1String("rr")) {
            const double to = w.at(2).toDouble(), stepS = w.at(3).toDouble();
            for (double t = w.at(1).toDouble(); t <= to + 1e-9; t += stepS)
                reading(t, w.at(4).toDouble());
        }
        else if (c == QLatin1String("t")) tick(w.at(1).toDouble());
        else if (c == QLatin1String("ack")) acknowledge(w.at(1).toDouble(), slotNamed(w.at(2)));
        else if (c == QLatin1String("snz")) snooze(w.at(1).toDouble(), slotNamed(w.at(2)), w.at(3).toInt());
        else if (c == QLatin1String("st"))
            staleness(w.at(1).toDouble(), w.at(2) == QLatin1String("-") ? QString() : w.at(2),
                      w.at(3) == QLatin1String("-") ? -1e9 : w.at(3).toDouble());
        else if (c == QLatin1String("th")) {
            SubjectAlertConfig n = m_cfg;
            n.thresholds.l = w.at(2).toInt();
            n.thresholds.h = w.at(3).toInt();
            configure(w.at(1).toDouble(), n);
        }
        else if (c == QLatin1String("cfg")) {
            SubjectAlertConfig n = m_cfg;
            n.persistenceLowMs = qint64(w.at(2).toDouble() * kAlertMinuteMs);
            n.persistenceHighMs = qint64(w.at(3).toDouble() * kAlertMinuteMs);
            n.missingLimitMs = qint64(w.at(4).toDouble() * kAlertMinuteMs);
            configure(w.at(1).toDouble(), n);
        }
        else if (c == QLatin1String("silent")) setSilent(w.at(1).toDouble(), w.at(2).toInt());
        else if (c == QLatin1String("unsilent")) endSilent(w.at(1).toDouble());
        else if (c == QLatin1String("gap")) serviceStart(w.at(1).toDouble(), w.at(2).toDouble(), w.at(3).toDouble());
        else QFAIL(qPrintable(QStringLiteral("unknown script command: ") + c));
    }

    void log(const AlertEffects &effects)
    {
        for (int i = 0; i < effects.size(); ++i) {
            const AlertEffect &e = effects.at(i);
            m_effects.append(e);
            const QString at = fmt(e.at);
            const QString who = alertSlotName(e.slot) + QLatin1Char(' ') + alertTypeName(e.type);
            const QString cause = e.cause.isEmpty() ? QString() : QStringLiteral(" cause=") + e.cause;
            switch (e.kind) {
            case AlertEffect::Journal:
                m_records.append(e.record);
                m_log.append(QStringLiteral("J %1 %2 %3 %4%5").arg(at, who, alertStateName(e.record.state),
                                 e.record.event, e.record.cause.isEmpty() ? QString()
                                                  : QStringLiteral(" reason=") + e.record.cause));
                break;
            case AlertEffect::Notify:
                m_log.append(QStringLiteral("N %1 %2%3%4").arg(at, who, cause, e.silent ? " silent" : ""));
                break;
            case AlertEffect::Remind:
                m_log.append(QStringLiteral("R %1 %2 #%3%4").arg(at, who).arg(e.reminderNumber).arg(e.silent ? " silent" : ""));
                break;
            case AlertEffect::Update:
                m_log.append(QStringLiteral("U %1 %2 %3%4%5").arg(at, who, alertStateName(e.state), cause, e.silent ? " silent" : ""));
                break;
            case AlertEffect::Withdraw:
                m_log.append(QStringLiteral("W %1 %2").arg(at, who));
                break;
            case AlertEffect::Interruption:
                m_log.append(QStringLiteral("I %1 %2").arg(fmt(e.gapFrom), fmt(e.gapTo)));
                break;
            }
        }
    }

    int m_ids;
    SilentBook m_silentBook;
    AlertEngine m_engine;
    QUuid m_subject;
    SubjectAlertConfig m_cfg;
    QStringList m_log;
    QVector<AlertRecord> m_records;
    AlertEffects m_effects;
};

} // namespace testing
} // namespace lm

#endif
