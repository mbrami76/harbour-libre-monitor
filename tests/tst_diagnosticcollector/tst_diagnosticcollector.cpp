// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Michele Brami
//
// What the service answers to DiagnosticReport(connection).
// The collector stands between the adapter driver and the guard; these tests drive it the way the
// driver does (a violation, the batch a guard kill interrupted, the end of the command) with a fake
// guard behind it. What is proved: the guard's answers pass through unchanged and every call reaches
// it; the last guard kill of a connection is kept with its command, counts and violations; a clean
// success discards it, an adapter error does not; a kill outside a batch gives the context and a
// batch line "none"; and the text holds NONE of the synthetic fixture's personal or secret fields
// and NO measurement value, in plain or JSON-escaped form ( no glucose value, name, reference,
// address or credential). Every value below is invented.
#include "service/diagnosticcollector.h"

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QtTest>

using namespace lm;

namespace {

// Each fixture string has a character JSON escapes, so the escaped form is searched too.
const char *const kSecrets[] = {
    "Zo\xC3\xAB \"Zed\" Synthetic-Patient",          // a name
    "caregiver.synthetic@example.invalid",           // an account address
    "Via Sintetica 7/B, 00000 Nowhere",               // a postal address
    "SUBJ-REF-9931-synthetic/a",                     // a source reference
    "SENSOR-SYN-778899\\q",                          // a sensor id
    "tok/SYNTHETIC.eyJhbGciOiJub25lIn0.77aa",        // a token
    "pw-synthetic-\"secret\"",                       // a password
    "2026-03-04T05:06:07Z",                          // an absolute time
    "3/4/2026 6:06:07 AM",                           // the source's own local time text
};
// Measurement values: decimals that nothing else in a report can produce.
const double kValues[] = { 187.375, 43.625, 61.125, 7.875 };

QString escaped(const QString &s)
{
    QJsonArray a;
    a.append(s);
    const QString t = QString::fromUtf8(QJsonDocument(a).toJson(QJsonDocument::Compact));
    return t.mid(2, t.size() - 4);
}

QByteArray leakyBatch()
{
    QJsonArray records;
    for (int i = 0; i < 3; ++i) {
        QJsonObject r;
        r.insert("shape", "sample");
        r.insert("subject_ref", QString::fromUtf8(kSecrets[3]));
        r.insert("metric", "glucose");
        r.insert("value", kValues[i]);
        r.insert("unit", "mg/dL");
        r.insert("measured_at", QString::fromUtf8(kSecrets[7]));
        r.insert("original_timestamp", QString::fromUtf8(kSecrets[8]));
        r.insert("source", "libre.graph");
        r.insert("sensor", QString::fromUtf8(kSecrets[4]));
        r.insert("name", QString::fromUtf8(kSecrets[0]));
        r.insert("email", QString::fromUtf8(kSecrets[1]));
        r.insert("address", QString::fromUtf8(kSecrets[2]));
        records.append(r);
    }
    QJsonObject ev;
    ev.insert("shape", "event");
    ev.insert("metric", "logbook_entry");
    ev.insert("measured_at", QString::fromUtf8(kSecrets[7]));
    QJsonObject payload;
    payload.insert("entry_type", 1);
    payload.insert("value", kValues[3]);
    payload.insert("unit", "mmol/L");
    payload.insert("note", QString::fromUtf8(kSecrets[0]));
    ev.insert("payload", payload);
    records.append(ev);
    QJsonObject m;
    m.insert("type", "records");
    m.insert("version", 1);
    m.insert("id", "msg-synthetic-1");
    m.insert("correlation_id", "corr-synthetic-1");
    m.insert("feed", "graph");
    m.insert("token", QString::fromUtf8(kSecrets[5]));
    m.insert("password", QString::fromUtf8(kSecrets[6]));
    m.insert("records", records);
    return QJsonDocument(m).toJson(QJsonDocument::Compact);
}

// The guard behind the collector: records each call and answers what the test says.
class FakeGuard : public LinkPolicy
{
public:
    FakeGuard() : violationAnswer(Action::kill()) {}
    LinkLimits limits() const { LinkLimits l; l.maxLineBytes = 4242; return l; }
    MayStartResult mayStart(const StartRequest &) { calls << "mayStart"; return MayStartResult::denied(MayStartResult::SpawnRate); }
    void onSpawned(const LinkId &) { calls << "spawned"; }
    MayStartResult onAdmitted(const LinkId &) { calls << "admitted"; return MayStartResult::allowed(); }
    void onExited(const LinkId &, const ExitStatus &) { calls << "exited"; }
    Action onLine(const LinkId &, qint64, const UtcTime &) { calls << "line"; return Action::pause(77); }
    Action onBatch(const LinkId &, const QString &, int, qint64) { calls << "batch"; return Action::drop(); }
    Action onStderr(const LinkId &, qint64) { calls << "stderr"; return Action::proceed(); }
    Action onViolation(const Violation &) { calls << "violation"; return violationAnswer; }
    void onCommandFinished(const CommandOutcome &) { calls << "finished"; }

    Action violationAnswer;
    QStringList calls;
};

LinkId link(const char *connection)
{
    LinkId l;
    l.connectionId = QString::fromLatin1(connection);
    l.adapterId = QStringLiteral("libre");
    l.adapterVersion = QStringLiteral("0.3.0");
    l.sessionId = QStringLiteral("session-synthetic");
    return l;
}

Violation violation(const char *connection)
{
    Violation v;
    v.link = link(connection);
    v.kind = ViolationKind::UndeclaredMetric;
    v.severity = Severity::Hard;
    v.detail = QStringLiteral("record metric not declared in hello");
    return v;
}

CommandOutcome outcome(const char *connection, CommandOutcome::KilledBy by, CommandOutcome::Result result)
{
    CommandOutcome o;
    o.link = link(connection);
    o.commandType = QStringLiteral("fetch_range");
    o.killedBy = by;
    o.result = result;
    o.batches = 2;
    o.recordsReceived = 9;
    o.recordsValid = 5;
    o.hardViolations = 1;
    return o;
}

// The guard kill of a batch, in the driver's order: the violation, the held line, the end.
void killMidBatch(DiagnosticCollector &c, const char *connection)
{
    c.onViolation(violation(connection));
    c.rejected(link(connection), leakyBatch(), CommandOutcome::Guard, true, ViolationKind::UndeclaredMetric);
    c.onCommandFinished(outcome(connection, CommandOutcome::Guard, CommandOutcome::Error));
}

QList<QJsonObject> lines(const QByteArray &text)
{
    QList<QJsonObject> out;
    const QList<QByteArray> parts = text.split('\n');
    for (int i = 0; i < parts.size(); ++i)
        if (!parts.at(i).isEmpty())
            out.append(QJsonDocument::fromJson(parts.at(i)).object());
    return out;
}

} // namespace

class TstDiagnosticCollector : public QObject
{
    Q_OBJECT

private slots:
    // A decorator must not change the guard: every answer comes back as the guard gave it and every
    // call reaches it, once.
    void theGuardsAnswersPassThroughUnchanged()
    {
        FakeGuard g;
        DiagnosticCollector c(&g);
        QCOMPARE(c.limits().maxLineBytes, 4242);
        StartRequest req;
        QCOMPARE(int(c.mayStart(req).reason), int(MayStartResult::SpawnRate));
        c.onSpawned(link("c1"));
        QCOMPARE(int(c.onAdmitted(link("c1")).kind), int(MayStartResult::Allowed));
        QCOMPARE(int(c.onLine(link("c1"), 10, UtcTime()).kind), int(Action::Pause));
        QCOMPARE(c.onLine(link("c1"), 10, UtcTime()).pauseMs, 77);
        QCOMPARE(int(c.onBatch(link("c1"), "graph", 3, 100).kind), int(Action::Drop));
        QCOMPARE(int(c.onStderr(link("c1"), 5).kind), int(Action::Proceed));
        QCOMPARE(int(c.onViolation(violation("c1")).kind), int(Action::Kill));
        g.violationAnswer = Action::proceed();
        QCOMPARE(int(c.onViolation(violation("c1")).kind), int(Action::Proceed));
        c.onCommandFinished(outcome("c1", CommandOutcome::NotKilled, CommandOutcome::Ok));
        c.onExited(link("c1"), ExitStatus());
        QCOMPARE(g.calls, QStringList() << "mayStart" << "spawned" << "admitted" << "line" << "line" << "batch"
                                        << "stderr" << "violation" << "violation" << "finished" << "exited");
    }

    // The last guard kill is kept with what the report needs; the report names the versions, the
    // command, the violation and the counts, and the batch only by its shape.
    void aGuardKillInABatchIsKeptAndReported()
    {
        FakeGuard g;
        DiagnosticCollector c(&g);
        QVERIFY(!c.hasReport("c1"));
        killMidBatch(c, "c1");
        QVERIFY(c.hasReport("c1"));
        QVERIFY(!c.hasReport("c2"));
        QVERIFY(c.report("c2", "0.1-2").isEmpty());
        const QList<QJsonObject> l = lines(c.report("c1", "0.1-2"));
        QCOMPARE(l.size(), 2 + 4);                                  // report, batch, four records
        QCOMPARE(l.at(0).value("line_type").toString(), QStringLiteral("report"));
        QCOMPARE(l.at(0).value("application_version").toString(), QStringLiteral("0.1-2"));
        QCOMPARE(l.at(0).value("adapter_id").toString(), QStringLiteral("libre"));
        QCOMPARE(l.at(0).value("adapter_version").toString(), QStringLiteral("0.3.0"));
        QCOMPARE(l.at(0).value("command").toString(), QStringLiteral("fetch_range"));
        QCOMPARE(l.at(0).value("violations").toArray().size(), 1);
        QCOMPARE(l.at(0).value("violations").toArray().first().toObject().value("kind").toString(),
                 QStringLiteral("undeclared-metric"));
        QCOMPARE(l.at(0).value("counts").toObject().value("records_received").toInt(), 9);
        QCOMPARE(l.at(1).value("line_type").toString(), QStringLiteral("batch"));
        QCOMPARE(l.at(1).value("status").toString(), QStringLiteral("ok"));
        QCOMPARE(l.at(1).value("feed").toString(), QStringLiteral("graph"));    // a protocol word is kept
        QCOMPARE(l.at(2).value("metric").toString(), QStringLiteral("glucose"));
        QCOMPARE(l.at(2).value("unit").toString(), QStringLiteral("mg/dL"));
    }

    // The central test: nothing of the fixture's personal or secret fields and no measurement value
    // is in the text, in plain or JSON-escaped form; every value field reads "number".
    void theReportHoldsNoValueNameReferenceAddressOrCredential()
    {
        FakeGuard g;
        DiagnosticCollector c(&g);
        killMidBatch(c, "c1");
        const QByteArray bytes = c.report("c1", "0.1-2");
        const QString text = QString::fromUtf8(bytes);
        QVERIFY(!text.isEmpty());
        for (size_t i = 0; i < sizeof(kSecrets) / sizeof(kSecrets[0]); ++i) {
            const QString s = QString::fromUtf8(kSecrets[i]);
            QVERIFY2(!text.contains(s), kSecrets[i]);
            QVERIFY2(!text.contains(escaped(s)), kSecrets[i]);
        }
        for (size_t i = 0; i < sizeof(kValues) / sizeof(kValues[0]); ++i) {
            QVERIFY2(!text.contains(QString::number(kValues[i], 'g', 10)), "a measurement value is in the report");
            QVERIFY2(!text.contains(QString::number(int(kValues[i])) + QLatin1Char('.')), "a measurement value is in the report");
        }
        const QList<QJsonObject> l = lines(bytes);
        int valueFields = 0;
        for (int i = 0; i < l.size(); ++i) {
            if (l.at(i).value("line_type").toString() != QLatin1String("record"))
                continue;
            const QJsonObject p = l.at(i).value("payload").toObject();
            if (l.at(i).contains("value")) { QCOMPARE(l.at(i).value("value").toString(), QStringLiteral("number")); ++valueFields; }
            if (p.contains("value")) { QCOMPARE(p.value("value").toString(), QStringLiteral("number")); ++valueFields; }
        }
        QCOMPARE(valueFields, 4);
        // The session id of the link is never part of a report either.
        QVERIFY(!text.contains(QStringLiteral("session-synthetic")));
    }

    // a successful command discards the held batch; an error the adapter reported
    // (no kill) leaves it; a new guard kill replaces it.
    void aSuccessDiscardsAnErrorKeepsANewKillReplaces()
    {
        FakeGuard g;
        DiagnosticCollector c(&g);
        killMidBatch(c, "c1");
        c.onCommandFinished(outcome("c1", CommandOutcome::NotKilled, CommandOutcome::Error));
        QVERIFY(c.hasReport("c1"));
        c.onCommandFinished(outcome("c1", CommandOutcome::Deadline, CommandOutcome::Error));
        QVERIFY(c.hasReport("c1"));
        // A second kill, this time not in a batch: the report says there was no batch.
        c.onViolation(violation("c1"));
        c.onCommandFinished(outcome("c1", CommandOutcome::Guard, CommandOutcome::Error));
        const QList<QJsonObject> l = lines(c.report("c1", "0.1-2"));
        QCOMPARE(l.size(), 2);
        QCOMPARE(l.at(1).value("status").toString(), QStringLiteral("none"));
        QCOMPARE(l.at(0).value("violations").toArray().size(), 1);  // only this command's violation
        c.onCommandFinished(outcome("c1", CommandOutcome::NotKilled, CommandOutcome::Ok));
        QVERIFY(!c.hasReport("c1"));
        killMidBatch(c, "c1");
        c.discard("c1");
        QVERIFY(!c.hasReport("c1"));
    }

    // Connections are kept apart.
    void connectionsAreKeptApart()
    {
        FakeGuard g;
        DiagnosticCollector c(&g);
        killMidBatch(c, "c1");
        c.onCommandFinished(outcome("c2", CommandOutcome::NotKilled, CommandOutcome::Ok));
        QVERIFY(c.hasReport("c1"));
        QVERIFY(!c.hasReport("c2"));
    }

    // The stripping alone: a report line is left as it is, a record line loses its values.
    void onlyRecordLinesAreStripped()
    {
        const QByteArray in = "{\"counts\":{\"batches\":3},\"line_type\":\"report\"}\n"
                              "{\"index\":0,\"line_type\":\"record\",\"payload\":{\"value\":5.5},\"value\":99.5}\n";
        const QList<QJsonObject> l = lines(DiagnosticCollector::withoutMeasurements(in));
        QCOMPARE(l.at(0).value("counts").toObject().value("batches").toInt(), 3);
        QCOMPARE(l.at(1).value("value").toString(), QStringLiteral("number"));
        QCOMPARE(l.at(1).value("payload").toObject().value("value").toString(), QStringLiteral("number"));
    }
};

QTEST_GUILESS_MAIN(TstDiagnosticCollector)
#include "tst_diagnosticcollector.moc"
