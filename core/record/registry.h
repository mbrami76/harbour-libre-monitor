// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Michele Brami
#ifndef LM_REGISTRY_H
#define LM_REGISTRY_H

#include <QHash>
#include <QJsonObject>
#include <QList>
#include <QString>
#include <QStringList>

namespace lm {

// Plausible range of a metric in one accepted unit (inclusive).
struct UnitRange
{
    QString unit;   // exact, case-sensitive; never normalised or inferred (REQ-DAT-01)
    double min;
    double max;
};

// One allowed payload field of an Event.
struct PayloadField
{
    enum Type { Integer, Number, String, Object };

    QString name;
    Type type;
    bool required;      // must be present and not null
    bool nullable;      // a JSON null is accepted when the field is optional
    bool knownUnit;     // String only: the value must be a unit known to the registry
};

// Whether and how the compaction rule thins samples of a kind (REQ-STO-07, REQ-STO-12).
// Data only; the algorithm belongs to the store (core/store/compactor.cpp).
struct ThinningRule
{
    ThinningRule() : applies(false), slotMinutes(0) {}
    ThinningRule(bool a, int m, const QStringList &f) : applies(a), slotMinutes(m), feeds(f) {}

    bool applies;
    int slotMinutes;    // UTC-aligned slot length, 0 when thinning does not apply
    // The feeds whose samples are thinned, by the feed part of the source tag
    // ("<adapter>.<feed>", REQ-DAT-09). Samples of any other feed are never thinned.
    QStringList feeds;

    bool thinsFeed(const QString &feed) const { return applies && slotMinutes > 0 && feeds.contains(feed); }
};

// Declaration of one metric or event type. Registry data lives in code
// tables: glucose and the logbook_entry event are implemented in V1.
struct MetricDef
{
    enum Shape { Sample, Event };

    QString name;
    Shape shape;
    QList<UnitRange> units;        // accepted units with plausible range (Sample)
    double maxSamplesPerMinute;    // per Subject and feed, for the guard (REQ-GRD-01)
    QList<PayloadField> payloadFields;  // allowed payload fields (Event)
    ThinningRule thinning;         // REQ-STO-07, REQ-STO-12

    const PayloadField *findPayloadField(const QString &name) const;

    const UnitRange *findUnit(const QString &unit) const;
};

class MetricRegistry
{
public:
    static const MetricRegistry &instance();

    const MetricDef *find(const QString &name) const;
    QStringList names() const;

    // Native resolution of a unit as a number of decimals (REQ-DAT-12); -1 for a unit the
    // registry does not know. Exact, case-sensitive match.
    int unitDecimals(const QString &unit) const;
    bool isKnownUnit(const QString &unit) const { return unitDecimals(unit) >= 0; }

    // Thinning rule of a kind; unknown kinds are never thinned (REQ-STO-12).
    ThinningRule thinningFor(const QString &name) const;

private:
    MetricRegistry();
    QList<MetricDef> m_defs;
    QHash<QString, int> m_unitDecimals;
};

// Metric names.
inline QString metricGlucose() { return QStringLiteral("glucose"); }
inline QString metricLogbookEntry() { return QStringLiteral("logbook_entry"); }

} // namespace lm

#endif
