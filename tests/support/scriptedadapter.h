// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Michele Brami
#ifndef LM_SCRIPTEDADAPTER_H
#define LM_SCRIPTEDADAPTER_H

// A fake adapter that answers like the real one over in-memory pipes, for tests of the pieces
// above the driver (the service: discovery, the whole service pipeline). Where fakeadapter.h
// lets a test say each line by hand, this one runs the adapter's side of the
// protocol by itself: hello, accept, configure, then a reply to every command from a script.
// It speaks the contract so the REAL driver, guard and scheduler are
// exercised; only the process and the network are missing.
//
// Use: make an AdapterScript, set the hooks you need, and hand `factory(script)` to
// CoreOptions::channelFactory. Several channels (several processes) share one script, which
// counts what they were asked. The script also keeps the credentials it was given, so a test
// can check what the core sent; test code only, never a real secret.
//
// Every answer is posted to the event loop, never given while the driver is still inside its
// own write, because a real process cannot answer that fast either and the driver does not
// expect it.

#include "jsonl/jsonlcodec.h"
#include "protocol/envelope.h"
#include "session/adapterlink.h"
#include "session/adapterregistry.h"

#include <QBuffer>
#include <QJsonArray>
#include <QJsonDocument>
#include <QPointer>
#include <QTimer>

#include <functional>
#include <memory>

namespace lm {
namespace testing {

struct AdapterAnswer
{
    AdapterAnswer() : ok(true), retryAfterS(-1), silent(false) {}

    static AdapterAnswer error(const QString &errorClass, int retryAfterS = -1)
    {
        AdapterAnswer a;
        a.ok = false;
        a.errorClass = errorClass;
        a.retryAfterS = retryAfterS;
        return a;
    }
    static AdapterAnswer never() { AdapterAnswer a; a.silent = true; return a; }

    bool ok;
    QString errorClass;
    int retryAfterS;
    bool silent;                // the adapter never answers (a hung process)
    QJsonObject payload;        // the reply's fields, for a success
    QJsonArray records;         // fetch_current: sent as one `records` message before the reply
    QString rawLine;            // when set, written instead of any message (a malformed adapter)
};

struct AdapterScript
{
    AdapterScript() : spawned(0), authenticates(0), discovers(0), fetches(0), ended(0) {}

    // Hooks; a hook that is not set answers with a plain success.
    std::function<AdapterAnswer(const QString &username, const QString &password, const QString &session)> authenticate;
    std::function<AdapterAnswer()> discover;
    std::function<AdapterAnswer(const QString &subjectRef)> fetchCurrent;
    std::function<AdapterAnswer(const QString &subjectRef, const QStringList &feeds)> fetchRange;
    // Called when a channel is closed by the core (shutdown or kill).
    std::function<void()> onEnd;

    QString adapterVersion;     // empty = the registry's
    bool silentHello = false;   // never says hello

    int spawned, authenticates, discovers, fetches, ended;
    // What the core sent. Credentials are INCLUDED here: test code only.
    QString lastUsername, lastPassword, lastSession;
    QList<QString> commandTypes;
};

typedef std::shared_ptr<AdapterScript> AdapterScriptPtr;

// The core's writes arrive here; complete lines go to a callback.
class LineSinkDevice : public QIODevice
{
    Q_OBJECT
public:
    LineSinkDevice() { open(QIODevice::WriteOnly); }
    std::function<void(const QByteArray &)> onLine;
    bool isSequential() const { return true; }

protected:
    qint64 readData(char *, qint64) { return -1; }
    qint64 writeData(const char *in, qint64 n)
    {
        m_buffer.append(in, int(n));
        int nl;
        while ((nl = m_buffer.indexOf('\n')) >= 0) {
            const QByteArray line = m_buffer.left(nl);
            m_buffer.remove(0, nl + 1);
            if (onLine)
                onLine(line);
        }
        return n;
    }

private:
    QByteArray m_buffer;
};

class ScriptedPipe : public QIODevice
{
    Q_OBJECT
public:
    ScriptedPipe() { open(QIODevice::ReadOnly); }
    void push(const QByteArray &data) { m_data.append(data); emit readyRead(); }
    qint64 bytesAvailable() const { return m_data.size() + QIODevice::bytesAvailable(); }
    bool isSequential() const { return true; }

protected:
    qint64 readData(char *out, qint64 max)
    {
        const qint64 n = qMin<qint64>(max, m_data.size());
        memcpy(out, m_data.constData(), n);
        m_data.remove(0, n);
        return n;
    }
    qint64 writeData(const char *, qint64) { return -1; }

private:
    QByteArray m_data;
};

class ScriptedAdapterChannel : public LinkChannel
{
    Q_OBJECT
public:
    explicit ScriptedAdapterChannel(const AdapterScriptPtr &script)
        : m_script(script), m_running(false), m_nextId(1)
    {
        m_in.onLine = [this](const QByteArray &line) {
            // Answered from the event loop (see the file comment).
            QTimer::singleShot(0, this, [this, line]() { handle(line); });
        };
    }

    void open()
    {
        ++m_script->spawned;
        m_running = true;
        emit started();
        if (!m_script->silentHello)
            QTimer::singleShot(0, this, [this]() { if (m_running) sayHello(); });
    }
    QIODevice *fromAdapter() { return &m_out; }
    QIODevice *toAdapter() { return &m_in; }
    bool isRunning() const { return m_running; }
    void kill() { end(-1, true); }

private:
    QString nextId() { return QStringLiteral("a%1").arg(m_nextId++); }

    void say(MessageType type, const QString &correlation, const QJsonObject &payload = QJsonObject())
    {
        m_out.push(encodeJsonLine(buildMessage(type, nextId(), correlation, payload)));
    }

    void sayHello()
    {
        QJsonArray units;
        units.append(QStringLiteral("mg/dL"));
        units.append(QStringLiteral("mmol/L"));
        QJsonObject glucose, logbook;
        glucose.insert(QStringLiteral("metric"), QStringLiteral("glucose"));
        glucose.insert(QStringLiteral("units"), units);
        logbook.insert(QStringLiteral("metric"), QStringLiteral("logbook_entry"));
        logbook.insert(QStringLiteral("units"), units);
        QJsonArray feeds;
        feeds.append(QStringLiteral("current"));
        feeds.append(QStringLiteral("graph"));
        feeds.append(QStringLiteral("logbook"));
        const AdapterEntry *entry = AdapterRegistry::instance().find(QStringLiteral("libre"));
        QJsonObject p;
        p.insert(QStringLiteral("adapter"), QStringLiteral("libre"));
        p.insert(QStringLiteral("adapter_version"),
                 !m_script->adapterVersion.isEmpty() ? m_script->adapterVersion : (entry ? entry->version : QString()));
        p.insert(QStringLiteral("contract_version"), 0);
        p.insert(QStringLiteral("metrics"), QJsonArray() << glucose << logbook);
        p.insert(QStringLiteral("feeds"), feeds);
        say(MessageType::Hello, QString(), p);
    }

    void reply(const QString &correlation, const AdapterAnswer &a)
    {
        if (a.silent)
            return;
        if (!a.rawLine.isEmpty()) {
            m_out.push(a.rawLine.toUtf8() + "\n");
            return;
        }
        if (!a.ok) {
            QJsonObject p;
            p.insert(QStringLiteral("class"), a.errorClass);
            p.insert(QStringLiteral("detail"), QStringLiteral("scripted failure"));
            if (a.retryAfterS >= 0)
                p.insert(QStringLiteral("retry_after_s"), a.retryAfterS);
            say(MessageType::Error, correlation, p);
            return;
        }
        say(MessageType::Reply, correlation, a.payload);
    }

    void handle(const QByteArray &line)
    {
        if (!m_running)
            return;
        const QJsonObject cmd = QJsonDocument::fromJson(line).object();
        const QString type = cmd.value(QStringLiteral("type")).toString();
        const QString id = cmd.value(QStringLiteral("id")).toString();
        m_script->commandTypes.append(type);
        if (type == QLatin1String("accept")) {
            return;
        } else if (type == QLatin1String("configure")) {
            say(MessageType::Reply, id);
        } else if (type == QLatin1String("authenticate")) {
            ++m_script->authenticates;
            const QJsonObject creds = cmd.value(QStringLiteral("credentials")).toObject();
            m_script->lastUsername = creds.value(QStringLiteral("username")).toString();
            m_script->lastPassword = creds.value(QStringLiteral("password")).toString();
            m_script->lastSession = cmd.value(QStringLiteral("session")).toString();
            AdapterAnswer a;
            if (m_script->authenticate)
                a = m_script->authenticate(m_script->lastUsername, m_script->lastPassword, m_script->lastSession);
            if (a.ok && !a.payload.contains(QStringLiteral("session")))
                a.payload.insert(QStringLiteral("session"), QStringLiteral("opaque-scripted-session"));
            reply(id, a);
        } else if (type == QLatin1String("discover_subjects")) {
            ++m_script->discovers;
            AdapterAnswer a;
            if (m_script->discover)
                a = m_script->discover();
            if (a.ok && !a.payload.contains(QStringLiteral("subjects")))
                a.payload.insert(QStringLiteral("subjects"), QJsonArray());
            reply(id, a);
        } else if (type == QLatin1String("fetch_current")) {
            ++m_script->fetches;
            const QString ref = cmd.value(QStringLiteral("subject_ref")).toString();
            AdapterAnswer a;
            if (m_script->fetchCurrent)
                a = m_script->fetchCurrent(ref);
            completeFetch(id, QStringLiteral("current"), a);
        } else if (type == QLatin1String("fetch_range")) {
            ++m_script->fetches;
            const QString ref = cmd.value(QStringLiteral("subject_ref")).toString();
            QStringList feeds;
            const QJsonArray fa = cmd.value(QStringLiteral("feeds")).toArray();
            for (int i = 0; i < fa.size(); ++i)
                feeds << fa.at(i).toString();
            AdapterAnswer a;
            if (m_script->fetchRange)
                a = m_script->fetchRange(ref, feeds);
            completeFetch(id, feeds.value(0, QStringLiteral("graph")), a);
        } else if (type == QLatin1String("shutdown")) {
            end(0, false);
        }
    }

    // records first, then the completion with the counters the driver checks.
    void completeFetch(const QString &id, const QString &feed, AdapterAnswer a)
    {
        if (a.silent || !a.rawLine.isEmpty() || !a.ok) {
            reply(id, a);
            return;
        }
        QString newest;
        if (!a.records.isEmpty()) {
            QJsonObject recs;
            recs.insert(QStringLiteral("feed"), feed);
            recs.insert(QStringLiteral("records"), a.records);
            say(MessageType::Records, id, recs);
            for (int i = 0; i < a.records.size(); ++i) {
                const QString t = a.records.at(i).toObject().value(QStringLiteral("measured_at")).toString();
                if (t > newest)         // ISO strings of one format order like the times
                    newest = t;
            }
        }
        QJsonObject p = a.payload;
        p.insert(QStringLiteral("records_sent"), a.records.size());
        if (!p.contains(QStringLiteral("watermark")))
            p.insert(QStringLiteral("watermark"), newest.isEmpty() ? QJsonValue(QJsonValue::Null) : QJsonValue(newest));
        if (!p.contains(QStringLiteral("coverage"))) {
            QJsonArray coverage;
            if (!newest.isEmpty()) {
                QJsonObject c;
                c.insert(QStringLiteral("feed"), feed);
                c.insert(QStringLiteral("from"), newest);
                c.insert(QStringLiteral("to"), newest);
                coverage.append(c);
            }
            p.insert(QStringLiteral("coverage"), coverage);
        }
        say(MessageType::Reply, id, p);
    }

    void end(int code, bool crashed)
    {
        if (!m_running)
            return;
        m_running = false;
        ++m_script->ended;
        if (m_script->onEnd)
            m_script->onEnd();
        emit finished(code, crashed);
    }

    AdapterScriptPtr m_script;
    ScriptedPipe m_out;
    LineSinkDevice m_in;
    bool m_running;
    int m_nextId;
};

// The factory to give CoreOptions::channelFactory (or AdapterLink::setChannelFactory).
inline ChannelFactory scriptedChannels(const AdapterScriptPtr &script)
{
    return [script](const AdapterEntry &) -> LinkChannel * { return new ScriptedAdapterChannel(script); };
}

// A glucose sample in the contract's record shape, for fetch_current.
inline QJsonObject scriptedSample(const QString &subjectRef, const UtcTime &measuredAt, double value = 112)
{
    QJsonObject r;
    r.insert(QStringLiteral("shape"), QStringLiteral("sample"));
    r.insert(QStringLiteral("subject_ref"), subjectRef);
    r.insert(QStringLiteral("metric"), QStringLiteral("glucose"));
    r.insert(QStringLiteral("value"), value);
    r.insert(QStringLiteral("unit"), QStringLiteral("mg/dL"));
    r.insert(QStringLiteral("measured_at"), measuredAt.toIso());
    r.insert(QStringLiteral("original_timestamp"), QStringLiteral("synthetic"));
    r.insert(QStringLiteral("source"), QStringLiteral("libre.current"));
    return r;
}

// One entry of a discover_subjects reply.
inline QJsonObject scriptedSubject(const QString &ref, const QString &name, const QString &unit,
                                   const QString &basis = QStringLiteral("code"))
{
    QJsonObject o;
    o.insert(QStringLiteral("subject_ref"), ref);
    o.insert(QStringLiteral("display_name"), name);
    o.insert(QStringLiteral("unit"), unit.isEmpty() ? QJsonValue(QJsonValue::Null) : QJsonValue(unit));
    o.insert(QStringLiteral("unit_basis"), basis);
    return o;
}

} // namespace testing
} // namespace lm

#endif
