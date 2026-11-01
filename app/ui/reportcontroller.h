// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Michele Brami
#ifndef LM_UI_REPORTCONTROLLER_H
#define LM_UI_REPORTCONTROLLER_H

// The diagnostic report dialog (REQ-GRD-15;
//).
//
// After the second guard stop of an account in a row the service proposes a report for the developer
// (`report_proposed` in Status()). The dialog says in words what the file holds and asks for consent;
// only then the controller asks the service for the text (`DiagnosticReport(connection)`, which holds NO
// measurement value: every value and unit is the word "number"), writes it unchanged as a
// plain-text JSON Lines file to the phone's documents folder, shows the path and the developer's address,
// and tells the service that the proposal was handled (`ReportHandled(connection)`). "Not now" tells the
// service the same without a file. The application sends nothing.
//
// The service's error words: `not-proposed` (the proposal has ended meanwhile: nothing to do),
// `unknown-connection`, `service-not-running` (try again when monitoring runs), and `no-report` (a
// proposal without details, for example after a restart of the service): the dialog says "no details are
// available; the report will be offered again after the next problems" and ends the proposal, so that it
// returns only after two new guard stops.
//
// The text is never logged and never kept in memory after it was written.

#include "maintenancebackend.h"

#include <QObject>
#include <QString>

namespace lm {
namespace ui {

// The error word of an answer of DiagnosticReport, or empty when the answer is a report. An answer that
// is neither (empty, not JSON) is "malformed".
QString reportAnswerError(const QString &text);
// Writes the text unchanged to "<folder>/libre-monitor-report-<UTC YYYYMMDDTHHMMSSZ>.jsonl", never over an
// existing file (a "-2", "-3" ... is added). False when it cannot be written; *path is the file written.
bool writeReportFile(const QString &folder, const QString &text, qint64 nowMs, QString *path);

class ReportController : public QObject
{
    Q_OBJECT
    // "consent", "waiting", "written", "error", "declined"
    Q_PROPERTY(QString stage READ stage NOTIFY changed)
    Q_PROPERTY(QString errorKey READ errorKey NOTIFY changed)
    Q_PROPERTY(QString path READ path NOTIFY changed)
    Q_PROPERTY(QString folder READ folder CONSTANT)
    // The developer's address: one source, the same as the About page (the build define LM_DEVELOPER_CONTACT,
    // from the spec's Packager line since 0.1-3; empty only in a build without common.pri).
    Q_PROPERTY(QString developerContact READ developerContact CONSTANT)

public:
    explicit ReportController(QObject *parent = 0);
    void setBackend(MaintenanceBackend *backend);
    void setFolder(const QString &folder) { m_folder = folder; }
    void setDeveloperContact(const QString &contact) { m_contact = contact; }
    void setNowMs(qint64 nowMs) { m_fixedNowMs = nowMs; }

    QString stage() const { return m_stage; }
    QString errorKey() const { return m_errorKey; }
    QString path() const { return m_path; }
    QString folder() const { return m_folder; }
    QString developerContact() const { return m_contact; }

    // The dialog opened for the connection of a patient.
    Q_INVOKABLE void open(const QString &connection);
    // Consent given: ask for the text, then write it.
    Q_INVOKABLE void write();
    // "Not now": the proposal ends without a file.
    Q_INVOKABLE void decline();

signals:
    void changed();

private slots:
    void onAnswered(bool ok, const QString &connection, const QString &text);

private:
    void fail(const QString &key);

    MaintenanceBackend *m_backend;
    QString m_connection, m_stage, m_errorKey, m_path, m_folder, m_contact;
    qint64 m_fixedNowMs;
};

} // namespace ui
} // namespace lm

#endif
