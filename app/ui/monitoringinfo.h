// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Michele Brami
#ifndef LM_UI_MONITORINGINFO_H
#define LM_UI_MONITORINGINFO_H

// The monitoring-state strip as the QML sees it (REQ-UI-10): is the
// monitoring service running, stopped, starting, waiting for the user's approval, or not
// answering. The strip shows a pictogram and a text for each (REQ-UI-04); this class only
// names the state and forwards the two buttons.
//
// Pattern: a thin read-only adapter over Backend, refreshed when the backend says it changed.
// It decides nothing about glucose.

#include "backend.h"

#include <QObject>

namespace lm {
namespace ui {

class MonitoringInfo : public QObject
{
    Q_OBJECT
    // "connecting", "running", "starting", "stopped", "approval", "call-failed", and "service-older" (the service runs but is older than this window or names no version), "service-newer" (the service is newer than this window)
    Q_PROPERTY(QString status READ status NOTIFY changed)
    Q_PROPERTY(qlonglong lastHeartbeatMs READ lastHeartbeatMs NOTIFY changed)

public:
    explicit MonitoringInfo(QObject *parent = 0);

    void setBackend(Backend *backend);
    QString status() const;
    qlonglong lastHeartbeatMs() const;

    Q_INVOKABLE void startMonitoring();
    // The Stop button of the "service-older" state (only that state shows it; Backend::stopMonitoring).
    Q_INVOKABLE void stopMonitoring();
    Q_INVOKABLE void askAgain();

signals:
    void changed();

private:
    Backend *m_backend;
};

} // namespace ui
} // namespace lm

#endif
