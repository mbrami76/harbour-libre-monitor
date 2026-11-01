// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Michele Brami
#ifndef LM_UI_SENSORVIEW_H
#define LM_UI_SENSORVIEW_H

// The sensor block of the patient page (REQ-SEN-01 to 03): which sensor the patient wears, when it started, whether its warm-up still runs (and that the
// no-data alerts wait meanwhile), and when the monitoring first and last saw it.
//
// Where the facts come from: the store's `sensor` table, read READ-ONLY by the interface like the
// readings (app/real/storequeries.h). It is the one place that has all of them: the service's Status()
// carries the identifier, the activation and the warm-up of the same row (daemon/servicecore.cpp
// `latestSensor` picks it the same way, the most recently seen sensor of the patient), but not the first
// and last time seen. One source, so the block cannot mix two sensors.
//
// Pattern: plain facts plus one pure function to the map the QML shows; tested on the host.
//
// The identifier is shown only in a SHORT HASHED form, never as stored: the adapter reports a hash of the
// serial number, and to be safe even if a later adapter reported the serial itself the screen
// shows the first 8 hexadecimal characters of the SHA-256 of whatever is stored, as "xxxx-xxxx". It is
// enough to tell two sensors apart on one phone; it cannot be turned back into the serial.
// "First seen" and "last seen" are the times the service noted the sensor while fetching readings (the
// store calls them not authoritative): the page says "seen", not "reading".

#include <QString>
#include <QVariantMap>
#include <QtGlobal>

namespace lm {
namespace ui {

struct SensorFacts
{
    SensorFacts() : known(false), activatedMs(-1), warmupMinutes(-1), firstSeenMs(-1), lastSeenMs(-1) {}

    bool known;                 // false: no sensor stored for the patient yet
    QString id;                 // as stored (a hash of the serial); NEVER shown as it is
    qint64 activatedMs;         // -1 unknown
    int warmupMinutes;          // -1 unknown
    qint64 firstSeenMs, lastSeenMs;     // -1 unknown
};

// "xxxx-xxxx": the first 8 hexadecimal characters of the SHA-256 of the stored identifier. Empty for an
// empty identifier.
QString shortSensorId(const QString &storedId);

// The map the page reads: known (bool), shortId, activatedMs, warmupEndMs (activation plus the warm-up,
// -1 when either is unknown), warmingUp (true while now is before that end), firstSeenMs, lastSeenMs
// (numbers; -1 = unknown). The stored identifier is not in the map.
QVariantMap sensorMap(const SensorFacts &facts, qint64 nowMs);

} // namespace ui
} // namespace lm

#endif
