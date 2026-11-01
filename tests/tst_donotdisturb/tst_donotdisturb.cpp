// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Michele Brami
//
// The Do not disturb watcher. A fake source stands for the
// dconf value. What is proved: the notice "alerts will be silent" is posted once when Do not disturb
// turns on (urgency 1, priority 50, no feedback, a banner and a page entry), withdrawn when it turns
// off, never repeated while it stays on, reconciled at start whatever the last run left, and the
// callback for the resident notice is called with each change. Synthetic data.
#include "notifiertest.h"
#include "service/donotdisturb.h"

#include <QtTest>

using namespace lm;
using namespace lm::testing;

namespace {

class FakeDnd : public DoNotDisturbSource
{
public:
    FakeDnd() : on(false) {}
    bool isOn() const { return on; }
    void setChangeHandler(const ChangeHandler &h) { handler = h; }
    void turn(bool value) { on = value; if (handler) handler(value); }
    bool on;
    ChangeHandler handler;
};

} // namespace

class TstDoNotDisturb : public QObject
{
    Q_OBJECT

    QTemporaryDir m_dir;
    NotificationTranslator m_translator;

private slots:
    void initTestCase()
    {
        QVERIFY(m_dir.isValid());
        QVERIFY(loadNoteLanguage(&m_dir, &m_translator, "en"));
    }

    void theNoticeIsPostedWhenItTurnsOnAndWithdrawnWhenItEnds()
    {
        NoteRig r;
        FakeDnd dnd;
        QList<bool> seen;
        DoNotDisturbWatcher w(&dnd, &r.presenter, [&seen](bool on) { seen << on; });
        w.start();
        QVERIFY(r.backend.posts.isEmpty());                           // off at start: nothing to say
        QCOMPARE(seen, QList<bool>() << false);                       // the resident notice is told the state
        dnd.turn(true);
        QCOMPARE(r.backend.posts.size(), 1);
        const FakeNoteBackend::Post p = r.backend.last();
        QCOMPARE(p.request.summary, QStringLiteral("GLUCOSE ALERTS SILENT"));
        QCOMPARE(p.request.hints.urgency, 1);
        QCOMPARE(p.request.hints.priority, 50);
        QVERIFY(p.request.hints.feedback.isEmpty() && p.request.hints.banner);
        QVERIFY(w.isOn());
        dnd.turn(false);
        QCOMPARE(r.backend.closes, QList<quint32>() << p.id);
        QVERIFY(!w.isOn());
        QCOMPARE(seen, QList<bool>() << false << true << false);
    }

    // One notification per change, no nagging: a repeated report of the same state does nothing.
    void aRepeatedReportOfTheSameStateDoesNothing()
    {
        NoteRig r;
        FakeDnd dnd;
        int calls = 0;
        DoNotDisturbWatcher w(&dnd, &r.presenter, [&calls](bool) { ++calls; });
        w.start();
        dnd.turn(true);
        dnd.turn(true);
        dnd.turn(true);
        QCOMPARE(r.backend.posts.size(), 1);
        QCOMPARE(calls, 2);                                           // start (off) and the one change
        dnd.turn(false);
        dnd.turn(false);
        QCOMPARE(r.backend.closes.size(), 1);
    }

    // A start is not a change: the notice the last run left is adopted by its
    // stored id and nothing is posted again, so a restart never raises a second banner. It is posted only
    // on a real change: on while the service was down, or the entry is absent; and withdrawn when it went off.
    void aRestartWithDoNotDisturbStillOnAdoptsTheEntryAndPostsNothing()
    {
        MemoryNoteSettings settings;
        FakeNoteBackend backend;
        SubjectLabelBook labels;
        NoteClock clock;
        quint32 id;
        {
            AlertNotificationPresenter first(&backend, &settings, &labels, &clock);
            first.setTexts(NotificationTexts(noteZoneTime));
            FakeDnd dnd;
            dnd.on = true;
            DoNotDisturbWatcher w(&dnd, &first, std::function<void(bool)>());
            w.start();
            QCOMPARE(backend.posts.size(), 1);
            id = backend.last().id;
        }
        AlertNotificationPresenter second(&backend, &settings, &labels, &clock);
        second.setTexts(NotificationTexts(noteZoneTime));
        FakeDnd still;
        still.on = true;
        DoNotDisturbWatcher w2(&still, &second, std::function<void(bool)>());
        w2.start();
        second.closeOrphans();                                        // the notice is not an orphan: the watcher owns it
        QCOMPARE(backend.posts.size(), 1);                            // nothing was posted again
        QCOMPARE(backend.live.size(), 1);
        QVERIFY(backend.live.contains(id));
        // The adopted entry is the one the next change takes down.
        still.turn(false);
        QVERIFY(backend.live.isEmpty());
        QCOMPARE(backend.closes.last(), id);
    }

    void aRestartWithDoNotDisturbStillOffDoesNothing()
    {
        MemoryNoteSettings settings;
        FakeNoteBackend backend;
        SubjectLabelBook labels;
        NoteClock clock;
        AlertNotificationPresenter p(&backend, &settings, &labels, &clock);
        FakeDnd off;
        DoNotDisturbWatcher w(&off, &p, std::function<void(bool)>());
        w.start();
        QVERIFY(backend.posts.isEmpty());
        QVERIFY(backend.closes.isEmpty());
    }

    void doNotDisturbChangedWhileTheServiceWasDownIsMatchedAtStart()
    {
        MemoryNoteSettings settings;
        FakeNoteBackend backend;
        SubjectLabelBook labels;
        NoteClock clock;
        quint32 id;
        {   // on when the last run ended
            AlertNotificationPresenter first(&backend, &settings, &labels, &clock);
            first.setTexts(NotificationTexts(noteZoneTime));
            FakeDnd dnd;
            dnd.on = true;
            DoNotDisturbWatcher w(&dnd, &first, std::function<void(bool)>());
            w.start();
            id = backend.last().id;
        }
        {   // off now: the stale entry is taken down, nothing is posted
            AlertNotificationPresenter second(&backend, &settings, &labels, &clock);
            FakeDnd off;
            DoNotDisturbWatcher w(&off, &second, std::function<void(bool)>());
            w.start();
            QVERIFY(backend.live.isEmpty());
            QCOMPARE(backend.closes.last(), id);
            QCOMPARE(backend.posts.size(), 1);
        }
        {   // on now, and the last run left no entry: one is posted
            AlertNotificationPresenter third(&backend, &settings, &labels, &clock);
            third.setTexts(NotificationTexts(noteZoneTime));
            FakeDnd on;
            on.on = true;
            DoNotDisturbWatcher w(&on, &third, std::function<void(bool)>());
            w.start();
            QCOMPARE(backend.posts.size(), 2);
            QCOMPARE(backend.live.size(), 1);
        }
    }

    // An entry the user deleted while the service ran is forgotten, so the next start posts it again if
    // Do not disturb is still on (the entry is absent).
    void anEntryTheUserDeletedIsPostedAgainAtTheNextStart()
    {
        MemoryNoteSettings settings;
        FakeNoteBackend backend;
        SubjectLabelBook labels;
        NoteClock clock;
        {
            AlertNotificationPresenter first(&backend, &settings, &labels, &clock);
            first.setTexts(NotificationTexts(noteZoneTime));
            FakeDnd dnd;
            dnd.on = true;
            DoNotDisturbWatcher w(&dnd, &first, std::function<void(bool)>());
            w.start();
            backend.userDeletes(backend.last().id);
        }
        AlertNotificationPresenter second(&backend, &settings, &labels, &clock);
        second.setTexts(NotificationTexts(noteZoneTime));
        FakeDnd still;
        still.on = true;
        DoNotDisturbWatcher w2(&still, &second, std::function<void(bool)>());
        w2.start();
        QCOMPARE(backend.posts.size(), 2);
        QCOMPARE(backend.live.size(), 1);
    }

    void withoutASourceNothingHappensAndAStoppedWatcherIsDeaf()
    {
        NoteRig r;
        DoNotDisturbWatcher none(0, &r.presenter, std::function<void(bool)>());
        none.start();
        QVERIFY(r.backend.posts.isEmpty());
        FakeDnd dnd;
        DoNotDisturbWatcher w(&dnd, &r.presenter, std::function<void(bool)>());
        w.start();
        w.stop();
        dnd.turn(true);
        QVERIFY(r.backend.posts.isEmpty());
    }

    // The notice is a different notification from every alert: an alarm and the notice live side by side.
    void theNoticeDoesNotDisturbTheAlarms()
    {
        NoteRig r;
        FakeDnd dnd;
        DoNotDisturbWatcher w(&dnd, &r.presenter, std::function<void(bool)>());
        w.start();
        r.notify(1, AlertSlot::Low, AlertType::LL);
        dnd.turn(true);
        QCOMPARE(r.backend.live.size(), 2);
        QVERIFY(r.holds(1, AlertSlot::Low));                          // the alarm is still posted with its hints
        r.remind(1, AlertSlot::Low, AlertType::LL);
        QCOMPARE(r.lastFeedback(1, AlertSlot::Low), QStringLiteral("clock"));      // the phone silences it, not the service
    }
};

QTEST_MAIN(TstDoNotDisturb)
#include "tst_donotdisturb.moc"
