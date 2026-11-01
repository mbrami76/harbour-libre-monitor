// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Michele Brami
#include "libretimestamp.h"

#include <QtTest>

using namespace lm;

class TstLibreTime : public QObject
{
    Q_OBJECT

private slots:
    void valid_data()
    {
        QTest::addColumn<QString>("text");
        QTest::addColumn<QString>("iso");
        QTest::newRow("noon") << "1/1/2026 12:00:00 PM" << "2026-01-01T12:00:00.000Z";
        QTest::newRow("midnight") << "1/1/2026 12:00:00 AM" << "2026-01-01T00:00:00.000Z";
        QTest::newRow("12:30 AM") << "3/4/2026 12:30:15 AM" << "2026-03-04T00:30:15.000Z";
        QTest::newRow("12:59 PM") << "3/4/2026 12:59:59 PM" << "2026-03-04T12:59:59.000Z";
        QTest::newRow("1 AM") << "3/4/2026 1:05:09 AM" << "2026-03-04T01:05:09.000Z";
        QTest::newRow("1 PM") << "3/4/2026 1:05:09 PM" << "2026-03-04T13:05:09.000Z";
        QTest::newRow("11 PM") << "12/31/2026 11:59:59 PM" << "2026-12-31T23:59:59.000Z";
        QTest::newRow("single digits") << "9/8/2026 7:06:05 AM" << "2026-09-08T07:06:05.000Z";
        QTest::newRow("leading zeros") << "01/02/2026 01:05:09 AM" << "2026-01-02T01:05:09.000Z";
        QTest::newRow("leap day") << "2/29/2028 3:04:05 PM" << "2028-02-29T15:04:05.000Z";
        QTest::newRow("epoch") << "1/1/1970 12:00:00 AM" << "1970-01-01T00:00:00.000Z";
    }
    void valid()
    {
        QFETCH(QString, text);
        QFETCH(QString, iso);
        UtcTime t;
        QVERIFY(parseLibreTimestamp(text, &t));
        QVERIFY(t.isValid());
        QCOMPARE(t.toIso(), iso);
    }
    void invalid_data()
    {
        QTest::addColumn<QString>("text");
        QTest::newRow("empty") << "";
        QTest::newRow("iso") << "2026-01-01T12:00:00Z";
        QTest::newRow("day first month 13") << "13/1/2026 12:00:00 PM";
        QTest::newRow("month 0") << "0/1/2026 12:00:00 PM";
        QTest::newRow("day 0") << "1/0/2026 12:00:00 PM";
        QTest::newRow("day 32") << "1/32/2026 12:00:00 PM";
        QTest::newRow("31 April") << "4/31/2026 12:00:00 PM";
        QTest::newRow("29 Feb non-leap") << "2/29/2026 12:00:00 PM";
        QTest::newRow("hour 0") << "1/1/2026 0:00:00 AM";
        QTest::newRow("hour 13") << "1/1/2026 13:00:00 PM";
        QTest::newRow("minute 60") << "1/1/2026 12:60:00 PM";
        QTest::newRow("second 60") << "1/1/2026 12:00:60 PM";
        QTest::newRow("one digit minute") << "1/1/2026 12:0:00 PM";
        QTest::newRow("one digit second") << "1/1/2026 12:00:0 PM";
        QTest::newRow("two digit year") << "1/1/26 12:00:00 PM";
        QTest::newRow("five digit year") << "1/1/20260 12:00:00 PM";
        QTest::newRow("dash separators") << "1-1-2026 12:00:00 PM";
        QTest::newRow("dot separators") << "1.1.2026 12:00:00 PM";
        QTest::newRow("T separator") << "1/1/2026T12:00:00 PM";
        QTest::newRow("no marker") << "1/1/2026 12:00:00";
        QTest::newRow("lower case marker") << "1/1/2026 12:00:00 pm";
        QTest::newRow("long marker") << "1/1/2026 12:00:00 PMX";
        QTest::newRow("wrong marker") << "1/1/2026 12:00:00 XM";
        QTest::newRow("no space before marker") << "1/1/2026 12:00:00PM";
        QTest::newRow("trailing text") << "1/1/2026 12:00:00 PM extra";
        QTest::newRow("trailing space") << "1/1/2026 12:00:00 PM ";
        QTest::newRow("leading space") << " 1/1/2026 12:00:00 PM";
        QTest::newRow("double space") << "1/1/2026  12:00:00 PM";
        QTest::newRow("three digit day") << "1/001/2026 12:00:00 PM";
        QTest::newRow("sign") << "+1/1/2026 12:00:00 PM";
        QTest::newRow("arabic-indic digit") << QString::fromUtf8("\xd9\xa1/1/2026 12:00:00 PM");
        QTest::newRow("24 hour clock") << "1/1/2026 23:00:00";
    }
    void invalid()
    {
        QFETCH(QString, text);
        UtcTime t = UtcTime::fromMSecsSinceEpoch(42);
        QVERIFY(!parseLibreTimestamp(text, &t));
        QCOMPARE(t.msecsSinceEpoch(), qint64(42));   // untouched on failure
    }
    void ignoresDefaultLocale()
    {
        const QLocale saved = QLocale();
        QLocale::setDefault(QLocale(QLocale::German, QLocale::Germany));
        UtcTime t;
        QVERIFY(parseLibreTimestamp("1/2/2026 3:04:05 PM", &t));
        QCOMPARE(t.toIso(), QString("2026-01-02T15:04:05.000Z"));
        QVERIFY(!parseLibreTimestamp("1/2/2026 3:04:05 nachm.", &t));
        QLocale::setDefault(saved);
    }
};

QTEST_APPLESS_MAIN(TstLibreTime)
#include "tst_libretime.moc"
