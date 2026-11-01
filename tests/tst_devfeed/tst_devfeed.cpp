// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Michele Brami
//
// The developer-only feed of synthetic effects (daemon/devfeed.h).
// Built only with the development flag, which the host build implies. What is proved: every ready-made
// scenario of the phone protocol works and shows what the protocol says it shows, the synthetic
// Subjects are the only ones the feed will touch, a refusal changes nothing, a deletion of a synthetic
// notification acknowledges the synthetic slot, and `clear` leaves a clean tray. Synthetic data only.
#include "notifiertest.h"
#include "daemon/devfeed.h"
#include "daemon/devlog.h"

#include <QJsonDocument>
#include <QJsonObject>
#include <QtTest>

using namespace lm;
using namespace lm::testing;

#ifdef LIBREMON_DEVSOURCE

namespace {

QJsonObject json(const QString &text) { return QJsonDocument::fromJson(text.toUtf8()).object(); }

} // namespace

class TstDevFeed : public QObject
{
    Q_OBJECT

    QTemporaryDir m_dir;
    NotificationTranslator m_translator;

    // The feed needs the presenter first, so the world is built in this order.
    struct World
    {
        World() : presenter(&backend, &settings, &labels, &clock),
                  feed(&presenter, &labels, &clock, 0, [this](bool on) { dnd = on; }, &events), dnd(false)
        {
            presenter.setTexts(NotificationTexts(noteZoneTime));
            presenter.setFactsReader(&feed);
            presenter.setConnectionInfo([](const QString &) {
                return QStringList() << DevFeed::subjectId(1).toString() << DevFeed::subjectId(2).toString();
            }, [](const QString &c) { return c; });
            presenter.setAcknowledgeHandler([this](const QUuid &s, AlertSlot slot) { feed.acknowledge(s, slot); });
        }
        // Records what the feed tells the service's event sinks.
        struct Events : ServiceEventSink
        {
            void alertChanged(const QUuid &, const QString &slot, const QString &state) { seen << slot + QLatin1Char(':') + state; }
            QStringList seen;
        } events;
        QString run(const char *scenario) { return feed.apply(QStringLiteral("{\"scenario\":\"%1\"}").arg(QString::fromLatin1(scenario))); }
        FakeNoteBackend backend;
        MemoryNoteSettings settings;
        SubjectLabelBook labels;
        NoteClock clock;
        AlertNotificationPresenter presenter;
        DevFeed feed;
        bool dnd;
    };

private slots:
    void initTestCase()
    {
        QVERIFY(m_dir.isValid());
        QVERIFY(loadNoteLanguage(&m_dir, &m_translator, "en"));
    }

    void everyScenarioOfTheProtocolRuns()
    {
        const QStringList names = DevFeed::scenarioNames();
        QVERIFY(names.size() >= 20);
        for (int i = 0; i < names.size(); ++i) {
            World w;
            w.run("urgent-low");                                     // something to remind, acknowledge or lower
            const QJsonObject r = json(w.run(names.at(i).toLatin1().constData()));
            QVERIFY2(r.value("ok").toBool(), qPrintable(names.at(i) + QStringLiteral(": ") + r.value("error").toString()));
        }
    }

    void theFiveKindsShowTheirOwnWordsLabelAndSince()
    {
        struct Row { const char *scenario; const char *summary; const char *line2; };
        const Row rows[] = {
            { "urgent-low", "VERY LOW GLUCOSE", "Below 54 mg/dL since 05:12 (8 min)." },
            { "low", "LOW GLUCOSE", "Below 70 mg/dL since 05:12 (8 min)." },
            { "high", "HIGH GLUCOSE", "Above 180 mg/dL since 05:00 (20 min)." },
            { "urgent-high", "VERY HIGH GLUCOSE", "Above 250 mg/dL since 05:12 (8 min)." },
            { "no-data", "NO GLUCOSE DATA", "No reading since 05:02 (18 min): The source cannot be reached." },
        };
        for (size_t i = 0; i < sizeof(rows) / sizeof(rows[0]); ++i) {
            World w;
            QVERIFY(json(w.run(rows[i].scenario)).value("ok").toBool());
            const NotificationRequest q = w.backend.last().request;
            QCOMPARE(q.summary, QString::fromLatin1(rows[i].summary));
            QCOMPARE(q.body.split(QLatin1Char('\n')).at(0), QStringLiteral("AAA"));         // the synthetic label, never a real one
            QCOMPARE(q.body.split(QLatin1Char('\n')).at(1), QString::fromLatin1(rows[i].line2));
        }
    }

    void twoSubjectsAtOnceFollowTheArbiter()
    {
        World w;
        QVERIFY(json(w.run("two-subjects-urgent-low-and-low")).value("ok").toBool());
        QCOMPARE(w.backend.live.size(), 2);
        QVERIFY(w.presenter.hasAlarm(DevFeed::subjectId(1), AlertSlot::Low));
        QUuid s; AlertSlot slot;
        QVERIFY(w.presenter.toneHolder(&s, &slot) && s == DevFeed::subjectId(1));
        QVERIFY(w.backend.last().request.hints.feedback.isEmpty());          // the low one is kept silent over the tone
        QVERIFY(w.backend.last().request.body.startsWith(QStringLiteral("BBB")));
        QVERIFY(json(w.run("two-urgent-lows")).value("ok").toBool());
        QVERIFY(w.presenter.toneHolder(&s, &slot) && s == DevFeed::subjectId(1));       // since 12 min, the earlier onset
        QVERIFY(json(w.run("urgent-high-then-urgent-low")).value("ok").toBool());
        QVERIFY(w.presenter.toneHolder(&s, &slot) && s == DevFeed::subjectId(2));       // urgent low outranks urgent high
    }

    void aReminderSoundsAgainWithTheCurrentMinutes()
    {
        World w;
        w.run("urgent-low");
        w.clock.advanceS(120);
        QVERIFY(json(w.run("reminder")).value("ok").toBool());
        QCOMPARE(w.backend.last().request.hints.feedback, QStringLiteral("clock"));
        QVERIFY(w.backend.last().request.body.contains(QStringLiteral("(10 min)")));
    }

    void silentStopsTheToneAndTheNextReminderSoundsWhenItIsOff()
    {
        World w;
        w.run("urgent-low");
        QVERIFY(json(w.run("silent")).value("ok").toBool());
        QVERIFY(w.backend.last().request.hints.feedback.isEmpty());
        QVERIFY(w.feed.silentLabels() == QStringList() << QStringLiteral("AAA"));
        QVERIFY(json(w.run("unsilent")).value("ok").toBool());
        QCOMPARE(w.backend.last().request.hints.feedback, QStringLiteral("clock"));
        QVERIFY(w.feed.silentLabels().isEmpty());
    }

    void theFeedTellsTheServiceWhatChangedLikeTheRelayDoes()
    {
        World w;
        w.run("urgent-low");
        w.run("acknowledge");
        w.run("clear");
        QVERIFY(w.events.seen.contains(QStringLiteral("low:active")));
        QVERIFY(w.events.seen.contains(QStringLiteral("low:acknowledged")));
        QVERIFY(w.events.seen.contains(QStringLiteral("low:snoozed")));          // `clear` takes alerts down as snoozed ones
    }

    void acknowledgeClosesTheEntryAndADeletionDoesTheSame()
    {
        World w;
        w.run("urgent-low");
        const quint32 id = w.backend.last().id;
        QVERIFY(json(w.run("acknowledge")).value("ok").toBool());
        QVERIFY(w.backend.closes.contains(id));
        QVERIFY(w.feed.residentAlerts().first().acknowledged);
        // The user deletes a synthetic notification: the synthetic slot is acknowledged by the feed.
        World v;
        v.run("urgent-low");
        v.backend.userDeletes(v.backend.last().id);
        QVERIFY(v.feed.residentAlerts().first().acknowledged);
        QVERIFY(v.backend.live.isEmpty());
    }

    void anAlertThatEndsUnacknowledgedLeavesTheEndedEntry()
    {
        World w;
        QVERIFY(json(w.run("ended")).value("ok").toBool());
        QCOMPARE(w.backend.live.size(), 1);
        QCOMPARE(w.backend.last().request.summary, QStringLiteral("LOW GLUCOSE ENDED"));
        QVERIFY(w.backend.last().request.body.contains(QStringLiteral("after 28 min")));
        w.run("clear");
        QVERIFY(w.backend.live.isEmpty());
    }

    void theNoticesAndDoNotDisturb()
    {
        World w;
        w.run("approval");
        QCOMPARE(w.backend.last().request.summary, QStringLiteral("APPROVE NEW PATIENT"));
        QVERIFY(w.backend.last().request.body.startsWith(QStringLiteral("AAA, BBB")));
        w.run("guard");
        QCOMPARE(w.backend.last().request.summary, QStringLiteral("GLUCOSE SOURCE DISABLED"));
        w.run("report");
        QCOMPARE(w.backend.last().request.summary, QStringLiteral("REVIEW DIAGNOSTIC REPORT"));
        w.run("interruption");
        QCOMPARE(w.backend.last().request.summary, QStringLiteral("GLUCOSE MONITOR WAS DOWN"));
        w.run("dnd-on");
        QVERIFY(w.dnd && w.feed.doNotDisturb());
        QCOMPARE(w.backend.last().request.summary, QStringLiteral("GLUCOSE ALERTS SILENT"));
        QCOMPARE(w.backend.live.size(), 5);
        w.run("dnd-off");
        w.run("approval-done");
        w.run("guard-done");
        w.run("report-done");
        QVERIFY(!w.dnd);
        QCOMPARE(w.backend.live.size(), 1);                                  // the interruption notice stays until cleared
        w.run("clear");
        QVERIFY(w.backend.live.isEmpty());
    }

    // The development log holds numbers and hint words, never a summary, a body or a label.
    void theDevelopmentLogHoldsNoTextAndNoLabel()
    {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        const QString path = dir.path() + QStringLiteral("/notify.log");
        FakeNoteBackend inner;
        LoggingNoteBackend logged(&inner, path);
        MemoryNoteSettings settings;
        SubjectLabelBook labels;
        NoteClock clock;
        AlertNotificationPresenter presenter(&logged, &settings, &labels, &clock);
        presenter.setTexts(NotificationTexts(noteZoneTime));
        DevFeed feed(&presenter, &labels, &clock, 0, std::function<void(bool)>());
        presenter.setFactsReader(&feed);
        feed.apply(QStringLiteral("{\"scenario\":\"urgent-low\"}"));
        inner.userDeletes(inner.last().id);
        QFile f(path);
        QVERIFY(f.open(QIODevice::ReadOnly));
        const QString text = QString::fromUtf8(f.readAll());
        QVERIFY(text.contains(QStringLiteral("post id=")) && text.contains(QStringLiteral("feedback=clock")));
        QVERIFY(text.contains(QStringLiteral("closed id=")) && text.contains(QStringLiteral("reason=2")));
        QVERIFY(!text.contains(QStringLiteral("GLUCOSE")) && !text.contains(QStringLiteral("AAA")) && !text.contains(QStringLiteral("mg/dL")));
    }

    // Only the four made-up Subjects: a real Subject's UUID, a number outside 1 to 4 and a long label are refused.
    void anythingThatIsNotSyntheticIsRefusedAndChangesNothing()
    {
        World w;
        const QString real = QStringLiteral("{d2f1c9a0-5b3e-4c7d-8a91-0e2b4c6d8f10}");
        QJsonObject r = json(w.feed.apply(QStringLiteral("{\"ops\":[{\"op\":\"alert\",\"kind\":\"notify\",\"subject\":\"%1\",\"slot\":\"low\",\"type\":\"LL\"}]}").arg(real)));
        QVERIFY(!r.value("ok").toBool());
        QCOMPARE(r.value("error").toString(), QStringLiteral("not-a-synthetic-subject"));
        r = json(w.feed.apply(QStringLiteral("{\"ops\":[{\"op\":\"alert\",\"kind\":\"notify\",\"subject\":5,\"slot\":\"low\",\"type\":\"LL\"}]}")));
        QCOMPARE(r.value("error").toString(), QStringLiteral("not-a-synthetic-subject"));
        QVERIFY(w.backend.posts.isEmpty());
        QCOMPARE(json(w.feed.apply(QStringLiteral("{\"ops\":[{\"op\":\"label\",\"subject\":1,\"label\":\"TOOLONG\"}]}"))).value("error").toString(), QStringLiteral("bad-label"));
        QCOMPARE(json(w.feed.apply(QStringLiteral("not json"))).value("error").toString(), QStringLiteral("not-json"));
        QCOMPARE(json(w.feed.apply(QStringLiteral("{\"scenario\":\"nope\"}"))).value("error").toString(), QStringLiteral("unknown-scenario"));
        QCOMPARE(json(w.feed.apply(QStringLiteral("{}"))).value("error").toString(), QStringLiteral("nothing-to-do"));
        QCOMPARE(json(w.feed.apply(QStringLiteral("{\"ops\":[{\"op\":\"nope\"}]}"))).value("error").toString(), QStringLiteral("unknown-op"));
        QCOMPARE(json(w.feed.apply(QStringLiteral("{\"ops\":[{\"op\":\"alert\",\"kind\":\"notify\",\"subject\":1,\"slot\":\"low\",\"type\":\"H\"}]}"))).value("error").toString(),
                 QStringLiteral("type-does-not-fit-the-slot"));
        QVERIFY(w.backend.posts.isEmpty());
        // A label is changed for the synthetic Subject and appears in the next text.
        QVERIFY(json(w.feed.apply(QStringLiteral("{\"ops\":[{\"op\":\"label\",\"subject\":1,\"label\":\"X1\"},{\"op\":\"alert\",\"kind\":\"notify\",\"subject\":1,\"slot\":\"low\",\"type\":\"L\"}]}"))).value("ok").toBool());
        QVERIFY(w.backend.last().request.body.startsWith(QStringLiteral("X1")));
        // The ids are fixed and distinct, and none is the real Subject's.
        QSet<QUuid> ids;
        for (int i = 1; i <= 4; ++i) { ids.insert(DevFeed::subjectId(i)); QVERIFY(DevFeed::isSynthetic(DevFeed::subjectId(i))); }
        QCOMPARE(ids.size(), 4);
        QVERIFY(!DevFeed::isSynthetic(QUuid(real)));
        QVERIFY(DevFeed::subjectId(0).isNull() && DevFeed::subjectId(5).isNull());
    }
};

QTEST_MAIN(TstDevFeed)
#include "tst_devfeed.moc"

#else

// Without the development flag the feed does not exist; the test is then empty on purpose.
class TstDevFeed : public QObject { Q_OBJECT private slots: void absent() {} };
QTEST_MAIN(TstDevFeed)
#include "tst_devfeed.moc"

#endif
