// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Michele Brami
#include "session/processchannel.h"

#include "session/processmemory.h"

#include <sys/resource.h>

namespace lm {

// Runs in the child after fork, before exec: only async-signal-safe calls (setrlimit).
void LimitedProcess::setupChildProcess()
{
    struct rlimit rl;
    if (limits.memoryBytes > 0) {
        rl.rlim_cur = rl.rlim_max = static_cast<rlim_t>(limits.memoryBytes);
        ::setrlimit(RLIMIT_AS, &rl);
    }
    if (limits.cpuSeconds > 0) {
        rl.rlim_cur = rl.rlim_max = static_cast<rlim_t>(limits.cpuSeconds);
        ::setrlimit(RLIMIT_CPU, &rl);
    }
    if (limits.fileDescriptors > 0) {
        rl.rlim_cur = rl.rlim_max = static_cast<rlim_t>(limits.fileDescriptors);
        ::setrlimit(RLIMIT_NOFILE, &rl);
    }
}

ProcessChannel::ProcessChannel(const QString &program, const QStringList &arguments, QObject *parent)
    : LinkChannel(parent), m_program(program), m_arguments(arguments)
{
    m_process.setProcessChannelMode(QProcess::SeparateChannels);
    connect(&m_process, &QProcess::started, this, &LinkChannel::started);
    connect(&m_process, static_cast<void (QProcess::*)(QProcess::ProcessError)>(&QProcess::error),
            this, &ProcessChannel::onError);
    connect(&m_process, static_cast<void (QProcess::*)(int, QProcess::ExitStatus)>(&QProcess::finished),
            this, &ProcessChannel::onFinished);
    connect(&m_process, &QProcess::readyReadStandardError, this, &ProcessChannel::onStderr);
}

ProcessChannel::~ProcessChannel()
{
    if (m_process.state() != QProcess::NotRunning) {
        m_process.kill();
        m_process.waitForFinished(1000);
    }
}

void ProcessChannel::open()
{
    m_process.start(m_program, m_arguments, QIODevice::ReadWrite);
}

void ProcessChannel::kill()
{
    if (m_process.state() != QProcess::NotRunning)
        m_process.kill();
}

qint64 ProcessChannel::peakResidentBytes() const
{
    if (m_process.state() == QProcess::NotRunning)
        return -1;
    return readPeakResidentBytes(m_process.processId());
}

void ProcessChannel::onError(QProcess::ProcessError error)
{
    if (error == QProcess::FailedToStart)
        emit failedToStart(QStringLiteral("the adapter executable could not be started"));
    // Crashed: finished() follows.
}

void ProcessChannel::onFinished(int exitCode, QProcess::ExitStatus status)
{
    onStderr();     // whatever is left, for diagnostics
    emit finished(exitCode, status == QProcess::CrashExit);
}

void ProcessChannel::onStderr()
{
    const QByteArray data = m_process.readAllStandardError();
    if (!data.isEmpty())
        emit stderrData(data);
}

} // namespace lm
