// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Michele Brami
#include "record/registry.h"

namespace lm {

const UnitRange *MetricDef::findUnit(const QString &unit) const
{
    for (const UnitRange &u : units)
        if (u.unit == unit)
            return &u;
    return nullptr;
}

const PayloadField *MetricDef::findPayloadField(const QString &name) const
{
    for (const PayloadField &f : payloadFields)
        if (f.name == name)
            return &f;
    return nullptr;
}

MetricRegistry::MetricRegistry()
{
    // Native resolution (REQ-DAT-12). The mmol/L figure is unverified until a capture
    // of an mmol/L account exists.
    m_unitDecimals.insert(QStringLiteral("mg/dL"), 0);
    m_unitDecimals.insert(QStringLiteral("mmol/L"), 1);

    MetricDef glucose;
    glucose.name = metricGlucose();
    glucose.shape = MetricDef::Sample;
    // PROVISIONAL: 20 to 600 mg/dL covers the sensor range with margin; the
    // mmol/L bounds are the same limits divided by 18.016 and rounded outwards.
    glucose.units.append({ QStringLiteral("mg/dL"), 20.0, 600.0 });
    glucose.units.append({ QStringLiteral("mmol/L"), 1.1, 33.4 });
    // PROVISIONAL: the source updates once a minute; twice that
    // leaves room for two feeds of one Subject (REQ-DAT-09).
    glucose.maxSamplesPerMinute = 2.0;
    // Thinning (REQ-STO-07): 5-minute slots, the `current` feed only (about one
    // sample per minute); the `graph` (history) feed is kept in full.
    glucose.thinning = ThinningRule(true, 5, QStringList() << QStringLiteral("current"));
    m_defs.append(glucose);

    // Logbook entries are stored uninterpreted (REQ-SRC-08): no value range, no
    // thinning (events are kept in full, REQ-STO-06).
    MetricDef logbook;
    logbook.name = metricLogbookEntry();
    logbook.shape = MetricDef::Event;
    logbook.maxSamplesPerMinute = 0.0;
    logbook.payloadFields = {
        { QStringLiteral("entry_type"), PayloadField::Integer, true, false, false },
        { QStringLiteral("value"), PayloadField::Number, false, true, false },
        { QStringLiteral("unit"), PayloadField::String, false, true, true },
        { QStringLiteral("trend_code"), PayloadField::Integer, false, true, false },
        { QStringLiteral("alarm_code"), PayloadField::Integer, false, true, false },
        { QStringLiteral("color_code"), PayloadField::Integer, false, true, false },
        { QStringLiteral("raw"), PayloadField::Object, true, false, false }
    };
    m_defs.append(logbook);
}

const MetricRegistry &MetricRegistry::instance()
{
    static const MetricRegistry registry;
    return registry;
}

const MetricDef *MetricRegistry::find(const QString &name) const
{
    for (const MetricDef &d : m_defs)
        if (d.name == name)
            return &d;
    return nullptr;
}

QStringList MetricRegistry::names() const
{
    QStringList list;
    for (const MetricDef &d : m_defs)
        list.append(d.name);
    return list;
}

int MetricRegistry::unitDecimals(const QString &unit) const
{
    return m_unitDecimals.value(unit, -1);
}

ThinningRule MetricRegistry::thinningFor(const QString &name) const
{
    const MetricDef *d = find(name);
    return d ? d->thinning : ThinningRule();
}

} // namespace lm
