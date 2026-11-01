// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Michele Brami
#include "reportcontroller.h"

#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSaveFile>

namespace lm {
namespace ui {

QString reportAnswerError(const QString &text)
{
    const QString t = text.trimmed();
    if (t.isEmpty())
        return QStringLiteral("malformed");
    // An error answer is one JSON object with an "error" key; a report
    // is several lines, the first with "line_type":"report".
    if (!t.contains(QLatin1Char('\n'))) {
        const QJsonDocument d = QJsonDocument::fromJson(t.toUtf8());
        if (!d.isObject())
            return QStringLiteral("malformed");
        if (d.object().contains(QStringLiteral("error")))
            return d.object().value(QStringLiteral("error")).toString(QStringLiteral("malformed"));
    }
    const QJsonDocument first = QJsonDocument::fromJson(t.section(QLatin1Char('\n'), 0, 0).toUtf8());
    if (!first.isObject() || first.object().value(QStringLiteral("line_type")).toString() != QLatin1String("report"))
        return QStringLiteral("malformed");
    return QString();
}

bool writeReportFile(const QString &folder, const QString &text, qint64 nowMs, QString *path)
{
    if (folder.isEmpty() || !QDir().mkpath(folder))
        return false;
    const QString stamp = QDateTime::fromMSecsSinceEpoch(nowMs, Qt::UTC).toString(QStringLiteral("yyyyMMdd'T'HHmmss'Z'"));
    const QString base = QDir(folder).filePath(QStringLiteral("libre-monitor-report-%1").arg(stamp));
    QString name = base + QStringLiteral(".jsonl");
    for (int n = 2; QFile::exists(name) && n < 100; ++n)
        name = base + QStringLiteral("-%1.jsonl").arg(n);
    if (QFile::exists(name))
        return false;
    QSaveFile f(name);
    if (!f.open(QIODevice::WriteOnly | QIODevice::Text))
        return false;
    QByteArray bytes = text.toUtf8();
    if (!bytes.endsWith('\n'))
        bytes.append('\n');
    if (f.write(bytes) != bytes.size() || !f.commit())
        return false;
    *path = name;
    return true;
}

ReportController::ReportController(QObject *parent)
    : QObject(parent), m_backend(0), m_stage(QStringLiteral("consent")), m_fixedNowMs(0)
{
}

void ReportController::setBackend(MaintenanceBackend *backend)
{
    if (m_backend)
        disconnect(m_backend, 0, this, 0);
    m_backend = backend;
    if (m_backend)
        connect(m_backend, &MaintenanceBackend::diagnosticReportAnswered, this, &ReportController::onAnswered);
}

void ReportController::open(const QString &connection)
{
    m_connection = connection;
    m_stage = QStringLiteral("consent");
    m_errorKey.clear();
    m_path.clear();
    emit changed();
}

void ReportController::write()
{
    if (!m_backend || m_connection.isEmpty() || m_stage == QLatin1String("waiting"))
        return;
    m_stage = QStringLiteral("waiting");
    emit changed();
    m_backend->requestDiagnosticReport(m_connection);
}

void ReportController::decline()
{
    if (m_backend && !m_connection.isEmpty())
        m_backend->reportHandled(m_connection);
    m_stage = QStringLiteral("declined");
    emit changed();
}

void ReportController::fail(const QString &key)
{
    m_errorKey = key;
    m_stage = QStringLiteral("error");
    emit changed();
}

void ReportController::onAnswered(bool ok, const QString &connection, const QString &text)
{
    if (connection != m_connection || m_stage != QLatin1String("waiting"))
        return;
    if (!ok) {
        fail(QStringLiteral("ui-report-err-no-answer"));
        return;
    }
    const QString error = reportAnswerError(text);
    if (!error.isEmpty()) {
        if (error == QLatin1String("no-report") && m_backend)
            m_backend->reportHandled(m_connection);     // offered again after the next problems
        if (error == QLatin1String("not-proposed") || error == QLatin1String("unknown-connection")
            || error == QLatin1String("no-report") || error == QLatin1String("service-not-running"))
            fail(QStringLiteral("ui-report-err-") + error);
        else
            fail(QStringLiteral("ui-report-err-malformed"));
        return;
    }
    QString path;
    const qint64 now = m_fixedNowMs > 0 ? m_fixedNowMs : QDateTime::currentMSecsSinceEpoch();
    if (!writeReportFile(m_folder, text, now, &path)) {
        fail(QStringLiteral("ui-report-err-write"));   // the proposal stays: the user can try again
        return;
    }
    m_path = path;
    if (m_backend)
        m_backend->reportHandled(m_connection);
    m_stage = QStringLiteral("written");
    emit changed();
}

} // namespace ui
} // namespace lm
