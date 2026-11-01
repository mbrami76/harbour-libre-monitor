// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Michele Brami
//
// Silent mode in the notifier. A silent
// Subject's notifications carry no feedback hint at any level, the Update that the alert engine emits when a
// mode starts replaces a sounding notification without a hint and so stops the tone, the reminders
// that follow are silent, a level that rises stays silent, and the notices about the monitor itself
// (no data) are never silenced by a Subject's mode. Synthetic data.
#include "notifiertest.h"

#include <QtTest>

using namespace lm;
using namespace lm::testing;

class TstNotifierSilent : public QObject
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

    // Silent from the start: banner, wake-up and entry stay, the feedback goes, at every level.
    void everyLevelOfASilentSubjectIsPostedWithoutFeedback()
    {
        const AlertType types[] = { AlertType::LL, AlertType::L, AlertType::H, AlertType::HH };
        const AlertSlot slotList[] = { AlertSlot::Low, AlertSlot::Low, AlertSlot::High, AlertSlot::High };
        for (int i = 0; i < 4; ++i) {
            NoteRig r;
            r.notify(1, slotList[i], types[i], 0, true);
            const NoteHints h = r.backend.last().request.hints;
            QVERIFY2(h.feedback.isEmpty(), "feedback on a silent alert");
            QVERIFY(h.banner && h.displayOn && h.urgency == 2);
            r.remind(1, slotList[i], types[i], 0, true);
            QVERIFY(r.backend.last().request.hints.feedback.isEmpty());
            QVERIFY(r.noHolder());                                   // a silent alarm never holds the tone
        }
    }

    // The Update a mode start emits: the sounding tone stops (a replacement without hints).
    void theUpdateThatStartsAModeStopsASoundingTone()
    {
        NoteRig r;
        r.notify(1, AlertSlot::Low, AlertType::LL);
        QCOMPARE(r.lastFeedback(1, AlertSlot::Low), QStringLiteral("clock"));
        const quint32 id = r.presenter.idOf(noteSubject(1), AlertSlot::Low);
        const int posts = r.backend.posts.size();
        r.send(AlertEffect::Update, 1, AlertSlot::Low, AlertType::LL, AlertState::Active, 0, true);
        QCOMPARE(r.backend.posts.size(), posts + 1);
        QCOMPARE(r.backend.last().id, id);
        QCOMPARE(r.backend.last().replaces, id);
        QVERIFY(r.backend.last().request.hints.feedback.isEmpty());
        QVERIFY(r.noHolder());
        // The reminders that follow are silent.
        r.remind(1, AlertSlot::Low, AlertType::LL, 0, true);
        QVERIFY(r.backend.last().request.hints.feedback.isEmpty());
    }

    // A level that rises under a silent mode stays silent (L to LL).
    void aLevelThatRisesUnderASilentModeStaysSilent()
    {
        NoteRig r;
        r.notify(1, AlertSlot::Low, AlertType::L, 0, true);
        r.notify(1, AlertSlot::Low, AlertType::LL, 0, true);
        QVERIFY(r.backend.last().request.hints.feedback.isEmpty());
        QCOMPARE(r.backend.last().request.summary, QStringLiteral("VERY LOW GLUCOSE"));
        QVERIFY(r.noHolder());
    }

    // Nothing is emitted when a mode ends: the next reminder sounds, and it can hold the tone again.
    void whenTheModeEndsTheNextReminderSounds()
    {
        NoteRig r;
        r.notify(1, AlertSlot::Low, AlertType::LL, 0, true);
        const int posts = r.backend.posts.size();
        r.remind(1, AlertSlot::Low, AlertType::LL, 0, false);       // the mode has ended: the flag is off
        QCOMPARE(r.backend.posts.size(), posts + 1);
        QCOMPARE(r.backend.last().request.hints.feedback, QStringLiteral("clock"));
        QVERIFY(r.holds(1, AlertSlot::Low));
    }

    // Silent mode covers one Subject: the other one's alarm sounds, and a silent leader does not mute it.
    void aSilentSubjectDoesNotMuteTheOthers()
    {
        NoteRig r;
        r.notify(1, AlertSlot::Low, AlertType::LL, 0, true);
        r.notify(2, AlertSlot::Low, AlertType::LL, 5, false);
        QCOMPARE(r.lastFeedback(2, AlertSlot::Low), QStringLiteral("clock"));
        QVERIFY(r.holds(2, AlertSlot::Low));
        QCOMPARE(r.lastFeedback(1, AlertSlot::Low), QStringLiteral("(none)"));
    }

    // No data is about the monitor itself: not covered, even if a faulty fact says the Subject is silent.
    void noDataIsNeverSilencedBySubjectsMode()
    {
        NoteRig r;
        r.notify(1, AlertSlot::Missing, AlertType::Missing, 0, true);
        QCOMPARE(r.lastFeedback(1, AlertSlot::Missing), QStringLiteral("general_warning"));
        r.remind(1, AlertSlot::Missing, AlertType::Missing, 0, true);
        QCOMPARE(r.lastFeedback(1, AlertSlot::Missing), QStringLiteral("general_warning"));
    }

    // Acknowledged and snoozed slotList are unaffected: an Update for an acknowledged slot posts nothing.
    void anAcknowledgedSlotStaysWithoutAnEntryWhenTheModeStarts()
    {
        NoteRig r;
        r.notify(1, AlertSlot::Low, AlertType::LL);
        r.acknowledge(1, AlertSlot::Low, AlertType::LL);
        const int posts = r.backend.posts.size();
        r.send(AlertEffect::Update, 1, AlertSlot::Low, AlertType::LL, AlertState::Acknowledged, 0, true);
        QCOMPARE(r.backend.posts.size(), posts);
    }

    // Silencing the leader hands the tone to the next urgent alarm.
    void silencingTheLeaderHandsTheToneOn()
    {
        NoteRig r;
        r.notify(1, AlertSlot::Low, AlertType::LL, 0);
        r.notify(2, AlertSlot::High, AlertType::HH, 3);
        QVERIFY(r.holds(1, AlertSlot::Low));
        r.send(AlertEffect::Update, 1, AlertSlot::Low, AlertType::LL, AlertState::Active, 0, true);
        QVERIFY(r.holds(2, AlertSlot::High));
        QCOMPARE(r.lastFeedback(2, AlertSlot::High), QStringLiteral("clock"));
    }
};

QTEST_MAIN(TstNotifierSilent)
#include "tst_notifiersilent.moc"
