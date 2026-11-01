// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Michele Brami
#include "alerts/silentbook.h"

#include <QDateTime>

#include <algorithm>

namespace lm {

SilentBook::SilentBook(const IdFactory &ids)
    : m_ids(ids ? ids : IdFactory([]() { return QUuid::createUuid(); })), m_seq(1)
{
}

UtcTime SilentBook::untilTomorrow(const UtcTime &now, const QTimeZone &zone, int hour, qint64 minAheadMs)
{
    const QDateTime local = QDateTime::fromMSecsSinceEpoch(now.msecsSinceEpoch(), zone);
    QDateTime end(local.date(), QTime(hour, 0), zone);
    if (end.toMSecsSinceEpoch() <= now.msecsSinceEpoch() + minAheadMs)
        end = QDateTime(local.date().addDays(1), QTime(hour, 0), zone);
    return UtcTime::fromMSecsSinceEpoch(end.toMSecsSinceEpoch());
}

bool SilentBook::isSilent(const QUuid &subject, const UtcTime &at) const
{
    const QHash<QUuid, State>::const_iterator it = m_state.constFind(subject);
    if (it == m_state.constEnd() || !it->on)
        return false;
    return it->untilMs == 0 || at.msecsSinceEpoch() < it->untilMs;
}

UtcTime SilentBook::until(const QUuid &subject) const
{
    const QHash<QUuid, State>::const_iterator it = m_state.constFind(subject);
    if (it == m_state.constEnd() || !it->on || it->untilMs == 0)
        return UtcTime();
    return UtcTime::fromMSecsSinceEpoch(it->untilMs);
}

bool SilentBook::isOn(const QUuid &subject) const
{
    const QHash<QUuid, State>::const_iterator it = m_state.constFind(subject);
    return it != m_state.constEnd() && it->on;
}

SilentChange SilentBook::make(const QUuid &subject, const QString &event, const UtcTime &until,
                              const UtcTime &time)
{
    SilentChange c;
    c.id = m_ids();
    c.subject = subject;
    c.event = event;
    c.until = until;
    c.time = time;
    c.seq = m_seq++;
    return c;
}

SilentChange SilentBook::start(const QUuid &subject, const UtcTime &until, const UtcTime &now)
{
    State &s = m_state[subject];
    s.on = true;
    s.untilMs = until.isValid() ? until.msecsSinceEpoch() : 0;
    return make(subject, SilentEvent::start(), until, now);
}

SilentChange SilentBook::end(const QUuid &subject, const UtcTime &now)
{
    if (!isOn(subject))
        return SilentChange();
    m_state[subject] = State();
    return make(subject, SilentEvent::end(), UtcTime(), now);
}

QVector<SilentChange> SilentBook::expire(const UtcTime &now)
{
    QVector<SilentChange> out;
    for (QHash<QUuid, State>::iterator it = m_state.begin(); it != m_state.end(); ++it) {
        if (it->on && it->untilMs != 0 && it->untilMs <= now.msecsSinceEpoch()) {
            const UtcTime endsAt = UtcTime::fromMSecsSinceEpoch(it->untilMs);
            *it = State();
            out.append(make(it.key(), SilentEvent::end(), UtcTime(), endsAt));
        }
    }
    // Time order, and the book's own sequence for equal times, so that the journal reads in the
    // order things ended whatever the hash order was.
    std::sort(out.begin(), out.end(), [](const SilentChange &a, const SilentChange &b) {
        return a.time != b.time ? a.time < b.time : a.seq < b.seq;
    });
    return out;
}

UtcTime SilentBook::nextEnd() const
{
    bool found = false;
    qint64 best = 0;
    for (QHash<QUuid, State>::const_iterator it = m_state.constBegin(); it != m_state.constEnd(); ++it)
        if (it->on && it->untilMs != 0 && (!found || it->untilMs < best)) {
            best = it->untilMs;
            found = true;
        }
    return found ? UtcTime::fromMSecsSinceEpoch(best) : UtcTime();
}

QVector<SilentChange> SilentBook::restore(const QVector<SilentChange> &lines, const UtcTime &now)
{
    m_state.clear();
    QVector<SilentChange> sorted = lines;
    std::sort(sorted.begin(), sorted.end(), [](const SilentChange &a, const SilentChange &b) {
        if (a.time != b.time) return a.time < b.time;
        if (a.seq != b.seq) return a.seq < b.seq;
        return a.id.toString() < b.id.toString();
    });
    QHash<QUuid, SilentChange> last;
    for (int i = 0; i < sorted.size(); ++i) {
        last.insert(sorted.at(i).subject, sorted.at(i));
        m_seq = std::max(m_seq, sorted.at(i).seq + 1);
    }
    QVector<SilentChange> expired;
    for (QHash<QUuid, SilentChange>::const_iterator it = last.constBegin(); it != last.constEnd(); ++it) {
        const SilentChange &c = it.value();
        if (c.event != SilentEvent::start())
            continue;                           // ended, or expired: off
        State s;
        s.on = true;
        s.untilMs = c.until.isValid() ? c.until.msecsSinceEpoch() : 0;
        if (s.untilMs != 0 && s.untilMs <= now.msecsSinceEpoch()) {
            // The end passed while the service was off: the mode ends now, and says so.
            expired.append(make(c.subject, SilentEvent::expired(), c.until, now));
            continue;
        }
        m_state.insert(c.subject, s);
    }
    return expired;
}

} // namespace lm
