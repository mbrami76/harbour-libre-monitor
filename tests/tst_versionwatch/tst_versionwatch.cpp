// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Michele Brami

// Tests of the service's version stamp check: the same version,
// another version, a missing stamp, a doubtful read (one journal entry per episode, never a stop),
// what counts as a version, and the loop guard of 10 minutes, also across a restart (through the
// settings) and with a wall clock set back. A fake stamp reader, the in-memory journal and a hand
// moved clock; no file, no process, no bus.

#include "service/journallog.h"
#include "service/versionwatch.h"
#include "servicetest.h"

#include <QHash>
#include <QtTest>

using namespace lm;
using namespace lm::testing;

namespace {

class ScriptedStamp : public VersionStampReader
{
public:
    StampRead read() { ++reads; return next; }
    StampRead next = StampRead::present(QStringLiteral("0.1-3\n"));
    int reads = 0;
};

class MemorySettings : public ServiceSettingsStore
{
public:
    QString value(const QString &key) const { return values.value(key); }
    void setValue(const QString &key, const QString &value) { values.insert(key, value); }
    QHash<QString, QString> values;
};

struct WatchRig
{
    WatchRig() : watch(&stamp, QStringLiteral("0.1-3"), &journal, &clock, &settings) {}
    ScriptedStamp stamp;
    FakeJournal journal;
    FakeServiceClock clock;
    MemorySettings settings;
    VersionWatch watch;

    int notes(const QString &result) const
    {
        int n = 0;
        for (int i = 0; i < journal.entries.size(); ++i)
            if (journal.entries.at(i).kind == JournalKind::versionCheck()
                && journal.entries.at(i).extra.value(VersionCheckKey::result()).toString() == result)
                ++n;
        return n;
    }
};

} // namespace

class TstVersionWatch : public QObject
{
    Q_OBJECT

private slots:
    void theSameVersionKeepsRunningAndWritesNothing();
    void anotherVersionAsksForAnUpgradeStop();
    void anOlderVersionIsAnotherVersionToo();
    void aMissingStampAsksForARemovalStop();
    void aDoubtfulReadNeverStopsAndIsNotedOncePerEpisode();
    void whatLooksLikeAVersion();
    void aSecondRestartWithinTenMinutesIsRefusedAndNotedOnce();
    void theGuardHoldsAcrossARestartThroughTheSettings();
    void aClockSetBackStillCountsAsRecentWithinTheGuard();
    void withoutSettingsTheGuardLastsAsLongAsTheProcess();
    void aFailingJournalChangesNothing();
    void theRealJournalTakesAVersionCheckEntry();
};

void TstVersionWatch::theSameVersionKeepsRunningAndWritesNothing()
{
    WatchRig r;
    for (int i = 0; i < 5; ++i) {
        QCOMPARE(r.watch.check(), VersionAction::None);
        r.clock.advanceS(60);
    }
    QCOMPARE(r.stamp.reads, 5);
    QVERIFY(r.journal.entries.isEmpty());
    QCOMPARE(r.watch.stampVersion(), QStringLiteral("0.1-3"));     // the trailing newline is not part of it
    QVERIFY(r.settings.values.isEmpty());
}

void TstVersionWatch::anotherVersionAsksForAnUpgradeStop()
{
    WatchRig r;
    r.stamp.next = StampRead::present(QStringLiteral("0.1-4\n"));
    QCOMPARE(r.watch.check(), VersionAction::StopUpgraded);
    QCOMPARE(r.watch.stampVersion(), QStringLiteral("0.1-4"));
    QVERIFY(r.journal.entries.isEmpty());       // the stop record says it; no separate entry
    // Until the caller notes the restart, asking again gives the same answer (it decides, not acts).
    QCOMPARE(r.watch.check(), VersionAction::StopUpgraded);
    r.watch.noteSelfRestart();
    QCOMPARE(UtcTime::fromIso(r.settings.value(VersionWatch::restartKey())).msecsSinceEpoch(),
             r.clock.now().msecsSinceEpoch());
}

void TstVersionWatch::anOlderVersionIsAnotherVersionToo()
{
    // A downgrade (rpm --oldpackage) also installs another binary; the stamp is what matters.
    WatchRig r;
    r.stamp.next = StampRead::present(QStringLiteral("0.1-2"));
    QCOMPARE(r.watch.check(), VersionAction::StopUpgraded);
}

void TstVersionWatch::aMissingStampAsksForARemovalStop()
{
    WatchRig r;
    QCOMPARE(r.watch.check(), VersionAction::None);
    r.stamp.next = StampRead::missing();
    QCOMPARE(r.watch.check(), VersionAction::StopRemoved);
    QVERIFY(r.watch.stampVersion().isEmpty());
    QVERIFY(r.journal.entries.isEmpty());
    QVERIFY(r.settings.values.isEmpty());       // a removal is no self-restart
}

void TstVersionWatch::aDoubtfulReadNeverStopsAndIsNotedOncePerEpisode()
{
    WatchRig r;
    r.stamp.next = StampRead::unreadable();
    for (int i = 0; i < 4; ++i) {
        QCOMPARE(r.watch.check(), VersionAction::None);
        r.clock.advanceS(60);
    }
    QCOMPARE(r.notes(VersionCheckResult::stampUnreadable()), 1);
    // Other doubtful shapes in the same episode: still no stop and no new entry.
    r.stamp.next = StampRead::present(QString());
    QCOMPARE(r.watch.check(), VersionAction::None);
    r.stamp.next = StampRead::present(QStringLiteral("0.1-4\n0.1-5\n"));
    QCOMPARE(r.watch.check(), VersionAction::None);
    QCOMPARE(r.notes(VersionCheckResult::stampUnreadable()), 1);
    const JournalEntry e = r.journal.last(JournalKind::versionCheck());
    QCOMPARE(e.extra.value(VersionCheckKey::appVersion()).toString(), QStringLiteral("0.1-3"));
    QVERIFY(!e.extra.contains(VersionCheckKey::stamp()));       // nothing of the odd content is written
    QVERIFY(e.wallTime.isValid());
    // A good read ends the episode; the next doubtful read is noted again.
    r.stamp.next = StampRead::present(QStringLiteral("0.1-3"));
    QCOMPARE(r.watch.check(), VersionAction::None);
    r.stamp.next = StampRead::unreadable();
    QCOMPARE(r.watch.check(), VersionAction::None);
    QCOMPARE(r.notes(VersionCheckResult::stampUnreadable()), 2);
}

void TstVersionWatch::whatLooksLikeAVersion()
{
    QVERIFY(VersionWatch::looksLikeVersion(QStringLiteral("0.1-3")));
    QVERIFY(VersionWatch::looksLikeVersion(QStringLiteral("0.1-3\n")));
    QVERIFY(VersionWatch::looksLikeVersion(QStringLiteral("  1.2.3+git4~rc1-10_x \n")));
    QVERIFY(!VersionWatch::looksLikeVersion(QString()));
    QVERIFY(!VersionWatch::looksLikeVersion(QStringLiteral("\n")));
    QVERIFY(!VersionWatch::looksLikeVersion(QStringLiteral("0.1-3\n0.1-4")));
    QVERIFY(!VersionWatch::looksLikeVersion(QStringLiteral("0.1 3")));
    QVERIFY(!VersionWatch::looksLikeVersion(QStringLiteral("0.1-3;rm")));
    QVERIFY(!VersionWatch::looksLikeVersion(QString(65, QLatin1Char('1'))));
    QVERIFY(!VersionWatch::looksLikeVersion(QString::fromUtf8("0.1-3\xc3\xa9")));
}

void TstVersionWatch::aSecondRestartWithinTenMinutesIsRefusedAndNotedOnce()
{
    WatchRig r;
    r.stamp.next = StampRead::present(QStringLiteral("0.1-4"));
    QCOMPARE(r.watch.check(), VersionAction::StopUpgraded);
    r.watch.noteSelfRestart();
    // Still another version (a restart that did not bring the stamp's binary): no second restart
    // within 600 s, one entry, monitoring goes on. Heartbeats at 60 s to 540 s.
    for (int i = 0; i < 9; ++i) {
        r.clock.advanceS(60);
        QCOMPARE(r.watch.check(), VersionAction::None);
    }
    QCOMPARE(r.notes(VersionCheckResult::restartSuppressed()), 1);
    const JournalEntry e = r.journal.last(JournalKind::versionCheck());
    QCOMPARE(e.extra.value(VersionCheckKey::stamp()).toString(), QStringLiteral("0.1-4"));
    QCOMPARE(e.extra.value(VersionCheckKey::appVersion()).toString(), QStringLiteral("0.1-3"));
    r.clock.advanceS(59);           // 599 s: still refused
    QCOMPARE(r.watch.check(), VersionAction::None);
    r.clock.advanceS(1);            // 600 s: one more restart is allowed
    QCOMPARE(r.watch.check(), VersionAction::StopUpgraded);
    QCOMPARE(r.notes(VersionCheckResult::restartSuppressed()), 1);
}

void TstVersionWatch::theGuardHoldsAcrossARestartThroughTheSettings()
{
    WatchRig first;
    first.stamp.next = StampRead::present(QStringLiteral("0.1-4"));
    QCOMPARE(first.watch.check(), VersionAction::StopUpgraded);
    first.watch.noteSelfRestart();

    // The process that the activation started: a new watch, the settings of the first one.
    ScriptedStamp stamp;
    stamp.next = StampRead::present(QStringLiteral("0.1-4"));
    FakeJournal journal;
    first.clock.advanceS(65);
    VersionWatch second(&stamp, QStringLiteral("0.1-3"), &journal, &first.clock, &first.settings);
    QCOMPARE(second.check(), VersionAction::None);
    QCOMPARE(second.check(), VersionAction::None);
    QCOMPARE(journal.count(JournalKind::versionCheck()), 1);
    QCOMPARE(journal.last(JournalKind::versionCheck()).extra.value(VersionCheckKey::result()).toString(),
             VersionCheckResult::restartSuppressed());

    // The normal case: the new binary IS the stamp's version, so nothing happens at all.
    VersionWatch fine(&stamp, QStringLiteral("0.1-4"), &journal, &first.clock, &first.settings);
    QCOMPARE(fine.check(), VersionAction::None);
    QCOMPARE(journal.count(JournalKind::versionCheck()), 1);
}

void TstVersionWatch::aClockSetBackStillCountsAsRecentWithinTheGuard()
{
    WatchRig r;
    r.stamp.next = StampRead::present(QStringLiteral("0.1-4"));
    r.watch.noteSelfRestart();
    r.clock.setWallOffsetS(-300);       // the clock set 5 minutes back: still within the guard
    QCOMPARE(r.watch.check(), VersionAction::None);
    r.clock.setWallOffsetS(-3600);      // another hour back: the stored time is far ahead, guard over
    QCOMPARE(r.watch.check(), VersionAction::StopUpgraded);
}

void TstVersionWatch::withoutSettingsTheGuardLastsAsLongAsTheProcess()
{
    ScriptedStamp stamp;
    stamp.next = StampRead::present(QStringLiteral("0.1-4"));
    FakeJournal journal;
    FakeServiceClock clock;
    VersionWatch w(&stamp, QStringLiteral("0.1-3"), &journal, &clock, 0);
    QCOMPARE(w.check(), VersionAction::StopUpgraded);
    w.noteSelfRestart();
    clock.advanceS(60);
    QCOMPARE(w.check(), VersionAction::None);
    VersionWatch fresh(&stamp, QStringLiteral("0.1-3"), &journal, &clock, 0);
    QCOMPARE(fresh.check(), VersionAction::StopUpgraded);
}

void TstVersionWatch::aFailingJournalChangesNothing()
{
    WatchRig r;
    r.journal.failAppend = true;
    r.stamp.next = StampRead::unreadable();
    QCOMPARE(r.watch.check(), VersionAction::None);
    QCOMPARE(r.journal.appendAttempts, 1);
    QCOMPARE(r.watch.check(), VersionAction::None);
    QCOMPARE(r.journal.appendAttempts, 1);      // noted once even though the write failed
}

// The kind `version-check` is new in the journal: the real journal takes it, through the
// adapter the service uses, and an unknown kind is still refused.
void TstVersionWatch::theRealJournalTakesAVersionCheckEntry()
{
    Journal journal;
    QVERIFY(journal.open(QStringLiteral(":memory:")));
    JournalLifecycleLog log(&journal);
    ScriptedStamp stamp;
    stamp.next = StampRead::unreadable();
    FakeServiceClock clock;
    VersionWatch w(&stamp, QStringLiteral("0.1-3"), &log, &clock, 0);
    QCOMPARE(w.check(), VersionAction::None);
    const QVector<JournalEntry> entries = journal.entries(JournalKind::versionCheck());
    QCOMPARE(entries.size(), 1);
    QCOMPARE(entries.first().extra.value(VersionCheckKey::result()).toString(), VersionCheckResult::stampUnreadable());
    QCOMPARE(entries.first().extra.value(VersionCheckKey::appVersion()).toString(), QStringLiteral("0.1-3"));
    // A version-check entry is not a lifecycle entry: the start cause never reads it.
    JournalEntry last;
    QVERIFY(!log.lastLifecycleEntry(&last));
    JournalEntry odd;
    odd.kind = QStringLiteral("version-chek");
    odd.wallTime = clock.now();
    QVERIFY(!journal.append(odd));
}

QTEST_GUILESS_MAIN(TstVersionWatch)
#include "tst_versionwatch.moc"
