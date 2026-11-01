// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Michele Brami
#include "output.h"

#include "record/registry.h"

#include <QJsonArray>

#include <algorithm>

namespace lmcli {

namespace {

QJsonValue textOrNull(const QString &s)
{
    return s.isEmpty() ? QJsonValue(QJsonValue::Null) : QJsonValue(s);
}

QJsonValue timeOrNull(const lm::UtcTime &t)
{
    return t.isValid() ? QJsonValue(t.toIso()) : QJsonValue(QJsonValue::Null);
}

QString sensorLine(const QJsonObject &sensor)
{
    QStringList parts;
    if (sensor.value(QStringLiteral("id")).isString())
        parts << QStringLiteral("id ") + sensor.value(QStringLiteral("id")).toString().left(8);
    if (sensor.value(QStringLiteral("activated_at")).isString())
        parts << QStringLiteral("activated ") + sensor.value(QStringLiteral("activated_at")).toString();
    if (sensor.value(QStringLiteral("warmup_minutes")).isDouble())
        parts << QStringLiteral("warm-up %1 min").arg(sensor.value(QStringLiteral("warmup_minutes")).toInt());
    return parts.join(QStringLiteral(", "));
}

bool byTime(const lm::Record &a, const lm::Record &b)
{
    return a.measuredAt < b.measuredAt;
}

QJsonObject recordJson(const lm::Record &r)
{
    QJsonObject o;
    o.insert(QStringLiteral("metric"), r.metric);
    o.insert(QStringLiteral("measured_at"), r.measuredAt.toIso());
    o.insert(QStringLiteral("source"), r.source);
    o.insert(QStringLiteral("unit"), textOrNull(r.unit));
    if (r.shape == lm::Record::Sample)
        o.insert(QStringLiteral("value"), r.value);
    else
        o.insert(QStringLiteral("payload"), r.payload);
    return o;
}

QJsonObject currentOf(const lm::Record &r, const lm::UtcTime &now)
{
    QJsonObject c;
    c.insert(QStringLiteral("value"), r.value);
    c.insert(QStringLiteral("unit"), r.unit);
    c.insert(QStringLiteral("measured_at"), r.measuredAt.toIso());
    c.insert(QStringLiteral("age_minutes"), static_cast<double>(ageMinutes(r.measuredAt, now)));
    c.insert(QStringLiteral("source"), r.source);
    return c;
}

} // namespace

bool newestGlucose(const QList<lm::Record> &records, lm::Record *out)
{
    bool found = false;
    for (int i = 0; i < records.size(); ++i) {
        const lm::Record &r = records.at(i);
        if (r.metric != lm::metricGlucose() || r.shape != lm::Record::Sample)
            continue;
        if (!found || out->measuredAt < r.measuredAt) {
            *out = r;
            found = true;
        }
    }
    return found;
}

qint64 ageMinutes(const lm::UtcTime &measured, const lm::UtcTime &now)
{
    if (!measured.isValid() || !now.isValid())
        return 0;
    const qint64 ms = now.msecsSinceEpoch() - measured.msecsSinceEpoch();
    return ms < 0 ? 0 : ms / 60000;
}

QString formatValue(double value, const QString &unit)
{
    int decimals = lm::MetricRegistry::instance().unitDecimals(unit);
    if (decimals < 0)
        decimals = 1;
    return QString::number(value, 'f', decimals) + QLatin1Char(' ') + unit;
}

QString formatDiscover(const QList<lm::SubjectInfo> &subjects)
{
    QString out;
    for (int i = 0; i < subjects.size(); ++i) {
        const lm::SubjectInfo &s = subjects.at(i);
        out += QStringLiteral("%1  %2  unit: %3  basis: %4\n")
                   .arg(i)
                   .arg(s.displayName)
                   .arg(s.unit.isEmpty() ? QStringLiteral("undetermined") : s.unit)
                   .arg(s.unitBasis);
    }
    if (subjects.isEmpty())
        out = QStringLiteral("no Subjects\n");
    return out;
}

QString formatCurrent(const QList<SubjectResult> &results, const lm::UtcTime &now)
{
    QString out;
    for (int i = 0; i < results.size(); ++i) {
        const SubjectResult &r = results.at(i);
        out += QStringLiteral("Subject %1: %2\n").arg(r.index).arg(r.info.displayName);
        lm::Record newest;
        if (newestGlucose(r.records, &newest)) {
            out += QStringLiteral("  value:    %1\n").arg(formatValue(newest.value, newest.unit));
            out += QStringLiteral("  measured: %1 (%2 min ago)\n")
                       .arg(newest.measuredAt.toIso()).arg(ageMinutes(newest.measuredAt, now));
            out += QStringLiteral("  source:   %1\n").arg(newest.source);
        } else {
            out += QStringLiteral("  no current value\n");
        }
        const QString sensor = sensorLine(r.sensor);
        if (!sensor.isEmpty())
            out += QStringLiteral("  sensor:   %1\n").arg(sensor);
    }
    return out;
}

QString formatRange(const QList<SubjectResult> &results)
{
    QString out;
    for (int i = 0; i < results.size(); ++i) {
        const SubjectResult &r = results.at(i);
        out += QStringLiteral("Subject %1: %2\n").arg(r.index).arg(r.info.displayName);
        QList<lm::Record> sorted = r.records;
        std::stable_sort(sorted.begin(), sorted.end(), byTime);
        for (int j = 0; j < sorted.size(); ++j) {
            const lm::Record &rec = sorted.at(j);
            if (rec.shape == lm::Record::Sample)
                out += QStringLiteral("  %1  %2  %3\n").arg(rec.measuredAt.toIso())
                           .arg(formatValue(rec.value, rec.unit)).arg(rec.source);
            else
                out += QStringLiteral("  %1  %2 event  %3\n").arg(rec.measuredAt.toIso())
                           .arg(rec.metric).arg(rec.source);
        }
        out += QStringLiteral("  records: %1 received, %2 valid, %3 rejected\n")
                   .arg(r.received).arg(r.valid).arg(r.quarantined);
        for (int c = 0; c < r.coverage.size(); ++c)
            out += QStringLiteral("  coverage %1: %2 to %3\n").arg(r.coverage.at(c).feed)
                       .arg(r.coverage.at(c).from.toIso()).arg(r.coverage.at(c).to.toIso());
    }
    return out;
}

QJsonObject discoverJson(const QList<lm::SubjectInfo> &subjects, bool withRef)
{
    QJsonArray list;
    for (int i = 0; i < subjects.size(); ++i) {
        QJsonObject o;
        o.insert(QStringLiteral("index"), i);
        o.insert(QStringLiteral("display_name"), subjects.at(i).displayName);
        o.insert(QStringLiteral("unit"), textOrNull(subjects.at(i).unit));
        o.insert(QStringLiteral("unit_basis"), subjects.at(i).unitBasis);
        if (withRef)
            o.insert(QStringLiteral("subject_ref"), subjects.at(i).subjectRef);
        list.append(o);
    }
    QJsonObject root;
    root.insert(QStringLiteral("subjects"), list);
    return root;
}

QJsonObject currentJson(const QList<SubjectResult> &results, const lm::UtcTime &now)
{
    QJsonArray list;
    for (int i = 0; i < results.size(); ++i) {
        const SubjectResult &r = results.at(i);
        QJsonObject o;
        o.insert(QStringLiteral("index"), r.index);
        o.insert(QStringLiteral("display_name"), r.info.displayName);
        lm::Record newest;
        o.insert(QStringLiteral("current"),
                 newestGlucose(r.records, &newest) ? QJsonValue(currentOf(newest, now))
                                                    : QJsonValue(QJsonValue::Null));
        o.insert(QStringLiteral("sensor"),
                 r.sensor.isEmpty() ? QJsonValue(QJsonValue::Null) : QJsonValue(r.sensor));
        list.append(o);
    }
    QJsonObject root;
    root.insert(QStringLiteral("subjects"), list);
    return root;
}

QJsonObject rangeJson(const QList<SubjectResult> &results)
{
    QJsonArray list;
    for (int i = 0; i < results.size(); ++i) {
        const SubjectResult &r = results.at(i);
        QJsonObject o;
        o.insert(QStringLiteral("index"), r.index);
        o.insert(QStringLiteral("display_name"), r.info.displayName);
        QList<lm::Record> sorted = r.records;
        std::stable_sort(sorted.begin(), sorted.end(), byTime);
        QJsonArray records;
        for (int j = 0; j < sorted.size(); ++j)
            records.append(recordJson(sorted.at(j)));
        o.insert(QStringLiteral("records"), records);
        QJsonArray coverage;
        for (int c = 0; c < r.coverage.size(); ++c) {
            QJsonObject cov;
            cov.insert(QStringLiteral("feed"), r.coverage.at(c).feed);
            cov.insert(QStringLiteral("from"), r.coverage.at(c).from.toIso());
            cov.insert(QStringLiteral("to"), r.coverage.at(c).to.toIso());
            coverage.append(cov);
        }
        o.insert(QStringLiteral("coverage"), coverage);
        o.insert(QStringLiteral("watermark"), timeOrNull(r.watermark));
        QJsonObject counts;
        counts.insert(QStringLiteral("received"), static_cast<double>(r.received));
        counts.insert(QStringLiteral("valid"), static_cast<double>(r.valid));
        counts.insert(QStringLiteral("rejected"), static_cast<double>(r.quarantined));
        o.insert(QStringLiteral("records_count"), counts);
        o.insert(QStringLiteral("sensor"),
                 r.sensor.isEmpty() ? QJsonValue(QJsonValue::Null) : QJsonValue(r.sensor));
        list.append(o);
    }
    QJsonObject root;
    root.insert(QStringLiteral("subjects"), list);
    return root;
}

} // namespace lmcli
