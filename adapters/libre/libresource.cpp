// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Michele Brami
#include "libresource.h"

#include "libretimestamp.h"
#include "libreunit.h"
#include "record/registry.h"

#include <QCryptographicHash>
#include <QtGlobal>

#include <cmath>

namespace lm {

namespace {

QString k(const char *s) { return QString::fromLatin1(s); }

bool asInteger(const QJsonValue &v, qint64 *out)
{
    if (!v.isDouble())
        return false;
    const double d = v.toDouble();
    if (std::floor(d) != d || qAbs(d) > 9007199254740992.0)   // 2^53, exact in a double
        return false;
    *out = qint64(d);
    return true;
}

QJsonValue integerOrNull(const QJsonValue &v)
{
    qint64 n = 0;
    return asInteger(v, &n) ? QJsonValue(v.toDouble()) : QJsonValue(QJsonValue::Null);
}

// FactoryTimestamp as UTC; the string as received goes to *original.
bool readTime(const QJsonObject &o, UtcTime *t, QString *original)
{
    const QJsonValue ts = o.value(k("FactoryTimestamp"));
    if (!ts.isString())
        return false;
    *original = ts.toString();
    return parseLibreTimestamp(*original, t);
}

bool fail(SkipReason *reason, SkipReason r)
{
    *reason = r;
    return false;
}

} // namespace

void SkipCounts::add(SkipReason reason)
{
    ++examined;
    switch (reason) {
    case SkipReason::BadTimestamp: ++badTimestamp; break;
    case SkipReason::UndeterminedUnit: ++undeterminedUnit; break;
    case SkipReason::NoValue: ++noValue; break;
    case SkipReason::Malformed: ++malformed; break;
    case SkipReason::None: break;
    }
}

bool shouldEscalate(const SkipCounts &c)
{
    const int f = c.format();
    if (f == 0)
        return false;
    const int usable = c.examined - f - c.noValue;
    if (usable == 0 && c.noValue == 0)
        return true;
    return f >= 3 && f * 2 >= c.examined;
}

QString libreSourceTag(const QString &feed)
{
    return QStringLiteral("libre.") + feed;
}

bool buildGlucoseRecord(const QJsonValue &reading, const QString &subjectRef, const QString &feed,
                        QJsonObject *record, UtcTime *measuredAt, SkipReason *reason)
{
    *reason = SkipReason::None;
    if (!reading.isObject())
        return fail(reason, SkipReason::Malformed);
    const QJsonObject o = reading.toObject();

    const QJsonValue value = o.value(k("Value"));
    if (value.isNull() && o.contains(k("Value")))
        return fail(reason, SkipReason::NoValue);
    if (!value.isDouble())
        return fail(reason, SkipReason::Malformed);   // key absent or of another type

    UtcTime t;
    QString original;
    if (!readTime(o, &t, &original))
        return fail(reason, SkipReason::BadTimestamp);

    const UnitDetermination unit = determineGlucoseUnit(
        o.value(k("GlucoseUnits")), value, o.value(k("ValueInMgPerDl")));
    if (!unit.ok())
        return fail(reason, SkipReason::UndeterminedUnit);

    QJsonObject r;
    r.insert(k("shape"), k("sample"));
    r.insert(k("subject_ref"), subjectRef);
    r.insert(k("metric"), metricGlucose());
    r.insert(k("value"), value);                 // exactly as received, never converted
    r.insert(k("unit"), unit.unit);
    r.insert(k("measured_at"), t.toIso());
    r.insert(k("original_timestamp"), original);
    r.insert(k("source"), libreSourceTag(feed));
    *record = r;
    *measuredAt = t;
    return true;
}

QJsonValue trendOfReading(const QJsonValue &reading)
{
    if (!reading.isObject())
        return QJsonValue(QJsonValue::Null);
    return integerOrNull(reading.toObject().value(k("TrendArrow")));
}

bool buildLogbookRecord(const QJsonValue &entry, const QString &subjectRef,
                        QJsonObject *record, UtcTime *measuredAt, SkipReason *reason)
{
    *reason = SkipReason::None;
    if (!entry.isObject())
        return fail(reason, SkipReason::Malformed);
    const QJsonObject o = entry.toObject();

    qint64 type = 0;
    if (!asInteger(o.value(k("type")), &type))
        return fail(reason, SkipReason::Malformed);

    UtcTime t;
    QString original;
    if (!readTime(o, &t, &original))
        return fail(reason, SkipReason::BadTimestamp);

    // Value and unit are filled only when the entry has a number and the unit can be
    // determined; otherwise null. The raw entry keeps everything.
    QJsonValue value(QJsonValue::Null);
    QJsonValue unit(QJsonValue::Null);
    if (o.value(k("Value")).isDouble()) {
        value = o.value(k("Value"));
        const UnitDetermination d = determineGlucoseUnit(
            o.value(k("GlucoseUnits")), value, o.value(k("ValueInMgPerDl")));
        if (d.ok())
            unit = d.unit;
    }

    QJsonObject payload;
    payload.insert(k("entry_type"), double(type));
    payload.insert(k("value"), value);
    payload.insert(k("unit"), unit);
    payload.insert(k("trend_code"), integerOrNull(o.value(k("TrendArrow"))));
    payload.insert(k("alarm_code"), integerOrNull(o.value(k("alarmType"))));
    payload.insert(k("color_code"), integerOrNull(o.value(k("MeasurementColor"))));
    payload.insert(k("raw"), o);

    QJsonObject r;
    r.insert(k("shape"), k("event"));
    r.insert(k("subject_ref"), subjectRef);
    r.insert(k("metric"), metricLogbookEntry());
    r.insert(k("payload"), payload);
    r.insert(k("measured_at"), t.toIso());
    r.insert(k("original_timestamp"), original);
    r.insert(k("source"), libreSourceTag(k("logbook")));
    *record = r;
    *measuredAt = t;
    return true;
}

QJsonObject buildSensorObject(const QJsonValue &sensor)
{
    QJsonObject out;
    if (!sensor.isObject())
        return out;
    const QJsonObject s = sensor.toObject();
    const QJsonValue sn = s.value(k("sn"));
    if (sn.isString() && !sn.toString().isEmpty()) {
        // The serial identifies a physical device and never leaves the adapter.
        out.insert(k("id"), QString::fromLatin1(
            QCryptographicHash::hash(sn.toString().toUtf8(), QCryptographicHash::Sha256).toHex()));
    }
    qint64 n = 0;
    if (asInteger(s.value(k("a")), &n) && n > 0)
        out.insert(k("activated_at"), UtcTime::fromMSecsSinceEpoch(n * 1000).toIso());
    if (asInteger(s.value(k("w")), &n) && n >= 0)
        out.insert(k("warmup_minutes"), double(n));
    return out;
}

} // namespace lm
