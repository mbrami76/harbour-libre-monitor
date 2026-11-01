// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Michele Brami
#include "libreadapter.h"

#include "jsonl/jsonlcodec.h"
#include "libreclient.h"
#include "libresource.h"
#include "libreunit.h"
#include "protocol/envelope.h"
#include "qthttptransport.h"
#include "record/registry.h"

#include <QFileDevice>
#include <QJsonArray>
#include <QPair>
#include <QStringList>
#include <QUrl>
#include <QVector>

#include <algorithm>

namespace lm {

namespace {

// Contract 5.5 ceiling of records in one `records` message.
const int kMaxRecordsPerBatch = 500;

// Command deadlines used when a command carries none.
const int kDefaultAuthenticateMs = 30000;
const int kDefaultDiscoverMs = 30000;
const int kDefaultFetchCurrentMs = 15000;
const int kDefaultFetchRangeMs = 60000;

// The logbook is a rolling window; two captures bound it between 13 d 4 h and 14 d 1 h
// Coverage claims only what was seen to be kept.
const qint64 kLogbookWindowMs = qint64(13) * 24 * 3600 * 1000;

QString k(const char *s) { return QString::fromLatin1(s); }

struct FeedResult
{
    QString feed;
    QVector<QPair<UtcTime, QJsonObject> > records;   // sorted by time
    bool hasCoverage;
    UtcTime coverFrom;
    UtcTime coverTo;

    FeedResult() : hasCoverage(false) {}
};

bool inRange(const UtcTime &t, const UtcTime &from, const UtcTime &to)
{
    return !(t < from) && !(to < t);
}

bool recordLess(const QPair<UtcTime, QJsonObject> &a, const QPair<UtcTime, QJsonObject> &b)
{
    return a.first < b.first;
}

class AdapterRun
{
public:
    AdapterRun(QIODevice &in, QIODevice &out, const LibreAdapterEnv &env)
        : m_in(in), m_out(out), m_env(env), m_ids("a"), m_done(false), m_accepted(false),
          m_exit(LibreExitOk)
    {
        m_transport = env.transport ? env.transport : &m_ownTransport;
        m_client = new LibreClient(m_transport, env.config);
        m_client->setLogger([this](const QString &level, const QString &text) { sendLog(level, text); });
        if (env.clock)
            m_client->setClock(env.clock);
    }
    ~AdapterRun() { delete m_client; }

    int run();

private:
    UtcTime now() const { return m_env.clock ? m_env.clock() : UtcTime::now(); }

    void write(const QJsonObject &message);
    QString send(MessageType type, const QString &correlation, const QJsonObject &payload = QJsonObject());
    void sendLog(const QString &level, const QString &text);
    void sendError(const QString &correlation, const LibreError &error);
    void sendRecords(const QString &correlation, const QString &feed, const QJsonArray &records);

    void process(const JsonLinesItem &item);
    void handle(const Envelope &env);
    int deadlineMs(const Envelope &env, int fallback) const;

    void cmdConfigure(const Envelope &env);
    void cmdAuthenticate(const Envelope &env);
    void cmdDiscover(const Envelope &env);
    void cmdFetchCurrent(const Envelope &env);
    void cmdFetchRange(const Envelope &env);

    bool findConnection(const QString &subjectRef, Deadline &dl, QJsonObject *connection, LibreError *error);
    bool currentFeed(const QString &subjectRef, const QJsonObject &connection, FeedResult *out, LibreError *error);
    bool graphFeed(const QString &subjectRef, const UtcTime &from, const UtcTime &to,
                   Deadline &dl, FeedResult *out, QJsonObject *sensor, LibreError *error);
    bool logbookFeed(const QString &subjectRef, const UtcTime &from, const UtcTime &to,
                     Deadline &dl, FeedResult *out, LibreError *error);
    // `trend` is given by fetch_current only: the completion then carries the key,
    // with an integer or null; fetch_range passes no trend and the key is left out.
    void complete(const Envelope &env, QVector<FeedResult> &feeds, const QJsonObject &sensor,
                  const QJsonValue *trend = 0);
    bool checkSkips(const QString &feed, const SkipCounts &counts, LibreError *error);

    QIODevice &m_in;
    QIODevice &m_out;
    LibreAdapterEnv m_env;
    QtHttpTransport m_ownTransport;
    HttpTransport *m_transport;
    LibreClient *m_client;
    MessageIdGenerator m_ids;
    QString m_helloId;
    bool m_done;
    bool m_accepted;
    int m_exit;
};

void AdapterRun::write(const QJsonObject &message)
{
    const QByteArray line = encodeJsonLine(message);
    if (line.isEmpty())
        return;     // would exceed the line limit; callers that can split do so first
    m_out.write(line);
    if (QFileDevice *f = qobject_cast<QFileDevice *>(&m_out))
        f->flush();
}

QString AdapterRun::send(MessageType type, const QString &correlation, const QJsonObject &payload)
{
    const QString id = m_ids.next();
    write(buildMessage(type, id, correlation, payload));
    return id;
}

void AdapterRun::sendLog(const QString &level, const QString &text)
{
    QJsonObject p;
    p.insert(k("level"), level);
    p.insert(k("text"), text);
    send(MessageType::Log, QString(), p);
}

void AdapterRun::sendError(const QString &correlation, const LibreError &error)
{
    QJsonObject p;
    p.insert(k("class"), error.className());
    p.insert(k("detail"), error.detail);
    if (error.retryAfterS >= 0)
        p.insert(k("retry_after_s"), error.retryAfterS);
    send(MessageType::Error, correlation, p);
}

// Sends `records` messages of at most kMaxRecordsPerBatch records; a batch that would
// exceed the line limit is halved until it fits.
void AdapterRun::sendRecords(const QString &correlation, const QString &feed, const QJsonArray &records)
{
    for (int pos = 0; pos < records.size(); pos += kMaxRecordsPerBatch) {
        QJsonArray batch;
        for (int i = pos; i < qMin(pos + kMaxRecordsPerBatch, records.size()); ++i)
            batch.append(records.at(i));

        QVector<QJsonArray> pending;
        pending.append(batch);
        while (!pending.isEmpty()) {
            const QJsonArray part = pending.takeFirst();
            QJsonObject p;
            p.insert(k("feed"), feed);
            p.insert(k("records"), part);
            const QJsonObject msg = buildMessage(MessageType::Records, m_ids.next(), correlation, p);
            if (!encodeJsonLine(msg).isEmpty()) {
                write(msg);
            } else if (part.size() > 1) {
                QJsonArray a, b;
                for (int i = 0; i < part.size(); ++i)
                    (i < part.size() / 2 ? a : b).append(part.at(i));
                pending.prepend(b);
                pending.prepend(a);
            }
            // A single record above the line limit cannot be sent; the count in the
            // completion then differs and the core sees it.
        }
    }
}

int AdapterRun::run()
{
    QJsonObject hello;
    hello.insert(k("adapter"), k("libre"));
    hello.insert(k("adapter_version"), libreAdapterVersion());
    hello.insert(k("contract_version"), 0);
    QJsonArray units;
    units.append(k("mg/dL"));
    units.append(k("mmol/L"));
    QJsonArray metrics;
    // Both metrics are declared: the adapter sends logbook_entry events too (contract 7).
    QJsonObject glucose;
    glucose.insert(k("metric"), metricGlucose());
    glucose.insert(k("units"), units);
    metrics.append(glucose);
    QJsonObject logbook;
    logbook.insert(k("metric"), metricLogbookEntry());
    logbook.insert(k("units"), units);
    metrics.append(logbook);
    hello.insert(k("metrics"), metrics);
    QJsonArray feeds;
    feeds.append(k("current"));
    feeds.append(k("graph"));
    feeds.append(k("logbook"));
    hello.insert(k("feeds"), feeds);
    QJsonObject budgets;
    budgets.insert(k("max_records_per_batch"), kMaxRecordsPerBatch);
    hello.insert(k("budgets"), budgets);
    m_helloId = send(MessageType::Hello, QString(), hello);

    JsonLinesDecoder decoder;
    char buffer[4096];
    while (!m_done) {
        const qint64 n = m_in.read(buffer, sizeof buffer);
        if (n > 0) {
            const QVector<JsonLinesItem> items = decoder.feed(QByteArray(buffer, int(n)));
            for (int i = 0; i < items.size() && !m_done; ++i)
                process(items.at(i));
        } else if (n < 0) {
            break;
        } else if (!m_in.waitForReadyRead(-1)) {
            break;      // end of input
        }
    }
    return m_exit;
}

void AdapterRun::process(const JsonLinesItem &item)
{
    if (item.kind == JsonLinesItem::Oversize) {
        sendLog(k("error"), k("input line too long, stopping"));
        m_exit = LibreExitBadInput;
        m_done = true;
        return;
    }
    if (item.kind == JsonLinesItem::Malformed) {
        sendLog(k("warn"), k("malformed input line ignored"));
        return;
    }
    Envelope env;
    EnvelopeError ee;
    if (!parseEnvelope(item.message, &env, &ee)) {
        sendLog(k("warn"), k("message with an invalid header ignored: ") + ee.detail);
        return;
    }
    handle(env);
}

void AdapterRun::handle(const Envelope &env)
{
    if (!m_accepted) {
        if (env.type == MessageType::Accept && env.correlationId == m_helloId) {
            m_accepted = true;
        } else if (env.type == MessageType::Reject) {
            m_exit = LibreExitRejected;
            m_done = true;
        } else {
            sendLog(k("warn"), k("message before accept ignored"));
        }
        return;
    }
    switch (env.type) {
    case MessageType::Configure: cmdConfigure(env); break;
    case MessageType::Authenticate: cmdAuthenticate(env); break;
    case MessageType::DiscoverSubjects: cmdDiscover(env); break;
    case MessageType::FetchCurrent: cmdFetchCurrent(env); break;
    case MessageType::FetchRange: cmdFetchRange(env); break;
    case MessageType::Shutdown: m_done = true; break;
    default: sendLog(k("warn"), k("unexpected message type ignored")); break;
    }
}

int AdapterRun::deadlineMs(const Envelope &env, int fallback) const
{
    const QJsonValue v = env.fields.value(k("deadline_ms"));
    if (v.isDouble() && v.toDouble() >= 1 && v.toDouble() <= 3600000)
        return int(v.toDouble());
    return fallback;
}

void AdapterRun::cmdConfigure(const Envelope &env)
{
    const QJsonObject settings = env.fields.value(k("settings")).toObject();
    const QJsonValue host = settings.value(k("region_host"));
    if (host.isString() && !host.toString().isEmpty() && !m_client->setHost(host.toString()))
        sendLog(k("warn"), k("region_host is not an allowed host, default kept"));
    send(MessageType::Reply, env.id);
}

void AdapterRun::cmdAuthenticate(const Envelope &env)
{
    const QJsonObject cred = env.fields.value(k("credentials")).toObject();
    const QString user = cred.value(k("username")).toString();
    const QString pass = cred.value(k("password")).toString();
    if (user.isEmpty() || pass.isEmpty()) {
        sendError(env.id, LibreError(LibreError::InvalidCommand, k("credentials missing")));
        return;
    }
    const QString blob = env.fields.value(k("session")).toString();

    Deadline dl(deadlineMs(env, kDefaultAuthenticateMs));
    LibreClient::AuthResult result;
    LibreError error;
    if (!m_client->authenticate(user, pass, blob, dl, &result, &error)) {
        sendError(env.id, error);
        return;
    }
    QJsonObject p;
    p.insert(k("session"), result.blob);
    p.insert(k("session_expires"), UtcTime::fromMSecsSinceEpoch(result.expiresUnix * 1000).toIso());
    if (result.hostChanged) {
        QJsonObject upd;
        upd.insert(k("region_host"), result.host);
        p.insert(k("settings_update"), upd);
    }
    send(MessageType::Reply, env.id, p);
}

void AdapterRun::cmdDiscover(const Envelope &env)
{
    Deadline dl(deadlineMs(env, kDefaultDiscoverMs));
    QJsonValue data;
    LibreError error;
    if (!m_client->getData(k("/llu/connections"), dl, &data, &error)) {
        sendError(env.id, error);
        return;
    }
    if (!data.isArray() && !data.isNull()) {
        sendError(env.id, LibreError(LibreError::ProtocolChanged, k("connections is not an array")));
        return;
    }
    QJsonArray subjects;
    const QJsonArray list = data.toArray();     // null = no followed Subject
    for (int i = 0; i < list.size(); ++i) {
        const QJsonObject c = list.at(i).toObject();
        const QString ref = c.value(k("patientId")).toString();
        if (ref.isEmpty()) {
            sendError(env.id, LibreError(LibreError::ProtocolChanged, k("connection without patientId")));
            return;
        }
        QString name = (c.value(k("firstName")).toString() + QLatin1Char(' ')
                        + c.value(k("lastName")).toString()).trimmed();
        // The unit comes from the current reading (REQ-DAT-13, REQ-SUB-05).
        UnitDetermination unit;
        unit.basis = k("none");
        const QJsonValue gm = c.value(k("glucoseMeasurement"));
        if (gm.isObject()) {
            const QJsonObject r = gm.toObject();
            unit = determineGlucoseUnit(r.value(k("GlucoseUnits")), r.value(k("Value")),
                                        r.value(k("ValueInMgPerDl")));
        }
        QJsonObject s;
        s.insert(k("subject_ref"), ref);
        s.insert(k("display_name"), name);
        s.insert(k("unit"), unit.ok() ? QJsonValue(unit.unit) : QJsonValue(QJsonValue::Null));
        s.insert(k("unit_basis"), unit.basis);
        subjects.append(s);
    }
    QJsonObject p;
    p.insert(k("subjects"), subjects);
    send(MessageType::Reply, env.id, p);
}

bool AdapterRun::findConnection(const QString &subjectRef, Deadline &dl, QJsonObject *connection,
                                LibreError *error)
{
    QJsonValue data;
    if (!m_client->getData(k("/llu/connections"), dl, &data, error))
        return false;
    if (!data.isArray()) {
        *error = LibreError(LibreError::ProtocolChanged, k("connections is not an array"));
        return false;
    }
    const QJsonArray list = data.toArray();
    for (int i = 0; i < list.size(); ++i) {
        const QJsonObject c = list.at(i).toObject();
        if (c.value(k("patientId")).toString() == subjectRef) {
            *connection = c;
            return true;
        }
    }
    // PROVISIONAL: a Subject that is no longer shared is reported as transient; the
    // scheduler escalates repeated transient replies. Never observed.
    *error = LibreError(LibreError::Transient, k("subject not among the connections"));
    return false;
}

// Escalation decision and logging for one feed. Counts only; no values, no timestamps.
bool AdapterRun::checkSkips(const QString &feed, const SkipCounts &counts, LibreError *error)
{
    if (counts.format() + counts.noValue > 0) {
        sendLog(k("warn"), k("feed ") + feed + k(": ") + QString::number(counts.badTimestamp)
                + k(" bad timestamps, ") + QString::number(counts.undeterminedUnit)
                + k(" undetermined units, ") + QString::number(counts.malformed)
                + k(" malformed, ") + QString::number(counts.noValue)
                + k(" without value, of ") + QString::number(counts.examined) + k(" examined"));
    }
    if (shouldEscalate(counts)) {
        *error = LibreError(LibreError::ProtocolChanged,
                            k("feed ") + feed + k(": too many unusable items"));
        return false;
    }
    return true;
}

bool AdapterRun::currentFeed(const QString &subjectRef, const QJsonObject &connection,
                             FeedResult *out, LibreError *error)
{
    out->feed = k("current");
    if (!connection.contains(k("glucoseMeasurement"))) {
        *error = LibreError(LibreError::ProtocolChanged, k("connection has no glucoseMeasurement"));
        return false;
    }
    const QJsonValue gm = connection.value(k("glucoseMeasurement"));
    if (gm.isNull())
        return true;        // a Subject without data: empty answer, not an error (PROVISIONAL)

    SkipCounts counts;
    QJsonObject record;
    UtcTime t;
    SkipReason reason;
    if (buildGlucoseRecord(gm, subjectRef, out->feed, &record, &t, &reason)) {
        ++counts.examined;
        out->records.append(qMakePair(t, record));
        out->hasCoverage = true;
        out->coverFrom = out->coverTo = t;
    } else {
        counts.add(reason);
    }
    return checkSkips(out->feed, counts, error);
}

bool AdapterRun::graphFeed(const QString &subjectRef, const UtcTime &from, const UtcTime &to,
                           Deadline &dl, FeedResult *out, QJsonObject *sensor, LibreError *error)
{
    out->feed = k("graph");
    QJsonValue data;
    if (!m_client->getData(k("/llu/connections/") + QString::fromLatin1(QUrl::toPercentEncoding(subjectRef))
                           + k("/graph"), dl, &data, error))
        return false;
    if (!data.isObject()) {
        *error = LibreError(LibreError::ProtocolChanged, k("graph data is not an object"));
        return false;
    }
    const QJsonObject d = data.toObject();
    if (!d.contains(k("graphData"))) {
        *error = LibreError(LibreError::ProtocolChanged, k("graph reply has no graphData"));
        return false;
    }
    const QJsonValue g = d.value(k("graphData"));
    if (!g.isArray() && !g.isNull()) {
        *error = LibreError(LibreError::ProtocolChanged, k("graphData is not an array"));
        return false;
    }

    const QJsonArray sensors = d.value(k("activeSensors")).toArray();
    for (int i = 0; i < sensors.size() && sensor->isEmpty(); ++i)
        *sensor = buildSensorObject(sensors.at(i).toObject().value(k("sensor")));

    SkipCounts counts;
    const QJsonArray points = g.toArray();      // null = no data
    UtcTime oldest, newest;
    for (int i = 0; i < points.size(); ++i) {
        QJsonObject record;
        UtcTime t;
        SkipReason reason;
        if (!buildGlucoseRecord(points.at(i), subjectRef, out->feed, &record, &t, &reason)) {
            counts.add(reason);
            continue;
        }
        ++counts.examined;
        if (!oldest.isValid() || t < oldest) oldest = t;
        if (!newest.isValid() || newest < t) newest = t;
        if (inRange(t, from, to))      // the source ignores the range; the adapter filters
            out->records.append(qMakePair(t, record));
    }
    if (!checkSkips(out->feed, counts, error))
        return false;
    if (oldest.isValid()) {
        const UtcTime cf = oldest < from ? from : oldest;
        const UtcTime ct = to < newest ? to : newest;
        if (!(ct < cf)) {
            out->hasCoverage = true;
            out->coverFrom = cf;
            out->coverTo = ct;
        }
    }
    return true;
}

bool AdapterRun::logbookFeed(const QString &subjectRef, const UtcTime &from, const UtcTime &to,
                             Deadline &dl, FeedResult *out, LibreError *error)
{
    out->feed = k("logbook");
    QJsonValue data;
    if (!m_client->getData(k("/llu/connections/") + QString::fromLatin1(QUrl::toPercentEncoding(subjectRef))
                           + k("/logbook"), dl, &data, error))
        return false;
    if (!data.isArray() && !data.isNull()) {
        *error = LibreError(LibreError::ProtocolChanged, k("logbook data is not an array"));
        return false;
    }
    SkipCounts counts;
    const QJsonArray entries = data.toArray();
    for (int i = 0; i < entries.size(); ++i) {
        QJsonObject record;
        UtcTime t;
        SkipReason reason;
        if (!buildLogbookRecord(entries.at(i), subjectRef, &record, &t, &reason)) {
            counts.add(reason);
            continue;
        }
        ++counts.examined;
        if (inRange(t, from, to))
            out->records.append(qMakePair(t, record));
    }
    if (!checkSkips(out->feed, counts, error))
        return false;

    // Coverage is the part of the asked range inside the rolling window up to now.
    const UtcTime n = now();
    const UtcTime windowStart = UtcTime::fromMSecsSinceEpoch(n.msecsSinceEpoch() - kLogbookWindowMs);
    const UtcTime cf = from < windowStart ? windowStart : from;
    const UtcTime ct = n < to ? n : to;
    if (!(ct < cf)) {
        out->hasCoverage = true;
        out->coverFrom = cf;
        out->coverTo = ct;
    }
    return true;
}

void AdapterRun::complete(const Envelope &env, QVector<FeedResult> &feeds, const QJsonObject &sensor,
                          const QJsonValue *trend)
{
    int sent = 0;
    UtcTime newest;
    QJsonArray coverage;
    for (int f = 0; f < feeds.size(); ++f) {
        FeedResult &fr = feeds[f];
        std::stable_sort(fr.records.begin(), fr.records.end(), recordLess);
        QJsonArray records;
        for (int i = 0; i < fr.records.size(); ++i) {
            records.append(fr.records.at(i).second);
            if (!newest.isValid() || newest < fr.records.at(i).first)
                newest = fr.records.at(i).first;
        }
        if (!records.isEmpty())
            sendRecords(env.id, fr.feed, records);
        sent += records.size();
        if (fr.hasCoverage) {
            QJsonObject c;
            c.insert(k("feed"), fr.feed);
            c.insert(k("from"), fr.coverFrom.toIso());
            c.insert(k("to"), fr.coverTo.toIso());
            coverage.append(c);
        }
    }
    QJsonObject p;
    p.insert(k("records_sent"), sent);
    p.insert(k("watermark"), newest.isValid() ? QJsonValue(newest.toIso()) : QJsonValue(QJsonValue::Null));
    p.insert(k("coverage"), coverage);
    if (!sensor.isEmpty())
        p.insert(k("sensor"), sensor);
    if (trend)
        p.insert(k("trend"), *trend);
    send(MessageType::Reply, env.id, p);
}

void AdapterRun::cmdFetchCurrent(const Envelope &env)
{
    const QString ref = env.fields.value(k("subject_ref")).toString();
    if (ref.isEmpty()) {
        sendError(env.id, LibreError(LibreError::InvalidCommand, k("subject_ref missing")));
        return;
    }
    Deadline dl(deadlineMs(env, kDefaultFetchCurrentMs));
    LibreError error;
    QJsonObject connection;
    FeedResult current;
    if (!findConnection(ref, dl, &connection, &error) || !currentFeed(ref, connection, &current, &error)) {
        sendError(env.id, error);
        return;
    }
    // The arrow belongs to the reading that became the record: no record (a Subject without
    // data, or a reading that was skipped) means no arrow. Never stored, never interpreted.
    const QJsonValue trend = current.records.isEmpty()
        ? QJsonValue(QJsonValue::Null)
        : trendOfReading(connection.value(k("glucoseMeasurement")));
    QVector<FeedResult> feeds;
    feeds.append(current);
    complete(env, feeds, buildSensorObject(connection.value(k("sensor"))), &trend);
}

void AdapterRun::cmdFetchRange(const Envelope &env)
{
    const QString ref = env.fields.value(k("subject_ref")).toString();
    const UtcTime from = UtcTime::fromIso(env.fields.value(k("from")).toString());
    const UtcTime to = UtcTime::fromIso(env.fields.value(k("to")).toString());
    if (ref.isEmpty() || !from.isValid() || !to.isValid() || !(from < to)) {
        sendError(env.id, LibreError(LibreError::InvalidCommand, k("invalid fetch_range payload")));
        return;
    }
    QStringList wanted;
    if (env.fields.value(k("feeds")).isArray()) {
        const QJsonArray a = env.fields.value(k("feeds")).toArray();
        for (int i = 0; i < a.size(); ++i)
            wanted << a.at(i).toString();
    } else {
        wanted << k("graph") << k("logbook");     // the feeds that can cover a range
    }

    Deadline dl(deadlineMs(env, kDefaultFetchRangeMs));
    LibreError error;
    QVector<FeedResult> feeds;
    QJsonObject sensor;
    for (int i = 0; i < wanted.size(); ++i) {
        FeedResult fr;
        bool ok = true;
        if (wanted.at(i) == k("graph")) {
            ok = graphFeed(ref, from, to, dl, &fr, &sensor, &error);
        } else if (wanted.at(i) == k("logbook")) {
            ok = logbookFeed(ref, from, to, dl, &fr, &error);
        } else if (wanted.at(i) == k("current")) {
            QJsonObject connection;
            ok = findConnection(ref, dl, &connection, &error) && currentFeed(ref, connection, &fr, &error);
            if (ok) {
                // Only a reading inside the asked range counts.
                for (int r = fr.records.size() - 1; r >= 0; --r) {
                    if (!inRange(fr.records.at(r).first, from, to))
                        fr.records.remove(r);
                }
                if (fr.records.isEmpty())
                    fr.hasCoverage = false;
                if (sensor.isEmpty())
                    sensor = buildSensorObject(connection.value(k("sensor")));
            }
        } else {
            sendLog(k("warn"), k("requested feed is not offered, skipped"));
            continue;
        }
        if (!ok) {
            sendError(env.id, error);      // all or nothing: the core retries the whole command
            return;
        }
        feeds.append(fr);
    }
    complete(env, feeds, sensor);
}

} // namespace

int runLibreAdapter(QIODevice &in, QIODevice &out)
{
    return runLibreAdapter(in, out, LibreAdapterEnv());
}

int runLibreAdapter(QIODevice &in, QIODevice &out, const LibreAdapterEnv &env)
{
    AdapterRun run(in, out, env);
    return run.run();
}

} // namespace lm
