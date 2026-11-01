// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Michele Brami
// Tests of the alert parts of the interface: reading the service's Alerts() answer, the order of the alerts list, the buttons of each alert,
// silent mode and its confirmation, the rows of the tiles and of the cover, the words of the answers,
// landing on the overview after a banner tap, and the scripted source of development builds.
// Every time is synthetic and no glucose value is involved: the service's answer carries none.
#include "ui/alertmodel.h"
#include "ui/alertview.h"
#include "ui/fakealertsbackend.h"
#include "ui/fakebackend.h"
#include "ui/subjectmodel.h"

#include <QSignalSpy>
#include <QtTest>

using namespace lm::ui;

namespace {

const char *const kA = "{aaaaaaaa-0000-4000-8000-00000000000a}";
const char *const kB = "{aaaaaaaa-0000-4000-8000-00000000000b}";
const char *const kC = "{aaaaaaaa-0000-4000-8000-00000000000c}";

// Today at a local time, as milliseconds: the tests of "today" do not depend on the time zone.
qint64 localToday(int h, int m, int addDays = 0)
{
    return QDateTime(QDate::currentDate().addDays(addDays), QTime(h, m)).toMSecsSinceEpoch();
}

QString iso(qint64 ms)
{
    return QDateTime::fromMSecsSinceEpoch(ms).toUTC().toString(Qt::ISODate);
}

class TestBackend : public Backend
{
public:
    QList<SubjectSnapshot> list;
    qint64 now = localToday(10, 0);
    QList<SubjectSnapshot> subjects() const { return list; }
    ServiceStatus serviceStatus() const { return ServiceStatus::Running; }
    qint64 nowMs() const { return now; }
    void retry(const QString &) {}
    void reenable(const QString &) {}
    void startMonitoring() {}
    void askAgain() {}
};

// An alerts backend that records the requests and lets the test set the facts and answer.
class TestAlerts : public AlertsBackend
{
public:
    AlertsFacts f;
    QStringList calls;
    AlertsFacts facts() const { return f; }
    void acknowledge(const QString &s, const QString &slot) { calls << QStringLiteral("ack %1 %2").arg(s, slot); }
    void snooze(const QString &s, const QString &slot, int m) { calls << QStringLiteral("snooze %1 %2 %3").arg(s, slot).arg(m); }
    void setSilent(const QString &s, int m) { calls << QStringLiteral("silent %1 %2").arg(s).arg(m); }
    void endSilent(const QString &s) { calls << QStringLiteral("end %1").arg(s); }
    void setThresholds(const QString &s, int l, int h, int tl, int th) { calls << QStringLiteral("thr %1 %2 %3 %4 %5").arg(s).arg(l).arg(h).arg(tl).arg(th); }
    void set(const AlertsFacts &n) { f = n; emit changed(); }
    void answer(const QString &result) { emit answered(QStringLiteral("acknowledge"), QString::fromLatin1(kA), result); }
};

SubjectSnapshot person(const char *id, const char *label)
{
    SubjectSnapshot s;
    s.id = QString::fromLatin1(id);
    s.name = QStringLiteral("Name");
    s.label = QString::fromLatin1(label);
    s.report.state = QStringLiteral("idle");
    return s;
}

AlertFact fact(const char *subject, const char *slot, const char *level, const char *state, qint64 levelSince,
               const char *episode = "e")
{
    AlertFact a;
    a.subject = QString::fromLatin1(subject);
    a.slot = QString::fromLatin1(slot);
    a.level = QString::fromLatin1(level);
    a.state = QString::fromLatin1(state);
    a.acknowledged = a.state == QLatin1String("acknowledged");
    a.levelSinceMs = a.sinceMs = levelSince;
    a.episode = QString::fromLatin1(episode);
    return a;
}

QByteArray serviceAnswer(qint64 now)
{
    // The shape, as tst_daemon checks the service gives it.
    return QStringLiteral(
        "{\"format\":1,\"do_not_disturb\":true,"
        "\"alerts\":[{\"subject\":\"%1\",\"slot\":\"low\",\"level\":\"LL\",\"state\":\"active\",\"acknowledged\":false,"
        "\"episode\":\"{e1}\",\"since\":\"%2\",\"level_since\":\"%3\",\"episode_since\":\"%3\",\"limit\":54,"
        "\"newest_reading_at\":\"%2\",\"snooze_until\":\"\",\"cause\":\"\",\"later_key\":7},"
        "{\"subject\":\"%4\",\"slot\":\"missing\",\"level\":\"MISSING\",\"state\":\"snoozed\",\"acknowledged\":false,"
        "\"episode\":\"{e2}\",\"since\":\"%2\",\"level_since\":\"\",\"episode_since\":\"%2\",\"limit\":null,"
        "\"newest_reading_at\":\"\",\"snooze_until\":\"%5\",\"cause\":\"no-new-readings\"}],"
        "\"thresholds\":[{\"subject\":\"%1\",\"ll\":54,\"l\":70,\"h\":180,\"hh\":250,\"persistence_low_s\":0,"
        "\"persistence_high_s\":900,\"missing_limit_s\":900,\"silent\":true,\"silent_until\":\"%5\"},"
        "{\"subject\":\"%4\",\"ll\":54,\"l\":72,\"h\":200,\"hh\":250,\"persistence_low_s\":60,"
        "\"persistence_high_s\":1200,\"missing_limit_s\":600,\"silent\":true,\"silent_until\":\"\"}],"
        "\"limits\":{\"persistence_low_s\":{\"min\":0,\"max\":900,\"default\":0},"
        "\"persistence_high_s\":{\"min\":0,\"max\":3600,\"default\":900},"
        "\"missing_limit_s\":{\"min\":300,\"max\":3600,\"default\":900}}}")
        .arg(QString::fromLatin1(kA), iso(now - 120000), iso(now - 600000), QString::fromLatin1(kB), iso(now + 1800000))
        .toUtf8();
}

QVariant role(const QAbstractListModel &m, int row, const char *name)
{
    const QHash<int, QByteArray> r = m.roleNames();
    for (QHash<int, QByteArray>::const_iterator it = r.begin(); it != r.end(); ++it)
        if (it.value() == name)
            return m.data(m.index(row), it.key());
    return QVariant();
}

} // namespace

class TstUiAlerts : public QObject
{
    Q_OBJECT

private slots:
    // ---- the service's answer --------------------------------------------------------------------------
    void theAnswerOfAlertsIsReadWithEveryFieldThePagesUse()
    {
        const qint64 now = localToday(10, 0);
        const AlertsFacts f = parseAlertsJson(serviceAnswer(now));
        QVERIFY(f.known);
        QVERIFY(f.error.isEmpty());
        QCOMPARE(f.alerts.size(), 2);
        const AlertFact &ll = f.alerts.at(0);
        QCOMPARE(ll.subject, QString::fromLatin1(kA));
        QCOMPARE(ll.slot, QStringLiteral("low"));
        QCOMPARE(ll.level, QStringLiteral("LL"));
        QCOMPARE(ll.state, QStringLiteral("active"));
        QCOMPARE(ll.episode, QStringLiteral("{e1}"));
        QCOMPARE(ll.acknowledged, false);
        QCOMPARE(ll.levelSinceMs, (now - 600000) / 1000 * 1000);
        QCOMPARE(ll.sinceMs, (now - 120000) / 1000 * 1000);
        QCOMPARE(ll.limitMgdl, 54);
        QCOMPARE(ll.snoozeUntilMs, qint64(0));
        const AlertFact &missing = f.alerts.at(1);
        QCOMPARE(missing.limitMgdl, -1);                     // null for no data
        QCOMPARE(missing.cause, QStringLiteral("no-new-readings"));
        QCOMPARE(missing.levelSinceMs, missing.sinceMs);     // no level_since: the state's start
        QVERIFY(missing.snoozeUntilMs > now);
        QCOMPARE(f.patients.size(), 2);
        const PatientAlertFacts *a = f.patient(QString::fromLatin1(kA));
        QVERIFY(a);
        QCOMPARE(a->l, 70);
        QCOMPARE(a->h, 180);
        QCOMPARE(a->persistenceHighS, 900);
        QVERIFY(a->silent);
        QVERIFY(a->silentUntilMs > now);
        const PatientAlertFacts *b = f.patient(QString::fromLatin1(kB));
        QVERIFY(b && b->silent);
        QCOMPARE(b->silentUntilMs, qint64(0));               // silent without limit
        QCOMPARE(b->missingLimitS, 600);
        QCOMPARE(f.missingLimit.minS, 300);
        QCOMPARE(f.persistenceHigh.maxS, 3600);
        QCOMPARE(f.persistenceLow.defaultS, 0);
        QVERIFY(f.doNotDisturb);
    }
    void aServiceThatIsNotRunningOrATextThisVersionCannotReadIsNotKnown()
    {
        AlertsFacts f = parseAlertsJson("{\"format\":1,\"error\":\"service-not-running\"}");
        QVERIFY(!f.known);
        QCOMPARE(f.error, QStringLiteral("service-not-running"));
        f = parseAlertsJson("not json");
        QVERIFY(!f.known);
        QCOMPARE(f.error, QStringLiteral("unreadable"));
        f = parseAlertsJson("{\"format\":2,\"alerts\":[]}");
        QVERIFY(!f.known);
        // An empty but readable answer is known: "no alerts" may be said.
        f = parseAlertsJson("{\"format\":1,\"alerts\":[],\"thresholds\":[]}");
        QVERIFY(f.known);
        QVERIFY(f.alerts.isEmpty());
    }
    void theAnswerOfSetSilentGivesTheWordAndTheEnd()
    {
        qint64 until = -5;
        QCOMPARE(parseSilentAnswer("{\"result\":\"done\",\"silent_until\":\"2026-10-20T04:00:00Z\"}", &until), QStringLiteral("done"));
        QVERIFY(until > 0);
        QCOMPARE(parseSilentAnswer("{\"result\":\"done\",\"silent_until\":\"\"}", &until), QStringLiteral("done"));
        QCOMPARE(until, qint64(0));
        QCOMPARE(parseSilentAnswer("{\"result\":\"bad-duration\",\"silent_until\":\"\"}", &until), QStringLiteral("bad-duration"));
        QCOMPARE(parseSilentAnswer("garbage", &until), QStringLiteral("unreadable"));
    }

    // ---- the list and the buttons ----------------------------------------------------------------------
    void theListIsInTheArbitersOrderWithTheEarlierOnsetFirst()
    {
        const qint64 now = localToday(10, 0);
        AlertsFacts f;
        f.known = true;
        f.alerts << fact(kA, "high", "H", "active", now - 1000)
                 << fact(kB, "low", "L", "active", now - 9000)
                 << fact(kC, "missing", "MISSING", "active", now - 5000)
                 << fact(kA, "low", "LL", "active", now - 2000)
                 << fact(kB, "high", "HH", "active", now - 3000)
                 << fact(kC, "low", "LL", "active", now - 7000);
        QList<SubjectSnapshot> subs;
        subs << person(kA, "") << person(kB, "") << person(kC, "XY");
        const QList<AlertRow> rows = buildAlertRows(f, subs, now);
        QStringList order;
        for (int i = 0; i < rows.size(); ++i)
            order << rows.at(i).fact.level + QLatin1Char('/') + rows.at(i).shortLabel;
        // very low (the earlier onset first), very high, no data, low, high
        QCOMPARE(order, QStringList() << "LL/XY" << "LL/P1" << "HH/P2" << "MISSING/XY" << "L/P2" << "H/P1");
        // The same rank and the same onset: the place in the file decides, so the order never jumps.
        f.alerts.clear();
        f.alerts << fact(kC, "low", "L", "active", now - 1000) << fact(kA, "low", "L", "active", now - 1000);
        const QList<AlertRow> tie = buildAlertRows(f, subs, now);
        QCOMPARE(tie.at(0).fact.subject, QString::fromLatin1(kA));
        // A patient that is not in the configuration file is "?", never a guess.
        f.alerts.clear();
        f.alerts << fact("{ffffffff-0000-4000-8000-000000000000}", "low", "L", "active", now);
        QCOMPARE(buildAlertRows(f, subs, now).at(0).shortLabel, QStringLiteral("?"));
    }
    void snoozeIsOfferedForLowHighAndNoDataOnlyAndNeverForVeryLowOrVeryHigh()
    {
        const qint64 now = localToday(10, 0);
        const char *levels[] = { "LL", "L", "H", "HH", "MISSING" };
        const bool snoozable[] = { false, true, true, false, true };
        for (int i = 0; i < 5; ++i) {
            AlertsFacts f;
            f.known = true;
            f.alerts << fact(kA, "low", levels[i], "active", now - 60000);
            const AlertRow r = buildAlertRows(f, QList<SubjectSnapshot>() << person(kA, ""), now).at(0);
            QVERIFY2(r.canAcknowledge, levels[i]);
            QCOMPARE(r.canSnooze, snoozable[i]);
            QCOMPARE(alertCanSnooze(QString::fromLatin1(levels[i])), snoozable[i]);
            QCOMPARE(r.minutesSince, 1);
        }
        // Acknowledged: no buttons; a very low or very high one is a pause (the page says the alert returns).
        AlertsFacts f;
        f.known = true;
        f.alerts << fact(kA, "low", "LL", "acknowledged", now) << fact(kB, "low", "L", "acknowledged", now)
                 << fact(kC, "high", "H", "snoozed", now);
        const QList<AlertRow> rows = buildAlertRows(f, QList<SubjectSnapshot>(), now);
        QVERIFY(!rows.at(0).canAcknowledge && !rows.at(0).canSnooze && rows.at(0).isPause);
        QVERIFY(!rows.at(1).canAcknowledge && !rows.at(1).canSnooze && !rows.at(1).isPause);
        QVERIFY(rows.at(2).snoozed && !rows.at(2).canAcknowledge && !rows.at(2).canSnooze);
    }
    void onlyAnActiveGlucoseAlertKeepsItsBandColourAndNoDataSaysWhyInWords()
    {
        const qint64 now = localToday(10, 0);
        AlertsFacts f;
        f.known = true;
        f.alerts << fact(kA, "low", "LL", "active", now) << fact(kA, "high", "H", "acknowledged", now)
                 << fact(kB, "high", "HH", "snoozed", now);
        AlertFact missing = fact(kC, "missing", "MISSING", "active", now);
        missing.cause = QStringLiteral("disabled-by-guard");
        f.alerts << missing;
        const QList<AlertRow> rows = buildAlertRows(f, QList<SubjectSnapshot>(), now);
        QCOMPARE(rows.at(0).bandKey, QStringLiteral("ll"));          // active very low: its badge and colour
        QCOMPARE(rows.at(1).bandKey, QString());                     // very high snoozed (rank 2): neutral
        QCOMPARE(rows.at(2).levelKey, QStringLiteral("missing"));
        QCOMPARE(rows.at(2).bandKey, QString());                     // no data: never a band colour
        QCOMPARE(rows.at(2).causeKey, QStringLiteral("ui-stale-guard"));
        QCOMPARE(rows.at(3).bandKey, QString());                     // acknowledged high: neutral
        QCOMPARE(rows.at(3).causeKey, QString());
        QCOMPARE(alertCauseKey(QStringLiteral("source-unreachable")), QStringLiteral("ui-stale-unreachable"));
        QCOMPARE(alertCauseKey(QStringLiteral("sensor-warm-up")), QStringLiteral("ui-stale-warmup"));
        QCOMPARE(alertCauseKey(QStringLiteral("something-new")), QStringLiteral("ui-stale-other"));
        // No row carries a value: the maps the QML reads have no such key.
        QVERIFY(!rows.at(0).toMap().contains(QStringLiteral("value")));
    }
    void theSnoozeButtonsAreTheFourDurationsTheServiceAccepts()
    {
        QCOMPARE(snoozeChoicesMinutes(), QList<int>() << 15 << 30 << 60 << 120);
        AlertModel m;
        QCOMPARE(m.snoozeChoices().size(), 4);
    }

    // ---- silent mode -----------------------------------------------------------------------------------
    void theSilentChoicesSendTheServicesEncodingAndOnlyWithoutLimitAsksForConfirmation()
    {
        TestBackend b;
        TestAlerts a;
        AlertModel m;
        m.setBackends(&b, &a);
        const QString s = QString::fromLatin1(kA);
        QCOMPARE(m.requestSilent(s, QStringLiteral("hour")), QStringLiteral("sent"));
        QCOMPARE(m.requestSilent(s, QStringLiteral("four-hours")), QStringLiteral("sent"));
        QCOMPARE(m.requestSilent(s, QStringLiteral("tomorrow")), QStringLiteral("sent"));
        QCOMPARE(a.calls, QStringList() << "silent " + s + " 60" << "silent " + s + " 240" << "silent " + s + " -1");
        // Without limit: nothing is sent until the confirmation was accepted.
        a.calls.clear();
        QCOMPARE(m.requestSilent(s, QStringLiteral("no-limit")), QStringLiteral("confirm"));
        QVERIFY(a.calls.isEmpty());
        m.confirmSilentWithoutLimit(s);
        QCOMPARE(a.calls, QStringList() << "silent " + s + " 0");
        QCOMPARE(m.requestSilent(s, QStringLiteral("forever")), QStringLiteral("invalid"));
        m.endSilent(s);
        QCOMPARE(a.calls.last(), "end " + s);
        QVERIFY(silentChoiceNeedsConfirmation(QStringLiteral("no-limit")));
        QVERIFY(!silentChoiceNeedsConfirmation(QStringLiteral("tomorrow")));
    }
    void silentModeShowsTheServicesEndWithTheDayOnlyWhenItIsNotToday()
    {
        const qint64 now = localToday(10, 0);
        AlertsFacts f;
        f.known = true;
        PatientAlertFacts today, tomorrow, noLimit, off;
        today.subject = QString::fromLatin1(kA); today.silent = true; today.silentUntilMs = localToday(14, 30);
        tomorrow.subject = QString::fromLatin1(kB); tomorrow.silent = true; tomorrow.silentUntilMs = localToday(6, 0, 1);
        noLimit.subject = QString::fromLatin1(kC); noLimit.silent = true;
        off.subject = QStringLiteral("{d}");
        f.patients << today << tomorrow << noLimit << off;
        SilentView v = silentViewOf(f, QString::fromLatin1(kA), now);
        QVERIFY(v.silent && !v.noLimit && v.untilToday);
        QCOMPARE(v.untilMs, localToday(14, 30));
        v = silentViewOf(f, QString::fromLatin1(kB), now);
        QVERIFY(v.silent && !v.noLimit && !v.untilToday);
        QCOMPARE(v.untilMs, localToday(6, 0, 1));            // as the service said: the UI computes no 06:00
        v = silentViewOf(f, QString::fromLatin1(kC), now);
        QVERIFY(v.silent && v.noLimit);
        QVERIFY(!silentViewOf(f, QStringLiteral("{d}"), now).silent);
        // Alerts that are not known say nothing about silent mode.
        f.known = false;
        QVERIFY(!silentViewOf(f, QString::fromLatin1(kA), now).silent);
    }

    // ---- the tiles and the cover -----------------------------------------------------------------------
    void theTileAndTheCoverRowsCarryTheAlertsAndSilentMode()
    {
        TestBackend b;
        b.list << person(kA, "") << person(kB, "B");
        TestAlerts a;
        a.f.known = true;
        a.f.alerts << fact(kA, "low", "LL", "active", b.now - 300000) << fact(kA, "missing", "MISSING", "acknowledged", b.now)
                   << fact(kB, "high", "H", "snoozed", b.now);
        PatientAlertFacts sa;
        sa.subject = QString::fromLatin1(kA); sa.silent = true; sa.silentUntilMs = localToday(14, 30);
        a.f.patients << sa;
        SubjectModel m;
        m.setBackend(&b);
        QCOMPARE(role(m, 0, "unacknowledgedAlerts").toInt(), 0);      // no alerts backend yet: nothing
        QCOMPARE(role(m, 0, "silent").toBool(), false);
        m.setAlertsBackend(&a);
        QCOMPARE(role(m, 0, "unacknowledgedAlerts").toInt(), 1);
        const QVariantList list = role(m, 0, "alertList").toList();
        QCOMPARE(list.size(), 2);
        QCOMPARE(list.at(0).toMap().value("levelKey").toString(), QStringLiteral("ll"));
        QCOMPARE(list.at(0).toMap().value("canAcknowledge").toBool(), true);
        QCOMPARE(list.at(1).toMap().value("canAcknowledge").toBool(), false);
        QCOMPARE(role(m, 0, "silent").toBool(), true);
        QCOMPARE(role(m, 0, "silentUntilToday").toBool(), true);
        QCOMPARE(qint64(role(m, 0, "silentUntilMs").toDouble()), localToday(14, 30));
        QCOMPARE(role(m, 1, "unacknowledgedAlerts").toInt(), 0);
        QCOMPARE(role(m, 1, "silent").toBool(), false);
        // The cover's rows carry the same (they are the model's rows as maps).
        const QVariantList cover = m.coverRows(2);
        bool sawSilent = false;
        for (int i = 0; i < cover.size(); ++i)
            if (cover.at(i).toMap().value("subjectId").toString() == QLatin1String(kA))
                sawSilent = cover.at(i).toMap().value("silent").toBool();
        QVERIFY(sawSilent);
        // A new answer of the service reaches the rows at once (no polling: the backend's signal).
        a.f.patients.clear();
        a.set(a.f);
        QCOMPARE(role(m, 0, "silent").toBool(), false);
    }

    // ---- actions and answers ---------------------------------------------------------------------------
    void theActionsReachTheBackendAndItsAnswersBecomeWords()
    {
        TestBackend b;
        TestAlerts a;
        AlertModel m;
        m.setBackends(&b, &a);
        m.acknowledge(QString::fromLatin1(kA), QStringLiteral("low"));
        m.snooze(QString::fromLatin1(kA), QStringLiteral("high"), 30);
        QCOMPARE(a.calls, QStringList() << QStringLiteral("ack %1 low").arg(kA) << QStringLiteral("snooze %1 high 30").arg(kA));
        QSignalSpy spy(&m, SIGNAL(resultChanged()));
        a.answer(QStringLiteral("done"));
        QCOMPARE(m.resultKey(), QString());                          // done: the picture says it
        a.answer(QStringLiteral("not-active"));
        QCOMPARE(m.resultKey(), QStringLiteral("ui-alert-err-not-active"));
        a.answer(QStringLiteral("call-failed"));
        QCOMPARE(m.resultKey(), QStringLiteral("ui-alert-err-no-service"));
        a.answer(QStringLiteral("not-snoozable"));
        QCOMPARE(m.resultKey(), QStringLiteral("ui-alert-err-refused"));
        QVERIFY(spy.count() >= 3);
        m.acknowledge(QString::fromLatin1(kA), QStringLiteral("low"));  // a new action clears the old word
        QCOMPARE(m.resultKey(), QString());
        QCOMPARE(alertResultKey(QStringLiteral("already-acknowledged")), QStringLiteral("ui-alert-err-already"));
        QCOMPARE(alertResultKey(QStringLiteral("not-silent")), QStringLiteral("ui-alert-err-not-silent"));
    }
    void theModelCountsAndSaysWhenTheAlertsAreNotKnown()
    {
        TestBackend b;
        b.list << person(kA, "");
        TestAlerts a;
        AlertModel m;
        m.setBackends(&b, &a);
        QVERIFY(!m.known());
        QCOMPARE(m.rowCount(), 0);
        AlertsFacts f;
        f.known = true;
        f.alerts << fact(kA, "low", "L", "active", b.now) << fact(kA, "high", "H", "acknowledged", b.now);
        a.set(f);
        QVERIFY(m.known());
        QCOMPARE(m.rowCount(), 2);
        QCOMPARE(m.unacknowledgedCount(), 1);
        QCOMPARE(role(m, 0, "levelKey").toString(), QStringLiteral("l"));
        QCOMPARE(role(m, 0, "shortLabel").toString(), QStringLiteral("P1"));
        QCOMPARE(m.forSubject(QString::fromLatin1(kA)).size(), 2);
        QCOMPARE(m.forSubject(QString::fromLatin1(kB)).size(), 0);
        QCOMPARE(m.silentOf(QString::fromLatin1(kA)).value("silent").toBool(), false);
    }

    // ---- landing on the overview ---------------------------------------------------------------
    void anAlertThatArrivedWhileTheWindowWasAwayLandsOnTheOverviewOnce()
    {
        TestBackend b;
        b.list << person(kA, "") << person(kB, "");
        TestAlerts a;
        a.f.known = true;
        a.f.alerts << fact(kA, "low", "L", "active", b.now, "e1");
        AlertModel m;
        m.setBackends(&b, &a);
        QSignalSpy land(&m, SIGNAL(landOnOverview()));
        // Away and back with the same alert: nothing new, no landing.
        m.setApplicationActive(false);
        m.setApplicationActive(true);
        a.set(a.f);
        QCOMPARE(land.count(), 0);
        // A new alert arrives while away (the service's signal reaches the background process) ...
        m.setApplicationActive(false);
        AlertsFacts f = a.f;
        f.alerts << fact(kB, "low", "LL", "active", b.now, "e2");
        a.set(f);
        QCOMPARE(land.count(), 0);                                   // not while away
        m.setApplicationActive(true);
        QCOMPARE(land.count(), 1);                                   // at the return to the front
        a.set(f);
        QCOMPARE(land.count(), 1);                                   // once only
        // ... or comes with the fresh answer asked for at the return.
        m.setApplicationActive(false);
        m.setApplicationActive(true);
        QCOMPARE(land.count(), 1);
        f.alerts << fact(kA, "missing", "MISSING", "active", b.now, "e3");
        a.set(f);
        QCOMPARE(land.count(), 2);
        // An alert acknowledged elsewhere meanwhile (in Events) does not land.
        m.setApplicationActive(false);
        f.alerts[1].state = QStringLiteral("acknowledged");
        f.alerts[1].acknowledged = true;
        m.setApplicationActive(true);
        a.set(f);
        QCOMPARE(land.count(), 2);
    }

    // ---- the scripted source of development builds -----------------------------------------------------
    void theScriptedSourcePlaysTheServiceForEveryAction()
    {
        FakeBackend subjects;
        FakeAlertsBackend fake(&subjects);
        subjects.setScenario(0);
        AlertsFacts f = fake.facts();
        QVERIFY(f.known);
        QCOMPARE(f.alerts.size(), 1);
        const QString marco = f.alerts.at(0).subject;
        QCOMPARE(f.alerts.at(0).level, QStringLiteral("LL"));
        QSignalSpy answered(&fake, SIGNAL(answered(QString,QString,QString)));
        fake.snooze(marco, QStringLiteral("low"), 15);
        QVERIFY(answered.wait(1000));
        QCOMPARE(answered.last().at(2).toString(), QStringLiteral("not-snoozable"));
        fake.acknowledge(marco, QStringLiteral("low"));
        QVERIFY(answered.wait(1000));
        QCOMPARE(answered.last().at(2).toString(), QStringLiteral("done"));
        QVERIFY(fake.facts().alerts.at(0).acknowledged);
        fake.acknowledge(marco, QStringLiteral("low"));
        QVERIFY(answered.wait(1000));
        QCOMPARE(answered.last().at(2).toString(), QStringLiteral("already-acknowledged"));
        // Until tomorrow: the next local 06:00, at least an hour away (the service's rule).
        fake.setSilent(marco, -1);
        QVERIFY(answered.wait(1000));
        const qint64 until = fake.facts().patient(marco)->silentUntilMs;
        const QDateTime end = QDateTime::fromMSecsSinceEpoch(until);
        QCOMPARE(end.time(), QTime(6, 0));
        QVERIFY(until - subjects.nowMs() >= 3600000LL);
        QVERIFY(until - subjects.nowMs() <= 25 * 3600000LL);
        fake.endSilent(marco);
        QVERIFY(answered.wait(1000));
        fake.endSilent(marco);
        QVERIFY(answered.wait(1000));
        QCOMPARE(answered.last().at(2).toString(), QStringLiteral("not-silent"));
        // Scenario 1: a low that can be snoozed; scenario 2 with the service stopped: not known.
        subjects.setScenario(1);
        const AlertFact low = fake.facts().alerts.at(0);
        fake.snooze(low.subject, low.slot, 30);
        QVERIFY(answered.wait(1000));
        QCOMPARE(fake.facts().alerts.at(0).state, QStringLiteral("snoozed"));
        subjects.setScenario(2);
        QVERIFY(!fake.facts().known);
    }
};

QTEST_GUILESS_MAIN(TstUiAlerts)
#include "tst_uialerts.moc"
