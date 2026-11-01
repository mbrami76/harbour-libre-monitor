// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Michele Brami
#include "monitoringinfo.h"

namespace lm {
namespace ui {

MonitoringInfo::MonitoringInfo(QObject *parent) : QObject(parent), m_backend(0) {}

void MonitoringInfo::setBackend(Backend *backend)
{
    if (m_backend)
        disconnect(m_backend, 0, this, 0);
    m_backend = backend;
    if (m_backend)
        connect(m_backend, &Backend::changed, this, &MonitoringInfo::changed);
    emit changed();
}

QString MonitoringInfo::status() const
{
    if (!m_backend)
        return QStringLiteral("connecting");
    switch (m_backend->serviceStatus()) {
    case ServiceStatus::Connecting: return QStringLiteral("connecting");
    case ServiceStatus::Running: return QStringLiteral("running");
    case ServiceStatus::Starting: return QStringLiteral("starting");
    case ServiceStatus::Stopped: return QStringLiteral("stopped");
    case ServiceStatus::AwaitingApproval: return QStringLiteral("approval");
    case ServiceStatus::CallFailed: return QStringLiteral("call-failed");
    case ServiceStatus::ServiceOlder: return QStringLiteral("service-older");
    case ServiceStatus::ServiceNewer: return QStringLiteral("service-newer");
    }
    return QStringLiteral("call-failed");
}

qlonglong MonitoringInfo::lastHeartbeatMs() const
{
    return m_backend ? m_backend->lastHeartbeatMs() : 0;
}

void MonitoringInfo::startMonitoring() { if (m_backend) m_backend->startMonitoring(); }
void MonitoringInfo::stopMonitoring() { if (m_backend) m_backend->stopMonitoring(); }
void MonitoringInfo::askAgain() { if (m_backend) m_backend->askAgain(); }

} // namespace ui
} // namespace lm
