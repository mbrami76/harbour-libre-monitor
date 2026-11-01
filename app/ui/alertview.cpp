// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Michele Brami
#include "alertview.h"

#include "service/subjectlabel.h"
#include "time/utctime.h"

#include <QDateTime>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>

#include <algorithm>

namespace lm {
namespace ui {

namespace {

// An ISO time of the service as milliseconds; 0 for "" or a text that is not a time (the pages then
// say nothing about it rather than a wrong time).
qint64 msOf(const QJsonObject &o, const char *key)
{
    const QString text = o.value(QLatin1String(key)).toString();
    if (text.isEmpty())
        return 0;
    const UtcTime t = UtcTime::fromIso(text);
    return t.isValid() ? t.msecsSinceEpoch() : 0;
}

int intOf(const QJsonObject &o, const char *key)
{
    const QJsonValue v = o.value(QLatin1String(key));
    return v.isDouble() ? v.toInt(-1) : -1;
}

AlertTimeRange rangeOf(const QJsonObject &limits, const char *key)
{
    const QJsonObject o = limits.value(QLatin1String(key)).toObject();
    AlertTimeRange r;
    r.minS = intOf(o, "min");
    r.maxS = intOf(o, "max");
    r.defaultS = intOf(o, "default");
    return r;
}

} // namespace

AlertsFacts parseAlertsJson(const QByteArray &json)
{
    AlertsFacts f;
    QJsonParseError err;
    const QJsonDocument doc = QJsonDocument::fromJson(json, &err);
    if (err.error != QJsonParseError::NoError || !doc.isObject()) {
        f.error = QStringLiteral("unreadable");
        return f;
    }
    const QJsonObject root = doc.object();
    // Format 1 only: a newer service that changed the meaning would raise the number, and a guess would
    // be worse than saying the alerts are not known.
    if (root.value(QLatin1String("format")).toInt(-1) != 1) {
        f.error = QStringLiteral("unreadable");
        return f;
    }
    if (root.contains(QLatin1String("error"))) {
        f.error = root.value(QLatin1String("error")).toString();
        if (f.error.isEmpty())
            f.error = QStringLiteral("unreadable");
        return f;
    }
    const QJsonArray alerts = root.value(QLatin1String("alerts")).toArray();
    for (int i = 0; i < alerts.size(); ++i) {
        const QJsonObject o = alerts.at(i).toObject();
        AlertFact a;
        a.subject = o.value(QLatin1String("subject")).toString();
        a.slot = o.value(QLatin1String("slot")).toString();
        a.level = o.value(QLatin1String("level")).toString();
        a.state = o.value(QLatin1String("state")).toString();
        a.episode = o.value(QLatin1String("episode")).toString();
        a.acknowledged = o.value(QLatin1String("acknowledged")).toBool(false);
        a.sinceMs = msOf(o, "since");
        a.levelSinceMs = msOf(o, "level_since");
        if (a.levelSinceMs == 0)
            a.levelSinceMs = a.sinceMs;     // the service gives the state's start when it lacks the level's
        a.snoozeUntilMs = msOf(o, "snooze_until");
        a.limitMgdl = intOf(o, "limit");
        a.cause = o.value(QLatin1String("cause")).toString();
        if (a.subject.isEmpty() || a.slot.isEmpty())
            continue;                       // not an alert this version can show
        f.alerts.append(a);
    }
    const QJsonArray th = root.value(QLatin1String("thresholds")).toArray();
    for (int i = 0; i < th.size(); ++i) {
        const QJsonObject o = th.at(i).toObject();
        PatientAlertFacts p;
        p.subject = o.value(QLatin1String("subject")).toString();
        p.ll = intOf(o, "ll");
        p.l = intOf(o, "l");
        p.h = intOf(o, "h");
        p.hh = intOf(o, "hh");
        p.persistenceLowS = intOf(o, "persistence_low_s");
        p.persistenceHighS = intOf(o, "persistence_high_s");
        p.missingLimitS = intOf(o, "missing_limit_s");
        p.silent = o.value(QLatin1String("silent")).toBool(false);
        p.silentUntilMs = p.silent ? msOf(o, "silent_until") : 0;
        if (!p.subject.isEmpty())
            f.patients.append(p);
    }
    const QJsonObject limits = root.value(QLatin1String("limits")).toObject();
    f.persistenceLow = rangeOf(limits, "persistence_low_s");
    f.persistenceHigh = rangeOf(limits, "persistence_high_s");
    f.missingLimit = rangeOf(limits, "missing_limit_s");
    f.doNotDisturb = root.value(QLatin1String("do_not_disturb")).toBool(false);
    f.known = true;
    return f;
}

QString parseSilentAnswer(const QByteArray &json, qint64 *untilMs)
{
    if (untilMs)
        *untilMs = 0;
    const QJsonDocument doc = QJsonDocument::fromJson(json);
    if (!doc.isObject())
        return QStringLiteral("unreadable");
    const QJsonObject o = doc.object();
    const QString result = o.value(QLatin1String("result")).toString();
    if (untilMs)
        *untilMs = msOf(o, "silent_until");
    return result.isEmpty() ? QStringLiteral("unreadable") : result;
}

int alertRank(const QString &level)
{
    if (level == QLatin1String("LL")) return 1;
    if (level == QLatin1String("HH")) return 2;
    if (level == QLatin1String("MISSING")) return 3;
    if (level == QLatin1String("L")) return 4;
    if (level == QLatin1String("H")) return 5;
    return 6;
}

bool alertCanSnooze(const QString &level)
{
    return level == QLatin1String("L") || level == QLatin1String("H") || level == QLatin1String("MISSING");
}

QString alertLevelKey(const QString &level)
{
    if (level == QLatin1String("LL")) return QStringLiteral("ll");
    if (level == QLatin1String("L")) return QStringLiteral("l");
    if (level == QLatin1String("H")) return QStringLiteral("h");
    if (level == QLatin1String("HH")) return QStringLiteral("hh");
    if (level == QLatin1String("MISSING")) return QStringLiteral("missing");
    return QString();
}

QString alertCauseKey(const QString &cause)
{
    // The same list as NotificationTexts::causeText, so the page and the notification say the same.
    if (cause == QLatin1String("source-unreachable")) return QStringLiteral("ui-stale-unreachable");
    if (cause == QLatin1String("no-new-readings")) return QStringLiteral("ui-stale-no-new");
    if (cause == QLatin1String("sensor-warm-up")) return QStringLiteral("ui-stale-warmup");
    if (cause == QLatin1String("disabled-by-guard")) return QStringLiteral("ui-stale-guard");
    if (cause == QLatin1String("service-not-running")) return QStringLiteral("ui-stale-service-gap");
    if (cause == QLatin1String("credentials-locked")) return QStringLiteral("ui-stale-locked");
    if (cause == QLatin1String("needs-attention") || cause.endsWith(QLatin1String("-failed")))
        return QStringLiteral("ui-attn-internal");
    return QStringLiteral("ui-stale-other");
}

QString alertResultKey(const QString &result)
{
    if (result == QLatin1String("done")) return QString();
    // Someone else (the Events page, a reminder) changed the alert first: the picture is refreshed and
    // the page says so plainly.
    if (result == QLatin1String("not-active")) return QStringLiteral("ui-alert-err-not-active");
    if (result == QLatin1String("already-acknowledged")) return QStringLiteral("ui-alert-err-already");
    if (result == QLatin1String("not-silent")) return QStringLiteral("ui-alert-err-not-silent");
    if (result == QLatin1String("service-not-running") || result == QLatin1String("call-failed"))
        return QStringLiteral("ui-alert-err-no-service");
    // The service runs but is an older build without this method: the same sentence as
    // the strip's "older service" state, which tells the user what to do.
    if (result == QLatin1String("service-older"))
        return QStringLiteral("ui-service-older");
    // not-snoozable, bad-duration, bad-slot, unknown-subject, unsupported, unreadable, anything new.
    return QStringLiteral("ui-alert-err-refused");
}

QList<int> snoozeChoicesMinutes()
{
    return QList<int>() << 15 << 30 << 60 << 120;
}

int silentMinutesForChoice(const QString &choice)
{
    if (choice == QLatin1String("hour")) return 60;
    if (choice == QLatin1String("four-hours")) return 240;
    if (choice == QLatin1String("tomorrow")) return -1;        // the service's "until tomorrow" (SetSilent)
    if (choice == QLatin1String("no-limit")) return 0;         // the service's "without limit"
    return -2;
}

bool silentChoiceNeedsConfirmation(const QString &choice)
{
    return choice == QLatin1String("no-limit");
}

SilentView silentViewOf(const AlertsFacts &facts, const QString &subject, qint64 nowMs)
{
    SilentView v;
    const PatientAlertFacts *p = facts.known ? facts.patient(subject) : 0;
    if (!p || !p->silent)
        return v;
    v.silent = true;
    v.noLimit = p->silentUntilMs <= 0;
    v.untilMs = v.noLimit ? 0 : p->silentUntilMs;
    if (!v.noLimit)
        v.untilToday = QDateTime::fromMSecsSinceEpoch(v.untilMs).toLocalTime().date()
                       == QDateTime::fromMSecsSinceEpoch(nowMs).toLocalTime().date();
    return v;
}

QVariantMap AlertRow::toMap() const
{
    QVariantMap m;
    m.insert(QStringLiteral("subjectId"), fact.subject);
    m.insert(QStringLiteral("slot"), fact.slot);
    m.insert(QStringLiteral("state"), fact.state);
    m.insert(QStringLiteral("acknowledged"), fact.acknowledged);
    m.insert(QStringLiteral("levelSinceMs"), double(fact.levelSinceMs));
    m.insert(QStringLiteral("snoozeUntilMs"), double(fact.snoozeUntilMs));
    m.insert(QStringLiteral("name"), name);
    m.insert(QStringLiteral("shortLabel"), shortLabel);
    m.insert(QStringLiteral("levelKey"), levelKey);
    m.insert(QStringLiteral("bandKey"), bandKey);
    m.insert(QStringLiteral("causeKey"), causeKey);
    m.insert(QStringLiteral("minutesSince"), minutesSince);
    m.insert(QStringLiteral("canAcknowledge"), canAcknowledge);
    m.insert(QStringLiteral("canSnooze"), canSnooze);
    m.insert(QStringLiteral("isPause"), isPause);
    m.insert(QStringLiteral("snoozed"), snoozed);
    return m;
}

QList<AlertRow> buildAlertRows(const AlertsFacts &facts, const QList<SubjectSnapshot> &subjects, qint64 nowMs)
{
    QList<AlertRow> rows;
    if (!facts.known)
        return rows;
    for (int i = 0; i < facts.alerts.size(); ++i) {
        const AlertFact &a = facts.alerts.at(i);
        AlertRow r;
        r.fact = a;
        for (int k = 0; k < subjects.size(); ++k)
            if (subjects.at(k).id == a.subject) {
                r.position = k;
                r.name = subjects.at(k).name;
                r.shortLabel = patientLabel(subjects.at(k).label, k);
                break;
            }
        if (r.position < 0)
            r.shortLabel = patientLabel(QString(), -1);     // not in the file (any more): "?", never a guess
        r.levelKey = alertLevelKey(a.level);
        r.rank = alertRank(a.level);
        const bool active = a.state == QLatin1String("active");
        r.snoozed = a.state == QLatin1String("snoozed");
        const bool glucose = a.level != QLatin1String("MISSING") && !r.levelKey.isEmpty();
        r.bandKey = glucose && active && !a.acknowledged ? r.levelKey : QString();
        r.causeKey = glucose ? QString() : alertCauseKey(a.cause);
        r.minutesSince = a.levelSinceMs > 0 && nowMs >= a.levelSinceMs ? int((nowMs - a.levelSinceMs) / 60000) : -1;
        r.canAcknowledge = active && !a.acknowledged;
        r.canSnooze = r.canAcknowledge && alertCanSnooze(a.level);
        r.isPause = a.acknowledged && (a.level == QLatin1String("LL") || a.level == QLatin1String("HH"));
        rows.append(r);
    }
    std::stable_sort(rows.begin(), rows.end(), [](const AlertRow &x, const AlertRow &y) {
        if (x.rank != y.rank)
            return x.rank < y.rank;
        if (x.fact.levelSinceMs != y.fact.levelSinceMs)
            return x.fact.levelSinceMs < y.fact.levelSinceMs;       // the earlier onset first
        return x.position < y.position;
    });
    return rows;
}

} // namespace ui
} // namespace lm
