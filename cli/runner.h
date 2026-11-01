// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Michele Brami
#ifndef LMCLI_RUNNER_H
#define LMCLI_RUNNER_H

#include "options.h"
#include "output.h"
#include "session/adapterlink.h"

#include <QObject>

namespace lmcli {

// One run of the command-line program: start the adapter, log in, discover the Subjects,
// then print what the command asks for. It uses NullPolicy (contract defaults); a hard
// violation ends the run with exit code 21.
class CliRunner : public QObject, public lm::RecordSink, public lm::QuarantineSink
{
    Q_OBJECT
public:
    CliRunner(const Options &options, const QString &adapterPath, QObject *parent = 0);

    // RecordSink and QuarantineSink: the program has no store; records are kept for printing.
    void accept(const lm::LinkId &link, const QString &feed, const QVector<lm::LinkRecord> &records);
    void reject(const lm::LinkId &link, const QString &reason, const QString &detail,
                const QJsonObject &rawRecord);

public slots:
    void run();

signals:
    void finished(int exitCode);

private:
    void finish(int code, const QString &message = QString());
    void authenticated(const lm::SessionGrant &grant);
    void discovered(const QList<lm::SubjectInfo> &subjects);
    void fetchNext();
    void report();
    void failedCommand(const lm::CommandReport &report);

    Options m_options;
    QString m_adapterPath;
    lm::NullPolicy m_policy;
    lm::AdapterLink m_link;
    lm::Credentials m_credentials;
    QString m_session;
    QList<lm::SubjectInfo> m_subjects;
    QList<int> m_chosen;                // indices still to fetch
    QList<SubjectResult> m_results;
    SubjectResult m_current;            // the one being fetched
    bool m_finishing;
};

} // namespace lmcli

#endif
