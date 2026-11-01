// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Michele Brami
#include "purgecontroller.h"
#include "storageview.h"

#include <QVariant>

namespace lm {
namespace ui {

namespace {
const char *kEstimate = "purge-estimate";
const char *kPurge = "purge";
}

PurgeController::PurgeController(QObject *parent)
    : QObject(parent), m_backend(0), m_age(kDefaultPurgeAgeMonths), m_stage(QStringLiteral("idle")),
      m_fraction(-1), m_step(0), m_steps(0), m_resultOk(false)
{
}

void PurgeController::setBackend(MaintenanceBackend *backend)
{
    if (m_backend)
        disconnect(m_backend, 0, this, 0);
    m_backend = backend;
    if (!m_backend)
        return;
    connect(m_backend, &MaintenanceBackend::maintenanceAnswered, this, &PurgeController::onAnswered);
    connect(m_backend, &MaintenanceBackend::maintenanceProgress, this, &PurgeController::onProgress);
    connect(m_backend, &MaintenanceBackend::maintenanceFinished, this, &PurgeController::onFinished);
    connect(m_backend, &MaintenanceBackend::runningChanged, this, &PurgeController::changed);
}

void PurgeController::open(const QString &subjectId, const QString &name, const QString &configuredLabel)
{
    m_subject = subjectId;
    m_name = name;
    m_label = purgeLabelOf(configuredLabel);
    m_age = kDefaultPurgeAgeMonths;
    m_estimate = PurgeFacts();
    m_result = PurgeFacts();
    m_lastTyped.clear();
    m_resultKey.clear();
    m_resultOk = false;
    m_phaseKey.clear();
    m_stage = QStringLiteral("idle");
    emit changed();
    refreshEstimate();
}

QVariantList PurgeController::ageChoicesList() const
{
    QVariantList out;
    const QList<int> c = purgeAgeChoices();
    for (int i = 0; i < c.size(); ++i)
        out << c.at(i);
    return out;
}

void PurgeController::setAgeMonths(int months)
{
    if (!purgeAgeChoices().contains(months) || months == m_age || busy())
        return;
    m_age = months;
    m_estimate = PurgeFacts();          // the estimate was for the other age
    emit changed();
    refreshEstimate();
}

bool PurgeController::busy() const
{
    return !m_pendingKind.isEmpty() || m_stage == QLatin1String("running");
}

bool PurgeController::canContinue() const
{
    return m_backend && !busy() && !(m_backend->maintenanceRunning()) && hasLabel() && m_estimate.known
           && m_estimate.ageMonths == m_age && m_estimate.storeRows + m_estimate.journalEntries > 0
           && m_estimate.space == QLatin1String("enough")
           // While the repeat is offered, the same purge is finished with repeat(), not started anew.
           && m_resultKey != QLatin1String("ui-purge-repeat");
}

bool PurgeController::canRepeat() const
{
    return m_resultKey == QLatin1String("ui-purge-repeat") && !m_lastTyped.isEmpty() && !busy()
           && m_backend && !m_backend->maintenanceRunning();
}

void PurgeController::send(const QString &kind, const QString &parameters)
{
    m_pendingKind = kind;
    m_phaseKey.clear();
    m_fraction = -1;
    m_step = m_steps = 0;
    emit changed();
    m_backend->startMaintenance(kind, parameters);
}

void PurgeController::refreshEstimate()
{
    if (!m_backend || m_subject.isEmpty() || busy())
        return;
    m_stage = QStringLiteral("estimating");
    send(QString::fromLatin1(kEstimate), purgeEstimateParameters(m_subject, m_age));
}

bool PurgeController::purge(const QString &typed)
{
    if (!canContinue() || !typedLabelMatches(typed, m_label))
        return false;
    m_lastTyped = typed;
    m_resultKey.clear();
    m_result = PurgeFacts();
    m_stage = QStringLiteral("starting");
    send(QString::fromLatin1(kPurge), purgeParameters(m_subject, m_age, typed));
    return true;
}

bool PurgeController::repeat()
{
    if (!canRepeat())
        return false;
    m_resultKey.clear();
    m_result = PurgeFacts();
    m_stage = QStringLiteral("starting");
    send(QString::fromLatin1(kPurge), purgeParameters(m_subject, m_age, m_lastTyped));
    return true;
}

void PurgeController::cancel()
{
    if (m_backend && m_stage == QLatin1String("running"))
        m_backend->cancelMaintenance();
}

void PurgeController::onAnswered(bool ok, const QString &kind, const QString &result)
{
    if (m_pendingKind.isEmpty() || kind != m_pendingKind)
        return;
    const bool isPurge = kind == QLatin1String(kPurge);
    if (ok && result.isEmpty()) {
        // Started: the estimate's answer comes with MaintenanceFinished; the purge now runs.
        if (isPurge) {
            m_pendingKind.clear();
            m_stage = QStringLiteral("running");
        }
        emit changed();
        return;
    }
    m_pendingKind.clear();
    m_resultKey = purgeStartKey(ok ? result : QStringLiteral("no-answer"));
    m_resultOk = false;
    m_stage = isPurge ? QStringLiteral("finished") : QStringLiteral("idle");
    emit changed();
}

void PurgeController::onProgress(const QString &kind, const QString &phase, int step, int steps, qlonglong,
                                 qlonglong, double fraction)
{
    if (kind != QLatin1String(kPurge) || m_stage != QLatin1String("running"))
        return;
    m_phaseKey = lm::ui::phaseKey(phase);
    m_step = step;
    m_steps = steps;
    m_fraction = fraction;
    emit changed();
}

void PurgeController::onFinished(const QString &kind, bool ok, const QString &errorClass, const QString &,
                                 const QString &cancelReason, const QString &detailsJson)
{
    const PurgeFacts f = purgeFactsFromDetails(detailsJson);
    if (kind == QLatin1String(kEstimate)) {
        if (m_pendingKind != QLatin1String(kEstimate) || (f.known && f.subject != m_subject))
            return;
        m_pendingKind.clear();
        m_stage = QStringLiteral("idle");
        // The details carry the estimate also when the dry run ended with a refusal (the space check):
        // the page shows the figures and the space line either way.
        if (f.known && f.ageMonths == m_age) {
            m_estimate = f;
            m_stage = QStringLiteral("estimated");
        }
        if (!ok && !f.known && m_resultKey.isEmpty()) { // a purge's result, if any, stays the message
            m_resultKey = purgeFinishedKey(false, errorClass, cancelReason, false);
            m_resultOk = false;
        }
        emit changed();
        return;
    }
    if (kind != QLatin1String(kPurge) || (m_stage != QLatin1String("running") && m_stage != QLatin1String("starting"))
        || (f.known && f.subject != m_subject))
        return;
    m_pendingKind.clear();
    m_phaseKey.clear();
    m_fraction = -1;
    m_result = f;
    m_resultOk = ok;
    m_resultKey = purgeFinishedKey(ok, errorClass, cancelReason, f.storePartDone);
    m_stage = QStringLiteral("finished");
    if (ok)
        m_lastTyped.clear();            // nothing to repeat
    emit changed();
    emit purgeFinished(m_subject);
    // A new estimate shows what is left (nothing, after a complete purge).
    if (m_resultKey != QLatin1String("ui-purge-repeat")) {
        m_estimate = PurgeFacts();
        refreshEstimate();
        m_stage = QStringLiteral("finished");
        emit changed();
    }
}

} // namespace ui
} // namespace lm
