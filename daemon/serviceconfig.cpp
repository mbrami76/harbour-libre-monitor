// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Michele Brami
#include "serviceconfig.h"

#include "daemon/discovery.h"
#include "session/adapterregistry.h"

#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSet>
#include <QUuid>

#include <cmath>

namespace lm {

namespace {

ServiceConfig fail(const char *key)
{
    ServiceConfig c;
    c.error = QString::fromLatin1(key);
    return c;
}

// A non-negative whole number of seconds, or -1 when absent (= the scheduler's default).
bool readIntervalMs(const QJsonObject &o, const char *key, qint64 *ms)
{
    const QJsonValue v = o.value(QLatin1String(key));
    if (v.isUndefined()) {
        *ms = -1;
        return true;
    }
    if (!v.isDouble() || v.toDouble() < 1 || v.toDouble() > 7 * 24 * 3600)
        return false;
    *ms = qint64(v.toDouble()) * 1000;
    return true;
}

// A per-Subject alert key in whole seconds: -1 when the key is absent or not a
// non-negative whole number, so a page that wrote something odd leaves the default in force and never
// stops the service from starting (a wrong interval above is different: it is refused). A very large
// number is held at seven days before it is turned into milliseconds; the engine then clamps it to its own
// range, which is the one place that range is defined (core/alerts/alertsettings.h).
qint64 readWholeSeconds(const QJsonObject &o, const char *key)
{
    const QJsonValue v = o.value(QLatin1String(key));
    if (!v.isDouble())
        return -1;
    const double d = v.toDouble();
    const double cap = 7 * 24 * 3600;
    if (!(d >= 0))
        return -1;                      // negative, or not a number
    if (d > cap)
        return qint64(cap);
    if (d != std::floor(d))
        return -1;                      // not a whole number of seconds
    return qint64(d);
}

} // namespace

ServiceConfig parseServiceConfig(const QByteArray &json)
{
    QJsonParseError pe;
    const QJsonDocument doc = QJsonDocument::fromJson(json, &pe);
    if (pe.error != QJsonParseError::NoError || !doc.isObject())
        return fail("config-not-json");
    const QJsonObject root = doc.object();
    if (root.value(QStringLiteral("format")).toInt(-1) != 1)
        return fail("config-format");
    const QJsonValue list = root.value(QStringLiteral("connections"));
    if (!list.isArray())
        return fail("config-connections");

    ServiceConfig out;
    QSet<QString> ids;
    QSet<QUuid> subjects;
    const QJsonArray conns = list.toArray();
    for (int i = 0; i < conns.size(); ++i) {
        if (!conns.at(i).isObject())
            return fail("config-connection");
        const QJsonObject o = conns.at(i).toObject();
        ConnectionConfig c;
        c.id = o.value(QStringLiteral("id")).toString();
        c.adapterId = o.value(QStringLiteral("adapter")).toString();
        if (c.id.isEmpty() || c.id.size() > 64 || ids.contains(c.id))
            return fail("config-connection-id");
        // `probe` names the one-time login test of the set-up page: a connection with
        // that id would be read from, and have its secret removed as, a probe.
        if (c.id == probeConnectionId())
            return fail("config-connection-id");
        ids.insert(c.id);
        // Only built-in adapters exist (REQ-ADP-05, REQ-ADP-16).
        if (!AdapterRegistry::instance().find(c.adapterId))
            return fail("config-adapter");
        if (!readIntervalMs(o, "current_interval_s", &c.currentIntervalMs)
            || !readIntervalMs(o, "graph_interval_s", &c.graphIntervalMs)
            || !readIntervalMs(o, "logbook_interval_s", &c.logbookIntervalMs))
            return fail("config-interval");

        const QJsonValue subs = o.value(QStringLiteral("subjects"));
        if (!subs.isArray())
            return fail("config-subjects");
        const QJsonArray sa = subs.toArray();
        for (int j = 0; j < sa.size(); ++j) {
            const QJsonObject so = sa.at(j).toObject();
            SubjectConfig s;
            s.id = QUuid(so.value(QStringLiteral("id")).toString());
            s.subjectRef = so.value(QStringLiteral("ref")).toString();
            if (s.id.isNull() || s.subjectRef.isEmpty() || subjects.contains(s.id))
                return fail("config-subject");
            subjects.insert(s.id);
            c.subjects.append(s);
            out.subjectOrder.append(s.id);
            const QString label = so.value(QStringLiteral("label")).toString().trimmed();
            if (!label.isEmpty())
                out.subjectLabels.insert(s.id, label);
            SubjectAlertKeys keys;
            keys.persistenceLowS = readWholeSeconds(so, "alert_persistence_low_s");
            keys.persistenceHighS = readWholeSeconds(so, "alert_persistence_high_s");
            keys.missingLimitS = readWholeSeconds(so, "missing_limit_s");
            if (keys.persistenceLowS >= 0 || keys.persistenceHighS >= 0 || keys.missingLimitS >= 0)
                out.subjectAlertKeys.insert(s.id, keys);
        }
        const QString connectionLabel = o.value(QStringLiteral("label")).toString().trimmed();
        if (!connectionLabel.isEmpty() && connectionLabel.size() <= 24)
            out.connectionLabels.insert(c.id, connectionLabel);
        out.connections.append(c);
    }
    out.language = root.value(QStringLiteral("language")).toString().trimmed();
    out.alertsShowValue = root.value(QStringLiteral("alerts_show_value")).toBool(false);
    out.ok = true;
    return out;
}

ServiceConfig loadServiceConfig(const QString &path)
{
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly))
        return fail("config-unreadable");
    return parseServiceConfig(f.read(1 << 20));
}

} // namespace lm
