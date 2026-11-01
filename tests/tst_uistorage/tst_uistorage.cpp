// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Michele Brami

// and (REQ-STO-02, 05, 13, 15, 16;):
// what the storage page sends to the service and how it reads the answers, against the
// scripted MaintenanceBackend. There is no export and no import: compaction is sent after one
// confirmation, with the window only. What each test proves is in its comment. Synthetic data only.

#include "ui/fakemaintenancebackend.h"
#include "ui/storagecontroller.h"
#include "ui/storageview.h"
#include "ui/purgecontroller.h"
#include "ui/purgeview.h"
#include "ui/reportcontroller.h"

#include <QJsonDocument>
#include <QJsonObject>
#include <QTemporaryDir>
#include <QtTest>

using namespace lm::ui;

namespace {
QJsonObject json(const QString &text) { return QJsonDocument::fromJson(text.toUtf8()).object(); }

// Facts the core's dry run could give: 1000 bytes now, an estimate for the window, the rebuild need and
// the space check as the core states them (the controller never computes them itself).
StorageFacts facts(const QString &space, qint64 records = 10)
{
    StorageFacts f;
    f.ok = true; f.storeBytes = 1000; f.journalBytes = 10; f.freeBytes = 5000;
    f.estimateKnown = true; f.estimateBytes = 400; f.estimateRecords = records;
    f.rebuildNeedBytes = 1600; f.space = space;
    f.missingBytes = space == QLatin1String("short") ? 1234 : 0;
    return f;
}

// A backend that hands the given facts back unchanged (the fake's own figures are for the emulator).
class FixedFacts : public FakeMaintenanceBackend
{
public:
    StorageFacts fixed;
    void requestStorageFacts(int windowMonths)
    {
        calls << QStringLiteral("facts:%1").arg(windowMonths);
        StorageFacts f = fixed;
        f.windowMonths = windowMonths;
        emit storageFactsLoaded(f);
    }
};
}

class TestUiStorage : public QObject
{
    Q_OBJECT
private slots:
    // The parameters are those daemon/maintenancejobs.cpp reads: the window only, never below 14 days,
    // and nothing of the archive any more.
    void theParametersAreTheWindowOnly()
    {
        QJsonObject c = json(compactionParameters(3));
        QCOMPARE(c.value("window_months").toInt(), 3);
        QCOMPARE(c.size(), 1);
        c = json(compactionParameters(0));
        QCOMPARE(c.value("window_days").toInt(), 14);
        QCOMPARE(c.size(), 1);
        QCOMPARE(windowChoices(), QList<int>() << 0 << 1 << 3 << 6 << 12);
    }
    // Every word of StartMaintenance and every end of a compaction has its text, with the service's error
    // classes and phases; the words of the export and the import are gone; an unknown word gets
    // the general text, never a guess.
    void everyAnswerHasItsText()
    {
        QCOMPARE(startAnswerKey(QString()), QString());
        QCOMPARE(startAnswerKey("maintenance-running"), QStringLiteral("ui-maint-start-maintenance-running"));
        QCOMPARE(startAnswerKey("service-not-running"), QStringLiteral("ui-maint-start-service-not-running"));
        QCOMPARE(startAnswerKey("parameter-window"), QStringLiteral("ui-maint-start-parameter-window"));
        QCOMPARE(startAnswerKey("parameter-archive"), QStringLiteral("ui-maint-start-other"));
        QCOMPARE(startAnswerKey("something-new"), QStringLiteral("ui-maint-start-other"));
        QCOMPARE(finishedKey("compaction", true, QString(), QString()), QStringLiteral("ui-maint-done-compaction"));
        QCOMPARE(finishedKey("export", true, QString(), QString()), QStringLiteral("ui-maint-done-other"));
        QCOMPARE(finishedKey("compaction", false, "cancelled", "user"), QStringLiteral("ui-maint-cancelled"));
        QCOMPARE(finishedKey("compaction", false, "cancelled", "queue-high-water"), QStringLiteral("ui-maint-queue-stop"));
        QCOMPARE(finishedKey("compaction", false, "cancelled", "shutdown"), QStringLiteral("ui-maint-shutdown"));
        QCOMPARE(finishedKey("compaction", false, "insufficient-space", QString()), QStringLiteral("ui-maint-fail-insufficient-space"));
        QCOMPARE(finishedKey("compaction", false, "free-space-unknown", QString()), QStringLiteral("ui-maint-fail-free-space-unknown"));
        QCOMPARE(finishedKey("compaction", false, "window-too-short", QString()), QStringLiteral("ui-maint-fail-window-too-short"));
        QCOMPARE(finishedKey("compaction", false, "io", QString()), QStringLiteral("ui-maint-fail-io"));
        QCOMPARE(finishedKey("compaction", false, "readback-mismatch", QString()), QStringLiteral("ui-maint-fail-other"));
        QCOMPARE(phaseKey("checking-space"), QStringLiteral("ui-maint-phase-checking-space"));
        QCOMPARE(phaseKey("compacting"), QStringLiteral("ui-maint-phase-compacting"));
        QCOMPARE(phaseKey("finalizing"), QStringLiteral("ui-maint-phase-finalizing"));
        QCOMPARE(phaseKey("done"), QStringLiteral("ui-maint-phase-done"));
        QCOMPARE(phaseKey("writing"), QStringLiteral("ui-maint-phase-other"));
        QCOMPARE(detailBytes("{\"missing_bytes\":2048}", "missing_bytes"), qint64(2048));
        QCOMPARE(detailBytes("{}", "missing_bytes"), qint64(-1));
        QCOMPARE(detailBytes("not json", "bytes_freed"), qint64(-1));
    }
    // REQ-STO-05, 13, 16: compaction is sent only with a known estimate that removes something and a
    // rebuild the core says fits, never while a run goes on; with the chosen window only; the result says
    // the bytes freed and the facts are read again.
    void compactionIsSentOnlyWhenTheEstimateAllowsIt()
    {
        FixedFacts b;
        b.setAutoPlay(false);
        StorageController c;
        c.setBackend(&b);
        QVERIFY(!c.canCompact());                               // no facts yet
        QVERIFY(!c.compact());
        b.fixed = facts("short");
        c.refresh();
        QCOMPARE(c.space(), QStringLiteral("short"));
        QCOMPARE(c.missingBytes(), 1234.0);
        QVERIFY(!c.canCompact());                               // the service would refuse
        QVERIFY(!c.compact());
        b.fixed = facts("unknown");
        c.refresh();
        QVERIFY(!c.canCompact());                               // no override of an unknown free space
        b.fixed = facts("enough", 0);
        c.refresh();
        QVERIFY(!c.canCompact());                               // nothing would be removed
        b.fixed = facts("enough");
        c.setWindowMonths(6);
        QCOMPARE(b.calls.last(), QStringLiteral("facts:6"));
        QVERIFY(c.canCompact());
        QCOMPARE(c.rebuildNeedBytes(), 1600.0);                 // the core's figure, as given
        bool sentOnce = false;
        for (int i = 0; i < b.calls.size(); ++i)
            sentOnce = sentOnce || b.calls.at(i).startsWith("start:");
        QVERIFY(!sentOnce);                                     // nothing started before compact()
        QVERIFY(c.compact());
        QCOMPARE(b.calls.last(), QStringLiteral("start:compaction:{\"window_months\":6}"));
        QVERIFY(c.running());
        QVERIFY(!c.canCompact());
        QVERIFY(!c.compact());                                  // still running: nothing more is sent
        emit b.maintenanceProgress("compaction", "compacting", 2, 3, 10, 100, 0.5);
        QCOMPARE(c.phaseKey(), QStringLiteral("ui-maint-phase-compacting"));
        QCOMPARE(c.step(), 2);
        QCOMPARE(c.steps(), 3);
        b.setRunning(false, QString());
        emit b.maintenanceFinished("compaction", true, QString(), QString(), QString(), "{\"bytes_freed\":4096,\"removed\":10}");
        QCOMPARE(c.resultKey(), QStringLiteral("ui-maint-done-compaction"));
        QCOMPARE(c.resultArg(), QStringLiteral("4096"));
        QVERIFY(c.resultOk());
        QCOMPARE(b.calls.last(), QStringLiteral("facts:6"));    // read again after the run
        QVERIFY(c.canCompact());
    }
    // REQ-STO-16: a refusal of the service for space says the missing bytes; an unknown free space says
    // that nothing was removed; a start refused is said in words; Cancel goes to the service only while
    // something runs; readings lost are counted.
    void refusalsCancelAndLostReadings()
    {
        FixedFacts b;
        b.setAutoPlay(false);
        b.fixed = facts("enough");
        StorageController c;
        c.setBackend(&b);
        c.refresh();
        c.cancel();
        QVERIFY(!b.calls.contains("cancel"));                  // nothing runs: nothing sent
        b.setStartAnswer("maintenance-running");
        QVERIFY(c.compact());
        QCOMPARE(c.resultKey(), QStringLiteral("ui-maint-start-maintenance-running"));
        QVERIFY(!c.running());
        b.setStartAnswer(QString());
        QVERIFY(c.compact());
        c.cancel();
        QVERIFY(b.calls.contains("cancel"));
        QCOMPARE(c.resultKey(), QStringLiteral("ui-maint-cancelled"));
        emit b.maintenanceFinished("compaction", false, "insufficient-space", "m", QString(),
                                   "{\"space\":\"short\",\"missing_bytes\":5242880,\"rebuild_need_bytes\":9,\"free_bytes\":1}");
        QCOMPARE(c.resultKey(), QStringLiteral("ui-maint-fail-insufficient-space"));
        QCOMPARE(c.resultArg(), QStringLiteral("5242880"));
        emit b.maintenanceFinished("compaction", false, "insufficient-space", "m", QString(), "{}");
        QCOMPARE(c.resultArg(), QString());                     // no number: the text without one
        emit b.maintenanceFinished("compaction", false, "free-space-unknown", "m", QString(), "{\"space\":\"unknown\",\"free_bytes\":-1}");
        QCOMPARE(c.resultKey(), QStringLiteral("ui-maint-fail-free-space-unknown"));
        QCOMPARE(c.resultArg(), QString());
        QVERIFY(!c.resultOk());
        emit b.maintenanceWarning("reading-queue-full", "count=3");
        QCOMPARE(c.lostReadings(), 3);
        emit b.maintenanceWarning("reading-queue-high-water", "count=80");
        QCOMPARE(c.lostReadings(), 3);
        c.dismissResult();
        QCOMPARE(c.lostReadings(), 0);
        QCOMPARE(c.resultKey(), QString());
    }
    // REQ-STO-02 and 13: the facts follow the window; a stale answer for another window is ignored; the
    // low-space line follows the last weekly check; the scripted backend of the emulator states the space
    // check in the core's words and refuses as the service does when told to.
    void theFactsFollowTheWindowAndTheSpaceCheck()
    {
        FakeMaintenanceBackend b;
        b.setAutoPlay(false);
        StorageFacts f;
        f.ok = true; f.storeBytes = 100LL * 1024 * 1024; f.journalBytes = 10; f.freeBytes = 1024LL * 1024 * 1024;
        f.estimateKnown = true; f.checkKnown = true; f.checkLow = true;
        b.setFacts(f);
        StorageController c;
        c.setBackend(&b);
        c.refresh();
        QVERIFY(c.factsLoaded());
        QCOMPARE(c.windowMonths(), 3);
        QCOMPARE(c.estimateBytes(), 40.0 * 1024 * 1024);        // the fake's 40 % at 3 months
        QCOMPARE(c.space(), QStringLiteral("enough"));
        QVERIFY(c.low());
        QVERIFY(c.canCompact());
        c.setWindowMonths(0);                                   // 14 days frees more
        QVERIFY(c.estimateBytes() > 40.0 * 1024 * 1024);
        QCOMPARE(b.calls.last(), QStringLiteral("facts:0"));
        c.setWindowMonths(5);                                   // not a choice: ignored
        QCOMPARE(c.windowMonths(), 0);
        StorageFacts old = f; old.windowMonths = 12; old.estimateBytes = 1;
        emit b.storageFactsLoaded(old);                         // an answer for a window left meanwhile
        QVERIFY(c.estimateBytes() > 40.0 * 1024 * 1024);
        b.setSpaceScript("short");
        c.setWindowMonths(3);
        QCOMPARE(c.space(), QStringLiteral("short"));
        QCOMPARE(c.missingBytes(), 5.0 * 1024 * 1024);
        QVERIFY(!c.canCompact());
        b.setSpaceScript("unknown");
        c.setWindowMonths(1);
        QCOMPARE(c.space(), QStringLiteral("unknown"));
        QVERIFY(!c.canCompact());
        StorageFacts unreadable;                                // the store could not be read
        unreadable.error = "store-unreadable";
        b.setFacts(unreadable);
        c.setWindowMonths(6);
        QVERIFY(!c.factsOk());
        QVERIFY(!c.estimateKnown());
        QCOMPARE(c.space(), QString());
        QVERIFY(!c.canCompact());
    }
    // ---- (REQ-STO-17): the purge page -------------------------------
    // The ages, the label the service compares with (trimmed once, as daemon/serviceconfig.cpp reads it),
    // the exact comparison of the typed text, and the parameters: the typed text goes unchanged.
    void thePurgeLabelAndParametersAreTheServicesOwn()
    {
        QCOMPARE(purgeAgeChoices(), QList<int>() << 3 << 6 << 12 << 24);
        QCOMPARE(kDefaultPurgeAgeMonths, 24);
        QCOMPARE(purgeLabelOf("  AB "), QStringLiteral("AB"));
        QCOMPARE(purgeLabelOf("\tA\n"), QStringLiteral("A"));
        QCOMPARE(purgeLabelOf("   "), QString());
        QVERIFY(typedLabelMatches("AB", "AB"));
        QVERIFY(!typedLabelMatches("ab", "AB"));                // no case folding
        QVERIFY(!typedLabelMatches("AB ", "AB"));               // no trimming of the typed text
        QVERIFY(!typedLabelMatches("", ""));                    // no label: nothing matches
        QJsonObject e = json(purgeEstimateParameters("{s1}", 6));
        QCOMPARE(e.value("subject").toString(), QStringLiteral("{s1}"));
        QCOMPARE(e.value("age_months").toInt(), 6);
        QVERIFY(e.value("age_months").isDouble());              // a JSON number, as the service demands
        QCOMPARE(e.size(), 2);
        QJsonObject p = json(purgeParameters("{s1}", 24, " AB"));
        QCOMPARE(p.value("typed_label").toString(), QStringLiteral(" AB"));
        QCOMPARE(p.size(), 3);
    }
    // The service's details read into counts and sizes; every answer has its text identifier.
    void thePurgeAnswersAreRead()
    {
        const PurgeFacts f = purgeFactsFromDetails(
            "{\"subject\":\"{s1}\",\"age_months\":12,\"store_rows\":{\"record\":10,\"mark\":1,\"sensor\":2,\"quarantine\":0,\"ingest\":3},"
            "\"journal_entries\":{\"alert\":4,\"threshold\":1,\"silent\":0},\"total_bytes_estimated\":5000,\"needed_bytes\":9000,"
            "\"free_bytes\":100,\"space\":\"short\",\"missing_bytes\":8900,\"store_part_done\":true,\"removed\":{\"record\":10},"
            "\"journal_removed\":{\"alert\":0},\"bytes_freed\":700,\"journal_bytes_freed\":20}");
        QVERIFY(f.known);
        QCOMPARE(f.ageMonths, 12);
        QCOMPARE(f.storeRows, qint64(16));
        QCOMPARE(f.journalEntries, qint64(5));
        QCOMPARE(f.bytesEstimated, qint64(5000));
        QCOMPARE(f.neededBytes, qint64(9000));
        QCOMPARE(f.space, QStringLiteral("short"));
        QCOMPARE(f.missingBytes, qint64(8900));
        QVERIFY(f.storePartDone);
        QCOMPARE(f.removedStore, qint64(10));
        QCOMPARE(f.bytesFreed, qint64(720));
        QVERIFY(!purgeFactsFromDetails("{}").known);
        QCOMPARE(purgeStartKey(QString()), QString());
        QCOMPARE(purgeStartKey("label-mismatch"), QStringLiteral("ui-purge-start-label-mismatch"));
        QCOMPARE(purgeStartKey("no-label"), QStringLiteral("ui-purge-start-no-label"));
        QCOMPARE(purgeStartKey("unknown-subject"), QStringLiteral("ui-purge-start-unknown-subject"));
        QCOMPARE(purgeStartKey("age-not-allowed"), QStringLiteral("ui-purge-start-age-not-allowed"));
        QCOMPARE(purgeStartKey("maintenance-running"), QStringLiteral("ui-maint-start-maintenance-running"));
        QCOMPARE(purgeFinishedKey(true, QString(), QString(), true), QStringLiteral("ui-purge-done"));
        QCOMPARE(purgeFinishedKey(false, "io", QString(), true), QStringLiteral("ui-purge-repeat"));
        QCOMPARE(purgeFinishedKey(false, "io", QString(), false), QStringLiteral("ui-maint-fail-io"));
        QCOMPARE(purgeFinishedKey(false, "insufficient-space", QString(), false), QStringLiteral("ui-maint-fail-insufficient-space"));
        QCOMPARE(purgeFinishedKey(false, "free-space-unknown", QString(), false), QStringLiteral("ui-maint-fail-free-space-unknown"));
        QCOMPARE(purgeFinishedKey(false, "cancelled", "queue-high-water", false), QStringLiteral("ui-maint-queue-stop"));
        QCOMPARE(purgeFinishedKey(false, "no-subject", QString(), false), QStringLiteral("ui-purge-start-unknown-subject"));
        QCOMPARE(phaseKey("purging"), QStringLiteral("ui-maint-phase-purging"));
    }
    // The order of the page: the estimate first (the service's), nothing sent for the purge before the
    // estimate allows it and the typed text equals the label; a Subject without a label never; the estimate
    // follows the age; another Subject's or a compaction's answers are not read.
    void thePurgeIsSentOnlyAfterTheEstimateAndTheExactLabel()
    {
        FakeMaintenanceBackend b;
        b.setAutoPlay(false);
        PurgeController c;
        c.setBackend(&b);
        c.open("{s1}", "Anna", " AB ");
        QCOMPARE(c.label(), QStringLiteral("AB"));
        QCOMPARE(c.ageMonths(), 24);
        QCOMPARE(b.calls.last(), QStringLiteral("start:purge-estimate:{\"age_months\":24,\"subject\":\"{s1}\"}"));
        QCOMPARE(c.stage(), QStringLiteral("estimating"));
        QVERIFY(!c.canContinue());
        QVERIFY(!c.purge("AB"));                                // no estimate yet
        b.setRunning(false, QString());
        emit b.maintenanceFinished("compaction", true, QString(), QString(), QString(), "{\"bytes_freed\":1}");
        QVERIFY(!c.estimateKnown());                            // a compaction's end is not ours
        emit b.maintenanceFinished("purge-estimate", true, QString(), QString(), QString(),
                                   "{\"subject\":\"{s2}\",\"age_months\":24,\"store_rows\":{\"record\":5},\"space\":\"enough\"}");
        QVERIFY(!c.estimateKnown());                            // another Subject's
        emit b.maintenanceFinished("purge-estimate", true, QString(), QString(), QString(),
                                   "{\"subject\":\"{s1}\",\"age_months\":24,\"store_rows\":{\"record\":5},\"journal_entries\":{\"alert\":1},"
                                   "\"total_bytes_estimated\":800,\"needed_bytes\":2000,\"free_bytes\":9000,\"space\":\"enough\"}");
        QVERIFY(c.estimateKnown());
        QCOMPARE(c.storeRows(), 5.0);
        QCOMPARE(c.journalEntries(), 1.0);
        QVERIFY(c.canContinue());
        const int before = b.calls.size();
        QVERIFY(!c.purge("ab"));
        QVERIFY(!c.purge(" AB "));
        QVERIFY(!c.purge(""));
        QCOMPARE(b.calls.size(), before);                       // nothing sent for a wrong text
        c.setAgeMonths(6);                                      // the estimate follows the age
        QVERIFY(!c.estimateKnown());
        QVERIFY(b.calls.last().startsWith("start:purge-estimate:{\"age_months\":6"));
        QVERIFY(!c.purge("AB"));
        b.setRunning(false, QString());
        emit b.maintenanceFinished("purge-estimate", true, QString(), QString(), QString(),
                                   "{\"subject\":\"{s1}\",\"age_months\":6,\"store_rows\":{\"record\":9},\"space\":\"enough\"}");
        QVERIFY(c.purge("AB"));
        QCOMPARE(b.calls.last(), QStringLiteral("start:purge:{\"age_months\":6,\"subject\":\"{s1}\",\"typed_label\":\"AB\"}"));
        QCOMPARE(c.stage(), QStringLiteral("running"));
        QVERIFY(!c.canContinue());
        emit b.maintenanceProgress("compaction", "compacting", 2, 3, 0, 0, 0.5);
        QCOMPARE(c.phaseKey(), QString());                      // not ours
        emit b.maintenanceProgress("purge", "purging", 2, 3, 0, 0, 0.5);
        QCOMPARE(c.phaseKey(), QStringLiteral("ui-maint-phase-purging"));
        QSignalSpy finished(&c, SIGNAL(purgeFinished(QString)));
        b.setRunning(false, QString());
        emit b.maintenanceFinished("purge", true, QString(), QString(), QString(),
                                   "{\"subject\":\"{s1}\",\"age_months\":6,\"store_part_done\":true,\"removed\":{\"record\":9},"
                                   "\"journal_removed\":{\"alert\":1},\"bytes_freed\":4096}");
        QCOMPARE(c.resultKey(), QStringLiteral("ui-purge-done"));
        QVERIFY(c.resultOk());
        QCOMPARE(c.removedStore(), 9.0);
        QCOMPARE(c.removedJournal(), 1.0);
        QCOMPARE(c.bytesFreed(), 4096.0);
        QCOMPARE(finished.size(), 1);
        QVERIFY(b.calls.last().startsWith("start:purge-estimate:"));   // a fresh estimate after the purge
        QVERIFY(!c.canRepeat());

        PurgeController none;                                   // no label, never a purge
        none.setBackend(&b);
        none.open("{s3}", "Luca", "  ");
        QVERIFY(!none.hasLabel());
        b.setRunning(false, QString());
        emit b.maintenanceFinished("purge-estimate", true, QString(), QString(), QString(),
                                   "{\"subject\":\"{s3}\",\"age_months\":24,\"store_rows\":{\"record\":9},\"space\":\"enough\"}");
        QVERIFY(none.estimateKnown());                          // the estimate is shown
        QVERIFY(!none.canContinue());
        QVERIFY(!none.purge(""));
    }
    // The space check and the refusals: short and unknown space, nothing to remove, a start refused with
    // the service's word, and the repeat after a failed journal part with the same typed text.
    void thePurgeRefusalsAndTheRepeat()
    {
        FakeMaintenanceBackend b;
        b.setAutoPlay(false);
        PurgeController c;
        c.setBackend(&b);
        c.open("{s1}", "Anna", "AB");
        const char *estimate = "{\"subject\":\"{s1}\",\"age_months\":24,\"store_rows\":{\"record\":%1},\"space\":\"%2\",\"missing_bytes\":%3}";
        b.setRunning(false, QString());
        emit b.maintenanceFinished("purge-estimate", true, QString(), QString(), QString(), QString(estimate).arg(5).arg("short").arg(123));
        QCOMPARE(c.space(), QStringLiteral("short"));
        QCOMPARE(c.missingBytes(), 123.0);
        QVERIFY(!c.canContinue());
        c.refreshEstimate();
        b.setRunning(false, QString());
        emit b.maintenanceFinished("purge-estimate", true, QString(), QString(), QString(), QString(estimate).arg(5).arg("unknown").arg(0));
        QVERIFY(!c.canContinue());
        c.refreshEstimate();
        b.setRunning(false, QString());
        emit b.maintenanceFinished("purge-estimate", true, QString(), QString(), QString(), QString(estimate).arg(0).arg("enough").arg(0));
        QVERIFY(!c.canContinue());                              // nothing to remove
        c.refreshEstimate();
        b.setRunning(false, QString());
        emit b.maintenanceFinished("purge-estimate", true, QString(), QString(), QString(), QString(estimate).arg(5).arg("enough").arg(0));
        QVERIFY(c.canContinue());
        b.setStartAnswer("label-mismatch");                     // the service's own check
        QVERIFY(c.purge("AB"));
        QCOMPARE(c.resultKey(), QStringLiteral("ui-purge-start-label-mismatch"));
        QVERIFY(!c.canRepeat());
        b.setStartAnswer(QString());
        c.refreshEstimate();
        b.setRunning(false, QString());
        emit b.maintenanceFinished("purge-estimate", true, QString(), QString(), QString(), QString(estimate).arg(5).arg("enough").arg(0));
        QVERIFY(c.purge("AB"));
        b.setRunning(false, QString());
        emit b.maintenanceFinished("purge", false, "io", "m", QString(),
                                   "{\"subject\":\"{s1}\",\"age_months\":24,\"store_part_done\":true,\"removed\":{\"record\":5}}");
        QCOMPARE(c.resultKey(), QStringLiteral("ui-purge-repeat"));
        QVERIFY(c.canRepeat());
        QVERIFY(!c.canContinue());                              // finished with Repeat, not started anew
        QVERIFY(c.repeat());
        QCOMPARE(b.calls.last(), QStringLiteral("start:purge:{\"age_months\":24,\"subject\":\"{s1}\",\"typed_label\":\"AB\"}"));
        b.setRunning(false, QString());
        emit b.maintenanceFinished("purge", false, "insufficient-space", "m", QString(),
                                   "{\"subject\":\"{s1}\",\"age_months\":24,\"store_part_done\":false,\"missing_bytes\":777}");
        QCOMPARE(c.resultKey(), QStringLiteral("ui-maint-fail-insufficient-space"));
        QCOMPARE(c.resultMissingBytes(), 777.0);
        QVERIFY(!c.canRepeat());
    }
    // A purge's end is never a compaction result on the storage page, and its progress is not
    // shown there either; the storage facts are read again after a purge.
    void theStoragePageReadsOnlyCompactionResults()
    {
        FakeMaintenanceBackend b;
        b.setAutoPlay(false);
        StorageController c;
        c.setBackend(&b);
        emit b.maintenanceProgress("purge", "purging", 2, 3, 0, 0, 0.5);
        QCOMPARE(c.phaseKey(), QString());
        const int before = b.calls.size();
        emit b.maintenanceFinished("purge", true, QString(), QString(), QString(), "{\"bytes_freed\":4096}");
        QCOMPARE(c.resultKey(), QString());
        QCOMPARE(b.calls.size(), before + 1);
        QVERIFY(b.calls.last().startsWith("facts:"));
        emit b.maintenanceFinished("purge-estimate", false, "io", QString(), QString(), "{}");
        QCOMPARE(c.resultKey(), QString());
        emit b.maintenanceFinished("compaction", false, "io", QString(), QString(), "{}");
        QCOMPARE(c.resultKey(), QStringLiteral("ui-maint-fail-io"));
    }
    // The scripted backend of the emulator plays the service's purge: the estimate, the phases, the repeat.
    void theScriptedPurgePlaysTheService()
    {
        FakeMaintenanceBackend b;
        b.setPurgeScript("repeat");
        PurgeController c;
        c.setBackend(&b);
        c.open("{s1}", "Anna", "AB");
        QTRY_VERIFY_WITH_TIMEOUT(c.canContinue(), 3000);
        QVERIFY(c.purge("AB"));
        QTRY_COMPARE_WITH_TIMEOUT(c.resultKey(), QStringLiteral("ui-purge-repeat"), 5000);
        QVERIFY(c.repeat());
        QTRY_COMPARE_WITH_TIMEOUT(c.resultKey(), QStringLiteral("ui-purge-done"), 5000);
    }
    // ----: the diagnostic report dialog (REQ-GRD-15) ------------------
    // The service's answers are read: a report, or one of its error words; anything else is "malformed".
    void theReportAnswersAreRead()
    {
        QCOMPARE(reportAnswerError(FakeMaintenanceBackend::syntheticReport()), QString());
        QCOMPARE(reportAnswerError("{\"format\":1,\"error\":\"no-report\"}"), QStringLiteral("no-report"));
        QCOMPARE(reportAnswerError("{\"format\":1,\"error\":\"not-proposed\"}"), QStringLiteral("not-proposed"));
        QCOMPARE(reportAnswerError(""), QStringLiteral("malformed"));
        QCOMPARE(reportAnswerError("garbage"), QStringLiteral("malformed"));
        QCOMPARE(reportAnswerError("{\"line_type\":\"batch\"}\n{}"), QStringLiteral("malformed"));
    }
    // Consent first: nothing is asked of the service before write(); the text is written unchanged to the
    // documents folder, never over an existing file; then ReportHandled. "Not now" sends ReportHandled
    // without a file.
    void theFileIsWrittenOnlyAfterConsentAndThenHandled()
    {
        QTemporaryDir dir;
        FakeMaintenanceBackend b;
        b.setAutoPlay(false);
        ReportController r;
        r.setBackend(&b);
        r.setFolder(dir.path());
        r.setNowMs(1760860800000LL);
        r.open("c1");
        QCOMPARE(r.stage(), QStringLiteral("consent"));
        QVERIFY(b.calls.isEmpty());
        r.write();
        QCOMPARE(b.calls, QStringList() << "report:c1" << "handled:c1");
        QCOMPARE(r.stage(), QStringLiteral("written"));
        QCOMPARE(r.path(), dir.path() + "/libre-monitor-report-20251019T080000Z.jsonl");
        QFile f(r.path());
        QVERIFY(f.open(QIODevice::ReadOnly));
        QCOMPARE(QString::fromUtf8(f.readAll()), FakeMaintenanceBackend::syntheticReport());
        r.open("c1");
        r.write();                                              // the same second: a new name, nothing overwritten
        QCOMPARE(r.path(), dir.path() + "/libre-monitor-report-20251019T080000Z-2.jsonl");
        b.calls.clear();
        r.open("c2");
        r.decline();
        QCOMPARE(b.calls, QStringList() << "handled:c2");
        QCOMPARE(r.stage(), QStringLiteral("declined"));
    }
    // The error words: no-report ends the proposal (it comes back after the next problems), the others
    // leave it; nothing is written; an answer for another account is ignored; a folder that cannot be
    // written leaves the proposal open.
    void theErrorWordsAndAFailedWrite()
    {
        QTemporaryDir dir;
        FakeMaintenanceBackend b;
        b.setAutoPlay(false);
        ReportController r;
        r.setBackend(&b);
        r.setFolder(dir.path());
        b.setReportError("no-report");
        r.open("c1");
        r.write();
        QCOMPARE(r.errorKey(), QStringLiteral("ui-report-err-no-report"));
        QVERIFY(b.calls.contains("handled:c1"));
        b.calls.clear();
        b.setReportError("service-not-running");
        r.open("c1");
        r.write();
        QCOMPARE(r.errorKey(), QStringLiteral("ui-report-err-service-not-running"));
        QVERIFY(!b.calls.contains("handled:c1"));
        b.setReportError("not-proposed");
        r.open("c1");
        r.write();
        QCOMPARE(r.errorKey(), QStringLiteral("ui-report-err-not-proposed"));
        QCOMPARE(QDir(dir.path()).entryList(QDir::Files).size(), 0);
        r.open("c1");
        r.write();                                              // waiting...
        emit b.diagnosticReportAnswered(true, "c9", FakeMaintenanceBackend::syntheticReport());
        QCOMPARE(r.stage(), QStringLiteral("error"));           // (the fake answered c1 already: not-proposed)
        b.setReportError(QString());
        b.calls.clear();
        r.setFolder("/proc/no-such-folder");
        r.open("c1");
        r.write();
        QCOMPARE(r.errorKey(), QStringLiteral("ui-report-err-write"));
        QVERIFY(!b.calls.contains("handled:c1"));
    }
};

QTEST_GUILESS_MAIN(TestUiStorage)
#include "tst_uistorage.moc"
