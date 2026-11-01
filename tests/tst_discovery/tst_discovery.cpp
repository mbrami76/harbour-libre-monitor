// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Michele Brami

// Tests of Discover (follow-ups for the user interface):
// the one-time probe of a login that is not saved yet and the discovery for a known
// connection. The REAL driver and guard run against a scripted adapter over in-memory pipes
// (support/scriptedadapter.h); the secret store is the in-memory one. Synthetic data only. The
// clock is moved by hand; only the watchdog test lets a little real time pass.
//
// What is checked, in the words of the task: a good login, a wrong one and the other error
// classes; the probe cleanup (the secret is gone after the answer, and a stale one is removed
// at start, also when the secret store was locked at first); the limits; a probe never touches
// the guard's persistent state.

#include "daemon/discovery.h"
#include "daemon/hostports.h"
#include "guard/guard.h"
#include "guard/statelog.h"
#include "scriptedadapter.h"
#include "servicetest.h"
#include "session/adapterregistry.h"

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QtTest>

using namespace lm;
using namespace lm::testing;

namespace {

const char *const kUser = "probe-user-sentinel";
const char *const kPassword = "probe-password-sentinel";
const char *const kRegionHost = "api-eu.example.invalid";

// Everything one test needs around a DiscoveryService.
struct Rig
{
    Rig()
        : keeper(&persistentLog, &clock), script(new AdapterScript)
    {
        keeper.load();
        ConnectionConfig c1;
        c1.id = QStringLiteral("c1");
        c1.adapterId = QStringLiteral("libre");
        known.insert(c1.id, c1);

        script->discover = []() {
            AdapterAnswer a;
            QJsonArray subjects;
            subjects.append(scriptedSubject(QStringLiteral("ref-1"), QStringLiteral("Test Person One"), QStringLiteral("mg/dL")));
            subjects.append(scriptedSubject(QStringLiteral("ref-2"), QStringLiteral("Test Person Two"), QString(), QStringLiteral("none")));
            a.payload.insert(QStringLiteral("subjects"), subjects);
            return a;
        };
        buildService();
    }

    void buildService(const DiscoverySettings &settings = DiscoverySettings(), bool withWriter = true)
    {
        DiscoveryPorts p;
        p.clock = &clock;
        p.secrets = &secrets;
        p.writer = withWriter ? static_cast<CredentialWriter *>(&secrets) : 0;
        p.state = &keeper;
        p.connection = [this](const QString &id) { return known.value(id); };
        p.settings = [](const QString &) { QJsonObject o; o.insert(QStringLiteral("region_host"), QString::fromLatin1(kRegionHost)); return o; };
        const AdapterScriptPtr s = script;
        Clock *c = &clock;
        p.makeLink = [s, c](const QString &, LinkPolicy *policy) {
            AdapterLink *link = new AdapterLink(policy);
            link->setClock([c]() { return c->now(); });
            link->setChannelFactory(scriptedChannels(s));
            return link;
        };
        service.reset(new DiscoveryService(p, settings));
    }

    void storeProbe() { secrets.setCredentials(QStringLiteral("probe"), QString::fromLatin1(kUser), QString::fromLatin1(kPassword)); }

    // Asks and waits for the answer; the parsed object, and the raw text in *raw.
    QJsonObject ask(const QString &connection, QString *raw = 0)
    {
        QStringList answers;
        service->discover(connection, [&answers](const QString &json) { answers << json; });
        // Never before discover() has returned: nothing is in the list yet.
        if (!answers.isEmpty())
            return QJsonObject();
        for (int i = 0; i < 500 && answers.isEmpty(); ++i)
            QTest::qWait(10);
        if (answers.isEmpty())
            return QJsonObject();
        if (raw)
            *raw = answers.first();
        return QJsonDocument::fromJson(answers.first().toUtf8()).object();
    }

    FakeServiceClock clock;
    MemoryStateLog persistentLog;       // the PERSISTENT guard state: a discovery must not write it
    StateKeeper keeper;
    MemorySecretStore secrets;
    AdapterScriptPtr script;
    QMap<QString, ConnectionConfig> known;
    std::unique_ptr<DiscoveryService> service;
};

QString errorOf(const QJsonObject &o) { return o.value("error").toString(); }

// The version the service would run, as the guard's version state is keyed.
QString libreVersion() { return AdapterRegistry::instance().find(QStringLiteral("libre"))->version; }

} // namespace

class TstDiscovery : public QObject
{
    Q_OBJECT

private slots:
    // ---- the probe -------------------------------------------------------------------------
    void aGoodProbeAnswersTheSubjectsAndLeavesNothingBehind();
    void aWrongPasswordIsAuthFailed();
    void everyErrorClassOfTheAdapterIsMapped_data();
    void everyErrorClassOfTheAdapterIsMapped();
    void aMissingProbeSecretAndAnUnknownConnectionAreRefusedBeforeAnyProcess();
    void aLockedSecretStoreIsReportedAndTheSecretIsLeftAlone();
    void aProbeThatCannotBeRemovedStillAnswers();
    void withoutAWriterThePasswordIsNotTakenIn();
    void anUndeterminedUnitIsAnEmptyString();

    // ---- limits and time ----------------------------------------------------------------------
    void oneProbeEvery30SecondsAndSixAnHour();
    void aRefusedProbeStillTakesItsSecretOut();
    void onlyOneProbeRunsAtATime();
    void aHungAdapterEndsAtTheWatchdog();
    void cancelAllAnswersAndKillsTheProcess();

    // ---- the guard -----------------------------------------------------------------------------
    void aMisbehavingAdapterAnswersProtocolChangedWithoutTouchingThePersistentState();
    void aStoppedConnectionOrVersionIsNotStarted();
    void theProbeNeverCountsAsAConnectionForTheGuard();

    // ---- a known connection -----------------------------------------------------------------
    void aKnownConnectionOffersItsStoredSessionAndKeepsItsSecret();
    void aSessionNearItsEndIsNotOffered();
    void theSettingsPortIsAskedForAKnownConnectionOnly();

    // ---- the stale probe secret --------------------------------------------------------------
    void aStaleProbeSecretIsRemovedAtStart();
    void aLockedStoreHidesTheStaleSecretUntilALaterTick();
    void noStaleSecretMeansNothingToDo();

    // ---- what the answer never holds ---------------------------------------------------------
    void noAnswerHoldsACredentialOrASession();
};

// ---- the probe ------------------------------------------------------------------------------------

void TstDiscovery::aGoodProbeAnswersTheSubjectsAndLeavesNothingBehind()
{
    Rig r;
    r.storeProbe();
    QString raw;
    const QJsonObject a = r.ask(QStringLiteral("probe"), &raw);
    QVERIFY2(a.value("ok").toBool(), qPrintable(raw));
    const QJsonArray subjects = a.value("subjects").toArray();
    QCOMPARE(subjects.size(), 2);
    QCOMPARE(subjects.at(0).toObject().value("subject_ref").toString(), QStringLiteral("ref-1"));
    QCOMPARE(subjects.at(0).toObject().value("display_name").toString(), QStringLiteral("Test Person One"));
    QCOMPARE(subjects.at(0).toObject().value("unit").toString(), QStringLiteral("mg/dL"));
    QCOMPARE(subjects.at(0).toObject().value("unit_basis").toString(), QStringLiteral("code"));

    // The login reached the adapter, once, and was offered no session.
    QCOMPARE(r.script->authenticates, 1);
    QCOMPARE(r.script->lastUsername, QString::fromLatin1(kUser));
    QCOMPARE(r.script->lastPassword, QString::fromLatin1(kPassword));
    QVERIFY(r.script->lastSession.isEmpty());
    QCOMPARE(r.script->discovers, 1);

    // Nothing is kept: the secret is gone, no session was saved, the persistent state is empty.
    QVERIFY(!r.secrets.hasSecret(QStringLiteral("probe")));
    QVERIFY(r.secrets.sessionOf(QStringLiteral("probe")).isEmpty());
    QCOMPARE(r.persistentLog.size(), 0);
    QVERIFY(!r.keeper.connectionDisabled(QStringLiteral("probe")));
    // The adapter process ended with the discovery.
    QTRY_COMPARE_WITH_TIMEOUT(r.script->ended, 1, 3000);
    QTRY_COMPARE_WITH_TIMEOUT(r.service->running(), 0, 3000);
}

void TstDiscovery::aWrongPasswordIsAuthFailed()
{
    Rig r;
    r.storeProbe();
    r.script->authenticate = [](const QString &, const QString &, const QString &) {
        return AdapterAnswer::error(QStringLiteral("auth-failed"));
    };
    const QJsonObject a = r.ask(QStringLiteral("probe"));
    QVERIFY(!a.value("ok").toBool());
    QCOMPARE(errorOf(a), QStringLiteral("auth-failed"));
    QCOMPARE(r.script->discovers, 0);                    // no discovery after a refused login
    QVERIFY(!r.secrets.hasSecret(QStringLiteral("probe")));
    QCOMPARE(r.persistentLog.size(), 0);
}

void TstDiscovery::everyErrorClassOfTheAdapterIsMapped_data()
{
    QTest::addColumn<QString>("adapterClass");
    QTest::addColumn<QString>("answerClass");
    QTest::newRow("auth") << "auth-failed" << "auth-failed";
    QTest::newRow("terms") << "terms-must-be-accepted" << "terms-must-be-accepted";
    QTest::newRow("rate") << "rate-limited" << "rate-limited";
    QTest::newRow("transient") << "transient" << "unreachable";
    QTest::newRow("protocol") << "protocol-changed" << "protocol-changed";
    QTest::newRow("invalid") << "invalid-command" << "internal";
}

void TstDiscovery::everyErrorClassOfTheAdapterIsMapped()
{
    QFETCH(QString, adapterClass);
    QFETCH(QString, answerClass);
    for (int where = 0; where < 2; ++where) {           // at the login, and at the discovery itself
        Rig r;
        r.storeProbe();
        if (where == 0)
            r.script->authenticate = [adapterClass](const QString &, const QString &, const QString &) {
                return AdapterAnswer::error(adapterClass, 120);
            };
        else
            r.script->discover = [adapterClass]() { return AdapterAnswer::error(adapterClass, 120); };
        const QJsonObject a = r.ask(QStringLiteral("probe"));
        QVERIFY(!a.value("ok").toBool());
        QCOMPARE(errorOf(a), answerClass);
        QVERIFY(!r.secrets.hasSecret(QStringLiteral("probe")));
    }
}

void TstDiscovery::aMissingProbeSecretAndAnUnknownConnectionAreRefusedBeforeAnyProcess()
{
    Rig r;
    QCOMPARE(errorOf(r.ask(QStringLiteral("probe"))), QStringLiteral("credentials-missing"));
    QCOMPARE(errorOf(r.ask(QStringLiteral("nobody"))), QStringLiteral("unknown-connection"));
    QCOMPARE(errorOf(r.ask(QString())), QStringLiteral("unknown-connection"));
    QCOMPARE(r.script->spawned, 0);
}

void TstDiscovery::aLockedSecretStoreIsReportedAndTheSecretIsLeftAlone()
{
    Rig r;
    r.storeProbe();
    r.secrets.lock();
    QCOMPARE(errorOf(r.ask(QStringLiteral("probe"))), QStringLiteral("secrets-locked"));
    QCOMPARE(r.script->spawned, 0);
    QVERIFY(r.secrets.hasSecret(QStringLiteral("probe")));      // cannot be touched now; cleaned at a later start
}

void TstDiscovery::aProbeThatCannotBeRemovedStillAnswers()
{
    Rig r;
    r.storeProbe();
    r.secrets.failRemoves(true);
    const QJsonObject a = r.ask(QStringLiteral("probe"));
    QVERIFY(a.value("ok").toBool());                            // the discovery itself is not at fault
    QVERIFY(r.secrets.hasSecret(QStringLiteral("probe")));      // left for the cleanup at the next start
}

void TstDiscovery::withoutAWriterThePasswordIsNotTakenIn()
{
    Rig r;
    r.buildService(DiscoverySettings(), false);
    r.storeProbe();
    QCOMPARE(errorOf(r.ask(QStringLiteral("probe"))), QStringLiteral("internal"));
    QCOMPARE(r.script->spawned, 0);
    QCOMPARE(r.script->authenticates, 0);
}

void TstDiscovery::anUndeterminedUnitIsAnEmptyString()
{
    Rig r;
    r.storeProbe();
    const QJsonObject a = r.ask(QStringLiteral("probe"));
    const QJsonObject second = a.value("subjects").toArray().at(1).toObject();
    QVERIFY(second.contains("unit"));
    QCOMPARE(second.value("unit").toString(), QString());
    QCOMPARE(second.value("unit_basis").toString(), QStringLiteral("none"));
}

// ---- limits and time ------------------------------------------------------------------------------------

void TstDiscovery::oneProbeEvery30SecondsAndSixAnHour()
{
    Rig r;
    r.storeProbe();
    QVERIFY(r.ask(QStringLiteral("probe")).value("ok").toBool());               // 1
    const int spawnedAfterFirst = r.script->spawned;

    // Too soon: refused, and nothing reaches the source.
    r.clock.advanceS(29);
    r.storeProbe();
    QCOMPARE(errorOf(r.ask(QStringLiteral("probe"))), QStringLiteral("rate-limited"));
    QCOMPARE(r.script->spawned, spawnedAfterFirst);
    QCOMPARE(r.script->authenticates, 1);

    // The refusal did not use up the allowance: 30 s after the first one it is accepted.
    r.clock.advanceS(1);
    r.storeProbe();
    QVERIFY(r.ask(QStringLiteral("probe")).value("ok").toBool());               // 2
    for (int n = 3; n <= 6; ++n) {
        r.clock.advanceS(31);
        r.storeProbe();
        QVERIFY2(r.ask(QStringLiteral("probe")).value("ok").toBool(), qPrintable(QString::number(n)));
    }
    QCOMPARE(r.script->authenticates, 6);

    // The seventh within the hour is refused however long the pause since the sixth.
    r.clock.advanceS(600);
    r.storeProbe();
    QCOMPARE(errorOf(r.ask(QStringLiteral("probe"))), QStringLiteral("rate-limited"));
    QCOMPARE(r.script->authenticates, 6);

    // An hour after the first, the window has moved on.
    r.clock.advanceS(3600);
    r.storeProbe();
    QVERIFY(r.ask(QStringLiteral("probe")).value("ok").toBool());
}

void TstDiscovery::aRefusedProbeStillTakesItsSecretOut()
{
    Rig r;
    r.storeProbe();
    QVERIFY(r.ask(QStringLiteral("probe")).value("ok").toBool());
    r.storeProbe();                                                 // the UI stores it again at once
    QVERIFY(r.secrets.hasSecret(QStringLiteral("probe")));
    QCOMPARE(errorOf(r.ask(QStringLiteral("probe"))), QStringLiteral("rate-limited"));
    QVERIFY(!r.secrets.hasSecret(QStringLiteral("probe")));         // gone whatever the outcome
}

void TstDiscovery::onlyOneProbeRunsAtATime()
{
    Rig r;
    r.storeProbe();
    r.script->discover = []() { return AdapterAnswer::never(); };    // the first one hangs
    QStringList first, second;
    r.service->discover(QStringLiteral("probe"), [&first](const QString &j) { first << j; });
    QTRY_COMPARE_WITH_TIMEOUT(r.script->discovers, 1, 3000);
    QCOMPARE(r.service->running(), 1);

    r.clock.advanceS(60);                                           // the spacing is not the reason
    r.storeProbe();
    r.service->discover(QStringLiteral("probe"), [&second](const QString &j) { second << j; });
    QTRY_COMPARE_WITH_TIMEOUT(second.size(), 1, 3000);
    QCOMPARE(QJsonDocument::fromJson(second.first().toUtf8()).object().value("error").toString(),
             QStringLiteral("rate-limited"));
    QVERIFY(first.isEmpty());
    r.service->cancelAll();
    QTRY_COMPARE_WITH_TIMEOUT(first.size(), 1, 3000);
}

void TstDiscovery::aHungAdapterEndsAtTheWatchdog()
{
    Rig r;
    DiscoverySettings s;
    s.timeoutMs = 300;                                              // the real value is a minute
    r.buildService(s);
    r.storeProbe();
    r.script->authenticate = [](const QString &, const QString &, const QString &) { return AdapterAnswer::never(); };
    const QJsonObject a = r.ask(QStringLiteral("probe"));
    QCOMPARE(errorOf(a), QStringLiteral("unreachable"));
    QVERIFY(!r.secrets.hasSecret(QStringLiteral("probe")));
    QTRY_COMPARE_WITH_TIMEOUT(r.script->ended, 1, 5000);            // the stuck process is ended
}

void TstDiscovery::cancelAllAnswersAndKillsTheProcess()
{
    Rig r;
    r.storeProbe();
    r.script->discover = []() { return AdapterAnswer::never(); };
    QStringList answers;
    r.service->discover(QStringLiteral("probe"), [&answers](const QString &j) { answers << j; });
    QTRY_COMPARE_WITH_TIMEOUT(r.script->discovers, 1, 3000);
    r.service->cancelAll();
    QCOMPARE(answers.size(), 1);
    QCOMPARE(QJsonDocument::fromJson(answers.first().toUtf8()).object().value("error").toString(),
             QStringLiteral("service-stopping"));
    QCOMPARE(r.script->ended, 1);                                   // killed at once, not after a wait
    QCOMPARE(r.service->running(), 0);
}

// ---- the guard ----------------------------------------------------------------------------------------------

void TstDiscovery::aMisbehavingAdapterAnswersProtocolChangedWithoutTouchingThePersistentState()
{
    Rig r;
    r.storeProbe();
    r.script->discover = []() {
        AdapterAnswer a;
        a.rawLine = QString(1100000, QLatin1Char('x'));             // over the line limit: the driver ends it
        return a;
    };
    const QJsonObject a = r.ask(QStringLiteral("probe"));
    QCOMPARE(errorOf(a), QStringLiteral("protocol-changed"));
    QCOMPARE(r.persistentLog.size(), 0);                            // no disabled state, no attention, no hold
    QVERIFY(!r.keeper.connectionDisabled(QStringLiteral("probe")));
    QVERIFY(!r.keeper.versionDisabled(QStringLiteral("libre"), libreVersion()));
    QVERIFY(!r.secrets.hasSecret(QStringLiteral("probe")));
}

void TstDiscovery::aStoppedConnectionOrVersionIsNotStarted()
{
    {
        Rig r;
        r.keeper.disableConnection(QStringLiteral("c1"), QStringLiteral("libre"), QStringLiteral("0.1.0"),
                                   StateCause::score(), QStringLiteral("test"));
        r.secrets.setCredentials(QStringLiteral("c1"), QStringLiteral("u"), QStringLiteral("p"));
        QCOMPARE(errorOf(r.ask(QStringLiteral("c1"))), QStringLiteral("stopped-by-guard"));
        QCOMPARE(r.script->spawned, 0);
    }
    {
        Rig r;
        r.keeper.disableVersion(QStringLiteral("libre"), libreVersion(), QStringLiteral("test"));
        r.storeProbe();
        QCOMPARE(errorOf(r.ask(QStringLiteral("probe"))), QStringLiteral("stopped-by-guard"));
        QCOMPARE(r.script->spawned, 0);
        QVERIFY(!r.secrets.hasSecret(QStringLiteral("probe")));     // taken out although refused
    }
}

void TstDiscovery::theProbeNeverCountsAsAConnectionForTheGuard()
{
    // A probe whose adapter keeps failing leaves no trace in the persistent state, however
    // often it is tried (the limits let six through an hour).
    Rig r;
    r.script->authenticate = [](const QString &, const QString &, const QString &) {
        return AdapterAnswer::error(QStringLiteral("protocol-changed"));
    };
    for (int i = 0; i < 6; ++i) {
        r.storeProbe();
        QCOMPARE(errorOf(r.ask(QStringLiteral("probe"))), QStringLiteral("protocol-changed"));
        r.clock.advanceS(31);
    }
    QCOMPARE(r.persistentLog.size(), 0);
    QVERIFY(r.keeper.disabledConnections().isEmpty());
    QVERIFY(!r.keeper.attention(QStringLiteral("probe")).active());
}

// ---- a known connection ---------------------------------------------------------------------------------------

void TstDiscovery::aKnownConnectionOffersItsStoredSessionAndKeepsItsSecret()
{
    Rig r;
    r.secrets.setCredentials(QStringLiteral("c1"), QStringLiteral("known-user"), QStringLiteral("known-password"));
    r.secrets.saveSession(QStringLiteral("c1"), QStringLiteral("stored-session-blob"),
                          UtcTime::fromMSecsSinceEpoch(r.clock.now().msecsSinceEpoch() + 30LL * 24 * 3600 * 1000));
    r.script->authenticate = [](const QString &, const QString &, const QString &) {
        AdapterAnswer a;
        a.payload.insert(QStringLiteral("session"), QStringLiteral("a-new-session-the-scheduler-must-not-lose"));
        return a;
    };
    const QJsonObject a = r.ask(QStringLiteral("c1"));
    QVERIFY(a.value("ok").toBool());
    QCOMPARE(r.script->lastSession, QStringLiteral("stored-session-blob"));    // reuse the live login
    QCOMPARE(r.script->lastUsername, QStringLiteral("known-user"));
    // The scheduler's secret is as it was: not removed, session not replaced.
    QVERIFY(r.secrets.hasSecret(QStringLiteral("c1")));
    QCOMPARE(r.secrets.sessionOf(QStringLiteral("c1")), QStringLiteral("stored-session-blob"));
    QCOMPARE(r.persistentLog.size(), 0);
}

void TstDiscovery::aSessionNearItsEndIsNotOffered()
{
    Rig r;
    r.secrets.setCredentials(QStringLiteral("c1"), QStringLiteral("u"), QStringLiteral("p"));
    r.secrets.saveSession(QStringLiteral("c1"), QStringLiteral("old-blob"),
                          UtcTime::fromMSecsSinceEpoch(r.clock.now().msecsSinceEpoch() + 30 * 1000));
    QVERIFY(r.ask(QStringLiteral("c1")).value("ok").toBool());
    QVERIFY(r.script->lastSession.isEmpty());                                   // a fresh login, as the scheduler would make
}

void TstDiscovery::theSettingsPortIsAskedForAKnownConnectionOnly()
{
    // The non-secret settings (the regional host) are asked for a known connection; the probe has none.
    Rig r;
    QStringList asked;
    DiscoveryPorts p;
    p.clock = &r.clock;
    p.secrets = &r.secrets;
    p.writer = &r.secrets;
    p.state = &r.keeper;
    p.connection = [&r](const QString &id) { return r.known.value(id); };
    p.settings = [&asked](const QString &id) { asked << id; return QJsonObject(); };
    const AdapterScriptPtr s = r.script;
    Clock *c = &r.clock;
    p.makeLink = [s, c](const QString &, LinkPolicy *policy) {
        AdapterLink *link = new AdapterLink(policy);
        link->setClock([c]() { return c->now(); });
        link->setChannelFactory(scriptedChannels(s));
        return link;
    };
    r.service.reset(new DiscoveryService(p));
    r.secrets.setCredentials(QStringLiteral("c1"), QStringLiteral("u"), QStringLiteral("p"));
    QVERIFY(r.ask(QStringLiteral("c1")).value("ok").toBool());
    r.storeProbe();
    r.clock.advanceS(40);
    QVERIFY(r.ask(QStringLiteral("probe")).value("ok").toBool());
    QCOMPARE(asked, QStringList() << QStringLiteral("c1"));
}

// ---- the stale probe secret ------------------------------------------------------------------------------------

void TstDiscovery::aStaleProbeSecretIsRemovedAtStart()
{
    Rig r;
    r.storeProbe();                                                 // left over from a UI that died
    r.service->cleanStaleProbe();
    QVERIFY(!r.secrets.hasSecret(QStringLiteral("probe")));
    QVERIFY(!r.service->cleanupPending());
    QCOMPARE(r.script->spawned, 0);                                 // and nothing was sent to the source
}

void TstDiscovery::aLockedStoreHidesTheStaleSecretUntilALaterTick()
{
    Rig r;
    r.storeProbe();
    r.secrets.lock();                                               // right after a phone restart
    r.service->cleanStaleProbe();
    QVERIFY(r.service->cleanupPending());
    QVERIFY(r.secrets.hasSecret(QStringLiteral("probe")));

    r.secrets.unlock();
    r.clock.advanceS(60);
    r.service->tick();                                              // too early: looked at every 5 minutes
    QVERIFY(r.secrets.hasSecret(QStringLiteral("probe")));
    r.clock.advanceS(250);
    r.service->tick();
    QVERIFY(!r.secrets.hasSecret(QStringLiteral("probe")));
    QVERIFY(!r.service->cleanupPending());
}

void TstDiscovery::noStaleSecretMeansNothingToDo()
{
    Rig r;
    r.service->cleanStaleProbe();
    QVERIFY(!r.service->cleanupPending());
    r.secrets.setCredentials(QStringLiteral("c1"), QStringLiteral("u"), QStringLiteral("p"));
    r.service->cleanStaleProbe();
    QVERIFY(r.secrets.hasSecret(QStringLiteral("c1")));             // only the probe is ever touched
}

// ---- what the answer never holds ---------------------------------------------------------------------------------

void TstDiscovery::noAnswerHoldsACredentialOrASession()
{
    Rig r;
    r.storeProbe();
    r.script->authenticate = [](const QString &, const QString &, const QString &) {
        AdapterAnswer a;
        a.payload.insert(QStringLiteral("session"), QStringLiteral("session-sentinel-value"));
        return a;
    };
    QString okRaw, failRaw;
    r.ask(QStringLiteral("probe"), &okRaw);
    r.clock.advanceS(31);
    r.storeProbe();
    r.script->authenticate = [](const QString &, const QString &, const QString &) {
        return AdapterAnswer::error(QStringLiteral("auth-failed"));
    };
    r.ask(QStringLiteral("probe"), &failRaw);
    for (int i = 0; i < 2; ++i) {
        const QString text = i == 0 ? okRaw : failRaw;
        QVERIFY(!text.isEmpty());
        QVERIFY(!text.contains(QString::fromLatin1(kUser)));
        QVERIFY(!text.contains(QString::fromLatin1(kPassword)));
        QVERIFY(!text.contains(QStringLiteral("session-sentinel-value")));
        QVERIFY(!text.contains(QStringLiteral("opaque-scripted-session")));
    }
}

QTEST_GUILESS_MAIN(TstDiscovery)
#include "tst_discovery.moc"
