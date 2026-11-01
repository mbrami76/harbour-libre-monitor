// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Michele Brami
#include "settingsstore.h"

#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QSaveFile>

namespace lm {
namespace ui {

SettingsStore::SettingsStore() {}

void SettingsStore::load(const QString &path)
{
    m_path = path;
    m_root = QJsonObject();
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly))
        return;
    const QJsonDocument doc = QJsonDocument::fromJson(f.readAll());
    if (doc.isObject())
        m_root = doc.object();      // anything else: defaults
}

bool SettingsStore::save() const
{
    if (m_path.isEmpty())
        return false;
    QSaveFile out(m_path);
    if (!out.open(QIODevice::WriteOnly))
        return false;
    out.write(QJsonDocument(m_root).toJson(QJsonDocument::Indented));
    return out.commit();
}

QString SettingsStore::displayUnit() const
{
    const QString u = m_root.value(QLatin1String("display_unit")).toString();
    return (u == QLatin1String("mg/dL") || u == QLatin1String("mmol/L")) ? u : QString();
}

void SettingsStore::setDisplayUnit(const QString &unit)
{
    if (unit == QLatin1String("mg/dL") || unit == QLatin1String("mmol/L"))
        m_root.insert(QStringLiteral("display_unit"), unit);
}

SettingsStore::Feeds SettingsStore::feedsShown() const
{
    const QString f = m_root.value(QLatin1String("feeds_shown")).toString();
    if (f == QLatin1String("current")) return FeedCurrent;
    if (f == QLatin1String("history")) return FeedHistory;
    return FeedBoth;
}

void SettingsStore::setFeedsShown(Feeds f)
{
    m_root.insert(QStringLiteral("feeds_shown"),
                  f == FeedCurrent ? QStringLiteral("current")
                                   : f == FeedHistory ? QStringLiteral("history") : QStringLiteral("both"));
}

bool SettingsStore::valueAxisLeft() const { return m_root.value(QLatin1String("value_axis_left")).toBool(false); }
void SettingsStore::setValueAxisLeft(bool left) { m_root.insert(QStringLiteral("value_axis_left"), left); }
// On by default: the author wants names on the cover (2026-10-16, reversing the privacy default D-C);
// the setting stays so that it can be turned off.
bool SettingsStore::coverShowsName() const { return m_root.value(QLatin1String("cover_shows_name")).toBool(true); }
void SettingsStore::setCoverShowsName(bool on) { m_root.insert(QStringLiteral("cover_shows_name"), on); }
bool SettingsStore::safetyAccepted() const { return m_root.value(QLatin1String("safety_accepted")).toBool(false); }
void SettingsStore::setSafetyAccepted(bool a) { m_root.insert(QStringLiteral("safety_accepted"), a); }

bool SettingsStore::flag(const QString &key) const
{
    return m_root.value(QLatin1String("flags")).toObject().value(key).toBool(false);
}

void SettingsStore::setFlag(const QString &key, bool on)
{
    QJsonObject flags = m_root.value(QLatin1String("flags")).toObject();
    if (on) flags.insert(key, true); else flags.remove(key);
    m_root.insert(QStringLiteral("flags"), flags);
}

} // namespace ui
} // namespace lm
