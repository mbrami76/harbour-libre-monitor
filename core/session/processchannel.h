// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Michele Brami
#ifndef LM_PROCESSCHANNEL_H
#define LM_PROCESSCHANNEL_H

#include "session/linkchannel.h"

#include <QProcess>
#include <QStringList>

namespace lm {

// The adapter as a child process (QProcess, QtCore). Credentials never pass through
// here: arguments and environment carry no secret (REQ-ADP-11).
//
// OS limits (address space, CPU time, open descriptors; REQ-GRD-01, REQ-GRD-02) are set in
// the child between fork and exec with setrlimit, through QProcess::setupChildProcess()
// (Unix only; Qt 5.6 has it). 0 = no limit. The limits come from LinkPolicy::limits().
struct OsLimits
{
    OsLimits() : memoryBytes(0), cpuSeconds(0), fileDescriptors(0) {}

    qint64 memoryBytes;         // RLIMIT_AS
    int cpuSeconds;             // RLIMIT_CPU
    int fileDescriptors;        // RLIMIT_NOFILE
};

class LimitedProcess : public QProcess
{
public:
    OsLimits limits;

protected:
    void setupChildProcess();
};

class ProcessChannel : public LinkChannel
{
    Q_OBJECT
public:
    ProcessChannel(const QString &program, const QStringList &arguments, QObject *parent = 0);

    ~ProcessChannel();

    void setOsLimits(const OsLimits &limits) { m_process.limits = limits; }

    void open();
    QIODevice *fromAdapter() { return &m_process; }
    QIODevice *toAdapter() { return &m_process; }
    bool isRunning() const { return m_process.state() != QProcess::NotRunning; }
    void kill();
    qint64 peakResidentBytes() const;

private slots:
    void onError(QProcess::ProcessError error);
    void onFinished(int exitCode, QProcess::ExitStatus status);
    void onStderr();

private:
    LimitedProcess m_process;
    QString m_program;
    QStringList m_arguments;
};

} // namespace lm

#endif
