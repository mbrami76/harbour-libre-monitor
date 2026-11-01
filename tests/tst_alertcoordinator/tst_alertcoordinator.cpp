// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Michele Brami
//
// The adapter between the service and the alert engine. The scheduler's
// events, the newest stored reading, the clock and a real (file) journal are replaced by fakes and
// a manual clock; what is checked is the plumbing: a stored reading becomes an alert, the journal
// line is written (and retried when the journal is busy), the notification port is told, a restart
// restores the state, and the user's actions and the threshold edit go through in the right order.
// Every value is synthetic.
#include "alerts/alertcoordinator.h"
#include "alerts/alertjournal.h"

#include "modtest.h"

#include <QDir>
#include <QTemporaryDir>
#include <QtTest>

using namespace lm;
using namespace lm::testing;

namespace {

QUuid sid(int n = 1)
{
    return QUuid::createUuidV5(QUuid("{6ba7b810-9dad-11d1-80b4-00c04fd430c8}"), QString("coord-%1").arg(n));
}

class FakeReadings : public AlertReadingSource
{
public:
    FakeReadings() : value(100), measuredMs(0), has(false) {}
    bool newestCurrent(const QUuid &, const QString &source, double *v, QString *u, UtcTime *t)
    {
        lastSource = source;
        if (!has) return false;
        *v = value;
        *u = unit;
        *t = UtcTime::fromMSecsSinceEpoch(measuredMs);
        return true;
    }
    void set(double v, qint64 ms) { value = v; measuredMs = ms; has = true; unit = "mg/dL"; }
    double value; qint64 measuredMs; bool has; QString unit, lastSource;
};

class Recorder : public AlertNotifier
{
public:
    void deliver(const AlertEffect &e) { seen.append(e); }
    int count(AlertEffect::Kind k) const
    {
        int n = 0;
        for (int i = 0; i < seen.size(); ++i) n += seen.at(i).kind == k;
        return n;
    }
    QVector<AlertEffect> seen;
};

SubjectStatus status(const QUuid &s, qint64 lastMs, bool stale, const char *cause = "")
{
    SubjectStatus st;
    st.subject = s;
    if (lastMs >= 0) st.lastSuccessAt = UtcTime::fromMSecsSinceEpoch(lastMs);
    st.stale = stale;
    st.staleCause = QString::fromLatin1(cause);
    return st;
}

} // namespace

class TstAlertCoordinator : public QObject
{
    Q_OBJECT

    QTemporaryDir *m_dir;
    Journal m_journal;
    ManualClock m_clock;
    FakeReadings m_readings;
    Recorder m_notes;

    AlertSubjectSettings settings() const
    {
        AlertSubjectSettings s;
        s.currentSource = "libre.current";
        return s;
    }

private slots:
    void init()
    {
        m_dir = new QTemporaryDir;
        QVERIFY(m_journal.open(m_dir->path() + "/journal.db"));
        m_readings = FakeReadings();
        m_notes.seen.clear();
        m_clock.setMs(1768000000000LL);
    }
    void cleanup() { m_journal.close(); delete m_dir; }

    // The configuration file's per-Subject times can change while the service runs: a
    // high reading that waits out its persistence time becomes an alert as soon as the time is cut to zero,
    // and an unknown Subject is ignored.
    void newSubjectSettingsApplyAtOnceAfterStart()
    {
        AlertCoordinator c(&m_journal, &m_readings, &m_notes, &m_clock);
        c.addSubject(sid(), settings());                      // default: H must hold for 15 minutes
        c.start();
        m_readings.set(200, m_clock.ms());
        c.dataChanged(sid());
        QCOMPARE(m_notes.count(AlertEffect::Notify), 0);      // waiting out the persistence time
        AlertSubjectSettings now = settings();
        now.persistenceHighMs = 0;
        c.updateSubjectSettings(sid(99), now);                // unknown Subject: nothing happens
        QCOMPARE(m_notes.count(AlertEffect::Notify), 0);
        c.updateSubjectSettings(sid(), now);
        QCOMPARE(m_notes.count(AlertEffect::Notify), 1);
        QCOMPARE(c.alerts().first().type, AlertType::H);
    }

    void aStoredReadingBecomesAnAlertAJournalLineAndANotification()
    {
        AlertCoordinator c(&m_journal, &m_readings, &m_notes, &m_clock);
        c.addSubject(sid(), settings());
        c.start();
        m_readings.set(53, m_clock.ms());
        c.dataChanged(sid());
        QCOMPARE(m_readings.lastSource, QString("libre.current"));
        QCOMPARE(c.alerts().size(), 1);
        QCOMPARE(c.alerts().first().type, AlertType::LL);
        QVERIFY(c.alerts().first().levelSince.isValid());     // the list carries the facts of the notice
        QVERIFY(c.alerts().first().hasLimit);
        QCOMPARE(c.alerts().first().limit, 54);
        QVERIFY(!c.alerts().first().acknowledged);
        QCOMPARE(m_notes.count(AlertEffect::Notify), 1);
        const QVector<AlertRecord> lines = loadAlertRecords(&m_journal);
        QCOMPARE(lines.size(), 1);
        QCOMPARE(lines.first().event, QString("active"));
    }

    void anEventForAnUnknownSubjectOrBeforeStartDoesNothing()
    {
        AlertCoordinator c(&m_journal, &m_readings, &m_notes, &m_clock);
        c.addSubject(sid(), settings());
        m_readings.set(53, m_clock.ms());
        c.dataChanged(sid());                       // not started yet
        c.start();                                   // start reads the newest reading itself
        QCOMPARE(c.alerts().size(), 1);
        c.dataChanged(sid(9));
        c.stalenessChanged(status(sid(9), 0, true, "source-unreachable"));
        QCOMPARE(c.alerts().size(), 1);
    }

    void missingDataComesFromTheClockAndTheStalenessAndNamesTheCause()
    {
        AlertCoordinator c(&m_journal, &m_readings, &m_notes, &m_clock);
        c.addSubject(sid(), settings());
        c.start();
        m_readings.set(100, m_clock.ms());
        c.dataChanged(sid());
        const qint64 last = m_clock.ms();
        m_clock.advanceS(200);
        c.stalenessChanged(status(sid(), last, true, "source-unreachable"));
        c.tick();                                    // 200 s: pending (limit 180 s)
        QVERIFY(c.nextWakeAt().isValid());
        QVERIFY(c.alerts().isEmpty());              // pending is not in the list: nobody has been told yet
        m_clock.advanceS(800);
        c.tick();                                    // 1000 s: past 15 minutes
        QCOMPARE(c.alerts().first().state, AlertState::Active);
        QCOMPARE(m_notes.seen.last().kind, AlertEffect::Notify);
        QCOMPARE(m_notes.seen.last().cause, QString("source-unreachable"));
        // a new reading ends it
        m_readings.set(100, m_clock.ms());
        c.dataChanged(sid());
        QVERIFY(c.alerts().isEmpty());
        QCOMPARE(m_notes.seen.last().kind, AlertEffect::Withdraw);
    }

    void aRestartRestoresTheStateAndNotifiesAgain()
    {
        {
            AlertCoordinator c(&m_journal, &m_readings, &m_notes, &m_clock);
            c.addSubject(sid(), settings());
            c.start();
            m_readings.set(53, m_clock.ms());
            c.dataChanged(sid());
        }
        m_notes.seen.clear();
        m_clock.advanceS(600);
        AlertCoordinator again(&m_journal, &m_readings, &m_notes, &m_clock);
        again.addSubject(sid(), settings());
        again.gapDetected(UtcTime::fromMSecsSinceEpoch(m_clock.ms() - 3600000), m_clock.now());
        again.start();
        QCOMPARE(again.alerts().size(), 1);
        QCOMPARE(again.alerts().first().state, AlertState::Active);
        QCOMPARE(m_notes.count(AlertEffect::Interruption), 1);
        QVERIFY(m_notes.count(AlertEffect::Notify) >= 1);
    }

    void aGapFoundAfterStartIsAnnouncedToo()
    {
        AlertCoordinator c(&m_journal, &m_readings, &m_notes, &m_clock);
        c.addSubject(sid(), settings());
        c.start();
        c.gapDetected(UtcTime::fromMSecsSinceEpoch(m_clock.ms() - 3600000), m_clock.now());
        QCOMPARE(m_notes.count(AlertEffect::Interruption), 1);
    }

    void aJournalThatIsBusyIsRetriedAndTheAlertDoesNotWait()
    {
        AlertCoordinator c(&m_journal, &m_readings, &m_notes, &m_clock);
        c.addSubject(sid(), settings());
        c.start();
        const QString path = m_journal.path();
        m_journal.close();                           // the journal cannot be written now
        m_readings.set(53, m_clock.ms());
        c.dataChanged(sid());
        QCOMPARE(m_notes.count(AlertEffect::Notify), 1);   // the alert is raised anyway (REQ-STO-15)
        QCOMPARE(c.pendingJournalLines(), 1);
        QVERIFY(m_journal.open(path));
        c.tick();
        QCOMPARE(c.pendingJournalLines(), 0);
        QCOMPARE(loadAlertRecords(&m_journal).size(), 1);
    }

    void acknowledgeAndSnoozeReturnTheEnginesAnswer()
    {
        AlertCoordinator c(&m_journal, &m_readings, &m_notes, &m_clock);
        c.addSubject(sid(), settings());
        c.start();
        QCOMPARE(c.acknowledge(sid(), AlertSlot::Low), ActionResult::NotActive);
        m_readings.set(53, m_clock.ms());
        c.dataChanged(sid());
        QCOMPARE(c.snooze(sid(), AlertSlot::Low, 15), ActionResult::NotSnoozable);
        QCOMPARE(c.acknowledge(sid(), AlertSlot::Low), ActionResult::Done);
        QCOMPARE(c.alerts().first().state, AlertState::Acknowledged);
        QCOMPARE(c.acknowledge(sid(9), AlertSlot::Low), ActionResult::UnknownSubject);
    }

    void aThresholdEditIsRefusedWrittenThenAppliedAndSurvivesARestart()
    {
        AlertCoordinator c(&m_journal, &m_readings, &m_notes, &m_clock);
        c.addSubject(sid(), settings());
        c.start();
        m_readings.set(66, m_clock.ms());
        c.dataChanged(sid());
        QCOMPARE(c.alerts().size(), 1);                                  // 66 is below L 70
        // looser than the global value: refused without confirmation, and nothing is written
        QCOMPARE(c.setThresholds(sid(), 60, 180, Confirmation()), EditResult::NeedsConfirmation);
        QCOMPARE(c.setThresholds(sid(), 60, 180, Confirmation::typed(61, 180)), EditResult::ConfirmationMismatch);
        QCOMPARE(loadThresholdChanges(&m_journal).size(), 0);
        QCOMPARE(c.thresholds(sid()).l, 70);
        // accepted: written, applied, and the newest reading is judged again at once
        QCOMPARE(c.setThresholds(sid(), 60, 180, Confirmation::typed(60, 180)), EditResult::Accepted);
        QCOMPARE(c.thresholds(sid()).l, 60);
        QCOMPARE(loadThresholdChanges(&m_journal).size(), 1);
        QCOMPARE(c.thresholdHistory(sid()).first().value, 60);
        m_clock.advanceS(60);
        m_readings.set(66, m_clock.ms());
        c.dataChanged(sid());                                            // the second confirming reading
        QVERIFY(c.alerts().isEmpty());

        // After a restart the Subject still has L 60: 62 raises nothing, 59 does, and the alert
        // line cites the threshold entry in force.
        AlertCoordinator again(&m_journal, &m_readings, &m_notes, &m_clock);
        again.addSubject(sid(), settings());
        again.start();
        QCOMPARE(again.thresholds(sid()).l, 60);
        m_clock.advanceS(60);
        m_readings.set(62, m_clock.ms());
        again.dataChanged(sid());
        QVERIFY(again.alerts().isEmpty());
        m_clock.advanceS(60);
        m_readings.set(59, m_clock.ms());
        again.dataChanged(sid());
        QCOMPARE(again.alerts().size(), 1);
        QCOMPARE(loadAlertRecords(&m_journal).last().thresholdsId, loadThresholdChanges(&m_journal).last().id);
    }

    // ---- silent mode ---------------------------------------------------------------------
    int silentFlags(AlertEffect::Kind kind) const
    {
        int n = 0;
        for (int i = 0; i < m_notes.seen.size(); ++i)
            n += m_notes.seen.at(i).kind == kind && m_notes.seen.at(i).silent;
        return n;
    }

    void silentModeGivesOneSilentUpdateAndSilentRemindersAndIsJournaled()
    {
        AlertCoordinator c(&m_journal, &m_readings, &m_notes, &m_clock);
        c.addSubject(sid(), settings());
        c.start();
        m_readings.set(53, m_clock.ms());
        c.dataChanged(sid());                                   // LL active at once
        QCOMPARE(silentFlags(AlertEffect::Notify), 0);
        m_notes.seen.clear();
        QVERIFY(c.setSilent(sid(), 60));
        QCOMPARE(m_notes.count(AlertEffect::Update), 1);
        QCOMPARE(silentFlags(AlertEffect::Update), 1);
        QVERIFY(c.isSilent(sid()));
        QVERIFY(c.silentUntil(sid()).isValid());
        QCOMPARE(c.silentUntil(sid()).msecsSinceEpoch(), m_clock.ms() + 3600 * 1000LL);
        // set again (longer): nothing changes for the ear, nothing is emitted
        m_notes.seen.clear();
        QVERIFY(c.setSilent(sid(), 240));
        QCOMPARE(m_notes.seen.size(), 0);
        QCOMPARE(c.silentUntil(sid()).msecsSinceEpoch(), m_clock.ms() + 4 * 3600 * 1000LL);
        // the reminders that follow are silent
        m_clock.advanceS(130);
        c.tick();
        QCOMPARE(m_notes.count(AlertEffect::Remind), 1);
        QCOMPARE(silentFlags(AlertEffect::Remind), 1);
        // two start lines in the journal
        QCOMPARE(loadSilentChanges(&m_journal).size(), 2);
    }

    void aTimedModeEndsByTheWakeUpWithItsJournalLineAndEmitsNothing()
    {
        AlertCoordinator c(&m_journal, &m_readings, &m_notes, &m_clock);
        c.addSubject(sid(), settings());
        c.start();
        m_readings.set(53, m_clock.ms());
        c.dataChanged(sid());
        const qint64 t0 = m_clock.ms();
        QVERIFY(c.setSilent(sid(), 1));
        m_notes.seen.clear();
        // the next wake-up is the earlier of the first reminder (120 s) and the end (60 s)
        QCOMPARE(c.nextWakeAt().msecsSinceEpoch(), t0 + 60 * 1000LL);
        m_readings.set(53, m_clock.ms() + 1000);                // keep the data fresh
        m_clock.advanceS(100);
        c.tick();
        QVERIFY(!c.isSilent(sid()));
        const QVector<SilentChange> lines = loadSilentChanges(&m_journal);
        QCOMPARE(lines.size(), 2);
        SilentChange end;
        for (int i = 0; i < lines.size(); ++i)
            if (lines.at(i).event == SilentEvent::end()) end = lines.at(i);
        QCOMPARE(end.time.msecsSinceEpoch(), t0 + 60 * 1000LL);   // stamped with the end, not the tick
        QCOMPARE(m_notes.count(AlertEffect::Update), 0);          // nothing at the end
        // the reminder due at 120 s sounds (not silent) once the mode is over
        m_clock.advanceS(30);
        c.tick();
        QCOMPARE(m_notes.count(AlertEffect::Remind), 1);
        QCOMPARE(silentFlags(AlertEffect::Remind), 0);
    }

    void theUserCanEndAModeAndRefusalsAreAnswered()
    {
        AlertCoordinator c(&m_journal, &m_readings, &m_notes, &m_clock);
        c.addSubject(sid(), settings());
        c.start();
        QVERIFY(!c.endSilent(sid()));                           // not silent
        QVERIFY(!c.setSilent(sid(9), 60));                      // unknown Subject
        QVERIFY(!c.setSilent(sid(), -2));                       // not a length
        QVERIFY(!c.setSilent(sid(), 400 * 24 * 60));            // more than a year
        QVERIFY(c.setSilent(sid(), 0));
        QVERIFY(c.silentOn(sid()));
        QVERIFY(!c.silentUntil(sid()).isValid());               // without limit: no end time
        QVERIFY(c.endSilent(sid()));
        QVERIFY(!c.isSilent(sid()));
        QCOMPARE(loadSilentChanges(&m_journal).size(), 2);
    }

    void untilTomorrowEndsAtALocalSixInTheMorning()
    {
        AlertCoordinator c(&m_journal, &m_readings, &m_notes, &m_clock);
        c.addSubject(sid(), settings());
        c.start();
        QVERIFY(c.setSilent(sid(), kSilentUntilTomorrow));
        const QDateTime end = QDateTime::fromMSecsSinceEpoch(c.silentUntil(sid()).msecsSinceEpoch(), Qt::LocalTime);
        QCOMPARE(end.time(), QTime(6, 0));
        const qint64 ahead = c.silentUntil(sid()).msecsSinceEpoch() - m_clock.ms();
        QVERIFY(ahead > 3600 * 1000LL && ahead <= 31 * 3600 * 1000LL);
    }

    void missingDataAndTheInterruptionNoticeAreNeverSilent()
    {
        AlertCoordinator c(&m_journal, &m_readings, &m_notes, &m_clock);
        c.addSubject(sid(), settings());
        c.start();
        QVERIFY(c.setSilent(sid(), 0));
        m_readings.set(100, m_clock.ms());
        c.dataChanged(sid());
        const qint64 last = m_clock.ms();
        m_clock.advanceS(1000);
        c.stalenessChanged(status(sid(), last, true, "source-unreachable"));
        c.tick();
        QCOMPARE(c.alerts().first().slot, AlertSlot::Missing);
        c.gapDetected(UtcTime::fromMSecsSinceEpoch(m_clock.ms() - 3600000), m_clock.now());
        QVERIFY(m_notes.count(AlertEffect::Notify) >= 1);
        QVERIFY(m_notes.count(AlertEffect::Interruption) == 1);
        QCOMPARE(silentFlags(AlertEffect::Notify), 0);
        QCOMPARE(silentFlags(AlertEffect::Interruption), 0);
    }

    void urgentAlertsAreStillImmediateAndNotSnoozableInSilentMode()
    {
        AlertCoordinator c(&m_journal, &m_readings, &m_notes, &m_clock);
        c.addSubject(sid(), settings());
        c.start();
        QVERIFY(c.setSilent(sid(), 0));
        m_readings.set(53, m_clock.ms());
        c.dataChanged(sid());
        QCOMPARE(m_notes.count(AlertEffect::Notify), 1);        // at once
        QCOMPARE(silentFlags(AlertEffect::Notify), 1);          // without feedback hint
        QCOMPARE(c.snooze(sid(), AlertSlot::Low, 15), ActionResult::NotSnoozable);
    }

    void aRestartKeepsTheModeAndNotifiesTheActiveAlertSilently()
    {
        {
            AlertCoordinator c(&m_journal, &m_readings, &m_notes, &m_clock);
            c.addSubject(sid(), settings());
            c.start();
            m_readings.set(53, m_clock.ms());
            c.dataChanged(sid());
            QVERIFY(c.setSilent(sid(), 0));
        }
        m_notes.seen.clear();
        m_clock.advanceS(600);
        AlertCoordinator again(&m_journal, &m_readings, &m_notes, &m_clock);
        again.addSubject(sid(), settings());
        again.start();
        QVERIFY(again.isSilent(sid()));
        QCOMPARE(m_notes.count(AlertEffect::Notify), 1);
        QCOMPARE(silentFlags(AlertEffect::Notify), 1);
        QCOMPARE(m_notes.count(AlertEffect::Update), 0);        // restoring is not a start of the mode
    }

    void aModeThatRanOutWhileTheServiceWasOffEndsAtStartAndTheAlertSounds()
    {
        {
            AlertCoordinator c(&m_journal, &m_readings, &m_notes, &m_clock);
            c.addSubject(sid(), settings());
            c.start();
            m_readings.set(53, m_clock.ms());
            c.dataChanged(sid());
            QVERIFY(c.setSilent(sid(), 60));
        }
        m_notes.seen.clear();
        m_clock.advanceS(2 * 3600);
        AlertCoordinator again(&m_journal, &m_readings, &m_notes, &m_clock);
        again.addSubject(sid(), settings());
        again.start();
        QVERIFY(!again.isSilent(sid()));
        QCOMPARE(silentFlags(AlertEffect::Notify), 0);
        bool expired = false;
        const QVector<SilentChange> lines = loadSilentChanges(&m_journal);
        for (int i = 0; i < lines.size(); ++i)
            expired = expired || lines.at(i).event == SilentEvent::expired();
        QVERIFY(expired);
    }

    void aThresholdThatCannotBeWrittenIsNotApplied()
    {
        AlertCoordinator c(&m_journal, &m_readings, &m_notes, &m_clock);
        c.addSubject(sid(), settings());
        c.start();
        m_journal.close();
        QVERIFY(c.setThresholds(sid(), 75, 180, Confirmation()) != EditResult::Accepted);
        QCOMPARE(c.thresholds(sid()).l, 70);
    }

    // The D-Bus layer needs the book's whole answer, and two words of
    // its own for the cases the book never sees.
    void theWholeAnswerOfTheBookIsAvailableAndTwoCasesHaveTheirOwnWords()
    {
        AlertCoordinator c(&m_journal, &m_readings, &m_notes, &m_clock);
        c.addSubject(sid(), settings());
        c.start();
        EditOutcome out;
        QCOMPARE(c.setThresholds(sid(), 60, 180, Confirmation::typed(61, -1), &out), EditResult::ConfirmationMismatch);
        QCOMPARE(out.result, EditResult::ConfirmationMismatch);
        QVERIFY(out.hasOffending && out.offending == Boundary::L);
        QCOMPARE(out.globalL, 70);
        QCOMPARE(c.setThresholds(sid(), 190, 180, Confirmation(), &out), EditResult::Inconsistent);
        QCOMPARE(out.lMin, 55);
        QCOMPARE(out.lMax, 179);
        QCOMPARE(c.setThresholds(sid(), 75, 180, Confirmation(), 0), EditResult::Accepted);   // details may be left out
        // A Subject the coordinator does not follow.
        QCOMPARE(c.setThresholds(sid(9), 75, 180, Confirmation(), &out), EditResult::UnknownSubject);
        QCOMPARE(out.result, EditResult::UnknownSubject);
        QCOMPARE(editResultName(EditResult::UnknownSubject), QString("unknown-subject"));
        // A journal that cannot take the line: its own word, and nothing changed.
        m_journal.close();
        QCOMPARE(c.setThresholds(sid(), 77, 180, Confirmation(), &out), EditResult::NotSaved);
        QCOMPARE(out.result, EditResult::NotSaved);
        QVERIFY(out.changes.isEmpty());
        QCOMPARE(c.thresholds(sid()).l, 75);
        QCOMPARE(editResultName(EditResult::NotSaved), QString("not-saved"));
    }
};

QTEST_APPLESS_MAIN(TstAlertCoordinator)
#include "tst_alertcoordinator.moc"
