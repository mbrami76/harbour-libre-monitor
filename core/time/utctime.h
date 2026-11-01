// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Michele Brami
#ifndef LM_UTCTIME_H
#define LM_UTCTIME_H

#include <QString>
#include <QtGlobal>

namespace lm {

// Default clock-skew tolerance for "not in the future" (REQ-ADM-02).
// PROVISIONAL: 5 minutes, proposed (phone clock versus source clock).
const qint64 kDefaultClockSkewToleranceMs = 5 * 60 * 1000;

// A point in time in UTC with millisecond resolution (REQ-DAT-02). Default-constructed
// values are invalid. Built on qint64 so it works at the Qt 5.6 API level.
class UtcTime
{
public:
    UtcTime() : m_ms(0), m_valid(false) {}

    static UtcTime fromMSecsSinceEpoch(qint64 ms);
    static UtcTime now();

    // Strict ISO 8601 UTC: YYYY-MM-DDTHH:MM:SS[.f{1,3}]Z. Anything else (offsets, lower
    // case, more digits, impossible dates) gives an invalid value; nothing is guessed.
    static UtcTime fromIso(const QString &text);

    bool isValid() const { return m_valid; }
    qint64 msecsSinceEpoch() const { return m_ms; }

    // YYYY-MM-DDTHH:MM:SS.mmmZ; empty when invalid or the year is outside 1..9999.
    QString toIso() const;

    // True when this time is not later than now + toleranceMs. False if either is invalid.
    bool isNotAfter(const UtcTime &now, qint64 toleranceMs = kDefaultClockSkewToleranceMs) const;

    bool operator==(const UtcTime &o) const { return m_valid == o.m_valid && m_ms == o.m_ms; }
    bool operator!=(const UtcTime &o) const { return !(*this == o); }
    bool operator<(const UtcTime &o) const
    { return m_valid != o.m_valid ? !m_valid : m_ms < o.m_ms; }

private:
    qint64 m_ms;
    bool m_valid;
};

} // namespace lm

#endif
