// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Michele Brami
#include "fakemaintenancebackend.h"

#include <QDateTime>
#include <QJsonDocument>
#include <QJsonObject>
#include <QTimer>

namespace lm {
namespace ui {

FakeMaintenanceBackend::FakeMaintenanceBackend(QObject *parent)
    : MaintenanceBackend(parent), m_autoPlay(true), m_running(false), m_stepDone(0)
{
    // Invented figures for the emulator: a store of 30 MB, 2 GB free, a weekly check a day ago.
    m_facts.ok = true;
    m_facts.storeBytes = 30LL * 1024 * 1024;
    m_facts.journalBytes = 2LL * 1024 * 1024;
    m_facts.freeBytes = 2LL * 1024 * 1024 * 1024;
    m_facts.checkKnown = true;
    m_facts.checkTimeMs = QDateTime::currentMSecsSinceEpoch() - 24LL * 3600 * 1000;
    m_facts.estimateKnown = true;
}

void FakeMaintenanceBackend::setSpaceScript(const QString &script)
{
    m_spaceScript = script;
    if (script == QLatin1String("low"))
        m_facts.checkLow = true;
}

void FakeMaintenanceBackend::setRunning(bool running, const QString &kind)
{
    m_running = running;
    m_kind = running ? kind : QString();
    emit runningChanged();
}

void FakeMaintenanceBackend::requestStorageFacts(int windowMonths)
{
    calls << QStringLiteral("facts:%1").arg(windowMonths);
    StorageFacts f = m_facts;
    f.windowMonths = windowMonths;
    if (f.ok && f.estimateKnown && f.storeBytes > 0) {
        // A shorter window frees more (REQ-STO-16): invented proportions.
        const int m = windowMonths <= 0 ? 0 : windowMonths;
        const double share = m == 0 ? 0.9 : m == 1 ? 0.7 : m == 3 ? 0.4 : m == 6 ? 0.2 : 0.05;
        f.estimateBytes = qint64(f.storeBytes * share);
        f.estimateRecords = f.estimateBytes / 120;
        // The shape of the core's rule (what remains plus a tenth, at least 1 MiB); invented here,
        // the real backend takes the core's own figure.
        const qint64 remains = f.storeBytes - f.estimateBytes;
        f.rebuildNeedBytes = remains + qMax<qint64>(remains / 10, 1024 * 1024);
        if (m_spaceScript == QLatin1String("unknown"))
            f.freeBytes = -1;
        else if (m_spaceScript == QLatin1String("short"))
            f.freeBytes = f.rebuildNeedBytes - 5LL * 1024 * 1024;
        f.space = f.freeBytes < 0 ? QStringLiteral("unknown")
                : f.freeBytes >= f.rebuildNeedBytes ? QStringLiteral("enough") : QStringLiteral("short");
        f.missingBytes = f.space == QLatin1String("short") ? f.rebuildNeedBytes - f.freeBytes : 0;
    }
    if (m_autoPlay)
        QTimer::singleShot(0, this, [this, f]() { emit storageFactsLoaded(f); });
    else
        emit storageFactsLoaded(f);
}

void FakeMaintenanceBackend::startMaintenance(const QString &kind, const QString &parametersJson)
{
    calls << QStringLiteral("start:%1:%2").arg(kind, parametersJson);
    if (!m_startAnswer.isEmpty()) {
        emit maintenanceAnswered(true, kind, m_startAnswer);
        return;
    }
    m_parameters = parametersJson;
    setRunning(true, kind);
    emit maintenanceAnswered(true, kind, QString());
    if (m_autoPlay) {
        m_stepDone = 0;
        const bool purge = kind.startsWith(QLatin1String("purge"));
        QTimer::singleShot(purge ? 400 : 600, this, [this, purge]() { if (purge) playPurge(); else playStep(); });
    }
}

// The details of the service's purge job, invented figures.
QString FakeMaintenanceBackend::purgeDetails(bool result, bool storePartDone) const
{
    const QJsonObject p = QJsonDocument::fromJson(m_parameters.toUtf8()).object();
    const bool nothing = m_purgeScript == QLatin1String("nothing");
    QJsonObject store, journal;
    store.insert(QStringLiteral("record"), nothing ? 0 : 52000);
    store.insert(QStringLiteral("mark"), nothing ? 0 : 3);
    store.insert(QStringLiteral("sensor"), nothing ? 0 : 4);
    store.insert(QStringLiteral("quarantine"), 0);
    store.insert(QStringLiteral("ingest"), nothing ? 0 : 120);
    journal.insert(QStringLiteral("alert"), nothing ? 0 : 41);
    journal.insert(QStringLiteral("threshold"), nothing ? 0 : 2);
    journal.insert(QStringLiteral("silent"), nothing ? 0 : 5);
    QJsonObject d;
    d.insert(QStringLiteral("subject"), p.value(QStringLiteral("subject")).toString());
    d.insert(QStringLiteral("age_months"), p.value(QStringLiteral("age_months")).toInt());
    d.insert(QStringLiteral("store_rows"), store);
    d.insert(QStringLiteral("journal_entries"), journal);
    d.insert(QStringLiteral("total_bytes_estimated"), nothing ? 0.0 : 9.5 * 1024 * 1024);
    const double needed = 21.0 * 1024 * 1024;
    d.insert(QStringLiteral("needed_bytes"), needed);
    const bool unknown = m_purgeScript == QLatin1String("unknown"), shortSpace = m_purgeScript == QLatin1String("short");
    d.insert(QStringLiteral("free_bytes"), unknown ? -1.0 : shortSpace ? needed - 6.0 * 1024 * 1024 : 2.0 * 1024 * 1024 * 1024);
    d.insert(QStringLiteral("space"), unknown ? QStringLiteral("unknown") : shortSpace ? QStringLiteral("short") : QStringLiteral("enough"));
    d.insert(QStringLiteral("missing_bytes"), shortSpace ? 6.0 * 1024 * 1024 : 0.0);
    if (result) {
        d.insert(QStringLiteral("store_part_done"), storePartDone);
        d.insert(QStringLiteral("removed"), store);
        d.insert(QStringLiteral("journal_removed"), storePartDone && m_purgeJournalFailed ? QJsonObject() : journal);
        d.insert(QStringLiteral("bytes_freed"), 9.0 * 1024 * 1024);
        d.insert(QStringLiteral("journal_bytes_freed"), 64.0 * 1024);
    }
    return QString::fromUtf8(QJsonDocument(d).toJson(QJsonDocument::Compact));
}

void FakeMaintenanceBackend::playPurge()
{
    if (!m_running)
        return;
    const QString kind = m_kind;
    if (kind == QLatin1String("purge-estimate")) {
        setRunning(false, QString());
        emit maintenanceFinished(kind, true, QString(), QString(), QString(), purgeDetails(false, false));
        return;
    }
    ++m_stepDone;
    static const char *phases[] = { "checking-space", "purging", "finalizing" };
    const int pause = m_purgeScript == QLatin1String("slow") ? 4000 : 600;
    if (m_stepDone <= 3) {
        emit maintenanceProgress(kind, QString::fromLatin1(phases[m_stepDone - 1]), m_stepDone, 3,
                                 m_stepDone * 100, 300, m_stepDone / 3.0);
        QTimer::singleShot(pause, this, [this]() { playPurge(); });
        return;
    }
    setRunning(false, QString());
    if (m_purgeScript == QLatin1String("repeat") && !m_purgeJournalFailed) {
        // the store part is done, the journal part failed; the same purge can be repeated.
        m_purgeJournalFailed = true;
        emit maintenanceFinished(kind, false, QStringLiteral("io"),
                                 QStringLiteral("the store part is done; the journal part failed and can be repeated"),
                                 QString(), purgeDetails(true, true));
        return;
    }
    emit maintenanceProgress(kind, QStringLiteral("done"), 3, 3, 300, 300, 1.0);
    emit maintenanceFinished(kind, true, QString(), QString(), QString(), purgeDetails(true, true));
}

void FakeMaintenanceBackend::playStep()
{
    if (!m_running)
        return;
    ++m_stepDone;
    // The service's phases of a compaction (core/store/compactor.h), 3 steps; the space check refuses
    // first when the script says so, as the service does (the store unchanged).
    static const char *phases[] = { "checking-space", "compacting", "finalizing" };
    const QString kind = m_kind;
    QJsonObject d;
    if (m_stepDone == 2 && (m_spaceScript == QLatin1String("short") || m_spaceScript == QLatin1String("unknown"))) {
        const bool unknown = m_spaceScript == QLatin1String("unknown");
        d.insert(QStringLiteral("space"), unknown ? QStringLiteral("unknown") : QStringLiteral("short"));
        d.insert(QStringLiteral("missing_bytes"), unknown ? 0.0 : 5.0 * 1024 * 1024);
        setRunning(false, QString());
        emit maintenanceFinished(kind, false, unknown ? QStringLiteral("free-space-unknown") : QStringLiteral("insufficient-space"),
                                 QString(), QString(), QString::fromUtf8(QJsonDocument(d).toJson(QJsonDocument::Compact)));
        return;
    }
    if (m_stepDone <= 3) {
        emit maintenanceProgress(kind, QString::fromLatin1(phases[m_stepDone - 1]), m_stepDone, 3,
                                 m_stepDone * 100, 300, m_stepDone / 3.0);
        QTimer::singleShot(600, this, [this]() { playStep(); });
        return;
    }
    emit maintenanceProgress(kind, QStringLiteral("done"), 3, 3, 300, 300, 1.0);
    d.insert(QStringLiteral("removed"), 1000);
    d.insert(QStringLiteral("bytes_freed"), 12.0 * 1024 * 1024);
    d.insert(QStringLiteral("space"), QStringLiteral("enough"));
    setRunning(false, QString());
    emit maintenanceFinished(kind, true, QString(), QString(), QString(),
                             QString::fromUtf8(QJsonDocument(d).toJson(QJsonDocument::Compact)));
}

void FakeMaintenanceBackend::cancelMaintenance()
{
    calls << QStringLiteral("cancel");
    if (!m_running)
        return;
    const QString kind = m_kind;
    setRunning(false, QString());
    emit maintenanceFinished(kind, false, QStringLiteral("cancelled"), QString(), QStringLiteral("user"), QStringLiteral("{}"));
}

QString FakeMaintenanceBackend::syntheticReport()
{
    // The service's shape, invented content, values already "number".
    return QStringLiteral(
        "{\"line_type\":\"report\",\"format\":1,\"application_version\":\"0.1-2\",\"adapter_id\":\"libre\","
        "\"adapter_version\":\"0.1\",\"backend_version\":\"\",\"command\":\"fetch_current\","
        "\"violations\":[{\"kind\":\"schema\",\"detail\":\"synthetic\"}],\"counts\":{\"batches\":2,\"records_received\":3,"
        "\"records_valid\":1,\"soft_violations\":0,\"hard_violations\":2,\"quarantined_by_reason\":{}}}\n"
        "{\"line_type\":\"batch\",\"status\":\"ok\"}\n"
        "{\"line_type\":\"record\",\"index\":0,\"value\":\"number\",\"unit\":\"number\"}\n");
}

void FakeMaintenanceBackend::requestDiagnosticReport(const QString &connection)
{
    calls << QStringLiteral("report:%1").arg(connection);
    const QString text = m_reportError.isEmpty() ? syntheticReport()
        : QStringLiteral("{\"format\":1,\"error\":\"%1\"}").arg(m_reportError);
    if (m_autoPlay)
        QTimer::singleShot(300, this, [this, connection, text]() { emit diagnosticReportAnswered(true, connection, text); });
    else
        emit diagnosticReportAnswered(true, connection, text);
}

void FakeMaintenanceBackend::reportHandled(const QString &connection)
{
    calls << QStringLiteral("handled:%1").arg(connection);
}

} // namespace ui
} // namespace lm
