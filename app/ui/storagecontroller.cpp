// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Michele Brami
#include "storagecontroller.h"

#include <QVariant>

namespace lm {
namespace ui {

namespace {
const char *kCompaction = "compaction";
}

StorageController::StorageController(QObject *parent)
    : QObject(parent), m_backend(0), m_factsLoaded(false), m_windowMonths(kStandardWindowMonths),
      m_starting(false), m_fraction(-1), m_step(0), m_steps(0), m_resultOk(false), m_lost(0)
{
    // canCompact depends on the facts too: one signal for the page's button.
    connect(this, &StorageController::factsChanged, this, &StorageController::stateChanged);
}

void StorageController::setBackend(MaintenanceBackend *backend)
{
    if (m_backend)
        disconnect(m_backend, 0, this, 0);
    m_backend = backend;
    if (!m_backend)
        return;
    connect(m_backend, &MaintenanceBackend::storageFactsLoaded, this, &StorageController::onFacts);
    connect(m_backend, &MaintenanceBackend::maintenanceAnswered, this, &StorageController::onAnswered);
    connect(m_backend, &MaintenanceBackend::maintenanceProgress, this, &StorageController::onProgress);
    connect(m_backend, &MaintenanceBackend::maintenanceFinished, this, &StorageController::onFinished);
    connect(m_backend, &MaintenanceBackend::maintenanceWarning, this, &StorageController::onWarning);
    connect(m_backend, &MaintenanceBackend::runningChanged, this, &StorageController::stateChanged);
}

bool StorageController::canCompact() const
{
    // REQ-STO-16: the core's check says the rebuild fits; "short" and "unknown" are refused by the service
    // without an override, so they are not offered. An estimate that removes nothing is not offered either.
    return m_backend && !running() && estimateKnown() && m_facts.estimateRecords > 0
           && m_facts.space == QLatin1String("enough");
}

QVariantList StorageController::windowChoicesList() const
{
    QVariantList out;
    const QList<int> c = windowChoices();
    for (int i = 0; i < c.size(); ++i)
        out << c.at(i);
    return out;
}

void StorageController::setWindowMonths(int months)
{
    if (!windowChoices().contains(months) || months == m_windowMonths)
        return;
    m_windowMonths = months;
    emit factsChanged();
    refresh();
}

bool StorageController::running() const
{
    return m_starting || (m_backend && m_backend->maintenanceRunning());
}

QString StorageController::runningKind() const
{
    if (m_backend && m_backend->maintenanceRunning())
        return m_backend->maintenanceKind();
    return m_starting ? QString::fromLatin1(kCompaction) : QString();
}

void StorageController::refresh()
{
    if (m_backend)
        m_backend->requestStorageFacts(m_windowMonths);
}

void StorageController::onFacts(const StorageFacts &facts)
{
    if (facts.windowMonths != m_windowMonths && m_factsLoaded)
        return;                         // an answer for a window the user has left meanwhile
    m_facts = facts;
    m_factsLoaded = true;
    emit factsChanged();
}

void StorageController::setResult(const QString &key, const QString &arg, bool ok)
{
    m_resultKey = key;
    m_resultArg = arg;
    m_resultOk = ok;
}

bool StorageController::compact()
{
    if (!canCompact())
        return false;
    m_starting = true;
    m_phaseKey.clear();
    m_fraction = -1;
    m_step = m_steps = 0;
    setResult(QString(), QString(), false);
    emit stateChanged();
    m_backend->startMaintenance(QString::fromLatin1(kCompaction), compactionParameters(m_windowMonths));
    return true;
}

void StorageController::cancel()
{
    if (m_backend && running())
        m_backend->cancelMaintenance();
}

void StorageController::dismissResult()
{
    setResult(QString(), QString(), false);
    m_lost = 0;
    emit stateChanged();
}

void StorageController::onAnswered(bool ok, const QString &kind, const QString &result)
{
    if (!m_starting || kind != QLatin1String(kCompaction))
        return;
    m_starting = false;
    if (!ok)
        setResult(startAnswerKey(QStringLiteral("no-answer")), QString(), false);
    else if (!result.isEmpty())
        setResult(startAnswerKey(result), QString(), false);
    emit stateChanged();
}

void StorageController::onProgress(const QString &kind, const QString &phase, int step, int steps, qlonglong,
                                   qlonglong, double fraction)
{
    // Only compaction's progress belongs to this page; a purge or its estimate has its own page (purge
    //): the page still says that maintenance is running (runningKind), with its kind.
    if (kind != QLatin1String(kCompaction))
        return;
    m_starting = false;
    m_phaseKey = lm::ui::phaseKey(phase);
    m_step = step;
    m_steps = steps;
    m_fraction = fraction;
    emit stateChanged();
}

void StorageController::onFinished(const QString &kind, bool ok, const QString &errorClass, const QString &,
                                   const QString &cancelReason, const QString &detailsJson)
{
    // A purge's or an estimate's end is not a compaction result: only the facts are read
    // again, because a purge changes the sizes.
    if (kind != QLatin1String(kCompaction)) {
        if (kind == QLatin1String("purge"))
            refresh();
        return;
    }
    m_starting = false;
    m_phaseKey.clear();
    m_fraction = -1;
    const QString key = finishedKey(kind, ok, errorClass, cancelReason);
    // The argument of the text: the bytes freed after a run, the bytes missing after a refusal for space
    // (the details of the service, REQ-STO-16); none otherwise.
    QString arg;
    const qint64 bytes = detailBytes(detailsJson, ok ? "bytes_freed" : "missing_bytes");
    if ((ok && kind == QLatin1String(kCompaction)) || key == QLatin1String("ui-maint-fail-insufficient-space"))
        arg = bytes >= 0 ? QString::number(bytes) : QString();
    setResult(key, arg, ok);
    emit stateChanged();
    refresh();                                          // sizes, free space, the estimate
}

void StorageController::onWarning(const QString &kind, const QString &detail)
{
    if (kind == QLatin1String("reading-queue-full")) {
        // "count=N": readings that could not be held while the store was busy.
        const QString n = detail.startsWith(QLatin1String("count=")) ? detail.mid(6) : QString();
        m_lost += qMax(1, n.toInt());
        emit stateChanged();
    }
}

} // namespace ui
} // namespace lm
