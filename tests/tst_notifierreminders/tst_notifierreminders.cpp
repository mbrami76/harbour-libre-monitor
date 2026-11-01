// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Michele Brami
//
// Reminders. A Remind effect of the engine re-posts
// the same notification, by the same id or by the one the platform gave back, with the hints its
// role allows at that moment; each reminder of an urgent alarm restarts the fade-in and wakes the
// screen; and the service never re-posts anything on its own to keep a tone alive. Synthetic data.
#include "notifiertest.h"

#include <QtTest>

using namespace lm;
using namespace lm::testing;

class TstNotifierReminders : public QObject
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

    // Every reminder of the holder is the same entry again with the alarm hints: the tone restarts
    // its fade-in and the screen wakes (one stream, accepted).
    void eachReminderOfAnUrgentAlarmRepostsTheSameEntryWithTheAlarmHints()
    {
        NoteRig r;
        r.notify(1, AlertSlot::Low, AlertType::LL);
        const quint32 id = r.presenter.idOf(noteSubject(1), AlertSlot::Low);
        for (int i = 1; i <= 3; ++i) {
            r.remind(1, AlertSlot::Low, AlertType::LL);
            const FakeNoteBackend::Post &p = r.backend.last();
            QCOMPARE(p.replaces, id);
            QCOMPARE(p.id, id);
            QCOMPARE(p.request.hints.feedback, QStringLiteral("clock"));
            QVERIFY(p.request.hints.displayOn && p.request.hints.urgency == 2 && p.request.hints.banner);
        }
        QCOMPARE(r.clockPostsOf(1, AlertSlot::Low), 4);              // the raise and three reminders
        QCOMPARE(r.backend.live.size(), 1);                          // one entry throughout
    }

    // The minutes in the text are current at each reminder: the notification is re-published.
    void aReminderCarriesTheCurrentMinutes()
    {
        NoteRig r;
        r.notify(1, AlertSlot::Low, AlertType::L, 12);               // onset 03:12, now 05:20 in the rig's clock
        QVERIFY(r.backend.last().request.body.contains(QStringLiteral("since 03:12 (128 min)")));
        r.clock.advanceS(15 * 60);
        r.remind(1, AlertSlot::Low, AlertType::L, 12);
        QVERIFY(r.backend.last().request.body.contains(QStringLiteral("since 03:12 (143 min)")));
        // No data counts from the last reading.
        r.notify(1, AlertSlot::Missing, AlertType::Missing);
        QVERIFY(r.backend.last().request.body.contains(QStringLiteral("No reading since 03:00 (")));
        r.clock.advanceS(30 * 60);
        r.remind(1, AlertSlot::Missing, AlertType::Missing);
        QVERIFY(r.backend.last().request.body.contains(QStringLiteral("(")));
    }

    void aReminderAfterTheUserDeletedTheEntryUsesTheIdItIsGivenBack()
    {
        NoteRig r;
        r.notify(1, AlertSlot::Low, AlertType::L);
        const quint32 first = r.presenter.idOf(noteSubject(1), AlertSlot::Low);
        r.backend.deleted.insert(first);                              // gone; no signal reached the service
        r.remind(1, AlertSlot::Low, AlertType::L);
        const quint32 second = r.presenter.idOf(noteSubject(1), AlertSlot::Low);
        QVERIFY(second != first && second != 0);
        r.remind(1, AlertSlot::Low, AlertType::L);
        QCOMPARE(r.backend.last().replaces, second);
        QCOMPARE(r.backend.last().id, second);
    }

    // Low, high and no data: their own short sound at each reminder while no urgent tone rings.
    void aNonUrgentReminderSoundsItsOwnEventWhenNoToneRings()
    {
        NoteRig r;
        r.notify(1, AlertSlot::High, AlertType::H);
        r.remind(1, AlertSlot::High, AlertType::H);
        r.remind(1, AlertSlot::High, AlertType::H);
        QCOMPARE(r.postsOf(1, AlertSlot::High), 3);
        QCOMPARE(r.lastFeedback(1, AlertSlot::High), QStringLiteral("calendar"));
        r.notify(2, AlertSlot::Missing, AlertType::Missing);
        r.remind(2, AlertSlot::Missing, AlertType::Missing);
        QCOMPARE(r.lastFeedback(2, AlertSlot::Missing), QStringLiteral("general_warning"));
    }

    // Over a ringing tone the reminders of the others are silent, and it is the same entry each time.
    void remindersOfDemotedAlarmsAreSilentWhileTheToneRings()
    {
        NoteRig r;
        r.notify(1, AlertSlot::Low, AlertType::LL);
        r.notify(2, AlertSlot::High, AlertType::H);
        const quint32 hid = r.presenter.idOf(noteSubject(2), AlertSlot::High);
        r.remind(2, AlertSlot::High, AlertType::H);
        QCOMPARE(r.backend.last().id, hid);
        QVERIFY(r.backend.last().request.hints.feedback.isEmpty());
        r.remind(1, AlertSlot::Low, AlertType::LL);                    // the holder's own reminder still rings
        QCOMPARE(r.backend.last().request.hints.feedback, QStringLiteral("clock"));
        QCOMPARE(r.backend.live.size(), 2);
    }

    // A silent Subject's reminders post the banner and no feedback at every level.
    void remindersOfASilentSubjectCarryNoFeedback()
    {
        NoteRig r;
        r.notify(1, AlertSlot::Low, AlertType::LL, 0, true);
        r.remind(1, AlertSlot::Low, AlertType::LL, 0, true);
        QVERIFY(r.backend.last().request.hints.feedback.isEmpty());
        QVERIFY(r.backend.last().request.hints.banner);
        r.notify(1, AlertSlot::High, AlertType::H, 0, true);
        r.remind(1, AlertSlot::High, AlertType::H, 0, true);
        QVERIFY(r.backend.last().request.hints.feedback.isEmpty());
    }

    // Nothing is re-published to keep a tone alive: only effects of the engine make a post.
    void nothingIsRepostedOnItsOwn()
    {
        NoteRig r;
        r.notify(1, AlertSlot::Low, AlertType::LL);
        const int posts = r.backend.posts.size();
        r.clock.advanceS(3600);
        QCOMPARE(r.backend.posts.size(), posts);
        r.presenter.closeOrphans();                                    // housekeeping posts nothing either
        QCOMPARE(r.backend.posts.size(), posts);
    }
};

QTEST_MAIN(TstNotifierReminders)
#include "tst_notifierreminders.moc"
