// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Michele Brami
#include "jsonl/jsonlcodec.h"

#include <QJsonDocument>
#include <QJsonParseError>
#include <QTextCodec>

namespace lm {

JsonLinesDecoder::JsonLinesDecoder(int maxLineLength)
    : m_scanned(0), m_max(maxLineLength), m_lineNumber(0), m_failed(false)
{
}

QVector<JsonLinesItem> JsonLinesDecoder::feed(const QByteArray &data)
{
    QVector<JsonLinesItem> items;
    if (m_failed)
        return items;

    m_buffer.append(data);

    for (;;) {
        const int nl = m_buffer.indexOf('\n', m_scanned);
        if (nl < 0) {
            // No terminator yet. A pending \r may still belong to the terminator.
            const int size = m_buffer.size();
            const bool pendingCr = size > 0 && m_buffer.at(size - 1) == '\r';
            if (size > m_max + (pendingCr ? 1 : 0)) {
                JsonLinesItem item;
                item.kind = JsonLinesItem::Oversize;
                item.error = QStringLiteral("line exceeds %1 bytes").arg(m_max);
                item.lineNumber = m_lineNumber + 1;
                items.append(item);
                m_failed = true;
                m_buffer.clear();
            } else {
                m_scanned = size;
            }
            return items;
        }

        QByteArray line = m_buffer.left(nl);
        m_buffer.remove(0, nl + 1);
        m_scanned = 0;
        ++m_lineNumber;
        if (line.endsWith('\r'))
            line.chop(1);

        if (line.size() > m_max) {
            JsonLinesItem item;
            item.kind = JsonLinesItem::Oversize;
            item.error = QStringLiteral("line exceeds %1 bytes").arg(m_max);
            item.lineNumber = m_lineNumber;
            items.append(item);
            m_failed = true;
            m_buffer.clear();
            return items;
        }
        if (line.trimmed().isEmpty())
            continue;

        JsonLinesItem item = decodeLine(line);
        item.lineNumber = m_lineNumber;
        items.append(item);
    }
}

JsonLinesItem JsonLinesDecoder::decodeLine(const QByteArray &line) const
{
    JsonLinesItem item;
    item.kind = JsonLinesItem::Malformed;
    item.lineNumber = 0;

    QTextCodec::ConverterState state;
    QTextCodec *utf8 = QTextCodec::codecForName("UTF-8");
    if (utf8) {
        utf8->toUnicode(line.constData(), line.size(), &state);
        if (state.invalidChars > 0) {
            item.error = QStringLiteral("invalid UTF-8");
            return item;
        }
    }

    QJsonParseError parseError;
    const QJsonDocument doc = QJsonDocument::fromJson(line, &parseError);
    if (parseError.error != QJsonParseError::NoError) {
        item.error = QStringLiteral("invalid JSON: %1").arg(parseError.errorString());
        return item;
    }
    if (!doc.isObject()) {
        item.error = QStringLiteral("not a JSON object");
        return item;
    }

    item.kind = JsonLinesItem::Message;
    item.message = doc.object();
    return item;
}

QByteArray encodeJsonLine(const QJsonObject &message, int maxLineLength)
{
    QByteArray line = QJsonDocument(message).toJson(QJsonDocument::Compact);
    if (line.size() > maxLineLength)
        return QByteArray();
    line.append('\n');
    return line;
}

} // namespace lm
