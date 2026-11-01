// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Michele Brami
#ifndef LM_UNIXSIGNALS_H
#define LM_UNIXSIGNALS_H

// Turns SIGTERM, SIGINT and SIGHUP into a Qt signal on the main thread (REQ-ARC-10). The
// system sends SIGTERM at a shutdown and the service must write its stop record and leave
// cleanly (S3). A signal handler may do almost nothing safely, so it only writes one byte to a
// pipe; a socket notifier in the event loop reads it and emits the Qt signal. This is the
// standard "self-pipe" pattern. Only one instance may exist per process.

#include <QObject>

class QSocketNotifier;

namespace lm {

class UnixSignalForwarder : public QObject
{
    Q_OBJECT
public:
    explicit UnixSignalForwarder(QObject *parent = 0);
    ~UnixSignalForwarder();

signals:
    void signalReceived(int signalNumber);

private slots:
    void onReadable();

private:
    QSocketNotifier *m_notifier;
};

} // namespace lm

#endif
