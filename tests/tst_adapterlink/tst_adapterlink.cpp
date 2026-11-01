// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Michele Brami
//
// Session driver against a scripted fake adapter over in-memory pipes (
// REQ-ADP-06 to 12, REQ-ADM-01 to 04). Synthetic values only (REQ-DOC-03).
#include "fakeadapter.h"
#include "session/diagnosticreport.h"
#include "session/processchannel.h"
#include "session/processmemory.h"
#include "store/store.h"

#include <QtTest>

using namespace lm;

namespace {

QStringList g_qtMessages;

void captureMessages(QtMsgType, const QMessageLogContext &, const QString &message)
{
    g_qtMessages << message;
}

const char *kUser = "alice@example.invalid";
const char *kPassword = "hunter2-synthetic";
const char *kSession = "SESSION-BLOB-synthetic-9f3a";
const char *kName = "Maria Rossi";

QJsonArray arrayOf(const QStringList &list)
{
    QJsonArray a;
    for (int i = 0; i < list.size(); ++i)
        a.append(list.at(i));
    return a;
}

QJsonObject helloPayload()
{
    QJsonArray metrics;
    QJsonObject glucose, logbook;
    glucose.insert("metric", "glucose");
    glucose.insert("units", arrayOf(QStringList() << "mg/dL" << "mmol/L"));
    logbook.insert("metric", "logbook_entry");
    logbook.insert("units", arrayOf(QStringList() << "mg/dL" << "mmol/L"));
    metrics.append(glucose);
    metrics.append(logbook);
    QJsonObject p;
    p.insert("adapter", "libre");
    p.insert("adapter_version", "0.1.0");
    p.insert("contract_version", 0);
    p.insert("metrics", metrics);
    p.insert("feeds", arrayOf(QStringList() << "current" << "graph" << "logbook"));
    return p;
}

QJsonObject sampleRecord(double value = 112, const QString &time = "2026-01-01T12:00:00Z")
{
    QJsonObject r;
    r.insert("shape", "sample");
    r.insert("subject_ref", "S-0001");
    r.insert("metric", "glucose");
    r.insert("value", value);
    r.insert("unit", "mg/dL");
    r.insert("measured_at", time);
    r.insert("original_timestamp", "1/1/2026 12:00:00 PM");
    r.insert("source", "libre.current");
    return r;
}

QJsonObject eventRecord()
{
    QJsonObject payload;
    payload.insert("entry_type", 3);
    payload.insert("value", 112);
    payload.insert("unit", "mg/dL");
    QJsonObject raw;
    raw.insert("type", 3);
    payload.insert("raw", raw);
    QJsonObject r;
    r.insert("shape", "event");
    r.insert("subject_ref", "S-0001");
    r.insert("metric", "logbook_entry");
    r.insert("payload", payload);
    r.insert("measured_at", "2026-01-01T11:00:00Z");
    r.insert("original_timestamp", "1/1/2026 11:00:00 AM");
    r.insert("source", "libre.logbook");
    return r;
}

// Everything a test needs around one link.
struct Rig
{
    Rig() : link(&policy), channel(0), wasKilled(false), startDone(false)
    {
        link.setRecordSink(&sinks);
        link.setQuarantineSink(&sinks);
        link.setClock([]() { return UtcTime::fromIso("2026-01-01T12:05:00Z"); });
        link.setChannelFactory([this](const AdapterEntry &) {
            channel = new FakeChannel;
            channels.append(channel);
            channel->silentStart = silentStart;
            channel->startFails = startFails;
            if (helloBeforeStarted)
                channel->outputBeforeStarted = encodeJsonLine(hello());
            channel->onKill = [this]() { wasKilled = true; sentAtKill = channel->sent(); };
            return static_cast<LinkChannel *>(channel);
        });
        QObject::connect(&link, &AdapterLink::diagnostic,
                         [this](const QString &s, const QString &l, const QString &t) {
                             diagnostics << s + " " + l + ": " + t;
                         });
        silentStart = false;
        startFails = false;
        helloBeforeStarted = false;
    }

    void begin(const QString &adapter = "libre")
    {
        startDone = false;
        link.start(adapter, "conn-1", QJsonObject(),
                   [this](const StartResult &r, const QString &d) {
                       startDone = true;
                       startResult = r;
                       startDetail = d;
                   });
    }

    QJsonObject hello(const QString &id = "a1") const
    {
        return buildMessage(MessageType::Hello, id, "", helloPayload());
    }

    // hello, accept, configure, reply: the link is ready.
    bool bringUp()
    {
        begin();
        if (!channel)
            return false;
        channel->sayJson(hello());
        if (channel->sent().size() < 2 || channel->lastType() != "configure")
            return false;
        channel->sayJson(buildMessage(MessageType::Reply, "a2", channel->lastId()));
        return startDone && startResult.kind == StartResult::Admitted;
    }

    void say(MessageType type, const QString &correlation, const QJsonObject &payload = QJsonObject())
    {
        channel->sayJson(buildMessage(type, "a" + QString::number(++adapterIds + 10), correlation, payload));
    }

    void sayRecords(const QString &correlation, const QString &feed, const QJsonArray &records)
    {
        QJsonObject p;
        p.insert("feed", feed);
        p.insert("records", records);
        say(MessageType::Records, correlation, p);
    }

    void sayCompletion(const QString &correlation, int sent, const QString &watermark = "2026-01-01T12:00:00Z",
                       const QString &feed = "current")
    {
        QJsonObject cov;
        cov.insert("feed", feed);
        cov.insert("from", "2026-01-01T12:00:00Z");
        cov.insert("to", "2026-01-01T12:00:00Z");
        QJsonObject p;
        p.insert("records_sent", sent);
        p.insert("watermark", watermark.isEmpty() ? QJsonValue(QJsonValue::Null) : QJsonValue(watermark));
        p.insert("coverage", QJsonArray() << cov);
        say(MessageType::Reply, correlation, p);
    }

    // Starts a fetch_current and records the result.
    void fetch()
    {
        fetchDone = false;
        link.fetchCurrent("S-0001", [this](const CommandReport &r, const FetchCompletion &c) {
            fetchDone = true;
            report = r;
            completion = c;
        });
    }

    RecordingPolicy policy;
    RecordingSinks sinks;
    AdapterLink link;
    FakeChannel *channel;
    QList<FakeChannel *> channels;     // every channel made, oldest first (a killed one stays listed)
    bool silentStart, startFails;
    bool helloBeforeStarted;    // the adapter's hello arrives ahead of the `started` signal
    bool wasKilled;     // the link owns and deletes the channel; this outlives it
    QList<QJsonObject> sentAtKill;  // what the driver had written when the channel was killed
    bool startDone;
    StartResult startResult;
    QString startDetail;
    bool fetchDone;
    CommandReport report;
    FetchCompletion completion;
    QStringList diagnostics;
    int adapterIds = 0;
};

} // namespace

Q_DECLARE_METATYPE(lm::ViolationKind)
Q_DECLARE_METATYPE(lm::ErrorClass)

class TstAdapterLink : public QObject
{
    Q_OBJECT

private slots:
    // ---- Lifecycle ---------------------------------------------------------------

    void transcriptOfContractSection8()
    {
        Rig r;
        QVERIFY(r.bringUp());
        QCOMPARE(r.startResult.link.adapterId, QString("libre"));
        QCOMPARE(r.startResult.link.adapterVersion, QString("0.1.0"));
        QCOMPARE(r.startResult.feeds, QStringList() << "current" << "graph" << "logbook");
        QCOMPARE(r.startResult.metrics.size(), 2);
        QCOMPARE(r.policy.spawned.size(), 1);

        QList<QJsonObject> sent = r.channel->sent();
        QCOMPARE(sent.at(0).value("type").toString(), QString("accept"));
        QCOMPARE(sent.at(0).value("id").toString(), QString("c1"));
        QCOMPARE(sent.at(0).value("correlation_id").toString(), QString("a1"));
        QCOMPARE(sent.at(1).value("type").toString(), QString("configure"));
        QCOMPARE(sent.at(1).value("id").toString(), QString("c2"));
        QVERIFY(sent.at(1).value("settings").isObject());

        bool authDone = false;
        SessionGrant grant;
        CommandReport authReport;
        Credentials c;
        c.username = kUser;
        c.password = kPassword;
        r.link.authenticate(c, QString(), [&](const CommandReport &rep, const SessionGrant &g) {
            authDone = true;
            authReport = rep;
            grant = g;
        });
        QCOMPARE(r.channel->lastType(), QString("authenticate"));
        QCOMPARE(r.channel->lastId(), QString("c3"));
        QCOMPARE(r.channel->last().value("deadline_ms").toInt(), 150);
        QCOMPARE(r.channel->last().value("credentials").toObject().value("username").toString(),
                 QString(kUser));
        QVERIFY(r.channel->last().value("session").isNull());
        QJsonObject reply;
        reply.insert("session", kSession);
        reply.insert("session_expires", "2026-06-30T12:00:00Z");
        r.say(MessageType::Reply, "c3", reply);
        QVERIFY(authDone);
        QCOMPARE(authReport.outcome.result, CommandOutcome::Ok);
        QCOMPARE(grant.session, QString(kSession));
        QCOMPARE(grant.expiresAt.toIso(), QString("2026-06-30T12:00:00.000Z"));

        r.fetch();
        QCOMPARE(r.channel->lastType(), QString("fetch_current"));
        QCOMPARE(r.channel->lastId(), QString("c4"));
        r.sayRecords("c4", "current", QJsonArray() << sampleRecord());
        r.sayCompletion("c4", 1);
        QVERIFY(r.fetchDone);
        QCOMPARE(r.report.outcome.result, CommandOutcome::Ok);
        QCOMPARE(r.report.outcome.commandType, QString("fetch_current"));
        QCOMPARE(r.report.outcome.recordsReceived, qint64(1));
        QCOMPARE(r.report.outcome.recordsValid, qint64(1));
        QCOMPARE(r.report.outcome.batches, qint64(1));
        QCOMPARE(r.report.outcome.linesRead, qint64(2));
        QVERIFY(r.report.outcome.bytesRead > 100);
        QCOMPARE(r.report.outcome.killedBy, CommandOutcome::NotKilled);
        QCOMPARE(r.completion.recordsSent, qint64(1));
        QCOMPARE(r.completion.watermark.toIso(), QString("2026-01-01T12:00:00.000Z"));
        QCOMPARE(r.completion.coverage.size(), 1);
        QCOMPARE(r.sinks.valid.size(), 1);
        QCOMPARE(r.sinks.validFeeds.at(0), QString("current"));
        const LinkRecord rec = r.sinks.valid.at(0);
        QCOMPARE(rec.subjectRef, QString("S-0001"));
        QVERIFY(rec.record.subject.isNull());
        QCOMPARE(rec.record.value, 112.0);
        QCOMPARE(rec.record.unit, QString("mg/dL"));
        QCOMPARE(rec.record.source, QString("libre.current"));
        QCOMPARE(rec.record.originalTimestamp, QString("1/1/2026 12:00:00 PM"));
        QCOMPARE(rec.record.ingestedAt.toIso(), QString("2026-01-01T12:05:00.000Z"));
        QVERIFY(rec.record.origin == Origin::Adapter);
        QCOMPARE(r.policy.outcomes.size(), 2);      // authenticate and fetch; not configure

        bool stopped = false;
        r.link.stop([&]() { stopped = true; });
        QCOMPARE(r.channel->lastType(), QString("shutdown"));
        r.channel->exitWith(0);
        QVERIFY(stopped);
        QCOMPARE(r.policy.exited.size(), 1);
        QVERIFY(!r.channel || !r.link.isAlive());
        QVERIFY(r.policy.violations.isEmpty());
    }

    // cause: the adapter's first output can be read before the channel's `started`
    // signal is handled (both become ready in one pass of the event loop). The hello is the
    // answer to the spawn, so it must be accepted, not counted as "hello outside the start".
    void helloReadBeforeStartedIsAccepted()
    {
        Rig r;
        r.helloBeforeStarted = true;
        r.begin();
        QVERIFY(r.channel);
        QVERIFY2(!r.policy.saw(ViolationKind::UnknownType), "hello read before `started` was a violation");
        QVERIFY(r.policy.violations.isEmpty());
        QVERIFY(!r.wasKilled);
        QCOMPARE(r.channel->lastType(), QString("configure"));
        r.channel->sayJson(buildMessage(MessageType::Reply, "a2", r.channel->lastId()));
        QVERIFY(r.startDone);
        QCOMPARE(r.startResult.kind, StartResult::Admitted);
        QCOMPARE(r.policy.spawned.size(), 1);
    }

    // invariant: at most one start() per link at a time. A second start while the
    // first is still coming up is refused on its own callback and changes nothing: no second
    // process, no second `mayStart` question, no effect on the start in progress.
    void aSecondStartWhileStartingChangesNothing()
    {
        Rig r;
        r.begin();                              // Spawning, then WaitHello
        QCOMPARE(r.channels.size(), 1);
        bool secondDone = false;
        StartResult second;
        r.link.start("libre", "conn-1", QJsonObject(),
                     [&](const StartResult &res, const QString &) { secondDone = true; second = res; });
        QTRY_VERIFY(secondDone);
        QCOMPARE(second.kind, StartResult::Refused);
        QCOMPARE(r.channels.size(), 1);
        QCOMPARE(r.policy.starts.size(), 1);
        QVERIFY(!r.wasKilled);
        // The first start still completes normally.
        r.channel->sayJson(r.hello());
        QCOMPARE(r.channel->lastType(), QString("configure"));
        r.channel->sayJson(buildMessage(MessageType::Reply, "a2", r.channel->lastId()));
        QVERIFY(r.startDone);
        QCOMPARE(r.startResult.kind, StartResult::Admitted);
        QVERIFY(r.policy.violations.isEmpty());
    }

    // invariant: no message of an old or killed link is ever counted against a new
    // link. The first process is ended during its start; the link is started again; the old
    // channel then reports a late `started`, a line of standard error and a late hello. None
    // of that may move the new start on or reach the policy as news about the new link.
    void lateEventsOfAKilledChannelAreNotCountedAgainstTheNewStart()
    {
        Rig r;
        r.begin();
        FakeChannel *old = r.channel;
        const QString oldSession = r.startResult.link.sessionId;
        Q_UNUSED(oldSession);
        r.link.stop();                          // ends it during the start: killed, detached
        QVERIFY(r.wasKilled);
        QVERIFY(r.startDone);                   // the first start ended as refused, once

        r.silentStart = true;                   // the new process has not reported `started`
        r.wasKilled = false;
        r.begin();
        QCOMPARE(r.channels.size(), 2);
        QVERIFY(r.channel != old);
        const int spawnedBefore = r.policy.spawned.size();

        emit old->started();                    // late notification of the dead process
        old->sayStderr("a late line of the old process");
        old->pipe.push(encodeJsonLine(r.hello("old-hello")));
        old->exitWith(0);

        QCOMPARE(r.policy.spawned.size(), spawnedBefore);   // the new start was not advanced
        QVERIFY(r.policy.stderrBytes.isEmpty());            // nothing counted for the new link
        QVERIFY(r.policy.violations.isEmpty());
        QVERIFY(r.diagnostics.isEmpty());
        QVERIFY(r.channel->sent().isEmpty());               // nothing was answered to the old hello
        QVERIFY(!r.startDone);

        // The new process then starts and says hello as it should.
        emit r.channel->started();
        r.channel->sayJson(r.hello());
        QCOMPARE(r.channel->lastType(), QString("configure"));
        r.channel->sayJson(buildMessage(MessageType::Reply, "a2", r.channel->lastId()));
        QVERIFY(r.startDone);
        QCOMPARE(r.startResult.kind, StartResult::Admitted);
        QVERIFY(r.policy.violations.isEmpty());
    }

    void eventRecordsAreAccepted()
    {
        Rig r;
        QVERIFY(r.bringUp());
        r.fetch();
        r.sayRecords("c3", "logbook", QJsonArray() << eventRecord());
        r.sayCompletion("c3", 1, "2026-01-01T11:00:00Z", "logbook");
        QVERIFY(r.fetchDone);
        QCOMPARE(r.report.outcome.result, CommandOutcome::Ok);
        QCOMPARE(r.sinks.valid.size(), 1);
        QVERIFY(r.sinks.valid.at(0).record.shape == Record::Event);
    }

    void admissionRejections_data()
    {
        QTest::addColumn<QString>("what");
        QTest::addColumn<QString>("reason");
        QTest::newRow("other adapter") << "adapter-other" << "unknown_adapter";
        QTest::newRow("no version") << "no-version" << "bad_manifest";
        QTest::newRow("no metrics") << "no-metrics" << "bad_manifest";
        QTest::newRow("no feeds") << "no-feeds" << "bad_manifest";
        QTest::newRow("contract 1") << "contract-1" << "version";
        QTest::newRow("unknown metric") << "metric" << "metric";
        QTest::newRow("unit not accepted") << "unit" << "unit";
        QTest::newRow("feed with bad name") << "feed" << "feed";
        QTest::newRow("feed twice") << "feed-twice" << "feed";
        QTest::newRow("budget above ceiling") << "budget" << "budget";
    }

    void admissionRejections()
    {
        QFETCH(QString, what);
        QFETCH(QString, reason);
        Rig r;
        r.begin();
        QJsonObject p = helloPayload();
        if (what == "adapter-other") p.insert("adapter", "other");
        else if (what == "no-version") p.remove("adapter_version");
        else if (what == "no-metrics") p.remove("metrics");
        else if (what == "no-feeds") p.remove("feeds");
        else if (what == "contract-1") p.insert("contract_version", 1);
        else if (what == "metric") {
            QJsonObject m; m.insert("metric", "pulse"); m.insert("units", arrayOf(QStringList() << "bpm"));
            p.insert("metrics", QJsonArray() << m);
        } else if (what == "unit") {
            QJsonObject m; m.insert("metric", "glucose"); m.insert("units", arrayOf(QStringList() << "mg/dl"));
            p.insert("metrics", QJsonArray() << m);
        } else if (what == "feed") p.insert("feeds", arrayOf(QStringList() << "bad feed!"));
        else if (what == "feed-twice") p.insert("feeds", arrayOf(QStringList() << "current" << "current"));
        else if (what == "budget") {
            QJsonObject b; b.insert("max_records_per_batch", 501);
            p.insert("budgets", b);
        }
        r.channel->sayJson(buildMessage(MessageType::Hello, "a1", "", p));
        QVERIFY(r.startDone);
        QCOMPARE(r.startResult.kind, StartResult::Refused);
        QCOMPARE(r.startResult.refusal, StartResult::AdmissionRejected);
        QCOMPARE(r.startResult.rejectReason, reason);
        QCOMPARE(r.channel->lastType(), QString("reject"));
        QCOMPARE(r.channel->last().value("reason").toString(), reason);
        QCOMPARE(r.channel->last().value("correlation_id").toString(), QString("a1"));
        QVERIFY(!r.channel->last().value("detail").toString().isEmpty());
        // The adapter is ended within the end-of-session wait, killed if it does not go.
        QTRY_VERIFY_WITH_TIMEOUT(r.wasKilled, 2000);
        QTRY_COMPARE_WITH_TIMEOUT(r.policy.exited.size(), 1, 2000);
    }

    void effectiveBudgetsAreTheLowerOfRequestAndCeiling()
    {
        Rig r;
        r.begin();
        QJsonObject b;
        b.insert("max_records_per_batch", 100);
        b.insert("max_line_bytes", 50000);
        QJsonObject p = helloPayload();
        p.insert("budgets", b);
        r.channel->sayJson(buildMessage(MessageType::Hello, "a1", "", p));
        r.channel->sayJson(buildMessage(MessageType::Reply, "a2", r.channel->lastId()));
        QVERIFY(r.startDone);
        QCOMPARE(r.startResult.kind, StartResult::Admitted);
        QCOMPARE(r.startResult.budgets.maxRecordsPerBatch, 100);
        QCOMPARE(r.startResult.budgets.maxLineBytes, 50000);
    }

    void unknownAdapterIsNotSpawned()
    {
        Rig r;
        r.begin("nonexistent");
        QTRY_VERIFY(r.startDone);
        QCOMPARE(r.startResult.refusal, StartResult::AdmissionRejected);
        QCOMPARE(r.startResult.rejectReason, QString("unknown_adapter"));
        QVERIFY(!r.channel);
        QVERIFY(r.policy.starts.isEmpty());
    }

    void policyDeniesTheStart()
    {
        Rig r;
        r.policy.mayStartResult = MayStartResult::denied(MayStartResult::CircuitOpen);
        r.begin();
        QTRY_VERIFY(r.startDone);
        QCOMPARE(r.startResult.refusal, StartResult::Denied);
        QCOMPARE(r.startResult.denyReason, QString("circuit-open"));
        QVERIFY(!r.channel);
        QCOMPARE(r.policy.starts.size(), 1);
        QCOMPARE(r.policy.starts.at(0).adapterId, QString("libre"));
    }

    void helloTimeoutKillsTheProcess()
    {
        Rig r;
        r.begin();
        QVERIFY(!r.startDone);
        QTRY_VERIFY_WITH_TIMEOUT(r.startDone, 2000);
        QCOMPARE(r.startResult.refusal, StartResult::HelloTimeout);
        QVERIFY(r.wasKilled);
        QCOMPARE(r.policy.exited.size(), 1);
    }

    void spawnFailure()
    {
        Rig r;
        r.startFails = true;
        r.begin();
        QVERIFY(r.startDone);
        QCOMPARE(r.startResult.refusal, StartResult::SpawnFailed);
        QVERIFY(r.policy.spawned.isEmpty());
    }

    void exitBeforeHello()
    {
        Rig r;
        r.begin();
        r.channel->exitWith(3);
        QVERIFY(r.startDone);
        QCOMPARE(r.startResult.refusal, StartResult::SpawnFailed);
        QCOMPARE(r.policy.exitStatuses.at(0).exitCode, 3);
    }

    void configureDeadline()
    {
        Rig r;
        r.begin();
        r.channel->sayJson(r.hello());
        QTRY_VERIFY_WITH_TIMEOUT(r.startDone, 2000);
        QCOMPARE(r.startResult.refusal, StartResult::HelloTimeout);
        QVERIFY(r.wasKilled);
    }

    void oneCommandAtATime()
    {
        Rig r;
        QVERIFY(r.bringUp());
        r.fetch();
        bool second = false;
        CommandReport rep;
        r.link.discoverSubjects([&](const CommandReport &x, const QList<SubjectInfo> &) {
            second = true;
            rep = x;
        });
        QTRY_VERIFY(second);
        QCOMPARE(rep.outcome.errorClass, ErrorClass::InvalidCommand);
        QCOMPARE(rep.outcome.result, CommandOutcome::Error);
        QCOMPARE(r.channel->sent().size(), 3);      // accept, configure, fetch_current only
    }

    void commandBeforeStartIsInvalid()
    {
        Rig r;
        bool done = false;
        r.link.fetchCurrent("S-0001", [&](const CommandReport &x, const FetchCompletion &) {
            done = true;
            QCOMPARE(x.outcome.errorClass, ErrorClass::InvalidCommand);
        });
        QTRY_VERIFY(done);
    }

    // ---- Commands ----------------------------------------------------------------

    void discoverSubjects()
    {
        Rig r;
        QVERIFY(r.bringUp());
        bool done = false;
        QList<SubjectInfo> subjects;
        r.link.discoverSubjects([&](const CommandReport &rep, const QList<SubjectInfo> &s) {
            done = true;
            QCOMPARE(rep.outcome.result, CommandOutcome::Ok);
            subjects = s;
        });
        QCOMPARE(r.channel->lastType(), QString("discover_subjects"));
        QJsonObject s1, s2;
        s1.insert("subject_ref", "S-0001"); s1.insert("display_name", kName);
        s1.insert("unit", "mg/dL"); s1.insert("unit_basis", "code");
        s2.insert("subject_ref", "S-0002"); s2.insert("display_name", "Second Person");
        s2.insert("unit", QJsonValue(QJsonValue::Null)); s2.insert("unit_basis", "none");
        QJsonObject p;
        p.insert("subjects", QJsonArray() << s1 << s2);
        r.say(MessageType::Reply, "c3", p);
        QVERIFY(done);
        QCOMPARE(subjects.size(), 2);
        QCOMPARE(subjects.at(0).subjectRef, QString("S-0001"));
        QCOMPARE(subjects.at(0).displayName, QString(kName));
        QCOMPARE(subjects.at(0).unit, QString("mg/dL"));
        QCOMPARE(subjects.at(0).unitBasis, QString("code"));
        QVERIFY(subjects.at(1).unit.isEmpty());
    }

    void malformedReplyIsProtocolChanged()
    {
        Rig r;
        QVERIFY(r.bringUp());
        bool done = false;
        CommandReport rep;
        r.link.discoverSubjects([&](const CommandReport &x, const QList<SubjectInfo> &) { done = true; rep = x; });
        QJsonObject p;
        p.insert("subjects", "not an array");
        r.say(MessageType::Reply, "c3", p);
        QVERIFY(done);
        QCOMPARE(rep.outcome.result, CommandOutcome::Error);
        QCOMPARE(rep.outcome.errorClass, ErrorClass::ProtocolChanged);
    }

    void fetchRangeSendsTheRange()
    {
        Rig r;
        QVERIFY(r.bringUp());
        bool done = false;
        r.link.fetchRange("S-0001", UtcTime::fromIso("2026-01-01T00:00:00Z"),
                          UtcTime::fromIso("2026-01-01T12:00:00Z"), QStringList() << "graph",
                          [&](const CommandReport &, const FetchCompletion &) { done = true; });
        const QJsonObject m = r.channel->last();
        QCOMPARE(m.value("type").toString(), QString("fetch_range"));
        QCOMPARE(m.value("from").toString(), QString("2026-01-01T00:00:00.000Z"));
        QCOMPARE(m.value("to").toString(), QString("2026-01-01T12:00:00.000Z"));
        QCOMPARE(m.value("feeds").toArray().size(), 1);
        QCOMPARE(m.value("deadline_ms").toInt(), 150);
        QJsonObject sensor;
        sensor.insert("id", "ab12");
        QJsonObject cov;
        cov.insert("feed", "graph");
        cov.insert("from", "2026-01-01T00:30:00Z");
        cov.insert("to", "2026-01-01T11:55:00Z");
        QJsonObject p;
        p.insert("records_sent", 0);
        p.insert("watermark", QJsonValue(QJsonValue::Null));
        p.insert("coverage", QJsonArray() << cov);
        p.insert("sensor", sensor);
        r.say(MessageType::Reply, m.value("id").toString(), p);
        QVERIFY(done);
    }

    void invalidRangeIsRefusedBeforeTheWire()
    {
        Rig r;
        QVERIFY(r.bringUp());
        bool done = false;
        CommandReport rep;
        const UtcTime t = UtcTime::fromIso("2026-01-01T00:00:00Z");
        r.link.fetchRange("S-0001", t, t, QStringList(),
                          [&](const CommandReport &x, const FetchCompletion &) { done = true; rep = x; });
        QTRY_VERIFY(done);
        QCOMPARE(rep.outcome.errorClass, ErrorClass::InvalidCommand);
        QCOMPARE(r.channel->sent().size(), 2);
        QVERIFY(r.link.isIdle());
    }

    void completionWithSensor()
    {
        Rig r;
        QVERIFY(r.bringUp());
        r.fetch();
        QJsonObject sensor;
        sensor.insert("id", "ab12");
        sensor.insert("warmup_minutes", 60);
        QJsonObject cov;
        cov.insert("feed", "current");
        cov.insert("from", "2026-01-01T12:00:00Z");
        cov.insert("to", "2026-01-01T12:00:00Z");
        QJsonObject p;
        p.insert("records_sent", 0);
        p.insert("watermark", QJsonValue(QJsonValue::Null));
        p.insert("coverage", QJsonArray() << cov);
        p.insert("sensor", sensor);
        r.say(MessageType::Reply, "c3", p);
        QVERIFY(r.fetchDone);
        QCOMPARE(r.completion.sensor.value("warmup_minutes").toInt(), 60);
        QVERIFY(!r.completion.watermark.isValid());
    }

    // the trend arrow of the completion of fetch_current reaches the caller as
    // an integer the driver does not interpret; null or absent means none; anything else is a
    // malformed reply (the adapter is untrusted input).
    void completionCarriesTheTrendArrow()
    {
        Rig r;
        QVERIFY(r.bringUp());
        r.fetch();
        QJsonObject p;
        p.insert("records_sent", 0);
        p.insert("watermark", QJsonValue(QJsonValue::Null));
        p.insert("coverage", QJsonArray());
        p.insert("trend", 4);
        r.say(MessageType::Reply, "c3", p);
        QVERIFY(r.fetchDone);
        QCOMPARE(r.report.outcome.result, CommandOutcome::Ok);
        QVERIFY(r.completion.hasTrend);
        QCOMPARE(r.completion.trend, 4);
    }

    void completionWithNullOrNoTrendHasNone()
    {
        for (int variant = 0; variant < 2; ++variant) {
            Rig r;
            QVERIFY(r.bringUp());
            r.fetch();
            QJsonObject p;
            p.insert("records_sent", 0);
            p.insert("watermark", QJsonValue(QJsonValue::Null));
            p.insert("coverage", QJsonArray());
            if (variant == 0)
                p.insert("trend", QJsonValue(QJsonValue::Null));
            r.say(MessageType::Reply, "c3", p);
            QVERIFY(r.fetchDone);
            QCOMPARE(r.report.outcome.result, CommandOutcome::Ok);
            QVERIFY(!r.completion.hasTrend);
        }
    }

    void completionWithAMalformedTrendIsAProtocolChange_data()
    {
        QTest::addColumn<QJsonValue>("trend");
        QTest::newRow("string") << QJsonValue(QStringLiteral("3"));
        QTest::newRow("fraction") << QJsonValue(2.5);
        QTest::newRow("object") << QJsonValue(QJsonObject());
        QTest::newRow("too large") << QJsonValue(4e12);
    }

    void completionWithAMalformedTrendIsAProtocolChange()
    {
        QFETCH(QJsonValue, trend);
        Rig r;
        QVERIFY(r.bringUp());
        r.fetch();
        QJsonObject p;
        p.insert("records_sent", 0);
        p.insert("watermark", QJsonValue(QJsonValue::Null));
        p.insert("coverage", QJsonArray());
        p.insert("trend", trend);
        r.say(MessageType::Reply, "c3", p);
        QVERIFY(r.fetchDone);
        QCOMPARE(r.report.outcome.result, CommandOutcome::Error);
        QCOMPARE(r.report.outcome.errorClass, ErrorClass::ProtocolChanged);
        QVERIFY(!r.completion.hasTrend);
    }

    // ---- Errors and deadlines ----------------------------------------------------

    void errorClasses_data()
    {
        QTest::addColumn<QString>("name");
        QTest::addColumn<ErrorClass>("errorClass");
        QTest::newRow("auth") << "auth-failed" << ErrorClass::AuthFailed;
        QTest::newRow("terms") << "terms-must-be-accepted" << ErrorClass::TermsMustBeAccepted;
        QTest::newRow("rate") << "rate-limited" << ErrorClass::RateLimited;
        QTest::newRow("transient") << "transient" << ErrorClass::Transient;
        QTest::newRow("protocol") << "protocol-changed" << ErrorClass::ProtocolChanged;
        QTest::newRow("invalid") << "invalid-command" << ErrorClass::InvalidCommand;
    }

    void errorClasses()
    {
        QFETCH(QString, name);
        QFETCH(ErrorClass, errorClass);
        Rig r;
        QVERIFY(r.bringUp());
        r.fetch();
        QJsonObject p;
        p.insert("class", name);
        p.insert("detail", "HTTP 429");
        p.insert("retry_after_s", 120);
        r.say(MessageType::Error, "c3", p);
        QVERIFY(r.fetchDone);
        QCOMPARE(r.report.outcome.result, CommandOutcome::Error);
        QCOMPARE(r.report.outcome.errorClass, errorClass);
        QCOMPARE(r.report.outcome.retryAfterS, 120);
        QCOMPARE(r.report.detail, QString("HTTP 429"));
        QCOMPARE(r.policy.outcomes.size(), 1);
        QVERIFY(r.link.isIdle());       // an error does not end the link
        QVERIFY(r.completion.watermark.isValid() == false);
    }

    void unknownErrorClassIsProtocolChanged()
    {
        Rig r;
        QVERIFY(r.bringUp());
        r.fetch();
        QJsonObject p;
        p.insert("class", "melted");
        r.say(MessageType::Error, "c3", p);
        QCOMPARE(r.report.outcome.errorClass, ErrorClass::ProtocolChanged);
        QCOMPARE(r.report.outcome.retryAfterS, -1);
    }

    void commandDeadlineKillsAndCountsTransient()
    {
        Rig r;
        QVERIFY(r.bringUp());
        r.fetch();
        QVERIFY(!r.fetchDone);
        QTRY_VERIFY_WITH_TIMEOUT(r.fetchDone, 2000);
        QCOMPARE(r.report.outcome.result, CommandOutcome::Error);
        QCOMPARE(r.report.outcome.errorClass, ErrorClass::Transient);
        QCOMPARE(r.report.outcome.killedBy, CommandOutcome::Deadline);
        QVERIFY(r.report.outcome.durationMs >= 100);
        QVERIFY(r.wasKilled);
        QVERIFY(!r.link.isAlive());
        QCOMPARE(r.policy.exited.size(), 1);
        QCOMPARE(r.policy.outcomes.size(), 1);
        QCOMPARE(r.policy.outcomes.at(0).killedBy, CommandOutcome::Deadline);
    }

    void abnormalExitDuringCommand()
    {
        Rig r;
        QVERIFY(r.bringUp());
        r.fetch();
        r.channel->exitWith(9);
        QVERIFY(r.fetchDone);
        QCOMPARE(r.report.outcome.errorClass, ErrorClass::Transient);
        QCOMPARE(r.report.outcome.killedBy, CommandOutcome::Exit);
        QCOMPARE(r.policy.exitStatuses.at(0).exitCode, 9);
        QVERIFY(!r.link.isAlive());
    }

    // ---- Violations -----------------------------------------

    void violations_data()
    {
        QTest::addColumn<QByteArray>("line");
        QTest::addColumn<ViolationKind>("kind");
        QTest::addColumn<bool>("hard");
        QTest::newRow("malformed json") << QByteArray("{\"version\":0,\n") << ViolationKind::MalformedFraming << true;
        QTest::newRow("invalid utf-8") << QByteArray("{\"version\":0,\"id\":\"\xff\"}\n") << ViolationKind::MalformedFraming << true;
        QTest::newRow("non-protocol output") << QByteArray("Starting adapter...\n") << ViolationKind::NonProtocolOutput << true;
        QTest::newRow("unknown type") << QByteArray("{\"version\":0,\"id\":\"a9\",\"type\":\"dance\",\"correlation_id\":\"\"}\n") << ViolationKind::UnknownType << true;
        QTest::newRow("wrong version") << QByteArray("{\"version\":7,\"id\":\"a9\",\"type\":\"log\",\"correlation_id\":\"\"}\n") << ViolationKind::BadHeader << true;
        QTest::newRow("missing id") << QByteArray("{\"version\":0,\"type\":\"log\",\"correlation_id\":\"\"}\n") << ViolationKind::BadHeader << true;
        QTest::newRow("invalid id") << QByteArray("{\"version\":0,\"id\":\"a b\",\"type\":\"log\",\"correlation_id\":\"\"}\n") << ViolationKind::BadHeader << true;
        QTest::newRow("core type from adapter") << QByteArray("{\"version\":0,\"id\":\"a9\",\"type\":\"accept\",\"correlation_id\":\"\"}\n") << ViolationKind::UnknownType << true;
        QTest::newRow("second hello") << QByteArray("{\"version\":0,\"id\":\"a9\",\"type\":\"hello\",\"correlation_id\":\"\"}\n") << ViolationKind::UnknownType << true;
        QTest::newRow("unmatched correlation") << QByteArray("{\"version\":0,\"id\":\"a9\",\"type\":\"reply\",\"correlation_id\":\"c99\"}\n") << ViolationKind::UnmatchedCorrelation << true;
        QTest::newRow("unsolicited records") << QByteArray("{\"version\":0,\"id\":\"a9\",\"type\":\"records\",\"correlation_id\":\"\",\"feed\":\"current\",\"records\":[]}\n") << ViolationKind::UnmatchedCorrelation << true;
        QTest::newRow("bad log payload") << QByteArray("{\"version\":0,\"id\":\"a9\",\"type\":\"log\",\"correlation_id\":\"\",\"level\":\"loud\",\"text\":\"x\"}\n") << ViolationKind::MalformedFraming << true;
    }

    void violations()
    {
        QFETCH(QByteArray, line);
        QFETCH(ViolationKind, kind);
        QFETCH(bool, hard);
        Rig r;
        QVERIFY(r.bringUp());
        r.fetch();
        r.channel->say(line);
        QVERIFY(r.policy.saw(kind));
        QCOMPARE(r.policy.violations.last().severity, hard ? Severity::Hard : Severity::Soft);
        QCOMPARE(r.policy.violations.last().link.adapterId, QString("libre"));
        QVERIFY(r.policy.violations.last().time.isValid());
        // A violation is reported, not acted on: the command is still open.
        QVERIFY(!r.fetchDone);
        QCOMPARE(r.wasKilled, false);
        // The next valid exchange still works, and the counters are in the outcome.
        r.sayCompletion("c3", 0, QString());
        QVERIFY(r.fetchDone);
        QCOMPARE(r.report.outcome.hardViolations, hard ? 1 : 0);
    }

    void duplicateCompletion()
    {
        Rig r;
        QVERIFY(r.bringUp());
        r.fetch();
        r.sayCompletion("c3", 0, QString());
        QVERIFY(r.fetchDone);
        r.sayCompletion("c3", 0, QString());
        QVERIFY(r.policy.saw(ViolationKind::DuplicateCompletion));
        QCOMPARE(r.policy.violations.last().severity, Severity::Hard);
    }

    void lineOverTheLimitKillsAtOnce()
    {
        Rig r;
        r.policy.lim.maxLineBytes = 2000;
        QVERIFY(r.bringUp());
        r.fetch();
        r.channel->say(QByteArray(2500, 'x') + "\n");
        QVERIFY(r.fetchDone);
        QCOMPARE(r.policy.violations.first().kind, ViolationKind::LineTooLong);
        QCOMPARE(r.policy.violations.first().severity, Severity::Hard);
        QCOMPARE(r.report.outcome.errorClass, ErrorClass::ProtocolChanged);
        QCOMPARE(r.report.outcome.killedBy, CommandOutcome::DriverLimit);
        QCOMPARE(killedByName(r.report.outcome.killedBy), QString("driver-limit"));
        QVERIFY(r.wasKilled);
    }

    // The driver's own limit wins over the policy's answer: even a policy that answers
    // kill (as NullPolicy does) leaves killed_by at driver-limit, and the violation is
    // still reported first.
    void lineOverTheLimitStaysDriverLimitWhenThePolicyKills()
    {
        Rig r;
        r.policy.lim.maxLineBytes = 2000;
        QVERIFY(r.bringUp());
        r.fetch();
        r.policy.violationActions << Action::kill();
        r.channel->say(QByteArray(2500, 'x') + "\n");
        QVERIFY(r.fetchDone);
        QCOMPARE(r.policy.violations.first().kind, ViolationKind::LineTooLong);
        QCOMPARE(r.report.outcome.killedBy, CommandOutcome::DriverLimit);
        QCOMPARE(r.report.outcome.hardViolations, 1);
    }

    // ---- the resident-size limit --------------------------------------------------

    // Above the limit the driver ends the process by its own rule: a driver-limit kill, the
    // command fails, and the guard hears of the exit with the peak and the reason.
    void residentSizeAboveTheLimitEndsTheAdapterAsADriverLimitKill()
    {
        Rig r;
        r.policy.lim.residentLimitBytes = 1000;
        r.policy.lim.memoryCheckIntervalMs = 20;
        QVERIFY(r.bringUp());
        r.fetch();
        r.channel->peak = 5000;
        QTRY_VERIFY_WITH_TIMEOUT(r.fetchDone, 2000);
        QCOMPARE(r.report.outcome.killedBy, CommandOutcome::DriverLimit);
        QCOMPARE(r.report.outcome.errorClass, ErrorClass::ProtocolChanged);
        QVERIFY(r.wasKilled);
        QTRY_COMPARE_WITH_TIMEOUT(r.policy.exitStatuses.size(), 1, 2000);
        QVERIFY(r.policy.exitStatuses.first().killedForMemory);
        QCOMPARE(r.policy.exitStatuses.first().peakResidentBytes, qint64(5000));
    }

    // Below the limit nothing happens, and an orderly stop reports the peak the driver saw.
    void residentSizeBelowTheLimitIsLeftAloneAndReportedAtStop()
    {
        Rig r;
        r.policy.lim.residentLimitBytes = 1000;
        r.policy.lim.memoryCheckIntervalMs = 20;
        QVERIFY(r.bringUp());
        r.channel->peak = 900;
        QTest::qWait(120);
        QVERIFY(!r.wasKilled);
        r.channel->peak = 950;
        bool stopped = false;
        r.link.stop([&]() { stopped = true; });
        QTRY_VERIFY_WITH_TIMEOUT(stopped, 2000);                 // the fake adapter never answers: killed after the wait
        QTRY_COMPARE_WITH_TIMEOUT(r.policy.exitStatuses.size(), 1, 2000);
        QVERIFY(!r.policy.exitStatuses.first().killedForMemory);
        QCOMPARE(r.policy.exitStatuses.first().peakResidentBytes, qint64(950));
    }

    // No limit (0) means no check at all: a huge size is not a reason to end anything.
    void noResidentLimitMeansNoCheck()
    {
        Rig r;
        r.policy.lim.residentLimitBytes = 0;
        r.policy.lim.memoryCheckIntervalMs = 20;
        QVERIFY(r.bringUp());
        r.channel->peak = Q_INT64_C(1) << 40;
        QTest::qWait(120);
        QVERIFY(!r.wasKilled);
    }

    // The kill needs no command in flight: an idle adapter that grows is ended too, and the
    // size is read again at the next start (a new process starts from nothing).
    void anIdleAdapterThatGrowsIsEndedAndTheNextStartBeginsFresh()
    {
        Rig r;
        r.policy.lim.residentLimitBytes = 1000;
        r.policy.lim.memoryCheckIntervalMs = 20;
        QVERIFY(r.bringUp());
        r.channel->peak = 2000;
        QTRY_VERIFY_WITH_TIMEOUT(r.wasKilled, 2000);
        QTRY_COMPARE_WITH_TIMEOUT(r.policy.exitStatuses.size(), 1, 2000);
        QVERIFY(r.policy.exitStatuses.first().killedForMemory);
        r.wasKilled = false;
        // The rig's kill hook stores what was sent into a member that is destroyed before the
        // link; empty it so that the second process, killed when the rig goes, writes into
        // nothing that was freed.
        r.sentAtKill.clear();
        QVERIFY(r.bringUp());                                    // a second process
        r.channel->peak = 300;
        QTest::qWait(120);
        QVERIFY(!r.wasKilled);
    }

    // The reader of /proc: the text of a status file, and the real file of this very process.
    void vmHwmIsReadFromAStatusFile()
    {
        QCOMPARE(parseVmHwmBytes("Name:\tx\nVmPeak:\t  479000 kB\nVmHWM:\t   26124 kB\nVmRSS:\t 25000 kB\n"),
                 qint64(26124) * 1024);
        QCOMPARE(parseVmHwmBytes("Name:\tx\nVmRSS:\t 25000 kB\n"), qint64(-1));          // no such line
        QCOMPARE(parseVmHwmBytes("VmHWM:\t   12 MB\n"), qint64(-1));                       // not kB
        QCOMPARE(parseVmHwmBytes("VmHWM:\t   abc kB\n"), qint64(-1));
        QCOMPARE(parseVmHwmBytes(QByteArray()), qint64(-1));
        QVERIFY(readPeakResidentBytes(QCoreApplication::applicationPid()) > 1024 * 1024);
        QCOMPARE(readPeakResidentBytes(0), qint64(-1));
        QCOMPARE(readPeakResidentBytes(-5), qint64(-1));
    }

    void aRealChildReportsItsPeakOnlyWhileItRuns()
    {
        ProcessChannel channel("/bin/sh", QStringList() << "-c" << "sleep 5");
        QCOMPARE(channel.peakResidentBytes(), qint64(-1));       // not started
        bool started = false;
        connect(&channel, &LinkChannel::started, [&]() { started = true; });
        channel.open();
        QTRY_VERIFY_WITH_TIMEOUT(started, 5000);
        QVERIFY(channel.peakResidentBytes() > 0);
        channel.kill();
        QTRY_COMPARE_WITH_TIMEOUT(channel.peakResidentBytes(), qint64(-1), 5000);
    }

    void unterminatedLineOverTheLimitKills()
    {
        Rig r;
        r.policy.lim.maxLineBytes = 2000;
        QVERIFY(r.bringUp());
        r.fetch();
        r.channel->say(QByteArray(1500, 'x'));
        QVERIFY(!r.fetchDone);
        r.channel->say(QByteArray(1500, 'x'));
        QVERIFY(r.fetchDone);
        QVERIFY(r.policy.saw(ViolationKind::LineTooLong));
    }

    void budgetLowersTheLineLimitAfterAccept()
    {
        Rig r;
        r.begin();
        QJsonObject b;
        b.insert("max_line_bytes", 1500);
        QJsonObject p = helloPayload();
        p.insert("budgets", b);
        r.channel->sayJson(buildMessage(MessageType::Hello, "a1", "", p));
        r.channel->sayJson(buildMessage(MessageType::Reply, "a2", r.channel->lastId()));
        QVERIFY(r.startDone);
        r.fetch();
        r.channel->say(QByteArray(1600, 'x') + "\n");
        QVERIFY(r.policy.saw(ViolationKind::LineTooLong));
    }

    void undeclaredMetricUnitAndFeed()
    {
        Rig r;
        QVERIFY(r.bringUp());
        r.fetch();
        QJsonObject badMetric = sampleRecord();
        badMetric.insert("metric", "pulse");
        QJsonObject badUnit = sampleRecord();
        badUnit.insert("unit", "mmol/l");
        QJsonObject badSource = sampleRecord();
        badSource.insert("source", "libre.secret");
        QJsonObject otherAdapter = sampleRecord();
        otherAdapter.insert("source", "other.current");
        r.sayRecords("c3", "current", QJsonArray() << badMetric << badUnit << badSource << otherAdapter << sampleRecord());
        QVERIFY(r.policy.saw(ViolationKind::UndeclaredMetric));
        QVERIFY(r.policy.saw(ViolationKind::UndeclaredUnit));
        QVERIFY(r.policy.saw(ViolationKind::UndeclaredFeed));
        int hard = 0;
        for (int i = 0; i < r.policy.violations.size(); ++i)
            hard += r.policy.violations.at(i).severity == Severity::Hard;
        QCOMPARE(hard, 4);
        // Violation and quarantine at once: each offending record is delivered to the
        // quarantine sink unmodified, with its own reason; the good one is delivered.
        QCOMPARE(r.sinks.reasons, QStringList() << "UndeclaredMetric" << "UndeclaredUnit"
                                                << "UndeclaredFeed" << "UndeclaredFeed");
        QCOMPARE(r.sinks.rawRecords.at(0), badMetric);
        QCOMPARE(r.sinks.rawRecords.at(1), badUnit);
        QCOMPARE(r.sinks.rawRecords.at(2), badSource);
        QCOMPARE(r.sinks.rawRecords.at(3), otherAdapter);
        QCOMPARE(r.sinks.valid.size(), 1);
    }

    void undeclaredBatchFeed()
    {
        Rig r;
        QVERIFY(r.bringUp());
        r.fetch();
        r.sayRecords("c3", "secret", QJsonArray() << sampleRecord());
        QVERIFY(r.policy.saw(ViolationKind::UndeclaredFeed));
        QVERIFY(r.sinks.valid.isEmpty());
    }

    // every record of a batch for an undeclared feed is quarantined, unmodified, with
    // UndeclaredFeed; the violation is reported once for the batch, not once per record.
    void undeclaredBatchFeedQuarantinesEveryRecord()
    {
        Rig r;
        QVERIFY(r.bringUp());
        r.fetch();
        const QJsonObject a = sampleRecord(100), b = sampleRecord(101, "2026-01-01T12:01:00Z");
        QJsonObject odd = sampleRecord(102);
        odd.insert("extra_field", "kept as received");
        r.sayRecords("c3", "secret", QJsonArray() << a << b << odd);
        QCOMPARE(r.policy.violations.size(), 1);
        QCOMPARE(r.policy.violations.at(0).kind, ViolationKind::UndeclaredFeed);
        QCOMPARE(r.sinks.reasons, QStringList() << "UndeclaredFeed" << "UndeclaredFeed" << "UndeclaredFeed");
        QCOMPARE(r.sinks.rawRecords.at(0), a);
        QCOMPARE(r.sinks.rawRecords.at(1), b);
        QCOMPARE(r.sinks.rawRecords.at(2), odd);
        QVERIFY(r.sinks.valid.isEmpty());
        QVERIFY(!r.wasKilled);
        // Counted like any other batch: three received, none valid, three quarantined; the
        // claim of three records_sent therefore matches and the burst rule (3 of 3) ends the
        // fetch as protocol-changed.
        r.sayCompletion("c3", 3);
        QVERIFY(r.fetchDone);
        QCOMPARE(r.report.outcome.recordsReceived, qint64(3));
        QCOMPARE(r.report.outcome.recordsValid, qint64(0));
        QCOMPARE(r.report.outcome.quarantinedByReason.value(int(Validation::UndeclaredFeed)), qint64(3));
        QCOMPARE(r.report.outcome.hardViolations, 1);
        QCOMPARE(r.report.outcome.errorClass, ErrorClass::ProtocolChanged);
    }

    // The records are quarantined before the violation is reported, so a kill on that very
    // violation still leaves them in quarantine; nothing is delivered as valid.
    void undeclaredBatchFeedThenKillKeepsTheQuarantine()
    {
        Rig r;
        RecordingRejectedSink rejected;
        r.link.setRejectedBatchSink(&rejected);
        QVERIFY(r.bringUp());
        r.fetch();
        r.policy.violationActions << Action::kill();
        r.sayRecords("c3", "secret", QJsonArray() << sampleRecord() << sampleRecord(101));
        QVERIFY(r.fetchDone);
        QCOMPARE(r.report.outcome.killedBy, CommandOutcome::Guard);
        QCOMPARE(r.sinks.reasons.size(), 2);
        QVERIFY(r.sinks.valid.isEmpty());
        QCOMPARE(rejected.lines.size(), 1);
        QVERIFY(rejected.hadViolation.at(0));
        QCOMPARE(rejected.kinds.at(0), ViolationKind::UndeclaredFeed);
    }

    // A batch without any `feed` key stays a framing violation: there is no feed to name,
    // so nothing is quarantined.
    void batchWithoutFeedKeyStaysMalformedFraming()
    {
        Rig r;
        QVERIFY(r.bringUp());
        r.fetch();
        QJsonObject p;
        p.insert("records", QJsonArray() << sampleRecord());
        r.say(MessageType::Records, "c3", p);
        QCOMPARE(r.policy.violations.size(), 1);
        QCOMPARE(r.policy.violations.at(0).kind, ViolationKind::MalformedFraming);
        QVERIFY(r.sinks.reasons.isEmpty());
    }

    // ---- RejectedBatchSink (REQ-GRD-13) -----------------------------------------

    // A kill at a bad record in the middle of a batch hands the line, exactly as received,
    // to the sink, once. The batch behaviour is the one built before: nothing delivered.
    void killInTheMiddleOfABatchHandsOverTheRawLine()
    {
        Rig r;
        RecordingRejectedSink rejected;
        r.link.setRejectedBatchSink(&rejected);
        QVERIFY(r.bringUp());
        r.fetch();
        r.policy.violationActions << Action::kill();
        QJsonObject badMetric = sampleRecord();
        badMetric.insert("metric", "pulse");
        QJsonObject p;
        p.insert("feed", "current");
        p.insert("records", QJsonArray() << sampleRecord() << badMetric << badMetric);
        const QByteArray wire = encodeJsonLine(buildMessage(MessageType::Records, "a77", "c3", p));
        r.channel->say(wire);
        QVERIFY(r.fetchDone);
        QCOMPARE(rejected.lines.size(), 1);
        QCOMPARE(rejected.lines.at(0), wire.left(wire.size() - 1));     // no line end
        QCOMPARE(rejected.killedBy.at(0), CommandOutcome::Guard);
        QVERIFY(rejected.hadViolation.at(0));
        QCOMPARE(rejected.kinds.at(0), ViolationKind::UndeclaredMetric);
        QVERIFY(r.sinks.valid.isEmpty());
        QCOMPARE(r.sinks.reasons, QStringList() << "UndeclaredMetric");     // stays quarantined
        QCOMPARE(r.policy.outcomes.size(), 1);
    }

    // The line the sink receives is what the report is built from (REQ-GRD-15): the driver
    // and the report agree on the form of the batch, and the reference of the Subject in it
    // does not come out again.
    void handedOverLineMakesAnAnonymizedReport()
    {
        Rig r;
        RecordingRejectedSink rejected;
        r.link.setRejectedBatchSink(&rejected);
        QVERIFY(r.bringUp());
        r.fetch();
        r.policy.violationActions << Action::kill();
        QJsonObject badMetric = sampleRecord(140, "2026-01-01T12:02:00Z");
        badMetric.insert("metric", "pulse");
        r.sayRecords("c3", "current", QJsonArray() << sampleRecord(120) << badMetric);
        QCOMPARE(rejected.lines.size(), 1);
        DiagnosticContext c;
        c.adapterId = "libre";
        c.commandType = r.report.outcome.commandType;
        c.declaredFeeds = r.startResult.feeds;
        c.declaredMetrics << "glucose" << "logbook_entry";
        c.declaredUnits << "mg/dL" << "mmol/L";
        const QByteArray out = buildDiagnosticReport(rejected.lines.at(0), c);
        QVERIFY(out.contains("\"status\":\"ok\""));
        QVERIFY(out.contains("\"value\":120"));
        QVERIFY(out.contains("\"value\":140"));
        QVERIFY(out.contains("t+120 s"));
        QVERIFY(!out.contains("S-0001"));
        QVERIFY(!out.contains("2026"));
        QVERIFY(!out.contains("pulse"));
    }

    // The policy's own batch check can end the process before any record is looked at.
    void killOnTheBatchCheckHandsOverTheLineWithoutAViolation()
    {
        Rig r;
        RecordingRejectedSink rejected;
        r.link.setRejectedBatchSink(&rejected);
        QVERIFY(r.bringUp());
        r.fetch();
        r.policy.batchActions << Action::kill();
        r.sayRecords("c3", "current", QJsonArray() << sampleRecord());
        QVERIFY(r.fetchDone);
        QCOMPARE(rejected.lines.size(), 1);
        QVERIFY(!rejected.hadViolation.at(0));
        QVERIFY(r.sinks.valid.isEmpty());
    }

    // No kill, no line: a drop, a violation answered with proceed, a normal batch and a kill
    // that has nothing to do with a batch (a kill at a completion) hand nothing over.
    void noKillInABatchHandsOverNothing()
    {
        Rig r;
        RecordingRejectedSink rejected;
        r.link.setRejectedBatchSink(&rejected);
        QVERIFY(r.bringUp());
        r.fetch();
        r.policy.batchActions << Action::drop();
        r.sayRecords("c3", "current", QJsonArray() << sampleRecord());
        QJsonObject badMetric = sampleRecord();
        badMetric.insert("metric", "pulse");
        r.policy.violationActions << Action::proceed();
        r.sayRecords("c3", "current", QJsonArray() << badMetric);
        r.sayRecords("c3", "current", QJsonArray() << sampleRecord());
        r.policy.violationActions << Action::kill();
        r.sayCompletion("c3", 99);          // soft violation, killed: not inside a batch
        QVERIFY(r.fetchDone);
        QVERIFY(r.wasKilled);
        QVERIFY(rejected.lines.isEmpty());
    }

    // The sink is optional: the same kill without one behaves as before.
    void killInTheMiddleOfABatchWorksWithoutASink()
    {
        Rig r;
        QVERIFY(r.bringUp());
        r.fetch();
        r.policy.violationActions << Action::kill();
        QJsonObject badMetric = sampleRecord();
        badMetric.insert("metric", "pulse");
        r.sayRecords("c3", "current", QJsonArray() << sampleRecord() << badMetric);
        QVERIFY(r.fetchDone);
        QCOMPARE(r.report.outcome.killedBy, CommandOutcome::Guard);
    }

    void recordsSentMismatchIsSoft()
    {
        Rig r;
        QVERIFY(r.bringUp());
        r.fetch();
        r.sayRecords("c3", "current", QJsonArray() << sampleRecord());
        r.sayCompletion("c3", 5);
        QVERIFY(r.fetchDone);
        QCOMPARE(r.report.outcome.result, CommandOutcome::Ok);
        QVERIFY(r.policy.saw(ViolationKind::RecordsSentMismatch));
        QCOMPARE(r.policy.violations.last().severity, Severity::Soft);
        QCOMPARE(r.report.outcome.softViolations, 1);
    }

    void batchTooLargeIsSoftAndStillProcessed()
    {
        Rig r;
        r.policy.lim.maxRecordsPerBatch = 2;
        QVERIFY(r.bringUp());
        r.fetch();
        r.sayRecords("c3", "current", QJsonArray() << sampleRecord(100) << sampleRecord(101) << sampleRecord(102));
        QVERIFY(r.policy.saw(ViolationKind::BatchTooLarge));
        QCOMPARE(r.policy.violations.last().severity, Severity::Soft);
        QCOMPARE(r.sinks.valid.size(), 3);
        QCOMPARE(r.policy.batches.size(), 1);
        QCOMPARE(r.policy.batches.at(0).second, 3);
    }

    // ---- Records ------------------------------------------------------------------

    void invalidRecordReasons_data()
    {
        QTest::addColumn<QString>("key");
        QTest::addColumn<QVariant>("value");       // invalid: removes the key; else sets it
        QTest::addColumn<QString>("reason");
        QTest::newRow("no subject") << "subject_ref" << QVariant() << "MissingSubject";
        QTest::newRow("metric not a string") << "metric" << QVariant(7) << "UnknownMetric";
        QTest::newRow("shape") << "shape" << QVariant("point") << "ShapeMismatch";
        QTest::newRow("no unit") << "unit" << QVariant() << "MissingUnit";
        QTest::newRow("value not a number") << "value" << QVariant("112") << "ValueNotFinite";
        QTest::newRow("value too low") << "value" << QVariant(5) << "ValueOutOfRange";
        QTest::newRow("value too high") << "value" << QVariant(900) << "ValueOutOfRange";
        QTest::newRow("bad timestamp") << "measured_at" << QVariant("2026-01-01 12:00") << "MalformedMeasurementTime";
        QTest::newRow("offset timestamp") << "measured_at" << QVariant("2026-01-01T12:00:00+02:00") << "MalformedMeasurementTime";
        QTest::newRow("no time") << "measured_at" << QVariant() << "MissingMeasurementTime";
        QTest::newRow("time not a string") << "measured_at" << QVariant(5) << "MissingMeasurementTime";
        QTest::newRow("future") << "measured_at" << QVariant("2026-01-01T13:00:00Z") << "MeasurementTimeInFuture";
        QTest::newRow("no original") << "original_timestamp" << QVariant() << "MissingOriginalTimestamp";
        QTest::newRow("no source") << "source" << QVariant() << "MissingSource";
    }

    void invalidRecordReasons()
    {
        QFETCH(QString, key);
        QFETCH(QVariant, value);
        QFETCH(QString, reason);
        Rig r;
        QVERIFY(r.bringUp());
        r.fetch();
        QJsonObject bad = sampleRecord();
        if (!value.isValid())
            bad.remove(key);
        else
            bad.insert(key, QJsonValue::fromVariant(value));
        r.sayRecords("c3", "current", QJsonArray() << bad << sampleRecord());
        r.sayCompletion("c3", 2);
        QVERIFY(r.fetchDone);
        QCOMPARE(r.sinks.reasons, QStringList() << reason);
        QCOMPARE(r.sinks.rawRecords.size(), 1);
        QCOMPARE(r.sinks.rawRecords.at(0), bad);        // unmodified (REQ-DAT-08)
        QCOMPARE(r.sinks.valid.size(), 1);
        QCOMPARE(r.report.outcome.recordsReceived, qint64(2));
        QCOMPARE(r.report.outcome.recordsValid, qint64(1));
        QCOMPARE(r.report.outcome.quarantinedByReason.size(), 1);
        QVERIFY(r.policy.violations.isEmpty());         // invalid records are not violations
        QCOMPARE(r.report.outcome.result, CommandOutcome::Ok);
    }

    void invalidEventReasons()
    {
        Rig r;
        QVERIFY(r.bringUp());
        r.fetch();
        QJsonObject noPayload = eventRecord();
        noPayload.remove("payload");
        QJsonObject badDuration = eventRecord();
        badDuration.insert("duration_ms", 1.5);
        QJsonObject badField = eventRecord();
        QJsonObject pl = badField.value("payload").toObject();
        pl.insert("surprise", 1);
        badField.insert("payload", pl);
        r.sayRecords("c3", "logbook", QJsonArray() << noPayload << badDuration << badField << eventRecord());
        r.sayCompletion("c3", 4, "2026-01-01T11:00:00Z", "logbook");
        QVERIFY(r.fetchDone);
        QCOMPARE(r.sinks.reasons, QStringList() << "InvalidPayload" << "InvalidDuration" << "InvalidPayload");
        QCOMPARE(r.sinks.valid.size(), 1);
        // 3 of 4 rejected: a burst.
        QCOMPARE(r.report.outcome.errorClass, ErrorClass::ProtocolChanged);
    }

    void burstOfRejectedRecordsIsProtocolChanged()
    {
        Rig r;
        QVERIFY(r.bringUp());
        r.fetch();
        QJsonObject bad = sampleRecord();
        bad.remove("unit");
        r.sayRecords("c3", "current", QJsonArray() << bad << bad << bad << sampleRecord());
        r.sayCompletion("c3", 4);
        QVERIFY(r.fetchDone);
        QCOMPARE(r.report.outcome.result, CommandOutcome::Error);
        QCOMPARE(r.report.outcome.errorClass, ErrorClass::ProtocolChanged);
        QCOMPARE(r.report.outcome.quarantinedByReason.value(Validation::MissingUnit), qint64(3));
        QCOMPARE(r.sinks.rawRecords.size(), 3);
        QCOMPARE(r.sinks.valid.size(), 1);      // what was valid was delivered
        QVERIFY(r.link.isIdle());
    }

    void fewRejectedAmongManyIsNotABurst()
    {
        Rig r;
        QVERIFY(r.bringUp());
        r.fetch();
        QJsonObject bad = sampleRecord();
        bad.remove("unit");
        QJsonArray records;
        for (int i = 0; i < 10; ++i)
            records << sampleRecord(100 + i);
        records << bad << bad;
        r.sayRecords("c3", "current", records);
        r.sayCompletion("c3", 12);
        QCOMPARE(r.report.outcome.result, CommandOutcome::Ok);
    }

    // Burst rule edges: at least three rejected and at least half
    // of the records examined. A feed with fewer than three records never escalates in the
    // driver, even if every record was rejected; the rejections are still counted.
    void burstRuleEdges_data()
    {
        QTest::addColumn<int>("rejected");
        QTest::addColumn<int>("total");
        QTest::addColumn<bool>("burst");
        QTest::newRow("1 of 1") << 1 << 1 << false;
        QTest::newRow("2 of 2") << 2 << 2 << false;
        QTest::newRow("3 of 3") << 3 << 3 << true;
        QTest::newRow("3 of 7") << 3 << 7 << false;
        QTest::newRow("4 of 7") << 4 << 7 << true;
        QTest::newRow("3 of 6") << 3 << 6 << true;
    }

    void burstRuleEdges()
    {
        QFETCH(int, rejected);
        QFETCH(int, total);
        QFETCH(bool, burst);
        Rig r;
        QVERIFY(r.bringUp());
        r.fetch();
        QJsonObject bad = sampleRecord();
        bad.insert("measured_at", "garbage");
        QJsonArray records;
        for (int i = 0; i < total; ++i)
            records << (i < rejected ? bad : sampleRecord(100 + i));
        r.sayRecords("c3", "current", records);
        r.sayCompletion("c3", total);
        QVERIFY(r.fetchDone);
        QCOMPARE(r.report.outcome.recordsValid, qint64(total - rejected));
        QCOMPARE(r.report.outcome.quarantinedByReason.value(Validation::MalformedMeasurementTime),
                 qint64(rejected));
        if (burst) {
            QCOMPARE(r.report.outcome.result, CommandOutcome::Error);
            QCOMPARE(r.report.outcome.errorClass, ErrorClass::ProtocolChanged);
        } else {
            QCOMPARE(r.report.outcome.result, CommandOutcome::Ok);
        }
    }

    void clockSkewIsAParameter()
    {
        Rig r;
        r.link.setClockSkewToleranceMs(2 * 3600 * 1000);
        QVERIFY(r.bringUp());
        r.fetch();
        r.sayRecords("c3", "current", QJsonArray() << sampleRecord(112, "2026-01-01T13:00:00Z"));
        r.sayCompletion("c3", 1);
        QCOMPARE(r.report.outcome.result, CommandOutcome::Ok);
        QCOMPARE(r.sinks.valid.size(), 1);
    }

    // ---- Policy actions ----------------------------------------------------------

    void policyKillOnALine()
    {
        Rig r;
        QVERIFY(r.bringUp());
        r.fetch();
        r.policy.lineActions << Action::kill();
        r.sayRecords("c3", "current", QJsonArray() << sampleRecord());
        QVERIFY(r.fetchDone);
        QCOMPARE(r.report.outcome.errorClass, ErrorClass::ProtocolChanged);
        QCOMPARE(r.report.outcome.killedBy, CommandOutcome::Guard);
        QVERIFY(r.wasKilled);
        QVERIFY(r.sinks.valid.isEmpty());
    }

    void policyDropOnABatch()
    {
        Rig r;
        QVERIFY(r.bringUp());
        r.fetch();
        r.policy.batchActions << Action::drop();
        r.sayRecords("c3", "current", QJsonArray() << sampleRecord());
        QVERIFY(r.sinks.valid.isEmpty());
        QVERIFY(!r.fetchDone);
        r.sayCompletion("c3", 1);
        QVERIFY(r.fetchDone);
    }

    void policyKillOnABatch()
    {
        Rig r;
        QVERIFY(r.bringUp());
        r.fetch();
        r.policy.batchActions << Action::kill();
        r.sayRecords("c3", "current", QJsonArray() << sampleRecord());
        QVERIFY(r.fetchDone);
        QCOMPARE(r.report.outcome.killedBy, CommandOutcome::Guard);
        QVERIFY(r.sinks.valid.isEmpty());
    }

    // ---- onViolation answers with an Action -----------------------

    void policyKillsOnAHardViolation()
    {
        Rig r;
        QVERIFY(r.bringUp());
        r.fetch();
        r.policy.violationActions << Action::kill();
        r.channel->say("this is not a protocol line\n");
        QVERIFY(r.fetchDone);       // the adapter said nothing more: the kill is at once
        QCOMPARE(r.report.outcome.result, CommandOutcome::Error);
        QCOMPARE(r.report.outcome.errorClass, ErrorClass::ProtocolChanged);
        QCOMPARE(r.report.outcome.killedBy, CommandOutcome::Guard);
        QCOMPARE(killedByName(r.report.outcome.killedBy), QString("guard"));
        QCOMPARE(r.report.outcome.hardViolations, 1);
        QVERIFY(r.wasKilled);
    }

    void policyKillsOnASoftViolationToo()
    {
        Rig r;
        QVERIFY(r.bringUp());
        r.fetch();
        r.sayRecords("c3", "current", QJsonArray() << sampleRecord());
        r.policy.violationActions << Action::kill();
        r.sayCompletion("c3", 5);       // records_sent mismatch: soft
        QVERIFY(r.fetchDone);
        QCOMPARE(r.policy.violations.last().severity, Severity::Soft);
        QCOMPARE(r.report.outcome.result, CommandOutcome::Error);
        QCOMPARE(r.report.outcome.errorClass, ErrorClass::ProtocolChanged);
        QCOMPARE(r.report.outcome.killedBy, CommandOutcome::Guard);
        QCOMPARE(r.report.outcome.softViolations, 1);
        QVERIFY(r.wasKilled);
        QCOMPARE(r.policy.outcomes.size(), 1);      // exactly one outcome
    }

    // A kill at a bad record in the middle of a batch ends the command; the records of that
    // batch are not delivered and the rest of the batch is not looked at.
    void policyKillInTheMiddleOfABatch()
    {
        Rig r;
        QVERIFY(r.bringUp());
        r.fetch();
        r.policy.violationActions << Action::kill();
        QJsonObject badMetric = sampleRecord();
        badMetric.insert("metric", "pulse");
        r.sayRecords("c3", "current", QJsonArray() << sampleRecord() << badMetric << badMetric);
        QVERIFY(r.fetchDone);
        QCOMPARE(r.report.outcome.killedBy, CommandOutcome::Guard);
        QCOMPARE(r.policy.violations.size(), 1);
        QVERIFY(r.sinks.valid.isEmpty());
        QCOMPARE(r.sinks.reasons, QStringList() << "UndeclaredMetric");
    }

    void policyPausesOnAViolation()
    {
        Rig r;
        QVERIFY(r.bringUp());
        r.policy.violationActions << Action::pause(150);
        QJsonObject log;
        log.insert("level", "info");
        log.insert("text", "after");
        const QByteArray good = encodeJsonLine(buildMessage(MessageType::Log, "a40", "", log));
        r.channel->say(QByteArray("not a protocol line\n") + good);
        QCOMPARE(r.policy.violations.size(), 1);
        QCOMPARE(r.diagnostics.size(), 0);      // reading stopped at the violation
        QTRY_COMPARE_WITH_TIMEOUT(r.diagnostics.size(), 1, 2000);
        QVERIFY(!r.wasKilled);
    }

    void policyDropOrProceedOnAViolationChangesNothing()
    {
        Rig r;
        QVERIFY(r.bringUp());
        r.fetch();
        r.policy.violationActions << Action::drop() << Action::proceed();
        r.channel->say("junk one\n");
        r.channel->say("junk two\n");
        QCOMPARE(r.policy.violations.size(), 2);
        QVERIFY(!r.fetchDone);
        QVERIFY(!r.wasKilled);
        r.sayCompletion("c3", 0);
        QCOMPARE(r.report.outcome.result, CommandOutcome::Ok);
    }

    // The kill is applied at once, not at the next message: the adapter never speaks again.
    void hardViolationThenSilenceIsKilledAtOnce()
    {
        Rig r;
        QVERIFY(r.bringUp());
        r.fetch();
        r.policy.violationActions << Action::kill();
        r.sayCompletion("c9", 0);       // matches no open command: hard violation
        QVERIFY(r.fetchDone);
        QCOMPARE(r.policy.violations.first().kind, ViolationKind::UnmatchedCorrelation);
        QCOMPARE(r.report.outcome.killedBy, CommandOutcome::Guard);
    }

    void aKillBetweenCommandsClosesTheLink()
    {
        Rig r;
        QVERIFY(r.bringUp());
        r.policy.violationActions << Action::kill();
        r.channel->say("junk\n");
        QVERIFY(r.wasKilled);
        QVERIFY(!r.link.isAlive());
        bool done = false;
        CommandReport rep;
        r.link.discoverSubjects([&](const CommandReport &x, const QList<SubjectInfo> &) { done = true; rep = x; });
        QTRY_VERIFY(done);
        QCOMPARE(rep.outcome.errorClass, ErrorClass::InvalidCommand);
    }

    // ---- onAdmitted: the second policy check -----------------------

    void onAdmittedSeesTheAdapterVersionAndAllows()
    {
        Rig r;
        QVERIFY(r.bringUp());
        QCOMPARE(r.policy.starts.size(), 1);
        QVERIFY(r.policy.starts.at(0).adapterVersion.isEmpty());        // unknown at mayStart
        QCOMPARE(r.policy.admitted.size(), 1);
        QCOMPARE(r.policy.admitted.at(0).adapterVersion, QString("0.1.0"));
        QCOMPARE(r.policy.admitted.at(0).adapterId, QString("libre"));
        QCOMPARE(r.startResult.kind, StartResult::Admitted);
    }

    void onAdmittedDenialRefusesTheStartAndNothingRunsAfter()
    {
        Rig r;
        r.policy.admittedResult = MayStartResult::denied(MayStartResult::AdapterVersionDisabled);
        r.begin();
        r.channel->sayJson(r.hello());
        QVERIFY(r.startDone);
        QCOMPARE(r.startResult.kind, StartResult::Refused);
        QCOMPARE(r.startResult.refusal, StartResult::Denied);
        QCOMPARE(r.startResult.denyReason, QString("adapter-version-disabled"));
        QCOMPARE(r.policy.admitted.at(0).adapterVersion, QString("0.1.0"));
        // End-of-session message, no accept, no configure.
        QCOMPARE(r.channel->lastType(), QString("shutdown"));
        const QList<QJsonObject> sent = r.channel->sent();
        for (int i = 0; i < sent.size(); ++i) {
            QVERIFY(sent.at(i).value("type").toString() != "accept");
            QVERIFY(sent.at(i).value("type").toString() != "configure");
        }
        // The adapter does not leave: it is killed after the wait.
        QTRY_VERIFY_WITH_TIMEOUT(r.wasKilled, 2000);

        // No command can follow.
        bool done = false;
        CommandReport rep;
        r.link.fetchCurrent("S-0001", [&](const CommandReport &x, const FetchCompletion &) { done = true; rep = x; });
        QTRY_VERIFY(done);
        QCOMPARE(rep.outcome.result, CommandOutcome::Error);
        QCOMPARE(rep.outcome.errorClass, ErrorClass::InvalidCommand);
        QCOMPARE(r.policy.outcomes.last().commandType, QString("fetch_current"));
        // Read from the copy taken at the kill: the link deletes the channel once it is
        // killed, so reading r.channel here was a use after free that crashed now and then.
        for (int i = 0; i < r.sentAtKill.size(); ++i)
            QVERIFY(r.sentAtKill.at(i).value("type").toString() != "fetch_current");
    }

    void onAdmittedDenialByGuardNamesItsReason()
    {
        Rig r;
        r.policy.admittedResult = MayStartResult::denied(MayStartResult::DisabledByGuard);
        r.begin();
        r.channel->sayJson(r.hello());
        QVERIFY(r.startDone);
        QCOMPARE(r.startResult.denyReason, QString("disabled-by-guard"));
    }

    void nullPolicyAllowsAtAdmission()
    {
        NullPolicy p;
        LinkId l;
        l.adapterVersion = "9.9.9";
        QVERIFY(p.onAdmitted(l).kind == MayStartResult::Allowed);
    }

    // ---- Reason names -------------------------------------------------------------

    // Every enumerator has a name in the driver and in the store, and the two agree
    // (REQ-DAT-08; the archive carries the names as text). Add new reasons after the last
    // one and extend the loop bound.
    void everyReasonNameAgreesBetweenDriverAndStore()
    {
        QSet<QString> names;
        for (int i = int(Validation::MissingSubject); i <= int(Validation::MalformedMeasurementTime); ++i) {
            const Validation::Reason reason = Validation::Reason(i);
            const QString driver = quarantineReasonName(reason);
            const QString store = validationReasonName(reason);
            QVERIFY2(!driver.isEmpty(), qPrintable(QString::number(i)));
            QCOMPARE(driver, store);
            Validation::Reason back = Validation::Ok;
            QVERIFY(validationReasonFromName(store, &back));
            QCOMPARE(int(back), i);
            names.insert(driver);
        }
        QCOMPARE(names.size(), 20);
        QCOMPARE(quarantineReasonName(Validation::Ok), QString("Ok"));
    }

    void policyPauseStopsReadingForAWhile()
    {
        Rig r;
        QVERIFY(r.bringUp());
        r.policy.lineActions << Action::pause(150);
        QJsonObject log;
        log.insert("level", "info");
        log.insert("text", "one");
        const QByteArray l1 = encodeJsonLine(buildMessage(MessageType::Log, "a40", "", log));
        log.insert("text", "two");
        const QByteArray l2 = encodeJsonLine(buildMessage(MessageType::Log, "a41", "", log));
        r.channel->say(l1 + l2);
        QCOMPARE(r.diagnostics.size(), 1);      // the second line waits
        QTRY_COMPARE_WITH_TIMEOUT(r.diagnostics.size(), 2, 2000);
    }

    void policyLineSizesAreReported()
    {
        Rig r;
        QVERIFY(r.bringUp());
        const int before = r.policy.lineBytes.size();
        QJsonObject log;
        log.insert("level", "info");
        log.insert("text", "x");
        const QByteArray line = encodeJsonLine(buildMessage(MessageType::Log, "a40", "", log));
        r.channel->say(line);
        QCOMPARE(r.policy.lineBytes.size(), before + 1);
        QCOMPARE(r.policy.lineBytes.last(), qint64(line.size()));
    }

    // ---- Standard error and log ---------------------------------------------------

    void stderrIsDiagnosticsOnly()
    {
        Rig r;
        QVERIFY(r.bringUp());
        r.fetch();
        // A line that looks like a completion on standard error is never parsed.
        r.channel->sayStderr("{\"version\":0,\"id\":\"a50\",\"type\":\"reply\",\"correlation_id\":\"c3\"}\n");
        QVERIFY(!r.fetchDone);
        QVERIFY(r.policy.violations.isEmpty());
        QCOMPARE(r.diagnostics.size(), 1);
        QVERIFY(r.diagnostics.at(0).startsWith("stderr"));
        QCOMPARE(r.policy.stderrBytes.size(), 1);
    }

    void stderrFloodIsReportedOnceAsSoft()
    {
        Rig r;
        r.policy.lim.maxStderrBytes = 100;
        QVERIFY(r.bringUp());
        for (int i = 0; i < 5; ++i)
            r.channel->sayStderr(QByteArray(60, 'e') + "\n");
        int floods = 0;
        for (int i = 0; i < r.policy.violations.size(); ++i)
            floods += r.policy.violations.at(i).kind == ViolationKind::StderrFlood;
        QCOMPARE(floods, 1);
        QCOMPARE(r.policy.violations.last().severity, Severity::Soft);
        QCOMPARE(r.policy.stderrBytes.size(), 5);       // the policy still sees every chunk
        QVERIFY(r.diagnostics.size() < 5);
    }

    void policyCanKillOnStderr()
    {
        Rig r;
        QVERIFY(r.bringUp());
        r.fetch();
        r.policy.stderrActions << Action::kill();
        r.channel->sayStderr("noise\n");
        QVERIFY(r.fetchDone);
        QCOMPARE(r.report.outcome.killedBy, CommandOutcome::Guard);
    }

    void diagnosticsAreCapped()
    {
        Rig r;
        QVERIFY(r.bringUp());
        for (int i = 0; i < kMaxDiagnosticLines + 20; ++i)
            r.channel->sayStderr("line\n");
        QCOMPARE(r.link.diagnostics().size(), kMaxDiagnosticLines);
    }

    // ---- Secrets (REQ-ADP-11, REQ-ADP-12, REQ-SEC-06) ------------------------------

    void noSecretInAnyOutput()
    {
        g_qtMessages.clear();
        QtMessageHandler previous = qInstallMessageHandler(captureMessages);
        {
            Rig r;
            QVERIFY(r.bringUp());

            Credentials c;
            c.username = kUser;
            c.password = kPassword;
            CommandReport authReport;
            bool authDone = false;
            r.link.authenticate(c, kSession, [&](const CommandReport &rep, const SessionGrant &) {
                authDone = true;
                authReport = rep;
            });
            // The adapter misbehaves and repeats secrets on every channel it has.
            const QString leak = QString("login %1 / %2 / %3").arg(kUser, kPassword, kSession);
            QJsonObject log;
            log.insert("level", "error");
            log.insert("text", leak);
            r.say(MessageType::Log, QString(), log);
            r.channel->sayStderr(leak.toUtf8() + "\n");
            QJsonObject err;
            err.insert("class", "auth-failed");
            err.insert("detail", leak);
            r.say(MessageType::Error, "c3", err);
            QVERIFY(authDone);
            QVERIFY(!authReport.detail.contains(kPassword));
            QVERIFY(!authReport.detail.contains(kUser));
            QVERIFY(!authReport.detail.contains(kSession));
            QVERIFY(authReport.detail.contains("[redacted]"));

            // A name received from discover is scrubbed as well.
            bool discovered = false;
            r.link.discoverSubjects([&](const CommandReport &, const QList<SubjectInfo> &) { discovered = true; });
            QJsonObject s;
            s.insert("subject_ref", "S-0001");
            s.insert("display_name", kName);
            s.insert("unit", "mg/dL");
            s.insert("unit_basis", "code");
            QJsonObject p;
            p.insert("subjects", QJsonArray() << s);
            r.say(MessageType::Reply, r.channel->lastId(), p);
            QVERIFY(discovered);
            log.insert("text", QString("subject %1 has no sensor").arg(kName));
            r.say(MessageType::Log, QString(), log);
            r.channel->sayStderr(QByteArray("name ") + kName + "\n");

            // Violations and outcomes carry no secret either.
            QStringList everything = r.diagnostics;
            everything += r.link.diagnostics();
            for (int i = 0; i < r.policy.violations.size(); ++i)
                everything << r.policy.violations.at(i).detail;
            for (int i = 0; i < r.policy.outcomes.size(); ++i)
                everything << r.policy.outcomes.at(i).commandId << r.policy.outcomes.at(i).commandType;
            everything += g_qtMessages;
            const QString all = everything.join("\n");
            QVERIFY(!all.contains(kUser));
            QVERIFY(!all.contains(kPassword));
            QVERIFY(!all.contains(kSession));
            QVERIFY(!all.contains(kName));
            QVERIFY(all.contains("[redacted]"));
        }
        qInstallMessageHandler(previous);
    }

    void jsonEscapedSecretsAreScrubbed()
    {
        Rig r;
        QVERIFY(r.bringUp());
        Credentials c;
        c.username = kUser;
        c.password = "pa\"ss\\word";
        r.link.authenticate(c, QString(), [](const CommandReport &, const SessionGrant &) {});
        r.channel->sayStderr("echo {\"password\":\"pa\\\"ss\\\\word\"}\n");
        QVERIFY(!r.diagnostics.last().contains("pa\\\"ss"));
        QVERIFY(r.diagnostics.last().contains("[redacted]"));
    }

    // ---- Stopping -----------------------------------------------------------------

    void stopKillsAnAdapterThatDoesNotEnd()
    {
        Rig r;
        QVERIFY(r.bringUp());
        bool stopped = false;
        r.link.stop([&]() { stopped = true; });
        QCOMPARE(r.channel->lastType(), QString("shutdown"));
        QVERIFY(!stopped);
        QTRY_VERIFY_WITH_TIMEOUT(stopped, 2000);
        QVERIFY(r.wasKilled);
    }

    void stopDuringACommand()
    {
        Rig r;
        QVERIFY(r.bringUp());
        r.fetch();
        bool stopped = false;
        r.link.stop([&]() { stopped = true; });
        QVERIFY(r.fetchDone);
        QCOMPARE(r.report.outcome.result, CommandOutcome::Error);
        r.channel->exitWith(0);
        QVERIFY(stopped);
    }

    void stopWhenNothingRuns()
    {
        Rig r;
        bool stopped = false;
        r.link.stop([&]() { stopped = true; });
        QTRY_VERIFY(stopped);
    }

    void restartAfterAnEnd()
    {
        Rig r;
        QVERIFY(r.bringUp());
        r.channel->exitWith(1);
        QVERIFY(!r.link.isAlive());
        FakeChannel *first = r.channel;
        QVERIFY(r.bringUp());
        QVERIFY(r.channel != first);
        QVERIFY(r.link.isIdle());
        QVERIFY(!r.link.linkId().sessionId.isEmpty());
    }

    void sessionIdIsNewForEveryStart()
    {
        Rig r;
        QVERIFY(r.bringUp());
        const QString first = r.link.linkId().sessionId;
        r.channel->exitWith(1);
        QVERIFY(r.bringUp());
        QVERIFY(r.link.linkId().sessionId != first);
    }

    void idleHookForAProcessPolicy()
    {
        Rig r;
        QVERIFY(r.bringUp());
        QVERIFY(r.link.isIdle());
        QTest::qWait(30);
        QVERIFY(r.link.idleMs() >= 20);
        r.fetch();
        QVERIFY(!r.link.isIdle());
    }

    // ---- The real process channel ---------------------------------------------------

    void processChannelAppliesOsLimits()
    {
        ProcessChannel channel("/bin/sh", QStringList() << "-c" << "ulimit -n; ulimit -t; ulimit -v");
        OsLimits os;
        os.fileDescriptors = 77;
        os.cpuSeconds = 33;
        os.memoryBytes = 2048LL * 1024 * 1024;
        channel.setOsLimits(os);
        bool done = false;
        connect(&channel, &LinkChannel::finished, [&](int, bool) { done = true; });
        channel.open();
        QTRY_VERIFY_WITH_TIMEOUT(done, 5000);
        const QList<QByteArray> lines = channel.fromAdapter()->readAll().split('\n');
        QCOMPARE(lines.at(0), QByteArray("77"));
        QCOMPARE(lines.at(1), QByteArray("33"));
        QCOMPARE(lines.at(2), QByteArray("2097152"));      // kibibytes
    }

    void processChannelWithoutLimitsLeavesThemAlone()
    {
        ProcessChannel channel("/bin/sh", QStringList() << "-c" << "ulimit -t");
        bool done = false;
        connect(&channel, &LinkChannel::finished, [&](int, bool) { done = true; });
        channel.open();
        QTRY_VERIFY_WITH_TIMEOUT(done, 5000);
        QCOMPARE(channel.fromAdapter()->readAll().trimmed(), QByteArray("unlimited"));
    }

    void processChannelReportsAMissingExecutable()
    {
        ProcessChannel channel("/nonexistent/adapter", QStringList());
        QString reason;
        connect(&channel, &LinkChannel::failedToStart, [&](const QString &r) { reason = r; });
        channel.open();
        QTRY_VERIFY_WITH_TIMEOUT(!reason.isEmpty(), 5000);
        QVERIFY(!reason.contains("/nonexistent"));      // no path or argument in the text
    }

    void processChannelCarriesStderrAndExitCode()
    {
        ProcessChannel channel("/bin/sh", QStringList() << "-c" << "echo diag >&2; exit 5");
        QByteArray err;
        int code = -1;
        connect(&channel, &LinkChannel::stderrData, [&](const QByteArray &d) { err += d; });
        connect(&channel, &LinkChannel::finished, [&](int c, bool) { code = c; });
        channel.open();
        QTRY_COMPARE_WITH_TIMEOUT(code, 5, 5000);
        QCOMPARE(err.trimmed(), QByteArray("diag"));
    }

    void linkRunsARealProcessAndHelloTimesOut()
    {
        // The real channel inside the driver: a process that never says hello.
        RecordingPolicy policy;
        policy.lim.helloTimeoutMs = 300;
        policy.lim.fileDescriptorLimit = 64;
        AdapterLink link(&policy);
        link.setProgram("/bin/sh", QStringList() << "-c" << "sleep 5");
        bool done = false;
        StartResult result;
        link.start("libre", "conn-1", QJsonObject(), [&](const StartResult &r, const QString &) {
            done = true;
            result = r;
        });
        QTRY_VERIFY_WITH_TIMEOUT(done, 5000);
        QCOMPARE(result.refusal, StartResult::HelloTimeout);
        QCOMPARE(policy.spawned.size(), 1);
        QTRY_COMPARE_WITH_TIMEOUT(policy.exited.size(), 1, 5000);   // killed, not left running
    }

    // ---- Names ---------------------------------------------------------------------

    void namesRoundTrip()
    {
        const ErrorClass all[] = { ErrorClass::AuthFailed, ErrorClass::TermsMustBeAccepted,
                                   ErrorClass::RateLimited, ErrorClass::Transient,
                                   ErrorClass::ProtocolChanged, ErrorClass::InvalidCommand };
        for (size_t i = 0; i < 6; ++i) {
            ErrorClass back = ErrorClass::AuthFailed;
            QVERIFY(errorClassFromName(errorClassName(all[i]), &back));
            QVERIFY(back == all[i]);
        }
        ErrorClass dummy;
        QVERIFY(!errorClassFromName("nope", &dummy));
        QCOMPARE(violationKindName(ViolationKind::LineTooLong), QString("line-too-long"));
        QCOMPARE(severityOf(ViolationKind::RecordsSentMismatch), Severity::Soft);
        QCOMPARE(severityOf(ViolationKind::NonProtocolOutput), Severity::Hard);
    }

    void nullPolicyRecordsViolations()
    {
        NullPolicy p;
        QVERIFY(!p.sawHardViolation());
        Violation soft;
        soft.severity = Severity::Soft;
        QVERIFY(p.onViolation(soft).kind == Action::Proceed);
        QVERIFY(!p.sawHardViolation());
        Violation hard;
        hard.severity = Severity::Hard;
        QVERIFY(p.onViolation(hard).kind == Action::Kill);      // a hard one ends the adapter
        QVERIFY(p.sawHardViolation());
        QCOMPARE(p.violations().size(), 2);
        QVERIFY(p.mayStart(StartRequest()).kind == MayStartResult::Allowed);
        QVERIFY(p.onLine(LinkId(), 10, UtcTime()).kind == Action::Proceed);
        QCOMPARE(p.limits().maxLineBytes, 1048576);
        QCOMPARE(p.limits().fetchRangeDeadlineMs, 60000);
        QCOMPARE(p.limits().helloTimeoutMs, 10000);
    }
};

QTEST_MAIN(TstAdapterLink)
#include "tst_adapterlink.moc"
