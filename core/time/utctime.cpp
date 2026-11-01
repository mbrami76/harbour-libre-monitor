// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Michele Brami
#include "time/utctime.h"

#include <QDate>
#include <QDateTime>
#include <QRegularExpression>
#include <QTime>

namespace lm {

UtcTime UtcTime::fromMSecsSinceEpoch(qint64 ms)
{
    UtcTime t;
    t.m_ms = ms;
    t.m_valid = true;
    return t;
}

UtcTime UtcTime::now()
{
    return fromMSecsSinceEpoch(QDateTime::currentMSecsSinceEpoch());
}

UtcTime UtcTime::fromIso(const QString &text)
{
    static const QRegularExpression re(QStringLiteral(
        "^(\\d{4})-(\\d{2})-(\\d{2})T(\\d{2}):(\\d{2}):(\\d{2})(?:\\.(\\d{1,3}))?Z\\z"));
    const QRegularExpressionMatch m = re.match(text);
    if (!m.hasMatch())
        return UtcTime();

    const QDate date(m.captured(1).toInt(), m.captured(2).toInt(), m.captured(3).toInt());
    const int msec = m.captured(7).leftJustified(3, QLatin1Char('0')).toInt();
    const QTime time(m.captured(4).toInt(), m.captured(5).toInt(), m.captured(6).toInt(), msec);
    if (!date.isValid() || !time.isValid())
        return UtcTime();

    return fromMSecsSinceEpoch(QDateTime(date, time, Qt::UTC).toMSecsSinceEpoch());
}

QString UtcTime::toIso() const
{
    if (!m_valid)
        return QString();
    const QDateTime dt = QDateTime::fromMSecsSinceEpoch(m_ms, Qt::UTC);
    if (!dt.isValid() || dt.date().year() < 1 || dt.date().year() > 9999)
        return QString();
    return dt.toString(QStringLiteral("yyyy-MM-dd'T'HH:mm:ss.zzz'Z'"));
}

bool UtcTime::isNotAfter(const UtcTime &now, qint64 toleranceMs) const
{
    if (!m_valid || !now.m_valid)
        return false;
    return m_ms <= now.m_ms + toleranceMs;
}

} // namespace lm
