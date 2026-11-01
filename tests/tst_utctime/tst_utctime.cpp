// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Michele Brami
#include "time/utctime.h"

#include <QtTest>

using namespace lm;

class TstUtcTime : public QObject
{
    Q_OBJECT
private slots:
    void roundTripWithMilliseconds()
    {
        const UtcTime t = UtcTime::fromIso(QStringLiteral("2026-03-29T01:30:15.042Z"));
        QVERIFY(t.isValid());
        QCOMPARE(t.toIso(), QStringLiteral("2026-03-29T01:30:15.042Z"));
        QCOMPARE(UtcTime::fromMSecsSinceEpoch(t.msecsSinceEpoch()), t);
    }
    void knownEpochValue()
    {
        QCOMPARE(UtcTime::fromIso(QStringLiteral("1970-01-01T00:00:01.5Z")).msecsSinceEpoch(), qint64(1500));
        QCOMPARE(UtcTime::fromIso(QStringLiteral("2000-01-01T00:00:00Z")).msecsSinceEpoch(), qint64(946684800000LL));
    }
    void formatsAlwaysThreeDigits()
    {
        QCOMPARE(UtcTime::fromMSecsSinceEpoch(0).toIso(), QStringLiteral("1970-01-01T00:00:00.000Z"));
    }
    void rejectsWhatItWouldHaveToGuess_data()
    {
        QTest::addColumn<QString>("text");
        QTest::newRow("empty") << QString();
        QTest::newRow("offset") << QStringLiteral("2026-03-29T01:30:15+02:00");
        QTest::newRow("no zone") << QStringLiteral("2026-03-29T01:30:15");
        QTest::newRow("lower case") << QStringLiteral("2026-03-29t01:30:15z");
        QTest::newRow("four fraction digits") << QStringLiteral("2026-03-29T01:30:15.1234Z");
        QTest::newRow("source format") << QStringLiteral("3/29/2026 1:30:15 AM");
        QTest::newRow("bad month") << QStringLiteral("2026-13-01T00:00:00Z");
        QTest::newRow("bad day") << QStringLiteral("2026-02-30T00:00:00Z");
        QTest::newRow("leap second") << QStringLiteral("2026-06-30T23:59:60Z");
        QTest::newRow("year zero") << QStringLiteral("0000-01-01T00:00:00Z");
        QTest::newRow("trailing newline") << QStringLiteral("2026-03-29T01:30:15Z\n");
        QTest::newRow("trailing text") << QStringLiteral("2026-03-29T01:30:15Z x");
    }
    void rejectsWhatItWouldHaveToGuess()
    {
        QFETCH(QString, text);
        QVERIFY(!UtcTime::fromIso(text).isValid());
    }
    void invalidHasNoText()
    {
        QVERIFY(UtcTime().toIso().isEmpty());
    }
    void futureCheckUsesTolerance()
    {
        const UtcTime now = UtcTime::fromMSecsSinceEpoch(1000000);
        QVERIFY(UtcTime::fromMSecsSinceEpoch(1000000).isNotAfter(now, 0));
        QVERIFY(!UtcTime::fromMSecsSinceEpoch(1000001).isNotAfter(now, 0));
        QVERIFY(UtcTime::fromMSecsSinceEpoch(1000500).isNotAfter(now, 500));
        QVERIFY(!UtcTime::fromMSecsSinceEpoch(1000501).isNotAfter(now, 500));
        QVERIFY(!UtcTime().isNotAfter(now, 500));
        QVERIFY(!now.isNotAfter(UtcTime(), 500));
    }
};

QTEST_APPLESS_MAIN(TstUtcTime)
#include "tst_utctime.moc"
