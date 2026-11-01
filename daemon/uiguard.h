// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Michele Brami
#ifndef LM_UIGUARD_H
#define LM_UIGUARD_H

// One user-interface process at a time. With `X-Nemo-Single-Instance=no` in
// the desktop file the launcher no longer hands a tap to a running instance, and while the
// headless service runs a tap starts a separate UI process. A
// second tap would then start a second UI. This guard prevents that without touching the
// service: the first UI takes a lock file in the data directory and listens on a local socket;
// a second UI that finds the lock sends "raise" over the socket and exits, and the first UI
// brings its window forward.
//
// Pattern: a lock file plus a local socket, the standard recipe for a single-instance desktop
// application. A lock left behind by a killed UI is recovered by QLockFile (it checks that the
// process still exists), which the spike showed on the emulator.
//
// Used only in UI mode. The service never takes this lock, so the service and the UI do not
// block each other (REQ-ARC-03).

#include <QLockFile>
#include <QObject>
#include <QString>

class QLocalServer;

namespace lm {

class UiGuard : public QObject
{
    Q_OBJECT
public:
    enum Result {
        First,          // this process is the UI; the guard now listens for raise requests
        SecondRaised,   // another UI runs and was asked to come forward; this process should exit
        SecondNoServer  // another UI holds the lock but did not answer; this process should exit
    };

    // `directory` is the application's data directory (private under Sailjail).
    explicit UiGuard(const QString &directory, QObject *parent = 0);
    ~UiGuard();

    Result acquire();

signals:
    // A second launch asked this UI to come forward (a tap on the icon).
    void raiseRequested();

private slots:
    void onNewConnection();

private:
    QString m_lockPath;
    QString m_socketPath;
    QLockFile m_lock;
    QLocalServer *m_server;
};

} // namespace lm

#endif
