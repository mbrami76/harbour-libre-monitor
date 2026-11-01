// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Michele Brami
#include "units/glucoseunits.h"

#include <QtTest>

#include <cmath>

using namespace lm;

class TstUnits : public QObject
{
    Q_OBJECT

    static double conv(double v, const char *from, const char *to)
    {
        double out = -1;
        const bool ok = convertGlucoseForDisplay(v, from, to, &out);
        return ok ? out : qQNaN();
    }

private slots:
    void factorIsTheDocumentedConstant()
    {
        QCOMPARE(kMgDlPerMmolL, 18.0156);
    }
    void roundingHalfAwayFromZeroOnDecimalRepresentation()
    {
        // Binary doubles below the tie must still round up: 5.55 is 5.5499999... in binary.
        QCOMPARE(roundHalfAwayFromZero(5.55, 1), 5.6);
        QCOMPARE(roundHalfAwayFromZero(0.05, 1), 0.1);
        QCOMPARE(roundHalfAwayFromZero(0.15, 1), 0.2);
        QCOMPARE(roundHalfAwayFromZero(0.25, 1), 0.3);
        QCOMPARE(roundHalfAwayFromZero(1.005, 2), 1.01);
        QCOMPARE(roundHalfAwayFromZero(2.675, 2), 2.68);
        QCOMPARE(roundHalfAwayFromZero(8.35, 1), 8.4);
        QCOMPARE(roundHalfAwayFromZero(0.5, 0), 1.0);
        QCOMPARE(roundHalfAwayFromZero(1.5, 0), 2.0);
        QCOMPARE(roundHalfAwayFromZero(2.5, 0), 3.0);      // not banker's rounding
        QCOMPARE(roundHalfAwayFromZero(99.5, 0), 100.0);
        QCOMPARE(roundHalfAwayFromZero(-0.5, 0), -1.0);
        QCOMPARE(roundHalfAwayFromZero(-5.55, 1), -5.6);
        // Below the tie stays below.
        QCOMPARE(roundHalfAwayFromZero(5.549, 1), 5.5);
        QCOMPARE(roundHalfAwayFromZero(99.49, 0), 99.0);
        QCOMPARE(roundHalfAwayFromZero(6.04, 1), 6.0);
        // Already on the grid.
        QCOMPARE(roundHalfAwayFromZero(6.1, 1), 6.1);
        QCOMPARE(roundHalfAwayFromZero(110.0, 0), 110.0);
        QVERIFY(std::isnan(roundHalfAwayFromZero(qQNaN(), 1)));
    }
    void roundingIgnoresLocale()
    {
        const QLocale saved = QLocale();
        QLocale::setDefault(QLocale(QLocale::German, QLocale::Germany));
        QCOMPARE(roundHalfAwayFromZero(5.55, 1), 5.6);
        QCOMPARE(conv(6.1, "mmol/L", "mg/dL"), 110.0);
        QLocale::setDefault(saved);
    }
    void roundingToNativeResolution()
    {
        double r = 0;
        QVERIFY(roundToNativeResolution(110.4, "mg/dL", &r));
        QCOMPARE(r, 110.0);
        QVERIFY(roundToNativeResolution(6.14, "mmol/L", &r));
        QCOMPARE(r, 6.1);
        QVERIFY(!roundToNativeResolution(6.14, "g/L", &r));
        QVERIFY(!roundToNativeResolution(qQNaN(), "mg/dL", &r));
    }
    void refusesUnknownUnitsAndNonFinite()
    {
        double r = 0;
        QVERIFY(!convertGlucoseForDisplay(100, "mg/dl", "mmol/L", &r));
        QVERIFY(!convertGlucoseForDisplay(100, "mg/dL", "mmol", &r));
        QVERIFY(!convertGlucoseForDisplay(100, "", "mmol/L", &r));
        QVERIFY(!convertGlucoseForDisplay(qQNaN(), "mg/dL", "mmol/L", &r));
        QVERIFY(!convertGlucoseForDisplay(qInf(), "mg/dL", "mg/dL", &r));
    }
    void sameUnitOnlyRounds()
    {
        QCOMPARE(conv(110.0, "mg/dL", "mg/dL"), 110.0);
        QCOMPARE(conv(6.1, "mmol/L", "mmol/L"), 6.1);
        QCOMPARE(conv(5.55, "mmol/L", "mmol/L"), 5.6);
    }
    void knownValues()
    {
        QCOMPARE(conv(100.0, "mg/dL", "mmol/L"), 5.6);    // 5.5506
        QCOMPARE(conv(180.0, "mg/dL", "mmol/L"), 10.0);   // 9.9913
        QCOMPARE(conv(70.0, "mg/dL", "mmol/L"), 3.9);     // 3.8856
        QCOMPARE(conv(5.5, "mmol/L", "mg/dL"), 99.0);     // 99.086
        QCOMPARE(conv(10.0, "mmol/L", "mg/dL"), 180.0);   // 180.156
        QCOMPARE(conv(5.55, "mmol/L", "mg/dL"), 100.0);   // 99.986
    }
    void mgdlToMmollAcrossPlausibleRange()
    {
        for (int mg = 20; mg <= 600; ++mg) {
            const double out = conv(mg, "mg/dL", "mmol/L");
            QVERIFY2(!std::isnan(out), "conversion failed");
            // On the one-decimal grid ...
            QVERIFY2(std::fabs(out * 10.0 - std::floor(out * 10.0 + 0.5)) < 1e-9, "not one decimal");
            // ... and the nearest grid point to the exact quotient.
            QVERIFY2(std::fabs(out - mg / kMgDlPerMmolL) <= 0.05 + 1e-9, qPrintable(QString::number(mg)));
        }
    }
    void mmollToMgdlAcrossPlausibleRange()
    {
        for (int k = 11; k <= 334; ++k) {
            const double mmol = k / 10.0;
            const double out = conv(mmol, "mmol/L", "mg/dL");
            QVERIFY2(!std::isnan(out), "conversion failed");
            QCOMPARE(out, std::floor(out));      // whole number
            QVERIFY2(std::fabs(out - mmol * kMgDlPerMmolL) <= 0.5 + 1e-9, qPrintable(QString::number(mmol)));
        }
    }
    // Property reported in the module status: a consequence of the units, not a defect.
    void mmollThroughMgdlAndBackIsIdentity()
    {
        for (int k = 11; k <= 334; ++k) {
            const double mmol = k / 10.0;
            const double mg = conv(mmol, "mmol/L", "mg/dL");
            QCOMPARE(conv(mg, "mg/dL", "mmol/L"), mmol);
        }
    }
    void mgdlThroughMmollAndBackDiffersByAtMostOne()
    {
        int differing = 0, worst = 0;
        for (int mg = 20; mg <= 600; ++mg) {
            const double mmol = conv(mg, "mg/dL", "mmol/L");
            const int back = int(conv(mmol, "mmol/L", "mg/dL"));
            const int diff = std::abs(back - mg);
            QVERIFY2(diff <= 1, qPrintable(QString::number(mg)));
            if (diff) ++differing;
            if (diff > worst) worst = diff;
        }
        qInfo("mg/dL -> mmol/L -> mg/dL: %d of 581 whole numbers differ, largest difference %d mg/dL",
              differing, worst);
        QVERIFY(differing > 0);   // the round trip is not an identity, by the units
    }
    // The conversion must read the stored value: converting twice from a rounded value
    // gives a different answer for some inputs, which is why the API takes stored values only.
    void startingFromTheStoredValueMatters()
    {
        int differs = 0;
        for (int mg = 20; mg <= 600; ++mg) {
            const double once = conv(mg, "mg/dL", "mmol/L");
            const double viaRounded = conv(once, "mmol/L", "mg/dL");
            if (viaRounded != mg) ++differs;
        }
        QVERIFY(differs > 0);
    }
    // Distance from the nearest rounding tie in every conversion: a margin check for the
    // decimal-text rounding rule (noise absorbed is 5e-7 in scaled units).
    void noConversionLiesNearARoundingTie()
    {
        double minMargin = 1e9;
        for (int mg = 20; mg <= 600; ++mg) {
            const double scaled = mg / kMgDlPerMmolL * 10.0;
            minMargin = qMin(minMargin, std::fabs(scaled - std::floor(scaled) - 0.5));
        }
        for (int k = 11; k <= 334; ++k) {
            const double scaled = k / 10.0 * kMgDlPerMmolL;
            minMargin = qMin(minMargin, std::fabs(scaled - std::floor(scaled) - 0.5));
        }
        qInfo("smallest distance of a plausible-range conversion from a rounding tie: %g", minMargin);
        QVERIFY(minMargin > 1e-4);
    }
};

QTEST_APPLESS_MAIN(TstUnits)
#include "tst_units.moc"
