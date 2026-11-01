// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Michele Brami
#ifndef LM_ENVELOPE_H
#define LM_ENVELOPE_H

#include <QJsonObject>
#include <QString>

namespace lm {

// Message envelope (requirements , REQ-ADP-02). Only the header is defined
// here. Command and reply payloads belong to contract v0, which is not written yet;
// payload fields travel untouched in Envelope::fields.

// Protocol version carried by every message. PROVISIONAL: 0 = contract v0, which is
// labelled unstable (REQ-ADP-14); format proposed.
const int kProtocolVersion = 0;

// Known message types. The names come from REQ-ADP-06, REQ-ADP-07 and section 4.2;
// reply, error, records and log are PROVISIONAL until the contract is written.
enum class MessageType {
    Hello, Accept, Reject, Configure, Shutdown,
    Authenticate, DiscoverSubjects, FetchCurrent, FetchRange,
    Reply, Error, Records, Log
};

QString messageTypeName(MessageType type);
bool messageTypeFromName(const QString &name, MessageType *type);

struct Envelope
{
    Envelope() : version(kProtocolVersion), type(MessageType::Reply) {}

    int version;
    QString id;             // 1 to 64 characters of [A-Za-z0-9._:-]
    MessageType type;
    QString correlationId;  // empty = not a response to anything
    QJsonObject fields;     // the whole message object as received, header included
};

struct EnvelopeError
{
    enum Code { None, MissingField, WrongFieldType, InvalidId, UnsupportedVersion, UnknownType };

    EnvelopeError() : code(None) {}

    Code code;
    QString detail;         // names the field, never quotes values
};

// Header keys on the wire.
inline QString envelopeVersionKey() { return QStringLiteral("version"); }
inline QString envelopeIdKey() { return QStringLiteral("id"); }
inline QString envelopeTypeKey() { return QStringLiteral("type"); }
inline QString envelopeCorrelationKey() { return QStringLiteral("correlation_id"); }

bool isValidMessageId(const QString &id);

// Parses the header. Unknown fields are ignored; an unknown type is an error.
bool parseEnvelope(const QJsonObject &object, Envelope *out, EnvelopeError *error);

// Builds a message: payload fields first, then the header keys (which win on conflict).
QJsonObject buildMessage(MessageType type, const QString &id, const QString &correlationId,
                         const QJsonObject &payload = QJsonObject());

// Message ids "<prefix><n>" with n counting up from 1 (prefix "c" core, "a" adapter).
class MessageIdGenerator
{
public:
    explicit MessageIdGenerator(const QString &prefix) : m_prefix(prefix), m_next(1) {}
    QString next() { return m_prefix + QString::number(m_next++); }

private:
    QString m_prefix;
    quint64 m_next;
};

} // namespace lm

#endif
