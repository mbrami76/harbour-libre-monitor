// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Michele Brami
#include "alerts/alertcoordinator.h"

#include "alerts/alertjournal.h"
#include "store/store.h"

namespace lm {

bool StoreAlertReadings::newestCurrent(const QUuid &subject, const QString &source, double *value,
                                       QString *unit, UtcTime *measuredAt)
{
    Record r;
    // `effective`: readings the user marked as ignored do not count (REQ-DAT-07).
    if (!m_store->latest(subject, QStringLiteral("glucose"), source, true, &r))
        return false;
    *value = r.value;
    *unit = r.unit;
    *measuredAt = r.measuredAt;
    return true;
}

AlertCoordinator::AlertCoordinator(Journal *journal, AlertReadingSource *readings,
                                   AlertNotifier *notifier, Clock *clock,
                                   const AlertSettings &settings)
    : m_journal(journal), m_readings(readings), m_notifier(notifier), m_clock(clock),
      m_engine(settings), m_settings(settings), m_started(false)
{
    m_engine.setSilentBook(&m_silent);
}

void AlertCoordinator::addSubject(const QUuid &subject, const AlertSubjectSettings &settings)
{
    if (!m_subjects.contains(subject))
        m_order.append(subject);
    SubjectEntry e;
    e.settings = settings;
    e.hasInfo = false;
    m_subjects.insert(subject, e);
}

void AlertCoordinator::updateSubjectSettings(const QUuid &subject, const AlertSubjectSettings &settings)
{
    QHash<QUuid, SubjectEntry>::iterator it = m_subjects.find(subject);
    if (it == m_subjects.end())
        return;
    it->settings = settings;
    if (m_started)
        carryOut(m_engine.setSubject(subject, configFor(subject, it->settings), m_clock->now()));
}

SubjectAlertConfig AlertCoordinator::configFor(const QUuid &subject, const AlertSubjectSettings &s) const
{
    SubjectAlertConfig c;
    c.thresholds = m_book.thresholdsFor(subject);
    c.thresholdsId = m_book.idInForce(subject);
    c.persistenceLowMs = s.persistenceLowMs;
    c.persistenceHighMs = s.persistenceHighMs;
    c.missingLimitMs = s.missingLimitMs;
    return c;
}

// The order is: thresholds first, then the Subjects, then the
// journal's alert lines, then the gap, then the first staleness information. The reading is fed
// last of all, because a restored alert must exist before a reading can clear or escalate it.
void AlertCoordinator::start()
{
    if (m_started)
        return;
    m_started = true;
    const UtcTime now = m_clock->now();
    m_book.restore(loadThresholdChanges(m_journal));
    // Silent mode first, so that the alerts notified again below carry the right flag. A timed mode
    // that ran out while the service was off ends now and says so in the journal.
    const QVector<SilentChange> expired = m_silent.restore(loadSilentChanges(m_journal), now);
    for (int i = 0; i < expired.size(); ++i)
        writeEntry(toJournalEntry(expired.at(i)));
    AlertEffects fx;
    for (int i = 0; i < m_order.size(); ++i)
        fx += m_engine.setSubject(m_order.at(i), configFor(m_order.at(i), m_subjects[m_order.at(i)].settings), now);
    fx += m_engine.restore(loadAlertRecords(m_journal), now);
    if (m_gapFrom.isValid())
        fx += m_engine.onServiceStart(m_gapFrom, m_gapTo, now);
    for (int i = 0; i < m_order.size(); ++i) {
        const SubjectEntry &e = m_subjects[m_order.at(i)];
        if (e.hasInfo)
            fx += m_engine.onStaleness(m_order.at(i), e.info, now);
    }
    carryOut(fx);
    for (int i = 0; i < m_order.size(); ++i)
        feedReading(m_order.at(i));
}

void AlertCoordinator::tick()
{
    if (!m_started)
        return;
    // Lines that could not be written earlier first, so the journal keeps its order as far as
    // possible.
    if (!m_unwritten.isEmpty()) {
        QVector<JournalEntry> still;
        for (int i = 0; i < m_unwritten.size(); ++i)
            if (!m_journal->append(m_unwritten.at(i)))
                still.append(m_unwritten.at(i));
        m_unwritten = still;
    }
    const UtcTime now = m_clock->now();
    // A timed silent mode ends by itself: only the journal line is made; nothing is emitted, the
    // next reminder simply sounds.
    const QVector<SilentChange> ended = m_silent.expire(now);
    for (int i = 0; i < ended.size(); ++i)
        writeEntry(toJournalEntry(ended.at(i)));
    carryOut(m_engine.tick(now));
}

UtcTime AlertCoordinator::nextWakeAt() const
{
    if (!m_started)
        return UtcTime();
    const UtcTime a = m_engine.nextWakeup();
    const UtcTime b = m_silent.nextEnd();
    if (!a.isValid()) return b;
    if (!b.isValid()) return a;
    return b < a ? b : a;
}

void AlertCoordinator::feedReading(const QUuid &subject)
{
    QHash<QUuid, SubjectEntry>::const_iterator it = m_subjects.constFind(subject);
    if (it == m_subjects.constEnd() || it->settings.currentSource.isEmpty())
        return;
    double value;
    QString unit;
    UtcTime measuredAt;
    if (!m_readings->newestCurrent(subject, it->settings.currentSource, &value, &unit, &measuredAt))
        return;
    carryOut(m_engine.onReading(subject, value, unit, measuredAt, m_clock->now()));
}

void AlertCoordinator::feedStaleness(const QUuid &subject, const SubjectStatus &status)
{
    QHash<QUuid, SubjectEntry>::iterator it = m_subjects.find(subject);
    if (it == m_subjects.end())
        return;
    StalenessInfo info;
    info.lastReadingAt = status.lastSuccessAt;
    info.staleLimitMs = it->settings.currentIntervalMs * it->settings.staleFactor;
    info.cause = status.stale ? status.staleCause : QString();
    it->info = info;
    it->hasInfo = true;
    if (m_started)
        carryOut(m_engine.onStaleness(subject, info, m_clock->now()));
}

void AlertCoordinator::dataChanged(const QUuid &subject)
{
    if (m_started)
        feedReading(subject);
}

void AlertCoordinator::stalenessChanged(const SubjectStatus &status) { feedStaleness(status.subject, status); }
void AlertCoordinator::subjectStateChanged(const SubjectStatus &status) { feedStaleness(status.subject, status); }

void AlertCoordinator::gapDetected(const UtcTime &from, const UtcTime &to)
{
    m_gapFrom = from;
    m_gapTo = to;
    if (m_started)
        carryOut(m_engine.onServiceStart(from, to, m_clock->now()));
}

ActionResult AlertCoordinator::acknowledge(const QUuid &subject, AlertSlot slot)
{
    const ActionOutcome o = m_engine.acknowledge(subject, slot, m_clock->now());
    carryOut(o.effects);
    return o.result;
}

ActionResult AlertCoordinator::snooze(const QUuid &subject, AlertSlot slot, int minutes)
{
    const ActionOutcome o = m_engine.snooze(subject, slot, minutes, m_clock->now());
    carryOut(o.effects);
    return o.result;
}

// The order that keeps the history true: write first, apply after. A threshold that
// could not be written is not applied; the caller is told (`NotSaved`) and the user can try again.
EditResult AlertCoordinator::setThresholds(const QUuid &subject, int l, int h,
                                           const Confirmation &confirmation, EditOutcome *details)
{
    EditOutcome out;
    QHash<QUuid, SubjectEntry>::const_iterator it = m_subjects.constFind(subject);
    if (it == m_subjects.constEnd()) {
        out.result = EditResult::UnknownSubject;
    } else {
        const UtcTime now = m_clock->now();
        out = m_book.check(subject, l, h, confirmation, now);
        if (out.result == EditResult::Accepted) {
            if (!writeThresholdChanges(m_journal, out.changes)) {
                out.result = EditResult::NotSaved;      // the journal is not writable: nothing changed
                out.changes.clear();
            } else {
                m_book.apply(out.changes);
                carryOut(m_engine.setSubject(subject, configFor(subject, it->settings), now));
            }
        }
    }
    if (details)
        *details = out;
    return out.result;
}

// One journal line, in order: if something is already waiting for a retry, this one queues behind it.
void AlertCoordinator::writeEntry(const JournalEntry &e)
{
    if (!m_unwritten.isEmpty() || !m_journal->append(e))
        m_unwritten.append(e);
}

void AlertCoordinator::writeLines(const AlertEffects &effects)
{
    for (int i = 0; i < effects.size(); ++i)
        if (effects.at(i).kind == AlertEffect::Journal)
            writeEntry(toJournalEntry(effects.at(i).record));
}

bool AlertCoordinator::setSilent(const QUuid &subject, int minutes)
{
    if (!m_subjects.contains(subject) || minutes < kSilentUntilTomorrow || minutes > 366 * 24 * 60)
        return false;
    const UtcTime now = m_clock->now();
    UtcTime until;                                  // invalid = without limit
    if (minutes == kSilentUntilTomorrow)
        until = SilentBook::untilTomorrow(now, QTimeZone::systemTimeZone(), m_settings.silentTomorrowHour,
                                          m_settings.silentTomorrowMinAheadMs);
    else if (minutes > 0)
        until = UtcTime::fromMSecsSinceEpoch(now.msecsSinceEpoch() + qint64(minutes) * kAlertMinuteMs);
    const bool wasSilent = m_silent.isSilent(subject, now);
    writeEntry(toJournalEntry(m_silent.start(subject, until, now)));
    if (!wasSilent && m_started)
        carryOut(m_engine.onSilentStarted(subject, now));
    return true;
}

bool AlertCoordinator::endSilent(const QUuid &subject)
{
    const SilentChange c = m_silent.end(subject, m_clock->now());
    if (c.event.isEmpty())
        return false;
    writeEntry(toJournalEntry(c));
    return true;
}

bool AlertCoordinator::isSilent(const QUuid &subject) const
{
    return m_silent.isSilent(subject, m_clock->now());
}

void AlertCoordinator::carryOut(const AlertEffects &effects)
{
    writeLines(effects);
    if (!m_notifier)
        return;
    for (int i = 0; i < effects.size(); ++i)
        if (effects.at(i).kind != AlertEffect::Journal)
            m_notifier->deliver(effects.at(i));
}

} // namespace lm
