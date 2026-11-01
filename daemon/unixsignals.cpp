// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Michele Brami
#include "unixsignals.h"

#include <QSocketNotifier>

#include <csignal>
#include <unistd.h>

namespace lm {

namespace {

int g_pipe[2] = { -1, -1 };

// Async-signal-safe: one write of one byte, nothing else.
void handler(int number)
{
    const char byte = char(number);
    if (g_pipe[1] >= 0) {
        const ssize_t n = ::write(g_pipe[1], &byte, 1);
        (void)n;
    }
}

} // namespace

UnixSignalForwarder::UnixSignalForwarder(QObject *parent) : QObject(parent), m_notifier(0)
{
    if (::pipe(g_pipe) != 0)
        return;
    m_notifier = new QSocketNotifier(g_pipe[0], QSocketNotifier::Read, this);
    connect(m_notifier, SIGNAL(activated(int)), this, SLOT(onReadable()));

    struct sigaction sa;
    sa.sa_handler = handler;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = SA_RESTART;
    sigaction(SIGTERM, &sa, 0);
    sigaction(SIGINT, &sa, 0);
    sigaction(SIGHUP, &sa, 0);
}

UnixSignalForwarder::~UnixSignalForwarder()
{
    signal(SIGTERM, SIG_DFL);
    signal(SIGINT, SIG_DFL);
    signal(SIGHUP, SIG_DFL);
    if (g_pipe[0] >= 0) ::close(g_pipe[0]);
    if (g_pipe[1] >= 0) ::close(g_pipe[1]);
    g_pipe[0] = g_pipe[1] = -1;
}

void UnixSignalForwarder::onReadable()
{
    m_notifier->setEnabled(false);
    char byte = 0;
    if (::read(g_pipe[0], &byte, 1) == 1)
        emit signalReceived(int(byte));
    m_notifier->setEnabled(true);
}

} // namespace lm
