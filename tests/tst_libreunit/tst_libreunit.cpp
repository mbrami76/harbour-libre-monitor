// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Michele Brami
#include "libreunit.h"

#include <QtTest>

using namespace lm;

namespace {
const QJsonValue kMissing;   // undefined: what QJsonObject::value gives for an absent key
const QJsonValue kNull(QJsonValue::Null);
}

class TstLibreUnit : public QObject
{
    Q_OBJECT

    static void check(const UnitDetermination &d, const char *unit, const char *basis)
    {
        QCOMPARE(d.unit, QString::fromLatin1(unit));
        QCOMPARE(d.basis, QString::fromLatin1(basis));
        QCOMPARE(d.ok(), qstrlen(unit) > 0);
    }

private slots:
    // Step 1 and 2: code and relation.
    void verifiedCodeAgreesWithRelation()
    {
        check(determineGlucoseUnit(1, 112, 112), "mg/dL", "code");
    }
    void unknownCodeFallsBackToRelation()
    {
        check(determineGlucoseUnit(7, 112, 112), "mg/dL", "relation");
        check(determineGlucoseUnit(kMissing, 112, 112), "mg/dL", "relation");
        check(determineGlucoseUnit(kNull, 112, 112), "mg/dL", "relation");
        check(determineGlucoseUnit("1", 112, 112), "mg/dL", "relation");   // not a number
        check(determineGlucoseUnit(1.5, 112, 112), "mg/dL", "relation");
    }
    void provisionalMmolCodeNeverCountsAsBasisCode()
    {
        // Code 2 is provisional: the relation decides and names the basis.
        check(determineGlucoseUnit(2, 5.6, 101), "mmol/L", "relation");
    }
    void relationFindsMmolWithoutAnyCode()
    {
        check(determineGlucoseUnit(kMissing, 5.6, 101), "mmol/L", "relation");
        check(determineGlucoseUnit(kMissing, 33.4, 602), "mmol/L", "relation");
        check(determineGlucoseUnit(kMissing, 1.1, 20), "mmol/L", "relation");
    }
    void codeAndRelationDisagree()
    {
        // Code says mg/dL, the two values say mmol/L: the relation wins.
        check(determineGlucoseUnit(1, 5.6, 101), "mmol/L", "relation");
        // Code says mmol/L, the values are equal: mg/dL.
        check(determineGlucoseUnit(2, 112, 112), "mg/dL", "relation");
    }
    void valuesFittingNeitherUnitAreUndetermined()
    {
        check(determineGlucoseUnit(1, 112, 150), "", "none");
        check(determineGlucoseUnit(2, 5.6, 150), "", "none");
        // The magnitude must not rescue a reading whose two values contradict each other.
        check(determineGlucoseUnit(1, 100, 150), "", "none");
    }
    void toleranceEdges()
    {
        // 5.6 * 18.0156 = 100.88736
        check(determineGlucoseUnit(kMissing, 5.6, 102.3), "mmol/L", "relation");   // 1.41 off
        check(determineGlucoseUnit(kMissing, 5.6, 102.5), "", "none");             // 1.61 off
    }
    void binaryFloatingPointDoesNotBreakEquality()
    {
        // 0.1 + 0.2 style noise: equal for the purpose of the relation.
        check(determineGlucoseUnit(1, 0.1 + 0.2, 0.3), "mg/dL", "code");
    }

    // Step 3: ValueInMgPerDl missing, magnitude.
    void magnitudeWhenMgValueIsMissing_data()
    {
        QTest::addColumn<double>("value");
        QTest::addColumn<QString>("unit");
        QTest::newRow("40") << 40.0 << "mg/dL";
        QTest::newRow("400") << 400.0 << "mg/dL";
        QTest::newRow("112") << 112.0 << "mg/dL";
        QTest::newRow("2.2") << 2.2 << "mmol/L";
        QTest::newRow("22.2") << 22.2 << "mmol/L";
        QTest::newRow("22.3") << 22.3 << "mmol/L";
        QTest::newRow("0") << 0.0 << "mmol/L";
        QTest::newRow("gap 22.4") << 22.4 << "";
        QTest::newRow("gap 30") << 30.0 << "";
        QTest::newRow("gap 39.9") << 39.9 << "";
    }
    void magnitudeWhenMgValueIsMissing()
    {
        QFETCH(double, value);
        QFETCH(QString, unit);
        // The code is ignored in this step; it is present here to prove that.
        const UnitDetermination d = determineGlucoseUnit(1, value, kMissing);
        QCOMPARE(d.unit, unit);
        QCOMPARE(d.basis, QString(unit.isEmpty() ? "none" : "magnitude"));
        const UnitDetermination d2 = determineGlucoseUnit(kMissing, value, kNull);
        QCOMPARE(d2.unit, unit);
    }
    void missingMgValueOfAnyKind()
    {
        check(determineGlucoseUnit(1, 112, kNull), "mg/dL", "magnitude");
        check(determineGlucoseUnit(1, 112, "112"), "mg/dL", "magnitude");
    }

    // No value, nothing to determine.
    void missingOrNonNumericValue()
    {
        check(determineGlucoseUnit(1, kMissing, 112), "", "none");
        check(determineGlucoseUnit(1, kNull, 112), "", "none");
        check(determineGlucoseUnit(1, "112", 112), "", "none");
        check(determineGlucoseUnit(1, true, 112), "", "none");
    }
};

QTEST_APPLESS_MAIN(TstLibreUnit)
#include "tst_libreunit.moc"
