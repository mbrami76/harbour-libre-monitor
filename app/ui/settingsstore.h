// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Michele Brami
#ifndef LM_UI_SETTINGSSTORE_H
#define LM_UI_SETTINGSSTORE_H

// The user interface's own preferences, apart from the configuration file (
// 8.1): the display unit, which glucose feeds the plot shows, which side the value axis is on,
// whether the cover may show a name, and a few "already told the user" flags.
//
// Why a separate file from the configuration: these hold no personal data (no name, no
// reference), the service never reads them, and a damaged preferences file must not touch the
// monitoring configuration. A missing or unreadable file gives the defaults (the first-run
// state); the settings are not safety-relevant, so they fail open to defaults.

#include <QJsonObject>
#include <QSet>
#include <QString>

namespace lm {
namespace ui {

class SettingsStore
{
public:
    enum Feeds { FeedCurrent, FeedHistory, FeedBoth };

    SettingsStore();

    void load(const QString &path);
    bool save() const;

    // The display unit; empty until the user chose one at the first run (REQ-UI-02).
    QString displayUnit() const;
    void setDisplayUnit(const QString &unit);       // "mg/dL" or "mmol/L"; anything else is ignored

    Feeds feedsShown() const;                        // default: both
    void setFeedsShown(Feeds f);

    bool valueAxisLeft() const;                      // default: right (docs 4.2)
    void setValueAxisLeft(bool left);

    bool coverShowsName() const;                     // default: on (on by default)
    void setCoverShowsName(bool on);

    // The user accepted the supplementary-monitor statement (REQ-SAF-01) at the first run.
    bool safetyAccepted() const;
    void setSafetyAccepted(bool accepted);

    // "Told once" flags keyed by an opaque string (a Subject UUID plus a purpose): the unit
    // change notice (REQ-SUB-05) and the diagnostic report proposal (REQ-GRD-15).
    bool flag(const QString &key) const;
    void setFlag(const QString &key, bool on);

private:
    QString m_path;
    QJsonObject m_root;
};

} // namespace ui
} // namespace lm

#endif
