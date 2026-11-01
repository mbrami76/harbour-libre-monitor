// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Michele Brami
#ifndef LM_FAKEADAPTER_H
#define LM_FAKEADAPTER_H

// Test doubles for the session driver: in-memory pipes, a scripted adapter end, a
// recording policy and recording sinks (rule 5).

#include "session/adapterlink.h"
#include "jsonl/jsonlcodec.h"

#include <QBuffer>
#include <QJsonArray>
#include <QJsonDocument>

namespace lm {

// Bytes the adapter "writes": the driver reads them.
class PipeDevice : public QIODevice
{
    Q_OBJECT
public:
    PipeDevice() { open(QIODevice::ReadOnly); }
    void push(const QByteArray &data) { m_data.append(data); emit readyRead(); }
    qint64 bytesAvailable() const { return m_data.size() + QIODevice::bytesAvailable(); }
    bool isSequential() const { return true; }

protected:
    qint64 readData(char *out, qint64 max)
    {
        const qint64 n = qMin<qint64>(max, m_data.size());
        memcpy(out, m_data.constData(), n);
        m_data.remove(0, n);
        return n;
    }
    qint64 writeData(const char *, qint64) { return -1; }

private:
    QByteArray m_data;
};

// Bytes the driver writes: the adapter reads them.
class SinkDevice : public QIODevice
{
    Q_OBJECT
public:
    SinkDevice() { open(QIODevice::WriteOnly); }
    QByteArray data;
    bool isSequential() const { return true; }

protected:
    qint64 readData(char *, qint64) { return -1; }
    qint64 writeData(const char *in, qint64 n) { data.append(in, int(n)); return n; }
};

class FakeChannel : public LinkChannel
{
    Q_OBJECT
public:
    FakeChannel() : killed(false), running(false), startFails(false), silentStart(false), peak(-1) {}
    // What the driver reads as the adapter's resident peak; a test raises it.
    qint64 peakResidentBytes() const { return running ? peak : -1; }

    void open()
    {
        if (startFails) {
            emit failedToStart(QStringLiteral("cannot start"));
            return;
        }
        running = true;
        // The race: with a real process the adapter's first bytes and the
        // `started` notification can reach the event loop in one pass, the output first.
        if (!outputBeforeStarted.isEmpty())
            pipe.push(outputBeforeStarted);
        if (!silentStart)
            emit started();
    }
    QIODevice *fromAdapter() { return &pipe; }
    QIODevice *toAdapter() { return &sink; }
    bool isRunning() const { return running; }
    void kill()
    {
        killed = true;
        if (onKill)
            onKill();
        if (running) {
            running = false;
            emit finished(-1, true);
        }
    }

    // The adapter's side.
    void say(const QByteArray &line) { pipe.push(line); }
    void sayJson(const QJsonObject &o) { pipe.push(encodeJsonLine(o)); }
    void sayStderr(const QByteArray &text) { emit stderrData(text); }
    void exitWith(int code) { if (running) { running = false; emit finished(code, false); } }

    // The core's side: every message the driver wrote, parsed.
    QList<QJsonObject> sent() const
    {
        QList<QJsonObject> list;
        const QList<QByteArray> lines = sink.data.split('\n');
        for (int i = 0; i < lines.size(); ++i)
            if (!lines.at(i).isEmpty())
                list.append(QJsonDocument::fromJson(lines.at(i)).object());
        return list;
    }
    QJsonObject last() const { const QList<QJsonObject> s = sent(); return s.isEmpty() ? QJsonObject() : s.last(); }
    QString lastId() const { return last().value("id").toString(); }
    QString lastType() const { return last().value("type").toString(); }

    std::function<void()> onKill;
    PipeDevice pipe;
    SinkDevice sink;
    bool killed;
    bool running;
    bool startFails;
    bool silentStart;
    qint64 peak;
    QByteArray outputBeforeStarted;     // delivered inside open(), ahead of `started`
};

class RecordingPolicy : public LinkPolicy
{
public:
    RecordingPolicy() : mayStartResult(MayStartResult::allowed()), admittedResult(MayStartResult::allowed())
    {
        // Short times so that timeout tests are quick.
        lim.helloTimeoutMs = 150;
        lim.shutdownWaitMs = 100;
        lim.authenticateDeadlineMs = 150;
        lim.discoverSubjectsDeadlineMs = 150;
        lim.fetchCurrentDeadlineMs = 150;
        lim.fetchRangeDeadlineMs = 150;
        lim.maxStderrBytes = 4096;
    }

    LinkLimits limits() const { return lim; }
    MayStartResult mayStart(const StartRequest &r) { starts.append(r); return mayStartResult; }
    void onSpawned(const LinkId &l) { spawned.append(l); }
    // Second check after hello: records the link as the driver
    // passes it, so tests can see that the adapter version is filled in by now.
    MayStartResult onAdmitted(const LinkId &l) { admitted.append(l); return admittedResult; }
    void onExited(const LinkId &l, const ExitStatus &s) { exited.append(l); exitStatuses.append(s); }
    Action onLine(const LinkId &, qint64 bytes, const UtcTime &)
    {
        lineBytes.append(bytes);
        if (!lineActions.isEmpty())
            return lineActions.takeFirst();
        return Action::proceed();
    }
    Action onBatch(const LinkId &, const QString &feed, int records, qint64)
    {
        batches.append(qMakePair(feed, records));
        if (!batchActions.isEmpty())
            return batchActions.takeFirst();
        return Action::proceed();
    }
    Action onStderr(const LinkId &, qint64 bytes)
    {
        stderrBytes.append(bytes);
        if (!stderrActions.isEmpty())
            return stderrActions.takeFirst();
        return Action::proceed();
    }
    // Scripted answers, taken in order; proceed when the script is empty. A script entry
    // is used for any violation, soft or hard.
    Action onViolation(const Violation &v)
    {
        violations.append(v);
        if (!violationActions.isEmpty())
            return violationActions.takeFirst();
        return Action::proceed();
    }
    void onCommandFinished(const CommandOutcome &o) { outcomes.append(o); }

    bool saw(ViolationKind kind) const
    {
        for (int i = 0; i < violations.size(); ++i)
            if (violations.at(i).kind == kind)
                return true;
        return false;
    }

    LinkLimits lim;
    MayStartResult mayStartResult, admittedResult;
    QList<Action> lineActions, batchActions, stderrActions, violationActions;
    QList<StartRequest> starts;
    QList<LinkId> spawned, admitted, exited;
    QList<ExitStatus> exitStatuses;
    QList<qint64> lineBytes, stderrBytes;
    QList<QPair<QString, int> > batches;
    QList<Violation> violations;
    QList<CommandOutcome> outcomes;
};

class RecordingSinks : public RecordSink, public QuarantineSink
{
public:
    void accept(const LinkId &, const QString &feed, const QVector<LinkRecord> &records)
    {
        for (int i = 0; i < records.size(); ++i) {
            valid.append(records.at(i));
            validFeeds.append(feed);
        }
        ++acceptCalls;
    }
    void reject(const LinkId &, const QString &reason, const QString &detail, const QJsonObject &raw)
    {
        reasons.append(reason);
        details.append(detail);
        rawRecords.append(raw);
    }

    RecordingSinks() : acceptCalls(0) {}
    QList<LinkRecord> valid;
    QStringList validFeeds;
    int acceptCalls;
    QStringList reasons, details;
    QList<QJsonObject> rawRecords;
};

// Records what the driver hands over when the process is ended in the middle of a batch.
class RecordingRejectedSink : public RejectedBatchSink
{
public:
    void rejected(const LinkId &, const QByteArray &rawLine, CommandOutcome::KilledBy by,
                  bool hasViolation, ViolationKind kind)
    {
        lines.append(rawLine);
        killedBy.append(by);
        hadViolation.append(hasViolation);
        kinds.append(kind);
    }

    QList<QByteArray> lines;
    QList<CommandOutcome::KilledBy> killedBy;
    QList<bool> hadViolation;
    QList<ViolationKind> kinds;
};

} // namespace lm

#endif
