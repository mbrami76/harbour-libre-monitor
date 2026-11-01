// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Michele Brami
#include "configstore.h"

#include <QFile>
#include <QFileDevice>
#include <QJsonArray>
#include <QJsonDocument>
#include <QSaveFile>

namespace lm {
namespace ui {

namespace {

QString str(const QJsonObject &o, const char *k) { return o.value(QLatin1String(k)).toString(); }
int wholeSeconds(const QJsonObject &o, const char *k);

SubjectEntry subjectFrom(const QJsonObject &o)
{
    SubjectEntry s;
    s.id = str(o, "id");
    s.ref = str(o, "ref");
    s.name = str(o, "name");
    s.label = str(o, "label");
    s.unit = str(o, "unit");
    s.unitBasis = str(o, "unit_basis");
    s.unitConfirmed = o.value(QLatin1String("unit_confirmed")).toBool(false);
    s.persistenceLowS = wholeSeconds(o, AlertTimeKey::persistenceLow());
    s.persistenceHighS = wholeSeconds(o, AlertTimeKey::persistenceHigh());
    s.missingLimitS = wholeSeconds(o, AlertTimeKey::missingLimit());
    return s;
}

// An alert time as the service reads it (daemon/serviceconfig.cpp): a non-negative whole number of
// seconds; anything else counts as absent (-1).
int wholeSeconds(const QJsonObject &o, const char *k)
{
    const QJsonValue v = o.value(QLatin1String(k));
    if (!v.isDouble())
        return -1;
    const double d = v.toDouble();
    if (d < 0 || d != double(qint64(d)) || d > 86400.0 * 7)
        return -1;
    return int(d);
}

void putSeconds(QJsonObject &o, const char *k, int v)
{
    if (v < 0) o.remove(QLatin1String(k)); else o.insert(QLatin1String(k), v);
}

// Sets or removes a string key: an empty value removes it so the file stays small and a
// cleared label really disappears.
void putStr(QJsonObject &o, const char *k, const QString &v)
{
    if (v.isEmpty()) o.remove(QLatin1String(k)); else o.insert(QLatin1String(k), v);
}

void putInt(QJsonObject &o, const char *k, int v)
{
    if (v <= 0) o.remove(QLatin1String(k)); else o.insert(QLatin1String(k), v);
}

} // namespace

ConfigStore::ConfigStore()
{
    m_root.insert(QStringLiteral("format"), kKnownConfigFormat);
    m_root.insert(QStringLiteral("connections"), QJsonArray());
}

bool ConfigStore::load(const QString &path)
{
    m_path = path;
    m_error.clear();
    m_root = QJsonObject();
    m_root.insert(QStringLiteral("format"), kKnownConfigFormat);
    m_root.insert(QStringLiteral("connections"), QJsonArray());

    QFile f(path);
    if (!f.exists())
        return true;                                // first run: nothing configured yet
    if (!f.open(QIODevice::ReadOnly)) {
        m_error = QStringLiteral("config-unreadable");
        return false;
    }
    QJsonParseError pe;
    const QJsonDocument doc = QJsonDocument::fromJson(f.readAll(), &pe);
    if (pe.error != QJsonParseError::NoError || !doc.isObject()) {
        m_error = QStringLiteral("config-malformed");
        return false;
    }
    const QJsonObject root = doc.object();
    if (root.value(QLatin1String("format")).toInt(kKnownConfigFormat) > kKnownConfigFormat) {
        m_error = QStringLiteral("config-format-newer");
        return false;                               // never overwrite what we do not understand
    }
    m_root = root;
    if (!m_root.contains(QLatin1String("connections")))
        m_root.insert(QStringLiteral("connections"), QJsonArray());
    return true;
}

bool ConfigStore::save()
{
    if (!m_error.isEmpty() || m_path.isEmpty())
        return false;
    QSaveFile out(m_path);
    if (!out.open(QIODevice::WriteOnly))
        return false;
    // Readable by the user only, set before the content is committed (the file holds names, REQ-SUB-03).
    out.setPermissions(QFileDevice::ReadOwner | QFileDevice::WriteOwner);
    out.write(QJsonDocument(m_root).toJson(QJsonDocument::Indented));
    return out.commit();
}

QList<ConnectionEntry> ConfigStore::connections() const
{
    QList<ConnectionEntry> result;
    const QJsonArray arr = m_root.value(QLatin1String("connections")).toArray();
    for (int i = 0; i < arr.size(); ++i) {
        const QJsonObject o = arr.at(i).toObject();
        ConnectionEntry c;
        c.id = str(o, "id");
        c.adapter = str(o, "adapter");
        c.label = str(o, "label");
        c.currentIntervalS = o.value(QLatin1String("current_interval_s")).toInt(0);
        c.graphIntervalS = o.value(QLatin1String("graph_interval_s")).toInt(0);
        c.logbookIntervalS = o.value(QLatin1String("logbook_interval_s")).toInt(0);
        const QJsonArray subs = o.value(QLatin1String("subjects")).toArray();
        for (int j = 0; j < subs.size(); ++j)
            c.subjects.append(subjectFrom(subs.at(j).toObject()));
        result.append(c);
    }
    return result;
}

void ConfigStore::setConnection(const ConnectionEntry &c)
{
    QJsonArray arr = m_root.value(QLatin1String("connections")).toArray();
    int at = -1;
    for (int i = 0; i < arr.size(); ++i)
        if (arr.at(i).toObject().value(QLatin1String("id")).toString() == c.id)
            at = i;
    // Patch the existing object so unknown keys survive; start empty for a new connection.
    QJsonObject o = at >= 0 ? arr.at(at).toObject() : QJsonObject();
    o.insert(QStringLiteral("id"), c.id);
    o.insert(QStringLiteral("adapter"), c.adapter);
    putStr(o, "label", c.label);
    putInt(o, "current_interval_s", c.currentIntervalS);
    putInt(o, "graph_interval_s", c.graphIntervalS);
    putInt(o, "logbook_interval_s", c.logbookIntervalS);

    // Subjects: keep the unknown keys of each existing one, matched by id.
    const QJsonArray oldSubs = o.value(QLatin1String("subjects")).toArray();
    QJsonArray subs;
    for (int j = 0; j < c.subjects.size(); ++j) {
        const SubjectEntry &s = c.subjects.at(j);
        QJsonObject so;
        for (int k = 0; k < oldSubs.size(); ++k)
            if (oldSubs.at(k).toObject().value(QLatin1String("id")).toString() == s.id)
                so = oldSubs.at(k).toObject();
        so.insert(QStringLiteral("id"), s.id);
        putStr(so, "ref", s.ref);
        putStr(so, "name", s.name);
        putStr(so, "label", s.label);
        putStr(so, "unit", s.unit);
        putStr(so, "unit_basis", s.unitBasis);
        so.insert(QStringLiteral("unit_confirmed"), s.unitConfirmed);
        putSeconds(so, AlertTimeKey::persistenceLow(), s.persistenceLowS);
        putSeconds(so, AlertTimeKey::persistenceHigh(), s.persistenceHighS);
        putSeconds(so, AlertTimeKey::missingLimit(), s.missingLimitS);
        subs.append(so);
    }
    o.insert(QStringLiteral("subjects"), subs);
    if (at >= 0) arr.replace(at, o); else arr.append(o);
    m_root.insert(QStringLiteral("connections"), arr);
}

bool ConfigStore::removeConnection(const QString &connectionId)
{
    QJsonArray arr = m_root.value(QLatin1String("connections")).toArray();
    for (int i = 0; i < arr.size(); ++i)
        if (arr.at(i).toObject().value(QLatin1String("id")).toString() == connectionId) {
            arr.removeAt(i);
            m_root.insert(QStringLiteral("connections"), arr);
            return true;
        }
    return false;
}

QString ConfigStore::newSubjectId()
{
    return QUuid::createUuid().toString();      // {xxxxxxxx-...} in braces, as the service writes them
}

QString ConfigStore::language() const { return str(m_root, "language").trimmed(); }

void ConfigStore::setLanguage(const QString &language) { putStr(m_root, "language", language.trimmed()); }

bool ConfigStore::alertsShowValue() const
{
    return m_root.value(QLatin1String("alerts_show_value")).toBool(false);
}

void ConfigStore::setAlertsShowValue(bool on) { m_root.insert(QStringLiteral("alerts_show_value"), on); }

bool ConfigStore::setConnectionLabel(const QString &connectionId, const QString &label)
{
    QJsonArray arr = m_root.value(QLatin1String("connections")).toArray();
    for (int i = 0; i < arr.size(); ++i) {
        QJsonObject o = arr.at(i).toObject();
        if (str(o, "id") != connectionId)
            continue;
        putStr(o, "label", label.trimmed());
        arr.replace(i, o);
        m_root.insert(QStringLiteral("connections"), arr);
        return true;
    }
    return false;
}

bool ConfigStore::patchSubject(const QString &subjectId, const std::function<void(QJsonObject &)> &patch)
{
    QJsonArray arr = m_root.value(QLatin1String("connections")).toArray();
    for (int i = 0; i < arr.size(); ++i) {
        QJsonObject o = arr.at(i).toObject();
        QJsonArray subs = o.value(QLatin1String("subjects")).toArray();
        for (int j = 0; j < subs.size(); ++j) {
            QJsonObject so = subs.at(j).toObject();
            if (str(so, "id") != subjectId)
                continue;
            patch(so);
            subs.replace(j, so);
            o.insert(QStringLiteral("subjects"), subs);
            arr.replace(i, o);
            m_root.insert(QStringLiteral("connections"), arr);
            return true;
        }
    }
    return false;
}

bool ConfigStore::setSubjectLabel(const QString &subjectId, const QString &label)
{
    const QString l = label.trimmed();
    return patchSubject(subjectId, [&l](QJsonObject &so) { putStr(so, "label", l); });
}

bool ConfigStore::setSubjectAlertTime(const QString &subjectId, const char *key, int seconds)
{
    return patchSubject(subjectId, [key, seconds](QJsonObject &so) { putSeconds(so, key, seconds); });
}

} // namespace ui
} // namespace lm
