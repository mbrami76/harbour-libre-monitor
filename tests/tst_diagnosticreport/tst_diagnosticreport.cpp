// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Michele Brami
//
// Diagnostic report for the developer (REQ-GRD-15). Synthetic values only
// (REQ-DOC-03): every name, identifier, reference, token and time below is invented. The
// central test searches the whole output for each of them, in plain and in JSON-escaped
// form, because a leak through an escape sequence would still be a leak.
#include "session/diagnosticreport.h"

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QtTest>

using namespace lm;

namespace {

// Invented personal and secret strings. Each has a character that JSON escapes (a quote, a
// slash, a backslash or a non-ASCII letter) so that the escaped form differs from the plain
// one and both are searched.
const char *kPatientName = "Zo\xC3\xAB \"Zed\" M\xC3\xBCller-Synthetic";     // Zoë "Zed" Müller-Synthetic
const char *kAccount = "caregiver.synthetic@example.invalid";
const char *kSubjectRef = "SUBJ-REF-7731-synthetic/a";
const char *kSensor = "SENSOR-SYN-445566\\x";
const char *kToken = "tok/SYNTHETIC.eyJhbGciOiJub25lIn0.9f3a1c77";
const char *kSession = "SESSION-BLOB-synthetic-0042";
const char *kMessageId = "a-msgid-synthetic-99";
const char *kOtherName = "Gianni Verdi-Synthetic";

QString jsonEscaped(const QString &s)
{
    QJsonArray array;
    array.append(s);
    const QString text = QString::fromUtf8(QJsonDocument(array).toJson(QJsonDocument::Compact));
    return text.mid(2, text.size() - 4);        // ["..."] -> the content between the quotes
}

QJsonObject sample(double value, const QString &time, const QString &original)
{
    QJsonObject r;
    r.insert("shape", "sample");
    r.insert("subject_ref", kSubjectRef);
    r.insert("metric", "glucose");
    r.insert("value", value);
    r.insert("unit", "mg/dL");
    r.insert("measured_at", time);
    r.insert("original_timestamp", original);
    r.insert("source", "libre.current");
    return r;
}

DiagnosticContext context()
{
    DiagnosticContext c;
    c.applicationVersion = "0.1.0-test";
    c.adapterId = "libre";
    c.adapterVersion = "0.2.0";
    c.backendVersion = "4.16";
    c.commandType = "fetch_range";
    DiagnosticViolation v1, v2;
    v1.kind = "undeclared-metric";
    v1.detail = "record metric not declared in hello";
    v2.kind = "batch-too-large";
    v2.detail = "batch above max_records_per_batch";
    c.violations << v1 << v2;
    c.batches = 2;
    c.recordsReceived = 7;
    c.recordsValid = 4;
    c.quarantinedByReason.insert("UndeclaredMetric", 3);
    c.softViolations = 1;
    c.hardViolations = 1;
    c.declaredFeeds << "current" << "graph" << "logbook";
    c.declaredMetrics << "glucose" << "logbook_entry";
    c.declaredUnits << "mg/dL" << "mmol/L";
    return c;
}

// A batch packed with everything that must not survive.
QByteArray leakyBatch()
{
    QJsonObject first = sample(112, "2026-03-04T10:00:00Z", "3/4/2026 10:00:00 AM");
    first.insert("patient_name", kPatientName);                  // unknown field with a name
    first.insert("account", kAccount);
    first.insert("sensor_id", kSensor);
    first.insert("session", kSession);
    first.insert("auth_token", kToken);                          // unknown field with a token
    QJsonObject second = sample(131.5, "2026-03-04T10:05:00Z", "3/4/2026 10:05:00 AM");
    second.insert("duration_ms", 60000);
    QJsonObject third = sample(99, "2026-03-04T09:59:00Z", "3/4/2026 9:59:00 AM");
    third.insert("metric", "pulse-of-Gianni");                    // undeclared, text from the source
    QJsonObject rawEntry;
    rawEntry.insert("holder", kOtherName);
    rawEntry.insert("device", kSensor);
    rawEntry.insert("logged_at", "2026-03-04T08:00:00Z");
    rawEntry.insert("serial", 1772614800000.0);                  // an epoch time as a number
    QJsonObject payload;
    payload.insert("entry_type", 3);
    payload.insert("value", 120);
    payload.insert("unit", "mg/dL");
    payload.insert("raw", rawEntry);
    payload.insert("note", kPatientName);
    QJsonObject event;
    event.insert("shape", "event");
    event.insert("subject_ref", kSubjectRef);
    event.insert("metric", "logbook_entry");
    event.insert("payload", payload);
    event.insert("measured_at", "2026-03-04T10:10:00Z");
    event.insert("original_timestamp", "3/4/2026 10:10:00 AM");
    event.insert("source", "libre.logbook");
    QJsonObject key;                                             // an identifier as a key
    key.insert("0a1b2c3d-4e5f-6789-abcd-ef0123456789", "x");
    event.insert("extras", key);

    QJsonObject b;
    b.insert("version", 0);
    b.insert("type", "records");
    b.insert("id", kMessageId);
    b.insert("correlation_id", "c3");
    b.insert("feed", "current");
    b.insert("cursor", kToken);                                  // unknown batch-level field
    b.insert("records", QJsonArray() << first << second << third << event);
    return QJsonDocument(b).toJson(QJsonDocument::Compact);
}

QList<QJsonObject> parseLines(const QByteArray &report, bool *ok = 0)
{
    QList<QJsonObject> out;
    bool good = report.endsWith('\n');
    const QList<QByteArray> lines = report.split('\n');
    for (int i = 0; i < lines.size() - 1; ++i) {
        QJsonParseError e;
        const QJsonDocument d = QJsonDocument::fromJson(lines.at(i), &e);
        if (e.error != QJsonParseError::NoError || !d.isObject())
            good = false;
        out.append(d.object());
    }
    if (ok)
        *ok = good;
    return out;
}

} // namespace

class TstDiagnosticReport : public QObject
{
    Q_OBJECT

private slots:
    // The whole output, searched for every planted string in plain and escaped form.
    void nothingPersonalOrSecretSurvives()
    {
        const QByteArray out = buildDiagnosticReport(leakyBatch(), context());
        const QString text = QString::fromUtf8(out);
        const QStringList planted = QStringList()
            << QString::fromUtf8(kPatientName) << kAccount << kSubjectRef << kSensor << kToken
            << kSession << kMessageId << kOtherName
            << "pulse-of-Gianni" << "Gianni" << "Verdi" << "Zed" << "ller"
            << "caregiver" << "eyJhbG" << "9f3a1c77" << "0a1b2c3d" << "4e5f"
            << "SUBJ-REF" << "7731" << "445566" << "0042";
        for (int i = 0; i < planted.size(); ++i) {
            QVERIFY2(!text.contains(planted.at(i)), qPrintable("plain: " + planted.at(i)));
            QVERIFY2(!text.contains(jsonEscaped(planted.at(i))),
                     qPrintable("escaped: " + planted.at(i)));
            QVERIFY2(!out.contains(planted.at(i).toUtf8()), qPrintable("bytes: " + planted.at(i)));
        }
        // Absolute times, in the ISO form, in the source's own form, as epoch milliseconds
        // and as fragments of either.
        const QStringList times = QStringList()
            << "2026-03-04" << "2026" << "T10:" << "10:00:00" << "10:05:00" << "09:59"
            << "3/4/2026" << "1772614800000" << "1772614800" << "10:10";
        for (int i = 0; i < times.size(); ++i) {
            QVERIFY2(!text.contains(times.at(i)), qPrintable("time: " + times.at(i)));
            QVERIFY2(!text.contains(jsonEscaped(times.at(i))), qPrintable("time escaped: " + times.at(i)));
        }
        bool ok = false;
        parseLines(out, &ok);
        QVERIFY(ok);
    }

    void glucoseValuesAndUnitsAreKept()
    {
        const QList<QJsonObject> lines = parseLines(buildDiagnosticReport(leakyBatch(), context()));
        QCOMPARE(lines.size(), 6);          // report, batch, four records
        QCOMPARE(lines.at(2).value("value").toDouble(), 112.0);
        QCOMPARE(lines.at(2).value("unit").toString(), QString("mg/dL"));
        QCOMPARE(lines.at(3).value("value").toDouble(), 131.5);
        QCOMPARE(lines.at(3).value("duration_ms").toDouble(), 60000.0);
        QCOMPARE(lines.at(4).value("value").toDouble(), 99.0);
        QCOMPARE(lines.at(2).value("shape").toString(), QString("sample"));
        QCOMPARE(lines.at(2).value("metric").toString(), QString("glucose"));
        QCOMPARE(lines.at(2).value("source").toString(), QString("libre.current"));
        // The event: its measurement fields survive, the unknown ones are reduced to types.
        const QJsonObject payload = lines.at(5).value("payload").toObject();
        QCOMPARE(payload.value("entry_type").toDouble(), 3.0);
        QCOMPARE(payload.value("value").toDouble(), 120.0);
        QCOMPARE(payload.value("unit").toString(), QString("mg/dL"));
        QCOMPARE(payload.value("note").toString(), QString("string"));
        QCOMPARE(payload.value("raw").toObject().value("holder").toString(), QString("string"));
        QCOMPARE(payload.value("raw").toObject().value("serial").toString(), QString("number"));
        // The text the source sent where a metric belongs is not copied, only its shape.
        QCOMPARE(lines.at(4).value("metric").toString(), QString("aaaaa-aa-Aaaaaa"));
    }

    void offsetsAreCountedFromTheFirstTimeOfTheBatch()
    {
        const QList<QJsonObject> lines = parseLines(buildDiagnosticReport(leakyBatch(), context()));
        QCOMPARE(lines.at(2).value("measured_at").toString(), QString("t+0"));
        QCOMPARE(lines.at(3).value("measured_at").toString(), QString("t+300 s"));
        QCOMPARE(lines.at(4).value("measured_at").toString(), QString("t-60 s"));
        QCOMPARE(lines.at(5).value("measured_at").toString(), QString("t+600 s"));
        // The source's own local time keeps its format only.
        QCOMPARE(lines.at(2).value("original_timestamp").toString(), QString("9/9/9999 99:99:99 AA"));
    }

    void millisecondOffsetsAndAnUnreadableFirstTime()
    {
        QJsonObject a = sample(100, "garbled 2026-03-04", "x");           // unreadable: no base yet
        QJsonObject b = sample(101, "2026-03-04T10:00:00.250Z", "x");     // becomes the base
        QJsonObject c = sample(102, "2026-03-04T10:00:01.500Z", "x");
        QJsonObject p;
        p.insert("type", "records");
        p.insert("feed", "current");
        p.insert("records", QJsonArray() << a << b << c);
        const QByteArray out = buildDiagnosticReport(QJsonDocument(p).toJson(QJsonDocument::Compact), context());
        const QList<QJsonObject> lines = parseLines(out);
        QCOMPARE(lines.at(2).value("measured_at").toString(), QString("<unreadable time aaaaaaa 9999-99-99>"));
        QCOMPARE(lines.at(3).value("measured_at").toString(), QString("t+0"));
        QCOMPARE(lines.at(4).value("measured_at").toString(), QString("t+1.250 s"));
        QVERIFY(!QString::fromUtf8(out).contains("2026"));
    }

    void contextIsComplete()
    {
        const QList<QJsonObject> lines = parseLines(buildDiagnosticReport(leakyBatch(), context()));
        const QJsonObject h = lines.at(0);
        QCOMPARE(h.value("line_type").toString(), QString("report"));
        QCOMPARE(h.value("format").toInt(), kDiagnosticReportFormat);
        QCOMPARE(h.value("application_version").toString(), QString("0.1.0-test"));
        QCOMPARE(h.value("adapter_id").toString(), QString("libre"));
        QCOMPARE(h.value("adapter_version").toString(), QString("0.2.0"));
        QCOMPARE(h.value("backend_version").toString(), QString("4.16"));
        QCOMPARE(h.value("command").toString(), QString("fetch_range"));
        const QJsonArray v = h.value("violations").toArray();
        QCOMPARE(v.size(), 2);
        QCOMPARE(v.at(0).toObject().value("kind").toString(), QString("undeclared-metric"));
        QCOMPARE(v.at(1).toObject().value("detail").toString(), QString("batch above max_records_per_batch"));
        const QJsonObject counts = h.value("counts").toObject();
        QCOMPARE(counts.value("records_received").toInt(), 7);
        QCOMPARE(counts.value("records_valid").toInt(), 4);
        QCOMPARE(counts.value("quarantined_by_reason").toObject().value("UndeclaredMetric").toInt(), 3);
        QCOMPARE(counts.value("hard_violations").toInt(), 1);
        const QJsonObject b = lines.at(1);
        QCOMPARE(b.value("status").toString(), QString("ok"));
        QCOMPARE(b.value("feed").toString(), QString("current"));
        QCOMPARE(b.value("record_count").toInt(), 4);
        QCOMPARE(b.value("id").toString(), QString("<removed>"));
        QCOMPARE(b.value("cursor").toString(), QString("string"));
    }

    void sameInputGivesTheSameBytes()
    {
        const QByteArray a = buildDiagnosticReport(leakyBatch(), context());
        const QByteArray b = buildDiagnosticReport(leakyBatch(), context());
        QCOMPARE(a, b);
        QVERIFY(!a.isEmpty());
        bool ok = false;
        const QList<QJsonObject> lines = parseLines(a, &ok);
        QVERIFY(ok);
        for (int i = 0; i < lines.size(); ++i)
            QVERIFY(!lines.at(i).isEmpty());
    }

    // An unparseable line still yields the context and a batch line with a status, and none
    // of its bytes.
    void unparseableLineGivesContextAndNoData()
    {
        const QList<QByteArray> inputs = QList<QByteArray>()
            << "this is Zoe Mueller's token tok/ABCDEF and {not json"
            << "[1,2,3]"
            << QByteArray("{\"feed\":\"current\",\"records\":[\"\xff\xfe broken\"]}")
            << QByteArray();
        for (int i = 0; i < inputs.size(); ++i) {
            const QByteArray out = buildDiagnosticReport(inputs.at(i), context());
            bool ok = false;
            const QList<QJsonObject> lines = parseLines(out, &ok);
            QVERIFY(ok);
            QCOMPARE(lines.size(), 2);
            QCOMPARE(lines.at(0).value("adapter_id").toString(), QString("libre"));
            QCOMPARE(lines.at(1).value("status").toString(), QString("unparseable"));
            QCOMPARE(lines.at(1).value("bytes").toInt(), inputs.at(i).size());
            QVERIFY(!out.contains("Mueller"));
            QVERIFY(!out.contains("ABCDEF"));
            QVERIFY(!out.contains("broken"));
        }
    }

    void aVeryLongLineIsCutAtTheLimit()
    {
        const QByteArray big = QByteArray(5000, 'x');
        const QByteArray out = buildDiagnosticReport(big, context(), 1000);
        const QList<QJsonObject> lines = parseLines(out);
        QCOMPARE(lines.size(), 2);
        QCOMPARE(lines.at(1).value("status").toString(), QString("cut"));
        QCOMPARE(lines.at(1).value("bytes").toInt(), 5000);
        QCOMPARE(lines.at(1).value("limit").toInt(), 1000);
        QVERIFY(!out.contains("xxxx"));
        QVERIFY(out.size() < 2000);

        // A valid batch just over the limit is also cut, a line exactly at it is read.
        const QByteArray valid = leakyBatch();
        QCOMPARE(parseLines(buildDiagnosticReport(valid, context(), valid.size())).at(1).value("status").toString(),
                 QString("ok"));
        QCOMPARE(parseLines(buildDiagnosticReport(valid, context(), valid.size() - 1)).at(1).value("status").toString(),
                 QString("cut"));
    }

    void moreThanTheCeilingIsCountedNotWritten()
    {
        QJsonArray records;
        for (int i = 0; i < kDiagnosticMaxRecords + 25; ++i)
            records.append(sample(100 + i, "2026-03-04T10:00:00Z", "x"));
        QJsonObject p;
        p.insert("type", "records");
        p.insert("feed", "current");
        p.insert("records", records);
        const QList<QJsonObject> lines =
            parseLines(buildDiagnosticReport(QJsonDocument(p).toJson(QJsonDocument::Compact), context()));
        QCOMPARE(lines.size(), 2 + kDiagnosticMaxRecords);
        QCOMPARE(lines.at(1).value("record_count").toInt(), kDiagnosticMaxRecords + 25);
        QCOMPARE(lines.at(1).value("records_omitted").toInt(), 25);
    }

    void entriesThatAreNotObjectsAreReducedToTheirType()
    {
        QJsonObject p;
        p.insert("type", "records");
        p.insert("feed", "undeclared-feed-from-the-source");
        p.insert("records", QJsonArray() << "a string with Zoe" << 42 << QJsonValue::Null);
        const QByteArray out = buildDiagnosticReport(QJsonDocument(p).toJson(QJsonDocument::Compact), context());
        const QList<QJsonObject> lines = parseLines(out);
        QCOMPARE(lines.size(), 5);
        QCOMPARE(lines.at(2).value("entry").toString(), QString("string"));
        QCOMPARE(lines.at(3).value("entry").toString(), QString("number"));
        QCOMPARE(lines.at(4).value("entry").toString(), QString("null"));
        QVERIFY(!out.contains("Zoe"));
        QVERIFY(!out.contains("undeclared-feed-from"));      // an undeclared feed name is masked
    }

    void aBatchWithoutRecordsOrWithOddKeysStillWorks()
    {
        const QByteArray out = buildDiagnosticReport(
            "{\"type\":\"records\",\"feed\":\"current\",\"records\":\"nope\","
            "\"4f3c9a1e-77aa-4b1c-9d00-1234567890ab\":1}", context());
        const QList<QJsonObject> lines = parseLines(out);
        QCOMPARE(lines.size(), 2);
        QCOMPARE(lines.at(1).value("record_count").toInt(), 0);
        QVERIFY(!out.contains("4f3c9a1e"));
    }
};

QTEST_APPLESS_MAIN(TstDiagnosticReport)
#include "tst_diagnosticreport.moc"
