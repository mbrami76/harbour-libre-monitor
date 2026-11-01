// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Michele Brami
#include "runner.h"

#include "credentials.h"

#include <QJsonDocument>

#include <cstdio>

namespace lmcli {

namespace {

void writeOut(const QByteArray &data)
{
    std::fwrite(data.constData(), 1, static_cast<size_t>(data.size()), stdout);
    std::fflush(stdout);
}

void writeErr(const QString &text)
{
    const QByteArray data = text.toUtf8() + '\n';
    std::fwrite(data.constData(), 1, static_cast<size_t>(data.size()), stderr);
    std::fflush(stderr);
}

} // namespace

CliRunner::CliRunner(const Options &options, const QString &adapterPath, QObject *parent)
    : QObject(parent), m_options(options), m_adapterPath(adapterPath), m_link(&m_policy),
      m_finishing(false)
{
    m_link.setRecordSink(this);
    m_link.setQuarantineSink(this);
    QStringList arguments;
    if (!options.baseUrl.isEmpty())
        arguments << QStringLiteral("--base-url") << options.baseUrl;
    m_link.setProgram(adapterPath, arguments);
    if (options.verbose) {
        connect(&m_link, &lm::AdapterLink::diagnostic,
                [](const QString &source, const QString &level, const QString &text) {
                    writeErr(QStringLiteral("[adapter %1%2] %3").arg(source)
                                 .arg(level.isEmpty() ? QString() : QLatin1Char(' ') + level).arg(text));
                });
    }
}

void CliRunner::accept(const lm::LinkId &, const QString &, const QVector<lm::LinkRecord> &records)
{
    for (int i = 0; i < records.size(); ++i)
        m_current.records.append(records.at(i).record);
}

void CliRunner::reject(const lm::LinkId &, const QString &, const QString &, const QJsonObject &)
{
    // Counted by the driver; the program has no store, so the record is only counted.
}

void CliRunner::finish(int code, const QString &message)
{
    if (m_finishing)
        return;
    m_finishing = true;
    if (!message.isEmpty())
        writeErr(message);
    const QList<lm::Violation> v = m_policy.violations();
    if (!v.isEmpty()) {
        QStringList kinds;
        int hard = 0;
        for (int i = 0; i < v.size(); ++i) {
            hard += v.at(i).severity == lm::Severity::Hard;
            const QString name = lm::violationKindName(v.at(i).kind);
            if (!kinds.contains(name))
                kinds << name;
        }
        writeErr(QStringLiteral("contract violations: %1 hard, %2 soft (%3)")
                     .arg(hard).arg(v.size() - hard).arg(kinds.join(QStringLiteral(", "))));
    }
    m_link.stop([this, code]() { emit finished(code); });
}

void CliRunner::failedCommand(const lm::CommandReport &report)
{
    // A hard contract violation makes the policy (NullPolicy) answer `kill`, so the driver
    // has already ended the command with protocol-changed and killed_by guard (or
    // driver-limit for an over-long line). That is exit 21, not a plain protocol-changed
    // There is no polling of the policy any more.
    if (report.outcome.killedBy == lm::CommandOutcome::Guard
        || report.outcome.killedBy == lm::CommandOutcome::DriverLimit) {
        finish(ExitContractViolation,
               QStringLiteral("error: the adapter broke the contract (hard violation)"));
        return;
    }
    const lm::ErrorClass cls = report.outcome.errorClass;
    QString text = QStringLiteral("error: %1").arg(lm::errorClassName(cls));
    if (!report.detail.isEmpty())
        text += QStringLiteral(": ") + report.detail;
    if (report.outcome.retryAfterS >= 0)
        text += QStringLiteral(" (retry after %1 s)").arg(report.outcome.retryAfterS);
    if (report.outcome.killedBy == lm::CommandOutcome::Deadline)
        text += QStringLiteral(" [deadline expired, adapter killed]");
    finish(exitCodeFor(cls), text);
}

void CliRunner::run()
{
    QString error;
    if (!m_options.baseUrl.isEmpty())
        writeErr(QStringLiteral("SIMULATED SOURCE: --base-url is set, no real data"));

    if (!m_options.credentialsFile.isEmpty()) {
        if (!readCredentialsFile(m_options.credentialsFile, &m_credentials, &error)) {
            finish(ExitUsage, QStringLiteral("error: credentials file: ") + error);
            return;
        }
    } else if (!promptCredentials(&m_credentials, &error)) {
        finish(ExitUsage, QStringLiteral("error: ") + error);
        return;
    }
    if (!m_options.sessionFile.isEmpty()
        && !readSessionFile(m_options.sessionFile, &m_session, &error)) {
        finish(ExitUsage, QStringLiteral("error: session file: ") + error);
        return;
    }

    m_link.start(QStringLiteral("libre"), QStringLiteral("cli"), QJsonObject(),
                 [this](const lm::StartResult &r, const QString &detail) {
                     if (r.kind == lm::StartResult::Admitted) {
                         m_link.authenticate(m_credentials, m_session,
                                             [this](const lm::CommandReport &rep, const lm::SessionGrant &g) {
                                                 if (rep.outcome.result != lm::CommandOutcome::Ok) {
                                                     failedCommand(rep);
                                                     return;
                                                 }
                                                 authenticated(g);
                                             });
                         return;
                     }
                     switch (r.refusal) {
                     case lm::StartResult::AdmissionRejected:
                         finish(ExitAdmissionRejected,
                                QStringLiteral("error: admission rejected (%1): %2").arg(r.rejectReason).arg(detail));
                         break;
                     case lm::StartResult::HelloTimeout:
                         finish(ExitTransient, QStringLiteral("error: the adapter did not say hello in time"));
                         break;
                     default:
                         // A hard violation while starting (for example a bad hello) kills the
                         // adapter before any command exists, so there is no outcome to look at.
                         if (m_policy.sawHardViolation())
                             finish(ExitContractViolation,
                                    QStringLiteral("error: the adapter broke the contract (hard violation)"));
                         else
                             finish(ExitFailure, QStringLiteral("error: the adapter could not be started: ") + detail);
                         break;
                     }
                 });
}

void CliRunner::authenticated(const lm::SessionGrant &grant)
{
    // Credentials are not needed any more.
    m_credentials = lm::Credentials();
    if (!m_options.sessionFile.isEmpty() && !grant.session.isEmpty()) {
        QString error;
        if (!writeSessionFile(m_options.sessionFile, grant.session, &error))
            writeErr(QStringLiteral("warning: session file: ") + error);
    }
    m_link.discoverSubjects([this](const lm::CommandReport &rep, const QList<lm::SubjectInfo> &subjects) {
        if (rep.outcome.result != lm::CommandOutcome::Ok) {
            failedCommand(rep);
            return;
        }
        discovered(subjects);
    });
}

void CliRunner::discovered(const QList<lm::SubjectInfo> &subjects)
{
    m_subjects = subjects;
    if (m_options.command == QLatin1String("discover")) {
        if (m_options.json)
            writeOut(QJsonDocument(discoverJson(subjects, m_options.showRef)).toJson(QJsonDocument::Compact) + '\n');
        else
            writeOut(formatDiscover(subjects).toUtf8());
        finish(ExitOk);
        return;
    }
    if (m_options.subjectIndex >= 0) {
        if (m_options.subjectIndex >= subjects.size()) {
            finish(ExitUsage, QStringLiteral("error: there is no Subject with that index"));
            return;
        }
        m_chosen << m_options.subjectIndex;
    } else {
        for (int i = 0; i < subjects.size(); ++i)
            m_chosen << i;
    }
    fetchNext();
}

void CliRunner::fetchNext()
{
    if (m_chosen.isEmpty()) {
        report();
        return;
    }
    const int index = m_chosen.takeFirst();
    m_current = SubjectResult();
    m_current.index = index;
    m_current.info = m_subjects.at(index);
    const QString ref = m_subjects.at(index).subjectRef;

    lm::AdapterLink::FetchCallback done = [this](const lm::CommandReport &rep, const lm::FetchCompletion &c) {
        if (rep.outcome.result != lm::CommandOutcome::Ok) {
            failedCommand(rep);
            return;
        }
        m_current.sensor = c.sensor;
        m_current.coverage = c.coverage;
        m_current.watermark = c.watermark;
        m_current.received = rep.outcome.recordsReceived;
        m_current.valid = rep.outcome.recordsValid;
        m_current.quarantined = rep.outcome.recordsReceived - rep.outcome.recordsValid;
        m_results.append(m_current);
        fetchNext();
    };
    if (m_options.command == QLatin1String("current"))
        m_link.fetchCurrent(ref, done);
    else
        m_link.fetchRange(ref, m_options.from, m_options.to, QStringList(), done);
}

void CliRunner::report()
{
    if (m_options.command == QLatin1String("current")) {
        if (m_options.json)
            writeOut(QJsonDocument(currentJson(m_results, lm::UtcTime::now())).toJson(QJsonDocument::Compact) + '\n');
        else
            writeOut(formatCurrent(m_results, lm::UtcTime::now()).toUtf8());
    } else {
        if (m_options.json)
            writeOut(QJsonDocument(rangeJson(m_results)).toJson(QJsonDocument::Compact) + '\n');
        else
            writeOut(formatRange(m_results).toUtf8());
    }
    finish(ExitOk);
}

} // namespace lmcli
