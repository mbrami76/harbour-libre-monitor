// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Michele Brami
#ifndef LM_REAL_STATUSPARSER_H
#define LM_REAL_STATUSPARSER_H

// Reads the JSON the service returns from Status() (daemon/serviceruntime.cpp `apiStatusJson`,
//) into plain values. Pure: no D-Bus, so the host tests feed it
// text.
//
// Assumptions: the contract of Status() is unstable until the first release (REQ-ADP-14), so
// the parser (1) refuses a `format` it does not know, with the fixed key "format-newer", and (2)
// ignores every key it does not know and treats every key it expects but does not find as absent,
// because the richer Status() (adapter and version, report_proposed, trend_code and
// trend_for, sensor) is built later: the UI must work with the older one. Times are
// ISO 8601 UTC text and become milliseconds since the epoch; an empty text is "none" (-1).
// No name, credential or reference is ever in this JSON.

#include <QList>
#include <QString>
#include <QStringList>
#include <QtGlobal>

namespace lm {
namespace real {

struct SubjectReport
{
    SubjectReport() : stale(false), staleSinceMs(-1), lastSuccessAtMs(-1), retryAtMs(-1),
                      reportProposed(false), hasSensor(false), sensorActivatedMs(-1), sensorWarmupMinutes(-1),
                      hasTrend(false), trendCode(0), trendForMs(-1) {}

    QString subject;            // the Subject's UUID in braces
    QString connection;
    QString state, cause;       // scheduler state and its cause
    bool stale;
    QString staleCause, staleDetail;
    qint64 staleSinceMs, lastSuccessAtMs, retryAtMs;
    // additions; absent in the first version of Status().
    QString adapter, adapterVersion;
    bool reportProposed;
    // The sensor the service knows for the Subject (REQ-SEN-01): a hash of its serial, never the
    // serial; each part only when known. Parsed now, shown on the sensor block.
    bool hasSensor;
    QString sensorId;
    qint64 sensorActivatedMs;
    int sensorWarmupMinutes;
    bool hasTrend;
    int trendCode;
    qint64 trendForMs;          // the measurement time of the reading the arrow belongs to
};

struct ServiceReport
{
    ServiceReport() : ok(false), lastHeartbeatMs(-1), maintenanceRunning(false) {}

    bool ok;
    QString error;              // fixed key: "malformed", "format-newer"
    QString state;              // "starting", "running", "running-awaiting-approval", "stopping", ...
    QString notice;
    qint64 lastHeartbeatMs;
    QString startCause;
    // The service's own build version, `service.app_version`, the spec's Version-Release such as "0.1-3". EMPTY when the key is absent: a service built before
    // that round does not send it, and that absence alone tells the window the service is older.
    QString appVersion;
    QStringList waitingConnections;
    QList<SubjectReport> subjects;
    bool maintenanceRunning;
    QString maintenanceKind;

    // The report of one Subject, or 0.
    const SubjectReport *subject(const QString &uuid) const;
};

const int kKnownStatusFormat = 1;

ServiceReport parseServiceStatus(const QByteArray &json);

} // namespace real
} // namespace lm

#endif
