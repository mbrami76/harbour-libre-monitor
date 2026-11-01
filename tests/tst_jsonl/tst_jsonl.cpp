// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Michele Brami
#include "jsonl/jsonlcodec.h"

#include <QJsonDocument>
#include <QtTest>

using namespace lm;

class TstJsonl : public QObject
{
    Q_OBJECT
private slots:
    void singleMessage()
    {
        JsonLinesDecoder d;
        const QVector<JsonLinesItem> r = d.feed("{\"a\":1}\n");
        QCOMPARE(r.size(), 1);
        QCOMPARE(r[0].kind, JsonLinesItem::Message);
        QCOMPARE(r[0].message.value("a").toInt(), 1);
        QCOMPARE(r[0].lineNumber, 1);
    }
    void partialLineWaitsForTerminator()
    {
        JsonLinesDecoder d;
        QVERIFY(d.feed("{\"a\":").isEmpty());
        QVERIFY(d.feed("2").isEmpty());
        const QVector<JsonLinesItem> r = d.feed("}\n");
        QCOMPARE(r.size(), 1);
        QCOMPARE(r[0].message.value("a").toInt(), 2);
    }
    void severalMessagesInOneRead()
    {
        JsonLinesDecoder d;
        const QVector<JsonLinesItem> r = d.feed("{\"n\":1}\n{\"n\":2}\n{\"n\":3}\n{\"n\":");
        QCOMPARE(r.size(), 3);
        QCOMPARE(r[2].message.value("n").toInt(), 3);
        QCOMPARE(d.feed("4}\n").size(), 1);
    }
    void byteByByte()
    {
        const QByteArray all = "{\"n\":1}\n\n{\"n\":2}\r\n";
        JsonLinesDecoder d;
        int messages = 0;
        for (int i = 0; i < all.size(); ++i)
            messages += d.feed(all.mid(i, 1)).size();
        QCOMPARE(messages, 2);
    }
    void emptyLinesAreSkippedButCounted()
    {
        JsonLinesDecoder d;
        const QVector<JsonLinesItem> r = d.feed("\n  \n{\"a\":1}\n");
        QCOMPARE(r.size(), 1);
        QCOMPARE(r[0].lineNumber, 3);
    }
    void crLfTerminator()
    {
        JsonLinesDecoder d;
        const QVector<JsonLinesItem> r = d.feed("{\"a\":1}\r\n{\"a\":2}\r\n");
        QCOMPARE(r.size(), 2);
        QCOMPARE(r[1].message.value("a").toInt(), 2);
    }
    void malformedLineDoesNotLoseLaterLines()
    {
        JsonLinesDecoder d;
        const QVector<JsonLinesItem> r = d.feed("{broken\n[1,2]\n{\"ok\":true}\n");
        QCOMPARE(r.size(), 3);
        QCOMPARE(r[0].kind, JsonLinesItem::Malformed);
        QCOMPARE(r[1].kind, JsonLinesItem::Malformed);   // valid JSON, not an object
        QCOMPARE(r[2].kind, JsonLinesItem::Message);
        QVERIFY(!d.hasFailed());
    }
    void malformedReasonDoesNotQuoteInput()
    {
        JsonLinesDecoder d;
        const QVector<JsonLinesItem> r = d.feed("{\"password\":\"hunter2\" oops}\n");
        QCOMPARE(r.size(), 1);
        QVERIFY(!r[0].error.contains("hunter2"));
    }
    void oversizeCompleteLine()
    {
        JsonLinesDecoder d(16);
        const QVector<JsonLinesItem> r = d.feed("{\"a\":\"0123456789abcdef\"}\n{\"a\":1}\n");
        QCOMPARE(r.size(), 1);
        QCOMPARE(r[0].kind, JsonLinesItem::Oversize);
        QVERIFY(d.hasFailed());
        QVERIFY(d.feed("{\"a\":1}\n").isEmpty());
    }
    void oversizeWithoutTerminator()
    {
        JsonLinesDecoder d(16);
        QVERIFY(d.feed(QByteArray(10, 'x')).isEmpty());
        const QVector<JsonLinesItem> r = d.feed(QByteArray(10, 'x'));
        QCOMPARE(r.size(), 1);
        QCOMPARE(r[0].kind, JsonLinesItem::Oversize);
        QVERIFY(d.hasFailed());
    }
    void lineAtExactLimitIsAccepted()
    {
        const QByteArray line = "{\"a\":\"xxxxxxxxxxx\"}";   // 18 bytes
        JsonLinesDecoder d(line.size());
        QCOMPARE(d.feed(line + "\r\n").size(), 1);
        QVERIFY(!d.hasFailed());
        JsonLinesDecoder d2(line.size());
        QVERIFY(d2.feed(line + "\r").isEmpty());
        QVERIFY(!d2.hasFailed());
        QCOMPARE(d2.feed("\n").size(), 1);
    }
    void utf8Text()
    {
        JsonLinesDecoder d;
        const QString text = QString::fromUtf8("caf\xc3\xa9 \xe2\x82\xac \xf0\x9f\x98\x80");
        const QByteArray line = QByteArray("{\"t\":\"") + text.toUtf8() + "\"}\n";
        const QVector<JsonLinesItem> r = d.feed(line);
        QCOMPARE(r.size(), 1);
        QCOMPARE(r[0].message.value("t").toString(), text);
    }
    void invalidUtf8IsMalformed()
    {
        JsonLinesDecoder d;
        const QVector<JsonLinesItem> r = d.feed("{\"t\":\"\xff\xfe\"}\n{\"ok\":1}\n");
        QCOMPARE(r.size(), 2);
        QCOMPARE(r[0].kind, JsonLinesItem::Malformed);
        QCOMPARE(r[1].kind, JsonLinesItem::Message);
    }
    void encodeRoundTrip()
    {
        QJsonObject o;
        o.insert("text", QString::fromUtf8("a\nb \xc3\xa9"));
        const QByteArray line = encodeJsonLine(o);
        QVERIFY(line.endsWith('\n'));
        QCOMPARE(line.count('\n'), 1);   // embedded newline is escaped
        JsonLinesDecoder d;
        const QVector<JsonLinesItem> r = d.feed(line);
        QCOMPARE(r.size(), 1);
        QCOMPARE(r[0].message, o);
    }
    void encodeRefusesOversize()
    {
        QJsonObject o;
        o.insert("a", QString(100, 'x'));
        QVERIFY(encodeJsonLine(o, 50).isEmpty());
        QVERIFY(!encodeJsonLine(o, 200).isEmpty());
    }
};

QTEST_APPLESS_MAIN(TstJsonl)
#include "tst_jsonl.moc"
