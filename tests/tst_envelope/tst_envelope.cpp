// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Michele Brami
#include "protocol/envelope.h"

#include <QtTest>

using namespace lm;

class TstEnvelope : public QObject
{
    Q_OBJECT

    static QJsonObject valid()
    {
        return buildMessage(MessageType::FetchCurrent, "c1", "");
    }
    static EnvelopeError::Code codeFor(const QJsonObject &o)
    {
        EnvelopeError e;
        Envelope env;
        return parseEnvelope(o, &env, &e) ? EnvelopeError::None : e.code;
    }

private slots:
    void buildAndParse()
    {
        QJsonObject payload;
        payload.insert("anything", 7);
        const QJsonObject o = buildMessage(MessageType::Reply, "a5", "c4", payload);
        Envelope env;
        EnvelopeError e;
        QVERIFY(parseEnvelope(o, &env, &e));
        QCOMPARE(env.version, kProtocolVersion);
        QCOMPARE(env.id, QString("a5"));
        QCOMPARE(env.correlationId, QString("c4"));
        QCOMPARE(env.type, MessageType::Reply);
        QCOMPARE(env.fields.value("anything").toInt(), 7);
    }
    void requiredFields_data()
    {
        QTest::addColumn<QString>("key");
        QTest::newRow("version") << envelopeVersionKey();
        QTest::newRow("id") << envelopeIdKey();
        QTest::newRow("type") << envelopeTypeKey();
        QTest::newRow("correlation") << envelopeCorrelationKey();
    }
    void requiredFields()
    {
        QFETCH(QString, key);
        QJsonObject o = valid();
        o.remove(key);
        EnvelopeError e;
        QVERIFY(!parseEnvelope(o, 0, &e));
        QCOMPARE(e.code, EnvelopeError::MissingField);
        QCOMPARE(e.detail, key);
    }
    void wrongFieldTypes()
    {
        QJsonObject o = valid();
        o.insert(envelopeIdKey(), 5);
        QCOMPARE(codeFor(o), EnvelopeError::WrongFieldType);
        o = valid();
        o.insert(envelopeVersionKey(), "0");
        QCOMPARE(codeFor(o), EnvelopeError::WrongFieldType);
        o = valid();
        o.insert(envelopeVersionKey(), 0.5);
        QCOMPARE(codeFor(o), EnvelopeError::WrongFieldType);
        o = valid();
        o.insert(envelopeCorrelationKey(), QJsonValue::Null);
        QCOMPARE(codeFor(o), EnvelopeError::WrongFieldType);
    }
    void unsupportedVersion()
    {
        QJsonObject o = valid();
        o.insert(envelopeVersionKey(), kProtocolVersion + 1);
        QCOMPARE(codeFor(o), EnvelopeError::UnsupportedVersion);
    }
    void unknownFieldsAreIgnored()
    {
        QJsonObject o = valid();
        o.insert("future_field", QJsonObject());
        QCOMPARE(codeFor(o), EnvelopeError::None);
    }
    void unknownTypeIsAnError()
    {
        QJsonObject o = valid();
        o.insert(envelopeTypeKey(), "make_coffee");
        QCOMPARE(codeFor(o), EnvelopeError::UnknownType);
        o.insert(envelopeTypeKey(), "FETCH_CURRENT");   // names are exact
        QCOMPARE(codeFor(o), EnvelopeError::UnknownType);
    }
    void invalidIds()
    {
        QJsonObject o = valid();
        o.insert(envelopeIdKey(), "");
        QCOMPARE(codeFor(o), EnvelopeError::InvalidId);
        o.insert(envelopeIdKey(), "has space");
        QCOMPARE(codeFor(o), EnvelopeError::InvalidId);
        o.insert(envelopeIdKey(), QString(65, 'a'));
        QCOMPARE(codeFor(o), EnvelopeError::InvalidId);
        o = valid();
        o.insert(envelopeCorrelationKey(), "bad id");
        QCOMPARE(codeFor(o), EnvelopeError::InvalidId);
    }
    void allTypeNamesRoundTrip()
    {
        for (int i = int(MessageType::Hello); i <= int(MessageType::Log); ++i) {
            MessageType back;
            const QString name = messageTypeName(MessageType(i));
            QVERIFY(!name.isEmpty());
            QVERIFY(messageTypeFromName(name, &back));
            QCOMPARE(int(back), i);
        }
    }
    void headerWinsOverPayload()
    {
        QJsonObject payload;
        payload.insert(envelopeIdKey(), "evil");
        const QJsonObject o = buildMessage(MessageType::Log, "a1", "", payload);
        QCOMPARE(o.value(envelopeIdKey()).toString(), QString("a1"));
    }
    void idGenerator()
    {
        MessageIdGenerator g("c");
        QCOMPARE(g.next(), QString("c1"));
        QCOMPARE(g.next(), QString("c2"));
        QVERIFY(isValidMessageId(g.next()));
    }
};

QTEST_APPLESS_MAIN(TstEnvelope)
#include "tst_envelope.moc"
