// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Michele Brami
//
// The journal kinds `alert` and `threshold` and the rebuild of the engine's
// state from them (for the pattern). The state
// of every slot is written as the engine goes, read back by a fresh engine, and must be the same
// slot in the same state, in every state a slot can be in. Every value is synthetic.
#include "alerttest.h"

#include "alerts/alertjournal.h"

#include <QJsonDocument>
#include <QSqlDatabase>
#include <QSqlError>
#include <QSqlQuery>
#include <QTemporaryDir>
#include <QtTest>

using namespace lm;
using namespace lm::testing;

Q_DECLARE_METATYPE(QStringList)

namespace {

// Writes a journal line straight into the database file, past the checks of Journal::append:
// the way a line the reader cannot use can still reach the journal (a damaged file, or a line
// written by another version of the application). The fold must skip it and keep what it can.
bool writeUncheckedLine(const QString &journalPath, const JournalEntry &e)
{
    bool ok = false;
    {
        QSqlDatabase db = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), QStringLiteral("unchecked"));
        db.setDatabaseName(journalPath);
        if (db.open()) {
            QSqlQuery q(db);
            q.prepare(QStringLiteral("INSERT INTO journal(id, kind, wall_time, extra) VALUES(?,?,?,?)"));
            q.addBindValue(e.id.toString().mid(1, 36));
            q.addBindValue(e.kind);
            q.addBindValue(qlonglong(e.wallTime.msecsSinceEpoch()));
            q.addBindValue(QString::fromUtf8(QJsonDocument(e.extra).toJson(QJsonDocument::Compact)));
            ok = q.exec();
            if (!ok)
                qWarning("unchecked line: %s", qPrintable(q.lastError().text()));
        }
        db.close();
    }
    QSqlDatabase::removeDatabase(QStringLiteral("unchecked"));
    return ok;
}

Journal *memoryJournal()
{
    Journal *j = new Journal;
    if (!j->open(QStringLiteral(":memory:")))
        qFatal("cannot open the in-memory journal");
    return j;
}

// The records of a rig written to a journal and read back: the path a real restart takes.
QVector<AlertRecord> throughTheJournal(const QVector<AlertRecord> &records)
{
    QScopedPointer<Journal> j(memoryJournal());
    for (int i = 0; i < records.size(); ++i)
        if (!j->append(toJournalEntry(records.at(i))))
            qFatal("journal refused a record: %s", qPrintable(j->lastError()));
    return loadAlertRecords(j.data());
}

} // namespace

class TstAlertJournal : public QObject
{
    Q_OBJECT

private slots:
    void initTestCase() { qRegisterMetaType<QStringList>(); }

    // ---- the restore, in every state ----------------------------------------------------------
    void everyStateComesBackAsTheSameSlot_data()
    {
        QTest::addColumn<QString>("script");
        QTest::addColumn<QString>("low");
        QTest::addColumn<QString>("high");
        QTest::addColumn<QString>("missing");

        // The scripts end in the state named by the columns; "-" is a normal slot.
        QTest::newRow("all normal") << "rr 0 300 60 100\n" << "-" << "-" << "-";
        QTest::newRow("L pending reverted to normal") << "cfg 1 5 15 15\nrr 0 60 60 100\nr 120 69\nr 180 100\n"
            << "-" << "-" << "-";
        QTest::newRow("cleared") << "rr 0 60 60 100\nr 120 69\nr 180 80\nr 240 80\n" << "-" << "-" << "-";
        QTest::newRow("H pending") << "rr 0 60 60 100\nr 120 181\nr 180 181\n"
            << "-" << "high:H:pending" << "-";
        QTest::newRow("L active") << "rr 0 60 60 100\nr 120 69\n" << "low:L:active" << "-" << "-";
        QTest::newRow("LL active") << "rr 0 60 60 100\nr 120 53\n" << "low:LL:active" << "-" << "-";
        QTest::newRow("HH active") << "rr 0 60 60 100\nr 120 260\n" << "-" << "high:HH:active" << "-";
        QTest::newRow("L acknowledged") << "rr 0 60 60 100\nr 120 69\nack 130 low\n"
            << "low:L:acknowledged" << "-" << "-";
        QTest::newRow("LL acknowledged") << "rr 0 60 60 100\nr 120 53\nack 130 low\n"
            << "low:LL:acknowledged" << "-" << "-";
        QTest::newRow("L snoozed") << "rr 0 60 60 100\nr 120 69\nsnz 130 low 30\n"
            << "low:L:snoozed" << "-" << "-";
        QTest::newRow("L active and H active") << "cfg 1 0 0 15\nrr 0 60 60 100\nr 120 69\nr 180 190\n"
            << "low:L:active" << "high:H:active" << "-";
        QTest::newRow("missing pending") << "r 0 100\nst 10 source-unreachable -\nt 300\n"
            << "-" << "-" << "missing:MISSING:pending";
        QTest::newRow("missing active") << "r 0 100\nst 10 source-unreachable -\nt 1000\n"
            << "-" << "-" << "missing:MISSING:active";
        QTest::newRow("missing acknowledged") << "r 0 100\nst 10 source-unreachable -\nt 1000\nack 1000 missing\n"
            << "-" << "-" << "missing:MISSING:acknowledged";
        QTest::newRow("missing snoozed") << "r 0 100\nt 1000\nsnz 1000 missing 60\n"
            << "-" << "-" << "missing:MISSING:snoozed";
        QTest::newRow("missing cleared by a reading") << "r 0 100\nt 1000\nr 1100 100\n" << "-" << "-" << "-";
        QTest::newRow("missing cleared by a warm-up") << "r 0 100\nst 10 x -\nt 1000\nst 1100 sensor-warm-up -\n"
            << "-" << "-" << "-";
    }
    void everyStateComesBackAsTheSameSlot()
    {
        QFETCH(QString, script); QFETCH(QString, low); QFETCH(QString, high); QFETCH(QString, missing);
        AlertRig before;
        before.run(script);
        QCOMPARE(before.shape(AlertSlot::Low), low);
        QCOMPARE(before.shape(AlertSlot::High), high);
        QCOMPARE(before.shape(AlertSlot::Missing), missing);

        // A fresh engine after a restart, with the same Subject configured, then the journal.
        AlertRig after;
        const QVector<AlertRecord> loaded = throughTheJournal(before.records());
        after.engine().restore(loaded, alertTime(100000));
        QCOMPARE(after.shape(AlertSlot::Low), low);
        QCOMPARE(after.shape(AlertSlot::High), high);
        QCOMPARE(after.shape(AlertSlot::Missing), missing);

        // The occurrence keeps its identity across the restart.
        const AlertSlot allSlots[] = { AlertSlot::Low, AlertSlot::High, AlertSlot::Missing };
        for (int i = 0; i < 3; ++i)
            QCOMPARE(after.view(allSlots[i]).episode, before.view(allSlots[i]).episode);
    }

    void anActiveAlertStaysActiveIsNotifiedAgainAndKeepsRemindingFromTheStart()
    {
        AlertRig before;
        before.run("rr 0 60 60 100\nr 120 69\n");
        AlertRig after;
        const AlertEffects fx = after.engine().restore(throughTheJournal(before.records()), alertTime(1000));
        QCOMPARE(fx.size(), 1);
        QCOMPARE(fx.first().kind, AlertEffect::Notify);
        QCOMPARE(fx.first().type, AlertType::L);
        // The reminders start from the restart, not from the first notification: 1000 + 15 min.
        const AlertEffects later = after.engine().tick(alertTime(2000));
        bool reminded = false;
        for (int i = 0; i < later.size(); ++i)
            reminded = reminded || (later.at(i).kind == AlertEffect::Remind && later.at(i).type == AlertType::L);
        QVERIFY(reminded);
    }

    void anAcknowledgedUrgentAlertKeepsItsReAlertTime()
    {
        AlertRig before;
        before.run("rr 0 60 60 100\nr 120 53\nack 130 low\n");      // re-alert at 130 + 300 = 430
        AlertRig after;
        after.engine().restore(throughTheJournal(before.records()), alertTime(300));
        after.reading(320, 53);                                     // the first reading after the restart
        after.take();
        after.tick(430);
        const QStringList got = after.take();
        QVERIFY2(got.contains("J 430 low LL active re-alerted"), qPrintable(got.join('\n')));
    }

    void aSnoozeKeepsItsEndTime()
    {
        AlertRig before;
        before.run("rr 0 60 60 100\nr 120 69\nsnz 130 low 30\n");   // ends at 130 + 1800 = 1930
        AlertRig after;
        after.engine().restore(throughTheJournal(before.records()), alertTime(500));
        after.reading(520, 69);
        after.take();
        after.tick(1929);
        QCOMPARE(after.shape(AlertSlot::Low), QString("low:L:snoozed"));
        after.tick(1930);
        QCOMPARE(after.shape(AlertSlot::Low), QString("low:L:active"));
    }

    void aPendingAlertKeepsWhenItBeganSoThePersistenceIsNotRestarted()
    {
        AlertRig before;
        before.run("rr 0 60 60 100\nr 120 181\n");                  // since 120, due 1020
        AlertRig after;
        after.engine().restore(throughTheJournal(before.records()), alertTime(1500));
        // The first reading after the restart, still beyond: the 15 minutes are long over.
        after.reading(1520, 190);
        QCOMPARE(after.shape(AlertSlot::High), QString("high:H:active"));
    }

    void aPendingAlertWhoseConditionIsGoneDoesNotComeBack()
    {
        AlertRig before;
        before.run("rr 0 60 60 100\nr 120 181\n");
        AlertRig after;
        after.engine().restore(throughTheJournal(before.records()), alertTime(1500));
        after.reading(1520, 150);                                   // back in range
        QCOMPARE(after.shape(AlertSlot::High), QString("-"));
    }

    void aRestoredMissingAlertEndsWithTheFirstReading()
    {
        AlertRig before;
        before.run("r 0 100\nst 10 source-unreachable -\nt 1000\n");
        AlertRig after;
        after.engine().restore(throughTheJournal(before.records()), alertTime(2000));
        QCOMPARE(after.shape(AlertSlot::Missing), QString("missing:MISSING:active"));
        after.reading(2100, 100);
        QCOMPARE(after.shape(AlertSlot::Missing), QString("-"));
    }

    void theLastRecordWinsEvenWhenWallTimesAreEqualBecauseOfTheSequenceNumber()
    {
        // Two transitions in the same millisecond (a pending alert made active by the same reading
        // would do it). The fold must take the one with the higher sequence number whatever the
        // random ids say.
        AlertRig before;
        before.run("rr 0 60 60 100\nr 120 69\nack 120 low\n");     // active and acknowledged at 120
        QVector<AlertRecord> records = before.records();
        QCOMPARE(records.size(), 2);
        QCOMPARE(records.at(0).time, records.at(1).time);
        for (int attempt = 0; attempt < 2; ++attempt) {
            // Try both orders of the ids by swapping them, and both reading orders.
            std::swap(records[0].id, records[1].id);
            QVector<AlertRecord> shuffled = records;
            std::reverse(shuffled.begin(), shuffled.end());
            AlertRig after;
            after.engine().restore(throughTheJournal(shuffled), alertTime(500));
            QCOMPARE(after.shape(AlertSlot::Low), QString("low:L:acknowledged"));
        }
    }

    void theSequenceContinuesAfterARestart()
    {
        AlertRig before;
        before.run("rr 0 60 60 100\nr 120 69\n");
        qint64 maxSeq = 0;
        for (int i = 0; i < before.records().size(); ++i)
            maxSeq = qMax(maxSeq, before.records().at(i).seq);
        AlertRig after;
        after.engine().restore(throughTheJournal(before.records()), alertTime(500));
        after.run("r 520 100\nr 580 100\nr 640 80\nr 700 80\n");    // clears the L alert
        QVERIFY(!after.records().isEmpty());
        QVERIFY(after.records().first().seq > maxSeq);
    }

    void aRestoreCarriesTheSameFactsAsBeforeTheRestart()
    {
        // An active alert that rose and was acknowledged once: the texts must read the
        // same after a restart (levelSince, episodeSince, the limit, the "acknowledged" memory).
        AlertRig before;
        before.run("rr 0 60 60 100\nr 120 69\nack 130 low\nr 180 53\nr 240 53\n");
        const AlertView was = before.view(AlertSlot::Low);
        QCOMPARE(was.type, AlertType::LL);
        AlertRig after;
        const AlertEffects fx = after.engine().restore(throughTheJournal(before.records()), alertTime(1000));
        QCOMPARE(fx.size(), 1);
        QCOMPARE(fx.first().kind, AlertEffect::Notify);
        QCOMPARE(fx.first().levelSince, was.levelSince);
        QCOMPARE(fx.first().episodeSince, was.episodeSince);
        QVERIFY(fx.first().levelSince.msecsSinceEpoch() != fx.first().episodeSince.msecsSinceEpoch());
        QCOMPARE(fx.first().limit, 54);
        // the acknowledgement of the episode is remembered: clearing it leaves no "ended" entry
        after.reading(1020, 100);
        after.reading(1080, 100);
        QVERIFY(!after.lastEffect(AlertEffect::Withdraw, AlertSlot::Low).neverAcknowledged);
        QVERIFY(after.lastEffect(AlertEffect::Withdraw, AlertSlot::Low).kind == AlertEffect::Withdraw);
    }

    void aRestoredMissingAlertKeepsTheNewestReadingItWasJudgedOn()
    {
        AlertRig before;
        before.run("r 0 100\nst 10 source-unreachable -\nt 1000\n");
        AlertRig after;
        const AlertEffects fx = after.engine().restore(throughTheJournal(before.records()), alertTime(2000));
        QCOMPARE(fx.size(), 1);
        QCOMPARE(fx.first().newestReadingAt.msecsSinceEpoch(), kAlertTestStartMs);
        QCOMPARE(fx.first().cause, QString("source-unreachable"));
    }

    void anAlertEntryHoldsTheFactsAsTimesAndAFlagAndNothingElse()
    {
        AlertRig rig;
        rig.run("rr 0 60 60 100\nr 120 69\nack 130 low\n");
        const JournalEntry e = toJournalEntry(rig.records().last());
        QVERIFY(e.extra.contains(AlertKey::levelSince()));
        QVERIFY(e.extra.contains(AlertKey::episodeSince()));
        QCOMPARE(e.extra.value(AlertKey::acknowledged()).toBool(), true);
        QScopedPointer<Journal> j(memoryJournal());
        QVERIFY(j->append(e));
        JournalEntry bad = e;
        bad.id = QUuid::createUuid();
        bad.extra.insert(AlertKey::acknowledged(), QStringLiteral("yes"));
        QVERIFY(!j->append(bad));
    }

    void aSubjectThatIsNoLongerConfiguredIsSkipped()
    {
        AlertRig before;
        before.run("rr 0 60 60 100\nr 120 69\n");
        QVector<AlertRecord> records = before.records();
        records[0].subject = alertTestSubject(77);                   // not registered in the new engine
        AlertRig after;
        const AlertEffects fx = after.engine().restore(throughTheJournal(records), alertTime(500));
        QVERIFY(fx.isEmpty());
        QCOMPARE(after.shape(AlertSlot::Low), QString("-"));
    }

    void aLineThatCannotBeReadBackIsSkippedAndTheEarlierOneStands()
    {
        // A damaged line (written past the checks of append) must not stop the fold.
        AlertRig before;
        before.run("rr 0 60 60 100\nr 120 69\nack 130 low\n");
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        QScopedPointer<Journal> j(new Journal);
        QVERIFY(j->open(dir.path() + "/journal.db"));
        QCOMPARE(before.records().size(), 2);
        QVERIFY(j->append(toJournalEntry(before.records().at(0))));
        JournalEntry bad = toJournalEntry(before.records().at(1));
        bad.extra.insert(AlertKey::state(), QStringLiteral("exploded"));
        QVERIFY(!j->append(bad));                           // append refuses it
        QVERIFY(writeUncheckedLine(j->path(), bad));
        const QVector<AlertRecord> loaded = loadAlertRecords(j.data());
        QCOMPARE(loaded.size(), 1);
        AlertRig after;
        after.engine().restore(loaded, alertTime(500));
        QCOMPARE(after.shape(AlertSlot::Low), QString("low:L:active"));   // the acknowledgement is lost, the alert is kept
    }

    // ---- what is written -----------------------------------------------------------------------
    void theJournalRefusesAnAlertEntryItCouldNotReadBack_data()
    {
        QTest::addColumn<QString>("key");
        QTest::addColumn<QString>("value");
        QTest::newRow("unknown state") << "state" << "exploded";
        QTest::newRow("unknown previous state") << "from" << "";
        QTest::newRow("unknown slot") << "slot" << "sideways";
        QTest::newRow("alert not of the slot") << "alert" << "HH";           // slot is low
        QTest::newRow("unknown alert") << "alert" << "XL";
        QTest::newRow("no event") << "event" << "";
        QTest::newRow("episode not a UUID") << "episode" << "not-a-uuid";
        QTest::newRow("subject not a UUID") << "subject" << "";
        QTest::newRow("a time that is no time") << "since" << "yesterday";
        QTest::newRow("thresholds id not a UUID") << "thresholds" << "x";
    }
    void theJournalRefusesAnAlertEntryItCouldNotReadBack()
    {
        QFETCH(QString, key); QFETCH(QString, value);
        AlertRig rig;
        rig.run("rr 0 60 60 100\nr 120 69\n");
        JournalEntry e = toJournalEntry(rig.records().first());
        QScopedPointer<Journal> j(memoryJournal());
        QVERIFY(j->append(e));                                      // as it stands it is fine
        e.id = QUuid::createUuid();
        e.extra.insert(key, value);
        QVERIFY2(!j->append(e), qPrintable(key));
    }

    void aSequenceNumberMustBeAWholeNonNegativeNumber()
    {
        AlertRig rig;
        rig.run("rr 0 60 60 100\nr 120 69\n");
        QScopedPointer<Journal> j(memoryJournal());
        JournalEntry e = toJournalEntry(rig.records().first());
        e.extra.insert(AlertKey::seq(), -1.0);
        QVERIFY(!j->append(e));
        e.extra.insert(AlertKey::seq(), 1.5);
        QVERIFY(!j->append(e));
    }

    void anAlertEntryCarriesNoGlucoseValueNoNameAndNoComment()
    {
        AlertRig rig;
        rig.run("rr 0 60 60 100\nr 120 53\nack 130 low\n");
        for (int i = 0; i < rig.records().size(); ++i) {
            const JournalEntry e = toJournalEntry(rig.records().at(i));
            QSet<QString> keys = QSet<QString>::fromList(e.extra.keys());
            QSet<QString> allowed;
            allowed << "subject" << "slot" << "alert" << "state" << "from" << "event" << "episode" << "seq"
                    << "measured_at" << "since" << "until" << "thresholds" << "reason"
                    << "level_since" << "episode_since" << "acknowledged_in_episode";
            QVERIFY2(QSet<QString>(keys).subtract(allowed).isEmpty(), qPrintable(QStringList(keys.toList()).join(',')));
            QVERIFY(e.cause.isNull());
        }
    }

    void theThresholdsInForceAreCitedByTheAlertEntry()
    {
        // REQ-ALR-05: one lookup answers "what were the thresholds when this fired".
        ThresholdBook book;
        const QUuid subject = alertTestSubject();
        const EditOutcome o = book.edit(subject, 60, 180, Confirmation::typed(60, 180), alertTime(10));
        QCOMPARE(o.result, EditResult::Accepted);
        AlertRig rig;
        SubjectAlertConfig c;
        c.thresholds = book.thresholdsFor(subject);
        c.thresholdsId = book.idInForce(subject);
        rig.configure(1, c);
        rig.run("r 60 100\nr 120 59\n");                            // below the Subject's L of 60
        QCOMPARE(rig.shape(AlertSlot::Low), QString("low:L:active"));
        QCOMPARE(rig.records().last().thresholdsId, book.idInForce(subject));
        const QVector<ThresholdChange> h = book.history(subject);
        QCOMPARE(h.last().id, rig.records().last().thresholdsId);
        QCOMPARE(h.last().value, 60);
        // and it survives the journal
        QCOMPARE(throughTheJournal(rig.records()).last().thresholdsId, h.last().id);
    }

    // ---- the threshold history through the journal --------------------------------------------
    void theThresholdHistoryIsRebuiltFromTheJournal()
    {
        const QUuid a = alertTestSubject(1), b = alertTestSubject(2);
        ThresholdBook book;
        book.edit(a, 60, 180, Confirmation::typed(60, 180), alertTime(10));
        book.edit(a, 65, 190, Confirmation::typed(65, 190), alertTime(20));
        book.edit(b, 80, 150, Confirmation(), alertTime(30));

        QScopedPointer<Journal> j(memoryJournal());
        QVERIFY(writeThresholdChanges(j.data(), book.history(a)));
        QVERIFY(writeThresholdChanges(j.data(), book.history(b)));

        ThresholdBook restored;
        restored.restore(loadThresholdChanges(j.data()));
        QCOMPARE(restored.thresholdsFor(a), book.thresholdsFor(a));
        QCOMPARE(restored.thresholdsFor(b), book.thresholdsFor(b));
        QCOMPARE(restored.idInForce(a), book.idInForce(a));
        QCOMPARE(restored.history(a).size(), 3);
        QCOMPARE(restored.history(a).first().value, 60);
        QCOMPARE(restored.history(a).first().previous, 70);
    }

    // the note on a threshold change is gone. A new entry has no "comment" key, the
    // journal refuses one that brings it, and an entry written by an older version that still holds a
    // comment is read back as before (its numbers and time count, the comment is not read).
    void aThresholdEntryCarriesNoCommentAndAnOldOneWithACommentStaysReadable()
    {
        const QUuid a = alertTestSubject();
        ThresholdBook book;
        book.edit(a, 60, 180, Confirmation::typed(60, 180), alertTime(10));
        const JournalEntry fresh = toJournalEntry(book.history(a).first());
        QVERIFY(!fresh.extra.contains(QStringLiteral("comment")));
        QSet<QString> keys = QSet<QString>::fromList(fresh.extra.keys());
        QSet<QString> allowed;
        allowed << "subject" << "boundary" << "value" << "previous";
        QCOMPARE(keys, allowed);

        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        QScopedPointer<Journal> j(new Journal);
        QVERIFY(j->open(dir.path() + "/journal.db"));
        JournalEntry old = fresh;
        old.extra.insert(QStringLiteral("comment"), QStringLiteral("synthetic old note"));
        QVERIFY(!j->append(old));                           // nothing new may carry one
        QVERIFY(writeUncheckedLine(j->path(), old));        // as an older version wrote it
        const QVector<ThresholdChange> loaded = loadThresholdChanges(j.data());
        QCOMPARE(loaded.size(), 1);
        QCOMPARE(loaded.first().id, fresh.id);
        QCOMPARE(loaded.first().value, 60);
        QCOMPARE(loaded.first().previous, 70);
        QCOMPARE(loaded.first().time, alertTime(10));
        ThresholdBook restored;
        restored.restore(loaded);
        QCOMPARE(restored.thresholdsFor(a).l, 60);
    }

    void theJournalRefusesAThresholdEntryItCouldNotReadBack()
    {
        ThresholdBook book;
        book.edit(alertTestSubject(), 60, 180, Confirmation::typed(60, 180), alertTime(10));
        const JournalEntry good = toJournalEntry(book.history(alertTestSubject()).first());
        QScopedPointer<Journal> j(memoryJournal());
        QVERIFY(j->append(good));
        // "comment" with any value is refused (here a number, as before).
        const char *keys[] = { "boundary", "value", "previous", "comment", "subject" };
        const QJsonValue bad[] = { QJsonValue(QStringLiteral("X")), QJsonValue(0), QJsonValue(-5),
                                   QJsonValue(5), QJsonValue(QStringLiteral("nobody")) };
        for (int i = 0; i < 5; ++i) {
            JournalEntry e = good;
            e.id = QUuid::createUuid();
            e.extra.insert(QLatin1String(keys[i]), bad[i]);
            QVERIFY2(!j->append(e), keys[i]);
        }
    }

    void theLastEntryOfABoundaryIsTheOneInForceWhateverTheReadingOrder()
    {
        const QUuid a = alertTestSubject();
        ThresholdBook book;
        book.edit(a, 60, 180, Confirmation::typed(60, 180), alertTime(10));
        book.edit(a, 62, 180, Confirmation::typed(62, 180), alertTime(20));
        book.edit(a, 64, 180, Confirmation::typed(64, 180), alertTime(30));
        QVector<ThresholdChange> h = book.history(a);
        std::reverse(h.begin(), h.end());
        ThresholdBook r;
        r.restore(h);
        QCOMPARE(r.thresholdsFor(a).l, 64);
    }
};

QTEST_APPLESS_MAIN(TstAlertJournal)
#include "tst_alertjournal.moc"
