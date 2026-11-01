// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Michele Brami
//
// The thresholds model, the band function and the threshold history
// (REQ-THR-01 to 03, REQ-ALR-05, REQ-DAT-12).
// Every reading here is synthetic.
#include "alerts/band.h"
#include "alerts/thresholdbook.h"
#include "alerts/thresholds.h"

#include <QtTest>

#include <cmath>

using namespace lm;

Q_DECLARE_METATYPE(lm::Band)

namespace {

const UtcTime kT1 = UtcTime::fromMSecsSinceEpoch(1768000000000LL);
const UtcTime kT2 = UtcTime::fromMSecsSinceEpoch(1768000060000LL);
const UtcTime kT3 = UtcTime::fromMSecsSinceEpoch(1768000120000LL);

QUuid subjectA() { return QUuid("{11111111-1111-4111-8111-111111111111}"); }
QUuid subjectB() { return QUuid("{22222222-2222-4222-8222-222222222222}"); }

IdFactory counterIds(int *n)
{
    return [n]() { return QUuid::createUuidV5(QUuid("{6ba7b810-9dad-11d1-80b4-00c04fd430c8}"),
                                              QString::number(++*n)); };
}

} // namespace

class TstAlertThresholds : public QObject
{
    Q_OBJECT

private slots:
    void initTestCase() { qRegisterMetaType<lm::Band>(); }

    // ---- the numbers -------------------------------------------------------------------------
    void theDefaultsAreWholeMgDlAndConsistent()
    {
        const Thresholds t;
        QCOMPARE(t.ll, 54);
        QCOMPARE(t.l, 70);
        QCOMPARE(t.h, 180);
        QCOMPARE(t.hh, 250);
        QVERIFY(t.isConsistent());
    }

    void consistencyNeedsLLLessThanLLessThanHLessThanHH_data()
    {
        QTest::addColumn<int>("ll"); QTest::addColumn<int>("l"); QTest::addColumn<int>("h");
        QTest::addColumn<int>("hh"); QTest::addColumn<bool>("ok");
        QTest::newRow("defaults") << 54 << 70 << 180 << 250 << true;
        QTest::newRow("L equals LL") << 54 << 54 << 180 << 250 << false;
        QTest::newRow("L above H") << 54 << 181 << 180 << 250 << false;
        QTest::newRow("H equals HH") << 54 << 70 << 250 << 250 << false;
        QTest::newRow("tight but legal") << 54 << 55 << 249 << 250 << true;
        QTest::newRow("zero LL") << 0 << 70 << 180 << 250 << false;
        QTest::newRow("negative") << -5 << 70 << 180 << 250 << false;
    }
    void consistencyNeedsLLLessThanLLessThanHLessThanHH()
    {
        QFETCH(int, ll); QFETCH(int, l); QFETCH(int, h); QFETCH(int, hh); QFETCH(bool, ok);
        QCOMPARE(Thresholds(ll, l, h, hh).isConsistent(), ok);
    }

    // ---- the band function: the boundary readings -----------------
    void boundaryReadingsWithTheDefaults_data()
    {
        QTest::addColumn<double>("value");
        QTest::addColumn<Band>("band");
        QTest::newRow("53") << 53.0 << Band::LL;     // below 54
        QTest::newRow("54") << 54.0 << Band::L;      // 54 itself is not below 54
        QTest::newRow("55") << 55.0 << Band::L;
        QTest::newRow("69") << 69.0 << Band::L;
        QTest::newRow("70") << 70.0 << Band::N;      // 70 itself is not below 70
        QTest::newRow("71") << 71.0 << Band::N;
        QTest::newRow("179") << 179.0 << Band::N;
        QTest::newRow("180") << 180.0 << Band::N;    // 180 itself is not above 180
        QTest::newRow("181") << 181.0 << Band::H;
        QTest::newRow("249") << 249.0 << Band::H;
        QTest::newRow("250") << 250.0 << Band::H;    // 250 itself is not above 250
        QTest::newRow("251") << 251.0 << Band::HH;
    }
    void boundaryReadingsWithTheDefaults()
    {
        QFETCH(double, value);
        QFETCH(Band, band);
        QCOMPARE(bandOf(value, "mg/dL"), band);
        QCOMPARE(bandOfMgDl(value, Thresholds()), band);
    }

    void boundaryReadingsWithAnEditedLAndH_data()
    {
        QTest::addColumn<double>("value");
        QTest::addColumn<Band>("band");
        // L loosened to 65, H loosened to 200: LL and HH stay where they are.
        QTest::newRow("53") << 53.0 << Band::LL;
        QTest::newRow("54") << 54.0 << Band::L;
        QTest::newRow("64") << 64.0 << Band::L;
        QTest::newRow("65") << 65.0 << Band::N;
        QTest::newRow("66") << 66.0 << Band::N;
        QTest::newRow("70 is normal now") << 70.0 << Band::N;
        QTest::newRow("181 is normal now") << 181.0 << Band::N;
        QTest::newRow("199") << 199.0 << Band::N;
        QTest::newRow("200") << 200.0 << Band::N;
        QTest::newRow("201") << 201.0 << Band::H;
        QTest::newRow("250") << 250.0 << Band::H;
        QTest::newRow("251") << 251.0 << Band::HH;
    }
    void boundaryReadingsWithAnEditedLAndH()
    {
        QFETCH(double, value);
        QFETCH(Band, band);
        QCOMPARE(bandOf(value, "mg/dL", Thresholds(54, 65, 200, 250)), band);
    }

    void aTightenedLAndHMoveTheEdgesTheOtherWay()
    {
        const Thresholds t(54, 80, 150, 250);
        QCOMPARE(bandOf(79, "mg/dL", t), Band::L);
        QCOMPARE(bandOf(80, "mg/dL", t), Band::N);
        QCOMPARE(bandOf(150, "mg/dL", t), Band::N);
        QCOMPARE(bandOf(151, "mg/dL", t), Band::H);
    }

    void aReadingStoredInMmolIsConvertedExactlyNeverFromARoundedValue()
    {
        // 3.0 mmol/L = 54.05 mg/dL: not below 54, so L (the old module-8 rule said the same here).
        QCOMPARE(bandOf(3.0, "mmol/L"), Band::L);
        QCOMPARE(bandOf(2.9, "mmol/L"), Band::LL);     // 52.2
        QCOMPARE(bandOf(3.9, "mmol/L"), Band::N);      // 70.26
        QCOMPARE(bandOf(3.8, "mmol/L"), Band::L);      // 68.46
        // with whole-mg/dL thresholds these two stored values are one band higher than the
        // mmol/L figures of requirements 9.3 suggest, because 10.0 mmol/L is 180.16 mg/dL.
        QCOMPARE(bandOf(10.0, "mmol/L"), Band::H);
        QCOMPARE(bandOf(13.9, "mmol/L"), Band::HH);
        QCOMPARE(bandOf(9.9, "mmol/L"), Band::N);      // 178.35
        // 3.95 would DISPLAY as 4.0 but is compared as 3.95 (REQ-DAT-12): 71.16 mg/dL, normal.
        QCOMPARE(bandOf(3.95, "mmol/L"), Band::N);
    }

    void anUnknownUnitOrANonNumberIsNeverGuessed()
    {
        QCOMPARE(bandOf(5, "furlongs"), Band::Unknown);
        QCOMPARE(bandOf(5, ""), Band::Unknown);
        QCOMPARE(bandOf(std::nan(""), "mg/dL"), Band::Unknown);
        QCOMPARE(bandOf(INFINITY, "mg/dL"), Band::Unknown);
        QCOMPARE(bandOf(-INFINITY, "mmol/L"), Band::Unknown);
    }

    // ---- an edit in mmol/L is rounded once to whole mg/dL --------------------------------------
    void anEditTypedInMmolIsRoundedOnceToWholeMgDl_data()
    {
        QTest::addColumn<double>("mmol"); QTest::addColumn<int>("mgdl");
        QTest::newRow("3.9 is the default L") << 3.9 << 70;      // 70.26
        QTest::newRow("10.0 is the default H") << 10.0 << 180;   // 180.156
        QTest::newRow("13.9") << 13.9 << 250;                    // 250.42
        QTest::newRow("3.0") << 3.0 << 54;                       // 54.05
        QTest::newRow("4.2") << 4.2 << 76;                       // 75.66
        QTest::newRow("12.0") << 12.0 << 216;                    // 216.19
        QTest::newRow("3.5") << 3.5 << 63;                       // 63.05
        QTest::newRow("11.1") << 11.1 << 200;                    // 199.97
    }
    void anEditTypedInMmolIsRoundedOnceToWholeMgDl()
    {
        QFETCH(double, mmol); QFETCH(int, mgdl);
        int got = 0;
        QVERIFY(mmolEditToMgDl(mmol, &got));
        QCOMPARE(got, mgdl);
    }

    void anImplausibleEditIsRefused()
    {
        int got = 0;
        QVERIFY(!mmolEditToMgDl(0.0, &got));
        QVERIFY(!mmolEditToMgDl(-3.9, &got));
        QVERIFY(!mmolEditToMgDl(std::nan(""), &got));
        QVERIFY(!mmolEditToMgDl(1000.0, &got));
    }

    void theStoredNumberIsWhatIsShownBack()
    {
        // 4.2 mmol/L was stored as 76 mg/dL; the display of 76 is 4.2 again.
        int stored = 0;
        QVERIFY(mmolEditToMgDl(4.2, &stored));
        QCOMPARE(mgDlToMmolForDisplay(stored), 4.2);
        QCOMPARE(mgDlToMmolForDisplay(70), 3.9);
        QCOMPARE(mgDlToMmolForDisplay(180), 10.0);
    }

    // ---- the history and the refusals of section 3 --------------------------------------------
    void aSubjectStartsWithTheGlobalDefaultsAndNoHistory()
    {
        ThresholdBook b;
        QCOMPARE(b.thresholdsFor(subjectA()), Thresholds());
        QVERIFY(b.idInForce(subjectA()).isNull());
        QVERIFY(b.history(subjectA()).isEmpty());
    }

    void aStricterChangeNeedsNoConfirmation()
    {
        int n = 0;
        ThresholdBook b(counterIds(&n));
        const EditOutcome o = b.edit(subjectA(), 80, 150, Confirmation(), kT1);
        QCOMPARE(o.result, EditResult::Accepted);
        QCOMPARE(o.changes.size(), 2);
        QCOMPARE(b.thresholdsFor(subjectA()), Thresholds(54, 80, 150, 250));
    }

    void aLooserLowerLNeedsConfirmationThenTheValueTypedAgain()
    {
        ThresholdBook b;
        QCOMPARE(b.check(subjectA(), 60, 180, Confirmation(), kT1).result, EditResult::NeedsConfirmation);
        // confirmed, but nothing typed again, or a different value: the challenge fails
        Confirmation none; none.confirmed = true;
        QCOMPARE(b.check(subjectA(), 60, 180, none, kT1).result, EditResult::ConfirmationMismatch);
        QCOMPARE(b.check(subjectA(), 60, 180, Confirmation::typed(66, 180), kT1).result, EditResult::ConfirmationMismatch);
        QCOMPARE(b.check(subjectA(), 60, 180, Confirmation::typed(6, 180), kT1).result, EditResult::ConfirmationMismatch);
        QCOMPARE(b.check(subjectA(), 60, 180, Confirmation::typed(60, 180), kT1).result, EditResult::Accepted);
    }

    void aRefusalCarriesWhatAScreenNeedsToExplainIt()
    {
        // the page shows the units and says what was violated, with numbers.
        ThresholdBook b;
        EditOutcome o = b.check(subjectA(), 190, 180, Confirmation(), kT1);      // L above H
        QCOMPARE(o.result, EditResult::Inconsistent);
        QVERIFY(o.hasOffending);
        QCOMPARE(o.offending, Boundary::L);
        QCOMPARE(o.lMin, 55);                    // above LL (54)
        QCOMPARE(o.lMax, 179);                   // below the H asked for
        QCOMPARE(o.hMin, 191);
        QCOMPARE(o.hMax, 249);                   // below HH (250)
        o = b.check(subjectA(), 70, 260, Confirmation(), kT1);                   // H above HH
        QCOMPARE(o.offending, Boundary::H);
        o = b.check(subjectA(), 60, 180, Confirmation::typed(61, 180), kT1);
        QCOMPARE(o.result, EditResult::ConfirmationMismatch);
        QCOMPARE(o.offending, Boundary::L);
        QCOMPARE(o.globalL, 70);
        QCOMPARE(o.globalH, 180);
        o = b.check(subjectA(), 70, 220, Confirmation::typed(-1, 202), kT1);
        QCOMPARE(o.offending, Boundary::H);
        o = b.check(subjectA(), 0, 180, Confirmation(), kT1);
        QCOMPARE(o.result, EditResult::InvalidValue);
        QCOMPARE(o.offending, Boundary::L);
    }

    void aLooserChangeNeedsOnlyTheTypedValueAndNoComment()
    {
        // the typed value replaced the mandatory comment; removed the optional one.
        // The challenge alone is enough, twice in a row.
        ThresholdBook b;
        const EditOutcome o = b.edit(subjectA(), 60, 180, Confirmation::typed(60, 180), kT1);
        QCOMPARE(o.result, EditResult::Accepted);
        const EditOutcome p = b.edit(subjectA(), 55, 180, Confirmation::typed(55, 180), kT2);
        QCOMPARE(p.result, EditResult::Accepted);
        QCOMPARE(b.history(subjectA()).last().value, 55);
    }

    void aLooserHigherHNeedsConfirmationAndTheValueTypedAgain()
    {
        ThresholdBook b;
        QCOMPARE(b.check(subjectA(), 70, 220, Confirmation(), kT1).result, EditResult::NeedsConfirmation);
        QCOMPARE(b.check(subjectA(), 70, 220, Confirmation::typed(70, 202), kT1).result, EditResult::ConfirmationMismatch);
        QCOMPARE(b.check(subjectA(), 70, 220, Confirmation::typed(70, 220), kT1).result, EditResult::Accepted);
    }

    void onlyTheLoosenedBoundaryIsChallenged()
    {
        // L tightened (no challenge) while H is loosened: only the typed H must match.
        ThresholdBook b;
        QCOMPARE(b.check(subjectA(), 75, 220, Confirmation::typed(-1, 220), kT1).result, EditResult::Accepted);
        QCOMPARE(b.check(subjectA(), 75, 220, Confirmation::typed(75, 221), kT1).result, EditResult::ConfirmationMismatch);
    }

    void aRefusedChangeLeavesNothingBehind()
    {
        ThresholdBook b;
        QCOMPARE(b.edit(subjectA(), 60, 180, Confirmation(), kT1).result, EditResult::NeedsConfirmation);
        QCOMPARE(b.edit(subjectA(), 180, 70, Confirmation::typed(180, 70), kT1).result, EditResult::Inconsistent);
        QVERIFY(b.history(subjectA()).isEmpty());
        QCOMPARE(b.thresholdsFor(subjectA()), Thresholds());
    }

    void changesThatBreakTheOrderAreRefused_data()
    {
        QTest::addColumn<int>("l"); QTest::addColumn<int>("h");
        QTest::newRow("L above H") << 190 << 180;
        QTest::newRow("L equals H") << 100 << 100;
        QTest::newRow("L at LL") << 54 << 180;
        QTest::newRow("L below LL") << 50 << 180;
        QTest::newRow("H at HH") << 70 << 250;
        QTest::newRow("H above HH") << 70 << 300;
    }
    void changesThatBreakTheOrderAreRefused()
    {
        QFETCH(int, l); QFETCH(int, h);
        ThresholdBook b;
        // Even confirmed and explained: consistency is not negotiable.
        QCOMPARE(b.check(subjectA(), l, h, Confirmation::typed(l, h), kT1).result, EditResult::Inconsistent);
    }

    void nonsenseNumbersAreRefused()
    {
        ThresholdBook b;
        QCOMPARE(b.check(subjectA(), 0, 180, Confirmation::typed(0, 180), kT1).result, EditResult::InvalidValue);
        QCOMPARE(b.check(subjectA(), 70, -1, Confirmation::typed(70, -1), kT1).result, EditResult::InvalidValue);
        QCOMPARE(b.check(subjectA(), 70, 5000, Confirmation::typed(70, 5000), kT1).result, EditResult::InvalidValue);
        QCOMPARE(b.check(subjectA(), 70, 180, Confirmation::typed(70, 180), UtcTime()).result, EditResult::InvalidValue);
    }

    void aChangeToTheSameNumbersIsNotRecorded()
    {
        ThresholdBook b;
        QCOMPARE(b.check(subjectA(), 70, 180, Confirmation(), kT1).result, EditResult::NoChange);
        b.edit(subjectA(), 80, 180, Confirmation(), kT1);
        QCOMPARE(b.check(subjectA(), 80, 180, Confirmation(), kT2).result, EditResult::NoChange);
    }

    void onlyAMovingBoundaryIsJudgedForTheDeviation()
    {
        // H was loosened and explained once; tightening L later must not ask again.
        ThresholdBook b;
        QCOMPARE(b.edit(subjectA(), 70, 220, Confirmation::typed(70, 220), kT1).result, EditResult::Accepted);
        QCOMPARE(b.check(subjectA(), 75, 220, Confirmation(), kT2).result, EditResult::Accepted);
        // but touching H again, still looser than global, does:
        QCOMPARE(b.check(subjectA(), 70, 230, Confirmation(), kT2).result, EditResult::NeedsConfirmation);
    }

    void theDeviationIsJudgedAgainstTheGlobalValueNotThePreviousOne()
    {
        ThresholdBook b;
        b.edit(subjectA(), 60, 180, Confirmation::typed(60, 180), kT1);
        // 60 -> 65 is stricter than before but still looser than the global 70.
        QCOMPARE(b.check(subjectA(), 65, 180, Confirmation(), kT2).result, EditResult::NeedsConfirmation);
        // back to the global value is not a deviation.
        QCOMPARE(b.check(subjectA(), 70, 180, Confirmation(), kT2).result, EditResult::Accepted);
    }

    void everyChangeIsAppendedWithNewPreviousAndTime()
    {
        int n = 0;
        ThresholdBook b(counterIds(&n));
        b.edit(subjectA(), 60, 180, Confirmation::typed(60, 180), kT1);
        b.edit(subjectA(), 65, 180, Confirmation::typed(65, 180), kT2);
        const QVector<ThresholdChange> h = b.history(subjectA());
        QCOMPARE(h.size(), 2);
        QCOMPARE(h.at(0).boundary, Boundary::L);
        QCOMPARE(h.at(0).value, 60);
        QCOMPARE(h.at(0).previous, 70);
        QCOMPARE(h.at(0).time, kT1);
        QCOMPARE(h.at(1).value, 65);
        QCOMPARE(h.at(1).previous, 60);        // the previous value, not the global one
        QCOMPARE(h.at(1).time, kT2);
        QVERIFY(h.at(0).id != h.at(1).id);
    }

    void aBoundaryThatDoesNotMoveGetsNoEntry()
    {
        ThresholdBook b;
        const EditOutcome o = b.edit(subjectA(), 80, 180, Confirmation(), kT1);
        QCOMPARE(o.changes.size(), 1);
        QCOMPARE(o.changes.at(0).boundary, Boundary::L);
    }

    void theIdInForceIsTheNewestEntryAndAnswersWhatWasInForce()
    {
        // REQ-ALR-05: "what were the thresholds when this fired" is one lookup by this id.
        ThresholdBook b;
        b.edit(subjectA(), 60, 180, Confirmation::typed(60, 180), kT1);
        const QUuid first = b.idInForce(subjectA());
        b.edit(subjectA(), 60, 200, Confirmation::typed(60, 200), kT2);
        const QUuid second = b.idInForce(subjectA());
        QVERIFY(!first.isNull());
        QVERIFY(first != second);
        QCOMPARE(b.history(subjectA()).last().id, second);
    }

    void subjectsDoNotShareThresholds()
    {
        ThresholdBook b;
        b.edit(subjectA(), 60, 180, Confirmation::typed(60, 180), kT1);
        QCOMPARE(b.thresholdsFor(subjectB()), Thresholds());
        QVERIFY(b.idInForce(subjectB()).isNull());
    }

    void applyingTheSameEntriesTwiceChangesNothing()
    {
        ThresholdBook b;
        const EditOutcome o = b.check(subjectA(), 60, 200, Confirmation::typed(60, 200), kT1);
        b.apply(o.changes);
        b.apply(o.changes);
        QCOMPARE(b.history(subjectA()).size(), 2);
    }

    void theLedgerIsRebuiltFromEntriesInAnyOrder()
    {
        int n = 0;
        ThresholdBook b(counterIds(&n));
        b.edit(subjectA(), 60, 180, Confirmation::typed(60, 180), kT1);
        b.edit(subjectA(), 65, 190, Confirmation::typed(65, 190), kT2);
        b.edit(subjectB(), 75, 180, Confirmation(), kT3);
        QVector<ThresholdChange> all = b.history(subjectA()) + b.history(subjectB());
        std::reverse(all.begin(), all.end());       // the journal may be read in any order
        ThresholdBook r;
        r.restore(all);
        QCOMPARE(r.thresholdsFor(subjectA()), Thresholds(54, 65, 190, 250));
        QCOMPARE(r.thresholdsFor(subjectB()), Thresholds(54, 75, 180, 250));
        QCOMPARE(r.idInForce(subjectA()), b.idInForce(subjectA()));
        QCOMPARE(r.history(subjectA()).size(), 3);      // two L entries and one H
    }
};

QTEST_APPLESS_MAIN(TstAlertThresholds)
#include "tst_alertthresholds.moc"
