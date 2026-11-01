// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Michele Brami
#ifndef LM_UI_APPSETTINGS_H
#define LM_UI_APPSETTINGS_H

// SettingsStore made visible to QML: properties with change signals, saved as soon as one
// changes (a settings page has no "OK").

#include "settingsstore.h"

#include <QObject>

namespace lm {
namespace ui {

class AppSettings : public QObject
{
    Q_OBJECT
    Q_PROPERTY(QString displayUnit READ displayUnit WRITE setDisplayUnit NOTIFY changed)
    Q_PROPERTY(bool firstRun READ firstRun NOTIFY changed)
    Q_PROPERTY(int feeds READ feeds WRITE setFeeds NOTIFY changed)
    Q_PROPERTY(bool valueAxisLeft READ valueAxisLeft WRITE setValueAxisLeft NOTIFY changed)
    Q_PROPERTY(bool coverShowsName READ coverShowsName WRITE setCoverShowsName NOTIFY changed)
    Q_PROPERTY(bool safetyAccepted READ safetyAccepted WRITE setSafetyAccepted NOTIFY changed)

public:
    explicit AppSettings(QObject *parent = 0) : QObject(parent) {}

    void load(const QString &path) { m_store.load(path); emit changed(); }

    QString displayUnit() const { return m_store.displayUnit().isEmpty() ? QStringLiteral("mmol/L") : m_store.displayUnit(); }
    // True until the user has chosen a unit and accepted the statement (REQ-UI-02, REQ-SAF-01).
    bool firstRun() const { return m_store.displayUnit().isEmpty() || !m_store.safetyAccepted(); }
    int feeds() const { return int(m_store.feedsShown() == SettingsStore::FeedCurrent ? 0 : m_store.feedsShown() == SettingsStore::FeedHistory ? 1 : 2); }
    bool valueAxisLeft() const { return m_store.valueAxisLeft(); }
    bool coverShowsName() const { return m_store.coverShowsName(); }
    bool safetyAccepted() const { return m_store.safetyAccepted(); }

    void setDisplayUnit(const QString &u) { m_store.setDisplayUnit(u); commit(); }
    void setFeeds(int f) { m_store.setFeedsShown(f == 0 ? SettingsStore::FeedCurrent : f == 1 ? SettingsStore::FeedHistory : SettingsStore::FeedBoth); commit(); }
    void setValueAxisLeft(bool v) { m_store.setValueAxisLeft(v); commit(); }
    void setCoverShowsName(bool v) { m_store.setCoverShowsName(v); commit(); }
    void setSafetyAccepted(bool v) { m_store.setSafetyAccepted(v); commit(); }

signals:
    void changed();

private:
    void commit() { m_store.save(); emit changed(); }

    SettingsStore m_store;
};

} // namespace ui
} // namespace lm

#endif
