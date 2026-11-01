// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Michele Brami
//
// Tests of the Libre adapter loop and commands with a fake transport. Every value, name,
// identifier and token in this file is invented (REQ-SIM-08, REQ-DOC-03). The shapes follow
//.

#include "jsonl/jsonlcodec.h"
#include "libreadapter.h"
#include "protocol/envelope.h"
#include "qthttptransport.h"
#include "record/record.h"
#include "session/adapterregistry.h"

#include <QBuffer>
#include <QCryptographicHash>
#include <QDateTime>
#include <QTcpServer>
#include <QTcpSocket>
#include <QtTest>

#include <functional>

using namespace lm;

namespace {

const char *kEmail = "carer@example.invalid";
const char *kPassword = "pw-secret-0001";
const char *kUserId = "user-id-0001";
const char *kToken = "TOKEN-SIM-0001";
const char *kFirst = "Ada";
const char *kLast = "Sample";
const char *kSerial = "SN-SIM-0001";
const qint64 kTokenExpires = 1800000000;        // Unix seconds
const char *kNow = "2026-01-02T00:00:00Z";

QJsonObject obj(const QByteArray &json)
{
    QJsonParseError pe;
    const QJsonDocument d = QJsonDocument::fromJson(json, &pe);
    if (pe.error != QJsonParseError::NoError || !d.isObject())
        qFatal("test fixture is not valid JSON: %s", pe.errorString().toLatin1().constData());
    return d.object();
}

QByteArray json(const QJsonObject &o) { return QJsonDocument(o).toJson(QJsonDocument::Compact); }

// "M/D/YYYY h:mm:ss AM|PM" from a UTC time, the source's format.
QString libreTime(const QDateTime &utc)
{
    const QDate d = utc.date();
    const QTime t = utc.time();
    const int h12 = (t.hour() % 12) == 0 ? 12 : t.hour() % 12;
    return QString("%1/%2/%3 %4:%5:%6 %7")
        .arg(d.month()).arg(d.day()).arg(d.year()).arg(h12)
        .arg(t.minute(), 2, 10, QChar('0')).arg(t.second(), 2, 10, QChar('0'))
        .arg(t.hour() >= 12 ? "PM" : "AM");
}

QDateTime utc(int y, int mo, int d, int h, int mi, int s)
{
    return QDateTime(QDate(y, mo, d), QTime(h, mi, s), Qt::UTC);
}

QJsonObject reading(const QDateTime &when, double value, int units = 1, double mg = -1)
{
    QJsonObject r;
    r.insert("FactoryTimestamp", libreTime(when));
    r.insert("Timestamp", libreTime(when.addSecs(7200)));
    r.insert("type", 1);
    r.insert("Value", value);
    r.insert("ValueInMgPerDl", mg < 0 ? value : mg);
    r.insert("TrendArrow", 3);
    r.insert("TrendMessage", QJsonValue(QJsonValue::Null));
    r.insert("MeasurementColor", 1);
    r.insert("GlucoseUnits", units);
    r.insert("isHigh", false);
    r.insert("isLow", false);
    return r;
}

QJsonObject envelopeOk(const QJsonValue &data)
{
    QJsonObject o;
    o.insert("status", 0);
    o.insert("data", data);
    QJsonObject ticket;
    ticket.insert("token", kToken);
    ticket.insert("expires", double(kTokenExpires));
    ticket.insert("duration", 15552000000.0);
    o.insert("ticket", ticket);
    return o;
}

QJsonObject sensorObject()
{
    QJsonObject s;
    s.insert("deviceId", "");
    s.insert("sn", kSerial);
    s.insert("a", 1767225600);     // 2026-01-01T00:00:00Z
    s.insert("w", 60);
    return s;
}

QJsonObject connection(const QJsonValue &measurement)
{
    QJsonObject c;
    c.insert("id", "conn-0001");
    c.insert("patientId", "S-0001");
    c.insert("country", "XX");
    c.insert("firstName", kFirst);
    c.insert("lastName", kLast);
    c.insert("uom", 1);
    c.insert("sensor", sensorObject());
    c.insert("glucoseMeasurement", measurement);
    c.insert("glucoseItem", measurement);
    c.insert("glucoseAlarm", QJsonValue(QJsonValue::Null));
    return c;
}

QJsonObject loginReply()
{
    QJsonObject user;
    user.insert("id", kUserId);
    user.insert("firstName", kFirst);
    user.insert("lastName", kLast);
    user.insert("email", kEmail);
    user.insert("country", "XX");
    QJsonObject ticket;
    ticket.insert("token", kToken);
    ticket.insert("expires", double(kTokenExpires));
    ticket.insert("duration", 15552000000.0);
    QJsonObject data;
    data.insert("user", user);
    data.insert("authTicket", ticket);
    data.insert("messages", obj("{\"unread\":0}"));
    data.insert("trustedDeviceToken", "");
    QJsonObject o;
    o.insert("status", 0);
    o.insert("data", data);
    return o;
}

HttpResponse ok(const QJsonObject &o, int status = 200)
{
    HttpResponse r;
    r.status = status;
    r.body = json(o);
    return r;
}

HttpResponse withStatus(int status, const QByteArray &body = "{\"message\":\"x\"}")
{
    HttpResponse r;
    r.status = status;
    r.body = body;
    return r;
}

HttpResponse fault(HttpResponse::Fault f)
{
    HttpResponse r;
    r.fault = f;
    return r;
}

// Fake transport: answers by "METHOD path"; the last response of a route repeats.
class FakeTransport : public HttpTransport
{
public:
    HttpResponse send(const HttpRequest &request)
    {
        requests.append(request);
        const QString key = QString::fromLatin1(request.method) + " " + request.url.path();
        if (!routes.contains(key)) {
            HttpResponse r;
            r.status = 404;
            r.body = "{}";
            return r;
        }
        QList<HttpResponse> &q = routes[key];
        const HttpResponse r = q.first();
        if (q.size() > 1)
            q.removeFirst();
        return r;
    }
    void route(const QString &method, const QString &path, const HttpResponse &r)
    {
        routes[method + " " + path] = QList<HttpResponse>() << r;
    }
    void routeSequence(const QString &method, const QString &path, const QList<HttpResponse> &rs)
    {
        routes[method + " " + path] = rs;
    }
    static QByteArray header(const HttpRequest &r, const QByteArray &name)
    {
        for (int i = 0; i < r.headers.size(); ++i)
            if (r.headers.at(i).first.toLower() == name.toLower())
                return r.headers.at(i).second;
        return QByteArray();
    }
    int count(const QString &method, const QString &path) const
    {
        int n = 0;
        for (int i = 0; i < requests.size(); ++i)
            if (requests.at(i).method == method.toLatin1() && requests.at(i).url.path() == path)
                ++n;
        return n;
    }

    QList<HttpRequest> requests;
    QMap<QString, QList<HttpResponse> > routes;
};

struct Run
{
    int exitCode;
    QByteArray raw;
    QVector<QJsonObject> messages;

    QVector<QJsonObject> ofType(const QString &type, const QString &corr = QString()) const
    {
        QVector<QJsonObject> out;
        for (int i = 0; i < messages.size(); ++i)
            if (messages.at(i).value("type").toString() == type
                && (corr.isEmpty() || messages.at(i).value("correlation_id").toString() == corr))
                out.append(messages.at(i));
        return out;
    }
    QJsonObject first(const QString &type, const QString &corr = QString()) const
    {
        const QVector<QJsonObject> v = ofType(type, corr);
        return v.isEmpty() ? QJsonObject() : v.first();
    }
};

QByteArray line(MessageType type, const QString &id, const QJsonObject &payload = QJsonObject())
{
    return encodeJsonLine(buildMessage(type, id, QString(), payload));
}

QByteArray accept() { return encodeJsonLine(buildMessage(MessageType::Accept, "c0", "a1")); }

QByteArray authenticateLine(const QString &id, const QString &session = QString())
{
    QJsonObject cred;
    cred.insert("username", kEmail);
    cred.insert("password", kPassword);
    QJsonObject p;
    p.insert("credentials", cred);
    p.insert("session", session.isEmpty() ? QJsonValue(QJsonValue::Null) : QJsonValue(session));
    p.insert("deadline_ms", 30000);
    return line(MessageType::Authenticate, id, p);
}

QByteArray command(MessageType type, const QString &id, const QString &subject = QString(),
                   int deadline = 15000)
{
    QJsonObject p;
    p.insert("deadline_ms", deadline);
    if (!subject.isEmpty())
        p.insert("subject_ref", subject);
    return line(type, id, p);
}

QByteArray rangeLine(const QString &id, const QString &from, const QString &to,
                     const QJsonArray &feeds = QJsonArray(), const QString &subject = "S-0001")
{
    QJsonObject p;
    p.insert("deadline_ms", 60000);
    p.insert("subject_ref", subject);
    p.insert("from", from);
    p.insert("to", to);
    if (!feeds.isEmpty())
        p.insert("feeds", feeds);
    return line(MessageType::FetchRange, id, p);
}

Run runLoop(const QByteArray &input, FakeTransport &transport,
            const QString &nowIso = QString::fromLatin1(kNow))
{
    QBuffer in;
    in.setData(input);
    in.open(QIODevice::ReadOnly);
    QBuffer out;
    out.open(QIODevice::WriteOnly);

    LibreAdapterEnv env;
    env.transport = &transport;
    const UtcTime fixedNow = UtcTime::fromIso(nowIso);
    env.clock = [fixedNow]() { return fixedNow; };

    Run run;
    run.exitCode = runLibreAdapter(in, out, env);
    run.raw = out.data();
    JsonLinesDecoder dec;
    const QVector<JsonLinesItem> items = dec.feed(run.raw);
    for (int i = 0; i < items.size(); ++i) {
        if (items.at(i).kind != JsonLinesItem::Message)
            qFatal("the adapter wrote a line that is not a JSON object");
        run.messages.append(items.at(i).message);
    }
    return run;
}

// accept, configure, authenticate, then the given commands, then shutdown.
QByteArray session(const QByteArray &commands, const QString &blob = QString())
{
    QJsonObject cfg;
    cfg.insert("settings", QJsonObject());
    return accept() + line(MessageType::Configure, "c1", cfg) + authenticateLine("c2", blob)
           + commands + line(MessageType::Shutdown, "c99");
}

void routeBasicsWith(FakeTransport &t, const QJsonValue &measurement)
{
    t.route("POST", "/llu/auth/login", ok(loginReply()));
    QJsonArray list;
    list.append(connection(measurement));
    t.route("GET", "/llu/connections", ok(envelopeOk(list)));
}

void routeBasics(FakeTransport &t)
{
    routeBasicsWith(t, reading(utc(2026, 1, 1, 12, 0, 0), 112));
}

// A real HTTP server on the loopback interface, living in the test's own event loop: the
// transport blocks in a nested loop, which keeps serving this object. One request per
// connection.
class StubServer
{
public:
    StubServer() : hang(false)
    {
        QObject::connect(&server, &QTcpServer::newConnection, [this]() { accept(); });
        if (!server.listen(QHostAddress::LocalHost, 0))
            qFatal("cannot listen on the loopback interface");
    }
    QString baseUrl() const { return QString("http://127.0.0.1:%1").arg(server.serverPort()); }

    // Response by request line; a handler may also be set for full control.
    std::function<QByteArray(const QByteArray &requestHead, const QByteArray &body)> handler;
    QByteArray lastHead;
    QByteArray lastBody;
    bool hang;      // accept and never answer

    static QByteArray http(int status, const QByteArray &body, const QByteArray &extra = QByteArray())
    {
        return "HTTP/1.1 " + QByteArray::number(status) + " X\r\nContent-Type: application/json\r\n"
               + extra + "Content-Length: " + QByteArray::number(body.size())
               + "\r\nConnection: close\r\n\r\n" + body;
    }

private:
    void accept()
    {
        QTcpSocket *sock = server.nextPendingConnection();
        QByteArray *buf = new QByteArray;
        QObject::connect(sock, &QTcpSocket::readyRead, [this, sock, buf]() {
            buf->append(sock->readAll());
            const int end = buf->indexOf("\r\n\r\n");
            if (end < 0)
                return;
            const QByteArray head = buf->left(end);
            int need = 0;
            const int cl = head.toLower().indexOf("content-length:");
            if (cl >= 0)
                need = head.mid(cl + 15, head.indexOf("\r\n", cl) - cl - 15).trimmed().toInt();
            if (buf->size() < end + 4 + need)
                return;
            lastHead = head;
            lastBody = buf->mid(end + 4, need);
            if (hang)
                return;
            sock->write(handler ? handler(head, lastBody) : http(200, "{}"));
            sock->disconnectFromHost();
        });
        QObject::connect(sock, &QTcpSocket::disconnected, [sock, buf]() { delete buf; sock->deleteLater(); });
    }

    QTcpServer server;
};

} // namespace

class TstLibreAdapter : public QObject
{
    Q_OBJECT

    static Validation validate(const QJsonObject &rec, const UtcTime &now)
    {
        Record r;
        r.subject = QUuid::createUuid();
        r.metric = rec.value("metric").toString();
        r.shape = rec.value("shape").toString() == "sample" ? Record::Sample : Record::Event;
        r.value = rec.value("value").toDouble();
        r.payload = rec.value("payload").toObject();
        r.unit = rec.value("unit").toString();
        r.measuredAt = UtcTime::fromIso(rec.value("measured_at").toString());
        r.ingestedAt = now;
        r.originalTimestamp = rec.value("original_timestamp").toString();
        r.source = rec.value("source").toString();
        return validateRecord(r, now);
    }

    static QJsonArray allRecords(const Run &run, const QString &corr, const QString &feed)
    {
        QJsonArray all;
        const QVector<QJsonObject> v = run.ofType("records", corr);
        for (int i = 0; i < v.size(); ++i) {
            if (v.at(i).value("feed").toString() != feed)
                continue;
            const QJsonArray a = v.at(i).value("records").toArray();
            for (int j = 0; j < a.size(); ++j)
                all.append(a.at(j));
        }
        return all;
    }

private slots:
    // The registry in the core carries the adapter's version as data, because the core must not
    // depend on this module. This is the one place where both are visible.
    void registryVersionEqualsTheAdapterConstant()
    {
        const AdapterEntry *e = AdapterRegistry::instance().find(QStringLiteral("libre"));
        QVERIFY(e);
        QCOMPARE(e->version, libreAdapterVersion());
    }

    // ------------------------------------------------------------------ hello and lifecycle
    void helloDeclaresIdentityMetricsAndFeeds()
    {
        FakeTransport t;
        const Run run = runLoop(accept() + line(MessageType::Shutdown, "c2"), t);
        QCOMPARE(run.exitCode, int(LibreExitOk));
        const QJsonObject hello = run.messages.first();
        QCOMPARE(hello.value("type").toString(), QString("hello"));
        QCOMPARE(hello.value("id").toString(), QString("a1"));
        QCOMPARE(hello.value("version").toInt(), 0);
        QCOMPARE(hello.value("adapter").toString(), QString("libre"));
        QCOMPARE(hello.value("contract_version").toInt(), 0);
        QVERIFY(!hello.value("adapter_version").toString().isEmpty());
        const QJsonArray metrics = hello.value("metrics").toArray();
        QStringList names;
        for (int i = 0; i < metrics.size(); ++i) {
            names << metrics.at(i).toObject().value("metric").toString();
            QCOMPARE(metrics.at(i).toObject().value("units").toArray().size(), 2);
        }
        QVERIFY(names.contains("glucose"));
        QVERIFY(names.contains("logbook_entry"));
        QCOMPARE(hello.value("feeds").toArray(),
                 QJsonArray() << "current" << "graph" << "logbook");
        QCOMPARE(hello.value("budgets").toObject().value("max_records_per_batch").toInt(), 500);
        QCOMPARE(t.requests.size(), 0);     // nothing touches the network before a command
    }
    void rejectEndsTheLoop()
    {
        FakeTransport t;
        const Run run = runLoop(encodeJsonLine(buildMessage(MessageType::Reject, "c1", "a1")), t);
        QCOMPARE(run.exitCode, int(LibreExitRejected));
        QCOMPARE(run.messages.size(), 1);   // only the hello
    }
    void commandsBeforeAcceptAreIgnored()
    {
        FakeTransport t;
        const Run run = runLoop(command(MessageType::DiscoverSubjects, "c1")
                                + accept() + line(MessageType::Shutdown, "c2"), t);
        QCOMPARE(run.ofType("reply").size(), 0);
        QCOMPARE(run.ofType("error").size(), 0);
        QCOMPARE(t.requests.size(), 0);
    }
    void endOfInputWithoutShutdownIsClean()
    {
        FakeTransport t;
        const Run run = runLoop(accept(), t);
        QCOMPARE(run.exitCode, int(LibreExitOk));
    }
    void malformedLineIsLoggedAndSkipped()
    {
        FakeTransport t;
        const Run run = runLoop(accept() + "this is not json\n"
                                + line(MessageType::Configure, "c1") + line(MessageType::Shutdown, "c2"), t);
        QCOMPARE(run.ofType("log").size(), 1);
        QCOMPARE(run.first("log").value("level").toString(), QString("warn"));
        QVERIFY(!run.first("log").value("text").toString().contains("not json"));   // never quoted
        QCOMPARE(run.ofType("reply", "c1").size(), 1);
    }
    void oversizeInputStopsTheLoop()
    {
        FakeTransport t;
        const Run run = runLoop(accept() + QByteArray(kDefaultMaxLineLength + 10, 'x') + "\n"
                                + line(MessageType::Configure, "c1"), t);
        QCOMPARE(run.exitCode, int(LibreExitBadInput));
        QCOMPARE(run.ofType("reply").size(), 0);
    }

    // ------------------------------------------------------------------ transcript, contract 8
    // The core-to-adapter lines of the example in contract section 8 (Draft 0.2), word for
    // word, and the adapter-to-core lines expected back. Times are compared as times (the
    // contract allows an optional fraction), the opaque session blob is only checked for
    // presence, and the optional `budgets` and `sensor` objects may be present.
    static bool sameTime(const QString &a, const QString &b)
    {
        const UtcTime ta = UtcTime::fromIso(a), tb = UtcTime::fromIso(b);
        return ta.isValid() && tb.isValid() && ta == tb;
    }
    static bool matches(const QJsonValue &expected, const QJsonValue &actual, const QString &key,
                        const QStringList &optionalExtras)
    {
        if (key == "session")
            return actual.isString() && !actual.toString().isEmpty();
        if (key == "measured_at" || key == "watermark" || key == "from" || key == "to"
            || key == "session_expires")
            return expected.isString() && actual.isString() && sameTime(expected.toString(), actual.toString());
        if (expected.isObject()) {
            if (!actual.isObject())
                return false;
            const QJsonObject e = expected.toObject(), a = actual.toObject();
            for (QJsonObject::const_iterator it = e.begin(); it != e.end(); ++it)
                if (!a.contains(it.key()) || !matches(it.value(), a.value(it.key()), it.key(), optionalExtras))
                    return false;
            const QStringList keys = a.keys();
            for (int i = 0; i < keys.size(); ++i)
                if (!e.contains(keys.at(i)) && !optionalExtras.contains(keys.at(i)))
                    return false;
            return true;
        }
        if (expected.isArray()) {
            if (!actual.isArray() || expected.toArray().size() != actual.toArray().size())
                return false;
            for (int i = 0; i < expected.toArray().size(); ++i)
                if (!matches(expected.toArray().at(i), actual.toArray().at(i), key, optionalExtras))
                    return false;
            return true;
        }
        return expected == actual;
    }

    void transcriptOfContractSection8()
    {
        const QByteArray input =
            "{\"version\":0,\"id\":\"c1\",\"type\":\"accept\",\"correlation_id\":\"a1\"}\n"
            "{\"version\":0,\"id\":\"c2\",\"type\":\"configure\",\"correlation_id\":\"\",\"settings\":{}}\n"
            "{\"version\":0,\"id\":\"c3\",\"type\":\"authenticate\",\"correlation_id\":\"\",\"deadline_ms\":30000,"
            "\"credentials\":{\"username\":\"user@example.invalid\",\"password\":\"synthetic\"},\"session\":null}\n"
            "{\"version\":0,\"id\":\"c4\",\"type\":\"fetch_current\",\"correlation_id\":\"\",\"deadline_ms\":15000,"
            "\"subject_ref\":\"S-0001\"}\n"
            "{\"version\":0,\"id\":\"c5\",\"type\":\"shutdown\",\"correlation_id\":\"\"}\n";

        QVector<QJsonObject> expected;
        expected << obj("{\"version\":0,\"id\":\"a1\",\"type\":\"hello\",\"correlation_id\":\"\",\"adapter\":\"libre\","
                        "\"adapter_version\":\"0.1.0\",\"contract_version\":0,"
                        "\"metrics\":[{\"metric\":\"glucose\",\"units\":[\"mg/dL\",\"mmol/L\"]},"
                        "{\"metric\":\"logbook_entry\",\"units\":[\"mg/dL\",\"mmol/L\"]}],"
                        "\"feeds\":[\"current\",\"graph\",\"logbook\"]}")
                 << obj("{\"version\":0,\"id\":\"a2\",\"type\":\"reply\",\"correlation_id\":\"c2\"}")
                 << obj("{\"version\":0,\"id\":\"a3\",\"type\":\"reply\",\"correlation_id\":\"c3\","
                        "\"session\":\"opaque-synthetic-blob\",\"session_expires\":\"2026-06-30T12:00:00Z\"}")
                 << obj("{\"version\":0,\"id\":\"a4\",\"type\":\"records\",\"correlation_id\":\"c4\",\"feed\":\"current\","
                        "\"records\":[{\"shape\":\"sample\",\"subject_ref\":\"S-0001\",\"metric\":\"glucose\","
                        "\"value\":112,\"unit\":\"mg/dL\",\"measured_at\":\"2026-01-01T12:00:00Z\","
                        "\"original_timestamp\":\"1/1/2026 12:00:00 PM\",\"source\":\"libre.current\"}]}")
                 << obj("{\"version\":0,\"id\":\"a5\",\"type\":\"reply\",\"correlation_id\":\"c4\",\"records_sent\":1,"
                        "\"watermark\":\"2026-01-01T12:00:00Z\",\"coverage\":[{\"feed\":\"current\","
                        "\"from\":\"2026-01-01T12:00:00Z\",\"to\":\"2026-01-01T12:00:00Z\"}]}");

        // The fake source: a token that expires at the time of the example.
        QJsonObject login = loginReply();
        QJsonObject data = login.value("data").toObject();
        QJsonObject ticket = data.value("authTicket").toObject();
        ticket.insert("expires", 1782820800.0);       // 2026-06-30T12:00:00Z
        data.insert("authTicket", ticket);
        login.insert("data", data);
        FakeTransport t;
        routeBasics(t);
        t.route("POST", "/llu/auth/login", ok(login));

        const Run run = runLoop(input, t);
        QCOMPARE(run.exitCode, int(LibreExitOk));
        QCOMPARE(run.messages.size(), expected.size());     // no reply to shutdown, nothing unasked
        // `trend` is the optional key of CP-01 that the example of section 8 predates; its value is
        // checked in fetchCurrentCompletionCarriesTheTrendUninterpreted.
        const QStringList extras = QStringList() << "budgets" << "sensor" << "trend";
        for (int i = 0; i < expected.size(); ++i) {
            QVERIFY2(matches(expected.at(i), run.messages.at(i), QString(), extras),
                     qPrintable(QString("line %1: %2").arg(i + 1).arg(QString::fromUtf8(json(run.messages.at(i))))));
        }
        // The login went out with the example's credentials, the data call with the token.
        QCOMPARE(obj(t.requests.first().body).value("email").toString(), QString("user@example.invalid"));
        QCOMPARE(t.requests.size(), 2);
    }
    void recordsPassTheCoreValidation()
    {
        FakeTransport t;
        routeBasics(t);
        const Run run = runLoop(session(command(MessageType::FetchCurrent, "c3", "S-0001")), t);
        const QJsonObject r = allRecords(run, "c3", "current").at(0).toObject();
        const Validation v = validate(r, UtcTime::fromIso(kNow));
        QVERIFY2(v.ok(), qPrintable(v.detail));
    }

    // ------------------------------------------------------------------ authenticate
    void loginRequestAndBlob()
    {
        FakeTransport t;
        routeBasics(t);
        const Run run = runLoop(session(QByteArray()), t);
        QCOMPARE(t.requests.size(), 1);
        const HttpRequest login = t.requests.first();
        QCOMPARE(login.method, QByteArray("POST"));
        QCOMPARE(login.url.scheme(), QString("https"));
        QCOMPARE(login.url.host(), QString("api.libreview.io"));
        QCOMPARE(login.url.path(), QString("/llu/auth/login"));
        QCOMPARE(FakeTransport::header(login, "product"), QByteArray("llu.android"));
        QVERIFY(!FakeTransport::header(login, "version").isEmpty());
        QVERIFY(FakeTransport::header(login, "authorization").isEmpty());
        QVERIFY(FakeTransport::header(login, "account-id").isEmpty());
        QVERIFY(login.timeoutMs > 0 && login.timeoutMs <= 30000);
        const QJsonObject body = obj(login.body);
        QCOMPARE(body.value("email").toString(), QString(kEmail));
        QCOMPARE(body.value("password").toString(), QString(kPassword));
        QCOMPARE(body.keys().size(), 2);

        const QJsonObject reply = run.first("reply", "c2");
        QVERIFY(!reply.value("session").toString().isEmpty());
        QCOMPARE(reply.value("session_expires").toString(),
                 UtcTime::fromMSecsSinceEpoch(kTokenExpires * 1000).toIso());
        QVERIFY(!reply.contains("settings_update"));

        // The blob holds token, expiry, host and the account-id hash, and no personal data.
        const QString blob = reply.value("session").toString();
        QVERIFY(blob.contains(kToken));
        QVERIFY(!blob.contains(kUserId));
        QVERIFY(!blob.contains(kEmail));
        QVERIFY(!blob.contains(kFirst));
        QVERIFY(!blob.contains(kLast));
        QVERIFY(!blob.contains(kPassword));
    }
    void dataRequestsCarryTokenAndAccountIdHash()
    {
        FakeTransport t;
        routeBasics(t);
        runLoop(session(command(MessageType::FetchCurrent, "c3", "S-0001")), t);
        QCOMPARE(t.requests.size(), 2);
        const HttpRequest get = t.requests.at(1);
        QCOMPARE(get.method, QByteArray("GET"));
        QCOMPARE(FakeTransport::header(get, "authorization"),
                 QByteArray("Bearer ") + kToken);
        QCOMPARE(FakeTransport::header(get, "account-id"),
                 QCryptographicHash::hash(kUserId, QCryptographicHash::Sha256).toHex());
        QCOMPARE(FakeTransport::header(get, "product"), QByteArray("llu.android"));
        QVERIFY(get.body.isEmpty());
    }
    void blobIsReusedWithoutLogin()
    {
        FakeTransport t1;
        routeBasics(t1);
        const Run first = runLoop(session(QByteArray()), t1);
        const QString blob = first.first("reply", "c2").value("session").toString();

        FakeTransport t2;
        routeBasics(t2);
        const Run second = runLoop(session(QByteArray(), blob), t2);
        QCOMPARE(t2.count("POST", "/llu/auth/login"), 0);
        QCOMPARE(t2.count("GET", "/llu/connections"), 1);      // one probe
        const QJsonObject reply = second.first("reply", "c2");
        QCOMPARE(reply.value("session").toString(), blob);
        QCOMPARE(reply.value("session_expires").toString(),
                 UtcTime::fromMSecsSinceEpoch(kTokenExpires * 1000).toIso());
    }
    void rejectedBlobTriggersLogin_data()
    {
        QTest::addColumn<int>("status");
        QTest::newRow("400") << 400;
        QTest::newRow("401") << 401;
    }
    void rejectedBlobTriggersLogin()
    {
        QFETCH(int, status);
        FakeTransport t1;
        routeBasics(t1);
        const QString blob = runLoop(session(QByteArray()), t1).first("reply", "c2")
                                 .value("session").toString();

        FakeTransport t2;
        routeBasics(t2);
        t2.routeSequence("GET", "/llu/connections",
                         QList<HttpResponse>() << withStatus(status)
                                               << ok(envelopeOk(QJsonArray())));
        const Run run = runLoop(session(QByteArray(), blob), t2);
        QCOMPARE(t2.count("GET", "/llu/connections"), 1);
        QCOMPARE(t2.count("POST", "/llu/auth/login"), 1);
        QCOMPARE(run.ofType("error").size(), 0);
        QVERIFY(!run.first("reply", "c2").value("session").toString().isEmpty());
    }
    void expiredOrBrokenBlobGoesStraightToLogin()
    {
        FakeTransport t1;
        routeBasics(t1);
        const QString blob = runLoop(session(QByteArray()), t1).first("reply", "c2")
                                 .value("session").toString();
        FakeTransport t2;
        routeBasics(t2);
        // Clock after the token expiry (kTokenExpires is 2027-01-15).
        const Run late = runLoop(session(QByteArray(), blob), t2, "2027-06-01T00:00:00Z");
        QCOMPARE(t2.count("POST", "/llu/auth/login"), 1);
        QCOMPARE(t2.count("GET", "/llu/connections"), 0);
        QCOMPARE(late.ofType("error").size(), 0);

        FakeTransport t3;
        routeBasics(t3);
        runLoop(session(QByteArray(), "{not a blob"), t3);
        QCOMPARE(t3.count("POST", "/llu/auth/login"), 1);

        // A blob naming a host the adapter does not know is never used (token exfiltration).
        FakeTransport t4;
        routeBasics(t4);
        QString evil = blob;
        evil.replace("api.libreview.io", "evil.example.invalid");
        runLoop(session(QByteArray(), evil), t4);
        QCOMPARE(t4.count("POST", "/llu/auth/login"), 1);
        for (int i = 0; i < t4.requests.size(); ++i)
            QCOMPARE(t4.requests.at(i).url.host(), QString("api.libreview.io"));
    }
    void networkTroubleDuringProbeIsNotAReasonToLogIn()
    {
        FakeTransport t1;
        routeBasics(t1);
        const QString blob = runLoop(session(QByteArray()), t1).first("reply", "c2")
                                 .value("session").toString();
        FakeTransport t2;
        routeBasics(t2);
        t2.route("GET", "/llu/connections", fault(HttpResponse::NetworkError));
        const Run run = runLoop(session(QByteArray(), blob), t2);
        QCOMPARE(t2.count("POST", "/llu/auth/login"), 0);
        QCOMPARE(run.first("error", "c2").value("class").toString(), QString("transient"));
    }
    void regionalRedirectIsFollowedOnce()
    {
        FakeTransport t;
        routeBasics(t);
        t.routeSequence("POST", "/llu/auth/login",
                        QList<HttpResponse>()
                            << ok(obj("{\"status\":0,\"data\":{\"redirect\":true,\"region\":\"de\"}}"))
                            << ok(loginReply()));
        const Run run = runLoop(session(command(MessageType::FetchCurrent, "c3", "S-0001")), t);
        QCOMPARE(t.requests.size(), 3);     // login, login on the region, then the data call
        QCOMPARE(t.requests.at(0).url.host(), QString("api.libreview.io"));
        QCOMPARE(t.requests.at(1).url.host(), QString("api-de.libreview.io"));
        QCOMPARE(t.requests.at(2).url.host(), QString("api-de.libreview.io"));   // session host
        const QJsonObject reply = run.first("reply", "c2");
        QCOMPARE(reply.value("settings_update").toObject().value("region_host").toString(),
                 QString("api-de.libreview.io"));
        QVERIFY(reply.value("session").toString().contains("api-de.libreview.io"));
    }
    void redirectToAnUnknownRegionOrTwiceIsProtocolChanged()
    {
        FakeTransport t;
        routeBasics(t);
        t.route("POST", "/llu/auth/login",
                ok(obj("{\"status\":0,\"data\":{\"redirect\":true,\"region\":\"evil.example.invalid\"}}")));
        Run run = runLoop(session(QByteArray()), t);
        QCOMPARE(run.first("error", "c2").value("class").toString(), QString("protocol-changed"));
        QCOMPARE(t.requests.size(), 1);     // credentials went to the default host only

        FakeTransport t2;
        t2.route("POST", "/llu/auth/login",
                 ok(obj("{\"status\":0,\"data\":{\"redirect\":true,\"region\":\"us\"}}")));
        run = runLoop(session(QByteArray()), t2);
        QCOMPARE(run.first("error", "c2").value("class").toString(), QString("protocol-changed"));
        QCOMPARE(t2.requests.size(), 2);    // one redirect followed, the second refused
    }
    void termsAndUnexpectedLoginRepliesAreProtocolChanged_data()
    {
        QTest::addColumn<QByteArray>("body");
        QTest::newRow("terms-like step") << QByteArray(
            "{\"status\":0,\"data\":{\"step\":{\"type\":\"tou\"},\"user\":{\"id\":\"u\"}}}");
        QTest::newRow("no ticket") << QByteArray("{\"status\":0,\"data\":{\"user\":{\"id\":\"u\"}}}");
        QTest::newRow("no user id") << QByteArray(
            "{\"status\":0,\"data\":{\"authTicket\":{\"token\":\"t\",\"expires\":1800000000}}}");
        QTest::newRow("data is a string") << QByteArray("{\"status\":0,\"data\":\"x\"}");
        QTest::newRow("empty token") << QByteArray(
            "{\"status\":0,\"data\":{\"user\":{\"id\":\"u\"},\"authTicket\":{\"token\":\"\",\"expires\":1800000000}}}");
    }
    void termsAndUnexpectedLoginRepliesAreProtocolChanged()
    {
        QFETCH(QByteArray, body);
        FakeTransport t;
        HttpResponse r;
        r.status = 200;
        r.body = body;
        t.route("POST", "/llu/auth/login", r);
        const Run run = runLoop(session(QByteArray()), t);
        QCOMPARE(run.first("error", "c2").value("class").toString(), QString("protocol-changed"));
        QCOMPARE(t.requests.size(), 1);     // nothing else was sent: terms are never accepted
    }
    void wrongCredentialsAreAuthFailed()
    {
        FakeTransport t;
        t.route("POST", "/llu/auth/login", withStatus(401));
        const Run run = runLoop(session(QByteArray()), t);
        QCOMPARE(run.first("error", "c2").value("class").toString(), QString("auth-failed"));
    }
    void missingCredentialsAreAnInvalidCommandWithoutNetwork()
    {
        FakeTransport t;
        const Run run = runLoop(accept() + line(MessageType::Authenticate, "c1")
                                + line(MessageType::Shutdown, "c2"), t);
        QCOMPARE(run.first("error", "c1").value("class").toString(), QString("invalid-command"));
        QCOMPARE(t.requests.size(), 0);
    }
    void configureHostIsUsedWhenAllowedAndIgnoredOtherwise()
    {
        FakeTransport t;
        routeBasics(t);
        QJsonObject settings;
        settings.insert("region_host", "api-eu.libreview.io");
        QJsonObject p;
        p.insert("settings", settings);
        runLoop(accept() + line(MessageType::Configure, "c1", p) + authenticateLine("c2")
                + line(MessageType::Shutdown, "c3"), t);
        QCOMPARE(t.requests.first().url.host(), QString("api-eu.libreview.io"));

        FakeTransport t2;
        routeBasics(t2);
        settings.insert("region_host", "evil.example.invalid");
        p.insert("settings", settings);
        const Run run = runLoop(accept() + line(MessageType::Configure, "c1", p) + authenticateLine("c2")
                                + line(MessageType::Shutdown, "c3"), t2);
        QCOMPARE(t2.requests.first().url.host(), QString("api.libreview.io"));
        QCOMPARE(run.ofType("reply", "c1").size(), 1);
        QCOMPARE(run.ofType("log").size(), 1);
    }
    void commandsNeedAnAuthenticatedSession()
    {
        FakeTransport t;
        routeBasics(t);
        const Run run = runLoop(accept() + command(MessageType::FetchCurrent, "c1", "S-0001")
                                + command(MessageType::DiscoverSubjects, "c2")
                                + line(MessageType::Shutdown, "c3"), t);
        QCOMPARE(run.first("error", "c1").value("class").toString(), QString("auth-failed"));
        QCOMPARE(run.first("error", "c2").value("class").toString(), QString("auth-failed"));
        QCOMPARE(t.requests.size(), 0);
    }

    // ------------------------------------------------------------------ error mapping
    void errorMapping_data()
    {
        QTest::addColumn<int>("kind");          // 0 = HTTP response, 1 = fault
        QTest::addColumn<int>("status");
        QTest::addColumn<QByteArray>("body");
        QTest::addColumn<QByteArray>("retryAfter");
        QTest::addColumn<int>("fault");
        QTest::addColumn<QString>("cls");
        QTest::addColumn<int>("retry");
        QTest::addColumn<QString>("detail");

        const QByteArray msg = "{\"message\":\"invalid or expired jwt\"}";
        QTest::newRow("HTTP 400") << 0 << 400 << msg << QByteArray() << 0 << "auth-failed" << -1 << "HTTP 400";
        QTest::newRow("HTTP 401") << 0 << 401 << msg << QByteArray() << 0 << "auth-failed" << -1 << "HTTP 401";
        QTest::newRow("HTTP 200 status 4") << 0 << 200
            << QByteArray("{\"status\":4,\"error\":{\"message\":\"could not load patient\"}}")
            << QByteArray() << 0 << "transient" << -1 << "source status 4";
        QTest::newRow("HTTP 200 status 1") << 0 << 200
            << QByteArray("{\"status\":1,\"error\":{\"message\":\"failed to retrieve measurements\"}}")
            << QByteArray() << 0 << "transient" << -1 << "source status 1";
        QTest::newRow("HTTP 200 not json") << 0 << 200 << QByteArray("<html>oops</html>")
            << QByteArray() << 0 << "protocol-changed" << -1 << "";
        QTest::newRow("HTTP 200 json array") << 0 << 200 << QByteArray("[1,2]")
            << QByteArray() << 0 << "protocol-changed" << -1 << "";
        QTest::newRow("HTTP 200 no status") << 0 << 200 << QByteArray("{\"data\":[]}")
            << QByteArray() << 0 << "protocol-changed" << -1 << "";
        QTest::newRow("HTTP 200 status text") << 0 << 200 << QByteArray("{\"status\":\"0\",\"data\":[]}")
            << QByteArray() << 0 << "protocol-changed" << -1 << "";
        QTest::newRow("HTTP 200 no data") << 0 << 200 << QByteArray("{\"status\":0}")
            << QByteArray() << 0 << "protocol-changed" << -1 << "";
        QTest::newRow("HTTP 429 with hint") << 0 << 429 << QByteArray("{}") << QByteArray("120")
            << 0 << "rate-limited" << 120 << "HTTP 429";
        QTest::newRow("HTTP 429 without hint") << 0 << 429 << QByteArray("{}") << QByteArray()
            << 0 << "rate-limited" << -1 << "HTTP 429";
        QTest::newRow("HTTP 429 date hint ignored") << 0 << 429 << QByteArray("{}")
            << QByteArray("Wed, 21 Oct 2026 07:28:00 GMT") << 0 << "rate-limited" << -1 << "HTTP 429";
        QTest::newRow("HTTP 403") << 0 << 403 << QByteArray("{}") << QByteArray() << 0
            << "protocol-changed" << -1 << "HTTP 403";
        QTest::newRow("HTTP 404") << 0 << 404 << QByteArray("{}") << QByteArray() << 0
            << "protocol-changed" << -1 << "HTTP 404";
        QTest::newRow("HTTP 302") << 0 << 302 << QByteArray("") << QByteArray() << 0
            << "protocol-changed" << -1 << "HTTP 302";
        QTest::newRow("HTTP 500") << 0 << 500 << QByteArray("{}") << QByteArray() << 0
            << "transient" << -1 << "HTTP 500";
        QTest::newRow("HTTP 503") << 0 << 503 << QByteArray("") << QByteArray() << 0
            << "transient" << -1 << "HTTP 503";
        QTest::newRow("timeout") << 1 << 0 << QByteArray() << QByteArray() << int(HttpResponse::Timeout)
            << "transient" << -1 << "";
        QTest::newRow("network fault") << 1 << 0 << QByteArray() << QByteArray()
            << int(HttpResponse::NetworkError) << "transient" << -1 << "";
        QTest::newRow("reply too large") << 1 << 0 << QByteArray() << QByteArray()
            << int(HttpResponse::TooLarge) << "protocol-changed" << -1 << "";
    }
    void errorMapping()
    {
        QFETCH(int, kind);
        QFETCH(int, status);
        QFETCH(QByteArray, body);
        QFETCH(QByteArray, retryAfter);
        QFETCH(int, fault);
        QFETCH(QString, cls);
        QFETCH(int, retry);
        QFETCH(QString, detail);

        FakeTransport t;
        routeBasics(t);
        HttpResponse bad;
        if (kind == 1) {
            bad.fault = HttpResponse::Fault(fault);
        } else {
            bad.status = status;
            bad.body = body;
            bad.retryAfter = retryAfter;
        }
        t.route("GET", "/llu/connections", bad);
        const Run run = runLoop(session(command(MessageType::FetchCurrent, "c3", "S-0001")), t);
        const QJsonObject err = run.first("error", "c3");
        QCOMPARE(err.value("class").toString(), cls);
        if (retry >= 0)
            QCOMPARE(err.value("retry_after_s").toInt(), retry);
        else
            QVERIFY(!err.contains("retry_after_s"));
        if (!detail.isEmpty())
            QCOMPARE(err.value("detail").toString(), detail);
        QCOMPARE(run.ofType("records").size(), 0);
        QCOMPARE(run.ofType("reply", "c3").size(), 0);     // exactly one completion
    }
    void errorsAlsoMapOnTheLoginCall()
    {
        FakeTransport t;
        t.route("POST", "/llu/auth/login", withStatus(429, "{}"));
        Run run = runLoop(session(QByteArray()), t);
        QCOMPARE(run.first("error", "c2").value("class").toString(), QString("rate-limited"));

        FakeTransport t2;
        t2.route("POST", "/llu/auth/login", ok(obj("{\"status\":2,\"data\":null}")));
        run = runLoop(session(QByteArray()), t2);
        QCOMPARE(run.first("error", "c2").value("class").toString(), QString("transient"));

        FakeTransport t3;
        t3.route("POST", "/llu/auth/login", fault(HttpResponse::Timeout));
        run = runLoop(session(QByteArray()), t3);
        QCOMPARE(run.first("error", "c2").value("class").toString(), QString("transient"));
    }
    void lvapiHeaderChangeIsLoggedOnce()
    {
        FakeTransport t;
        routeBasics(t);
        HttpResponse r = ok(envelopeOk(QJsonArray()));
        r.lvapi = "v1.2.3";
        t.route("GET", "/llu/connections", r);
        const Run run = runLoop(session(command(MessageType::DiscoverSubjects, "c3")
                                        + command(MessageType::DiscoverSubjects, "c4")), t);
        QCOMPARE(run.ofType("log").size(), 1);
        QVERIFY(run.first("log").value("text").toString().contains("v1.2.3"));
    }
    void deadlineBecomesTheRequestTimeout()
    {
        FakeTransport t;
        routeBasics(t);
        runLoop(session(command(MessageType::FetchCurrent, "c3", "S-0001", 4000)), t);
        const HttpRequest get = t.requests.last();
        QVERIFY(get.timeoutMs > 0);
        QVERIFY(get.timeoutMs <= 4000);
        // Without a deadline the contract default applies (fetch_current 15000).
        FakeTransport t2;
        routeBasics(t2);
        runLoop(session(line(MessageType::FetchCurrent, "c3", obj("{\"subject_ref\":\"S-0001\"}"))), t2);
        QVERIFY(t2.requests.last().timeoutMs > 4000);
        QVERIFY(t2.requests.last().timeoutMs <= 15000);
    }

    // ------------------------------------------------------------------ discover_subjects
    void discoverSubjects()
    {
        FakeTransport t;
        routeBasics(t);
        const Run run = runLoop(session(command(MessageType::DiscoverSubjects, "c3", QString(), 30000)), t);
        const QJsonObject reply = run.first("reply", "c3");
        const QJsonArray subjects = reply.value("subjects").toArray();
        QCOMPARE(subjects.size(), 1);
        const QJsonObject s = subjects.at(0).toObject();
        QCOMPARE(s.value("subject_ref").toString(), QString("S-0001"));
        QCOMPARE(s.value("display_name").toString(), QString("Ada Sample"));
        QCOMPARE(s.value("unit").toString(), QString("mg/dL"));
        QCOMPARE(s.value("unit_basis").toString(), QString("code"));
        QCOMPARE(t.requests.last().url.path(), QString("/llu/connections"));
    }
    void discoverUnitForMmolAndForNoReading()
    {
        FakeTransport t;
        QJsonArray list;
        list.append(connection(reading(utc(2026, 1, 1, 12, 0, 0), 5.6, 2, 101)));
        QJsonObject second = connection(QJsonValue(QJsonValue::Null));
        second.insert("patientId", "S-0002");
        list.append(second);
        QJsonObject third = connection(reading(utc(2026, 1, 1, 12, 0, 0), 30, 9, -1));
        third.insert("patientId", "S-0003");
        QJsonObject gm = third.value("glucoseMeasurement").toObject();
        gm.remove("ValueInMgPerDl");
        third.insert("glucoseMeasurement", gm);
        list.append(third);
        t.route("POST", "/llu/auth/login", ok(loginReply()));
        t.route("GET", "/llu/connections", ok(envelopeOk(list)));
        const Run run = runLoop(session(command(MessageType::DiscoverSubjects, "c3")), t);
        const QJsonArray s = run.first("reply", "c3").value("subjects").toArray();
        QCOMPARE(s.size(), 3);
        QCOMPARE(s.at(0).toObject().value("unit").toString(), QString("mmol/L"));
        QCOMPARE(s.at(0).toObject().value("unit_basis").toString(), QString("relation"));
        QVERIFY(s.at(1).toObject().value("unit").isNull());
        QCOMPARE(s.at(1).toObject().value("unit_basis").toString(), QString("none"));
        QVERIFY(s.at(2).toObject().value("unit").isNull());      // 30: the gap
        QCOMPARE(s.at(2).toObject().value("unit_basis").toString(), QString("none"));
    }
    void discoverWithNoConnectionsIsAnEmptyList()
    {
        FakeTransport t;
        t.route("POST", "/llu/auth/login", ok(loginReply()));
        t.route("GET", "/llu/connections", ok(envelopeOk(QJsonArray())));
        Run run = runLoop(session(command(MessageType::DiscoverSubjects, "c3")), t);
        QVERIFY(run.first("reply", "c3").value("subjects").toArray().isEmpty());
        t.route("GET", "/llu/connections", ok(envelopeOk(QJsonValue(QJsonValue::Null))));
        run = runLoop(session(command(MessageType::DiscoverSubjects, "c3")), t);
        QVERIFY(run.first("reply", "c3").value("subjects").toArray().isEmpty());
    }
    void discoverWithBrokenConnectionsIsProtocolChanged()
    {
        FakeTransport t;
        t.route("POST", "/llu/auth/login", ok(loginReply()));
        t.route("GET", "/llu/connections", ok(envelopeOk(obj("{\"x\":1}"))));
        Run run = runLoop(session(command(MessageType::DiscoverSubjects, "c3")), t);
        QCOMPARE(run.first("error", "c3").value("class").toString(), QString("protocol-changed"));
        QJsonArray list;
        list.append(obj("{\"firstName\":\"x\"}"));
        t.route("GET", "/llu/connections", ok(envelopeOk(list)));
        run = runLoop(session(command(MessageType::DiscoverSubjects, "c3")), t);
        QCOMPARE(run.first("error", "c3").value("class").toString(), QString("protocol-changed"));
    }

    // ------------------------------------------------------------------ fetch_current
    void fetchCurrentCompletionHasSensorObject()
    {
        FakeTransport t;
        routeBasics(t);
        const Run run = runLoop(session(command(MessageType::FetchCurrent, "c3", "S-0001")), t);
        const QJsonObject sensor = run.first("reply", "c3").value("sensor").toObject();
        QCOMPARE(sensor.value("id").toString(),
                 QString(QCryptographicHash::hash(kSerial, QCryptographicHash::Sha256).toHex()));
        QCOMPARE(sensor.value("activated_at").toString(), QString("2026-01-01T00:00:00.000Z"));
        QCOMPARE(sensor.value("warmup_minutes").toInt(), 60);
    }
    // the arrow of the current reading travels in the completion of
    // fetch_current as an integer, uninterpreted; fetch_range has none.
    void fetchCurrentCompletionCarriesTheTrendUninterpreted()
    {
        FakeTransport t;
        routeBasics(t);                 // the default reading has TrendArrow 3
        Run run = runLoop(session(command(MessageType::FetchCurrent, "c3", "S-0001")), t);
        QVERIFY(run.first("reply", "c3").contains("trend"));
        QCOMPARE(run.first("reply", "c3").value("trend").toInt(), 3);

        // A value outside the range seen so far is passed on as it is: the meaning is the UI's.
        QJsonObject r = reading(utc(2026, 1, 1, 12, 0, 0), 112);
        r.insert("TrendArrow", 17);
        FakeTransport t2;
        routeBasicsWith(t2, r);
        run = runLoop(session(command(MessageType::FetchCurrent, "c3", "S-0001")), t2);
        QCOMPARE(run.first("reply", "c3").value("trend").toInt(), 17);
    }
    void fetchCurrentTrendIsNullWhenTheSourceGivesNoWholeNumber()
    {
        QJsonObject r = reading(utc(2026, 1, 1, 12, 0, 0), 112);
        r.remove("TrendArrow");
        FakeTransport t;
        routeBasicsWith(t, r);
        Run run = runLoop(session(command(MessageType::FetchCurrent, "c3", "S-0001")), t);
        QVERIFY(run.first("reply", "c3").contains("trend"));
        QVERIFY(run.first("reply", "c3").value("trend").isNull());

        r.insert("TrendArrow", 2.5);            // not a whole number
        FakeTransport t2;
        routeBasicsWith(t2, r);
        run = runLoop(session(command(MessageType::FetchCurrent, "c3", "S-0001")), t2);
        QVERIFY(run.first("reply", "c3").value("trend").isNull());

        r.insert("TrendArrow", QStringLiteral("3"));    // a string is not an arrow
        FakeTransport t3;
        routeBasicsWith(t3, r);
        run = runLoop(session(command(MessageType::FetchCurrent, "c3", "S-0001")), t3);
        QVERIFY(run.first("reply", "c3").value("trend").isNull());
    }
    void fetchCurrentWithoutAReadingHasANullTrend()
    {
        FakeTransport t;
        routeBasicsWith(t, QJsonValue(QJsonValue::Null));
        const Run run = runLoop(session(command(MessageType::FetchCurrent, "c3", "S-0001")), t);
        QVERIFY(run.first("reply", "c3").contains("trend"));
        QVERIFY(run.first("reply", "c3").value("trend").isNull());
    }
    void fetchRangeHasNoTrend()
    {
        FakeTransport t;
        routeBasics(t);
        Run run = runLoop(session(rangeLine("c3", "2026-01-01T11:00:00Z", "2026-01-01T13:00:00Z",
                                            QJsonArray() << "current")), t);
        QVERIFY(!run.first("reply", "c3").contains("trend"));
        run = runLoop(session(rangeLine("c3", "2026-01-01T11:00:00Z", "2026-01-01T13:00:00Z",
                                        QJsonArray() << "graph")), t);
        QVERIFY(!run.first("reply", "c3").contains("trend"));
    }
    void fetchCurrentWithoutAMeasurementIsAnEmptyReply()
    {
        FakeTransport t;
        routeBasicsWith(t, QJsonValue(QJsonValue::Null));
        const Run run = runLoop(session(command(MessageType::FetchCurrent, "c3", "S-0001")), t);
        const QJsonObject reply = run.first("reply", "c3");
        QCOMPARE(reply.value("records_sent").toInt(), 0);
        QVERIFY(reply.value("watermark").isNull());
        QVERIFY(reply.value("coverage").toArray().isEmpty());
        QCOMPARE(run.ofType("records").size(), 0);
    }
    void fetchCurrentWithNullValueIsAnEmptyReply()
    {
        // HI and LO are unknown: a null Value is not a sample.
        QJsonObject r = reading(utc(2026, 1, 1, 12, 0, 0), 112);
        r.insert("Value", QJsonValue(QJsonValue::Null));
        FakeTransport t;
        routeBasicsWith(t, r);
        const Run run = runLoop(session(command(MessageType::FetchCurrent, "c3", "S-0001")), t);
        QCOMPARE(run.first("reply", "c3").value("records_sent").toInt(), 0);
        QCOMPARE(run.ofType("log").size(), 1);        // counted and logged, not an error
        QCOMPARE(run.ofType("error").size(), 0);
    }
    void fetchCurrentWithRenamedKeysIsProtocolChanged()
    {
        FakeTransport t;
        QJsonObject conn = connection(QJsonValue(QJsonValue::Null));
        conn.remove("glucoseMeasurement");
        t.route("POST", "/llu/auth/login", ok(loginReply()));
        t.route("GET", "/llu/connections", ok(envelopeOk(QJsonArray() << conn)));
        Run run = runLoop(session(command(MessageType::FetchCurrent, "c3", "S-0001")), t);
        QCOMPARE(run.first("error", "c3").value("class").toString(), QString("protocol-changed"));

        QJsonObject r = reading(utc(2026, 1, 1, 12, 0, 0), 112);
        r.remove("Value");
        FakeTransport t2;
        routeBasicsWith(t2, r);
        run = runLoop(session(command(MessageType::FetchCurrent, "c3", "S-0001")), t2);
        QCOMPARE(run.first("error", "c3").value("class").toString(), QString("protocol-changed"));
    }
    void fetchCurrentWithBadTimestampIsProtocolChanged()
    {
        QJsonObject r = reading(utc(2026, 1, 1, 12, 0, 0), 112);
        r.insert("FactoryTimestamp", "2026-01-01 12:00:00");
        FakeTransport t;
        routeBasicsWith(t, r);
        const Run run = runLoop(session(command(MessageType::FetchCurrent, "c3", "S-0001")), t);
        QCOMPARE(run.first("error", "c3").value("class").toString(), QString("protocol-changed"));
        QCOMPARE(run.ofType("records").size(), 0);
    }
    void fetchCurrentWithUnknownSubjectIsTransient()
    {
        FakeTransport t;
        routeBasics(t);
        const Run run = runLoop(session(command(MessageType::FetchCurrent, "c3", "S-9999")), t);
        QCOMPARE(run.first("error", "c3").value("class").toString(), QString("transient"));
    }
    void fetchCurrentKeepsMmolValueExactly()
    {
        FakeTransport t;
        routeBasicsWith(t, reading(utc(2026, 1, 1, 12, 0, 0), 5.6, 2, 101));
        const Run run = runLoop(session(command(MessageType::FetchCurrent, "c3", "S-0001")), t);
        const QJsonObject r = allRecords(run, "c3", "current").at(0).toObject();
        QCOMPARE(r.value("value").toDouble(), 5.6);
        QCOMPARE(r.value("unit").toString(), QString("mmol/L"));
        QVERIFY(validate(r, UtcTime::fromIso(kNow)).ok());
    }
    void fetchCurrentOutOfRangeValueIsPassedOnForTheCoreToQuarantine()
    {
        FakeTransport t;
        routeBasicsWith(t, reading(utc(2026, 1, 1, 12, 0, 0), 650));
        const Run run = runLoop(session(command(MessageType::FetchCurrent, "c3", "S-0001")), t);
        const QJsonObject r = allRecords(run, "c3", "current").at(0).toObject();
        QCOMPARE(r.value("value").toDouble(), 650.0);
        QCOMPARE(validate(r, UtcTime::fromIso(kNow)).reason, Validation::ValueOutOfRange);
    }
    void subjectRefIsPercentEncodedInThePath()
    {
        FakeTransport t;
        routeBasics(t);
        runLoop(session(rangeLine("c3", "2026-01-01T00:00:00Z", "2026-01-02T00:00:00Z",
                                  QJsonArray() << "graph", "S/../x?y")), t);
        // The request is made only for a Subject; the path never holds a raw separator.
        const HttpRequest last = t.requests.last();
        QVERIFY(last.url.toEncoded().contains("S%2F..%2Fx%3Fy"));
    }

    // ------------------------------------------------------------------ fetch_range
    static QJsonObject graphReply(const QJsonValue &points)
    {
        QJsonObject data;
        data.insert("connection", connection(QJsonValue(QJsonValue::Null)));
        QJsonObject sensors;
        sensors.insert("sensor", sensorObject());
        sensors.insert("device", obj("{\"did\":\"d\"}"));
        data.insert("activeSensors", QJsonArray() << sensors);
        data.insert("graphData", points);
        return envelopeOk(data);
    }
    static QJsonObject logbookEntry(const QDateTime &when, double value, int type = 3)
    {
        QJsonObject e = reading(when, value);
        e.insert("type", type);
        e.insert("alarmType", 1);
        e.insert("TrendArrow", 2);
        return e;
    }

    void fetchRangeGraphAndLogbook()
    {
        FakeTransport t;
        routeBasics(t);
        QJsonArray g;
        for (int i = 0; i < 6; ++i)     // 12:00 .. 12:25 every 5 minutes
            g.append(reading(utc(2026, 1, 1, 12, 0, 0).addSecs(300 * i), 100 + i));
        t.route("GET", "/llu/connections/S-0001/graph", ok(graphReply(g)));
        QJsonArray lb;
        lb.append(logbookEntry(utc(2025, 12, 31, 3, 0, 40), 71));
        lb.append(logbookEntry(utc(2025, 12, 30, 22, 15, 44), 65));
        t.route("GET", "/llu/connections/S-0001/logbook", ok(envelopeOk(lb)));

        const Run run = runLoop(session(rangeLine("c3", "2025-12-29T00:00:00Z", "2026-01-01T23:00:00Z")), t);
        QCOMPARE(run.ofType("error").size(), 0);

        const QJsonArray graph = allRecords(run, "c3", "graph");
        QCOMPARE(graph.size(), 6);
        QCOMPARE(graph.at(0).toObject().value("source").toString(), QString("libre.graph"));
        QCOMPARE(graph.at(0).toObject().value("measured_at").toString(), QString("2026-01-01T12:00:00.000Z"));
        QCOMPARE(graph.at(5).toObject().value("value").toDouble(), 105.0);

        const QJsonArray book = allRecords(run, "c3", "logbook");
        QCOMPARE(book.size(), 2);
        // Sorted by time, oldest first.
        QCOMPARE(book.at(0).toObject().value("measured_at").toString(), QString("2025-12-30T22:15:44.000Z"));
        const QJsonObject ev = book.at(1).toObject();
        QCOMPARE(ev.value("shape").toString(), QString("event"));
        QCOMPARE(ev.value("metric").toString(), QString("logbook_entry"));
        QCOMPARE(ev.value("source").toString(), QString("libre.logbook"));
        QVERIFY(!ev.contains("value") && !ev.contains("unit"));
        const QJsonObject p = ev.value("payload").toObject();
        QCOMPARE(p.value("entry_type").toInt(), 3);
        QCOMPARE(p.value("value").toDouble(), 71.0);
        QCOMPARE(p.value("unit").toString(), QString("mg/dL"));
        QCOMPARE(p.value("trend_code").toInt(), 2);
        QCOMPARE(p.value("alarm_code").toInt(), 1);
        QCOMPARE(p.value("color_code").toInt(), 1);
        QCOMPARE(p.value("raw").toObject(), logbookEntry(utc(2025, 12, 31, 3, 0, 40), 71));
        for (int i = 0; i < book.size(); ++i)
            QVERIFY2(validate(book.at(i).toObject(), UtcTime::fromIso(kNow)).ok(), "logbook event invalid");
        for (int i = 0; i < graph.size(); ++i)
            QVERIFY2(validate(graph.at(i).toObject(), UtcTime::fromIso(kNow)).ok(), "graph sample invalid");

        const QJsonObject done = run.first("reply", "c3");
        QCOMPARE(done.value("records_sent").toInt(), 8);
        QCOMPARE(done.value("watermark").toString(), QString("2026-01-01T12:25:00.000Z"));
        QCOMPARE(done.value("sensor").toObject().value("warmup_minutes").toInt(), 60);
        const QJsonArray cov = done.value("coverage").toArray();
        QCOMPARE(cov.size(), 2);
        QCOMPARE(cov.at(0).toObject().value("feed").toString(), QString("graph"));
        QCOMPARE(cov.at(0).toObject().value("from").toString(), QString("2026-01-01T12:00:00.000Z"));
        QCOMPARE(cov.at(0).toObject().value("to").toString(), QString("2026-01-01T12:25:00.000Z"));
        // Logbook: the 13-day window up to "now" cut by the asked range.
        QCOMPARE(cov.at(1).toObject().value("feed").toString(), QString("logbook"));
        QCOMPARE(cov.at(1).toObject().value("from").toString(), QString("2025-12-29T00:00:00.000Z"));
        QCOMPARE(cov.at(1).toObject().value("to").toString(), QString("2026-01-01T23:00:00.000Z"));
        QCOMPARE(t.count("GET", "/llu/connections/S-0001/graph"), 1);
    }
    void fetchRangeFiltersWhatTheSourceReturnsOutsideTheRange()
    {
        FakeTransport t;
        routeBasics(t);
        QJsonArray g;
        for (int i = 0; i < 6; ++i)
            g.append(reading(utc(2026, 1, 1, 12, 0, 0).addSecs(300 * i), 100 + i));
        t.route("GET", "/llu/connections/S-0001/graph", ok(graphReply(g)));
        QJsonArray lb;
        lb.append(logbookEntry(utc(2025, 12, 31, 3, 0, 40), 71));
        lb.append(logbookEntry(utc(2025, 12, 25, 3, 0, 40), 80));
        t.route("GET", "/llu/connections/S-0001/logbook", ok(envelopeOk(lb)));
        // 12:10 to 12:20 inclusive; the logbook entry of Dec 31 is outside, Dec 25 too.
        const Run run = runLoop(session(rangeLine("c3", "2026-01-01T12:10:00Z", "2026-01-01T12:20:00Z")), t);
        QCOMPARE(allRecords(run, "c3", "graph").size(), 3);
        QCOMPARE(allRecords(run, "c3", "logbook").size(), 0);
        const QJsonArray cov = run.first("reply", "c3").value("coverage").toArray();
        QCOMPARE(cov.at(0).toObject().value("from").toString(), QString("2026-01-01T12:10:00.000Z"));
        QCOMPARE(cov.at(0).toObject().value("to").toString(), QString("2026-01-01T12:20:00.000Z"));
    }
    void fetchRangeCoverageIsLimitedBySourceWindows()
    {
        FakeTransport t;
        routeBasics(t);
        QJsonArray g;
        for (int i = 0; i < 6; ++i)
            g.append(reading(utc(2026, 1, 1, 12, 0, 0).addSecs(300 * i), 100 + i));
        t.route("GET", "/llu/connections/S-0001/graph", ok(graphReply(g)));
        t.route("GET", "/llu/connections/S-0001/logbook", ok(envelopeOk(QJsonArray())));
        // Asked: 30 days; the graph has 25 minutes, the logbook 13 days before "now".
        const Run run = runLoop(session(rangeLine("c3", "2025-12-01T00:00:00Z", "2026-01-05T00:00:00Z")), t);
        const QJsonArray cov = run.first("reply", "c3").value("coverage").toArray();
        QCOMPARE(cov.at(0).toObject().value("from").toString(), QString("2026-01-01T12:00:00.000Z"));
        QCOMPARE(cov.at(0).toObject().value("to").toString(), QString("2026-01-01T12:25:00.000Z"));
        QCOMPARE(cov.at(1).toObject().value("from").toString(), QString("2025-12-20T00:00:00.000Z"));
        QCOMPARE(cov.at(1).toObject().value("to").toString(), QString(kNow).replace("Z", ".000Z"));
        QCOMPARE(run.first("reply", "c3").value("records_sent").toInt(), 6);
    }
    void fetchRangeWithOnlyTheRequestedFeed()
    {
        FakeTransport t;
        routeBasics(t);
        t.route("GET", "/llu/connections/S-0001/logbook", ok(envelopeOk(QJsonArray())));
        runLoop(session(rangeLine("c3", "2026-01-01T00:00:00Z", "2026-01-02T00:00:00Z",
                                  QJsonArray() << "logbook")), t);
        QCOMPARE(t.count("GET", "/llu/connections/S-0001/logbook"), 1);
        QCOMPARE(t.count("GET", "/llu/connections/S-0001/graph"), 0);
    }
    void fetchRangeWithCurrentFeed()
    {
        FakeTransport t;
        routeBasics(t);
        Run run = runLoop(session(rangeLine("c3", "2026-01-01T11:00:00Z", "2026-01-01T13:00:00Z",
                                            QJsonArray() << "current")), t);
        QCOMPARE(allRecords(run, "c3", "current").size(), 1);
        run = runLoop(session(rangeLine("c3", "2026-01-01T13:00:00Z", "2026-01-01T14:00:00Z",
                                        QJsonArray() << "current")), t);
        QCOMPARE(allRecords(run, "c3", "current").size(), 0);
        QVERIFY(run.first("reply", "c3").value("coverage").toArray().isEmpty());
        QVERIFY(run.first("reply", "c3").value("watermark").isNull());
    }
    void fetchRangeBatchesAreLimitedTo500()
    {
        FakeTransport t;
        routeBasics(t);
        QJsonArray g;
        for (int i = 0; i < 1200; ++i)
            g.append(reading(utc(2026, 1, 1, 0, 0, 0).addSecs(60 * i), 100 + (i % 50)));
        t.route("GET", "/llu/connections/S-0001/graph", ok(graphReply(g)));
        const Run run = runLoop(session(rangeLine("c3", "2026-01-01T00:00:00Z", "2026-01-02T00:00:00Z",
                                                  QJsonArray() << "graph")), t);
        const QVector<QJsonObject> batches = run.ofType("records", "c3");
        QCOMPARE(batches.size(), 3);
        QCOMPARE(batches.at(0).value("records").toArray().size(), 500);
        QCOMPARE(batches.at(1).value("records").toArray().size(), 500);
        QCOMPARE(batches.at(2).value("records").toArray().size(), 200);
        QCOMPARE(run.first("reply", "c3").value("records_sent").toInt(), 1200);
        // The completion comes after every batch.
        QCOMPARE(run.messages.last().value("type").toString(), QString("reply"));
        // Every record is accepted by the core's validation.
        const QJsonArray all = allRecords(run, "c3", "graph");
        for (int i = 0; i < all.size(); ++i)
            QVERIFY(validate(all.at(i).toObject(), UtcTime::fromIso(kNow)).ok());
    }
    void fetchRangeMmolGraph()
    {
        FakeTransport t;
        routeBasics(t);
        QJsonArray g;
        g.append(reading(utc(2026, 1, 1, 12, 0, 0), 5.6, 2, 101));
        g.append(reading(utc(2026, 1, 1, 12, 5, 0), 5.5, 2, 99));
        t.route("GET", "/llu/connections/S-0001/graph", ok(graphReply(g)));
        const Run run = runLoop(session(rangeLine("c3", "2026-01-01T00:00:00Z", "2026-01-02T00:00:00Z",
                                                  QJsonArray() << "graph")), t);
        const QJsonArray all = allRecords(run, "c3", "graph");
        QCOMPARE(all.at(0).toObject().value("unit").toString(), QString("mmol/L"));
        QCOMPARE(all.at(0).toObject().value("value").toDouble(), 5.6);
        QCOMPARE(all.at(1).toObject().value("value").toDouble(), 5.5);
    }
    void fetchRangeNullOrMissingGraphData()
    {
        FakeTransport t;
        routeBasics(t);
        t.route("GET", "/llu/connections/S-0001/graph", ok(graphReply(QJsonValue(QJsonValue::Null))));
        Run run = runLoop(session(rangeLine("c3", "2026-01-01T00:00:00Z", "2026-01-02T00:00:00Z",
                                            QJsonArray() << "graph")), t);
        QCOMPARE(run.first("reply", "c3").value("records_sent").toInt(), 0);
        QVERIFY(run.first("reply", "c3").value("coverage").toArray().isEmpty());

        QJsonObject noKey = graphReply(QJsonValue());
        QJsonObject data = noKey.value("data").toObject();
        data.remove("graphData");
        noKey.insert("data", data);
        t.route("GET", "/llu/connections/S-0001/graph", ok(noKey));
        run = runLoop(session(rangeLine("c3", "2026-01-01T00:00:00Z", "2026-01-02T00:00:00Z",
                                        QJsonArray() << "graph")), t);
        QCOMPARE(run.first("error", "c3").value("class").toString(), QString("protocol-changed"));

        t.route("GET", "/llu/connections/S-0001/graph", ok(graphReply(QJsonValue("text"))));
        run = runLoop(session(rangeLine("c3", "2026-01-01T00:00:00Z", "2026-01-02T00:00:00Z",
                                        QJsonArray() << "graph")), t);
        QCOMPARE(run.first("error", "c3").value("class").toString(), QString("protocol-changed"));
    }
    void fetchRangeUnusablePointsAreCountedAndABurstEscalates()
    {
        // 3 of 10 points with a bad timestamp: logged, the 7 good ones are sent.
        QJsonArray g;
        for (int i = 0; i < 10; ++i) {
            QJsonObject r = reading(utc(2026, 1, 1, 12, 0, 0).addSecs(300 * i), 100 + i);
            if (i < 3)
                r.insert("FactoryTimestamp", "garbage");
            g.append(r);
        }
        FakeTransport t;
        routeBasics(t);
        t.route("GET", "/llu/connections/S-0001/graph", ok(graphReply(g)));
        Run run = runLoop(session(rangeLine("c3", "2026-01-01T00:00:00Z", "2026-01-02T00:00:00Z",
                                            QJsonArray() << "graph")), t);
        QCOMPARE(run.first("reply", "c3").value("records_sent").toInt(), 7);
        QCOMPARE(run.ofType("log").size(), 1);
        QVERIFY(!run.first("log").value("text").toString().contains("garbage"));

        // 6 of 10: the source has probably changed, nothing is sent.
        g = QJsonArray();
        for (int i = 0; i < 10; ++i) {
            QJsonObject r = reading(utc(2026, 1, 1, 12, 0, 0).addSecs(300 * i), 100 + i);
            if (i < 6)
                r.insert("FactoryTimestamp", "garbage");
            g.append(r);
        }
        t.route("GET", "/llu/connections/S-0001/graph", ok(graphReply(g)));
        run = runLoop(session(rangeLine("c3", "2026-01-01T00:00:00Z", "2026-01-02T00:00:00Z",
                                        QJsonArray() << "graph")), t);
        QCOMPARE(run.first("error", "c3").value("class").toString(), QString("protocol-changed"));
        QCOMPARE(run.ofType("records").size(), 0);
    }
    void fetchRangeUndeterminedUnitIsNotStoredAsASample()
    {
        QJsonObject odd = reading(utc(2026, 1, 1, 12, 5, 0), 30, 1, 150);   // fits neither unit
        QJsonArray g;
        g.append(reading(utc(2026, 1, 1, 12, 0, 0), 100));
        g.append(odd);
        g.append(reading(utc(2026, 1, 1, 12, 10, 0), 101));
        g.append(reading(utc(2026, 1, 1, 12, 15, 0), 102));
        g.append(reading(utc(2026, 1, 1, 12, 20, 0), 103));
        FakeTransport t;
        routeBasics(t);
        t.route("GET", "/llu/connections/S-0001/graph", ok(graphReply(g)));
        const Run run = runLoop(session(rangeLine("c3", "2026-01-01T00:00:00Z", "2026-01-02T00:00:00Z",
                                                  QJsonArray() << "graph")), t);
        QCOMPARE(allRecords(run, "c3", "graph").size(), 4);
        QCOMPARE(run.ofType("log").size(), 1);
    }
    void logbookEntriesOfAnyTypeBecomeEvents()
    {
        QJsonArray lb;
        lb.append(logbookEntry(utc(2026, 1, 1, 1, 0, 0), 71));
        // A type never seen, without Value and with extra fields: kept raw.
        QJsonObject diary = obj("{\"type\":6,\"FactoryTimestamp\":\"1/1/2026 2:00:00 AM\","
                                "\"Timestamp\":\"1/1/2026 4:00:00 AM\",\"carbs\":45,\"notes\":\"synthetic\"}");
        lb.append(diary);
        // A value that fits no unit: the event is kept, value and unit are not guessed.
        QJsonObject odd = logbookEntry(utc(2026, 1, 1, 3, 0, 0), 30);
        odd.insert("ValueInMgPerDl", 150);
        lb.append(odd);
        // Without a type or with a bad timestamp: skipped and counted.
        lb.append(obj("{\"FactoryTimestamp\":\"1/1/2026 5:00:00 AM\"}"));
        QJsonObject badTs = logbookEntry(utc(2026, 1, 1, 6, 0, 0), 80);
        badTs.insert("FactoryTimestamp", "x");
        lb.append(badTs);
        lb.append(logbookEntry(utc(2026, 1, 1, 7, 0, 0), 90));
        lb.append(logbookEntry(utc(2026, 1, 1, 8, 0, 0), 91));
        lb.append(logbookEntry(utc(2026, 1, 1, 9, 0, 0), 92));
        lb.append(logbookEntry(utc(2026, 1, 1, 10, 0, 0), 93));
        lb.append(logbookEntry(utc(2026, 1, 1, 11, 0, 0), 94));
        FakeTransport t;
        routeBasics(t);
        t.route("GET", "/llu/connections/S-0001/logbook", ok(envelopeOk(lb)));
        const Run run = runLoop(session(rangeLine("c3", "2025-12-25T00:00:00Z", "2026-01-02T00:00:00Z",
                                                  QJsonArray() << "logbook")), t);
        const QJsonArray book = allRecords(run, "c3", "logbook");
        QCOMPARE(book.size(), 8);
        const QJsonObject d = book.at(1).toObject().value("payload").toObject();
        QCOMPARE(d.value("entry_type").toInt(), 6);
        QVERIFY(d.value("value").isNull());
        QVERIFY(d.value("unit").isNull());
        QVERIFY(d.value("trend_code").isNull());
        QCOMPARE(d.value("raw").toObject(), diary);
        const QJsonObject o = book.at(2).toObject().value("payload").toObject();
        QCOMPARE(o.value("value").toDouble(), 30.0);
        QVERIFY(o.value("unit").isNull());
        for (int i = 0; i < book.size(); ++i)
            QVERIFY(validate(book.at(i).toObject(), UtcTime::fromIso(kNow)).ok());
    }
    void logbookValueOutsideTheGlucoseRangeIsStillStored()
    {
        QJsonArray lb;
        lb.append(logbookEntry(utc(2026, 1, 1, 1, 0, 0), 700));
        FakeTransport t;
        routeBasics(t);
        t.route("GET", "/llu/connections/S-0001/logbook", ok(envelopeOk(lb)));
        const Run run = runLoop(session(rangeLine("c3", "2025-12-25T00:00:00Z", "2026-01-02T00:00:00Z",
                                                  QJsonArray() << "logbook")), t);
        const QJsonObject r = allRecords(run, "c3", "logbook").at(0).toObject();
        QVERIFY(validate(r, UtcTime::fromIso(kNow)).ok());     // no range check on events
    }
    void fetchRangeFailureOfOneFeedFailsTheCommand()
    {
        FakeTransport t;
        routeBasics(t);
        t.route("GET", "/llu/connections/S-0001/graph",
                ok(graphReply(QJsonArray() << reading(utc(2026, 1, 1, 12, 0, 0), 100))));
        t.route("GET", "/llu/connections/S-0001/logbook",
                ok(obj("{\"status\":1,\"error\":{\"message\":\"failed to retrieve measurements\"}}")));
        const Run run = runLoop(session(rangeLine("c3", "2026-01-01T00:00:00Z", "2026-01-02T00:00:00Z")), t);
        QCOMPARE(run.first("error", "c3").value("class").toString(), QString("transient"));
        QCOMPARE(run.ofType("records").size(), 0);
        QCOMPARE(run.ofType("reply", "c3").size(), 0);
    }
    void invalidRangeCommandsAreRejected()
    {
        FakeTransport t;
        routeBasics(t);
        const Run run = runLoop(session(
            rangeLine("c3", "2026-01-02T00:00:00Z", "2026-01-01T00:00:00Z")
            + rangeLine("c4", "yesterday", "2026-01-01T00:00:00Z")
            + rangeLine("c5", "2026-01-01T00:00:00Z", "2026-01-02T00:00:00Z", QJsonArray(), "")
            + command(MessageType::FetchCurrent, "c6")), t);
        for (int i = 3; i <= 6; ++i)
            QCOMPARE(run.first("error", QString("c%1").arg(i)).value("class").toString(),
                     QString("invalid-command"));
        QCOMPARE(t.requests.size(), 1);     // the login only
    }
    void oneCommandAtATimeEachWithExactlyOneCompletion()
    {
        FakeTransport t;
        routeBasics(t);
        t.route("GET", "/llu/connections/S-0001/graph", ok(graphReply(QJsonArray())));
        t.route("GET", "/llu/connections/S-0001/logbook", ok(envelopeOk(QJsonArray())));
        const Run run = runLoop(session(command(MessageType::DiscoverSubjects, "c3")
                                        + command(MessageType::FetchCurrent, "c4", "S-0001")
                                        + rangeLine("c5", "2026-01-01T00:00:00Z", "2026-01-02T00:00:00Z")), t);
        for (int i = 3; i <= 5; ++i) {
            const QString id = QString("c%1").arg(i);
            QCOMPARE(run.ofType("reply", id).size() + run.ofType("error", id).size(), 1);
        }
    }

    // ------------------------------------------------------------------ the real transport
    void realTransportGetAndHeaders()
    {
        StubServer srv;
        srv.handler = [](const QByteArray &, const QByteArray &) {
            return StubServer::http(200, "{\"status\":0}", "lvapi: v1.0.1\r\n");
        };
        QtHttpTransport transport;
        HttpRequest req;
        req.method = "GET";
        req.url = QUrl(srv.baseUrl() + "/llu/connections");
        req.timeoutMs = 5000;
        req.headers << qMakePair(QByteArray("product"), QByteArray("llu.android"))
                    << qMakePair(QByteArray("Authorization"), QByteArray("Bearer T"));
        const HttpResponse r = transport.send(req);
        QCOMPARE(int(r.fault), int(HttpResponse::NoFault));
        QCOMPARE(r.status, 200);
        QCOMPARE(r.body, QByteArray("{\"status\":0}"));
        QCOMPARE(r.lvapi, QByteArray("v1.0.1"));
        QVERIFY(srv.lastHead.startsWith("GET /llu/connections "));
        QVERIFY(srv.lastHead.toLower().contains("product: llu.android"));
        QVERIFY(srv.lastHead.toLower().contains("authorization: bearer t"));
    }
    void realTransportPostBodyAndErrorStatus()
    {
        StubServer srv;
        srv.handler = [](const QByteArray &, const QByteArray &) {
            return StubServer::http(429, "{\"message\":\"slow down\"}", "Retry-After: 90\r\n");
        };
        QtHttpTransport transport;
        HttpRequest req;
        req.method = "POST";
        req.url = QUrl(srv.baseUrl() + "/llu/auth/login");
        req.timeoutMs = 5000;
        req.body = "{\"email\":\"a\"}";
        req.headers << qMakePair(QByteArray("Content-Type"), QByteArray("application/json"));
        const HttpResponse r = transport.send(req);
        QCOMPARE(r.status, 429);                       // an HTTP error is an answer, not a fault
        QCOMPARE(r.retryAfter, QByteArray("90"));
        QCOMPARE(r.body, QByteArray("{\"message\":\"slow down\"}"));
        QCOMPARE(srv.lastBody, req.body);
        QVERIFY(srv.lastHead.startsWith("POST /llu/auth/login "));
    }
    void realTransportTimeout()
    {
        StubServer srv;
        srv.hang = true;
        QtHttpTransport transport;
        HttpRequest req;
        req.method = "GET";
        req.url = QUrl(srv.baseUrl() + "/x");
        req.timeoutMs = 300;
        QElapsedTimer clock;
        clock.start();
        const HttpResponse r = transport.send(req);
        QCOMPARE(int(r.fault), int(HttpResponse::Timeout));
        QVERIFY(clock.elapsed() < 3000);
        req.timeoutMs = 0;      // no time left: no request at all
        QCOMPARE(int(transport.send(req).fault), int(HttpResponse::Timeout));
    }
    void realTransportConnectionRefused()
    {
        QString url;
        {
            StubServer srv;
            url = srv.baseUrl() + "/x";
        }
        QtHttpTransport transport;
        HttpRequest req;
        req.method = "GET";
        req.url = QUrl(url);
        req.timeoutMs = 3000;
        QCOMPARE(int(transport.send(req).fault), int(HttpResponse::NetworkError));
    }
    void realTransportCapsTheReplySize()
    {
        StubServer srv;
        srv.handler = [](const QByteArray &, const QByteArray &) {
            return StubServer::http(200, QByteArray(QtHttpTransport::kMaxReplyBytes + 1024 * 1024, 'a'));
        };
        QtHttpTransport transport;
        HttpRequest req;
        req.method = "GET";
        req.url = QUrl(srv.baseUrl() + "/big");
        req.timeoutMs = 10000;
        const HttpResponse r = transport.send(req);
        QCOMPARE(int(r.fault), int(HttpResponse::TooLarge));
        QVERIFY(r.body.isEmpty());
    }
    void wholeAdapterOverRealHttpWithTheDevelopmentBaseUrl()
    {
        StubServer srv;
        srv.handler = [](const QByteArray &head, const QByteArray &) {
            if (head.startsWith("POST /llu/auth/login "))
                return StubServer::http(200, json(loginReply()));
            if (head.startsWith("GET /llu/connections "))
                return StubServer::http(200, json(envelopeOk(QJsonArray()
                    << connection(reading(utc(2026, 1, 1, 12, 0, 0), 112)))));
            return StubServer::http(404, "{}");
        };
        QBuffer in;
        in.setData(session(command(MessageType::DiscoverSubjects, "c3")));
        in.open(QIODevice::ReadOnly);
        QBuffer out;
        out.open(QIODevice::WriteOnly);
        LibreAdapterEnv env;
        env.config.baseUrlOverride = srv.baseUrl();
        const UtcTime fixedNow = UtcTime::fromIso(kNow);
        env.clock = [fixedNow]() { return fixedNow; };
        QCOMPARE(runLibreAdapter(in, out, env), int(LibreExitOk));

        JsonLinesDecoder dec;
        const QVector<JsonLinesItem> items = dec.feed(out.data());
        QJsonObject subjects;
        for (int i = 0; i < items.size(); ++i)
            if (items.at(i).message.value("correlation_id").toString() == "c3")
                subjects = items.at(i).message;
        QCOMPARE(subjects.value("type").toString(), QString("reply"));
        QCOMPARE(subjects.value("subjects").toArray().at(0).toObject().value("subject_ref").toString(),
                 QString("S-0001"));
        // The data call carried the bearer token and the account-id hash.
        QVERIFY(srv.lastHead.toLower().contains(QByteArray("authorization: bearer ").append(QByteArray(kToken).toLower())));
        QVERIFY(srv.lastHead.toLower().contains("account-id: " + QCryptographicHash::hash(kUserId, QCryptographicHash::Sha256).toHex()));
    }

    // ------------------------------------------------------------------ secrecy and stdout
    void nothingSecretOrPersonalLeavesTheAdapter()
    {
        FakeTransport t;
        routeBasics(t);
        // Make the source misbehave in every way that could tempt an implementation to echo
        // a body: an error body with the secrets inside, bad data with the name inside.
        QJsonArray g;
        QJsonObject bad = reading(utc(2026, 1, 1, 12, 0, 0), 100);
        bad.insert("FactoryTimestamp", QString("tok-") + kToken + " " + kFirst);
        g.append(bad);
        t.route("GET", "/llu/connections/S-0001/graph", ok(graphReply(g)));
        t.route("GET", "/llu/connections/S-0001/logbook",
                withStatus(500, QByteArray("{\"message\":\"") + kToken + " " + kEmail + " " + kPassword + "\"}"));

        const Run run = runLoop(session(
            command(MessageType::DiscoverSubjects, "c3")
            + command(MessageType::FetchCurrent, "c4", "S-0001")
            + rangeLine("c5", "2026-01-01T00:00:00Z", "2026-01-02T00:00:00Z", QJsonArray() << "graph")
            + rangeLine("c6", "2026-01-01T00:00:00Z", "2026-01-02T00:00:00Z", QJsonArray() << "logbook")), t);

        // The name appears once, in the discover reply, and nowhere else. The token appears
        // only in the session blob of the authenticate reply, which the core keeps secret.
        for (int i = 0; i < run.messages.size(); ++i) {
            const QJsonObject m = run.messages.at(i);
            const QByteArray text = json(m);
            const bool discover = m.value("correlation_id").toString() == "c3" && m.value("type").toString() == "reply";
            const bool authReply = m.value("correlation_id").toString() == "c2" && m.value("type").toString() == "reply";
            QVERIFY2(!text.contains(kSerial), text.constData());     // only its hash is sent
            QVERIFY2(!text.contains(kPassword), text.constData());
            QVERIFY2(!text.contains(kEmail), text.constData());
            QVERIFY2(!text.contains(kUserId), text.constData());
            QVERIFY2(!text.contains("Bearer"), text.constData());
            if (!authReply)     // the blob holds the account-id hash by design
                QVERIFY2(!text.contains(QCryptographicHash::hash(kUserId, QCryptographicHash::Sha256).toHex()),
                         text.constData());
            if (!discover) {
                QVERIFY2(!text.contains(kFirst), text.constData());
                QVERIFY2(!text.contains(kLast), text.constData());
            }
            if (!authReply)
                QVERIFY2(!text.contains(kToken), text.constData());
        }
        QVERIFY(run.ofType("error", "c5").size() + run.ofType("reply", "c5").size() == 1);
        QCOMPARE(run.first("error", "c6").value("class").toString(), QString("transient"));
    }
    void standardOutputCarriesProtocolLinesOnly()
    {
        // runLoop() already aborts when a line is not a JSON object; here also check that
        // every line is a complete message with the header and ends with a newline.
        FakeTransport t;
        routeBasics(t);
        const Run run = runLoop(session(command(MessageType::DiscoverSubjects, "c3")), t);
        QVERIFY(run.raw.endsWith('\n'));
        QCOMPARE(run.raw.count('\n'), run.messages.size());
        for (int i = 0; i < run.messages.size(); ++i) {
            Envelope env;
            EnvelopeError ee;
            QVERIFY(parseEnvelope(run.messages.at(i), &env, &ee));
        }
    }
};

QTEST_GUILESS_MAIN(TstLibreAdapter)
#include "tst_libreadapter.moc"
