// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Michele Brami
#include "record/record.h"

#include "record/registry.h"

#include <cmath>

namespace lm {

QString originName(Origin origin)
{
    switch (origin) {
    case Origin::Adapter: return QStringLiteral("adapter");
    case Origin::Manual: return QStringLiteral("manual");
    case Origin::Derived: return QStringLiteral("derived");
    }
    return QString();
}

bool RecordKey::operator==(const RecordKey &o) const
{
    return subject == o.subject && metric == o.metric && measuredAtMs == o.measuredAtMs
        && source == o.source && entryId == o.entryId;
}

bool RecordKey::operator<(const RecordKey &o) const
{
    if (subject != o.subject) return subject < o.subject;
    if (metric != o.metric) return metric < o.metric;
    if (measuredAtMs != o.measuredAtMs) return measuredAtMs < o.measuredAtMs;
    if (source != o.source) return source < o.source;
    return entryId < o.entryId;
}

QString RecordKey::toString() const
{
    return subject.toString() + QLatin1Char('/') + metric + QLatin1Char('/')
        + QString::number(measuredAtMs) + QLatin1Char('/') + source + QLatin1Char('/') + entryId;
}

RecordKey Record::key() const
{
    RecordKey k;
    k.subject = subject;
    k.metric = metric;
    k.measuredAtMs = measuredAt.msecsSinceEpoch();
    k.source = source;
    if (origin == Origin::Manual)
        k.entryId = entryId;
    return k;
}

namespace {

Validation reject(Validation::Reason reason, const QString &detail)
{
    Validation v;
    v.reason = reason;
    v.detail = detail;
    return v;
}

bool isIntegral(const QJsonValue &v)
{
    if (!v.isDouble())
        return false;
    const double d = v.toDouble();
    return std::isfinite(d) && d == std::floor(d) && std::fabs(d) <= 9007199254740992.0;
}

bool typeMatches(const PayloadField &f, const QJsonValue &v)
{
    switch (f.type) {
    case PayloadField::Integer: return isIntegral(v);
    case PayloadField::Number: return v.isDouble() && std::isfinite(v.toDouble());
    case PayloadField::String: return v.isString();
    case PayloadField::Object: return v.isObject();
    }
    return false;
}

// Structure only: names, types, required fields, known units. Values are never range
// checked, so an uninterpreted entry cannot turn into a quarantine entry.
Validation validatePayload(const MetricDef &def, const QJsonObject &payload)
{
    for (auto it = payload.constBegin(); it != payload.constEnd(); ++it) {
        const PayloadField *f = def.findPayloadField(it.key());
        const QString where = QStringLiteral("payload.") + it.key();
        if (!f)
            return reject(Validation::InvalidPayload, where);
        if (it.value().isNull()) {
            if (f->required || !f->nullable)
                return reject(Validation::InvalidPayload, where);
            continue;
        }
        if (!typeMatches(*f, it.value()))
            return reject(Validation::InvalidPayload, where);
        if (f->knownUnit && !MetricRegistry::instance().isKnownUnit(it.value().toString()))
            return reject(Validation::UnitNotAccepted, where);
    }
    for (const PayloadField &f : def.payloadFields)
        if (f.required && !payload.contains(f.name))
            return reject(Validation::InvalidPayload, QStringLiteral("payload.") + f.name);
    return Validation();
}

} // namespace

Validation validateRecord(const Record &r, const UtcTime &now, qint64 clockSkewToleranceMs)
{
    if (r.subject.isNull())
        return reject(Validation::MissingSubject, QStringLiteral("subject"));

    const MetricDef *def = MetricRegistry::instance().find(r.metric);
    if (!def)
        return reject(Validation::UnknownMetric, QStringLiteral("metric"));
    if ((def->shape == MetricDef::Sample) != (r.shape == Record::Sample))
        return reject(Validation::ShapeMismatch, QStringLiteral("shape"));

    if (r.source.isEmpty())
        return reject(Validation::MissingSource, QStringLiteral("source"));

    if (r.shape == Record::Sample) {
        if (r.unit.isEmpty())
            return reject(Validation::MissingUnit, QStringLiteral("unit"));
        const UnitRange *range = def->findUnit(r.unit);
        if (!range)
            return reject(Validation::UnitNotAccepted, QStringLiteral("unit"));
        if (!std::isfinite(r.value))
            return reject(Validation::ValueNotFinite, QStringLiteral("value"));
        if (r.value < range->min || r.value > range->max)
            return reject(Validation::ValueOutOfRange, QStringLiteral("value"));
    } else {
        const Validation p = validatePayload(*def, r.payload);
        if (!p.ok())
            return p;
        if (r.durationMs < -1)
            return reject(Validation::InvalidDuration, QStringLiteral("duration"));
    }

    if (!r.measuredAt.isValid())
        return reject(Validation::MissingMeasurementTime, QStringLiteral("measuredAt"));
    if (!r.measuredAt.isNotAfter(now, clockSkewToleranceMs))
        return reject(Validation::MeasurementTimeInFuture, QStringLiteral("measuredAt"));
    if (!r.ingestedAt.isValid())
        return reject(Validation::MissingIngestTime, QStringLiteral("ingestedAt"));

    switch (r.origin) {
    case Origin::Adapter:
        if (r.originalTimestamp.isEmpty())
            return reject(Validation::MissingOriginalTimestamp, QStringLiteral("originalTimestamp"));
        break;
    case Origin::Manual:
        if (r.entryId.isEmpty())
            return reject(Validation::MissingEntryId, QStringLiteral("entryId"));
        break;
    case Origin::Derived:
        if (r.ruleVersion.isEmpty())
            return reject(Validation::MissingRuleVersion, QStringLiteral("ruleVersion"));
        break;
    }
    return Validation();
}

} // namespace lm
