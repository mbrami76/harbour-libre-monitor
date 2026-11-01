// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Michele Brami
#include "realalertsbackend.h"

#include "daemon/dbusservice.h"
#include "ui/alertview.h"
#include "ui/thresholdview.h"

namespace lm {
namespace real {

namespace {
// The service's signals come in bursts (an alert raised changes the slot, the resident notice and,
// with silent mode, a second slot within a moment): one `Alerts()` call after this pause.
const int kAlertsCoalesceMs = 200;
}

RealAlertsBackend::RealAlertsBackend(ServiceLink *link, QObject *parent)
    : ui::AlertsBackend(parent), m_link(link)
{
    m_coalesce.setSingleShot(true);
    m_coalesce.setInterval(kAlertsCoalesceMs);
    connect(&m_coalesce, &QTimer::timeout, this, &RealAlertsBackend::refresh);

    connect(m_link, &ServiceLink::alertsAnswered, this, &RealAlertsBackend::onAlertsAnswered);
    // The reason of a failure arrives just before the failed answer. Only "unknown method" is
    // kept, and only for one of the calls this class makes; a reason for another call is not ours.
    connect(m_link, &ServiceLink::callFailed, this, [this](const QString &method, const QString &errorName) {
        static const char *const ours[] = { "Alerts", "AcknowledgeAlert", "SnoozeAlert", "SetSilent",
                                            "EndSilent", "SetThresholds" };
        if (!ServiceClient::isOlderServiceError(errorName))
            return;
        for (unsigned i = 0; i < sizeof(ours) / sizeof(ours[0]); ++i)
            if (method == QLatin1String(ours[i]))
                m_olderFailure = method;
    });
    // The arguments of the signals are not used: any of them means "the alerts changed", and the one
    // `Alerts()` answer after the pause tells everything (and is the only source of the facts shown).
    connect(m_link, &ServiceLink::alertChanged, this, [this](const QString &, const QString &, const QString &) { onSignal(); });
    connect(m_link, &ServiceLink::silentChanged, this, [this](const QString &, bool, const QString &) { onSignal(); });
    connect(m_link, &ServiceLink::reachableChanged, this, [this](bool reachable) {
        if (reachable) {
            onSignal();
        } else {
            // The service went away: what it said is no longer known to be true.
            m_facts = ui::AlertsFacts();
            m_facts.error = QStringLiteral("service-not-running");
            emit changed();
        }
    });

    connect(m_link, &ServiceLink::acknowledgeAnswered, this,
            [this](bool ok, const QString &subject, const QString &, const QString &result) {
        answer(QStringLiteral("acknowledge"), subject, ok, result, "AcknowledgeAlert");
    });
    connect(m_link, &ServiceLink::snoozeAnswered, this,
            [this](bool ok, const QString &subject, const QString &, const QString &result) {
        answer(QStringLiteral("snooze"), subject, ok, result, "SnoozeAlert");
    });
    connect(m_link, &ServiceLink::setSilentAnswered, this, [this](bool ok, const QString &subject, const QByteArray &json) {
        answer(QStringLiteral("silent"), subject, ok, ok ? ui::parseSilentAnswer(json, 0) : QString(), "SetSilent");
    });
    connect(m_link, &ServiceLink::endSilentAnswered, this, [this](bool ok, const QString &subject, const QString &result) {
        answer(QStringLiteral("end-silent"), subject, ok, result, "EndSilent");
    });
    // The thresholds' answer carries numbers (the allowed ranges, the standard values), so it is passed on
    // read, not as a word. The new limits in force then come with the next Alerts() (asked after the pause).
    connect(m_link, &ServiceLink::setThresholdsAnswered, this, [this](bool ok, const QString &subject, const QByteArray &json) {
        ui::ThresholdAnswer a;
        if (ok)
            a = ui::parseThresholdAnswer(json);
        else
            a.result = failureWord("SetThresholds");
        emit thresholdsAnswered(subject, a);
        onSignal();
    });
}

void RealAlertsBackend::refresh()
{
    m_coalesce.stop();
    m_link->alerts();
}

void RealAlertsBackend::onSignal()
{
    if (!m_coalesce.isActive())
        m_coalesce.start();
}

void RealAlertsBackend::onAlertsAnswered(bool ok, const QByteArray &json)
{
    if (!ok) {
        m_facts = ui::AlertsFacts();
        m_facts.error = failureWord("Alerts");
    } else {
        m_facts = ui::parseAlertsJson(json);
    }
    emit changed();
}

QString RealAlertsBackend::failureWord(const char *method)
{
    const bool older = m_olderFailure == QLatin1String(method);
    m_olderFailure.clear();
    return older ? QStringLiteral("service-older") : QStringLiteral("call-failed");
}

void RealAlertsBackend::answer(const QString &action, const QString &subject, bool ok, const QString &result,
                               const char *method)
{
    emit answered(action, subject, ok ? result : failureWord(method));
    // The service also signals the change; asking now as well keeps the page right if a signal was lost.
    onSignal();
}

void RealAlertsBackend::acknowledge(const QString &subject, const QString &slot) { m_link->acknowledgeAlert(subject, slot); }
void RealAlertsBackend::snooze(const QString &subject, const QString &slot, int minutes) { m_link->snoozeAlert(subject, slot, minutes); }
void RealAlertsBackend::setSilent(const QString &subject, int minutes) { m_link->setSilent(subject, minutes); }
void RealAlertsBackend::endSilent(const QString &subject) { m_link->endSilent(subject); }
void RealAlertsBackend::setThresholds(const QString &subject, int l, int h, int typedL, int typedH)
{
    m_link->setThresholds(subject, l, h, typedL, typedH);
}

} // namespace real
} // namespace lm
