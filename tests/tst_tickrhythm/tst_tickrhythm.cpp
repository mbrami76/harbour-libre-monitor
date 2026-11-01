// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Michele Brami
//
// the rhythm of tick() must not change what the guard decides. This test wires
// the REAL pieces together: the scheduler and its session manager, the real guard and the REAL
// session driver (AdapterLink) over a scripted adapter that lives on in-memory pipes. Time for
// the scheduler and the guard is a manual clock; the adapter answers one step per virtual
// second, so a tick can fall before, between or after any stage of a start, an authenticate, a
// fetch and a stop. Nothing here contacts a real source; every value is synthetic.
//
// The earlier scheduler tests use a scripted link that stands in for the driver, so they could
// not see the fault, which sat in the driver: they never went through the driver's
// own start sequence. This program does.
#include "schedtest.h"

#include "fakeadapter.h"
#include "jsonl/jsonlcodec.h"
#include "protocol/envelope.h"
#include "scheduler/adapterlinkport.h"

#include <QCoreApplication>
#include <QPointer>
#include <QtTest>

#include <deque>

using namespace lm;
using namespace lm::testing;

namespace {

const qint64 kSec = 1000;
const qint64 kMinute = 60 * kSec;

QUuid sid()
{
    static const QUuid ns("{6ba7b810-9dad-11d1-80b4-00c04fd430c8}");
    return QUuid::createUuidV5(ns, QStringLiteral("tickrhythm-subject"));
}

QJsonObject helloPayload()
{
    QJsonArray metrics, feeds;
    QJsonObject glucose, logbook;
    glucose.insert("metric", "glucose");
    glucose.insert("units", QJsonArray() << "mg/dL" << "mmol/L");
    logbook.insert("metric", "logbook_entry");
    logbook.insert("units", QJsonArray() << "mg/dL" << "mmol/L");
    metrics.append(glucose);
    metrics.append(logbook);
    feeds << "current" << "graph" << "logbook";
    QJsonObject p;
    p.insert("adapter", "libre");
    p.insert("adapter_version", "0.1.0");
    p.insert("contract_version", 0);
    p.insert("metrics", metrics);
    p.insert("feeds", feeds);
    return p;
}

// The scripted source side, shared by all channels of one run. Everything the adapter would do
// on its own (come up, answer) is put on `held` and released by the test one step at a time, so
// that the test decides where the ticks fall.
struct Hub
{
    Hub() : clock(0), race(false), opened(0), notReady(0), maxNotReady(0), fetchCurrents(0),
            faultAtFetch(-1), ids(0) {}

    ManualClock *clock;
    bool race;                  // hello is read ahead of the `started` signal (cause)
    int opened;                 // adapter processes started
    int notReady;               // started but not yet configured, all links
    int maxNotReady;
    int fetchCurrents;
    int faultAtFetch;           // this fetch_current claims one record more than it sent
    int ids;
    std::deque<std::function<void()> > held;

    QString nextId() { return QStringLiteral("a%1").arg(++ids); }
    void hold(const std::function<void()> &f) { held.push_back(f); }
    bool releaseOne()
    {
        if (held.empty())
            return false;
        std::function<void()> f = held.front();
        held.pop_front();
        f();
        return true;
    }
};

// Driver to adapter: collects bytes and hands each complete line to a function.
class Wire : public QIODevice
{
public:
    explicit Wire(const std::function<void(const QByteArray &)> &onLine) : m_onLine(onLine)
    {
        open(QIODevice::WriteOnly);
    }
    bool isSequential() const { return true; }

protected:
    qint64 readData(char *, qint64) { return -1; }
    qint64 writeData(const char *in, qint64 n)
    {
        m_buf.append(in, int(n));
        int nl;
        while ((nl = m_buf.indexOf('\n')) >= 0) {
            const QByteArray line = m_buf.left(nl);
            m_buf.remove(0, nl + 1);
            m_onLine(line);
        }
        return n;
    }

private:
    QByteArray m_buf;
    std::function<void(const QByteArray &)> m_onLine;
};

class ScriptChannel : public LinkChannel
{
public:
    explicit ScriptChannel(Hub *hub)
        : m_hub(hub), m_running(false), m_configured(false),
          m_wire([this](const QByteArray &line) { onDriverLine(line); }) {}
    ~ScriptChannel() { leaveNotReady(); }

    void open()
    {
        m_running = true;
        ++m_hub->opened;
        if (++m_hub->notReady > m_hub->maxNotReady)
            m_hub->maxNotReady = m_hub->notReady;
        QPointer<ScriptChannel> self(this);
        if (m_hub->race) {
            // One pass of the event loop: the first output and the start notification together,
            // the output first, as a real process can deliver them.
            m_hub->hold([self]() {
                if (!self || !self->m_running) return;
                self->m_pipe.push(encodeJsonLine(
                    buildMessage(MessageType::Hello, self->m_hub->nextId(), "", helloPayload())));
                emit self->started();
            });
        } else {
            m_hub->hold([self]() { if (self && self->m_running) emit self->started(); });
            m_hub->hold([self]() {
                if (self && self->m_running)
                    self->m_pipe.push(encodeJsonLine(
                        buildMessage(MessageType::Hello, self->m_hub->nextId(), "", helloPayload())));
            });
        }
    }
    QIODevice *fromAdapter() { return &m_pipe; }
    QIODevice *toAdapter() { return &m_wire; }
    bool isRunning() const { return m_running; }
    void kill()
    {
        if (!m_running)
            return;
        m_running = false;
        leaveNotReady();
        emit finished(-1, true);
    }

private:
    void leaveNotReady()
    {
        if (!m_configured) {
            m_configured = true;
            --m_hub->notReady;
        }
    }

    void say(const QJsonObject &o) { m_pipe.push(encodeJsonLine(o)); }

    void reply(const QString &correlation, const QJsonObject &payload)
    {
        QPointer<ScriptChannel> self(this);
        m_hub->hold([self, correlation, payload]() {
            if (self && self->m_running)
                self->say(buildMessage(MessageType::Reply, self->m_hub->nextId(), correlation, payload));
        });
    }

    void onDriverLine(const QByteArray &line)
    {
        const QJsonObject m = QJsonDocument::fromJson(line).object();
        const QString type = m.value("type").toString();
        const QString id = m.value("id").toString();
        QPointer<ScriptChannel> self(this);
        if (type == "configure") {
            reply(id, QJsonObject());
            m_hub->hold([self]() { if (self) self->leaveNotReady(); });
        } else if (type == "authenticate") {
            QJsonObject p;
            p.insert("session", "SESSION-synthetic-1");
            p.insert("session_expires",
                     UtcTime::fromMSecsSinceEpoch(m_hub->clock->ms() + 180LL * 24 * 3600 * 1000).toIso());
            reply(id, p);
        } else if (type == "fetch_current") {
            const int n = ++m_hub->fetchCurrents;
            const qint64 stamp = ((m_hub->clock->ms() - 7000) / 60000) * 60000;
            QJsonObject r;
            r.insert("shape", "sample");
            r.insert("subject_ref", m.value("subject_ref").toString());
            r.insert("metric", "glucose");
            r.insert("value", 110);
            r.insert("unit", "mg/dL");
            r.insert("measured_at", UtcTime::fromMSecsSinceEpoch(stamp).toIso());
            r.insert("original_timestamp", "synthetic");
            r.insert("source", "libre.current");
            QJsonObject rp;
            rp.insert("feed", "current");
            rp.insert("records", QJsonArray() << r);
            QJsonObject cov;
            cov.insert("feed", "current");
            cov.insert("from", UtcTime::fromMSecsSinceEpoch(stamp).toIso());
            cov.insert("to", UtcTime::fromMSecsSinceEpoch(stamp).toIso());
            QJsonObject done;
            done.insert("records_sent", n == m_hub->faultAtFetch ? 2 : 1);   // a soft violation
            done.insert("watermark", UtcTime::fromMSecsSinceEpoch(stamp).toIso());
            done.insert("coverage", QJsonArray() << cov);
            m_hub->hold([self, id, rp]() {
                if (self && self->m_running)
                    self->say(buildMessage(MessageType::Records, self->m_hub->nextId(), id, rp));
            });
            reply(id, done);
        } else if (type == "fetch_range") {
            QJsonObject done;
            done.insert("records_sent", 0);
            done.insert("watermark", QJsonValue(QJsonValue::Null));
            done.insert("coverage", QJsonArray());
            reply(id, done);
        } else if (type == "shutdown") {
            m_hub->hold([self]() {
                if (self && self->m_running) {
                    self->m_running = false;
                    self->leaveNotReady();
                    emit self->finished(0, false);
                }
            });
        }
    }

    Hub *m_hub;
    bool m_running;
    bool m_configured;
    PipeDevice m_pipe;
    Wire m_wire;
};

// The real guard, scheduler and driver over the scripted adapter.
struct Rig
{
    Rig() : links([this](const QString &) { return makeLink(); })
    {
        hub.clock = &w.clock;
        SchedulerPorts p;
        p.clock = &w.clock;
        p.random = &w.random;
        p.links = &links;
        p.secrets = &w.secrets;
        p.settings = &w.connSettings;
        p.readings = &w.readings;
        p.state = &w.keeper;
        p.guard = &w.guard;
        p.ingest = &w.ingest;
        w.scheduler = new Scheduler(p, w.settings);
        ConnectionConfig c;
        c.id = QStringLiteral("c1");
        c.adapterId = QStringLiteral("libre");
        SubjectConfig s;
        s.id = sid();
        s.subjectRef = QStringLiteral("S-0001");
        c.subjects.append(s);
        w.scheduler->addConnection(c);
        w.scheduler->start();
    }

    AdapterLink *makeLink()
    {
        AdapterLink *link = new AdapterLink(&w.guard);
        link->setRecordSink(w.scheduler->recordSink());
        link->setQuarantineSink(w.scheduler->quarantineSink());
        link->setClock([this]() { return w.clock.now(); });
        link->setChannelFactory([this](const AdapterEntry &) -> LinkChannel * { return new ScriptChannel(&hub); });
        return link;
    }

    // Lets what is waiting run: the fake secret store's answers (FakeWorld::pump) and the
    // driver's own queued callbacks (the Qt event loop).
    void settle()
    {
        w.pump();
        QCoreApplication::processEvents();
        w.pump();
    }

    // `seconds` of virtual time in one-second base steps. The scheduler ticks every `rhythmS`
    // seconds; the source releases one held step per second, before or after the tick.
    void run(int rhythmS, int seconds, bool tickFirst, int untilPolls = -1)
    {
        for (int s = 1; s <= seconds; ++s) {
            w.clock.advanceS(1);
            if (!tickFirst)
                hub.releaseOne();
            settle();
            if (s % rhythmS == 0) {
                w.scheduler->tick();
                settle();
            }
            if (tickFirst)
                hub.releaseOne();
            settle();
            if (untilPolls >= 0 && polls() >= untilPolls)
                break;
        }
        w.scheduler->tick();        // flushes the ingest-log queue into the recording sink
        settle();
    }

    int polls() { return w.readings.count(sid(), QStringLiteral("libre.current")); }
    int events(const QString &name) { return w.sink.count(name); }

    // What the guard decided, without times and scores: violations (kind, severity, action),
    // trips and circuit changes, in order.
    QStringList decisions()
    {
        QStringList out;
        for (int i = 0; i < w.sink.entries.size(); ++i) {
            const IngestLogEntry &e = w.sink.entries.at(i);
            if (e.event == "violation") {
                QString d = e.detail;
                d.remove(QRegExp(" score=[0-9.]+"));
                out << "violation " + d;
            } else if (e.event == "disabled" || e.event == "circuit-open" || e.event == "circuit-closed") {
                out << e.event;
            }
        }
        return out;
    }

    FakeWorld w;
    Hub hub;
    AdapterLinkFactory links;
};

} // namespace

class TstTickRhythm : public QObject
{
    Q_OBJECT

private slots:
    // Invariant: a tick during a start only observes, and the rhythm of the ticks decides
    // nothing. Every rhythm from 1 s to 5 min, the adapter's hello ahead of `started` or after
    // it, the tick before or after the adapter's step: the guard never sees a violation and
    // the connection keeps polling.
    void aTickAtAnyStageIsHarmless_data()
    {
        QTest::addColumn<int>("rhythmS");
        QTest::addColumn<bool>("race");
        QTest::addColumn<bool>("tickFirst");
        const int rhythms[] = { 1, 2, 5, 20, 30, 300 };
        for (unsigned i = 0; i < sizeof(rhythms) / sizeof(rhythms[0]); ++i) {
            for (int race = 0; race < 2; ++race)
                for (int first = 0; first < 2; ++first)
                    QTest::newRow(qPrintable(QStringLiteral("%1 s, %2, tick %3")
                                      .arg(rhythms[i]).arg(race ? "hello first" : "started first")
                                      .arg(first ? "first" : "last")))
                        << rhythms[i] << bool(race) << bool(first);
        }
    }
    void aTickAtAnyStageIsHarmless()
    {
        QFETCH(int, rhythmS);
        QFETCH(bool, race);
        QFETCH(bool, tickFirst);
        Rig r;
        r.hub.race = race;
        r.run(rhythmS, 3 * 3600, tickFirst);
        QCOMPARE(r.events("violation"), 0);
        QCOMPARE(r.events("disabled"), 0);
        QCOMPARE(r.events("circuit-open"), 0);
        QCOMPARE(r.events("session-end"), 0);
        QVERIFY(r.w.scheduler->status(sid()).state != ScheduleState::DisabledByGuard);
        QVERIFY2(r.polls() >= 5, qPrintable(QString::number(r.polls())));
        // At most one adapter process is coming up at a time for the one connection.
        QVERIFY(r.hub.maxNotReady <= 1);
    }

    // Invariant: the number of ticks is not time. A thousand ticks at one instant, in the
    // middle of a start and of a command, start nothing, kill nothing and decide nothing.
    void manyTicksAtOneInstantChangeNothing()
    {
        Rig r;
        // Until the scheduler first starts the adapter (when that is due is its own business).
        for (int s = 0; s < 300 && r.hub.opened == 0; ++s) {
            r.w.clock.advanceS(1);
            r.w.scheduler->tick();
            r.settle();
        }
        QCOMPARE(r.hub.opened, 1);
        for (int stage = 0; stage < 12; ++stage) {
            for (int i = 0; i < 1000; ++i)
                r.w.scheduler->tick();
            r.settle();
            QCOMPARE(r.hub.opened, 1);
            QCOMPARE(r.events("violation"), 0);
            r.w.clock.advanceS(1);
            r.hub.releaseOne();
            r.settle();
        }
        QCOMPARE(r.hub.opened, 1);
        QVERIFY(r.hub.maxNotReady <= 1);
    }

    // Invariant: a tick rhythm from 1 s to 5 min changes NO decision of the guard. The same
    // fault (a soft violation at the third fetch) meets the same reaction at every rhythm.
    void theGuardDecidesTheSameAtEveryRhythm()
    {
        QStringList reference;
        const int rhythms[] = { 1, 2, 5, 30, 300 };
        for (unsigned i = 0; i < sizeof(rhythms) / sizeof(rhythms[0]); ++i) {
            Rig r;
            r.hub.race = true;
            r.hub.faultAtFetch = 3;
            r.run(rhythms[i], 12 * 3600, true, 6);
            QVERIFY2(r.polls() >= 6, qPrintable(QStringLiteral("rhythm %1: %2 polls").arg(rhythms[i]).arg(r.polls())));
            const QStringList d = r.decisions();
            QVERIFY2(!d.isEmpty(), "the scripted fault produced no violation");
            QCOMPARE(d.size(), 1);
            QVERIFY2(d.first().contains("records-sent-mismatch") || d.first().contains("kind="), qPrintable(d.join("|")));
            if (i == 0)
                reference = d;
            else
                QCOMPARE(d, reference);
        }
    }
};

QTEST_MAIN(TstTickRhythm)
#include "tst_tickrhythm.moc"
