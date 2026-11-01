// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Michele Brami
//
// The arbiter of the long tone.
// Scripted sequences of effects for several Subjects, read back from a fake backend:
// who holds the `clock` tone, who is kept silent over it, who gives a short sound once the tone has
// ended, what a higher alarm arriving does, and what the acknowledgement, the clearing, the lowering
// and the silencing of the holder do. Every Subject and value is synthetic.
#include "notifiertest.h"

#include <QProcess>
#include <QTemporaryDir>
#include <QtTest>

using namespace lm;
using namespace lm::testing;

class TstNotifierArbiter : public QObject
{
    Q_OBJECT

    QTemporaryDir m_dir;
    NotificationTranslator m_translator;

    typedef NoteRig Rig;

private slots:
    void initTestCase()
    {
        QVERIFY(m_dir.isValid());
        QVERIFY(loadNoteLanguage(&m_dir, &m_translator, "en"));
    }

    void anUrgentAlarmAloneHoldsTheLongTone()
    {
        Rig r;
        r.notify(1, AlertSlot::Low, AlertType::LL);
        QVERIFY(r.holds(1, AlertSlot::Low));
        QCOMPARE(r.lastFeedback(1, AlertSlot::Low), QStringLiteral("clock"));
        r.notify(1, AlertSlot::High, AlertType::HH);                 // the same Subject can not have both, but the slots are independent
        QVERIFY(r.holds(1, AlertSlot::Low));                         // urgent low outranks urgent high
        QCOMPARE(r.lastFeedback(1, AlertSlot::High), QStringLiteral("(none)"));
    }

    // Without an urgent tone every alarm sounds its own, at its first post and at each reminder.
    void withNoUrgentToneAnAlarmsOwnSoundStands()
    {
        Rig r;
        r.notify(1, AlertSlot::Low, AlertType::L);
        r.notify(2, AlertSlot::High, AlertType::H);
        r.notify(1, AlertSlot::Missing, AlertType::Missing);
        QVERIFY(r.noHolder());
        QCOMPARE(r.lastFeedback(1, AlertSlot::Low), QStringLiteral("calendar"));
        QCOMPARE(r.lastFeedback(2, AlertSlot::High), QStringLiteral("calendar"));
        QCOMPARE(r.lastFeedback(1, AlertSlot::Missing), QStringLiteral("general_warning"));
        r.remind(1, AlertSlot::Low, AlertType::L);
        QCOMPARE(r.lastFeedback(1, AlertSlot::Low), QStringLiteral("calendar"));
    }

    // A lower alarm that comes up while the tone rings: banner, entry and wake-up, no sound; and its
    // short sound once, when the tone ends.
    void aLowerAlarmIsSilentOverTheToneAndGivesItsSoundOnceWhenItEnds()
    {
        Rig r;
        r.notify(1, AlertSlot::Low, AlertType::LL);
        r.notify(2, AlertSlot::Low, AlertType::L);
        QCOMPARE(r.lastFeedback(2, AlertSlot::Low), QStringLiteral("(none)"));
        const NoteHints h = r.backend.posts.last().request.hints;
        QVERIFY(h.banner && h.displayOn);                            // the banner and the wake-up are not taken away
        QCOMPARE(h.urgency, 2);
        r.remind(2, AlertSlot::Low, AlertType::L);                   // its own reminder over the tone: still silent
        QCOMPARE(r.lastFeedback(2, AlertSlot::Low), QStringLiteral("(none)"));
        const int before = r.backend.posts.size();
        r.acknowledge(1, AlertSlot::Low, AlertType::LL);             // the tone ends
        QCOMPARE(r.backend.posts.size(), before + 1);                // exactly one post: the short sound
        QCOMPARE(r.backend.last().id, r.presenter.idOf(noteSubject(2), AlertSlot::Low));
        QCOMPARE(r.backend.last().request.hints.feedback, QStringLiteral("calendar"));
        QVERIFY(r.noHolder());
        // Once, not again.
        r.clear(1, AlertSlot::Low, AlertType::LL);
        QCOMPARE(r.backend.posts.size(), before + 1);
    }

    void aDemotedAlarmThatIsAcknowledgedFirstOwesNothing()
    {
        Rig r;
        r.notify(1, AlertSlot::Low, AlertType::LL);
        r.notify(2, AlertSlot::Low, AlertType::L);
        r.acknowledge(2, AlertSlot::Low, AlertType::L);
        const int before = r.backend.posts.size();
        r.acknowledge(1, AlertSlot::Low, AlertType::LL);
        QCOMPARE(r.backend.posts.size(), before);                    // nothing is posted: no sound is owed
    }

    void severalDemotedAlarmsEachGiveTheirShortSoundOnce()
    {
        Rig r;
        r.notify(1, AlertSlot::Low, AlertType::LL);
        r.notify(2, AlertSlot::High, AlertType::H);
        r.notify(2, AlertSlot::Missing, AlertType::Missing);
        r.clear(1, AlertSlot::Low, AlertType::LL);
        QCOMPARE(r.lastFeedback(2, AlertSlot::High), QStringLiteral("calendar"));
        QCOMPARE(r.lastFeedback(2, AlertSlot::Missing), QStringLiteral("general_warning"));    // no data: its own short event
        const int posts = r.backend.posts.size();
        r.remind(2, AlertSlot::High, AlertType::H);                  // with no tone, a reminder sounds again
        QCOMPARE(r.backend.posts.size(), posts + 1);
        QCOMPARE(r.backend.last().request.hints.feedback, QStringLiteral("calendar"));
    }

    // Two alarms of the same rank cannot layer: the earlier onset leads, the other is demoted and
    // takes the tone (not a short sound) when the leader ends.
    void sameRankTheEarlierOnsetLeadsAndTheOtherTakesTheToneNext()
    {
        Rig r;
        r.notify(1, AlertSlot::Low, AlertType::LL, 12);              // onset 03:12
        r.notify(2, AlertSlot::Low, AlertType::LL, 15);              // onset 03:15
        QVERIFY(r.holds(1, AlertSlot::Low));
        QCOMPARE(r.lastFeedback(2, AlertSlot::Low), QStringLiteral("(none)"));
        r.acknowledge(1, AlertSlot::Low, AlertType::LL, 12);
        QVERIFY(r.holds(2, AlertSlot::Low));
        QCOMPARE(r.lastFeedback(2, AlertSlot::Low), QStringLiteral("clock"));      // the hand-over: a fade-in
        QCOMPARE(r.clockPostsOf(2, AlertSlot::Low), 1);
    }

    // After a restart the engine notifies in any order: the earlier onset still ends up holding it.
    void aRestoreInTheWrongOrderEndsWithTheEarlierOnsetHolding()
    {
        Rig r;
        r.notify(2, AlertSlot::Low, AlertType::LL, 15);
        QVERIFY(r.holds(2, AlertSlot::Low));
        r.notify(1, AlertSlot::Low, AlertType::LL, 12);
        QVERIFY(r.holds(1, AlertSlot::Low));
        QCOMPARE(r.lastFeedback(1, AlertSlot::Low), QStringLiteral("clock"));
        QCOMPARE(r.lastFeedback(2, AlertSlot::Low), QStringLiteral("(none)"));     // the first holder was replaced without hints
    }

    // A higher alarm arriving takes the tone: the old holder is replaced in place without hints.
    void aHigherAlarmTakesTheToneByReplacingTheOldHolderWithoutHints()
    {
        Rig r;
        r.notify(1, AlertSlot::High, AlertType::HH);
        const quint32 hh = r.presenter.idOf(noteSubject(1), AlertSlot::High);
        const int before = r.backend.posts.size();
        r.notify(2, AlertSlot::Low, AlertType::LL);                  // urgent low outranks urgent high
        QVERIFY(r.holds(2, AlertSlot::Low));
        QCOMPARE(r.backend.posts.size(), before + 2);
        QCOMPARE(r.backend.posts.at(before).id, hh);                 // first: the old holder, in place, no hints
        QVERIFY(r.backend.posts.at(before).request.hints.feedback.isEmpty());
        QCOMPARE(r.backend.posts.at(before).replaces, hh);
        QCOMPARE(r.backend.posts.at(before + 1).request.hints.feedback, QStringLiteral("clock"));
        // The low alarm ends: the high one is re-posted with the alarm hints, not given a short sound.
        r.clear(2, AlertSlot::Low, AlertType::LL);
        QVERIFY(r.holds(1, AlertSlot::High));
        QCOMPARE(r.backend.last().request.hints.feedback, QStringLiteral("clock"));
        QCOMPARE(r.backend.last().id, hh);
    }

    void aLoweredHolderStopsTheToneAndTheNextUrgentAlarmTakesIt()
    {
        Rig r;
        r.notify(1, AlertSlot::Low, AlertType::LL);
        r.notify(2, AlertSlot::High, AlertType::HH);
        QVERIFY(r.holds(1, AlertSlot::Low));
        r.send(AlertEffect::Update, 1, AlertSlot::Low, AlertType::L);        // the level is lowered
        QVERIFY(r.holds(2, AlertSlot::High));
        QCOMPARE(r.lastFeedback(2, AlertSlot::High), QStringLiteral("clock"));
        // The lowered alarm was replaced quietly and is not owed a sound while the new tone rings.
        QCOMPARE(r.lastFeedback(1, AlertSlot::Low), QStringLiteral("(none)"));
        r.acknowledge(2, AlertSlot::High, AlertType::HH);                     // the tone ends: the low one gives its sound once
        QCOMPARE(r.backend.last().id, r.presenter.idOf(noteSubject(1), AlertSlot::Low));
        QCOMPARE(r.backend.last().request.hints.feedback, QStringLiteral("calendar"));
    }

    void aSilentSubjectNeverLeadsAndALeaderThatBecomesSilentLosesTheTone()
    {
        Rig r;
        r.notify(1, AlertSlot::Low, AlertType::LL, 0, true);         // silent from the start
        QVERIFY(r.noHolder());
        QCOMPARE(r.lastFeedback(1, AlertSlot::Low), QStringLiteral("(none)"));
        r.notify(2, AlertSlot::Low, AlertType::LL, 5);
        QVERIFY(r.holds(2, AlertSlot::Low));                          // the silent one, though earlier, does not lead
        QCOMPARE(r.lastFeedback(2, AlertSlot::Low), QStringLiteral("clock"));
        // Subject 2 is set to silent while its alarm sounds: the Update replaces it without hints.
        r.send(AlertEffect::Update, 2, AlertSlot::Low, AlertType::LL, AlertState::Active, 5, true);
        QVERIFY(r.noHolder());
        QCOMPARE(r.lastFeedback(2, AlertSlot::Low), QStringLiteral("(none)"));
        // A silent alarm owes no short sound either.
        const int posts = r.backend.posts.size();
        r.remind(1, AlertSlot::Low, AlertType::LL);
        r.send(AlertEffect::Remind, 1, AlertSlot::Low, AlertType::LL, AlertState::Active, 0, true);
        QCOMPARE(r.lastFeedback(1, AlertSlot::Low), QStringLiteral("(none)"));
        Q_UNUSED(posts);
    }

    // Banners, entries and texts are never suppressed by the rank (last bullet).
    void theRankOrdersSoundOnly()
    {
        Rig r;
        r.notify(1, AlertSlot::Low, AlertType::LL);
        r.notify(2, AlertSlot::Low, AlertType::L);
        r.notify(2, AlertSlot::Missing, AlertType::Missing);
        for (int i = 0; i < r.backend.posts.size(); ++i) {
            const FakeNoteBackend::Post &p = r.backend.posts.at(i);
            QVERIFY(!p.request.summary.isEmpty() && !p.request.body.isEmpty());
            QVERIFY(p.request.hints.banner && p.request.hints.displayOn && p.request.hints.urgency == 2);
        }
        QVERIFY(r.backend.live.size() == 3);                          // three alarms, three entries
    }

    // The user deletes the holder's entry: that is the acknowledgement, and the tone passes on.
    void aDeletionOfTheHolderPassesTheToneOn()
    {
        Rig r;
        r.presenter.setAcknowledgeHandler([&r](const QUuid &s, AlertSlot slot) {
            r.send(AlertEffect::Update, s == noteSubject(1) ? 1 : 2, slot, AlertType::LL, AlertState::Acknowledged);
        });
        r.notify(1, AlertSlot::Low, AlertType::LL, 1);
        r.notify(2, AlertSlot::Low, AlertType::LL, 4);
        r.backend.userDeletes(r.presenter.idOf(noteSubject(1), AlertSlot::Low));
        QVERIFY(r.holds(2, AlertSlot::Low));
        QCOMPARE(r.backend.last().request.hints.feedback, QStringLiteral("clock"));
    }
};

QTEST_MAIN(TstNotifierArbiter)
#include "tst_notifierarbiter.moc"
