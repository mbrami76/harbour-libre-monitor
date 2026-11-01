// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Michele Brami
#ifndef LM_SERVICETEST_H
#define LM_SERVICETEST_H

// Fakes for the service lifecycle tests: every port of
// core/service/serviceports.h with a recording, scriptable implementation. Nothing here uses
// real time, a process, D-Bus or a database: a test moves the three clocks by hand and calls
// the lifecycle, which is what makes the transition table testable row by row.

#include "service/servicelifecycle.h"

#include <QList>
#include <QMap>

namespace lm {
namespace testing {

// Wall time, CLOCK_BOOTTIME and CLOCK_MONOTONIC, moved by hand. A normal advance moves all
// three; suspend() moves the wall and boot clocks but not the monotonic one, as the phone
// does while it sleeps; setWallOnly() models a clock change by the user.
class FakeServiceClock : public ServiceClock
{
public:
    FakeServiceClock() : m_wallMs(1768000000000LL), m_bootMs(600000), m_monoMs(500000) {}

    UtcTime now() const { return UtcTime::fromMSecsSinceEpoch(m_wallMs); }
    qint64 bootTimeMs() const { return m_bootMs; }
    qint64 monotonicMs() const { return m_monoMs; }

    void advanceS(qint64 s) { m_wallMs += s * 1000; m_bootMs += s * 1000; m_monoMs += s * 1000; }
    void advanceMs(qint64 ms) { m_wallMs += ms; m_bootMs += ms; m_monoMs += ms; }
    void suspendS(qint64 s) { m_wallMs += s * 1000; m_bootMs += s * 1000; }
    void setWallOffsetS(qint64 s) { m_wallMs += s * 1000; }
    // A phone restart: the boot counter starts again, the wall clock went on.
    void restartPhone(qint64 downS, qint64 newBootMs)
    { m_wallMs += downS * 1000; m_bootMs = newBootMs; m_monoMs = newBootMs; }
    void setBootMs(qint64 ms) { m_bootMs = ms; }

private:
    qint64 m_wallMs, m_bootMs, m_monoMs;
};

class FakeNameOwner : public NameOwner
{
public:
    FakeNameOwner() : available(true), owned(false), takes(0), releases(0) {}
    bool take() { ++takes; if (!available) return false; owned = true; return true; }
    void release() { ++releases; owned = false; }
    bool available, owned;
    int takes, releases;
};

class FakeStorage : public ServiceStorage
{
public:
    FakeStorage() : openOk(true), isOpen(false), opens(0), closes(0), checkDue(false), checksRun(0) {}
    bool open(QString *cause)
    { ++opens; if (!openOk) { *cause = failCause; return false; } isOpen = true; return true; }
    void close() { ++closes; isOpen = false; }
    bool storageCheckDue(const UtcTime &) { return checkDue; }
    void runStorageCheck(const UtcTime &) { ++checksRun; checkDue = false; }

    bool openOk;
    QString failCause;
    bool isOpen;
    int opens, closes;
    bool checkDue;
    int checksRun;
};

// An in-memory journal. `appended` records every append in order, including the failures'
// attempts, so a test can prove that a second instance wrote nothing.
class FakeJournal : public ServiceJournal
{
public:
    FakeJournal() : failAppend(false) {}

    bool lastLifecycleEntry(JournalEntry *entry)
    {
        bool found = false;
        for (int i = 0; i < entries.size(); ++i) {
            const QString k = entries.at(i).kind;
            if (k != JournalKind::heartbeat() && k != JournalKind::start() && k != JournalKind::stop())
                continue;
            if (!found || entries.at(i).wallTime.msecsSinceEpoch() >= entry->wallTime.msecsSinceEpoch()) {
                *entry = entries.at(i);
                found = true;
            }
        }
        return found;
    }
    bool hasStartRecord()
    {
        for (int i = 0; i < entries.size(); ++i)
            if (entries.at(i).kind == JournalKind::start())
                return true;
        return false;
    }
    bool append(const JournalEntry &entry)
    {
        ++appendAttempts;
        if (failAppend)
            return false;
        entries.append(entry);
        return true;
    }
    int count(const QString &kind) const
    {
        int n = 0;
        for (int i = 0; i < entries.size(); ++i)
            if (entries.at(i).kind == kind)
                ++n;
        return n;
    }
    JournalEntry last(const QString &kind) const
    {
        for (int i = entries.size() - 1; i >= 0; --i)
            if (entries.at(i).kind == kind)
                return entries.at(i);
        return JournalEntry();
    }

    QList<JournalEntry> entries;
    bool failAppend;
    int appendAttempts = 0;
};

class FakeNotifier : public Notifier
{
public:
    FakeNotifier() : withdrawals(0), calls(0) {}
    void show(const Notice &notice) { shown.append(notice); if (calls) calls->append(QStringLiteral("resident")); }
    void withdraw() { ++withdrawals; }
    Notice last() const { return shown.isEmpty() ? Notice() : shown.last(); }
    QList<Notice> shown;
    int withdrawals;            // the notice taken away after a removal
    QStringList *calls;         // optional: one list shared with a FakeNoteBackend, to see the order of the calls
};

class FakeWake : public WakeSource
{
public:
    FakeWake() { frequencies << 30 << 150 << 300 << 600 << 900 << 1800 << 3600; }
    QList<int> supportedFrequenciesS() const { return frequencies; }
    void setHandler(const std::function<void()> &h) { handler = h; }
    // Test hook: the platform delivers a wake-up.
    void fire() { if (handler) handler(); }
    void arm(int f) { armed = f; armCount++; cancelled = false; }
    void cancel() { cancelled = true; armed = 0; }
    QList<int> frequencies;
    std::function<void()> handler;
    int armed = 0;
    int armCount = 0;
    bool cancelled = false;
};

// Records the requests; a test completes them with answer().
class FakeApproval : public ApprovalSource
{
public:
    struct Request { QString connection; Done done; };
    void request(const QString &connection, const Done &done)
    {
        Request r; r.connection = connection; r.done = done;
        requests.append(r);
    }
    void answer(int index, bool granted) { requests.at(index).done(granted); }
    QList<Request> requests;
};

class FakeSchedulerHost : public SchedulerHost
{
public:
    FakeSchedulerHost() : started(0), ticks(0), shutdowns(0), ended(true), queued(0),
                          flushStays(false) {}
    void start(const UtcTime &since) { ++started; downSince = since; log << "start"; }
    void tick() { ++ticks; log << "tick"; }
    UtcTime nextWakeAt() const { return nextWake; }
    void shutdown() { ++shutdowns; log << "shutdown"; }
    bool linksEnded() const { return ended; }
    int queuedReadings() const { return queued; }
    int flushQueuedReadings() { log << "flush"; if (!flushStays) queued = 0; return queued; }
    void secretsAvailable(const QString &c) { available << c; }

    int started, ticks, shutdowns;
    UtcTime downSince, nextWake;
    bool ended;
    int queued;
    bool flushStays;
    QStringList available;
    QStringList log;
};

class RecordingSink : public ServiceEventSink
{
public:
    void gapDetected(const UtcTime &from, const UtcTime &to) { gaps.append(qMakePair(from, to)); }
    void readingQueueHighWater(int n) { highWater << n; }
    void connectionWarning(const QString &c, const QString &k, const QString &) { warnings << c + ":" + k; }
    void dataChanged(const QUuid &s) { changed << s; }
    QList<QPair<UtcTime, UtcTime> > gaps;
    QList<int> highWater;
    QStringList warnings;
    QList<QUuid> changed;
};

class RecordingIngestLog : public IngestLogSink
{
public:
    bool append(const IngestLogEntry &e) { entries.append(e); return true; }
    QList<IngestLogEntry> entries;
};

// Everything wired together, as the process will do it.
struct ServiceRig
{
    ServiceRig(const LifecycleSettings &settings = LifecycleSettings())
    {
        LifecyclePorts p;
        p.clock = &clock; p.names = &names; p.storage = &storage; p.journal = &journal;
        p.notifier = &notifier; p.wake = &wake; p.approval = &approval; p.scheduler = &scheduler;
        p.events = &sink; p.ingestLog = &ingestLog;
        lifecycle = new ServiceLifecycle(p, settings);
    }
    ~ServiceRig() { delete lifecycle; }

    FakeServiceClock clock;
    FakeNameOwner names;
    FakeStorage storage;
    FakeJournal journal;
    FakeNotifier notifier;
    FakeWake wake;
    FakeApproval approval;
    FakeSchedulerHost scheduler;
    RecordingSink sink;
    RecordingIngestLog ingestLog;
    ServiceLifecycle *lifecycle;
};

} // namespace testing
} // namespace lm

#endif
