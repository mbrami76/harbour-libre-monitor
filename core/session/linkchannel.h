// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Michele Brami
#ifndef LM_LINKCHANNEL_H
#define LM_LINKCHANNEL_H

#include <QByteArray>
#include <QIODevice>
#include <QObject>
#include <QString>

namespace lm {

// The driver's view of the other end: a process in production, in-memory pipes in tests.
// open() starts the other end; the signals report what happens. All signals are emitted
// from the thread of the event loop (a test channel may emit them from inside open()).
class LinkChannel : public QObject
{
    Q_OBJECT
public:
    explicit LinkChannel(QObject *parent = 0) : QObject(parent) {}

    virtual void open() = 0;
    virtual QIODevice *fromAdapter() = 0;       // the adapter's standard output; emits readyRead
    virtual QIODevice *toAdapter() = 0;         // the adapter's standard input
    virtual bool isRunning() const = 0;
    virtual void kill() = 0;                    // at once; finished() follows
    // The highest resident size the other end has reached so far, in bytes; -1 when unknown
    // (an in-memory end has no size, the process has ended). The driver reads it while the
    // adapter runs and ends the process above the limit.
    virtual qint64 peakResidentBytes() const { return -1; }

signals:
    void started();
    void failedToStart(const QString &reason);  // no secrets: a reason from the platform
    void stderrData(const QByteArray &data);    // diagnostics only, never parsed
    void finished(int exitCode, bool crashed);
};

} // namespace lm

#endif
