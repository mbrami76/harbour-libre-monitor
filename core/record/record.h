// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Michele Brami
#ifndef LM_RECORD_H
#define LM_RECORD_H

#include "time/utctime.h"

#include <QJsonObject>
#include <QString>
#include <QUuid>
#include <QtGlobal>

namespace lm {

enum class Origin { Adapter, Manual, Derived };

QString originName(Origin origin);

// Uniqueness key of REQ-DAT-06: (Subject, type, timestamp, source); manual records add
// the generated entry ID.
struct RecordKey
{
    QUuid subject;
    QString metric;
    qint64 measuredAtMs;
    QString source;
    QString entryId;    // empty unless origin is manual

    bool operator==(const RecordKey &o) const;
    bool operator!=(const RecordKey &o) const { return !(*this == o); }
    bool operator<(const RecordKey &o) const;
    QString toString() const;   // for logs and tests only
};

// A Sample or an Event. Plain data: nothing here converts or corrects.
struct Record
{
    enum Shape { Sample, Event };

    Record() : shape(Sample), value(0.0), durationMs(-1), origin(Origin::Adapter) {}

    Shape shape;
    QUuid subject;                  // opaque ID, never a name (REQ-SUB-01)
    QString metric;                 // registry name, e.g. "glucose"
    double value;                   // Sample
    QJsonObject payload;            // Event
    qint64 durationMs;              // Event, -1 = none
    QString unit;                   // as reported by the source (REQ-DAT-01)
    UtcTime measuredAt;             // UTC (REQ-DAT-02)
    UtcTime ingestedAt;
    QString originalTimestamp;      // as received, kept as audit
    QString source;                 // source tag, includes the feed (REQ-DAT-09)
    Origin origin;
    QString entryId;                // Manual only
    QString ruleVersion;            // Derived only (REQ-DAT-03)

    RecordKey key() const;
};

// Result of validation: a reason, never a correction (REQ-ADM-02, REQ-DAT-08).
struct Validation
{
    enum Reason {
        Ok,
        MissingSubject, UnknownMetric, ShapeMismatch, MissingUnit, UnitNotAccepted,
        ValueNotFinite, ValueOutOfRange, InvalidPayload, InvalidDuration,
        MissingMeasurementTime, MeasurementTimeInFuture, MissingIngestTime,
        MissingOriginalTimestamp, MissingSource, MissingEntryId, MissingRuleVersion,
        // Added (Draft 0.2). Only the session driver
        // produces them: a record that names a metric, unit or feed the adapter did not
        // declare in `hello`, and a measured_at that is present but not strict ISO 8601 UTC
        // (MissingMeasurementTime stays for an absent or non-text value). Appended at the
        // end on purpose: reasons travel as text (store, archive), never as numbers, but a
        // stable order keeps the tests that walk the enumeration simple. Add new reasons
        // after the last one, and extend the name tables of the driver and of the store.
        UndeclaredMetric, UndeclaredUnit, UndeclaredFeed, MalformedMeasurementTime
    };

    Validation() : reason(Ok) {}
    bool ok() const { return reason == Ok; }

    Reason reason;
    QString detail;     // names the field or limit, does not repeat the record content
};

// Checks one record against the registry. "now" and the tolerance are parameters so
// tests and callers control the clock.
Validation validateRecord(const Record &record, const UtcTime &now,
                          qint64 clockSkewToleranceMs = kDefaultClockSkewToleranceMs);

} // namespace lm

#endif
