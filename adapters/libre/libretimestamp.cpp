// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Michele Brami
#include "libretimestamp.h"

#include <QDate>
#include <QDateTime>
#include <QTime>

namespace lm {

namespace {

// Reads between minDigits and maxDigits ASCII digits at pos. Not QChar::isDigit: that
// accepts digits of other scripts, and the format is ASCII only.
bool readNumber(const QString &s, int &pos, int minDigits, int maxDigits, int *out)
{
    const int start = pos;
    int value = 0;
    while (pos < s.size() && pos - start < maxDigits) {
        const ushort c = s.at(pos).unicode();
        if (c < '0' || c > '9')
            break;
        value = value * 10 + (c - '0');
        ++pos;
    }
    if (pos - start < minDigits)
        return false;
    *out = value;
    return true;
}

bool expectChar(const QString &s, int &pos, ushort c)
{
    if (pos >= s.size() || s.at(pos).unicode() != c)
        return false;
    ++pos;
    return true;
}

} // namespace

bool parseLibreTimestamp(const QString &text, UtcTime *out)
{
    int pos = 0;
    int month = 0, day = 0, year = 0, hour = 0, minute = 0, second = 0;

    if (!readNumber(text, pos, 1, 2, &month) || !expectChar(text, pos, '/')
        || !readNumber(text, pos, 1, 2, &day) || !expectChar(text, pos, '/')
        || !readNumber(text, pos, 4, 4, &year) || !expectChar(text, pos, ' ')
        || !readNumber(text, pos, 1, 2, &hour) || !expectChar(text, pos, ':')
        || !readNumber(text, pos, 2, 2, &minute) || !expectChar(text, pos, ':')
        || !readNumber(text, pos, 2, 2, &second) || !expectChar(text, pos, ' '))
        return false;

    if (text.size() - pos != 2 || text.at(pos + 1).unicode() != 'M')
        return false;
    const ushort marker = text.at(pos).unicode();
    if (marker != 'A' && marker != 'P')
        return false;
    const bool pm = marker == 'P';

    if (hour < 1 || hour > 12 || minute > 59 || second > 59)
        return false;
    const QDate date(year, month, day);   // isValid() checks month, day and leap years
    if (!date.isValid())
        return false;

    const int hour24 = (hour % 12) + (pm ? 12 : 0);
    const QDateTime dt(date, QTime(hour24, minute, second), Qt::UTC);
    if (!dt.isValid())
        return false;
    *out = UtcTime::fromMSecsSinceEpoch(dt.toMSecsSinceEpoch());
    return true;
}

} // namespace lm
