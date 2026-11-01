// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Michele Brami
#ifndef LM_JSONLCODEC_H
#define LM_JSONLCODEC_H

#include <QByteArray>
#include <QJsonObject>
#include <QString>
#include <QVector>

namespace lm {

// Maximum length of one line in bytes, without the line terminator (REQ-GRD-01).
// PROVISIONAL: 1 MiB, proposed. The guard module sets the real ceiling.
const int kDefaultMaxLineLength = 1024 * 1024;

// One result of decoding. Malformed carries a reason that never quotes the input
// (the line may hold secrets, REQ-ADP-11).
struct JsonLinesItem
{
    enum Kind { Message, Malformed, Oversize };

    Kind kind;
    QJsonObject message;   // Message only
    QString error;         // Malformed and Oversize
    int lineNumber;        // 1-based, counts empty lines too
};

// Incremental JSON Lines decoder (REQ-ADP-02). Feed it whatever the pipe delivers.
// Lines end with \n or \r\n; empty and whitespace-only lines are skipped. A malformed
// line is reported and decoding continues with the next line. An oversize line is
// reported once as Oversize and the decoder then stays failed and returns nothing more:
// the core is expected to kill the process (REQ-GRD-01, REQ-ADM-04).
class JsonLinesDecoder
{
public:
    explicit JsonLinesDecoder(int maxLineLength = kDefaultMaxLineLength);

    QVector<JsonLinesItem> feed(const QByteArray &data);

    bool hasFailed() const { return m_failed; }
    int maxLineLength() const { return m_max; }

private:
    JsonLinesItem decodeLine(const QByteArray &line) const;

    QByteArray m_buffer;
    int m_scanned;
    int m_max;
    int m_lineNumber;
    bool m_failed;
};

// Compact JSON object followed by \n. Returns an empty array when the line (without
// terminator) would exceed maxLineLength, so the sender never produces an oversize line.
QByteArray encodeJsonLine(const QJsonObject &message, int maxLineLength = kDefaultMaxLineLength);

} // namespace lm

#endif
