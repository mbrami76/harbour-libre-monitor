// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Michele Brami
#ifndef LM_SCHEDULER_H
#define LM_SCHEDULER_H

// The scheduler (REQ-SCH-01 to 08, 12, 13): the state
// machine of every (connection, Subject), driven by time and by command outcomes. It decides
// WHEN to run which command, WHAT to do about an error, and, with the guard, when to stop.
//
// Pattern: a pure, polled state machine. Nothing here owns a timer or reads the system
// clock: the service calls tick() when nextWakeAt() has come (a keepalive wake-up on the
// phone, REQ-ARC-08), and tests move a manual clock and call tick(). Commands run through the
// session manager of each connection; outcomes come back by callback and are applied at once.
//
// Rows of the transition table in the document are cited as T-nn in the .cpp.
//
// Assumptions: one command at a time per link; the
// scheduler reacts to the error class and to `killedBy`, never to the adapter's text; the
// `current` feed is the only one that feeds the staleness clock; a restart
// recomputes everything except what the StateKeeper persisted.

#include "scheduler/connectionsession.h"
#include "scheduler/schedulemath.h"

#include <QHash>
#include <QList>
#include <QMap>
#include <QUuid>

namespace lm {

enum class ScheduleState { Idle, Fetching, Backoff, NeedsAttention, DisabledByGuard };
QString scheduleStateName(ScheduleState state);

enum Feed { FeedCurrent = 0, FeedGraph = 1, FeedLogbook = 2 };
const int kFeedCount = 3;
QString feedName(int feed);

// What the outside world (the service, the UI) may read about one Subject.
struct SubjectStatus
{
    SubjectStatus() : state(ScheduleState::Idle), backoffN(0), stale(false), hasTrend(false),
                      trendCode(0) {}

    QUuid subject;
    QString connection;
    ScheduleState state;
    QString cause;                  // why, in backoff, needs-attention, disabled-by-guard
    UtcTime retryAt;                // backoff / hold
    UtcTime nextDue[kFeedCount];
    int backoffN;
    UtcTime lastSuccessAt;          // newest stored `current` record (REQ-SCH-06)
    UtcTime lastContactOkAt;
    bool stale;
    QString staleCause;             // StaleCause
    QString staleDetail;
    UtcTime staleSince;
    // The source's trend arrow of the newest stored `current` reading (contract
    //), kept in memory only and never stored. `trendFor` is that reading's measurement
    // time, so the reader can tell which reading the arrow belongs to. Absent (hasTrend false)
    // when the adapter gave none, before the first reading after a start, and when the reading
    // is older than the staleness limit: an arrow of an old reading says nothing about now.
    // The number is not interpreted here; the UI maps it.
    bool hasTrend;
    int trendCode;
    UtcTime trendFor;
};

// Told about changes. All callbacks have empty defaults.
class SchedulerListener
{
public:
    virtual ~SchedulerListener() {}
    virtual void subjectStateChanged(const SubjectStatus &) {}
    virtual void stalenessChanged(const SubjectStatus &) {}
    // A visible warning (REQ-SCH-04): kind is "protocol-changed", "approval-waiting",
    // "renewal-refused" or "rate-limited".
    virtual void connectionWarning(const QString & /*connection*/, const QString & /*kind*/,
                                   const QString & /*detail*/) {}
    // The in-memory queue of readings that wait for a busy store is full: the maintenance
    // run should stop and the user be told (REQ-STO-15).
    virtual void readingQueueFull(int /*droppedRecords*/) {}
};

class Scheduler : public GuardListener, public SessionListener
{
public:
    // Neither the ports nor the listener are owned. Registers itself as the guard's listener.
    Scheduler(const SchedulerPorts &ports, const SchedulerSettings &settings = SchedulerSettings());
    ~Scheduler();

    void setListener(SchedulerListener *listener) { m_listener = listener; }

    // Configuration; before start().
    void addConnection(const ConnectionConfig &config);
    // The version of the built-in adapter, if the service knows it before a start; lets a
    // connection stopped by `protocol-changed` try once more after an update.
    void setCurrentAdapterVersion(const QString &adapterId, const QString &version);

    // Restores the persisted states and plans the first runs.
    // `serviceDownSince` is the start of the journal gap before this start, if any
    // (staleness cause 5).
    void start(const UtcTime &serviceDownSince = UtcTime());
    void shutdown();

    // Runs everything that is due now.
    void tick();
    // The earliest instant at which tick() has something to do; invalid if never.
    UtcTime nextWakeAt() const;

    // Events from the user and the app.
    void userRetry(const QString &connection);
    void credentialsChanged(const QString &connection);
    void secretsAvailable(const QString &connection);
    void intervalsChanged(const QString &connection, const ConnectionConfig &config);

    // Where the link's records and rejected records go; set them on the AdapterLink.
    RecordSink *recordSink() { return &m_recordSink; }
    QuarantineSink *quarantineSink() { return &m_quarantineSink; }

    // Reading the state.
    QList<QUuid> subjects() const;
    SubjectStatus status(const QUuid &subject) const;
    SessionState sessionState(const QString &connection) const;
    int queuedReadings() const { return m_queuedRecords; }

    // GuardListener
    void guardTripped(const QString &connection);
    void guardVersionTripped(const QString &adapter, const QString &adapterVersion);
    void guardReenabled(const QString &connection);
    void guardVersionReenabled(const QString &adapter, const QString &adapterVersion);

    // SessionListener
    void sessionAttentionCleared(const QString &connection);
    void sessionApprovalWaiting(const QString &connection, const UtcTime &since);
    void sessionRenewalRefused(const QString &connection);

private:
    struct Conn;

    struct Subj
    {
        Subj() : conn(0), state(ScheduleState::Idle), retryAtMs(-1), backoffN(0), recheckN(0),
                 rejectStreak(0), subjectFailN(0), firstFailMs(-1), lastReadingMs(-1),
                 lastContactOkMs(-1), sensorReadyMs(-1), inflightFeed(-1), stale(false),
                 staleSinceMs(0), hasTrend(false), trend(0), trendForMs(-1) {
            for (int i = 0; i < kFeedCount; ++i) nextDueMs[i] = -1;
        }

        SubjectConfig cfg;
        Conn *conn;
        ScheduleState state;
        QString cause;
        qint64 retryAtMs;
        qint64 nextDueMs[kFeedCount];
        int backoffN;
        int recheckN;
        int rejectStreak;
        int subjectFailN;
        qint64 firstFailMs;
        qint64 lastReadingMs;           // newest `current` record, -1 = none
        qint64 lastContactOkMs;
        qint64 sensorReadyMs;           // activation + warm-up, -1 = unknown
        int inflightFeed;               // -1 = none
        bool stale;
        QString staleCause;
        QString staleDetail;
        qint64 staleSinceMs;
        // The trend arrow of the reading at `trendForMs` (in memory only).
        bool hasTrend;
        int trend;
        qint64 trendForMs;
    };

    struct Conn
    {
        Conn() : session(0), rateFloorMs(0), lastRateLimitMs(0), lastHoldMs(0), invalidN(0),
                 inflight(0), newReading(false), lateRecords(0) {}

        ConnectionConfig cfg;
        ConnectionSession *session;
        QList<Subj *> subjects;
        qint64 rateFloorMs;             // REQ-SCH-13, in memory only
        qint64 lastRateLimitMs;
        qint64 lastHoldMs;
        int invalidN;
        QHash<QString, qint64> lastSuccessBySubject;
        // The command in flight.
        Subj *inflight;
        bool newReading;
        qint64 lateRecords;
    };

    struct PendingBatch
    {
        QUuid subject;
        LinkId link;
        QVector<Record> records;
    };

    // The link's sinks forward here.
    class Sink : public RecordSink
    {
    public:
        explicit Sink(Scheduler *scheduler) : m_owner(scheduler) {}
        void accept(const LinkId &link, const QString &feed, const QVector<LinkRecord> &records)
        { m_owner->acceptRecords(link, feed, records); }
    private:
        Scheduler *m_owner;
    };
    class QSink : public QuarantineSink
    {
    public:
        explicit QSink(Scheduler *scheduler) : m_owner(scheduler) {}
        void reject(const LinkId &link, const QString &reason, const QString &detail,
                    const QJsonObject &raw)
        { m_owner->rejectRecord(link, reason, detail, raw); }
    private:
        Scheduler *m_owner;
    };

    qint64 nowMs() const { return m_ports.clock->now().msecsSinceEpoch(); }
    UtcTime at(qint64 ms) const { return ms < 0 ? UtcTime() : UtcTime::fromMSecsSinceEpoch(ms); }
    Conn *connection(const QString &id) const;
    Subj *subjectOf(const QUuid &id) const;

    // Intervals
    qint64 nominalMs(const Conn &c, int feed) const;
    qint64 effectiveMs(const Conn &c, const Subj &s, int feed, qint64 now) const;
    qint64 firstRunMs(const Conn &c, const Subj &s, int feed, qint64 now);

    // The machine
    void setState(Subj &s, ScheduleState state, const QString &cause, qint64 retryAtMs = -1);
    void toIdleDue(Subj &s, qint64 dueMs);
    void dispatch(Conn &c, qint64 now);
    void startCommand(Conn &c, Subj &s, int feed, qint64 now);
    void onResult(Conn *c, Subj *s, int feed, const SessionResult &r);
    void onConnFailed(Conn &c, Subj &s, const SessionResult &r, qint64 now);
    void onOutcomeOk(Conn &c, Subj &s, int feed, const SessionResult &r, qint64 now);
    void onOutcomeError(Conn &c, Subj &s, const SessionResult &r, qint64 now);
    void transientBackoff(Conn &c, Subj &s, int retryAfterS, qint64 notBeforeMs, const QString &cause,
                          qint64 now);
    void rateLimited(Conn &c, int retryAfterS, qint64 now);
    void attentionForConnection(Conn &c, const QString &cause, const QString &detail, bool persist,
                                const QString &version = QString());
    void clearSubjectAttention(Subj &s);
    bool otherSubjectSucceededSince(const Conn &c, const Subj &s, qint64 sinceMs) const;
    void disableConnection(Conn &c, const QString &cause);
    qint64 earliestDueMs(const Conn &c) const;
    void expireTimers(Conn &c, qint64 now);

    // Staleness
    void refreshStaleness(Subj &s, qint64 now);
    void noteTrend(Subj &s, const FetchCompletion &completion);
    SubjectStatus statusOf(const Subj &s) const;
    void notifyState(const Subj &s);

    // Records
    void acceptRecords(const LinkId &link, const QString &feed, const QVector<LinkRecord> &records);
    void rejectRecord(const LinkId &link, const QString &reason, const QString &detail,
                      const QJsonObject &raw);
    void flushReadings();

    SchedulerPorts m_ports;
    SchedulerSettings m_s;
    SchedulerListener *m_listener;
    Sink m_recordSink;
    QSink m_quarantineSink;
    QList<Conn *> m_conns;
    QHash<QString, QString> m_adapterVersions;
    QList<PendingBatch> m_queue;
    int m_queuedRecords;
    qint64 m_startedAtMs;
    qint64 m_serviceDownMs;
    bool m_contactOkSinceStart;
    bool m_started;
};

} // namespace lm

#endif
