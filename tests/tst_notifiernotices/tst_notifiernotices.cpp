// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Michele Brami
//
// The notices. The
// interruption notice, the approval notice, the guard and report notices, and the page-only "ended"
// entries of alerts that cleared unacknowledged: what each posts, with which hints, when it is
// replaced, withdrawn or forgotten, and that none of them is ever silenced by a Subject's mode.
// Synthetic data.
#include "notifiertest.h"
#include "service/noticewatcher.h"

#include <QtTest>

using namespace lm;
using namespace lm::testing;

namespace {

SubjectStatus stateOf(const QString &connection, ScheduleState state, const QString &cause = QString())
{
    SubjectStatus s;
    s.subject = noteSubject(1);
    s.connection = connection;
    s.state = state;
    s.cause = cause;
    return s;
}

} // namespace

class TstNotifierNotices : public QObject
{
    Q_OBJECT

    QTemporaryDir m_dir;
    NotificationTranslator m_translator;

    struct Rig : NoteRig
    {
        Rig() : watcher(&presenter)
        {
            presenter.setConnectionInfo([](const QString &) {
                return QStringList() << noteSubject(1).toString() << noteSubject(2).toString();
            }, [](const QString &c) { return c == QLatin1String("c1") ? QStringLiteral("Home") : c; });
        }
        NoticeWatcher watcher;
    };

private slots:
    void initTestCase()
    {
        QVERIFY(m_dir.isValid());
        QVERIFY(loadNoteLanguage(&m_dir, &m_translator, "en"));
    }

    // ---- the interruption ------------------------------------------------------------------------

    void theInterruptionIsAPageOnlyNoticeReplacedByANewerOne()
    {
        Rig r;
        AlertEffect e;
        e.kind = AlertEffect::Interruption;
        e.gapFrom = noteAt(1, 5);
        e.gapTo = noteAt(2, 30);
        r.presenter.deliver(e);
        const FakeNoteBackend::Post p = r.backend.last();
        QCOMPARE(p.request.summary, QStringLiteral("GLUCOSE MONITOR WAS DOWN"));
        QCOMPARE(p.request.body, QStringLiteral("No readings were collected from 01:05 to 02:30.\nAlerts could not fire during that time."));
        QCOMPARE(p.request.hints.urgency, 0);
        QCOMPARE(p.request.hints.priority, 50);
        QVERIFY(p.request.hints.feedback.isEmpty() && !p.request.hints.banner && !p.request.hints.displayOn);
        e.gapFrom = noteAt(4, 0);
        e.gapTo = noteAt(4, 40);
        r.presenter.deliver(e);
        QCOMPARE(r.backend.last().replaces, p.id);                  // one entry, replaced
        QVERIFY(r.backend.last().request.body.contains(QStringLiteral("04:00")));
        QCOMPARE(r.backend.live.size(), 1);
    }

    // ---- waiting for approval ---------------------------------------------------------------------

    void theApprovalNoticeSoundsAtEachRaiseAndGoesWhenApproved()
    {
        Rig r;
        r.watcher.approvalRaised(QStringLiteral("c1"));
        const FakeNoteBackend::Post first = r.backend.last();
        QCOMPARE(first.request.summary, QStringLiteral("APPROVE NEW PATIENT"));
        QCOMPARE(first.request.body, QStringLiteral("AB, CD is waiting to be monitored.\nNo alerts until you approve in the app."));
        QCOMPARE(first.request.hints.urgency, 1);
        QCOMPARE(first.request.hints.priority, 100);
        QCOMPARE(first.request.hints.feedback, QStringLiteral("general_warning"));
        QVERIFY(first.request.hints.banner);
        r.watcher.approvalRaised(QStringLiteral("c1"));                // the 15-minute re-raise
        QCOMPARE(r.backend.last().replaces, first.id);
        QCOMPARE(r.backend.last().request.hints.feedback, QStringLiteral("general_warning"));
        QCOMPARE(r.backend.live.size(), 1);
        r.watcher.approvalSettled(QStringLiteral("c1"));
        QCOMPARE(r.backend.closes, QList<quint32>() << first.id);
        QVERIFY(r.backend.live.isEmpty());
        r.watcher.approvalSettled(QStringLiteral("c1"));               // nothing left to withdraw
        QCOMPARE(r.backend.closes.size(), 1);
    }

    void twoConnectionsWaitingHaveTheirOwnNotices()
    {
        Rig r;
        r.watcher.approvalRaised(QStringLiteral("c1"));
        r.watcher.approvalRaised(QStringLiteral("c2"));
        QCOMPARE(r.backend.live.size(), 2);
        r.watcher.approvalSettled(QStringLiteral("c1"));
        QCOMPARE(r.backend.live.size(), 1);
    }

    // ---- the guard and the report ---------------------------------------------------------------------

    void theGuardNoticeSaysHowManyAttemptsAndGoesWhenTheConnectionIsEnabledAgain()
    {
        Rig r;
        for (int i = 0; i < 3; ++i)
            r.watcher.subjectStateChanged(stateOf(QStringLiteral("c1"), ScheduleState::Backoff, QStringLiteral("guard-kill")));
        QVERIFY(r.backend.posts.isEmpty());                             // failures alone say nothing
        r.watcher.subjectStateChanged(stateOf(QStringLiteral("c1"), ScheduleState::DisabledByGuard, QStringLiteral("disabled-by-guard")));
        const FakeNoteBackend::Post p = r.backend.last();
        QCOMPARE(p.request.summary, QStringLiteral("GLUCOSE SOURCE DISABLED"));
        QCOMPARE(p.request.body, QStringLiteral("Home was disabled after 3 failed attempts.\nCause: Reading stopped for safety. Re-enable it in the app."));
        QVERIFY(p.request.hints.urgency == 0 && !p.request.hints.banner && p.request.hints.feedback.isEmpty());
        // The account name is not there: only the connection's label.
        QVERIFY(!p.request.body.contains(QStringLiteral("c1")));
        r.watcher.subjectStateChanged(stateOf(QStringLiteral("c1"), ScheduleState::DisabledByGuard, QStringLiteral("disabled-by-guard")));
        QCOMPARE(r.backend.last().replaces, p.id);                      // replaced, not repeated
        r.watcher.subjectStateChanged(stateOf(QStringLiteral("c1"), ScheduleState::Idle));
        QCOMPARE(r.backend.closes, QList<quint32>() << p.id);
    }

    void theReportNoticeIsPageOnlyAndGoesWhenHandled()
    {
        Rig r;
        r.watcher.diagnosticReportProposed(QStringLiteral("c1"));
        const FakeNoteBackend::Post p = r.backend.last();
        QCOMPARE(p.request.summary, QStringLiteral("REVIEW DIAGNOSTIC REPORT"));
        QVERIFY(p.request.hints.urgency == 0 && !p.request.hints.banner && p.request.hints.feedback.isEmpty());
        r.watcher.reportSettled(QStringLiteral("c1"));
        QCOMPARE(r.backend.closes, QList<quint32>() << p.id);
    }

    // None of the notices is covered by a Subject's silent mode: they never look at it.
    void noNoticeIsSilencedByASubjectsMode()
    {
        Rig r;
        r.notify(1, AlertSlot::Low, AlertType::L, 0, true);              // Subject 1 is silent
        r.watcher.approvalRaised(QStringLiteral("c1"));
        QCOMPARE(r.backend.last().request.hints.feedback, QStringLiteral("general_warning"));
        AlertEffect gap;
        gap.kind = AlertEffect::Interruption;
        gap.gapFrom = noteAt(1, 0);
        gap.gapTo = noteAt(2, 0);
        r.presenter.deliver(gap);
        QVERIFY(r.backend.last().request.summary == QStringLiteral("GLUCOSE MONITOR WAS DOWN"));
        r.notify(1, AlertSlot::Missing, AlertType::Missing, 0, true);
        QCOMPARE(r.lastFeedback(1, AlertSlot::Missing), QStringLiteral("general_warning"));
    }

    // ---- the "ended" entries -------------------------------------------------------------------------------

    void anAlertThatClearedUnacknowledgedLeavesOnePageOnlyEntry()
    {
        Rig r;
        r.notify(1, AlertSlot::Low, AlertType::LL, 12);                  // onset 03:12
        r.clear(1, AlertSlot::Low, AlertType::LL);
        QVERIFY(r.backend.live.size() == 1);                              // the alarm is gone, the ended entry is up
        const FakeNoteBackend::Post p = r.backend.last();
        QCOMPARE(p.request.summary, QStringLiteral("VERY LOW GLUCOSE ENDED"));
        QVERIFY(p.request.body.startsWith(QStringLiteral("AB\nThe alert ended at 05:20 after ")));
        QVERIFY(p.request.body.endsWith(QStringLiteral("You did not acknowledge it.")));
        QCOMPARE(p.request.hints.urgency, 0);
        QVERIFY(!p.request.hints.banner && p.request.hints.feedback.isEmpty() && p.request.hints.removable);
        QVERIFY(p.request.hints.iconPath.isEmpty() || p.request.hints.iconPath.endsWith(QStringLiteral("alert-ll.png")));
    }

    void thereIsNoEndedEntryAfterAnAcknowledgementASnoozeOrForNoData()
    {
        {   Rig r;
            r.notify(1, AlertSlot::Low, AlertType::L);
            r.acknowledge(1, AlertSlot::Low, AlertType::L);
            r.clear(1, AlertSlot::Low, AlertType::L);
            QVERIFY(r.backend.live.isEmpty());
        }
        {   Rig r;                                                          // snoozed: withdrawn, later cleared
            r.notify(1, AlertSlot::High, AlertType::H);
            r.send(AlertEffect::Withdraw, 1, AlertSlot::High, AlertType::H, AlertState::Snoozed);
            r.send(AlertEffect::Withdraw, 1, AlertSlot::High, AlertType::H, AlertState::Cleared);
            QVERIFY(r.backend.live.isEmpty());
        }
        {   Rig r;                                                          // no data never leaves one
            r.notify(1, AlertSlot::Missing, AlertType::Missing);
            r.clear(1, AlertSlot::Missing, AlertType::Missing);
            QVERIFY(r.backend.live.isEmpty());
        }
        {   Rig r;                                                          // a clearing we never saw raised: nothing to say
            r.clear(1, AlertSlot::Low, AlertType::L);
            QVERIFY(r.backend.posts.isEmpty());
        }
    }

    void theFourKindsHaveTheirWordsAndOneEntryPerSlotIsReplacedByANewerOne()
    {
        Rig r;
        r.notify(1, AlertSlot::Low, AlertType::L);
        r.clear(1, AlertSlot::Low, AlertType::L);
        QCOMPARE(r.backend.last().request.summary, QStringLiteral("LOW GLUCOSE ENDED"));
        const quint32 id = r.backend.last().id;
        r.notify(1, AlertSlot::Low, AlertType::LL);                          // the slot becomes active again: the old entry goes
        QVERIFY(r.backend.closes.contains(id));
        r.clear(1, AlertSlot::Low, AlertType::LL);
        QCOMPARE(r.backend.last().request.summary, QStringLiteral("VERY LOW GLUCOSE ENDED"));
        r.notify(1, AlertSlot::High, AlertType::H);
        r.clear(1, AlertSlot::High, AlertType::H);
        QCOMPARE(r.backend.last().request.summary, QStringLiteral("HIGH GLUCOSE ENDED"));
        r.notify(2, AlertSlot::High, AlertType::HH);
        r.clear(2, AlertSlot::High, AlertType::HH);
        QCOMPARE(r.backend.last().request.summary, QStringLiteral("VERY HIGH GLUCOSE ENDED"));
        // Two ended entries of the same slot in a row: the second replaces the first.
        r.notify(1, AlertSlot::High, AlertType::H);
        r.clear(1, AlertSlot::High, AlertType::H);
        QCOMPARE(r.backend.live.size(), 3);                                    // the Subject 1 low, high; Subject 2 high
    }

    // The user deletes an ended entry: that is not an acknowledgement of anything.
    void deletingAnEndedEntryAcknowledgesNothing()
    {
        Rig r;
        QList<QUuid> acked;
        r.presenter.setAcknowledgeHandler([&acked](const QUuid &s, AlertSlot) { acked << s; });
        r.notify(1, AlertSlot::Low, AlertType::L);
        r.clear(1, AlertSlot::Low, AlertType::L);
        r.backend.userDeletes(r.backend.last().id);
        QVERIFY(acked.isEmpty());
        // The next ended entry is a new notification.
        r.notify(1, AlertSlot::Low, AlertType::L);
        r.clear(1, AlertSlot::Low, AlertType::L);
        QCOMPARE(r.backend.last().replaces, quint32(0));
    }

    // ---- the restart ----------------------------------------------------------------------------------------

    // The notices that describe the moment are closed at start when this run did not post them again;
    // the interruption notice and the ended entries are history and stay.
    void atStartTheOutOfDateNoticesAreClosedAndTheHistoryStays()
    {
        MemoryNoteSettings settings;
        FakeNoteBackend backend;
        SubjectLabelBook labels;
        NoteClock clock;
        quint32 approvalId, reportId, interruptionId, endedId;
        {
            AlertNotificationPresenter first(&backend, &settings, &labels, &clock);
            first.setTexts(NotificationTexts(noteZoneTime));
            first.notifyApproval(QStringLiteral("c1"));
            approvalId = backend.last().id;
            first.notifyReportProposed(QStringLiteral("c1"));
            reportId = backend.last().id;
            AlertEffect gap;
            gap.kind = AlertEffect::Interruption;
            gap.gapFrom = noteAt(1, 0);
            gap.gapTo = noteAt(2, 0);
            first.deliver(gap);
            interruptionId = backend.last().id;
            first.deliver(noteEffect(AlertEffect::Notify, 1, AlertSlot::Low, AlertType::L, AlertState::Active));
            first.deliver(noteEffect(AlertEffect::Withdraw, 1, AlertSlot::Low, AlertType::L, AlertState::Cleared));
            endedId = backend.last().id;
        }
        AlertNotificationPresenter second(&backend, &settings, &labels, &clock);
        second.setTexts(NotificationTexts(noteZoneTime));
        second.notifyApproval(QStringLiteral("c1"));                           // still waiting: posted again, replaces
        QCOMPARE(backend.last().replaces, approvalId);
        const int closedBefore = backend.closes.size();
        second.closeOrphans();
        QCOMPARE(backend.closes.mid(closedBefore), QList<quint32>() << reportId);                // the report was not proposed again
        QVERIFY(backend.live.contains(approvalId) && backend.live.contains(interruptionId) && backend.live.contains(endedId));
    }
};

QTEST_MAIN(TstNotifierNotices)
#include "tst_notifiernotices.moc"
