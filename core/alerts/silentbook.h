// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Michele Brami
#ifndef LM_SILENTBOOK_H
#define LM_SILENTBOOK_H

// Silent mode (REQ-ALR-09): a per-Subject mode that withholds the
// sound and the vibration of the alerts and nothing else. The alert machine is not touched: states,
// reminders, acknowledgement and the journal run as always, the notification is still posted and
// replaced in place; only the feedback is withheld. It covers every glucose level, LL and HH
// included (the author's decision), and it never covers missing data, the interruption notice or the
// notices about the monitor itself.
//
// WHAT THIS CLASS IS. A small ledger, the same pattern as ThresholdBook: per Subject, whether the
// mode is on and its ABSOLUTE end time (a timed mode ends by itself, a mode without limit has none).
// It is pure: no clock of its own, no I/O. Every change returns a SilentChange, the line that the
// caller writes to the journal (kind `silent`), and the book is folded back from those lines at
// start. isSilent() answers from the end time alone, so the answer is right at any logical moment,
// whether or not a tick has "processed" the end yet; expire() only produces the journal line.
//
// DURATIONS. 1 hour and 4 hours are minutes; "without limit" is no end time; "until tomorrow" is a
// local-time boundary fixed here: the next 06:00 local time, or the one after if the next is less
// than an hour away. The boundary is computed by untilTomorrow() from a time zone that the
// caller passes, so a test needs no system zone.

#include "alerts/thresholdbook.h"
#include "time/utctime.h"

#include <QHash>
#include <QString>
#include <QTimeZone>
#include <QUuid>
#include <QVector>

namespace lm {

// `minutes` for "until tomorrow" in AlertCoordinator::setSilent (a negative number cannot be a
// length, so it is free to mean this).
const int kSilentUntilTomorrow = -1;

namespace SilentEvent {
inline QString start() { return QStringLiteral("start"); }
inline QString end() { return QStringLiteral("end"); }          // a timed mode ran out, or the user ended it
inline QString expired() { return QStringLiteral("expired"); }  // it ran out while the service was off, found at start
}

// One line of the journal kind `silent`. `until` is invalid for a mode without limit and for an end
// the user asked for.
struct SilentChange
{
    SilentChange() : seq(0) {}

    QUuid id;
    QUuid subject;
    QString event;
    UtcTime until;
    UtcTime time;           // when the line was made; for a timed `end` the end time itself
    qint64 seq;             // orders lines with the same wall time
};

class SilentBook
{
public:
    explicit SilentBook(const IdFactory &ids = IdFactory());

    // The next local `hour`:00 that is more than `minAheadMs` away from `now` (see the top).
    static UtcTime untilTomorrow(const UtcTime &now, const QTimeZone &zone, int hour, qint64 minAheadMs);

    // Is the Subject in silent mode at this moment? Silent, and either no end or `at` before it.
    bool isSilent(const QUuid &subject, const UtcTime &at) const;
    // The end time of a timed mode; invalid when the mode has no limit or is not on.
    UtcTime until(const QUuid &subject) const;
    // On, whatever the end (even a timed one that has passed and not been expired yet).
    bool isOn(const QUuid &subject) const;

    // Turns the mode on (replacing an earlier one): `until` invalid = without limit.
    SilentChange start(const QUuid &subject, const UtcTime &until, const UtcTime &now);
    // Turns it off at the user's request. A Subject that is not on gives an invalid change (empty
    // event).
    SilentChange end(const QUuid &subject, const UtcTime &now);
    // Ends every timed mode whose end time is not after `now`; one `end` change each, stamped with
    // the end time, in time order.
    QVector<SilentChange> expire(const UtcTime &now);
    // The earliest end time of a mode that is on, or an invalid time.
    UtcTime nextEnd() const;

    // Rebuilds the ledger from journal lines in any order: the last line per Subject wins (by time,
    // then seq, then id). A timed mode whose end passed while the service was off ends here: its
    // `expired` change is returned for the caller to journal (stamped `now`).
    QVector<SilentChange> restore(const QVector<SilentChange> &lines, const UtcTime &now);

private:
    struct State
    {
        State() : on(false), untilMs(0) {}
        bool on;
        qint64 untilMs;     // 0 = without limit
    };

    SilentChange make(const QUuid &subject, const QString &event, const UtcTime &until, const UtcTime &time);

    QHash<QUuid, State> m_state;
    IdFactory m_ids;
    qint64 m_seq;
};

} // namespace lm

#endif
