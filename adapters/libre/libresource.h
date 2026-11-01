// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Michele Brami
#ifndef LM_LIBRESOURCE_H
#define LM_LIBRESOURCE_H

#include "time/utctime.h"

#include <QJsonObject>
#include <QJsonValue>
#include <QString>

namespace lm {

// Why a reading or logbook entry did not become a record.
enum class SkipReason {
    None,
    BadTimestamp,       // FactoryTimestamp missing, not a string or not in the documented format
    UndeterminedUnit,   // REQ-DAT-13: not stored as a sample
    NoValue,            // `Value` is an explicit JSON null: a reading without a number
    Malformed           // not an object, `Value` absent or not a number/null, `type` missing
};

// Counts of one fetch. Format problems (bad timestamp, undetermined unit, malformed) can
// announce a changed source; NoValue cannot, it is an explicit null.
struct SkipCounts
{
    SkipCounts() : examined(0), badTimestamp(0), undeterminedUnit(0), noValue(0), malformed(0) {}

    int examined;
    int badTimestamp;
    int undeterminedUnit;
    int noValue;
    int malformed;

    int format() const { return badTimestamp + undeterminedUnit + malformed; }
    void add(SkipReason reason);    // counts one examined item with this outcome
};

// Burst rule of REQ-ADM-03 for the adapter side. PROVISIONAL:
// escalate to protocol-changed when nothing usable remains although format problems were
// seen, or when at least three items had format problems and they are half or more of
// all items examined. Below that the items are only counted and logged.
bool shouldEscalate(const SkipCounts &counts);

// Source tag of a record: "libre.<feed>" (REQ-DAT-09).
QString libreSourceTag(const QString &feed);

// Glucose sample of the `current` or `graph` feed from one reading object. Value and
// unit are passed on exactly as received; the time is FactoryTimestamp as UTC.
// Not interpreted: TrendArrow (reported apart, see trendOfReading), isHigh, isLow,
// MeasurementColor.
bool buildGlucoseRecord(const QJsonValue &reading, const QString &subjectRef, const QString &feed,
                        QJsonObject *record, UtcTime *measuredAt, SkipReason *reason);

// The source's trend arrow of one reading (`TrendArrow`), uninterpreted: an integer, or null
// when the reading has none, the value is not a whole number or the reading is not an object.
// It travels in the completion of `fetch_current` only, is kept in memory by the core and is
// never stored. What the number means is the UI's to decide.
QJsonValue trendOfReading(const QJsonValue &reading);

// One logbook entry of any type as a `logbook_entry` event with the raw entry.
bool buildLogbookRecord(const QJsonValue &entry, const QString &subjectRef,
                        QJsonObject *record, UtcTime *measuredAt, SkipReason *reason);

// Optional `sensor` object of the completion (contract 5.5): `id` (SHA-256 hex of
// the serial `sn`; the serial itself is never sent or logged), `activated_at` (UTC from
// `a`), `warmup_minutes` (`w`), each only if present and valid. Empty object if nothing
// is usable.
QJsonObject buildSensorObject(const QJsonValue &sensor);

} // namespace lm

#endif
