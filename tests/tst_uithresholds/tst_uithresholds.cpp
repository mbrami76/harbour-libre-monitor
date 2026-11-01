// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Michele Brami
// Tests of the thresholds page and the settings of the configuration file. What they prove:
//  - every answer of SetThresholds is said in words, with the numbers the service gave, in BOTH units, and
//    every word has its text in the page's table and in English and Italian;
//  - an edit in mmol/L is rounded once to whole mg/dL by the core, and the numbers are written with the unit;
//  - a looser limit asks the challenge, and NO property of the page's model ever carries the value asked
//    for or the value typed again (the screen does not show the value just asked for); a refused
//    challenge names the limit, not the number; leaving the challenge sends nothing;
//  - the history is read on opening and after an accepted edit, newest first, with units (no note);
//  - the alert times are offered inside the service's range only, with its default and the value in force;
//  - the configuration file keeps unknown keys, writes the service keys, refuses a label of more than three
//    characters (never shortens it) and an account name of more than 24, and a file it cannot read is not
//    touched.
// Synthetic values only; the patients are UUIDs and invented names.
#include "alerts/thresholds.h"
#include "ui/configsettings.h"
#include "ui/configstore.h"
#include "ui/thresholdeditor.h"
#include "ui/thresholdview.h"

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QMetaProperty>
#include <QTemporaryDir>
#include <QtTest>

using namespace lm::ui;

namespace {

const char *const kS = "{cccccccc-0000-4000-8000-000000000001}";
const char *const kS2 = "{cccccccc-0000-4000-8000-000000000002}";

// The service's part: the facts of Alerts() and SetThresholds recorded, answered when the test says so.
class ScriptedAlerts : public AlertsBackend
{
public:
    AlertsFacts f;
    QStringList calls;
    AlertsFacts facts() const { return f; }
    void acknowledge(const QString &, const QString &) {}
    void snooze(const QString &, const QString &, int) {}
    void setSilent(const QString &, int) {}
    void endSilent(const QString &) {}
    void setThresholds(const QString &s, int l, int h, int tl, int th)
    {
        calls << QStringLiteral("%1 %2 %3 %4 %5").arg(s).arg(l).arg(h).arg(tl).arg(th);
    }
    void answer(const ThresholdAnswer &a, const char *subject = kS) { emit thresholdsAnswered(QString::fromLatin1(subject), a); }
};

// The journal's part: records the requests for the history and answers when told.
class ScriptedData : public Backend
{
public:
    QStringList requests;
    QList<SubjectSnapshot> subjects() const { return QList<SubjectSnapshot>(); }
    ServiceStatus serviceStatus() const { return ServiceStatus::Running; }
    void retry(const QString &) {}
    void reenable(const QString &) {}
    void startMonitoring() {}
    void askAgain() {}
    void requestThresholdHistory(const QString &s) { requests << s; }
    void history(bool ok, const QList<ThresholdHistoryEntry> &e, const char *subject = kS)
    {
        emit thresholdHistoryLoaded(QString::fromLatin1(subject), ok, e);
    }
};

AlertsFacts factsWith(int l, int h)
{
    AlertsFacts f;
    f.known = true;
    PatientAlertFacts p;
    p.subject = QString::fromLatin1(kS);
    p.ll = 54; p.l = l; p.h = h; p.hh = 250;
    p.persistenceLowS = 0; p.persistenceHighS = 900; p.missingLimitS = 900;
    f.patients << p;
    f.persistenceLow.minS = 0; f.persistenceLow.maxS = 900; f.persistenceLow.defaultS = 0;
    f.persistenceHigh.minS = 0; f.persistenceHigh.maxS = 3600; f.persistenceHigh.defaultS = 900;
    f.missingLimit.minS = 300; f.missingLimit.maxS = 3600; f.missingLimit.defaultS = 900;
    return f;
}

ThresholdAnswer answerOf(const char *result)
{
    ThresholdAnswer a;
    a.result = QString::fromLatin1(result);
    return a;
}

// Everything the page's model shows, as one text: every property, maps and lists included.
QString everythingShown(const QObject &o)
{
    QString out;
    const QMetaObject *m = o.metaObject();
    for (int i = 0; i < m->propertyCount(); ++i) {
        const QVariant v = m->property(i).read(&o);
        if (v.type() == QVariant::Map || v.type() == QVariant::List)
            out += QString::fromUtf8(QJsonDocument::fromVariant(v).toJson(QJsonDocument::Compact));
        else
            out += v.toString();
        out += QLatin1Char('|');
    }
    return out;
}

QString keysOf(const ThresholdMessage &m)
{
    QStringList k;
    for (int i = 0; i < m.parts.size(); ++i)
        k << m.parts.at(i).first + (m.parts.at(i).second.isEmpty() ? QString() : QLatin1Char('(') + m.parts.at(i).second.join(QLatin1Char(';')) + QLatin1Char(')'));
    return k.join(QLatin1Char(' '));
}

QString readFile(const QString &path) { QFile f(path); f.open(QIODevice::ReadOnly); return QString::fromUtf8(f.readAll()); }
QString translationOf(const QString &ts, const QString &id)
{
    const int at = ts.indexOf("<message id=\"" + id + "\"");
    if (at < 0) return QString();
    const int end = ts.indexOf("</message>", at);
    const QString block = ts.mid(at, end - at);
    if (block.contains("type=\"unfinished\"")) return QString();
    const int a = block.indexOf("<translation>");
    const int b = block.indexOf("</translation>");
    return a < 0 || b < 0 ? QString() : block.mid(a + 13, b - a - 13);
}

QJsonObject readJson(const QString &path)
{
    QFile f(path);
    f.open(QIODevice::ReadOnly);
    return QJsonDocument::fromJson(f.readAll()).object();
}

void writeText(const QString &path, const QByteArray &text)
{
    QFile f(path);
    f.open(QIODevice::WriteOnly | QIODevice::Truncate);
    f.write(text);
}

const QByteArray kConfig =
    "{\"format\":1,\"future_key\":7,\"connections\":[{\"id\":\"c1\",\"adapter\":\"libre\",\"other\":true,"
    "\"subjects\":[{\"id\":\"{cccccccc-0000-4000-8000-000000000001}\",\"ref\":\"r1\",\"name\":\"Anna Example\","
    "\"keep\":\"me\"},{\"id\":\"{cccccccc-0000-4000-8000-000000000002}\",\"ref\":\"r2\",\"name\":\"Bruno Example\","
    "\"label\":\"BX\"}]}]}";

} // namespace

class TstUiThresholds : public QObject
{
    Q_OBJECT

private slots:
    // ---- the service's answer ---------------------------------------------------------------------------
    void theAnswerOfSetThresholdsIsReadInEveryShape()
    {
        ThresholdAnswer a = parseThresholdAnswer("{\"result\":\"accepted\",\"thresholds\":{\"subject\":\"{x}\",\"ll\":54,\"l\":75,\"h\":200,\"hh\":250}}");
        QCOMPARE(a.result, QStringLiteral("accepted"));
        QCOMPARE(a.l, 75); QCOMPARE(a.h, 200); QCOMPARE(a.ll, 54); QCOMPARE(a.hh, 250);
        a = parseThresholdAnswer("{\"result\":\"inconsistent\",\"l_min\":55,\"l_max\":59,\"h_min\":61,\"h_max\":249,\"offending\":\"L\",\"new\":1}");
        QCOMPARE(a.result, QStringLiteral("inconsistent"));
        QCOMPARE(a.lMin, 55); QCOMPARE(a.lMax, 59); QCOMPARE(a.hMin, 61); QCOMPARE(a.hMax, 249);
        QCOMPARE(a.offending, QStringLiteral("L"));
        a = parseThresholdAnswer("{\"result\":\"confirmation-mismatch\",\"global_l\":70,\"global_h\":180,\"offending\":\"H\"}");
        QCOMPARE(a.globalL, 70); QCOMPARE(a.globalH, 180); QCOMPARE(a.offending, QStringLiteral("H"));
        QCOMPARE(parseThresholdAnswer("not json").result, QStringLiteral("unreadable"));
        QCOMPARE(parseThresholdAnswer("{\"format\":1}").result, QStringLiteral("unreadable"));
        QCOMPARE(parseThresholdAnswer("{\"result\":\"service-not-running\"}").l, -1);
    }

    // ---- numbers and units ----------------------------------------------------------------------------
    void numbersAreWrittenWithTheUnitInBothUnits()
    {
        const QLocale it(QLocale::Italian, QLocale::Italy), c(QLocale::c());
        QCOMPARE(thresholdWithUnit(70, "mg/dL", it), QStringLiteral("70 mg/dL"));
        QCOMPARE(thresholdWithUnit(70, "mmol/L", it), QStringLiteral("3,9 mmol/L"));
        QCOMPARE(thresholdWithUnit(70, "mmol/L", c), QStringLiteral("3.9 mmol/L"));
        QCOMPARE(thresholdWithUnit(180, "mmol/L", c), QStringLiteral("10.0 mmol/L"));
        QCOMPARE(thresholdNumber(1000, "mg/dL", it), QStringLiteral("1000"));     // never "1.000"
    }
    void whatTheUserTypedIsReadInTheChosenUnitAndRoundedOnce()
    {
        int v = 0;
        QVERIFY(parseThresholdInput(" 75 ", "mg/dL", &v)); QCOMPARE(v, 75);
        QVERIFY(!parseThresholdInput("75.5", "mg/dL", &v));          // mg/dL limits are whole numbers
        QVERIFY(!parseThresholdInput("", "mg/dL", &v));
        QVERIFY(!parseThresholdInput("-5", "mg/dL", &v));
        QVERIFY(!parseThresholdInput("7O", "mg/dL", &v));
        QVERIFY(parseThresholdInput("3,9", "mmol/L", &v)); QCOMPARE(v, 70);
        QVERIFY(parseThresholdInput("3.9", "mmol/L", &v)); QCOMPARE(v, 70);
        QVERIFY(parseThresholdInput("4,2", "mmol/L", &v)); QCOMPARE(v, 76);  // 75.66 rounded once
        QVERIFY(parseThresholdInput("10", "mmol/L", &v)); QCOMPARE(v, 180);
        QVERIFY(!parseThresholdInput("3,9,1", "mmol/L", &v));
        QVERIFY(!parseThresholdInput("abc", "mmol/L", &v));
        // The same as the core's own conversion, the one the service's documentation names.
        int core = 0;
        QVERIFY(lm::mmolEditToMgDl(4.2, &core));
        QCOMPARE(core, 76);
    }

    // ---- every refusal in words, with numbers, in both units -------------------------------------------
    void everyAnswerHasItsWordsAndNumbers_data()
    {
        QTest::addColumn<QString>("json");
        QTest::addColumn<QString>("unit");
        QTest::addColumn<bool>("success");
        QTest::addColumn<QString>("expected");
        QTest::newRow("accepted mg/dL") << "{\"result\":\"accepted\",\"thresholds\":{\"ll\":54,\"l\":75,\"h\":200,\"hh\":250}}" << "mg/dL"
                                        << true << "ui-thr-saved(75 mg/dL;200 mg/dL)";
        QTest::newRow("accepted mmol/L") << "{\"result\":\"accepted\",\"thresholds\":{\"ll\":54,\"l\":75,\"h\":200,\"hh\":250}}" << "mmol/L"
                                         << true << "ui-thr-saved(4,2 mmol/L;11,1 mmol/L)";
        QTest::newRow("no change") << "{\"result\":\"no-change\"}" << "mg/dL" << true << "ui-thr-no-change";
        QTest::newRow("impossible low mg/dL") << "{\"result\":\"invalid-value\",\"offending\":\"L\"}" << "mg/dL" << false
                                              << "ui-thr-err-invalid-low ui-thr-hint-mgdl";
        QTest::newRow("impossible high mmol/L") << "{\"result\":\"invalid-value\",\"offending\":\"H\"}" << "mmol/L" << false
                                                << "ui-thr-err-invalid-high ui-thr-hint-mmol";
        QTest::newRow("order low mg/dL") << "{\"result\":\"inconsistent\",\"offending\":\"L\",\"l_min\":55,\"l_max\":119,\"h_min\":191,\"h_max\":249}"
                                         << "mg/dL" << false
                                         << "ui-thr-err-order-low ui-thr-range-low(55 mg/dL;119 mg/dL)";
        QTest::newRow("order low mmol/L") << "{\"result\":\"inconsistent\",\"offending\":\"L\",\"l_min\":55,\"l_max\":119,\"h_min\":191,\"h_max\":249}"
                                          << "mmol/L" << false
                                          << "ui-thr-err-order-low ui-thr-range-low(3,1 mmol/L;6,6 mmol/L)";
        QTest::newRow("order high mmol/L") << "{\"result\":\"inconsistent\",\"offending\":\"H\",\"l_min\":55,\"l_max\":119,\"h_min\":191,\"h_max\":249}"
                                           << "mmol/L" << false << "ui-thr-err-order-high ui-thr-range-high(10,6 mmol/L;13,8 mmol/L)";
        // An empty range of the limit named (the other limit is the wrong one): the other range instead, never
        // "from 261 to 249".
        QTest::newRow("order high, empty range") << "{\"result\":\"inconsistent\",\"offending\":\"H\",\"l_min\":55,\"l_max\":299,\"h_min\":261,\"h_max\":249}"
                                                 << "mg/dL" << false << "ui-thr-err-order-high ui-thr-range-low(55 mg/dL;299 mg/dL)";
        QTest::newRow("not confirmed") << "{\"result\":\"needs-confirmation\",\"global_l\":70,\"global_h\":180}" << "mg/dL" << false
                                       << "ui-thr-err-not-confirmed";
        QTest::newRow("mismatch low, no number") << "{\"result\":\"confirmation-mismatch\",\"global_l\":70,\"global_h\":180,\"offending\":\"L\"}"
                                                 << "mg/dL" << false << "ui-thr-err-mismatch-low";
        QTest::newRow("mismatch high, no number") << "{\"result\":\"confirmation-mismatch\",\"global_l\":70,\"global_h\":180,\"offending\":\"H\"}"
                                                  << "mmol/L" << false << "ui-thr-err-mismatch-high";
        QTest::newRow("unknown patient") << "{\"result\":\"unknown-subject\"}" << "mg/dL" << false << "ui-thr-err-unknown-patient";
        QTest::newRow("not saved") << "{\"result\":\"not-saved\"}" << "mg/dL" << false << "ui-thr-err-not-saved";
        QTest::newRow("not running") << "{\"result\":\"service-not-running\"}" << "mg/dL" << false << "ui-thr-err-no-service";
        QTest::newRow("unreadable") << "garbage" << "mg/dL" << false << "ui-thr-err-refused";
        QTest::newRow("a newer word") << "{\"result\":\"brand-new\"}" << "mg/dL" << false << "ui-thr-err-refused";
    }
    void everyAnswerHasItsWordsAndNumbers()
    {
        QFETCH(QString, json);
        QFETCH(QString, unit);
        QFETCH(bool, success);
        QFETCH(QString, expected);
        const ThresholdMessage m = thresholdMessage(parseThresholdAnswer(json.toUtf8()), unit, QLocale(QLocale::Italian, QLocale::Italy));
        QCOMPARE(m.success, success);
        QCOMPARE(keysOf(m), expected);
        ThresholdAnswer failed;
        failed.result = QStringLiteral("call-failed");
        QCOMPARE(keysOf(thresholdMessage(failed, unit, QLocale::c())), QStringLiteral("ui-thr-err-no-service"));
    }
    void everyWordHasItsTextInThePageTableAndInEnglishAndItalian()
    {
        const QString en = readFile(QString(LM_TRANSLATIONS_DIR) + "/harbour-libre-monitor.ts");
        const QString it = readFile(QString(LM_TRANSLATIONS_DIR) + "/harbour-libre-monitor-it.ts");
        const QString js = readFile(QString(LM_QML_DIR) + "/components/ThresholdText.js");
        QVERIFY2(!en.isEmpty() && !it.isEmpty() && !js.isEmpty(), "a source file could not be read");
        // Every key the model can produce, collected from the functions themselves.
        QSet<QString> keys;
        const char *results[] = { "accepted", "no-change", "invalid-value", "inconsistent", "needs-confirmation",
                                  "confirmation-mismatch", "unknown-subject", "not-saved", "service-not-running",
                                  "call-failed", "unreadable" };
        const char *units[] = { "mg/dL", "mmol/L" };
        const char *offending[] = { "L", "H" };
        for (unsigned r = 0; r < sizeof(results) / sizeof(results[0]); ++r)
            for (unsigned u = 0; u < 2; ++u)
                for (unsigned o = 0; o < 2; ++o) {
                    ThresholdAnswer a;
                    a.result = QString::fromLatin1(results[r]);
                    a.offending = QString::fromLatin1(offending[o]);
                    a.l = 75; a.h = 200; a.lMin = 55; a.lMax = 199; a.hMin = 76; a.hMax = 249;
                    const ThresholdMessage m = thresholdMessage(a, QString::fromLatin1(units[u]), QLocale::c());
                    for (int i = 0; i < m.parts.size(); ++i)
                        keys << m.parts.at(i).first;
                }
        keys << QStringLiteral("ui-thr-saved-plain");
        QVERIFY(keys.size() >= 17);
        foreach (const QString &k, keys) {
            QVERIFY2(js.contains("case \"" + k + "\":"), qPrintable(k + ": the page table has no case"));
            QVERIFY2(js.contains("qsTrId(\"" + k + "\")"), qPrintable(k + ": the page table does not use the id"));
            QVERIFY2(!translationOf(en, k).isEmpty(), qPrintable(k + ": no English text"));
            QVERIFY2(!translationOf(it, k).isEmpty(), qPrintable(k + ": no Italian text"));
        }
        // The configuration file's errors too (ConfigSettings.errorKey).
        const char *configErrors[] = { "label-too-long", "account-label-too-long", "config-unreadable",
                                       "config-write-failed", "service-config-invalid", "service-no-answer" };
        for (unsigned i = 0; i < sizeof(configErrors) / sizeof(configErrors[0]); ++i)
            QVERIFY2(js.contains("case \"" + QString::fromLatin1(configErrors[i]) + "\":"), configErrors[i]);
    }

    // ---- the challenge -----------------------------------------------------------------------------------
    void theChallengeAsksOnlyForALimitThatMovesAndBecomesLooser()
    {
        ChallengeNeed n = challengeFor(62, 180, 70, 180, 70, 180);
        QVERIFY(n.low && !n.high);
        n = challengeFor(70, 220, 70, 180, 70, 180);
        QVERIFY(!n.low && n.high);
        n = challengeFor(60, 220, 70, 180, 70, 180);
        QVERIFY(n.low && n.high);
        // Going from a loosened 60 to 65 is still looser than the standard (the reference is the standard).
        n = challengeFor(65, 180, 60, 180, 70, 180);
        QVERIFY(n.low && !n.high);
        // A loosened low limit that stays as it is is not asked again while the high limit moves looser.
        n = challengeFor(60, 200, 60, 180, 70, 180);
        QVERIFY(!n.low && n.high);
        // When the service asked but nothing qualifies (the picture changed meanwhile): every limit that moves.
        n = challengeFor(75, 170, 70, 180, 70, 180);
        QVERIFY(n.low && n.high);
    }
    void aLooserLimitAsksTheChallengeAndTheValueIsNeverShown()
    {
        ScriptedAlerts alerts;
        alerts.f = factsWith(70, 180);
        ScriptedData data;
        ThresholdEditor e;
        e.setBackends(&data, &alerts);
        e.setDisplayUnit("mg/dL");
        e.setLocale(QLocale::c());
        e.open(QString::fromLatin1(kS));
        QCOMPARE(data.requests, QStringList() << QString::fromLatin1(kS));
        QVERIFY(e.known());
        QCOMPARE(e.lowText(), QStringLiteral("70"));
        QCOMPARE(e.standardLowText(), QStringLiteral("70 mg/dL"));
        QCOMPARE(e.veryLowText(), QStringLiteral("54 mg/dL"));
        QCOMPARE(e.veryHighText(), QStringLiteral("250 mg/dL"));

        e.save("62", "180");
        QCOMPARE(alerts.calls.last(), QStringLiteral("%1 62 180 -1 -1").arg(kS));
        QCOMPARE(e.state(), QStringLiteral("sending"));
        ThresholdAnswer needs = answerOf("needs-confirmation");
        needs.globalL = 70; needs.globalH = 180;
        alerts.answer(needs);
        QCOMPARE(e.state(), QStringLiteral("challenge"));
        QVERIFY(e.challengeLow() && !e.challengeHigh());
        // The challenge screen shows the standard value, never the value asked for.
        QVERIFY2(!everythingShown(e).contains("62"), qPrintable(everythingShown(e)));

        // A wrong value typed again: sent once, then forgotten; the refusal names the limit, not a number.
        e.confirmChallenge("61", "");
        QCOMPARE(alerts.calls.last(), QStringLiteral("%1 62 180 61 0").arg(kS));
        QVERIFY(!everythingShown(e).contains("61") && !everythingShown(e).contains("62"));
        ThresholdAnswer mismatch = answerOf("confirmation-mismatch");
        mismatch.offending = "L"; mismatch.globalL = 70; mismatch.globalH = 180;
        alerts.answer(mismatch);
        QCOMPARE(e.state(), QStringLiteral("idle"));
        QCOMPARE(keysOf(thresholdMessage(mismatch, "mg/dL", QLocale::c())), QStringLiteral("ui-thr-err-mismatch-low"));
        QVERIFY2(!everythingShown(e).contains("61") && !everythingShown(e).contains("62"), qPrintable(everythingShown(e)));
        QCOMPARE(data.requests.size(), 1);                              // nothing changed: no new history

        // Again, right this time: accepted, the stored value is shown back, the history is read again.
        e.save("62", "180");
        alerts.answer(needs);
        e.confirmChallenge("62", "");
        QCOMPARE(alerts.calls.last(), QStringLiteral("%1 62 180 62 0").arg(kS));
        ThresholdAnswer ok = answerOf("accepted");
        ok.ll = 54; ok.l = 62; ok.h = 180; ok.hh = 250;
        alerts.answer(ok);
        QVERIFY(e.message().value("success").toBool());
        QVERIFY(everythingShown(e).contains("62 mg/dL"));               // after the acceptance it is shown back
        QCOMPARE(data.requests.size(), 2);
    }
    void theChallengeInMmolIsRoundedOnceAndNotShownEither()
    {
        ScriptedAlerts alerts;
        alerts.f = factsWith(70, 180);
        ScriptedData data;
        ThresholdEditor e;
        e.setBackends(&data, &alerts);
        e.setDisplayUnit("mmol/L");
        e.setLocale(QLocale(QLocale::Italian, QLocale::Italy));
        e.open(QString::fromLatin1(kS));
        QCOMPARE(e.lowText(), QStringLiteral("3,9"));
        QCOMPARE(e.standardHighText(), QStringLiteral("10,0 mmol/L"));
        e.save("3,4", "12,5");
        QCOMPARE(alerts.calls.last(), QStringLiteral("%1 61 225 -1 -1").arg(kS));     // 61.25 and 225.2, once
        ThresholdAnswer needs = answerOf("needs-confirmation");
        needs.globalL = 70; needs.globalH = 180;
        alerts.answer(needs);
        QVERIFY(e.challengeLow() && e.challengeHigh());
        const QString shown = everythingShown(e);
        QVERIFY2(!shown.contains("3,4") && !shown.contains("12,5") && !shown.contains("61") && !shown.contains("225"), qPrintable(shown));
        e.confirmChallenge("3.4", "12,5");
        QCOMPARE(alerts.calls.last(), QStringLiteral("%1 61 225 61 225").arg(kS));
    }
    void leavingTheChallengeSendsNothing()
    {
        ScriptedAlerts alerts;
        alerts.f = factsWith(70, 180);
        ScriptedData data;
        ThresholdEditor e;
        e.setBackends(&data, &alerts);
        e.setDisplayUnit("mg/dL");
        e.open(QString::fromLatin1(kS));
        e.save("70", "230");
        ThresholdAnswer needs = answerOf("needs-confirmation");
        needs.globalL = 70; needs.globalH = 180;
        alerts.answer(needs);
        QVERIFY(!e.challengeLow() && e.challengeHigh());
        e.cancelChallenge();
        QCOMPARE(alerts.calls.size(), 1);
        QCOMPARE(e.state(), QStringLiteral("idle"));
        QCOMPARE(e.message().value("parts").toList().first().toMap().value("key").toString(), QStringLiteral("ui-thr-err-not-confirmed"));
        // Neither the confirmation without a challenge nor a second save while one is on its way sends anything.
        e.confirmChallenge("230", "230");
        QCOMPARE(alerts.calls.size(), 1);
    }
    void anImpossibleTextIsRefusedWithoutSendingAndOtherPatientsAnswersAreIgnored()
    {
        ScriptedAlerts alerts;
        alerts.f = factsWith(70, 180);
        ScriptedData data;
        ThresholdEditor e;
        e.setBackends(&data, &alerts);
        e.setDisplayUnit("mg/dL");
        e.open(QString::fromLatin1(kS));
        e.save("seventy", "180");
        QVERIFY(alerts.calls.isEmpty());
        QCOMPARE(e.message().value("parts").toList().first().toMap().value("key").toString(), QStringLiteral("ui-thr-err-invalid-low"));
        e.save("70", "18O");
        QVERIFY(alerts.calls.isEmpty());
        QCOMPARE(e.message().value("parts").toList().first().toMap().value("key").toString(), QStringLiteral("ui-thr-err-invalid-high"));
        e.save("75", "180");
        QCOMPARE(e.state(), QStringLiteral("sending"));
        alerts.answer(answerOf("accepted"), kS2);                       // another page's answer
        QCOMPARE(e.state(), QStringLiteral("sending"));
        alerts.answer(answerOf("not-saved"));
        QCOMPARE(e.state(), QStringLiteral("idle"));
        // Without an answer of Alerts() the limits are not known and nothing can be saved.
        alerts.f = AlertsFacts();
        QVERIFY(!e.known());
        QVERIFY(!e.timesKnown());
    }

    // ---- the history ------------------------------------------------------------------------------------
    void theHistoryIsNewestFirstWithUnitsAndWithoutTheNote()
    {
        QList<ThresholdHistoryEntry> h;
        ThresholdHistoryEntry a; a.timeMs = 1000; a.boundary = "L"; a.value = 65; a.previous = 70;
        ThresholdHistoryEntry b; b.timeMs = 2000; b.boundary = "H"; b.value = 200; b.previous = 180;
        h << a << b;
        const QVariantList rows = thresholdHistoryRows(h, "mmol/L", QLocale(QLocale::Italian, QLocale::Italy));
        QCOMPARE(rows.size(), 2);
        QCOMPARE(rows.at(0).toMap().value("boundary").toString(), QStringLiteral("high"));
        QCOMPARE(rows.at(0).toMap().value("valueText").toString(), QStringLiteral("11,1 mmol/L"));
        QCOMPARE(rows.at(0).toMap().value("previousText").toString(), QStringLiteral("10,0 mmol/L"));
        QCOMPARE(rows.at(1).toMap().value("valueText").toString(), QStringLiteral("3,6 mmol/L"));
        QCOMPARE(QStringList(rows.at(1).toMap().keys()).join(QLatin1Char(',')), QStringLiteral("boundary,previousText,timeMs,valueText"));

        ScriptedAlerts alerts;
        alerts.f = factsWith(65, 200);
        ScriptedData data;
        ThresholdEditor e;
        e.setBackends(&data, &alerts);
        e.setDisplayUnit("mg/dL");
        e.open(QString::fromLatin1(kS));
        QCOMPARE(e.historyState(), QStringLiteral("loading"));
        data.history(true, h, kS2);                                     // another patient's: ignored
        QCOMPARE(e.historyState(), QStringLiteral("loading"));
        data.history(true, h);
        QCOMPARE(e.historyState(), QStringLiteral("ready"));
        QCOMPARE(e.history().size(), 2);
        QCOMPARE(e.history().at(1).toMap().value("valueText").toString(), QStringLiteral("65 mg/dL"));
        data.history(false, QList<ThresholdHistoryEntry>());
        QCOMPARE(e.historyState(), QStringLiteral("unreadable"));
        QVERIFY(e.history().isEmpty());
    }

    // ---- the alert times ------------------------------------------------------------------------------
    void theAlertTimesAreOfferedInsideTheServicesRangeOnly()
    {
        AlertTimeRange low; low.minS = 0; low.maxS = 900; low.defaultS = 0;
        QCOMPARE(alertTimeChoices(low, 0), QList<int>() << 0 << 300 << 600 << 900);
        AlertTimeRange high; high.minS = 0; high.maxS = 3600; high.defaultS = 900;
        const QList<int> h = alertTimeChoices(high, 900);
        QCOMPARE(h.size(), 13);
        QCOMPARE(h.first(), 0);
        QCOMPARE(h.last(), 3600);
        AlertTimeRange missing; missing.minS = 300; missing.maxS = 3600; missing.defaultS = 900;
        const QList<int> m = alertTimeChoices(missing, 420);                 // a value in force between steps
        QVERIFY(m.contains(420));
        QCOMPARE(m.first(), 300);
        QCOMPARE(m.last(), 3600);
        QVERIFY(!alertTimeChoices(missing, 7200).contains(7200));          // outside the range: not offered
        QVERIFY(!alertTimeChoices(missing, 60).contains(60));
        AlertTimeRange odd; odd.minS = 90; odd.maxS = 700; odd.defaultS = 100;
        QCOMPARE(alertTimeChoices(odd, -1), QList<int>() << 90 << 100 << 300 << 600 << 700);
        QVERIFY(alertTimeChoices(AlertTimeRange(), 900).isEmpty());          // not known: nothing offered
        AlertTimeRange wrong; wrong.minS = 900; wrong.maxS = 300; wrong.defaultS = 600;
        QVERIFY(alertTimeChoices(wrong, 600).isEmpty());

        ScriptedAlerts alerts;
        alerts.f = factsWith(70, 180);
        ScriptedData data;
        ThresholdEditor e;
        e.setBackends(&data, &alerts);
        e.open(QString::fromLatin1(kS));
        QVERIFY(e.timesKnown());
        const QVariantList choices = e.timeChoices("high");
        QCOMPARE(choices.size(), 13);
        QCOMPARE(choices.at(e.timeIndex("high")).toMap().value("seconds").toInt(), 900);
        QVERIFY(choices.at(e.timeIndex("high")).toMap().value("isDefault").toBool());
        QCOMPARE(e.timeChoices("missing").first().toMap().value("seconds").toInt(), 300);
        QVERIFY(e.timeChoices("nonsense").isEmpty());
        alerts.f.persistenceHigh = AlertTimeRange();                      // a range the answer did not carry
        QVERIFY(!e.timesKnown());
        QCOMPARE(e.timeIndex("high"), -1);
    }

    // ---- the configuration file --------------------------------------------------------------------------
    void theConfigurationKeepsUnknownKeysAndWritesTheServicesKeys()
    {
        QTemporaryDir dir;
        const QString path = dir.path() + "/config.json";
        writeText(path, kConfig);
        ConfigStore c;
        QVERIFY(c.load(path));
        QCOMPARE(c.language(), QString());
        QVERIFY(!c.alertsShowValue());
        c.setLanguage("it");
        c.setAlertsShowValue(true);
        QVERIFY(c.setConnectionLabel("c1", " Account "));
        QVERIFY(c.setSubjectLabel(QString::fromLatin1(kS), " AB "));
        QVERIFY(c.setSubjectAlertTime(QString::fromLatin1(kS), AlertTimeKey::missingLimit(), 600));
        QVERIFY(c.setSubjectAlertTime(QString::fromLatin1(kS), AlertTimeKey::persistenceHigh(), 0));
        QVERIFY(!c.setSubjectLabel("{not-there}", "X"));
        QVERIFY(c.save());
        QJsonObject root = readJson(path);
        QCOMPARE(root.value("language").toString(), QStringLiteral("it"));
        QCOMPARE(root.value("alerts_show_value").toBool(), true);
        QCOMPARE(root.value("future_key").toInt(), 7);
        QJsonObject conn = root.value("connections").toArray().at(0).toObject();
        QCOMPARE(conn.value("label").toString(), QStringLiteral("Account"));
        QCOMPARE(conn.value("other").toBool(), true);
        QJsonObject s1 = conn.value("subjects").toArray().at(0).toObject();
        QCOMPARE(s1.value("label").toString(), QStringLiteral("AB"));
        QCOMPARE(s1.value("missing_limit_s").toInt(), 600);
        QCOMPARE(s1.value("alert_persistence_high_s").toInt(), 0);
        QVERIFY(!s1.contains("alert_persistence_low_s"));
        QCOMPARE(s1.value("keep").toString(), QStringLiteral("me"));
        QCOMPARE(s1.value("name").toString(), QStringLiteral("Anna Example"));

        // A round trip through setConnection (what saving the account does) keeps the label and the times.
        ConfigStore again;
        QVERIFY(again.load(path));
        const QList<ConnectionEntry> conns = again.connections();
        QCOMPARE(conns.first().label, QStringLiteral("Account"));
        QCOMPARE(conns.first().subjects.first().missingLimitS, 600);
        QCOMPARE(conns.first().subjects.first().persistenceLowS, -1);
        again.setConnection(conns.first());
        QVERIFY(again.save());
        s1 = readJson(path).value("connections").toArray().at(0).toObject().value("subjects").toArray().at(0).toObject();
        QCOMPARE(s1.value("missing_limit_s").toInt(), 600);
        QCOMPARE(readJson(path).value("connections").toArray().at(0).toObject().value("label").toString(), QStringLiteral("Account"));

        // An empty value takes the key out (the service's default or P1 then applies).
        QVERIFY(again.setSubjectLabel(QString::fromLatin1(kS), ""));
        QVERIFY(again.setSubjectAlertTime(QString::fromLatin1(kS), AlertTimeKey::missingLimit(), -1));
        again.setLanguage("");
        QVERIFY(again.save());
        root = readJson(path);
        s1 = root.value("connections").toArray().at(0).toObject().value("subjects").toArray().at(0).toObject();
        QVERIFY(!s1.contains("label") && !s1.contains("missing_limit_s") && !root.contains("language"));
    }
    void theSettingsRefuseALongLabelAndNeverShortenIt()
    {
        QTemporaryDir dir;
        const QString path = dir.path() + "/config.json";
        writeText(path, kConfig);
        ConfigSettings s;
        QSignalSpy written(&s, SIGNAL(written()));
        s.load(path, QString());
        QVERIFY(s.available());
        QVERIFY(s.hasAccount());
        const QVariantList patients = s.patients();
        QCOMPARE(patients.size(), 2);
        QCOMPARE(patients.at(0).toMap().value("shortLabel").toString(), QStringLiteral("P1"));
        QCOMPARE(patients.at(1).toMap().value("shortLabel").toString(), QStringLiteral("BX"));
        QCOMPARE(patients.at(1).toMap().value("defaultLabel").toString(), QStringLiteral("P2"));

        const QByteArray before = readFile(path).toUtf8();
        QVERIFY(!s.setPatientLabel(QString::fromLatin1(kS), "Anna"));
        QCOMPARE(s.errorKey(), QStringLiteral("label-too-long"));
        QCOMPARE(readFile(path).toUtf8(), before);                      // nothing written, nothing shortened
        QCOMPARE(written.count(), 0);
        QVERIFY(s.setPatientLabel(QString::fromLatin1(kS), " an "));
        QCOMPARE(s.errorKey(), QString());
        QCOMPARE(s.patients().at(0).toMap().value("shortLabel").toString(), QStringLiteral("an"));
        QCOMPARE(written.count(), 1);

        QVERIFY(!s.setConnectionLabel(QString(25, QLatin1Char('x')), "Account"));
        QCOMPARE(s.errorKey(), QStringLiteral("account-label-too-long"));
        QVERIFY(s.setConnectionLabel("   ", "Account"));                 // empty: the default word comes back
        QCOMPARE(s.connectionLabel(), QStringLiteral("Account"));
        QVERIFY(s.setConnectionLabel("Home", "Account"));
        QCOMPARE(s.connectionLabel(), QStringLiteral("Home"));

        QVERIFY(!s.setLanguage("de"));                                    // no translation file for it
        QVERIFY(s.setLanguage("it"));
        QCOMPARE(s.language(), QStringLiteral("it"));
        QVERIFY(s.setAlertsShowValue(true));
        QVERIFY(s.setAlertTime(QString::fromLatin1(kS2), "missing", 1200));
        QVERIFY(!s.setAlertTime(QString::fromLatin1(kS2), "sometime", 1200));
        QVERIFY(!s.setAlertTime(QString::fromLatin1(kS2), "missing", -5));
        const int writes = written.count();
        QVERIFY(s.setAlertsShowValue(true));                             // no change: nothing written
        QCOMPARE(written.count(), writes);
        const QJsonObject root = readJson(path);
        QCOMPARE(root.value("language").toString(), QStringLiteral("it"));
        QVERIFY(root.value("alerts_show_value").toBool());
        QCOMPARE(root.value("connections").toArray().at(0).toObject().value("subjects").toArray().at(1).toObject()
                 .value("missing_limit_s").toInt(), 1200);
        QCOMPARE(root.value("future_key").toInt(), 7);

        s.configApplied(true, "config-invalid");
        QCOMPARE(s.errorKey(), QStringLiteral("service-config-invalid"));
        s.configApplied(false, QString());
        QCOMPARE(s.errorKey(), QStringLiteral("service-no-answer"));
        s.clearError();
        QCOMPARE(s.errorKey(), QString());
    }
    void aFileThatCannotBeReadIsNeverTouched()
    {
        QTemporaryDir dir;
        const QString path = dir.path() + "/config.json";
        writeText(path, "{\"format\":9,\"connections\":[]}");          // a newer format
        ConfigSettings s;
        s.load(path);
        QVERIFY(!s.available());
        QCOMPARE(s.errorKey(), QStringLiteral("config-unreadable"));
        QVERIFY(!s.setLanguage("it"));
        QVERIFY(!s.setAlertsShowValue(true));
        QCOMPARE(readFile(path), QStringLiteral("{\"format\":9,\"connections\":[]}"));
        writeText(path, "not json");
        s.load(path);
        QVERIFY(!s.available());
        QVERIFY(!s.setPatientLabel(QString::fromLatin1(kS), "A"));
        QCOMPARE(readFile(path), QStringLiteral("not json"));
        // No file yet (the first run): a valid, empty configuration; no account and no patients.
        ConfigSettings fresh;
        fresh.load(dir.path() + "/none.json");
        QVERIFY(fresh.available());
        QVERIFY(!fresh.hasAccount());
        QVERIFY(fresh.patients().isEmpty());
    }
};

QTEST_GUILESS_MAIN(TstUiThresholds)
#include "tst_uithresholds.moc"
