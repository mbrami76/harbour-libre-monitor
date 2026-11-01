// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Michele Brami
//
// The mechanics of the notifications on a fake backend: one
// notification per slot replaced in place, the ids kept across a restart, the ids the service
// closed itself, the user's deletion as the acknowledgement, a lowered level replaced without
// feedback, and the relay that turns the effects into the AlertChanged event. The later items add
// their own test programs (arbiter, reminders, silent mode, notices). Every value is synthetic.
#include "notifiertest.h"

#include <QProcess>
#include <QTemporaryDir>
#include <QtTest>

using namespace lm;
using namespace lm::testing;

namespace {

// `log` (optional) is one list shared by both recorders, so a test can see which was called first.
class EventRecorder : public ServiceEventSink
{
public:
    EventRecorder() : log(0) {}
    QStringList *log;
    struct Change { QUuid subject; QString slot, state; };
    void alertChanged(const QUuid &s, const QString &slot, const QString &state)
    {
        Change c; c.subject = s; c.slot = slot; c.state = state; changes.append(c);
        if (log) log->append(QStringLiteral("event"));
    }
    QList<Change> changes;
};

class Recorder : public AlertNotifier
{
public:
    Recorder() : log(0) {}
    void deliver(const AlertEffect &e) { seen.append(e); if (log) log->append(QStringLiteral("notification")); }
    QList<AlertEffect> seen;
    QStringList *log;
};

} // namespace

class TstAlertNotification : public QObject
{
    Q_OBJECT

    QTemporaryDir m_dir;
    NotificationTranslator m_translator;

    // A rig: two Subjects, the first with the label "AB", the second without one.
    struct Rig
    {
        Rig() : presenter(&backend, &settings, &labels, &clock)
        {
            QHash<QUuid, QString> l;
            l.insert(noteSubject(1), QStringLiteral("AB"));
            labels.set(QList<QUuid>() << noteSubject(1) << noteSubject(2), l);
            presenter.setTexts(NotificationTexts(noteZoneTime));
            presenter.setAcknowledgeHandler([this](const QUuid &s, AlertSlot slot) { acked.append(qMakePair(s, slot)); });
            EffectFacts f;
            f.levelSince = noteAt(3, 12);
            f.limit = 54;
            for (int s = 1; s <= 2; ++s)
                for (int k = 0; k < 3; ++k)
                    facts.set(noteSubject(s), static_cast<AlertSlot>(k), f);
            presenter.setFactsReader(&facts);
        }
        void ack(const AlertEffect &e)
        {
            AlertEffect u = e;
            u.kind = AlertEffect::Update;
            u.state = AlertState::Acknowledged;
            presenter.deliver(u);
        }
        FakeNoteBackend backend;
        MemoryNoteSettings settings;
        SubjectLabelBook labels;
        NoteClock clock;
        FakeFacts facts;
        AlertNotificationPresenter presenter;
        QList<QPair<QUuid, AlertSlot> > acked;
    };

private slots:
    void initTestCase()
    {
        QVERIFY(m_dir.isValid());
        QProcess p;
        p.start(QStringLiteral("lrelease"), QStringList() << QStringLiteral("-idbased")
                << QString(LM_TRANSLATIONS_DIR) + QStringLiteral("/harbour-libre-monitor.ts")
                << QStringLiteral("-qm") << m_dir.path() + QStringLiteral("/harbour-libre-monitor.qm"));
        QVERIFY(p.waitForFinished(30000) && p.exitCode() == 0);
        m_translator.load(m_dir.path(), QStringLiteral("harbour-libre-monitor"), QStringLiteral("en"));
    }

    // ---- one notification per slot, replaced in place -----------------------------------------

    void aSlotHasOneNotificationThatEveryEffectReplaces()
    {
        Rig r;
        r.presenter.deliver(noteEffect(AlertEffect::Notify, 1, AlertSlot::Low, AlertType::L, AlertState::Active));
        QCOMPARE(r.backend.posts.at(0).replaces, quint32(0));
        const quint32 id = r.backend.posts.at(0).id;
        QVERIFY(id != 0);
        QCOMPARE(r.backend.posts.at(0).request.summary, QStringLiteral("LOW GLUCOSE"));
        r.presenter.deliver(noteEffect(AlertEffect::Notify, 1, AlertSlot::Low, AlertType::LL, AlertState::Active));     // escalation
        QCOMPARE(r.backend.posts.at(1).replaces, id);
        QCOMPARE(r.backend.posts.at(1).id, id);
        QCOMPARE(r.backend.posts.at(1).request.summary, QStringLiteral("VERY LOW GLUCOSE"));     // new text, new icon, same entry
        QVERIFY(r.backend.posts.at(1).request.hints.iconPath.isEmpty() || r.backend.posts.at(1).request.hints.iconPath.endsWith(QStringLiteral("urgent-low.png")));
        // Another slot and another Subject get notifications of their own.
        r.presenter.deliver(noteEffect(AlertEffect::Notify, 1, AlertSlot::High, AlertType::H, AlertState::Active));
        r.presenter.deliver(noteEffect(AlertEffect::Notify, 2, AlertSlot::Low, AlertType::L, AlertState::Active));
        QCOMPARE(r.backend.posts.at(2).replaces, quint32(0));
        QCOMPARE(r.backend.posts.at(3).replaces, quint32(0));
        QVERIFY(r.backend.posts.at(2).id != id && r.backend.posts.at(3).id != r.backend.posts.at(2).id);
        QCOMPARE(r.presenter.idOf(noteSubject(1), AlertSlot::Low), id);
    }

    void aWithdrawTakesTheNotificationDownOnceAndTheNextOneIsNew()
    {
        Rig r;
        r.presenter.deliver(noteEffect(AlertEffect::Notify, 1, AlertSlot::Low, AlertType::L, AlertState::Active));
        const quint32 id = r.backend.last().id;
        r.presenter.deliver(noteEffect(AlertEffect::Withdraw, 1, AlertSlot::Low, AlertType::L, AlertState::Cleared));
        QCOMPARE(r.backend.closes, QList<quint32>() << id);
        QVERIFY(!r.presenter.hasAlarm(noteSubject(1), AlertSlot::Low));
        r.presenter.deliver(noteEffect(AlertEffect::Withdraw, 1, AlertSlot::Low, AlertType::L, AlertState::Cleared));
        QCOMPARE(r.backend.closes.size(), 1);                    // nothing is up: nothing to close
        r.presenter.deliver(noteEffect(AlertEffect::Notify, 1, AlertSlot::Low, AlertType::L, AlertState::Active));
        QCOMPARE(r.backend.last().replaces, quint32(0));         // a new alert, a new notification
        QVERIFY(r.backend.last().id != id);
    }

    void aWithdrawOfAnAlertNeverShownDoesNothing()
    {
        Rig r;
        r.presenter.deliver(noteEffect(AlertEffect::Withdraw, 1, AlertSlot::High, AlertType::H, AlertState::Snoozed));
        QVERIFY(r.backend.closes.isEmpty() && r.backend.posts.isEmpty());
    }

    void aNotificationThatCouldNotBePostedIsTriedAgainByTheNextEffect()
    {
        Rig r;
        r.backend.failNext = true;
        r.presenter.deliver(noteEffect(AlertEffect::Notify, 1, AlertSlot::Low, AlertType::L, AlertState::Active));
        QCOMPARE(r.backend.last().id, quint32(0));
        QCOMPARE(r.presenter.idOf(noteSubject(1), AlertSlot::Low), quint32(0));
        r.presenter.deliver(noteEffect(AlertEffect::Remind, 1, AlertSlot::Low, AlertType::L, AlertState::Active));
        QVERIFY(r.backend.last().id != 0);
        QCOMPARE(r.backend.last().replaces, quint32(0));
    }

    // ---- the user's deletion ---------------------------------------------------------------------

    // A deletion that the service did not make, with the reason "dismissed by the user", is the
    // acknowledgement; the entry is gone, so the engine's Update(acknowledged) posts nothing.
    void aDeletionByTheUserAcknowledgesTheSlot()
    {
        Rig r;
        r.presenter.deliver(noteEffect(AlertEffect::Notify, 1, AlertSlot::Low, AlertType::LL, AlertState::Active));
        const quint32 id = r.backend.last().id;
        r.backend.userDeletes(id);
        QCOMPARE(r.acked.size(), 1);
        QCOMPARE(r.acked.first().first, noteSubject(1));
        QVERIFY(r.acked.first().second == AlertSlot::Low);
        QCOMPARE(r.presenter.idOf(noteSubject(1), AlertSlot::Low), quint32(0));
        const int posts = r.backend.posts.size();
        r.ack(noteEffect(AlertEffect::Update, 1, AlertSlot::Low, AlertType::LL, AlertState::Acknowledged));    // the engine's answer
        QCOMPARE(r.backend.posts.size(), posts);                 // nothing is resurrected
        QVERIFY(r.backend.closes.isEmpty());                     // and there was nothing left to close
        // The alarm is still in the list, acknowledged.
        QVERIFY(r.presenter.hasAlarm(noteSubject(1), AlertSlot::Low));
    }

    void onlyTheReasonDismissedByTheUserOnAnIdWeDidNotCloseCounts()
    {
        Rig r;
        r.presenter.deliver(noteEffect(AlertEffect::Notify, 1, AlertSlot::Low, AlertType::L, AlertState::Active));
        const quint32 id = r.backend.last().id;
        r.backend.closedWith(id, 3);                             // closed by call, on an older object of the id
        r.backend.closedWith(id, 1);                             // expired
        QVERIFY(r.acked.isEmpty());
        QCOMPARE(r.presenter.idOf(noteSubject(1), AlertSlot::Low), id);
        // The service's own close: a stray echo of it with reason 2 is not a deletion by the user.
        r.presenter.deliver(noteEffect(AlertEffect::Withdraw, 1, AlertSlot::Low, AlertType::L, AlertState::Cleared));
        r.backend.closedWith(id, 2);
        QVERIFY(r.acked.isEmpty());
        // An id nobody owns is nobody's acknowledgement.
        r.backend.closedWith(9999, 2);
        QVERIFY(r.acked.isEmpty());
    }

    // The acknowledgement from the application: the engine's Update closes the entry and the service
    // remembers that it closed it, so the platform's echo is not taken for the user.
    void anAcknowledgementFromTheAppClosesTheEntry()
    {
        Rig r;
        r.presenter.deliver(noteEffect(AlertEffect::Notify, 1, AlertSlot::High, AlertType::HH, AlertState::Active));
        const quint32 id = r.backend.last().id;
        r.ack(noteEffect(AlertEffect::Update, 1, AlertSlot::High, AlertType::HH, AlertState::Acknowledged));
        QCOMPARE(r.backend.closes, QList<quint32>() << id);
        r.backend.closedWith(id, 2);
        QVERIFY(r.acked.isEmpty());
        QCOMPARE(r.presenter.idOf(noteSubject(1), AlertSlot::High), quint32(0));
    }

    // A notification the user deleted and the service re-posts (urgent low active again after the
    // acknowledgement) comes back under a new id, and the new id is used from then on.
    void aRepostedNotificationUsesTheIdItIsGivenBack()
    {
        Rig r;
        r.presenter.deliver(noteEffect(AlertEffect::Notify, 1, AlertSlot::Low, AlertType::LL, AlertState::Active));
        const quint32 first = r.backend.last().id;
        r.backend.userDeletes(first);
        r.presenter.deliver(noteEffect(AlertEffect::Notify, 1, AlertSlot::Low, AlertType::LL, AlertState::Active));    // re-alert
        const quint32 second = r.backend.last().id;
        QVERIFY(second != 0 && second != first);
        QCOMPARE(r.presenter.idOf(noteSubject(1), AlertSlot::Low), second);
        // A reminder replaces the new one, not the old one.
        r.presenter.deliver(noteEffect(AlertEffect::Remind, 1, AlertSlot::Low, AlertType::LL, AlertState::Active));
        QCOMPARE(r.backend.last().replaces, second);
        // And a deletion whose id is the OLD one, arriving late, is not taken for the new one.
        r.backend.closedWith(first, 2);
        QCOMPARE(r.acked.size(), 1);                             // only the first deletion counted
    }

    // Even a platform that hands the old id back (the notification was gone) is followed.
    void aPlatformThatKeepsTheIdIsFollowedToo()
    {
        Rig r;
        r.presenter.deliver(noteEffect(AlertEffect::Notify, 1, AlertSlot::Low, AlertType::L, AlertState::Active));
        const quint32 id = r.backend.last().id;
        r.backend.deleted.insert(id);                            // gone, and the handler was not told
        r.presenter.deliver(noteEffect(AlertEffect::Remind, 1, AlertSlot::Low, AlertType::L, AlertState::Active));
        QVERIFY(r.backend.last().id != id);
        QCOMPARE(r.presenter.idOf(noteSubject(1), AlertSlot::Low), r.backend.last().id);
    }

    // ---- quiet updates ------------------------------------------------------------------------------

    // A lowered level, a changed cause: replaced in place WITHOUT feedback, so a tone stops (U2b).
    void aLoweredLevelIsReplacedQuietlyInPlace()
    {
        Rig r;
        r.presenter.deliver(noteEffect(AlertEffect::Notify, 1, AlertSlot::Low, AlertType::LL, AlertState::Active));
        QCOMPARE(r.backend.last().request.hints.feedback, QStringLiteral("clock"));
        const quint32 id = r.backend.last().id;
        r.presenter.deliver(noteEffect(AlertEffect::Update, 1, AlertSlot::Low, AlertType::L, AlertState::Active));
        QCOMPARE(r.backend.last().replaces, id);
        QVERIFY(r.backend.last().request.hints.feedback.isEmpty());
        QCOMPARE(r.backend.last().request.summary, QStringLiteral("LOW GLUCOSE"));
    }

    // ---- the ids across a restart ---------------------------------------------------------------------

    void theIdsSurviveARestartOfTheService()
    {
        MemoryNoteSettings settings;
        FakeNoteBackend backend;
        SubjectLabelBook labels;
        NoteClock clock;
        quint32 id;
        {
            AlertNotificationPresenter first(&backend, &settings, &labels, &clock);
            first.deliver(noteEffect(AlertEffect::Notify, 1, AlertSlot::Low, AlertType::LL, AlertState::Active));
            id = backend.last().id;
        }
        AlertNotificationPresenter second(&backend, &settings, &labels, &clock);
        second.deliver(noteEffect(AlertEffect::Notify, 1, AlertSlot::Low, AlertType::LL, AlertState::Active));     // restored
        QCOMPARE(backend.last().replaces, id);
        QCOMPARE(backend.last().id, id);
        QCOMPARE(backend.live.size(), 1);                        // one entry, not two
        second.closeOrphans();
        QVERIFY(backend.closes.isEmpty());                       // it was re-notified: nothing is an orphan
        // Only opaque UUIDs and slot names are in the settings, never a name.
        for (QHash<QString, QString>::const_iterator it = settings.data.constBegin(); it != settings.data.constEnd(); ++it)
            QVERIFY(it.key().startsWith(QStringLiteral("note.")));
    }

    // The alert ended while the service was off: its entry is found and taken down.
    void anEntryOfAnAlertThatNoLongerExistsIsClosedAtStart()
    {
        MemoryNoteSettings settings;
        FakeNoteBackend backend;
        SubjectLabelBook labels;
        NoteClock clock;
        quint32 lowId, highId;
        {
            AlertNotificationPresenter first(&backend, &settings, &labels, &clock);
            first.deliver(noteEffect(AlertEffect::Notify, 1, AlertSlot::Low, AlertType::L, AlertState::Active));
            lowId = backend.last().id;
            first.deliver(noteEffect(AlertEffect::Notify, 2, AlertSlot::High, AlertType::H, AlertState::Active));
            highId = backend.last().id;
        }
        AlertNotificationPresenter second(&backend, &settings, &labels, &clock);
        second.deliver(noteEffect(AlertEffect::Notify, 2, AlertSlot::High, AlertType::H, AlertState::Active));     // only the high one is still on
        second.closeOrphans();
        QCOMPARE(backend.closes, QList<quint32>() << lowId);
        QVERIFY(backend.live.contains(highId) && !backend.live.contains(lowId));
        // And once closed it stays closed: a third start finds nothing to do.
        AlertNotificationPresenter third(&backend, &settings, &labels, &clock);
        third.deliver(noteEffect(AlertEffect::Notify, 2, AlertSlot::High, AlertType::H, AlertState::Active));
        third.closeOrphans();
        QCOMPARE(backend.closes.size(), 1);
    }

    void anAcknowledgedAlertRestoredAfterARestartLeavesNoEntry()
    {
        MemoryNoteSettings settings;
        FakeNoteBackend backend;
        SubjectLabelBook labels;
        NoteClock clock;
        quint32 id;
        {
            AlertNotificationPresenter first(&backend, &settings, &labels, &clock);
            first.deliver(noteEffect(AlertEffect::Notify, 1, AlertSlot::Low, AlertType::L, AlertState::Active));
            id = backend.last().id;
        }
        // The user acknowledged it in the app while the service was off; the restore says "acknowledged".
        AlertNotificationPresenter second(&backend, &settings, &labels, &clock);
        second.deliver(noteEffect(AlertEffect::Update, 1, AlertSlot::Low, AlertType::L, AlertState::Acknowledged));
        second.closeOrphans();
        QVERIFY(backend.closes.contains(id));
        QVERIFY(!backend.live.contains(id));
    }

    // ---- the value, only with the setting ----------------------------------------------------------------

    void theValueIsReadAtPostingTimeAndOnlyWithTheSetting()
    {
        Rig r;
        FakeValues values;
        values.has = true;
        values.value = 52;
        values.unit = QStringLiteral("mg/dL");
        values.trend = 2;
        values.measuredAt = noteAt(3, 19);
        r.presenter.setValueSource(&values);
        r.presenter.deliver(noteEffect(AlertEffect::Notify, 1, AlertSlot::Low, AlertType::LL, AlertState::Active));
        QVERIFY(!r.backend.last().request.body.contains(QStringLiteral("52")));
        r.presenter.setShowValue(true);
        r.presenter.deliver(noteEffect(AlertEffect::Remind, 1, AlertSlot::Low, AlertType::LL, AlertState::Active));
        QVERIFY(r.backend.last().request.body.startsWith(QString::fromUtf8("AB \xC2\xB7 52 mg/dL \xE2\x86\x98")));
        values.value = 49;                                        // a newer reading: the next post shows it
        r.presenter.deliver(noteEffect(AlertEffect::Remind, 1, AlertSlot::Low, AlertType::LL, AlertState::Active));
        QVERIFY(r.backend.last().request.body.contains(QStringLiteral("49 mg/dL")));
        // A Subject in mmol/L shows its limit in mmol/L.
        values.unit = QStringLiteral("mmol/L");
        values.value = 2.8;
        r.presenter.deliver(noteEffect(AlertEffect::Remind, 1, AlertSlot::Low, AlertType::LL, AlertState::Active));
        QVERIFY(r.backend.last().request.body.contains(QStringLiteral("Below 3.0 mmol/L")));
    }

    void theHintsAreTheCatalogues()
    {
        Rig r;
        const NotificationCatalog cat;
        // (the urgent alarm last: while its tone rings the others would be kept silent)
        r.presenter.deliver(noteEffect(AlertEffect::Notify, 2, AlertSlot::High, AlertType::H, AlertState::Active));
        r.presenter.deliver(noteEffect(AlertEffect::Notify, 1, AlertSlot::Missing, AlertType::Missing, AlertState::Active));
        r.presenter.deliver(noteEffect(AlertEffect::Notify, 1, AlertSlot::Low, AlertType::LL, AlertState::Active));
        const NoteKind kinds[] = { NoteKind::High, NoteKind::NoData, NoteKind::UrgentLow };
        for (int i = 0; i < 3; ++i) {
            const NoteHints want = cat.hints(kinds[i], Sound::Own);
            const NoteHints got = r.backend.posts.at(i).request.hints;
            QCOMPARE(got.urgency, want.urgency);
            QCOMPARE(got.priority, want.priority);
            QCOMPARE(got.feedback, want.feedback);
            QCOMPARE(got.displayOn, want.displayOn);
            QCOMPARE(got.category, want.category);
        }
    }

    // ---- the labels -----------------------------------------------------------------------------------

    void aLabelIsShortOrItIsNotALabel()
    {
        SubjectLabelBook b;
        QHash<QUuid, QString> l;
        l.insert(noteSubject(1), QStringLiteral("  Jo "));       // trimmed
        l.insert(noteSubject(2), QStringLiteral("Johnny"));      // a name, not a label: ignored
        l.insert(noteSubject(3), QString());
        b.set(QList<QUuid>() << noteSubject(1) << noteSubject(2) << noteSubject(3), l);
        QCOMPARE(b.labelOf(noteSubject(1)), QStringLiteral("Jo"));
        QCOMPARE(b.labelOf(noteSubject(2)), QStringLiteral("P2"));    // its position
        QCOMPARE(b.labelOf(noteSubject(3)), QStringLiteral("P3"));
        QCOMPARE(b.labelOf(noteSubject(9)), QStringLiteral("?"));
        FakeNoteBackend backend;
        AlertNotificationPresenter p(&backend, 0, 0, 0);          // safe without settings, labels and clock
        p.deliver(noteEffect(AlertEffect::Notify, 1, AlertSlot::Low, AlertType::L, AlertState::Active));
        QVERIFY(backend.last().request.body.startsWith(QStringLiteral("?")));
    }

    // ---- the relay -----------------------------------------------------------------------------------------

    void theRelayReportsEveryVisibleChangeAndOnlyThose()
    {
        Recorder next;
        EventRecorder events;
        AlertChangeRelay relay(&next, &events);
        relay.deliver(noteEffect(AlertEffect::Journal, 1, AlertSlot::Low, AlertType::L, AlertState::Active));
        relay.deliver(noteEffect(AlertEffect::Notify, 1, AlertSlot::Low, AlertType::L, AlertState::Active));
        relay.deliver(noteEffect(AlertEffect::Remind, 1, AlertSlot::Low, AlertType::L, AlertState::Active));
        relay.deliver(noteEffect(AlertEffect::Update, 1, AlertSlot::Low, AlertType::L, AlertState::Acknowledged));
        relay.deliver(noteEffect(AlertEffect::Withdraw, 1, AlertSlot::Low, AlertType::L, AlertState::Snoozed));
        relay.deliver(noteEffect(AlertEffect::Withdraw, 2, AlertSlot::High, AlertType::H, AlertState::Cleared));
        AlertEffect gap;
        gap.kind = AlertEffect::Interruption;
        relay.deliver(gap);
        QCOMPARE(next.seen.size(), 7);                          // the notifier hears of everything
        QCOMPARE(events.changes.size(), 4);
        QCOMPARE(events.changes.at(0).state, QStringLiteral("active"));
        QCOMPARE(events.changes.at(1).state, QStringLiteral("acknowledged"));
        QCOMPARE(events.changes.at(2).state, QStringLiteral("snoozed"));
        QCOMPARE(events.changes.at(3).slot, QStringLiteral("high"));
        QCOMPARE(events.changes.at(3).subject, noteSubject(2));
    }

    // The notification of an alarm is posted BEFORE anything that listens to the change is told, and the
    // resident notice is refreshed by that telling (the service runtime refreshes it in alertChanged). So
    // the alarm's banner is never queued behind the resident notice's own banner.
    void theNotificationIsPostedBeforeTheChangeIsAnnounced()
    {
        QStringList log;
        Recorder next;
        EventRecorder events;
        next.log = &log;
        events.log = &log;
        AlertChangeRelay relay(&next, &events);
        relay.deliver(noteEffect(AlertEffect::Notify, 1, AlertSlot::Low, AlertType::LL, AlertState::Active));
        relay.deliver(noteEffect(AlertEffect::Update, 1, AlertSlot::Low, AlertType::L, AlertState::Acknowledged));
        relay.deliver(noteEffect(AlertEffect::Withdraw, 1, AlertSlot::Low, AlertType::L, AlertState::Snoozed));
        QCOMPARE(log, QStringList() << "notification" << "event" << "notification" << "event" << "notification" << "event");
    }

    void theRelayWorksWithoutANotifierAndWithoutASink()
    {
        EventRecorder events;
        AlertChangeRelay noNotifier(0, &events);
        noNotifier.deliver(noteEffect(AlertEffect::Notify, 1, AlertSlot::Low, AlertType::L, AlertState::Active));
        QCOMPARE(events.changes.size(), 1);
        Recorder next;
        AlertChangeRelay noSink(&next, 0);
        noSink.deliver(noteEffect(AlertEffect::Notify, 1, AlertSlot::Low, AlertType::L, AlertState::Active));
        QCOMPARE(next.seen.size(), 1);
    }
};

QTEST_MAIN(TstAlertNotification)
#include "tst_alertnotification.moc"
