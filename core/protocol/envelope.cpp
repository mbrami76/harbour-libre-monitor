// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Michele Brami
#include "protocol/envelope.h"

#include <QJsonValue>
#include <QRegularExpression>
#include <cmath>

namespace lm {

namespace {

struct TypeName { MessageType type; const char *name; };

const TypeName kTypeNames[] = {
    { MessageType::Hello, "hello" },
    { MessageType::Accept, "accept" },
    { MessageType::Reject, "reject" },
    { MessageType::Configure, "configure" },
    { MessageType::Shutdown, "shutdown" },
    { MessageType::Authenticate, "authenticate" },
    { MessageType::DiscoverSubjects, "discover_subjects" },
    { MessageType::FetchCurrent, "fetch_current" },
    { MessageType::FetchRange, "fetch_range" },
    { MessageType::Reply, "reply" },
    { MessageType::Error, "error" },
    { MessageType::Records, "records" },
    { MessageType::Log, "log" }
};

bool fail(EnvelopeError *error, EnvelopeError::Code code, const QString &detail)
{
    if (error) {
        error->code = code;
        error->detail = detail;
    }
    return false;
}

} // namespace

QString messageTypeName(MessageType type)
{
    for (const TypeName &t : kTypeNames)
        if (t.type == type)
            return QString::fromLatin1(t.name);
    return QString();
}

bool messageTypeFromName(const QString &name, MessageType *type)
{
    for (const TypeName &t : kTypeNames) {
        if (name == QLatin1String(t.name)) {
            if (type)
                *type = t.type;
            return true;
        }
    }
    return false;
}

bool isValidMessageId(const QString &id)
{
    static const QRegularExpression re(QStringLiteral("^[A-Za-z0-9._:-]{1,64}\\z"));
    return re.match(id).hasMatch();
}

bool parseEnvelope(const QJsonObject &object, Envelope *out, EnvelopeError *error)
{
    if (error)
        *error = EnvelopeError();

    const QString keys[] = { envelopeVersionKey(), envelopeIdKey(), envelopeTypeKey(),
                             envelopeCorrelationKey() };
    for (const QString &key : keys)
        if (!object.contains(key))
            return fail(error, EnvelopeError::MissingField, key);

    const QJsonValue version = object.value(envelopeVersionKey());
    if (!version.isDouble() || std::floor(version.toDouble()) != version.toDouble())
        return fail(error, EnvelopeError::WrongFieldType, envelopeVersionKey());
    if (version.toDouble() != kProtocolVersion)
        return fail(error, EnvelopeError::UnsupportedVersion, envelopeVersionKey());

    for (const QString &key : { envelopeIdKey(), envelopeTypeKey(), envelopeCorrelationKey() })
        if (!object.value(key).isString())
            return fail(error, EnvelopeError::WrongFieldType, key);

    const QString id = object.value(envelopeIdKey()).toString();
    if (!isValidMessageId(id))
        return fail(error, EnvelopeError::InvalidId, envelopeIdKey());

    const QString correlation = object.value(envelopeCorrelationKey()).toString();
    if (!correlation.isEmpty() && !isValidMessageId(correlation))
        return fail(error, EnvelopeError::InvalidId, envelopeCorrelationKey());

    MessageType type;
    if (!messageTypeFromName(object.value(envelopeTypeKey()).toString(), &type))
        return fail(error, EnvelopeError::UnknownType, envelopeTypeKey());

    if (out) {
        out->version = kProtocolVersion;
        out->id = id;
        out->type = type;
        out->correlationId = correlation;
        out->fields = object;
    }
    return true;
}

QJsonObject buildMessage(MessageType type, const QString &id, const QString &correlationId,
                         const QJsonObject &payload)
{
    QJsonObject o = payload;
    o.insert(envelopeVersionKey(), kProtocolVersion);
    o.insert(envelopeIdKey(), id);
    o.insert(envelopeTypeKey(), messageTypeName(type));
    o.insert(envelopeCorrelationKey(), correlationId);
    return o;
}

} // namespace lm
