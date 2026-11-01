// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Michele Brami
#include "session/diagnosticreport.h"

#include "jsonl/jsonlcodec.h"
#include "time/utctime.h"

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonValue>
#include <QSet>

namespace lm {

namespace {

const char *kRemoved = "<removed>";

// How deep an unknown structure is described before it is cut to a plain "object".
const int kMaxShapeDepth = 4;
// Longest mask written for a text value; longer values are cut (the length is still true up
// to this bound, which is all a reader needs to see a format change).
const int kMaxMaskLength = 40;

QString k(const char *s) { return QString::fromLatin1(s); }

// Replaces every character of a text by its class, so that only the format remains:
// digit -> 9, lower-case letter -> a, upper-case letter -> A, space and ASCII punctuation
// kept (they are the structure of a date or unit and cannot spell a name), anything else
// -> ?. A name or a token turns into "Aaaaa aaaaa" or "aaaaaaaa", which says nothing about
// who or what it was.
QString maskText(const QString &text)
{
    QString out;
    const int n = qMin(text.size(), kMaxMaskLength);
    for (int i = 0; i < n; ++i) {
        const QChar c = text.at(i);
        const ushort u = c.unicode();
        if (u >= '0' && u <= '9')
            out += QLatin1Char('9');
        else if (u >= 'a' && u <= 'z')
            out += QLatin1Char('a');
        else if (u >= 'A' && u <= 'Z')
            out += QLatin1Char('A');
        else if (u < 128 && u >= 32)
            out += c;
        else
            out += QLatin1Char('?');
    }
    if (text.size() > n)
        out += QStringLiteral("...");
    return out;
}

// A key name is structure, not data, but an adapter could still put an identifier in a key.
// Keys that look like plain field names (letters and underscores, at most three digits, at
// most 32 characters) are kept; any other key is replaced by "<key N>" with N its position
// among the keys of that object, so two odd keys stay apart.
bool isPlainKey(const QString &key)
{
    if (key.isEmpty() || key.size() > 32)
        return false;
    int digits = 0;
    for (int i = 0; i < key.size(); ++i) {
        const ushort u = key.at(i).unicode();
        if (u >= '0' && u <= '9') {
            if (++digits > 3)
                return false;
        } else if (!((u >= 'a' && u <= 'z') || (u >= 'A' && u <= 'Z') || u == '_')) {
            return false;
        }
    }
    return true;
}

// Reduces any JSON value to its type, and objects to their structure. Used for every field
// the report does not know: its existence and kind help to see that a source changed, its
// content may be anything.
QJsonValue shapeOf(const QJsonValue &value, int depth)
{
    switch (value.type()) {
    case QJsonValue::Null: return k("null");
    case QJsonValue::Bool: return k("boolean");
    case QJsonValue::Double: return k("number");
    case QJsonValue::String: return k("string");
    case QJsonValue::Array: return QStringLiteral("array[%1]").arg(value.toArray().size());
    case QJsonValue::Object: {
        if (depth >= kMaxShapeDepth)
            return k("object");
        const QJsonObject o = value.toObject();
        QJsonObject out;
        int position = 0;
        for (QJsonObject::const_iterator it = o.begin(); it != o.end(); ++it, ++position) {
            const QString key = isPlainKey(it.key()) ? it.key() : QStringLiteral("<key %1>").arg(position);
            out.insert(key, shapeOf(it.value(), depth + 1));
        }
        return out;
    }
    default: return k("undefined");
    }
}

// A text value that must come from the adapter's declarations: copied when it is one of
// them, masked otherwise. `allowed` is the context's list.
QJsonValue declaredText(const QJsonValue &value, const QStringList &allowed)
{
    if (!value.isString())
        return shapeOf(value, 0);
    const QString text = value.toString();
    return allowed.contains(text) ? QJsonValue(text) : QJsonValue(maskText(text));
}

QString offsetText(qint64 ms)
{
    if (ms == 0)
        return k("t+0");
    // Whole seconds when exact, which is the usual case; otherwise milliseconds are shown
    // so no information is lost.
    const QString sign = ms < 0 ? QStringLiteral("-") : QStringLiteral("+");
    const qint64 abs = ms < 0 ? -ms : ms;
    if (abs % 1000 == 0)
        return QStringLiteral("t%1%2 s").arg(sign).arg(abs / 1000);
    return QStringLiteral("t%1%2.%3 s").arg(sign).arg(abs / 1000)
        .arg(abs % 1000, 3, 10, QLatin1Char('0'));
}

struct Anonymizer
{
    explicit Anonymizer(const DiagnosticContext &c) : context(c), haveBase(false), base(0)
    {
        for (int i = 0; i < c.declaredFeeds.size(); ++i)
            sources.append(c.adapterId + QLatin1Char('.') + c.declaredFeeds.at(i));
    }

    // The first readable measured_at of the batch, in arrival order, is the zero of all
    // offsets. Done before the records are written so that the order of keys cannot matter.
    void findBase(const QJsonArray &records)
    {
        for (int i = 0; i < records.size() && !haveBase; ++i) {
            const QJsonValue t = records.at(i).toObject().value(k("measured_at"));
            const UtcTime time = t.isString() ? UtcTime::fromIso(t.toString()) : UtcTime();
            if (time.isValid()) {
                haveBase = true;
                base = time.msecsSinceEpoch();
            }
        }
    }

    QJsonValue time(const QJsonValue &value) const
    {
        if (!value.isString())
            return shapeOf(value, 0);
        const UtcTime t = UtcTime::fromIso(value.toString());
        if (t.isValid() && haveBase)
            return offsetText(t.msecsSinceEpoch() - base);
        return QStringLiteral("<unreadable time ") + maskText(value.toString()) + QLatin1Char('>');
    }

    // A number is kept only where the report knows it to be a measurement.
    static QJsonValue measurement(const QJsonValue &value)
    {
        return value.isDouble() ? value : shapeOf(value, 0);
    }

    QJsonObject payload(const QJsonObject &p) const
    {
        QJsonObject out;
        int position = 0;
        for (QJsonObject::const_iterator it = p.begin(); it != p.end(); ++it, ++position) {
            const QString key = it.key();
            if (key == k("entry_type") || key == k("value"))
                out.insert(key, measurement(it.value()));
            else if (key == k("unit"))
                out.insert(key, declaredText(it.value(), context.declaredUnits));
            else
                out.insert(isPlainKey(key) ? key : QStringLiteral("<key %1>").arg(position),
                           shapeOf(it.value(), 1));
        }
        return out;
    }

    QJsonObject record(const QJsonObject &r, int index) const
    {
        QJsonObject out;
        out.insert(k("line_type"), k("record"));
        out.insert(k("index"), index);
        int position = 0;
        for (QJsonObject::const_iterator it = r.begin(); it != r.end(); ++it, ++position) {
            const QString key = it.key();
            const QJsonValue &v = it.value();
            if (key == k("shape")) {
                const QString s = v.toString();
                out.insert(key, (s == k("sample") || s == k("event")) ? QJsonValue(s)
                                                                       : QJsonValue(maskText(s)));
            } else if (key == k("metric")) {
                out.insert(key, declaredText(v, context.declaredMetrics));
            } else if (key == k("unit")) {
                out.insert(key, declaredText(v, context.declaredUnits));
            } else if (key == k("source")) {
                out.insert(key, declaredText(v, sources));
            } else if (key == k("value") || key == k("duration_ms")) {
                out.insert(key, measurement(v));
            } else if (key == k("measured_at")) {
                out.insert(key, time(v));
            } else if (key == k("original_timestamp")) {
                // The source's own local time text: a date. Only its format is kept.
                out.insert(key, v.isString() ? QJsonValue(maskText(v.toString())) : shapeOf(v, 0));
            } else if (key == k("payload") && v.isObject()) {
                out.insert(key, payload(v.toObject()));
            } else if (key == k("subject_ref") || key == k("sensor") || key == k("sensor_id")
                       || key == k("session") || key == k("token") || key == k("name")
                       || key == k("account") || key == k("account_id")) {
                // Known to be personal or secret: the key shows that the field was there,
                // the value is gone.
                out.insert(key, k(kRemoved));
            } else {
                out.insert(isPlainKey(key) ? key : QStringLiteral("<key %1>").arg(position),
                           shapeOf(v, 0));
            }
        }
        return out;
    }

    const DiagnosticContext &context;
    QStringList sources;
    bool haveBase;
    qint64 base;
};

QByteArray line(const QJsonObject &o)
{
    return QJsonDocument(o).toJson(QJsonDocument::Compact) + '\n';
}

QJsonObject reportHeader(const DiagnosticContext &c)
{
    QJsonObject h;
    h.insert(k("line_type"), k("report"));
    h.insert(k("format"), kDiagnosticReportFormat);
    h.insert(k("application_version"), c.applicationVersion);
    h.insert(k("adapter_id"), c.adapterId);
    h.insert(k("adapter_version"), c.adapterVersion);
    h.insert(k("backend_version"), c.backendVersion);
    h.insert(k("command"), c.commandType);
    QJsonArray violations;
    for (int i = 0; i < c.violations.size(); ++i) {
        QJsonObject v;
        v.insert(k("kind"), c.violations.at(i).kind);
        v.insert(k("detail"), c.violations.at(i).detail);
        violations.append(v);
    }
    h.insert(k("violations"), violations);
    QJsonObject counts;
    counts.insert(k("batches"), static_cast<double>(c.batches));
    counts.insert(k("records_received"), static_cast<double>(c.recordsReceived));
    counts.insert(k("records_valid"), static_cast<double>(c.recordsValid));
    counts.insert(k("soft_violations"), c.softViolations);
    counts.insert(k("hard_violations"), c.hardViolations);
    QJsonObject byReason;
    for (QMap<QString, qint64>::const_iterator it = c.quarantinedByReason.begin();
         it != c.quarantinedByReason.end(); ++it)
        byReason.insert(it.key(), static_cast<double>(it.value()));
    counts.insert(k("quarantined_by_reason"), byReason);
    h.insert(k("counts"), counts);
    return h;
}

} // namespace

QByteArray buildDiagnosticReport(const QByteArray &rawBatchLine, const DiagnosticContext &context,
                                 int maxLineBytes)
{
    QByteArray out = line(reportHeader(context));

    QJsonObject batch;
    batch.insert(k("line_type"), k("batch"));

    // A line over the limit is not parsed at all: the driver never hands one over (it kills
    // at the limit), but this function is the last guard before a file the user will read.
    if (rawBatchLine.size() > maxLineBytes) {
        batch.insert(k("status"), k("cut"));
        batch.insert(k("bytes"), rawBatchLine.size());
        batch.insert(k("limit"), maxLineBytes);
        return out + line(batch);
    }

    // The same decoder as the driver uses, so "valid JSON object in valid UTF-8" means the
    // same thing here and there.
    JsonLinesDecoder decoder(maxLineBytes + 16);
    QByteArray text = rawBatchLine;
    if (text.endsWith('\r'))
        text.chop(1);
    const QVector<JsonLinesItem> items = decoder.feed(text + '\n');
    if (items.isEmpty() || items.at(0).kind != JsonLinesItem::Message) {
        batch.insert(k("status"), k("unparseable"));
        batch.insert(k("bytes"), rawBatchLine.size());
        return out + line(batch);
    }

    const QJsonObject message = items.at(0).message;
    const QJsonArray records = message.value(k("records")).toArray();
    Anonymizer anonymizer(context);
    anonymizer.findBase(records);

    batch.insert(k("status"), k("ok"));
    const int shown = qMin(records.size(), kDiagnosticMaxRecords);
    batch.insert(k("record_count"), records.size());
    batch.insert(k("records_omitted"), records.size() - shown);
    int position = 0;
    for (QJsonObject::const_iterator it = message.begin(); it != message.end(); ++it, ++position) {
        const QString key = it.key();
        if (key == k("records")) {
            continue;                           // written one per line below
        } else if (key == k("type") || key == k("version")) {
            // Protocol constants. A wrong value is itself a violation worth seeing, but it
            // may be any text, so only the declared form is copied.
            batch.insert(key, key == k("type") && it.value().toString() == k("records")
                                  ? it.value()
                                  : (it.value().isDouble() ? it.value() : shapeOf(it.value(), 0)));
        } else if (key == k("feed")) {
            batch.insert(key, declaredText(it.value(), context.declaredFeeds));
        } else if (key == k("id") || key == k("correlation_id")) {
            batch.insert(key, k(kRemoved));     // message ids: protocol bookkeeping, not needed
        } else {
            batch.insert(isPlainKey(key) ? key : QStringLiteral("<key %1>").arg(position),
                         shapeOf(it.value(), 0));
        }
    }
    out += line(batch);

    for (int i = 0; i < shown; ++i) {
        if (records.at(i).isObject())
            out += line(anonymizer.record(records.at(i).toObject(), i));
        else {
            QJsonObject o;
            o.insert(k("line_type"), k("record"));
            o.insert(k("index"), i);
            o.insert(k("entry"), shapeOf(records.at(i), 0));
            out += line(o);
        }
    }
    return out;
}

} // namespace lm
