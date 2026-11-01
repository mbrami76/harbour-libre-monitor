// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Michele Brami
#include "statusparser.h"

#include "time/utctime.h"

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>

namespace lm {
namespace real {

namespace {

qint64 msOf(const QJsonObject &o, const char *key)
{
    const QString text = o.value(QLatin1String(key)).toString();
    if (text.isEmpty())
        return -1;
    const UtcTime t = UtcTime::fromIso(text);
    return t.isValid() ? t.msecsSinceEpoch() : -1;
}

} // namespace

const SubjectReport *ServiceReport::subject(const QString &uuid) const
{
    for (int i = 0; i < subjects.size(); ++i)
        if (subjects.at(i).subject == uuid)
            return &subjects.at(i);
    return 0;
}

ServiceReport parseServiceStatus(const QByteArray &json)
{
    ServiceReport r;
    QJsonParseError pe;
    const QJsonDocument doc = QJsonDocument::fromJson(json, &pe);
    if (pe.error != QJsonParseError::NoError || !doc.isObject()) {
        r.error = QStringLiteral("malformed");
        return r;
    }
    const QJsonObject root = doc.object();
    if (root.value(QLatin1String("format")).toInt(kKnownStatusFormat) > kKnownStatusFormat) {
        r.error = QStringLiteral("format-newer");
        return r;                       // never interpret what this version does not know
    }
    const QJsonObject svc = root.value(QLatin1String("service")).toObject();
    r.state = svc.value(QLatin1String("state")).toString();
    r.notice = svc.value(QLatin1String("notice")).toString();
    r.lastHeartbeatMs = msOf(svc, "last_heartbeat");
    r.startCause = svc.value(QLatin1String("start_cause")).toString();
    r.appVersion = svc.value(QLatin1String("app_version")).toString();     // "" when absent (older service)
    const QJsonArray waiting = svc.value(QLatin1String("waiting_connections")).toArray();
    for (int i = 0; i < waiting.size(); ++i)
        r.waitingConnections << waiting.at(i).toString();

    const QJsonArray subs = root.value(QLatin1String("subjects")).toArray();
    for (int i = 0; i < subs.size(); ++i) {
        const QJsonObject o = subs.at(i).toObject();
        SubjectReport s;
        s.subject = o.value(QLatin1String("subject")).toString();
        s.connection = o.value(QLatin1String("connection")).toString();
        s.state = o.value(QLatin1String("state")).toString();
        s.cause = o.value(QLatin1String("cause")).toString();
        s.stale = o.value(QLatin1String("stale")).toBool(false);
        s.staleCause = o.value(QLatin1String("stale_cause")).toString();
        s.staleDetail = o.value(QLatin1String("stale_detail")).toString();
        s.staleSinceMs = msOf(o, "stale_since");
        s.lastSuccessAtMs = msOf(o, "last_success_at");
        s.retryAtMs = msOf(o, "retry_at");
        s.adapter = o.value(QLatin1String("adapter")).toString();
        s.adapterVersion = o.value(QLatin1String("adapter_version")).toString();
        s.reportProposed = o.value(QLatin1String("report_proposed")).toBool(false);
        const QJsonValue sensor = o.value(QLatin1String("sensor"));
        if (sensor.isObject()) {
            const QJsonObject so = sensor.toObject();
            s.hasSensor = true;
            s.sensorId = so.value(QLatin1String("id")).toString();
            s.sensorActivatedMs = msOf(so, "activated_at");
            s.sensorWarmupMinutes = so.contains(QLatin1String("warmup_minutes")) ? so.value(QLatin1String("warmup_minutes")).toInt(-1) : -1;
        }
        if (o.contains(QLatin1String("trend_code")) && !o.value(QLatin1String("trend_code")).isNull()) {
            s.hasTrend = true;
            s.trendCode = o.value(QLatin1String("trend_code")).toInt();
            s.trendForMs = msOf(o, "trend_for");
        }
        if (!s.subject.isEmpty())
            r.subjects.append(s);
    }
    const QJsonObject m = root.value(QLatin1String("maintenance")).toObject();
    r.maintenanceRunning = m.value(QLatin1String("running")).toBool(false);
    r.maintenanceKind = m.value(QLatin1String("kind")).toString();
    r.ok = true;
    return r;
}

} // namespace real
} // namespace lm
