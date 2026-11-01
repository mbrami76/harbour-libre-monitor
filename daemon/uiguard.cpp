// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Michele Brami
#include "uiguard.h"

#include <QLocalServer>
#include <QLocalSocket>

namespace lm {

UiGuard::UiGuard(const QString &directory, QObject *parent)
    : QObject(parent), m_lockPath(directory + QStringLiteral("/ui.lock")),
      m_socketPath(directory + QStringLiteral("/ui.sock")), m_lock(m_lockPath), m_server(0)
{
    // A lock whose process is gone is stale at once: stale time 0 means "no time limit for a
    // living process, but a dead one is recovered", which is what a killed UI needs.
    m_lock.setStaleLockTime(0);
}

UiGuard::~UiGuard()
{
    if (m_server)
        m_server->close();
}

UiGuard::Result UiGuard::acquire()
{
    if (!m_lock.tryLock(0)) {
        QLocalSocket socket;
        socket.connectToServer(m_socketPath);
        if (!socket.waitForConnected(1000))
            return SecondNoServer;
        socket.write("raise\n");
        socket.waitForBytesWritten(1000);
        socket.disconnectFromServer();
        return SecondRaised;
    }
    // A socket file left by a killed UI would make listen() fail; the lock proves nobody owns it.
    QLocalServer::removeServer(m_socketPath);
    m_server = new QLocalServer(this);
    m_server->listen(m_socketPath);
    connect(m_server, SIGNAL(newConnection()), this, SLOT(onNewConnection()));
    return First;
}

void UiGuard::onNewConnection()
{
    QLocalSocket *socket = m_server->nextPendingConnection();
    if (!socket)
        return;
    connect(socket, &QLocalSocket::readyRead, this, [this, socket]() {
        socket->readAll();
        socket->disconnectFromServer();
        emit raiseRequested();
    });
    connect(socket, SIGNAL(disconnected()), socket, SLOT(deleteLater()));
}

} // namespace lm
