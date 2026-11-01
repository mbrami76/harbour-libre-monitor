// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Michele Brami

// Tests of the process side of the service: the mode parser, adapter mode
// selection, the identity names, the configuration reader, the D-Bus interface and the runtime
// wired to fakes. The D-Bus tests need a session bus; without one they are skipped (run the
// program under dbus-run-session, as tests/integration/run_service_integration.sh does).

#include "daemon/adaptermode.h"
#include "daemon/commonports.h"
#include "daemon/dbusservice.h"
#include "daemon/hostports.h"
#include "alerts/alertjournal.h"
#include "daemon/maintenancejobs.h"
#include "record/registry.h"
#include "store/journal.h"
#include "store/store.h"
#include "daemon/processmode.h"
#include "daemon/serviceconfig.h"
#include "daemon/serviceidentity.h"
#include "daemon/serviceruntime.h"
#include "session/adapterregistry.h"
#include "daemon/uiguard.h"
#include "scriptedadapter.h"
#include "servicetest.h"
#include "silenttest.h"
#include "notifiertest.h"

#include <QDateTime>
#include <QDBusAbstractAdaptor>
#include <QDBusConnection>
#include <QDBusConnectionInterface>
#include <QDBusInterface>
#include <QDBusMessage>
#include <QDBusPendingCall>
#include <QJsonDocument>
#include <QJsonObject>
#include <QAtomicInt>
#include <QSharedPointer>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QThread>
#include <QtTest>

using namespace lm;
using namespace lm::testing;

namespace {

QStringList argv(const char *a = 0, const char *b = 0, const char *c = 0, const char *d = 0)
{
    QStringList l;
    l << QStringLiteral("harbour-libre-monitor");
    if (a) l << QString::fromLatin1(a);
    if (b) l << QString::fromLatin1(b);
    if (c) l << QString::fromLatin1(c);
    if (d) l << QString::fromLatin1(d);
    return l;
}

const char *const kGoodConfig =
    "{\"format\":1,\"connections\":[{\"id\":\"c1\",\"adapter\":\"libre\","
    "\"subjects\":[{\"id\":\"{d2f1c9a0-5b3e-4c7d-8a91-0e2b4c6d8f10}\",\"ref\":\"opaque-1\","
    "\"name\":\"A Real Name\"}],\"current_interval_s\":60}]}";

} // namespace

class TstDaemon : public QObject
{
    Q_OBJECT

private slots:
    // ---- modes ------------------------------------------------------------------------------
    void noArgumentIsTheUserInterface();
    void serviceMode();
    void serviceModeTakesNoFurtherArgument();
    void adapterModeNeedsAKnownIdentifier();
    void adapterModeCarriesItsOptions();
    void aStrayArgumentDoesNotChangeTheMode();
    void invalidTextNeverEchoesTheArgument();
    void adapterModeRefusesAnUnknownOptionWithoutReadingAnything();
    void provisionModeCarriesCheckedOptions();
    void provisionModeRefusesBadOptionsWithoutEchoingThem();

    // ---- identity and configuration ------------------------------------------------------
    void identityFollowsD19();
    void identityHasNoHyphenInPathOrInterface();
    void configReadsConnectionsAndIgnoresNames();
    void configRejectsBadShapes();
    void configRejectsUnknownAdapter();
    void configRejectsDuplicateSubjects();

    // ---- ports ------------------------------------------------------------------------------
    void clockCountersAreSane();
    void settingsFileRoundTripsAndKeepsKindsApart();
    void memorySecretStoreAnswersLockedThenOk();

    // ---- the runtime ----------------------------------------------------------------------
    void statusJsonHasNoNameAndNoSecret();
    void runtimeWithoutBusReportsIt();
    void dbusInterfaceRoundTrip();
    void secondInstanceStepsAsideWritingNothing();
    void stopMethodWritesAStopRecordWithReasonRequested();
    void startMaintenanceNeedsARunningService();
    void aLockedSecretStoreLeadsToAwaitingApprovalAndBack();
    void configChangedAppliesIntervalsAndAsksForARestartOtherwise();

    // ---- the follow-ups for the user interface --------------------------
    void configRejectsTheReservedProbeIdentifier();
    void configChangedRefusesTheReservedProbeIdentifier();
    void statusCarriesAdapterSensorAndTrendAndDropsTheOldTrend();
    void stateChangedNamesTheConnection();
    void aReportProposalShowsInStatusAndIsClearedByReenable();
    void reportHandledEndsTheProposalUntilTwoNewKills();
    void diagnosticReportAnswersAnOpenProposalOverTheBusWithoutAnyValue();

    // ---- the alerts in the service ---------------------------------
    void configReadsOnlyTheShortLabelOfASubject();
    void theVersionDefineIsTheSpecsVersionAndRelease();
    void configReadsThePerSubjectAlertKeysAsWholeSecondsAndAnythingElseIsAbsent();
    void thePerSubjectAlertKeysApplyAtConfigChangedAndAlertsReportThemWithTheirRanges();
    void anUrgentLowIsListedAcknowledgedAndAnnouncedOverTheBus();
    void snoozeAndItsRefusalsAnswerInWords();
    void setThresholdsAnswersTheWordsOfTheBookWithTheDetailsOfTheRefusal();
    void alertsAndThresholdsSurviveARestartInEveryState();
    void aStoppedServiceAnswersTheAlertMethodsInWords();
    void alertsHoldWhatAPageNeedsAndAcknowledgingFromTheAppClosesTheEntry();
    void theAlarmIsPostedBeforeTheResidentNoticeIsRefreshed();
    void deletingTheNotificationAcknowledgesTheAlertInTheEngine();
    void notificationsAfterARestartAdoptTheEntriesInEveryState();
    void theApprovalNoticeAndTheResidentNoticeFollowTheLifecycle();
#ifdef LIBREMON_DEVSOURCE
    void theDevelopmentFeedGoesThroughTheWholeServiceAndTheResidentNoticeListsIt();
#endif
    void silentModeIsSetEndedAndAnnouncedOverTheBus();
    void silentModeWithTheRealEngineStopsTheToneAndIsListed();
    void discoverOverTheBusAnswersLaterWithTheSubjects();
    void discoverOverTheBusWithAWrongLoginAndWithNoSecret();
    void aStaleProbeSecretIsRemovedWhenTheServiceStarts();
    void discoverNeedsARunningServiceAndAStopCutsItShort();
    void asyncClientReturnsAtOnceAndAnswersBySignal();
    void asyncClientReportsAMissingMethodAndAMissingService();

    // ---- the UI guard ----------------------------------------------------
    void theFirstUiTakesTheLockASecondOneRaisesAndLeaves();
    void aStaleLockFromAKilledUiIsRecovered();

    // ---- the version stamp ------------------------------
    void theSameStampKeepsRunningChecksOncePerHeartbeatAndStatusCarriesTheVersion();
    void aNewerStampStopsWithReasonUpgradedAndActivatesTheNameAfterReleasingIt();
    void aMissingStampStopsWithReasonPackageRemovedAndStartsNothing();
    void anUnreadableStampKeepsMonitoringAndIsJournalledOnce();
    void noVersionCheckActsDuringAStopAlreadyInProgress();
    void theRestartedServiceThatStillSeesAnotherVersionDoesNotLoop();
    void theStampFileReaderTellsMissingFromUnreadable();

    // ---- compaction through the service -------------------
    void aCompactionThroughTheServiceThinsOldSamplesAndReportsTheEstimate();
    void aCompactionIsRefusedWhenTheRebuildWouldNotFit();
    void aCompactionIsRefusedWhenTheFreeSpaceIsUnknown();
    void theEstimateDetailsHaveTheirKeys();

    // ---- purge through the service -------------
    void theConfigurationGivesThePurgeItsSubjectFacts();
    void aPurgeIsRefusedBeforeItStartsWithoutTheExactLabel();
    void aPurgeEstimateChangesNothingAndThePurgeRemovesExactlyIt();
    void aPurgeWhoseJournalPartFailedCanBeRepeated();
    void aPurgeIsRefusedWhenTheSpaceIsShortOrUnknown();
    void aPurgeStopsAtTheQueueHighWaterMarkWhileTheServiceAnswers();
    void thePurgeDetailsHaveTheirKeys();
};

// ---- modes -------------------------------------------------------------------------------------

void TstDaemon::noArgumentIsTheUserInterface()
{
    QCOMPARE(parseProcessMode(argv()).kind, ProcessMode::Ui);
    QCOMPARE(parseProcessMode(QStringList()).kind, ProcessMode::Ui);
    // Qt and Silica options are not ours.
    QCOMPARE(parseProcessMode(argv("-platform", "wayland")).kind, ProcessMode::Ui);
}

void TstDaemon::serviceMode()
{
    QCOMPARE(parseProcessMode(argv("--service")).kind, ProcessMode::Service);
}

void TstDaemon::serviceModeTakesNoFurtherArgument()
{
    QCOMPARE(parseProcessMode(argv("--service", "--x")).kind, ProcessMode::Invalid);
}

void TstDaemon::adapterModeNeedsAKnownIdentifier()
{
    QCOMPARE(parseProcessMode(argv("--adapter")).kind, ProcessMode::Invalid);
    QCOMPARE(parseProcessMode(argv("--adapter", "nonexistent")).kind, ProcessMode::Invalid);
    const ProcessMode m = parseProcessMode(argv("--adapter", "libre"));
    QCOMPARE(m.kind, ProcessMode::Adapter);
    QCOMPARE(m.adapterId, QStringLiteral("libre"));
}

void TstDaemon::adapterModeCarriesItsOptions()
{
    const ProcessMode m = parseProcessMode(argv("--adapter", "libre", "--base-url", "http://x"));
    QCOMPARE(m.kind, ProcessMode::Adapter);
    QCOMPARE(m.adapterOptions, QStringList() << "--base-url" << "http://x");
}

void TstDaemon::aStrayArgumentDoesNotChangeTheMode()
{
    // The mode word counts only as the first argument.
    QCOMPARE(parseProcessMode(argv("--foo", "--service")).kind, ProcessMode::Ui);
    QCOMPARE(parseProcessMode(argv("-x", "--adapter", "libre")).kind, ProcessMode::Ui);
}

void TstDaemon::provisionModeCarriesCheckedOptions()
{
    const ProcessMode m = parseProcessMode(argv("--provision", "--connection", "c1", "--subject-ref"));
    QCOMPARE(m.kind, ProcessMode::Invalid);         // a value is missing
    const QStringList args = QStringList() << "harbour-libre-monitor" << "--provision"
                                           << "--connection" << "c1" << "--subject-ref" << "ref-1";
    const ProcessMode ok = parseProcessMode(args);
    QCOMPARE(ok.kind, ProcessMode::Provision);
    QCOMPARE(ok.provisionOptions, QStringList(args.mid(2)));
    QCOMPARE(parseProcessMode(argv("--provision", "--remove", "--connection", "c1")).kind,
             ProcessMode::Provision);
    // Only as the first argument, like every mode word.
    QCOMPARE(parseProcessMode(argv("--foo", "--provision")).kind, ProcessMode::Ui);
}

void TstDaemon::provisionModeRefusesBadOptionsWithoutEchoingThem()
{
    const QStringList bad = QStringList()
        << "--connection" << "has space" << "--subject-ref" << "ref-sentinel";
    ProcessMode m = parseProcessMode(QStringList() << "x" << "--provision" << bad);
    QCOMPARE(m.kind, ProcessMode::Invalid);
    QVERIFY(!m.error.contains(QStringLiteral("has space")));
    QVERIFY(!m.error.contains(QStringLiteral("ref-sentinel")));
    // An unknown option, a missing connection, and --remove with a reference.
    m = parseProcessMode(QStringList() << "x" << "--provision" << "--password" << "pw-sentinel");
    QCOMPARE(m.kind, ProcessMode::Invalid);
    QVERIFY(!m.error.contains(QStringLiteral("pw-sentinel")));
    QCOMPARE(parseProcessMode(argv("--provision")).kind, ProcessMode::Invalid);
    QCOMPARE(parseProcessMode(QStringList() << "x" << "--provision" << "--remove" << "--connection"
                                            << "c1" << "--subject-ref" << "r").kind,
             ProcessMode::Invalid);
}

void TstDaemon::invalidTextNeverEchoesTheArgument()
{
    const ProcessMode m = parseProcessMode(argv("--adapter", "secret-looking-value"));
    QCOMPARE(m.kind, ProcessMode::Invalid);
    QVERIFY(!m.error.contains(QStringLiteral("secret-looking-value")));
}

void TstDaemon::adapterModeRefusesAnUnknownOptionWithoutReadingAnything()
{
    // An option the adapter does not know ends the loop before any stream is opened: exit 64.
    // (In a build with devsource only --base-url is accepted; anything else is refused.)
    QCOMPARE(runAdapterMode(QStringLiteral("libre"), QStringList() << QStringLiteral("--password")), 64);
    QCOMPARE(runAdapterMode(QStringLiteral("nope"), QStringList()), 64);
}

// ---- identity and configuration -------------------------------------------------------------

void TstDaemon::identityFollowsD19()
{
    const ServiceIdentity id = ServiceIdentity::fromNames(QStringLiteral("it.michelebrami"), QStringLiteral("libre-monitor"));
    QCOMPARE(id.busName, QStringLiteral("it.michelebrami.libre-monitor"));
    QCOMPARE(id.objectPath, QStringLiteral("/it/michelebrami/libre_monitor"));
}

void TstDaemon::identityHasNoHyphenInPathOrInterface()
{
    const ServiceIdentity id = ServiceIdentity::fromNames(QStringLiteral("it.michelebrami"), QStringLiteral("libre-monitor-dev"));
    QVERIFY(!id.objectPath.contains(QLatin1Char('-')));
    QVERIFY(!id.interfaceName.contains(QLatin1Char('-')));
    QCOMPARE(id.interfaceName, QString::fromLatin1(kServiceInterface));
}

void TstDaemon::configReadsConnectionsAndIgnoresNames()
{
    const ServiceConfig c = parseServiceConfig(kGoodConfig);
    QVERIFY(c.ok);
    QCOMPARE(c.connections.size(), 1);
    QCOMPARE(c.connections.first().id, QStringLiteral("c1"));
    QCOMPARE(c.connections.first().currentIntervalMs, qint64(60000));
    QCOMPARE(c.connections.first().graphIntervalMs, qint64(-1));    // the scheduler's default
    QCOMPARE(c.connections.first().subjects.size(), 1);
    QCOMPARE(c.connections.first().subjects.first().subjectRef, QStringLiteral("opaque-1"));
    // The reader has nowhere to put a name: the structure it returns holds ids and refs only.
}

void TstDaemon::configRejectsBadShapes()
{
    QVERIFY(!parseServiceConfig("not json").ok);
    QVERIFY(!parseServiceConfig("{\"format\":2,\"connections\":[]}").ok);
    QVERIFY(!parseServiceConfig("{\"format\":1}").ok);
    QVERIFY(!parseServiceConfig("{\"format\":1,\"connections\":[{\"id\":\"\",\"adapter\":\"libre\",\"subjects\":[]}]}").ok);
    QVERIFY(!parseServiceConfig("{\"format\":1,\"connections\":[{\"id\":\"c\",\"adapter\":\"libre\",\"subjects\":[],\"current_interval_s\":0}]}").ok);
    QVERIFY(parseServiceConfig("{\"format\":1,\"connections\":[]}").ok);
}

void TstDaemon::configRejectsUnknownAdapter()
{
    const ServiceConfig c = parseServiceConfig("{\"format\":1,\"connections\":[{\"id\":\"c\",\"adapter\":\"other\",\"subjects\":[]}]}");
    QVERIFY(!c.ok);
    QCOMPARE(c.error, QStringLiteral("config-adapter"));
}

void TstDaemon::configRejectsDuplicateSubjects()
{
    const QByteArray two =
        "{\"format\":1,\"connections\":[{\"id\":\"a\",\"adapter\":\"libre\",\"subjects\":[{\"id\":\"{d2f1c9a0-5b3e-4c7d-8a91-0e2b4c6d8f10}\",\"ref\":\"x\"}]},"
        "{\"id\":\"b\",\"adapter\":\"libre\",\"subjects\":[{\"id\":\"{d2f1c9a0-5b3e-4c7d-8a91-0e2b4c6d8f10}\",\"ref\":\"y\"}]}]}";
    QVERIFY(!parseServiceConfig(two).ok);
}

// ---- ports ---------------------------------------------------------------------------------------

void TstDaemon::clockCountersAreSane()
{
    SystemServiceClock c;
    const qint64 b1 = c.bootTimeMs();
    const qint64 m1 = c.monotonicMs();
    QVERIFY(b1 > 0 && m1 > 0);
    QVERIFY(b1 >= m1);          // BOOTTIME includes suspend, MONOTONIC does not
    QTest::qSleep(20);
    QVERIFY(c.bootTimeMs() >= b1 + 15);
    QVERIFY(c.now().isValid());
}

void TstDaemon::settingsFileRoundTripsAndKeepsKindsApart()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString path = dir.path() + "/settings.json";
    {
        JsonFileSettings s(path);
        s.setValue(QStringLiteral("notice.id"), QStringLiteral("42"));
        QJsonObject o;
        o.insert(QStringLiteral("region_host"), QStringLiteral("eu.example"));
        s.merge(QStringLiteral("c1"), o);
    }
    JsonFileSettings again(path);
    QCOMPARE(again.value(QStringLiteral("notice.id")), QStringLiteral("42"));
    QCOMPARE(again.get(QStringLiteral("c1")).value(QStringLiteral("region_host")).toString(), QStringLiteral("eu.example"));
    QVERIFY(again.get(QStringLiteral("c2")).isEmpty());
    QVERIFY(again.value(QStringLiteral("c1")).isEmpty());
}

void TstDaemon::memorySecretStoreAnswersLockedThenOk()
{
    MemorySecretStore s;
    s.setCredentials(QStringLiteral("c1"), QStringLiteral("u"), QStringLiteral("p"));
    SecretLoad::Kind kind = SecretLoad::Error;
    s.lock();
    s.load(QStringLiteral("c1"), [&](const SecretLoad &r) { kind = r.kind; });
    QCOMPARE(kind, SecretLoad::Locked);
    s.unlock();
    s.load(QStringLiteral("c1"), [&](const SecretLoad &r) { kind = r.kind; });
    QCOMPARE(kind, SecretLoad::Ok);
    s.load(QStringLiteral("other"), [&](const SecretLoad &r) { kind = r.kind; });
    QCOMPARE(kind, SecretLoad::Missing);
}

// ---- the runtime ------------------------------------------------------------------------------

namespace {

// A runtime on fakes, with its own bus name so that a real service never meets it.
struct RuntimeRig
{
    RuntimeRig(const QString &suffix, bool writeConfig = true)
    {
        QVERIFY2(dir.isValid(), "temporary directory");
        if (writeConfig) {
            QFile f(dir.path() + "/config.json");
            f.open(QIODevice::WriteOnly);
            f.write(kGoodConfig);
        }
        options.identity = ServiceIdentity::fromNames(QStringLiteral("it.michelebrami"),
                                                      QStringLiteral("libre-monitor-test") + suffix);
        options.core.dataDirectory = dir.path();
        options.core.configPath = dir.path() + "/config.json";
        options.core.adapterLaunch = [](const QString &) {
            AdapterLaunch l;
            l.program = QStringLiteral("/nonexistent/adapter");     // no process: the channel below answers
            return l;
        };
        // The adapter is a script over in-memory pipes (support/scriptedadapter.h): the real
        // driver, guard and scheduler run against it, so the Subjects can be polled.
        script.reset(new AdapterScript);
        options.core.channelFactory = scriptedChannels(script);
        options.exitOnStop = false;
        names.reset(new DBusNameOwner(options.identity.busName));
        platform.clock = &clock;
        platform.random = &random;
        platform.wake = &wake;
        platform.notifier = &notifier;
        platform.approval = &approval;
        platform.secrets = &secrets;
        platform.credentials = &secrets;
        platform.connectionSettings = &settings;
        platform.names = names.get();
    }

    // Moves the fake clock and fires the wake-up until `done()` or the steps run out.
    bool pollUntil(const std::function<bool()> &done, int steps = 80, int stepS = 5)
    {
        for (int i = 0; i < steps && !done(); ++i) {
            clock.advanceS(stepS);
            wake.fire();
            QTest::qWait(20);
        }
        return done();
    }

    QTemporaryDir dir;
    RuntimeOptions options;
    FakeServiceClock clock;
    QrandRandom random;
    FakeWake wake;
    FakeNotifier notifier;
    FakeApproval approval;
    MemorySecretStore secrets;
    AdapterScriptPtr script;
    JsonFileSettings settings { dir.path() + "/settings.json" };
    std::unique_ptr<DBusNameOwner> names;
    ServicePlatform platform;
};

bool haveBus()
{
    return QDBusConnection::sessionBus().isConnected();
}

} // namespace

void TstDaemon::statusJsonHasNoNameAndNoSecret()
{
    if (!haveBus())
        QSKIP("no session bus");
    RuntimeRig rig(QStringLiteral("-status"));
    rig.secrets.setCredentials(QStringLiteral("c1"), QStringLiteral("user-sentinel"), QStringLiteral("password-sentinel"));
    ServiceRuntime runtime(rig.options, rig.platform);
    QCOMPARE(runtime.begin(), -1);
    const QString json = runtime.apiStatusJson();
    QVERIFY(!json.contains(QStringLiteral("A Real Name")));
    QVERIFY(!json.contains(QStringLiteral("sentinel")));
    QVERIFY(!json.contains(QStringLiteral("opaque-1")));        // the source's own reference stays inside
    const QJsonObject o = QJsonDocument::fromJson(json.toUtf8()).object();
    QCOMPARE(o.value("service").toObject().value("state").toString(), QStringLiteral("running"));
    QCOMPARE(o.value("subjects").toArray().size(), 1);
    QCOMPARE(o.value("subjects").toArray().first().toObject().value("subject").toString(),
             QStringLiteral("{d2f1c9a0-5b3e-4c7d-8a91-0e2b4c6d8f10}"));
    runtime.stop(StopReason::requested());
    int guard = 0;
    while (!runtime.stopped() && guard++ < 100) QTest::qWait(100);
    QVERIFY(runtime.stopped());
}

void TstDaemon::runtimeWithoutBusReportsIt()
{
    if (haveBus())
        QSKIP("a session bus exists");
    RuntimeRig rig(QStringLiteral("-nobus"));
    ServiceRuntime runtime(rig.options, rig.platform);
    QCOMPARE(runtime.begin(), int(ServiceExitBus));
}

void TstDaemon::dbusInterfaceRoundTrip()
{
    if (!haveBus())
        QSKIP("no session bus");
    RuntimeRig rig(QStringLiteral("-rt"));
    ServiceRuntime runtime(rig.options, rig.platform);
    QCOMPARE(runtime.begin(), -1);

    ServiceClient client(rig.options.identity.busName, rig.options.identity.objectPath);
    QSignalSpy warnings(&client, SIGNAL(warning(QString,QString,QString)));
    QSignalSpy gaps(&client, SIGNAL(gapDetected(QString,QString)));

    // Blocking client calls into the same process need the event loop to answer: a worker
    // thread makes the calls while this one runs the loop.
    QString state, status;
    class Caller : public QThread
    {
    public:
        Caller(const ServiceIdentity &id, QString *s, QString *st) : id(id), s(s), st(st) {}
        void run()
        {
            QDBusConnection::connectToBus(QDBusConnection::SessionBus, QStringLiteral("caller"));
            QDBusInterface i(id.busName, id.objectPath, QString::fromLatin1(kServiceInterface),
                             QDBusConnection::connectToBus(QDBusConnection::SessionBus, QStringLiteral("caller")));
            i.setTimeout(5000);
            *s = i.call(QStringLiteral("Start")).arguments().value(0).toString();
            *st = i.call(QStringLiteral("Status")).arguments().value(0).toString();
        }
        ServiceIdentity id; QString *s; QString *st;
    } caller(rig.options.identity, &state, &status);
    caller.start();
    while (!caller.isFinished())
        QTest::qWait(20);
    QCOMPARE(state, QStringLiteral("running"));
    QVERIFY(QJsonDocument::fromJson(status.toUtf8()).isObject());

    // A warning raised inside the service reaches a client as a D-Bus signal.
    runtime.connectionWarning(QStringLiteral("c1"), QStringLiteral("protocol-changed"), QString());
    QTRY_COMPARE_WITH_TIMEOUT(warnings.count(), 1, 3000);
    QCOMPARE(warnings.first().at(0).toString(), QStringLiteral("protocol-changed"));

    runtime.stop(StopReason::requested());
    int guard = 0;
    while (!runtime.stopped() && guard++ < 100) QTest::qWait(100);
}

void TstDaemon::secondInstanceStepsAsideWritingNothing()
{
    if (!haveBus())
        QSKIP("no session bus");
    RuntimeRig first(QStringLiteral("-two"));
    ServiceRuntime a(first.options, first.platform);
    QCOMPARE(a.begin(), -1);

    // The second instance uses its own bus connection name, as a second process would have
    // its own connection; the DBusNameOwner of this process would reuse the first one, so the
    // check is made at the lifecycle level with a name holder that finds the name taken.
    RuntimeRig second(QStringLiteral("-two"));
    FakeNameOwner taken;
    taken.available = false;
    second.platform.names = &taken;
    ServiceLifecycle probe([&] {
        LifecyclePorts p;
        p.clock = &second.clock; p.names = &taken; p.notifier = &second.notifier;
        return p;
    }());
    QCOMPARE(probe.start(), StartOutcome::NameTaken);
    QVERIFY(second.notifier.shown.isEmpty());
    QVERIFY(!QFile::exists(second.dir.path() + "/journal.db"));     // nothing created
    QVERIFY(!QFile::exists(second.dir.path() + "/store.db"));

    a.stop(StopReason::requested());
    int guard = 0;
    while (!a.stopped() && guard++ < 100) QTest::qWait(100);
}

void TstDaemon::stopMethodWritesAStopRecordWithReasonRequested()
{
    if (!haveBus())
        QSKIP("no session bus");
    RuntimeRig rig(QStringLiteral("-stop"));
    ServiceRuntime runtime(rig.options, rig.platform);
    QCOMPARE(runtime.begin(), -1);
    runtime.apiStop();
    int guard = 0;
    while (!runtime.stopped() && guard++ < 100) QTest::qWait(100);
    QVERIFY(runtime.stopped());

    Journal journal;
    QVERIFY(journal.openReadOnly(rig.dir.path() + "/journal.db"));
    const QVector<JournalEntry> stops = journal.entries(JournalKind::stop());
    QCOMPARE(stops.size(), 1);
    QCOMPARE(stops.first().extra.value("reason").toString(), QStringLiteral("requested"));
    QCOMPARE(journal.entries(JournalKind::start()).size(), 1);
}

void TstDaemon::startMaintenanceNeedsARunningService()
{
    if (!haveBus())
        QSKIP("no session bus");
    RuntimeRig rig(QStringLiteral("-maint"));
    ServiceRuntime runtime(rig.options, rig.platform);
    QCOMPARE(runtime.apiStartMaintenance(QStringLiteral("compaction"), QStringLiteral("{}")), QStringLiteral("service-not-running"));
    QCOMPARE(runtime.begin(), -1);
    QCOMPARE(runtime.apiStartMaintenance(QStringLiteral("compaction"), QStringLiteral("not json")), QStringLiteral("parameters-not-json"));
    // REQ-STO-13: a window below 14 days is refused before anything starts.
    QCOMPARE(runtime.apiStartMaintenance(QStringLiteral("compaction"), QStringLiteral("{\"window_days\":13}")), QStringLiteral("parameter-window"));
    // export and reimport are gone; they are unknown kinds like any other word.
    QCOMPARE(runtime.apiStartMaintenance(QStringLiteral("export"), QStringLiteral("{\"target\":\"/tmp/x\"}")), QStringLiteral("unknown-kind"));
    QCOMPARE(runtime.apiStartMaintenance(QStringLiteral("reimport"), QStringLiteral("{\"archive\":\"/tmp/x\"}")), QStringLiteral("unknown-kind"));
    QCOMPARE(runtime.apiStartMaintenance(QStringLiteral("shred"), QStringLiteral("{}")), QStringLiteral("unknown-kind"));
    QVERIFY(!runtime.maintenance()->isRunning());
    runtime.stop(StopReason::requested());
    int guard = 0;
    while (!runtime.stopped() && guard++ < 100) QTest::qWait(100);
}

void TstDaemon::aLockedSecretStoreLeadsToAwaitingApprovalAndBack()
{
    if (!haveBus())
        QSKIP("no session bus");
    RuntimeRig rig(QStringLiteral("-lock"));
    rig.secrets.setCredentials(QStringLiteral("c1"), QStringLiteral("u"), QStringLiteral("p"));
    rig.secrets.lock();                         // as after a phone restart
    ServiceRuntime runtime(rig.options, rig.platform);
    QCOMPARE(runtime.begin(), -1);
    for (int i = 0; i < 4; ++i) {               // the first poll is due at once
        rig.clock.advanceS(30);
        rig.wake.fire();
        QTest::qWait(50);
    }
    QCOMPARE(runtime.lifecycle()->state(), ServiceState::RunningAwaitingApproval);
    QCOMPARE(rig.notifier.last().state, NoticeState::AwaitingApproval);
    QCOMPARE(rig.approval.requests.size(), 1);
    runtime.stop(StopReason::requested());
    int guard = 0;
    while (!runtime.stopped() && guard++ < 100) QTest::qWait(100);
}

// ConfigChanged: the intervals of known connections are applied at once; adding or
// removing a connection or a Subject cannot be applied to a running scheduler, so the answer
// asks for a restart; a file that does not parse is refused and the service keeps running.
void TstDaemon::configChangedAppliesIntervalsAndAsksForARestartOtherwise()
{
    if (!haveBus())
        QSKIP("no session bus");
    RuntimeRig rig(QStringLiteral("-cfg"));
    ServiceRuntime runtime(rig.options, rig.platform);
    QCOMPARE(runtime.begin(), -1);
    const QString path = rig.dir.path() + "/config.json";
    const QString subject = QStringLiteral("{\"id\":\"{d2f1c9a0-5b3e-4c7d-8a91-0e2b4c6d8f10}\",\"ref\":\"opaque-1\"}");
    auto write = [&](const QString &connections) {
        QFile f(path);
        QVERIFY(f.open(QIODevice::WriteOnly | QIODevice::Truncate));
        f.write(QStringLiteral("{\"format\":1,\"connections\":[%1]}").arg(connections).toUtf8());
    };

    QCOMPARE(runtime.apiConfigChanged(), QString());        // the file as it was
    write(QStringLiteral("{\"id\":\"c1\",\"adapter\":\"libre\",\"subjects\":[%1],\"current_interval_s\":120}").arg(subject));
    QCOMPARE(runtime.apiConfigChanged(), QString());        // only an interval changed
    QCOMPARE(runtime.core()->connectionConfig(QStringLiteral("c1")).currentIntervalMs, qint64(120000));

    write(QStringLiteral("{\"id\":\"c1\",\"adapter\":\"libre\",\"subjects\":[%1]},"
                         "{\"id\":\"c2\",\"adapter\":\"libre\",\"subjects\":[]}").arg(subject));
    QCOMPARE(runtime.apiConfigChanged(), QStringLiteral("restart-needed"));
    write(QStringLiteral("{\"id\":\"c1\",\"adapter\":\"libre\",\"subjects\":"
                         "[{\"id\":\"{d2f1c9a0-5b3e-4c7d-8a91-0e2b4c6d8f10}\",\"ref\":\"another\"}]}"));
    QCOMPARE(runtime.apiConfigChanged(), QStringLiteral("restart-needed"));

    QFile f(path);
    QVERIFY(f.open(QIODevice::WriteOnly | QIODevice::Truncate));
    f.write("not json");
    f.close();
    QCOMPARE(runtime.apiConfigChanged(), QStringLiteral("config-invalid"));
    QCOMPARE(runtime.lifecycle()->state(), ServiceState::Running);      // it keeps running

    runtime.stop(StopReason::requested());
    int guard = 0;
    while (!runtime.stopped() && guard++ < 100) QTest::qWait(100);
}

// ---- the follow-ups for the user interface ------------------------------------

namespace {

const char *const kSubjectUuid = "{d2f1c9a0-5b3e-4c7d-8a91-0e2b4c6d8f10}";

void stopAndWait(ServiceRuntime &runtime)
{
    runtime.stop(StopReason::requested());
    int guard = 0;
    while (!runtime.stopped() && guard++ < 100) QTest::qWait(100);
}

// A source that gives one reading a minute with a trend arrow and a sensor, and records what
// it was asked. `trend` < 0 sends none.
void giveReadings(RuntimeRig &rig, int *trend)
{
    FakeServiceClock *clock = &rig.clock;
    rig.script->fetchCurrent = [clock, trend](const QString &ref) {
        AdapterAnswer a;
        a.records.append(scriptedSample(ref, UtcTime::fromMSecsSinceEpoch(clock->now().msecsSinceEpoch() - 40000)));
        if (*trend >= 0)
            a.payload.insert(QStringLiteral("trend"), *trend);
        QJsonObject sensor;
        sensor.insert(QStringLiteral("id"), QStringLiteral("sensor-hash-0001"));
        sensor.insert(QStringLiteral("activated_at"), QStringLiteral("2026-01-01T00:00:00.000Z"));
        sensor.insert(QStringLiteral("warmup_minutes"), 60);
        a.payload.insert(QStringLiteral("sensor"), sensor);
        return a;
    };
}

// A bus connection of its own for a client, as a second process has: a client that shares the
// service's connection in one test process is answered by a local shortcut that cannot carry a
// delayed reply, which `Discover` is.
QDBusConnection clientBus()
{
    return QDBusConnection::connectToBus(QDBusConnection::SessionBus, QStringLiteral("tst-daemon-client"));
}

QJsonObject firstSubject(ServiceRuntime &runtime)
{
    const QJsonObject root = QJsonDocument::fromJson(runtime.apiStatusJson().toUtf8()).object();
    return root.value("subjects").toArray().first().toObject();
}

} // namespace

void TstDaemon::configRejectsTheReservedProbeIdentifier()
{
    // `probe` is the secret name of the login test and never a connection.
    const ServiceConfig c = parseServiceConfig("{\"format\":1,\"connections\":[{\"id\":\"probe\",\"adapter\":\"libre\",\"subjects\":[]}]}");
    QVERIFY(!c.ok);
    QCOMPARE(c.error, QStringLiteral("config-connection-id"));
    QVERIFY(parseServiceConfig("{\"format\":1,\"connections\":[{\"id\":\"probe2\",\"adapter\":\"libre\",\"subjects\":[]}]}").ok);
}

void TstDaemon::configChangedRefusesTheReservedProbeIdentifier()
{
    if (!haveBus())
        QSKIP("no session bus");
    RuntimeRig rig(QStringLiteral("-reserved"));
    ServiceRuntime runtime(rig.options, rig.platform);
    QCOMPARE(runtime.begin(), -1);
    QFile f(rig.dir.path() + "/config.json");
    QVERIFY(f.open(QIODevice::WriteOnly | QIODevice::Truncate));
    f.write("{\"format\":1,\"connections\":[{\"id\":\"probe\",\"adapter\":\"libre\",\"subjects\":[]}]}");
    f.close();
    QCOMPARE(runtime.apiConfigChanged(), QStringLiteral("config-invalid"));
    QCOMPARE(runtime.lifecycle()->state(), ServiceState::Running);       // it keeps what it had
    stopAndWait(runtime);

    // At the start the same file stops the service with the notice for an unusable file.
    RuntimeRig again(QStringLiteral("-reserved2"));
    QFile g(again.dir.path() + "/config.json");
    QVERIFY(g.open(QIODevice::WriteOnly | QIODevice::Truncate));
    g.write("{\"format\":1,\"connections\":[{\"id\":\"probe\",\"adapter\":\"libre\",\"subjects\":[]}]}");
    g.close();
    ServiceRuntime refused(again.options, again.platform);
    QCOMPARE(refused.begin(), int(ServiceExitOpenFailed));
}

// per Subject the adapter and its version, whether the report is
// proposed, the sensor and the trend arrow with the time of its reading; the arrow comes from the
// scripted adapter through the driver and the scheduler, and goes when its reading is old.
void TstDaemon::statusCarriesAdapterSensorAndTrendAndDropsTheOldTrend()
{
    if (!haveBus())
        QSKIP("no session bus");
    RuntimeRig rig(QStringLiteral("-status2"));
    rig.secrets.setCredentials(QStringLiteral("c1"), QStringLiteral("u"), QStringLiteral("p"));
    int trend = 4;
    giveReadings(rig, &trend);
    ServiceRuntime runtime(rig.options, rig.platform);
    QCOMPARE(runtime.begin(), -1);

    // Before any poll: the fields are there, empty.
    QJsonObject s = firstSubject(runtime);
    QCOMPARE(s.value("adapter").toString(), QStringLiteral("libre"));
    QCOMPARE(s.value("adapter_version").toString(), AdapterRegistry::instance().find("libre")->version);
    QCOMPARE(s.value("report_proposed").toBool(true), false);
    QVERIFY(s.contains("sensor") && s.value("sensor").isNull());
    QVERIFY(s.contains("trend_code") && s.value("trend_code").isNull());
    QCOMPARE(s.value("trend_for").toString(), QString());

    QVERIFY2(rig.pollUntil([&]() { return !firstSubject(runtime).value("last_success_at").toString().isEmpty(); }),
             "no reading was polled");
    s = firstSubject(runtime);
    QCOMPARE(s.value("trend_code").toInt(-1), 4);
    QCOMPARE(s.value("trend_for").toString(), s.value("last_success_at").toString());   // the arrow of THIS reading
    const QJsonObject sensor = s.value("sensor").toObject();
    QCOMPARE(sensor.value("id").toString(), QStringLiteral("sensor-hash-0001"));
    QCOMPARE(sensor.value("activated_at").toString(), QStringLiteral("2026-01-01T00:00:00.000Z"));
    QCOMPARE(sensor.value("warmup_minutes").toInt(), 60);

    // Nothing in it that identifies a person or the account.
    const QString text = runtime.apiStatusJson();
    QVERIFY(!text.contains(QStringLiteral("opaque-1")));
    QVERIFY(!text.contains(QStringLiteral("A Real Name")));

    // No further reading: past the staleness limit the arrow is gone, the reading time is not.
    rig.clock.advanceS(400);
    s = firstSubject(runtime);
    QVERIFY(s.value("trend_code").isNull());
    QCOMPARE(s.value("trend_for").toString(), QString());
    QVERIFY(!s.value("last_success_at").toString().isEmpty());
    stopAndWait(runtime);
}

void TstDaemon::stateChangedNamesTheConnection()
{
    if (!haveBus())
        QSKIP("no session bus");
    RuntimeRig rig(QStringLiteral("-statesig"));
    rig.secrets.setCredentials(QStringLiteral("c1"), QStringLiteral("u"), QStringLiteral("p"));
    int trend = 3;
    giveReadings(rig, &trend);
    ServiceRuntime runtime(rig.options, rig.platform);
    QCOMPARE(runtime.begin(), -1);
    ServiceClient client(rig.options.identity.busName, rig.options.identity.objectPath);
    QSignalSpy states(&client, SIGNAL(stateChanged(QString,QString,QString,QString)));
    QVERIFY(rig.pollUntil([&]() { return states.count() >= 2; }));
    for (int i = 0; i < states.count(); ++i) {
        QCOMPARE(states.at(i).at(0).toString(), QString::fromLatin1(kSubjectUuid));
        QCOMPARE(states.at(i).at(3).toString(), QStringLiteral("c1"));
    }
    stopAndWait(runtime);
}

void TstDaemon::aReportProposalShowsInStatusAndIsClearedByReenable()
{
    if (!haveBus())
        QSKIP("no session bus");
    RuntimeRig rig(QStringLiteral("-report"));
    ServiceRuntime runtime(rig.options, rig.platform);
    QCOMPARE(runtime.begin(), -1);
    ServiceClient client(rig.options.identity.busName, rig.options.identity.objectPath);
    QSignalSpy proposed(&client, SIGNAL(diagnosticReportProposed(QString)));

    // Two guard kills of the connection in a row, as the scheduler reports them.
    SubjectStatus kill;
    kill.subject = QUuid(QString::fromLatin1(kSubjectUuid));
    kill.connection = QStringLiteral("c1");
    kill.state = ScheduleState::Backoff;
    kill.cause = QStringLiteral("guard-kill");
    runtime.events()->subjectStateChanged(kill);
    QCOMPARE(firstSubject(runtime).value("report_proposed").toBool(true), false);
    runtime.events()->subjectStateChanged(kill);
    QCOMPARE(firstSubject(runtime).value("report_proposed").toBool(false), true);
    QTRY_COMPARE_WITH_TIMEOUT(proposed.count(), 1, 3000);               // the signal still goes out, once
    QCOMPARE(proposed.first().at(0).toString(), QStringLiteral("c1"));

    // The UI that was closed at that moment finds it in Status(); a re-enable settles it.
    runtime.apiReenable(QStringLiteral("c1"));
    QCOMPARE(firstSubject(runtime).value("report_proposed").toBool(true), false);
    stopAndWait(runtime);
}

// the UI says the report was written or declined. The proposal ends at once, in Status()
// too, and returns only after two NEW consecutive guard kills (one kill after the answer is not
// enough). A call with no open proposal answers false and changes nothing, so an answer that
// arrives twice or for the wrong connection does no harm.
void TstDaemon::reportHandledEndsTheProposalUntilTwoNewKills()
{
    if (!haveBus())
        QSKIP("no session bus");
    RuntimeRig rig(QStringLiteral("-handled"));
    ServiceRuntime runtime(rig.options, rig.platform);
    QCOMPARE(runtime.begin(), -1);
    ServiceClient client(rig.options.identity.busName, rig.options.identity.objectPath, clientBus());
    QSignalSpy proposed(&client, SIGNAL(diagnosticReportProposed(QString)));
    QSignalSpy answers(&client, SIGNAL(reportHandledAnswered(bool,QString,bool)));

    SubjectStatus kill;
    kill.subject = QUuid(QString::fromLatin1(kSubjectUuid));
    kill.connection = QStringLiteral("c1");
    kill.state = ScheduleState::Backoff;
    kill.cause = QStringLiteral("guard-kill");

    QVERIFY(!runtime.apiReportHandled(QStringLiteral("c1")));           // nothing proposed yet
    runtime.events()->subjectStateChanged(kill);
    QVERIFY(!runtime.apiReportHandled(QStringLiteral("c1")));           // one kill is not a proposal
    runtime.events()->subjectStateChanged(kill);
    QCOMPARE(firstSubject(runtime).value("report_proposed").toBool(false), true);
    QVERIFY(!runtime.apiReportHandled(QStringLiteral("other")));        // another connection: no effect
    QCOMPARE(firstSubject(runtime).value("report_proposed").toBool(false), true);

    client.reportHandledAsync(QStringLiteral("c1"));                    // over the bus
    QTRY_COMPARE_WITH_TIMEOUT(answers.count(), 1, 5000);
    QVERIFY(answers.first().at(0).toBool());
    QVERIFY(answers.first().at(2).toBool());
    QCOMPARE(firstSubject(runtime).value("report_proposed").toBool(true), false);
    QVERIFY(!runtime.apiReportHandled(QStringLiteral("c1")));           // a second answer does nothing

    runtime.events()->subjectStateChanged(kill);                        // one new kill: not yet
    QCOMPARE(firstSubject(runtime).value("report_proposed").toBool(true), false);
    runtime.events()->subjectStateChanged(kill);                        // the second new kill
    QCOMPARE(firstSubject(runtime).value("report_proposed").toBool(false), true);
    QTRY_COMPARE_WITH_TIMEOUT(proposed.count(), 2, 3000);               // proposed again, once more
    stopAndWait(runtime);
}

// DiagnosticReport(connection) over the bus. It answers in words while
// nothing is proposed or the connection is unknown; with a proposal but no noted guard kill it says
// so; with both it returns the JSON Lines text (versions, command, counts, the batch's shape) and no
// measurement value, reference or name of the synthetic batch; asking for it does NOT end the
// proposal (ReportHandled does). The guard kill is fed to the service's own collector the way
// the driver feeds it.
void TstDaemon::diagnosticReportAnswersAnOpenProposalOverTheBusWithoutAnyValue()
{
    if (!haveBus())
        QSKIP("no session bus");
    RuntimeRig rig(QStringLiteral("-diag"));
    ServiceRuntime runtime(rig.options, rig.platform);
    QCOMPARE(runtime.begin(), -1);
    ServiceClient client(rig.options.identity.busName, rig.options.identity.objectPath, clientBus());
    const auto obj = [](const QString &t) { return QJsonDocument::fromJson(t.toUtf8()).object(); };
    const auto errorOf = [obj](const QString &text) { return obj(text).value("error").toString(); };
    // The asynchronous form: the service answers from this same thread's event loop, which a
    // blocking call would stop.
    QSignalSpy answers(&client, SIGNAL(diagnosticReportAnswered(bool,QString,QString)));
    const auto ask = [&](const QString &connection) -> QString {
        const int before = answers.count();
        client.diagnosticReportAsync(connection);
        for (int waited = 0; answers.count() <= before && waited < 5000; waited += 20)
            QTest::qWait(20);       // Qt 5.6 has no qWaitFor
        if (answers.count() <= before)
            return QStringLiteral("(no answer)");
        const QList<QVariant> a = answers.last();
        return a.at(0).toBool() && a.at(1).toString() == connection ? a.at(2).toString() : QStringLiteral("(bad answer)");
    };

    QCOMPARE(errorOf(ask(QStringLiteral("c1"))), QStringLiteral("not-proposed"));
    QCOMPARE(errorOf(ask(QStringLiteral("nobody"))), QStringLiteral("unknown-connection"));

    SubjectStatus kill;
    kill.subject = QUuid(QString::fromLatin1(kSubjectUuid));
    kill.connection = QStringLiteral("c1");
    kill.state = ScheduleState::Backoff;
    kill.cause = QStringLiteral("guard-kill");
    runtime.events()->subjectStateChanged(kill);
    runtime.events()->subjectStateChanged(kill);
    QCOMPARE(firstSubject(runtime).value("report_proposed").toBool(false), true);
    QCOMPARE(errorOf(ask(QStringLiteral("c1"))), QStringLiteral("no-report"));

    // The driver's view of a guard kill in the middle of a batch.
    DiagnosticCollector *d = runtime.core()->diagnostics();
    QVERIFY(d);
    LinkId link;
    link.connectionId = QStringLiteral("c1");
    link.adapterId = QStringLiteral("libre");
    link.adapterVersion = QStringLiteral("0.3.0");
    Violation v;
    v.link = link;
    v.kind = ViolationKind::UndeclaredUnit;
    v.severity = Severity::Hard;
    v.detail = QStringLiteral("record unit not declared in hello");
    d->onViolation(v);
    const QByteArray batch = "{\"type\":\"records\",\"feed\":\"current\",\"records\":[{\"shape\":\"sample\","
                             "\"subject_ref\":\"opaque-1\",\"metric\":\"glucose\",\"value\":143.625,\"unit\":\"furlongs\","
                             "\"measured_at\":\"2026-01-02T03:04:05Z\",\"name\":\"A Real Name\"}]}";
    d->rejected(link, batch, CommandOutcome::Guard, true, ViolationKind::UndeclaredUnit);
    CommandOutcome o;
    o.link = link;
    o.commandType = QStringLiteral("fetch_current");
    o.killedBy = CommandOutcome::Guard;
    o.result = CommandOutcome::Error;
    d->onCommandFinished(o);

    const QString text = ask(QStringLiteral("c1"));
    const QStringList lines = text.split(QLatin1Char('\n'), QString::SkipEmptyParts);
    QCOMPARE(lines.size(), 3);
    QCOMPARE(obj(lines.at(0)).value("application_version").toString(), rig.options.appVersion);
    QCOMPARE(obj(lines.at(0)).value("command").toString(), QStringLiteral("fetch_current"));
    QCOMPARE(obj(lines.at(2)).value("value").toString(), QStringLiteral("number"));
    for (const char *secret : { "143", "opaque-1", "A Real Name", "furlongs", "2026-01-02" })
        QVERIFY2(!text.contains(QString::fromUtf8(secret)), secret);
    // Reading the report does not settle the proposal; the user's answer does.
    QCOMPARE(firstSubject(runtime).value("report_proposed").toBool(false), true);
    QVERIFY(runtime.apiReportHandled(QStringLiteral("c1")));
    QCOMPARE(errorOf(ask(QStringLiteral("c1"))), QStringLiteral("not-proposed"));
    stopAndWait(runtime);
    QCOMPARE(errorOf(runtime.apiDiagnosticReport(QStringLiteral("c1"))), QStringLiteral("service-not-running"));
}

// ---- the alerts in the service -----------------------------------------------------------------

namespace {

const char *const kSubjectKey = "{d2f1c9a0-5b3e-4c7d-8a91-0e2b4c6d8f10}";

// Records what the alert coordinator asks the platform to show; stands in for the phone's
// notifications.
class RecordingAlertNotifier : public AlertNotifier
{
public:
    void deliver(const AlertEffect &e) { seen.append(e); }
    int count(AlertEffect::Kind k) const
    {
        int n = 0;
        for (int i = 0; i < seen.size(); ++i) n += seen.at(i).kind == k;
        return n;
    }
    QList<AlertEffect> seen;
};

// A source whose newest `current` reading is `*value` mg/dL, one a minute.
void giveValue(RuntimeRig &rig, double *value)
{
    FakeServiceClock *clock = &rig.clock;
    rig.script->fetchCurrent = [clock, value](const QString &ref) {
        AdapterAnswer a;
        a.records.append(scriptedSample(ref, UtcTime::fromMSecsSinceEpoch(clock->now().msecsSinceEpoch() - 40000), *value));
        return a;
    };
}

QJsonObject alertsOf(ServiceRuntime &runtime) { return QJsonDocument::fromJson(runtime.apiAlerts().toUtf8()).object(); }

QJsonObject slotOf(ServiceRuntime &runtime, const char *slot)
{
    const QJsonArray a = alertsOf(runtime).value("alerts").toArray();
    for (int i = 0; i < a.size(); ++i)
        if (a.at(i).toObject().value("slot").toString() == QLatin1String(slot))
            return a.at(i).toObject();
    return QJsonObject();
}

QJsonObject thresholdsOf(ServiceRuntime &runtime)
{
    return alertsOf(runtime).value("thresholds").toArray().first().toObject();
}

QJsonObject json(const QString &text) { return QJsonDocument::fromJson(text.toUtf8()).object(); }

} // namespace

// Only the Subject's short label is read from the file for the notifications;
// a name is still never loaded, and a label that is longer than three characters is not a label.
void TstDaemon::configReadsOnlyTheShortLabelOfASubject()
{
    const QByteArray text =
        "{\"format\":1,\"connections\":[{\"id\":\"c1\",\"adapter\":\"libre\",\"subjects\":["
        "{\"id\":\"{d2f1c9a0-5b3e-4c7d-8a91-0e2b4c6d8f10}\",\"ref\":\"a\",\"name\":\"A Real Name\",\"label\":\"AB\"},"
        "{\"id\":\"{d2f1c9a0-5b3e-4c7d-8a91-0e2b4c6d8f11}\",\"ref\":\"b\",\"name\":\"Other Name\"}]}]}";
    const ServiceConfig c = parseServiceConfig(text);
    QVERIFY(c.ok);
    QCOMPARE(c.subjectOrder.size(), 2);
    QCOMPARE(c.subjectOrder.first(), QUuid(QString::fromLatin1(kSubjectKey)));
    QCOMPARE(c.subjectLabels.size(), 1);
    QCOMPARE(c.subjectLabels.value(QUuid(QString::fromLatin1(kSubjectKey))), QStringLiteral("AB"));
    for (QHash<QUuid, QString>::const_iterator it = c.subjectLabels.constBegin(); it != c.subjectLabels.constEnd(); ++it)
        QVERIFY(!it.value().contains(QStringLiteral("Name")));
    // The presenter's book decides what counts as short.
    SubjectLabelBook book;
    book.set(c.subjectOrder, c.subjectLabels);
    QCOMPARE(book.labelOf(c.subjectOrder.at(0)), QStringLiteral("AB"));
    QCOMPARE(book.labelOf(c.subjectOrder.at(1)), QStringLiteral("P2"));   // no label: its position
}

// The version the code sees is the spec's "Version-Release", read by
// common.pri from the spec itself, and there is no stage word: the About page and the first-run page
// show "LM_APP_VERSION" only, and the service writes the version into the export
// header and the diagnostic report.
void TstDaemon::theVersionDefineIsTheSpecsVersionAndRelease()
{
    QFile spec(QStringLiteral(LM_TRANSLATIONS_DIR "/../../rpm/harbour-libre-monitor.spec"));
    QVERIFY(spec.open(QIODevice::ReadOnly | QIODevice::Text));
    QString version, release;
    while (!spec.atEnd()) {
        const QString line = QString::fromUtf8(spec.readLine()).trimmed();
        if (line.startsWith(QLatin1String("Version:")))
            version = line.mid(8).trimmed();
        else if (line.startsWith(QLatin1String("Release:")))
            release = line.mid(8).trimmed();
    }
    QVERIFY(!version.isEmpty() && !release.isEmpty());
    QCOMPARE(QStringLiteral(LM_APP_VERSION), version + QLatin1Char('-') + release);
    QCOMPARE(QStringLiteral(LM_APP_VERSION), QStringLiteral("1.0.0-1"));
}

// The alarm's banner must not wait behind the resident notice's own banner: in
// the whole service, in the step in which the alarm is raised, the notification is posted first and the
// resident notice is refreshed after it (the change is announced only once the notification is up).
void TstDaemon::theAlarmIsPostedBeforeTheResidentNoticeIsRefreshed()
{
    if (!haveBus())
        QSKIP("no session bus");
    RuntimeRig rig(QStringLiteral("-order"));
    rig.secrets.setCredentials(QStringLiteral("c1"), QStringLiteral("u"), QStringLiteral("p"));
    double value = 50;
    giveValue(rig, &value);
    QStringList calls;
    FakeNoteBackend backend;
    backend.calls = &calls;
    rig.notifier.calls = &calls;
    MemoryNoteSettings noteSettings;
    SubjectLabelBook labels;
    AlertNotificationPresenter presenter(&backend, &noteSettings, &labels, &rig.clock);
    rig.platform.alerts = &presenter;
    rig.platform.presenter = &presenter;
    rig.platform.labels = &labels;
    ServiceRuntime runtime(rig.options, rig.platform);
    QCOMPARE(runtime.begin(), -1);
    int before = 0;
    for (int i = 0; i < 80 && backend.posts.isEmpty(); ++i) {
        before = calls.size();
        rig.clock.advanceS(5);
        rig.wake.fire();
        QTest::qWait(20);
    }
    QVERIFY2(!backend.posts.isEmpty(), "no alarm was raised");
    const QStringList step = calls.mid(before);
    QVERIFY2(step.contains(QStringLiteral("post")), qPrintable(step.join(QLatin1Char(' '))));
    QVERIFY2(step.contains(QStringLiteral("resident")), "the resident notice is not refreshed by the alert");
    QVERIFY2(step.indexOf(QStringLiteral("post")) < step.indexOf(QStringLiteral("resident")), qPrintable(step.join(QLatin1Char(' '))));
    stopAndWait(runtime);
}

// The three per-Subject alert keys: whole seconds, absent = default, nothing
// clamped when read (the engine clamps), and a strange value never refuses the file.
void TstDaemon::configReadsThePerSubjectAlertKeysAsWholeSecondsAndAnythingElseIsAbsent()
{
    auto parse = [](const QString &subjectFields) {
        return parseServiceConfig(QStringLiteral("{\"format\":1,\"connections\":[{\"id\":\"c1\",\"adapter\":\"libre\",\"subjects\":"
                                                 "[{\"id\":\"{d2f1c9a0-5b3e-4c7d-8a91-0e2b4c6d8f10}\",\"ref\":\"a\"%1}]}]}")
                                      .arg(subjectFields).toUtf8());
    };
    const QUuid id(QString::fromLatin1(kSubjectKey));
    ServiceConfig c = parse(QString());
    QVERIFY(c.ok);
    QVERIFY(!c.subjectAlertKeys.contains(id));                 // none written: nothing to apply
    c = parse(QStringLiteral(",\"alert_persistence_low_s\":300,\"alert_persistence_high_s\":1200,\"missing_limit_s\":900"));
    QVERIFY(c.ok);
    QCOMPARE(c.subjectAlertKeys.value(id).persistenceLowS, qint64(300));
    QCOMPARE(c.subjectAlertKeys.value(id).persistenceHighS, qint64(1200));
    QCOMPARE(c.subjectAlertKeys.value(id).missingLimitS, qint64(900));
    c = parse(QStringLiteral(",\"alert_persistence_low_s\":0"));
    QCOMPARE(c.subjectAlertKeys.value(id).persistenceLowS, qint64(0));            // zero is a value, not absent
    QCOMPARE(c.subjectAlertKeys.value(id).persistenceHighS, qint64(-1));
    // Odd values count as absent and never stop the service from starting.
    const char *odd[] = { "-5", "1.5", "\"600\"", "null", "true", "[1]" };
    for (size_t i = 0; i < sizeof(odd) / sizeof(odd[0]); ++i) {
        c = parse(QStringLiteral(",\"missing_limit_s\":%1").arg(QLatin1String(odd[i])));
        QVERIFY2(c.ok, odd[i]);
        QVERIFY2(!c.subjectAlertKeys.contains(id), odd[i]);
    }
    c = parse(QStringLiteral(",\"missing_limit_s\":99999999999"));                // held at seven days; the engine clamps
    QCOMPARE(c.subjectAlertKeys.value(id).missingLimitS, qint64(7 * 24 * 3600));
}

void TstDaemon::thePerSubjectAlertKeysApplyAtConfigChangedAndAlertsReportThemWithTheirRanges()
{
    if (!haveBus())
        QSKIP("no session bus");
    RuntimeRig rig(QStringLiteral("-keys"));
    rig.secrets.setCredentials(QStringLiteral("c1"), QStringLiteral("u"), QStringLiteral("p"));
    double value = 100;
    giveValue(rig, &value);
    FakeNoteBackend backend;
    MemoryNoteSettings noteSettings;
    SubjectLabelBook labels;
    AlertNotificationPresenter presenter(&backend, &noteSettings, &labels, &rig.clock);
    rig.platform.alerts = &presenter;
    rig.platform.presenter = &presenter;
    rig.platform.labels = &labels;
    ServiceRuntime runtime(rig.options, rig.platform);
    QCOMPARE(runtime.begin(), -1);
    const QString path = rig.dir.path() + "/config.json";
    auto writeKeys = [&](const QString &keys) {
        QFile f(path);
        QVERIFY(f.open(QIODevice::WriteOnly | QIODevice::Truncate));
        f.write(QStringLiteral("{\"format\":1,\"connections\":[{\"id\":\"c1\",\"adapter\":\"libre\",\"subjects\":"
                               "[{\"id\":\"{d2f1c9a0-5b3e-4c7d-8a91-0e2b4c6d8f10}\",\"ref\":\"opaque-1\"%1}],\"current_interval_s\":60}]}")
                    .arg(keys).toUtf8());
    };

    // The defaults, and the ranges the thresholds page may show.
    QJsonObject th = thresholdsOf(runtime);
    QCOMPARE(th.value("persistence_low_s").toInt(-1), 0);
    QCOMPARE(th.value("persistence_high_s").toInt(-1), 15 * 60);
    QCOMPARE(th.value("missing_limit_s").toInt(-1), 15 * 60);
    const QJsonObject limits = alertsOf(runtime).value("limits").toObject();
    QCOMPARE(limits.value("persistence_low_s").toObject().value("max").toInt(-1), 15 * 60);
    QCOMPARE(limits.value("persistence_high_s").toObject().value("max").toInt(-1), 60 * 60);
    QCOMPARE(limits.value("persistence_high_s").toObject().value("default").toInt(-1), 15 * 60);
    QCOMPARE(limits.value("missing_limit_s").toObject().value("min").toInt(-1), 5 * 60);
    QCOMPARE(limits.value("missing_limit_s").toObject().value("max").toInt(-1), 60 * 60);

    // New values at ConfigChanged, no restart; the ones outside the range are clamped by the engine and
    // Alerts() reports what the engine uses.
    writeKeys(QStringLiteral(",\"alert_persistence_low_s\":120,\"alert_persistence_high_s\":7200,\"missing_limit_s\":30"));
    QCOMPARE(runtime.apiConfigChanged(), QString());
    th = thresholdsOf(runtime);
    QCOMPARE(th.value("persistence_low_s").toInt(-1), 120);
    QCOMPARE(th.value("persistence_high_s").toInt(-1), 60 * 60);       // 7200 s clamped to the 60 min maximum
    QCOMPARE(th.value("missing_limit_s").toInt(-1), 5 * 60);           // 30 s clamped to the 5 min minimum

    // A key taken out of the file brings the default back.
    writeKeys(QStringLiteral(",\"alert_persistence_high_s\":600"));
    QCOMPARE(runtime.apiConfigChanged(), QString());
    th = thresholdsOf(runtime);
    QCOMPARE(th.value("persistence_low_s").toInt(-1), 0);
    QCOMPARE(th.value("persistence_high_s").toInt(-1), 600);
    QCOMPARE(th.value("missing_limit_s").toInt(-1), 15 * 60);
    stopAndWait(runtime);
}

void TstDaemon::anUrgentLowIsListedAcknowledgedAndAnnouncedOverTheBus()
{
    if (!haveBus())
        QSKIP("no session bus");
    RuntimeRig rig(QStringLiteral("-alerts"));
    rig.secrets.setCredentials(QStringLiteral("c1"), QStringLiteral("u"), QStringLiteral("p"));
    double value = 50;                                          // below 54: urgent low, no persistence
    giveValue(rig, &value);
    RecordingAlertNotifier alertNotifier;
    rig.platform.alerts = &alertNotifier;
    ServiceRuntime runtime(rig.options, rig.platform);
    QCOMPARE(runtime.begin(), -1);
    ServiceClient client(rig.options.identity.busName, rig.options.identity.objectPath, clientBus());
    QSignalSpy changed(&client, SIGNAL(alertChanged(QString,QString,QString)));

    // Nothing is listed before the first reading; the thresholds are the global ones.
    QVERIFY(alertsOf(runtime).value("alerts").toArray().isEmpty());
    QCOMPARE(thresholdsOf(runtime).value("subject").toString(), QString::fromLatin1(kSubjectKey));
    QCOMPARE(thresholdsOf(runtime).value("ll").toInt(), 54);
    QCOMPARE(thresholdsOf(runtime).value("l").toInt(), 70);
    QCOMPARE(thresholdsOf(runtime).value("h").toInt(), 180);
    QCOMPARE(thresholdsOf(runtime).value("hh").toInt(), 250);

    QVERIFY2(rig.pollUntil([&]() { return !slotOf(runtime, "low").isEmpty(); }), "no urgent low was raised");
    QJsonObject low = slotOf(runtime, "low");
    QCOMPARE(low.value("level").toString(), QStringLiteral("LL"));
    QCOMPARE(low.value("state").toString(), QStringLiteral("active"));
    QCOMPARE(low.value("subject").toString(), QString::fromLatin1(kSubjectKey));
    QVERIFY(!low.value("episode").toString().isEmpty());
    QVERIFY(!low.value("since").toString().isEmpty());
    QCOMPARE(low.value("snooze_until").toString(), QString());
    // What the user sees was asked of the platform, and the UI heard of it over the bus.
    QVERIFY(alertNotifier.count(AlertEffect::Notify) >= 1);
    QTRY_VERIFY_WITH_TIMEOUT(changed.count() >= 1, 3000);
    QCOMPARE(changed.first().at(0).toString(), QString::fromLatin1(kSubjectKey));
    QCOMPARE(changed.first().at(1).toString(), QStringLiteral("low"));
    QCOMPARE(changed.first().at(2).toString(), QStringLiteral("active"));
    // No glucose value anywhere in the answer: not as a field, not with a unit.
    QVERIFY(!runtime.apiAlerts().contains(QStringLiteral("\"value\"")));
    QVERIFY(!runtime.apiAlerts().contains(QStringLiteral("mg/dL")));

    // The same answer over the bus, and the acknowledgement by the asynchronous client.
    QSignalSpy alertsAnswer(&client, SIGNAL(alertsAnswered(bool,QString)));
    QSignalSpy ackAnswer(&client, SIGNAL(acknowledgeAlertAnswered(bool,QString,QString,QString)));
    client.alertsAsync();
    QTRY_COMPARE_WITH_TIMEOUT(alertsAnswer.count(), 1, 5000);
    QVERIFY(alertsAnswer.first().at(0).toBool());
    QCOMPARE(json(alertsAnswer.first().at(1).toString()).value("alerts").toArray().size(), 1);

    const int before = changed.count();
    client.acknowledgeAlertAsync(QString::fromLatin1(kSubjectKey), QStringLiteral("low"));
    QTRY_COMPARE_WITH_TIMEOUT(ackAnswer.count(), 1, 5000);
    QVERIFY(ackAnswer.first().at(0).toBool());
    QCOMPARE(ackAnswer.first().at(3).toString(), QStringLiteral("done"));
    QCOMPARE(slotOf(runtime, "low").value("state").toString(), QStringLiteral("acknowledged"));
    QTRY_VERIFY_WITH_TIMEOUT(changed.count() > before, 3000);
    QCOMPARE(changed.last().at(2).toString(), QStringLiteral("acknowledged"));
    QVERIFY(alertNotifier.count(AlertEffect::Update) >= 1);

    // Refusals are words, and they change nothing.
    QCOMPARE(runtime.apiAcknowledgeAlert(QString::fromLatin1(kSubjectKey), QStringLiteral("low")), QStringLiteral("already-acknowledged"));
    QCOMPARE(runtime.apiAcknowledgeAlert(QString::fromLatin1(kSubjectKey), QStringLiteral("high")), QStringLiteral("not-active"));
    QCOMPARE(runtime.apiAcknowledgeAlert(QString::fromLatin1(kSubjectKey), QStringLiteral("sideways")), QStringLiteral("bad-slot"));
    QCOMPARE(runtime.apiAcknowledgeAlert(QStringLiteral("{00000000-0000-4000-8000-000000000001}"), QStringLiteral("low")), QStringLiteral("unknown-subject"));
    QCOMPARE(runtime.apiAcknowledgeAlert(QStringLiteral("not a uuid"), QStringLiteral("low")), QStringLiteral("unknown-subject"));
    QCOMPARE(slotOf(runtime, "low").value("state").toString(), QStringLiteral("acknowledged"));

    // The reading comes back in range: the alert clears and the UI is told.
    value = 110;
    QVERIFY2(rig.pollUntil([&]() { return slotOf(runtime, "low").isEmpty(); }, 120), "the alert did not clear");
    QTRY_VERIFY_WITH_TIMEOUT(changed.count() >= 3, 3000);
    QCOMPARE(changed.last().at(2).toString(), QStringLiteral("cleared"));
    QVERIFY(alertNotifier.count(AlertEffect::Withdraw) >= 1);
    stopAndWait(runtime);
}

// Snooze is for L, H and missing data only, and for four durations.
void TstDaemon::snoozeAndItsRefusalsAnswerInWords()
{
    if (!haveBus())
        QSKIP("no session bus");
    RuntimeRig rig(QStringLiteral("-snooze"));
    rig.secrets.setCredentials(QStringLiteral("c1"), QStringLiteral("u"), QStringLiteral("p"));
    double value = 50;                                          // urgent low first
    giveValue(rig, &value);
    ServiceRuntime runtime(rig.options, rig.platform);
    QCOMPARE(runtime.begin(), -1);
    const QString subject = QString::fromLatin1(kSubjectKey);

    QVERIFY(rig.pollUntil([&]() { return !slotOf(runtime, "low").isEmpty(); }));
    QCOMPARE(runtime.apiSnoozeAlert(subject, QStringLiteral("low"), 30), QStringLiteral("not-snoozable"));   // LL has no snooze
    QCOMPARE(slotOf(runtime, "low").value("state").toString(), QStringLiteral("active"));

    value = 65;                                                 // between 54 and 70: the low level L
    QVERIFY(rig.pollUntil([&]() { return slotOf(runtime, "low").value("level").toString() == QLatin1String("L"); }, 120));
    QCOMPARE(runtime.apiSnoozeAlert(subject, QStringLiteral("low"), 7), QStringLiteral("bad-duration"));
    QCOMPARE(runtime.apiSnoozeAlert(subject, QStringLiteral("high"), 30), QStringLiteral("not-active"));
    QCOMPARE(runtime.apiSnoozeAlert(subject, QStringLiteral("nowhere"), 30), QStringLiteral("bad-slot"));
    QCOMPARE(runtime.apiSnoozeAlert(subject, QStringLiteral("low"), 30), QStringLiteral("done"));
    const QJsonObject snoozed = slotOf(runtime, "low");
    QCOMPARE(snoozed.value("state").toString(), QStringLiteral("snoozed"));
    QVERIFY(!snoozed.value("snooze_until").toString().isEmpty());
    stopAndWait(runtime);
}

void TstDaemon::setThresholdsAnswersTheWordsOfTheBookWithTheDetailsOfTheRefusal()
{
    if (!haveBus())
        QSKIP("no session bus");
    RuntimeRig rig(QStringLiteral("-thresholds"));
    double value = 110;
    giveValue(rig, &value);
    ServiceRuntime runtime(rig.options, rig.platform);
    QCOMPARE(runtime.begin(), -1);
    const QString subject = QString::fromLatin1(kSubjectKey);

    // No change, an impossible number, a broken order (with the room that is left).
    QCOMPARE(json(runtime.apiSetThresholds(subject, 70, 180, -1, -1)).value("result").toString(), QStringLiteral("no-change"));
    QJsonObject r = json(runtime.apiSetThresholds(subject, 0, 180, -1, -1));
    QCOMPARE(r.value("result").toString(), QStringLiteral("invalid-value"));
    QCOMPARE(r.value("offending").toString(), QStringLiteral("L"));
    r = json(runtime.apiSetThresholds(subject, 190, 180, -1, -1));
    QCOMPARE(r.value("result").toString(), QStringLiteral("inconsistent"));
    QCOMPARE(r.value("l_min").toInt(), 55);
    QCOMPARE(r.value("l_max").toInt(), 179);
    QVERIFY(r.contains("h_min") && r.contains("h_max"));

    // Looser than the standard value: a challenge, enforced here, with the standard values.
    r = json(runtime.apiSetThresholds(subject, 60, 180, -1, -1));
    QCOMPARE(r.value("result").toString(), QStringLiteral("needs-confirmation"));
    QCOMPARE(r.value("global_l").toInt(), 70);
    QCOMPARE(r.value("global_h").toInt(), 180);
    r = json(runtime.apiSetThresholds(subject, 60, 180, 61, -1));
    QCOMPARE(r.value("result").toString(), QStringLiteral("confirmation-mismatch"));
    QCOMPARE(r.value("offending").toString(), QStringLiteral("L"));
    QCOMPARE(thresholdsOf(runtime).value("l").toInt(), 70);             // nothing changed yet

    // The value typed again matches: accepted, in force at once, and the answer says what is in force.
    // (The fake clock stands still, and the book orders entries by time: a person cannot make two
    // edits in one millisecond, so the test lets a second pass between them.)
    rig.clock.advanceS(1);
    r = json(runtime.apiSetThresholds(subject, 60, 180, 60, -1));
    QCOMPARE(r.value("result").toString(), QStringLiteral("accepted"));
    QCOMPARE(r.value("thresholds").toObject().value("l").toInt(), 60);
    QCOMPARE(thresholdsOf(runtime).value("l").toInt(), 60);
    QCOMPARE(thresholdsOf(runtime).value("ll").toInt(), 54);            // LL and HH are never edited
    // A change that does not go below the standard L or above the standard H needs no challenge,
    // whatever the value it replaces (the reference is the standard value, REQ-THR-02).
    rig.clock.advanceS(1);
    r = json(runtime.apiSetThresholds(subject, 75, 170, -1, -1));
    QCOMPARE(r.value("result").toString(), QStringLiteral("accepted"));
    QCOMPARE(thresholdsOf(runtime).value("h").toInt(), 170);

    // A Subject the service does not follow.
    QCOMPARE(json(runtime.apiSetThresholds(QStringLiteral("{00000000-0000-4000-8000-000000000002}"), 75, 170, -1, -1))
                 .value("result").toString(), QStringLiteral("unknown-subject"));
    // the method has no comment any more. A caller of the older shape (six arguments, the
    // last one the comment) is not served: the bus answers that no such method exists, which the
    // window reads as "an older service or application", and nothing changes.
    rig.clock.advanceS(1);
    {
        QDBusMessage old = QDBusMessage::createMethodCall(rig.options.identity.busName,
                                                          rig.options.identity.objectPath,
                                                          QString::fromLatin1(kServiceInterface),
                                                          QStringLiteral("SetThresholds"));
        old << subject << 76 << 170 << -1 << -1 << QStringLiteral("synthetic old note");
        // Asynchronous, with the event loop turning: the service answers from this same thread.
        QDBusPendingCall pending = clientBus().asyncCall(old, 5000);
        QTRY_VERIFY_WITH_TIMEOUT(pending.isFinished(), 5000);
        QVERIFY(pending.isError());
        QVERIFY2(ServiceClient::isOlderServiceError(pending.error().name()), qPrintable(pending.error().name()));
        QCOMPARE(thresholdsOf(runtime).value("l").toInt(), 75);
    }

    // Over the bus, with the typed value and the words back.
    rig.clock.advanceS(1);
    ServiceClient client(rig.options.identity.busName, rig.options.identity.objectPath, clientBus());
    QSignalSpy answered(&client, SIGNAL(setThresholdsAnswered(bool,QString,QString)));
    client.setThresholdsAsync(subject, 60, 170, 60, -1);
    QTRY_COMPARE_WITH_TIMEOUT(answered.count(), 1, 5000);
    QVERIFY(answered.first().at(0).toBool());
    QCOMPARE(answered.first().at(1).toString(), subject);
    QCOMPARE(json(answered.first().at(2).toString()).value("result").toString(), QStringLiteral("accepted"));
    QCOMPARE(thresholdsOf(runtime).value("l").toInt(), 60);
    stopAndWait(runtime);
}

// The restart in every state ("transitions across restarts"): an active
// alert stays active and is announced again, an acknowledged one stays acknowledged, a snoozed one
// stays snoozed, and a threshold the caregiver changed is still in force. The notification of a
// slot that is announced again REPLACES the one up (tested with the presenter in
// tst_alertnotification); here the service as a whole is stopped and started on the same files.
void TstDaemon::alertsAndThresholdsSurviveARestartInEveryState()
{
    if (!haveBus())
        QSKIP("no session bus");
    struct Case { const char *name; double value; const char *action; const char *state; const char *level; };
    const Case cases[] = {
        { "active", 50, "", "active", "LL" },
        { "acknowledged", 50, "ack", "acknowledged", "LL" },
        { "snoozed", 65, "snooze", "snoozed", "L" },
    };
    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); ++i) {
        const Case &c = cases[i];
        RuntimeRig rig(QStringLiteral("-restart-") + QString::fromLatin1(c.name));
        rig.secrets.setCredentials(QStringLiteral("c1"), QStringLiteral("u"), QStringLiteral("p"));
        double value = c.value;
        giveValue(rig, &value);
        RecordingAlertNotifier first;
        rig.platform.alerts = &first;
        const QString subject = QString::fromLatin1(kSubjectKey);
        QString episode, since;
        {
            ServiceRuntime runtime(rig.options, rig.platform);
            QCOMPARE(runtime.begin(), -1);
            QVERIFY2(rig.pollUntil([&]() { return !slotOf(runtime, "low").isEmpty(); }, 120), c.name);
            if (QByteArray(c.action) == "ack")
                QCOMPARE(runtime.apiAcknowledgeAlert(subject, QStringLiteral("low")), QStringLiteral("done"));
            if (QByteArray(c.action) == "snooze")
                QCOMPARE(runtime.apiSnoozeAlert(subject, QStringLiteral("low"), 120), QStringLiteral("done"));
            QCOMPARE(json(runtime.apiSetThresholds(subject, 60, 175, 60, -1)).value("result").toString(), QStringLiteral("accepted"));
            const QJsonObject slot = slotOf(runtime, "low");
            QCOMPARE(slot.value("state").toString(), QString::fromLatin1(c.state));
            episode = slot.value("episode").toString();
            since = slot.value("since").toString();
            stopAndWait(runtime);
            QVERIFY(runtime.stopped());
        }
        RecordingAlertNotifier second;
        rig.platform.alerts = &second;
        ServiceRuntime again(rig.options, rig.platform);
        QCOMPARE(again.begin(), -1);
        const QJsonObject slot = slotOf(again, "low");
        QVERIFY2(!slot.isEmpty(), c.name);
        QCOMPARE(slot.value("state").toString(), QString::fromLatin1(c.state));
        QCOMPARE(slot.value("level").toString(), QString::fromLatin1(c.level));
        QCOMPARE(slot.value("episode").toString(), episode);            // the same occurrence, not a new one
        QCOMPARE(slot.value("since").toString(), since);
        QCOMPARE(thresholdsOf(again).value("l").toInt(), 60);
        QCOMPARE(thresholdsOf(again).value("h").toInt(), 175);
        // What the user is shown again: an active alert is announced, an acknowledged one is put
        // back quietly, a snoozed one stays down.
        if (QByteArray(c.state) == "active")
            QVERIFY2(second.count(AlertEffect::Notify) >= 1, c.name);
        if (QByteArray(c.state) == "acknowledged")
            QVERIFY2(second.count(AlertEffect::Update) >= 1 && second.count(AlertEffect::Notify) == 0, c.name);
        if (QByteArray(c.state) == "snoozed")
            QVERIFY2(second.count(AlertEffect::Notify) == 0, c.name);
        stopAndWait(again);
    }
}

void TstDaemon::aStoppedServiceAnswersTheAlertMethodsInWords()
{
    if (!haveBus())
        QSKIP("no session bus");
    RuntimeRig rig(QStringLiteral("-alertsstop"));
    ServiceRuntime runtime(rig.options, rig.platform);
    // Not begun: nothing is running, and the answers say so instead of inventing an empty picture.
    QCOMPARE(json(runtime.apiAlerts()).value("error").toString(), QStringLiteral("service-not-running"));
    QCOMPARE(runtime.apiAcknowledgeAlert(QString::fromLatin1(kSubjectKey), QStringLiteral("low")), QStringLiteral("service-not-running"));
    QCOMPARE(runtime.apiSnoozeAlert(QString::fromLatin1(kSubjectKey), QStringLiteral("low"), 30), QStringLiteral("service-not-running"));
    QCOMPARE(json(runtime.apiSetThresholds(QString::fromLatin1(kSubjectKey), 60, 180, 60, -1)).value("result").toString(),
             QStringLiteral("service-not-running"));
}

// The real presenter on a fake backend inside the whole service: a reading below 54 raises the
// alarm, Alerts() gives a page everything in one call, and an acknowledgement from the application
// closes the notification so the tone stops.
void TstDaemon::alertsHoldWhatAPageNeedsAndAcknowledgingFromTheAppClosesTheEntry()
{
    if (!haveBus())
        QSKIP("no session bus");
    RuntimeRig rig(QStringLiteral("-pageneeds"));
    rig.secrets.setCredentials(QStringLiteral("c1"), QStringLiteral("u"), QStringLiteral("p"));
    double value = 50;
    giveValue(rig, &value);
    QTemporaryDir icons;
    FakeNoteBackend backend;
    MemoryNoteSettings noteSettings;
    SubjectLabelBook labels;
    AlertNotificationPresenter presenter(&backend, &noteSettings, &labels, &rig.clock);
    rig.platform.alerts = &presenter;
    rig.platform.presenter = &presenter;
    rig.platform.labels = &labels;
    ServiceRuntime runtime(rig.options, rig.platform);
    QCOMPARE(runtime.begin(), -1);
    QVERIFY2(rig.pollUntil([&]() { return !slotOf(runtime, "low").isEmpty(); }), "no urgent low was raised");

    const QJsonObject root = alertsOf(runtime);
    const QJsonObject low = slotOf(runtime, "low");
    QCOMPARE(low.value("level").toString(), QStringLiteral("LL"));
    QCOMPARE(low.value("acknowledged").toBool(true), false);
    QCOMPARE(low.value("limit").toInt(), 54);
    QVERIFY(!low.value("level_since").toString().isEmpty());
    QVERIFY(!low.value("since").toString().isEmpty());
    QVERIFY(low.contains("snooze_until") && low.contains("cause") && low.contains("episode"));
    QCOMPARE(root.value("do_not_disturb").toBool(true), false);
    const QJsonObject th = root.value("thresholds").toArray().first().toObject();
    QCOMPARE(th.value("silent").toBool(true), false);
    QVERIFY(th.contains("silent_until"));
    QVERIFY(!runtime.apiAlerts().contains(QStringLiteral("\"value\"")));
    // the question 1: ONE answer holds every field its pages read, per alert,
    // per patient and for the ranges of the alert times.
    for (const char *k : { "subject", "slot", "level", "state", "acknowledged", "episode", "since", "level_since",
                           "episode_since", "limit", "newest_reading_at", "snooze_until", "cause" })
        QVERIFY2(low.contains(QString::fromLatin1(k)), k);
    for (const char *k : { "subject", "ll", "l", "h", "hh", "persistence_low_s", "persistence_high_s",
                           "missing_limit_s", "silent", "silent_until" })
        QVERIFY2(th.contains(QString::fromLatin1(k)), k);
    const QJsonObject limits = root.value("limits").toObject();
    for (const char *k : { "persistence_low_s", "persistence_high_s", "missing_limit_s" }) {
        const QJsonObject range = limits.value(QString::fromLatin1(k)).toObject();
        QVERIFY2(range.contains("min") && range.contains("max") && range.contains("default"), k);
    }

    // The notification is up with the alarm tone; the app acknowledges; the entry is closed.
    QCOMPARE(backend.live.size(), 1);
    QCOMPARE(backend.posts.last().request.hints.feedback, QStringLiteral("clock"));
    const quint32 id = backend.posts.last().id;
    QCOMPARE(runtime.apiAcknowledgeAlert(QString::fromLatin1(kSubjectKey), QStringLiteral("low")), QStringLiteral("done"));
    QVERIFY(backend.closes.contains(id));
    QVERIFY(backend.live.isEmpty());
    QCOMPARE(slotOf(runtime, "low").value("acknowledged").toBool(false), true);
    // The end of a silent mode is in the same answer.
    QCOMPARE(json(runtime.apiSetSilent(QString::fromLatin1(kSubjectKey), 60)).value("result").toString(), QStringLiteral("done"));
    const QJsonObject silentTh = alertsOf(runtime).value("thresholds").toArray().first().toObject();
    QCOMPARE(silentTh.value("silent").toBool(false), true);
    QVERIFY(!silentTh.value("silent_until").toString().isEmpty());

    // Do not disturb is part of the same picture.
    runtime.core()->setDoNotDisturb(true);
    QCOMPARE(alertsOf(runtime).value("do_not_disturb").toBool(false), true);
    const QJsonObject status = QJsonDocument::fromJson(runtime.apiStatusJson().toUtf8()).object();
    QCOMPARE(status.value("service").toObject().value("do_not_disturb").toBool(false), true);
    stopAndWait(runtime);
}

// The user deletes the notification: the service acknowledges the slot in the engine, and nothing is
// posted again for it.
void TstDaemon::deletingTheNotificationAcknowledgesTheAlertInTheEngine()
{
    if (!haveBus())
        QSKIP("no session bus");
    RuntimeRig rig(QStringLiteral("-deleteack"));
    rig.secrets.setCredentials(QStringLiteral("c1"), QStringLiteral("u"), QStringLiteral("p"));
    double value = 50;
    giveValue(rig, &value);
    FakeNoteBackend backend;
    MemoryNoteSettings noteSettings;
    SubjectLabelBook labels;
    AlertNotificationPresenter presenter(&backend, &noteSettings, &labels, &rig.clock);
    rig.platform.alerts = &presenter;
    rig.platform.presenter = &presenter;
    rig.platform.labels = &labels;
    ServiceRuntime runtime(rig.options, rig.platform);
    QCOMPARE(runtime.begin(), -1);
    QVERIFY2(rig.pollUntil([&]() { return !slotOf(runtime, "low").isEmpty(); }), "no urgent low was raised");
    const quint32 id = backend.posts.last().id;
    const int posts = backend.posts.size();
    backend.userDeletes(id);
    QCOMPARE(slotOf(runtime, "low").value("state").toString(), QStringLiteral("acknowledged"));
    QCOMPARE(backend.posts.size(), posts);                       // not resurrected
    stopAndWait(runtime);
}

// The restart in every state, with the notifications: the engine's
// restore notifies again; the notifier adopts the entries the last run left (same id, replaced in
// place, not duplicated) and closes the ones that belong to nothing any more.
void TstDaemon::notificationsAfterARestartAdoptTheEntriesInEveryState()
{
    if (!haveBus())
        QSKIP("no session bus");
    struct Case { const char *name; double value; const char *action; bool entryAfter; };
    const Case cases[] = {
        { "active", 50, "", true },
        { "acknowledged", 50, "ack", false },
        { "snoozed", 65, "snooze", false },
    };
    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); ++i) {
        const Case &c = cases[i];
        RuntimeRig rig(QStringLiteral("-adopt-") + QString::fromLatin1(c.name));
        rig.secrets.setCredentials(QStringLiteral("c1"), QStringLiteral("u"), QStringLiteral("p"));
        double value = c.value;
        giveValue(rig, &value);
        FakeNoteBackend backend;
        MemoryNoteSettings noteSettings;
        SubjectLabelBook labels;
        quint32 firstId = 0;
        {
            AlertNotificationPresenter presenter(&backend, &noteSettings, &labels, &rig.clock);
            rig.platform.alerts = &presenter;
            rig.platform.presenter = &presenter;
            rig.platform.labels = &labels;
            ServiceRuntime runtime(rig.options, rig.platform);
            QCOMPARE(runtime.begin(), -1);
            QVERIFY2(rig.pollUntil([&]() { return !slotOf(runtime, "low").isEmpty(); }, 120), c.name);
            firstId = backend.posts.last().id;
            if (QByteArray(c.action) == "ack")
                QCOMPARE(runtime.apiAcknowledgeAlert(QString::fromLatin1(kSubjectKey), QStringLiteral("low")), QStringLiteral("done"));
            if (QByteArray(c.action) == "snooze")
                QCOMPARE(runtime.apiSnoozeAlert(QString::fromLatin1(kSubjectKey), QStringLiteral("low"), 120), QStringLiteral("done"));
            stopAndWait(runtime);
        }
        // A new run on the same files with a new presenter, the ids in the same settings.
        AlertNotificationPresenter again(&backend, &noteSettings, &labels, &rig.clock);
        rig.platform.alerts = &again;
        rig.platform.presenter = &again;
        ServiceRuntime runtime(rig.options, rig.platform);
        QCOMPARE(runtime.begin(), -1);
        QCOMPARE(backend.live.size(), c.entryAfter ? 1 : 0);
        if (c.entryAfter) {
            QVERIFY2(backend.live.contains(firstId), c.name);          // the same entry, replaced in place
            QCOMPARE(backend.postsOf(firstId), 2);
        } else {
            QVERIFY2(!backend.live.contains(firstId), c.name);
        }
        stopAndWait(runtime);
    }
}

// The approval notice and the resident notice follow the lifecycle: waiting for approval posts the
// notice (and says so in the resident notice); the approval withdraws it.
void TstDaemon::theApprovalNoticeAndTheResidentNoticeFollowTheLifecycle()
{
    if (!haveBus())
        QSKIP("no session bus");
    QTemporaryDir translations;
    NotificationTranslator english;
    QVERIFY(loadNoteLanguage(&translations, &english, "en"));
    RuntimeRig rig(QStringLiteral("-approvalnotice"));
    rig.secrets.setCredentials(QStringLiteral("c1"), QStringLiteral("u"), QStringLiteral("p"));
    rig.secrets.lock();
    FakeNoteBackend backend;
    MemoryNoteSettings noteSettings;
    SubjectLabelBook labels;
    AlertNotificationPresenter presenter(&backend, &noteSettings, &labels, &rig.clock);
    ResidentNoticeComposer composer(&presenter);
    rig.platform.alerts = &presenter;
    rig.platform.presenter = &presenter;
    rig.platform.labels = &labels;
    rig.platform.resident = &composer;
    ServiceRuntime runtime(rig.options, rig.platform);
    QCOMPARE(runtime.begin(), -1);
    for (int i = 0; i < 4; ++i) {
        rig.clock.advanceS(30);
        rig.wake.fire();
        QTest::qWait(50);
    }
    QCOMPARE(runtime.lifecycle()->state(), ServiceState::RunningAwaitingApproval);
    QCOMPARE(backend.live.size(), 1);
    QVERIFY(backend.posts.last().request.body.contains(QStringLiteral("is waiting to be monitored")));
    QCOMPARE(backend.posts.last().request.hints.feedback, QStringLiteral("general_warning"));
    // The resident notice composed for the state says waiting for approval and names the Subject by its position.
    const NoteText resident = composer.compose(rig.notifier.last());
    QVERIFY(!resident.summary.isEmpty());
    QVERIFY(resident.body.contains(QStringLiteral("P1")));
    // The user approves: the notice goes.
    rig.approval.answer(0, true);
    QTRY_VERIFY_WITH_TIMEOUT(backend.live.isEmpty(), 3000);
    QCOMPARE(runtime.lifecycle()->state(), ServiceState::Running);
    stopAndWait(runtime);
}

#ifdef LIBREMON_DEVSOURCE
// The developer-only feed through the whole service, over the bus: a synthetic
// urgent low is posted by the real notifier, the resident notice lists it, the real Alerts() does not,
// and a real Subject is refused.
void TstDaemon::theDevelopmentFeedGoesThroughTheWholeServiceAndTheResidentNoticeListsIt()
{
    if (!haveBus())
        QSKIP("no session bus");
    QTemporaryDir translations;
    NotificationTranslator english;
    QVERIFY(loadNoteLanguage(&translations, &english, "en"));
    RuntimeRig rig(QStringLiteral("-devfeed"));
    FakeNoteBackend backend;
    MemoryNoteSettings noteSettings;
    SubjectLabelBook labels;
    AlertNotificationPresenter presenter(&backend, &noteSettings, &labels, &rig.clock);
    ResidentNoticeComposer composer(&presenter);
    rig.platform.alerts = &presenter;
    rig.platform.presenter = &presenter;
    rig.platform.labels = &labels;
    rig.platform.resident = &composer;
    ServiceRuntime runtime(rig.options, rig.platform);
    QCOMPARE(runtime.begin(), -1);

    QVERIFY(json(runtime.apiDevInject(QStringLiteral("{\"scenario\":\"urgent-low\"}"))).value("ok").toBool());
    QCOMPARE(backend.live.size(), 1);
    QCOMPARE(backend.posts.last().request.summary, QStringLiteral("VERY LOW GLUCOSE"));
    QVERIFY(backend.posts.last().request.body.startsWith(QStringLiteral("AAA")));
    QCOMPARE(backend.posts.last().request.hints.feedback, QStringLiteral("clock"));
    // The resident notice (as the lifecycle last showed it) lists the synthetic alert by label and level word.
    const NoteText resident = composer.compose(rig.notifier.last());
    QVERIFY2(resident.body.contains(QStringLiteral("AAA very low")), qPrintable(resident.body));
    // The engine knows nothing of it.
    QVERIFY(alertsOf(runtime).value("alerts").toArray().isEmpty());
    // A real Subject is refused.
    const QString real = QString::fromLatin1(kSubjectKey);
    const QString refusal = runtime.apiDevInject(QStringLiteral("{\"ops\":[{\"op\":\"alert\",\"kind\":\"notify\",\"subject\":\"%1\",\"slot\":\"low\",\"type\":\"LL\"}]}").arg(real));
    QCOMPARE(json(refusal).value("error").toString(), QStringLiteral("not-a-synthetic-subject"));
    QCOMPARE(json(runtime.apiDevInject(QStringLiteral("{\"scenario\":\"clear\"}"))).value("ok").toBool(), true);
    QVERIFY(backend.live.isEmpty());
    stopAndWait(runtime);
}
#endif

// Silent mode: set, ended and run out, with the signal each time and the state in Status().
void TstDaemon::silentModeIsSetEndedAndAnnouncedOverTheBus()
{
    if (!haveBus())
        QSKIP("no session bus");
    RuntimeRig rig(QStringLiteral("-silentmode"));
    FakeSilentControl silent([&rig]() { return rig.clock.now().msecsSinceEpoch(); });
    rig.options.core.silent = &silent;
    ServiceRuntime runtime(rig.options, rig.platform);
    QCOMPARE(runtime.begin(), -1);
    ServiceClient client(rig.options.identity.busName, rig.options.identity.objectPath, clientBus());
    QSignalSpy changed(&client, SIGNAL(silentChanged(QString,bool,QString)));
    const QString subject = QString::fromLatin1(kSubjectKey);
    QCOMPARE(firstSubject(runtime).value("silent").toBool(true), false);
    QCOMPARE(firstSubject(runtime).value("silent_until").toString(QStringLiteral("x")), QString());
    rig.wake.fire();                                        // the first look only records the state

    QJsonObject r = json(runtime.apiSetSilent(subject, 60));
    QCOMPARE(r.value("result").toString(), QStringLiteral("done"));
    const qint64 hour = rig.clock.now().msecsSinceEpoch() + 3600 * 1000;
    QCOMPARE(UtcTime::fromIso(r.value("silent_until").toString()).msecsSinceEpoch(), hour);
    QCOMPARE(firstSubject(runtime).value("silent").toBool(), true);
    QCOMPARE(firstSubject(runtime).value("silent_until").toString(), r.value("silent_until").toString());
    QTRY_COMPARE_WITH_TIMEOUT(changed.count(), 1, 3000);
    QCOMPARE(changed.first().at(0).toString(), subject);
    QVERIFY(changed.first().at(1).toBool());
    QCOMPARE(changed.first().at(2).toString(), r.value("silent_until").toString());

    // Refusals are words.
    QCOMPARE(json(runtime.apiSetSilent(subject, -5)).value("result").toString(), QStringLiteral("bad-duration"));
    QCOMPARE(json(runtime.apiSetSilent(subject, 999999999)).value("result").toString(), QStringLiteral("bad-duration"));
    QCOMPARE(json(runtime.apiSetSilent(QStringLiteral("{00000000-0000-4000-8000-000000000003}"), 60)).value("result").toString(), QStringLiteral("unknown-subject"));
    QCOMPARE(runtime.apiEndSilent(QStringLiteral("{00000000-0000-4000-8000-000000000003}")), QStringLiteral("unknown-subject"));

    // Without limit: no end time. Until tomorrow: an end time.
    r = json(runtime.apiSetSilent(subject, 0));
    QCOMPARE(r.value("result").toString(), QStringLiteral("done"));
    QCOMPARE(r.value("silent_until").toString(), QString());
    r = json(runtime.apiSetSilent(subject, kSilentUntilTomorrow));
    QVERIFY(!r.value("silent_until").toString().isEmpty());

    // Ended by the user: the signal says off.
    QCOMPARE(runtime.apiEndSilent(subject), QStringLiteral("done"));
    QCOMPARE(runtime.apiEndSilent(subject), QStringLiteral("not-silent"));
    QCOMPARE(firstSubject(runtime).value("silent").toBool(true), false);
    QTRY_VERIFY_WITH_TIMEOUT(!changed.last().at(1).toBool(), 3000);

    // A timed mode runs out by itself: the next wake-up announces it.
    runtime.apiSetSilent(subject, 1);
    const int before = changed.count();
    QTRY_VERIFY_WITH_TIMEOUT(changed.count() > before, 3000);
    rig.clock.advanceS(120);
    rig.wake.fire();
    QTRY_VERIFY_WITH_TIMEOUT(!changed.last().at(1).toBool(), 3000);
    QCOMPARE(firstSubject(runtime).value("silent").toBool(true), false);

    // Over the bus with the asynchronous client.
    QSignalSpy answered(&client, SIGNAL(setSilentAnswered(bool,QString,QString)));
    QSignalSpy ended(&client, SIGNAL(endSilentAnswered(bool,QString,QString)));
    client.setSilentAsync(subject, 240);
    QTRY_COMPARE_WITH_TIMEOUT(answered.count(), 1, 5000);
    QVERIFY(answered.first().at(0).toBool());
    QCOMPARE(json(answered.first().at(2).toString()).value("result").toString(), QStringLiteral("done"));
    client.endSilentAsync(subject);
    QTRY_COMPARE_WITH_TIMEOUT(ended.count(), 1, 5000);
    QCOMPARE(ended.first().at(2).toString(), QStringLiteral("done"));
    stopAndWait(runtime);
}

// Silent mode real engine: SetSilent stops the tone of a sounding alarm by the one
// Update the engine emits, the reminders that follow carry no feedback, the state is in Alerts() and
// Status(), it ends on request, and a mode that ran out ends by the engine's own wake-up.
void TstDaemon::silentModeWithTheRealEngineStopsTheToneAndIsListed()
{
    if (!haveBus())
        QSKIP("no session bus");
    QTemporaryDir translations;
    NotificationTranslator english;
    QVERIFY(loadNoteLanguage(&translations, &english, "en"));
    RuntimeRig rig(QStringLiteral("-realsilent"));
    rig.secrets.setCredentials(QStringLiteral("c1"), QStringLiteral("u"), QStringLiteral("p"));
    double value = 50;
    giveValue(rig, &value);
    FakeNoteBackend backend;
    MemoryNoteSettings noteSettings;
    SubjectLabelBook labels;
    AlertNotificationPresenter presenter(&backend, &noteSettings, &labels, &rig.clock);
    rig.platform.alerts = &presenter;
    rig.platform.presenter = &presenter;
    rig.platform.labels = &labels;
    ServiceRuntime runtime(rig.options, rig.platform);
    QCOMPARE(runtime.begin(), -1);
    const QString subject = QString::fromLatin1(kSubjectKey);
    QVERIFY2(rig.pollUntil([&]() { return !slotOf(runtime, "low").isEmpty(); }), "no urgent low was raised");
    QCOMPARE(backend.posts.last().request.hints.feedback, QStringLiteral("clock"));
    const int posts = backend.posts.size();

    QJsonObject r = json(runtime.apiSetSilent(subject, 60));
    QCOMPARE(r.value("result").toString(), QStringLiteral("done"));
    QVERIFY(!r.value("silent_until").toString().isEmpty());
    QCOMPARE(backend.posts.size(), posts + 1);                      // the one Update that stops the tone
    QVERIFY(backend.posts.last().request.hints.feedback.isEmpty());
    QCOMPARE(firstSubject(runtime).value("silent").toBool(false), true);
    QCOMPARE(alertsOf(runtime).value("thresholds").toArray().first().toObject().value("silent").toBool(false), true);
    // The alarm is still raised and shown; the next reminder asks for no sound.
    QCOMPARE(slotOf(runtime, "low").value("state").toString(), QStringLiteral("active"));
    rig.clock.advanceS(150);
    for (int i = 0; i < 10; ++i) { rig.clock.advanceS(30); rig.wake.fire(); QTest::qWait(20); }
    for (int i = posts + 1; i < backend.posts.size(); ++i)
        QVERIFY2(backend.posts.at(i).request.hints.feedback.isEmpty(), "a reminder sounded in silent mode");
    // Ended on request: the answer, the state, and the next reminder sounds again.
    QCOMPARE(runtime.apiEndSilent(subject), QStringLiteral("done"));
    QCOMPARE(runtime.apiEndSilent(subject), QStringLiteral("not-silent"));
    QCOMPARE(firstSubject(runtime).value("silent").toBool(true), false);
    QCOMPARE(json(runtime.apiSetSilent(QStringLiteral("{00000000-0000-4000-8000-000000000004}"), 60)).value("result").toString(), QStringLiteral("unknown-subject"));
    QCOMPARE(json(runtime.apiSetSilent(subject, 99999999)).value("result").toString(), QStringLiteral("bad-duration"));
    stopAndWait(runtime);
}

void TstDaemon::discoverOverTheBusAnswersLaterWithTheSubjects()
{
    if (!haveBus())
        QSKIP("no session bus");
    RuntimeRig rig(QStringLiteral("-disc"), false);
    rig.script->discover = []() {
        AdapterAnswer a;
        QJsonArray subjects;
        subjects.append(scriptedSubject(QStringLiteral("ref-a"), QStringLiteral("Test Person"), QStringLiteral("mmol/L")));
        a.payload.insert(QStringLiteral("subjects"), subjects);
        return a;
    };
    ServiceRuntime runtime(rig.options, rig.platform);
    QCOMPARE(runtime.begin(), -1);
    // What the set-up page does: store the login under `probe`, call Discover, find it gone.
    rig.secrets.setCredentials(QStringLiteral("probe"), QStringLiteral("probe-user"), QStringLiteral("probe-password"));
    ServiceClient client(rig.options.identity.busName, rig.options.identity.objectPath, clientBus());
    QSignalSpy answers(&client, SIGNAL(discoverAnswered(QString,QString,QString)));
    client.discoverAsync(QStringLiteral("probe"));
    QCOMPARE(answers.count(), 0);                               // returned at once
    QTRY_COMPARE_WITH_TIMEOUT(answers.count(), 1, 8000);
    QCOMPARE(answers.first().at(0).toString(), QStringLiteral("probe"));
    QCOMPARE(answers.first().at(2).toString(), QString());      // the service answered
    const QJsonObject a = QJsonDocument::fromJson(answers.first().at(1).toString().toUtf8()).object();
    QVERIFY(a.value("ok").toBool());
    QCOMPARE(a.value("subjects").toArray().first().toObject().value("display_name").toString(), QStringLiteral("Test Person"));
    QCOMPARE(a.value("subjects").toArray().first().toObject().value("unit").toString(), QStringLiteral("mmol/L"));
    QVERIFY(!rig.secrets.hasSecret(QStringLiteral("probe")));
    QVERIFY(!answers.first().at(1).toString().contains(QStringLiteral("probe-password")));

    // The service logs nothing of it and Status() still names no one.
    QVERIFY(!runtime.apiStatusJson().contains(QStringLiteral("Test Person")));
    stopAndWait(runtime);
}

void TstDaemon::discoverOverTheBusWithAWrongLoginAndWithNoSecret()
{
    if (!haveBus())
        QSKIP("no session bus");
    RuntimeRig rig(QStringLiteral("-disc2"), false);
    rig.script->authenticate = [](const QString &, const QString &, const QString &) {
        return AdapterAnswer::error(QStringLiteral("auth-failed"));
    };
    ServiceRuntime runtime(rig.options, rig.platform);
    QCOMPARE(runtime.begin(), -1);
    ServiceClient client(rig.options.identity.busName, rig.options.identity.objectPath, clientBus());
    QSignalSpy answers(&client, SIGNAL(discoverAnswered(QString,QString,QString)));

    rig.secrets.setCredentials(QStringLiteral("probe"), QStringLiteral("u"), QStringLiteral("wrong"));
    client.discoverAsync(QStringLiteral("probe"));
    QTRY_COMPARE_WITH_TIMEOUT(answers.count(), 1, 8000);
    QCOMPARE(QJsonDocument::fromJson(answers.at(0).at(1).toString().toUtf8()).object().value("error").toString(),
             QStringLiteral("auth-failed"));
    QVERIFY(!rig.secrets.hasSecret(QStringLiteral("probe")));

    // No secret at all (the page forgot to store it) and a name that is no connection.
    rig.clock.advanceS(40);
    client.discoverAsync(QStringLiteral("probe"));
    QTRY_COMPARE_WITH_TIMEOUT(answers.count(), 2, 8000);
    QCOMPARE(QJsonDocument::fromJson(answers.at(1).at(1).toString().toUtf8()).object().value("error").toString(),
             QStringLiteral("credentials-missing"));
    client.discoverAsync(QStringLiteral("nobody"));
    QTRY_COMPARE_WITH_TIMEOUT(answers.count(), 3, 8000);
    QCOMPARE(QJsonDocument::fromJson(answers.at(2).at(1).toString().toUtf8()).object().value("error").toString(),
             QStringLiteral("unknown-connection"));
    QCOMPARE(rig.script->authenticates, 1);                     // only the first reached the adapter
    stopAndWait(runtime);
}

void TstDaemon::aStaleProbeSecretIsRemovedWhenTheServiceStarts()
{
    if (!haveBus())
        QSKIP("no session bus");
    RuntimeRig rig(QStringLiteral("-stale"));
    rig.secrets.setCredentials(QStringLiteral("probe"), QStringLiteral("u"), QStringLiteral("p"));      // a UI died after storing it
    rig.secrets.setCredentials(QStringLiteral("c1"), QStringLiteral("u"), QStringLiteral("p"));
    ServiceRuntime runtime(rig.options, rig.platform);
    QVERIFY(rig.secrets.hasSecret(QStringLiteral("probe")));
    QCOMPARE(runtime.begin(), -1);
    QVERIFY(!rig.secrets.hasSecret(QStringLiteral("probe")));
    QVERIFY(rig.secrets.hasSecret(QStringLiteral("c1")));       // a real connection is never touched
    QCOMPARE(rig.script->authenticates, 0);                     // and the stale login was never used
    stopAndWait(runtime);
}

void TstDaemon::discoverNeedsARunningServiceAndAStopCutsItShort()
{
    if (!haveBus())
        QSKIP("no session bus");
    RuntimeRig rig(QStringLiteral("-discstop"), false);
    rig.script->discover = []() { return AdapterAnswer::never(); };
    ServiceRuntime runtime(rig.options, rig.platform);

    QStringList answers;
    runtime.apiDiscover(QStringLiteral("probe"), [&answers](const QString &j) { answers << j; });
    QTRY_COMPARE_WITH_TIMEOUT(answers.size(), 1, 3000);
    QCOMPARE(QJsonDocument::fromJson(answers.first().toUtf8()).object().value("error").toString(),
             QStringLiteral("service-not-running"));

    QCOMPARE(runtime.begin(), -1);
    rig.secrets.setCredentials(QStringLiteral("probe"), QStringLiteral("u"), QStringLiteral("p"));
    runtime.apiDiscover(QStringLiteral("probe"), [&answers](const QString &j) { answers << j; });
    QTRY_COMPARE_WITH_TIMEOUT(rig.script->discovers, 1, 3000);
    QCOMPARE(answers.size(), 1);                                // still waiting
    stopAndWait(runtime);
    QCOMPARE(answers.size(), 2);                                // the stop answered it
    QCOMPARE(QJsonDocument::fromJson(answers.last().toUtf8()).object().value("error").toString(),
             QStringLiteral("service-stopping"));
    QCOMPARE(rig.script->ended, 1);                             // and ended its process

    runtime.apiDiscover(QStringLiteral("probe"), [&answers](const QString &j) { answers << j; });
    QTRY_COMPARE_WITH_TIMEOUT(answers.size(), 3, 3000);
}

// ---- the asynchronous client ----------------------------------------------------------

namespace {

// A service on its own bus name that answers every call with a recognisable value, and answers
// Discover LATER, so the tests can see that the client does not wait.
class FakeApi : public ServiceApi
{
public:
    FakeApi() : discoverDelayMs(150) {}
    QString apiStart() { calls << "Start"; return QStringLiteral("running"); }
    QString apiStatusJson() { calls << "Status"; return QStringLiteral("{\"format\":1}"); }
    void apiRetry(const QString &c) { calls << "Retry:" + c; }
    bool apiReenable(const QString &c) { calls << "Reenable:" + c; return c == QLatin1String("c1"); }
    bool apiReenableVersion(const QString &a, const QString &v) { calls << "ReenableVersion:" + a + ":" + v; return true; }
    bool apiReportHandled(const QString &c) { calls << "ReportHandled:" + c; return c == QLatin1String("c1"); }
    QString apiDiagnosticReport(const QString &c) { calls << "DiagnosticReport:" + c; return QStringLiteral("{\"line_type\":\"report\"}\n"); }
    void apiCredentialsChanged(const QString &c) { calls << "CredentialsChanged:" + c; }
    void apiIntervalsChanged(const QString &c) { calls << "IntervalsChanged:" + c; }
    QString apiConfigChanged() { calls << "ConfigChanged"; return QStringLiteral("restart-needed"); }
    QString apiAlerts() { calls << "Alerts"; return QStringLiteral("{\"format\":1}"); }
    QString apiAcknowledgeAlert(const QString &s, const QString &slot) { calls << "Ack:" + s + ":" + slot; return QStringLiteral("done"); }
    QString apiSnoozeAlert(const QString &s, const QString &slot, int m) { calls << "Snooze:" + s + ":" + slot + ":" + QString::number(m); return QStringLiteral("done"); }
#ifdef LIBREMON_DEVSOURCE
    QString apiDevInject(const QString &j) { calls << "DevInject:" + j; return QStringLiteral("{\"ok\":true}"); }
#endif
    QString apiSetSilent(const QString &s, int m) { calls << "SetSilent:" + s + ":" + QString::number(m); return QStringLiteral("{\"result\":\"done\"}"); }
    QString apiEndSilent(const QString &s) { calls << "EndSilent:" + s; return QStringLiteral("done"); }
    QString apiSetThresholds(const QString &s, int l, int h, int tl, int th)
    {
        calls << QStringLiteral("Thresholds:%1:%2:%3:%4:%5").arg(s).arg(l).arg(h).arg(tl).arg(th);
        return QStringLiteral("{\"result\":\"accepted\"}");
    }
    void apiDiscover(const QString &c, const std::function<void(const QString &)> &done)
    {
        calls << "Discover:" + c;
        const QString json = QStringLiteral("{\"ok\":true,\"subjects\":[]}");
        QTimer::singleShot(discoverDelayMs, [done, json]() { done(json); });
    }
    QString apiStartMaintenance(const QString &k, const QString &p) { calls << "StartMaintenance:" + k + ":" + p; return QStringLiteral("unknown-kind"); }
    void apiCancelMaintenance() { calls << "CancelMaintenance"; }
    void apiStop() { calls << "Stop"; }

    QStringList calls;
    int discoverDelayMs;
};

// An older service: the same interface name, without Discover.
class OldService : public QObject
{
    Q_OBJECT
    Q_CLASSINFO("D-Bus Interface", "it.michelebrami.libre_monitor.Service")
public slots:
    QString Status() { return QStringLiteral("{\"format\":1}"); }
};

} // namespace

void TstDaemon::asyncClientReturnsAtOnceAndAnswersBySignal()
{
    if (!haveBus())
        QSKIP("no session bus");
    const ServiceIdentity id = ServiceIdentity::fromNames(QStringLiteral("it.michelebrami"),
                                                          QStringLiteral("libre-monitor-test-fake"));
    FakeApi api;
    ServiceObject holder;
    ServiceAdaptor adaptor(&holder, &api);
    QDBusConnection bus = QDBusConnection::sessionBus();
    QVERIFY(bus.registerService(id.busName));
    QVERIFY(bus.registerObject(id.objectPath, &holder, QDBusConnection::ExportAdaptors));

    ServiceClient client(id.busName, id.objectPath, clientBus());
    QSignalSpy started(&client, SIGNAL(startAnswered(bool,QString)));
    QSignalSpy status(&client, SIGNAL(statusAnswered(bool,QString)));
    QSignalSpy retried(&client, SIGNAL(retryAnswered(bool,QString)));
    QSignalSpy reenabled(&client, SIGNAL(reenableAnswered(bool,QString,bool)));
    QSignalSpy reenabledV(&client, SIGNAL(reenableVersionAnswered(bool,QString,QString,bool)));
    QSignalSpy handled(&client, SIGNAL(reportHandledAnswered(bool,QString,bool)));
    QSignalSpy credentials(&client, SIGNAL(credentialsChangedAnswered(bool,QString)));
    QSignalSpy intervals(&client, SIGNAL(intervalsChangedAnswered(bool,QString)));
    QSignalSpy config(&client, SIGNAL(configChangedAnswered(bool,QString)));
    QSignalSpy maintenance(&client, SIGNAL(startMaintenanceAnswered(bool,QString,QString)));
    QSignalSpy cancelled(&client, SIGNAL(cancelMaintenanceAnswered(bool)));
    QSignalSpy stopped(&client, SIGNAL(stopAnswered(bool)));
    QSignalSpy pinged(&client, SIGNAL(pingAnswered(bool)));
    QSignalSpy discovered(&client, SIGNAL(discoverAnswered(QString,QString,QString)));
    QSignalSpy alertsSpy(&client, SIGNAL(alertsAnswered(bool,QString)));
    QSignalSpy ackSpy(&client, SIGNAL(acknowledgeAlertAnswered(bool,QString,QString,QString)));
    QSignalSpy snoozeSpy(&client, SIGNAL(snoozeAlertAnswered(bool,QString,QString,QString)));
    QSignalSpy thresholdsSpy(&client, SIGNAL(setThresholdsAnswered(bool,QString,QString)));

    // Every call returns before any answer exists (the event loop has not run yet).
    client.startAsync();
    client.statusJsonAsync();
    client.retryAsync(QStringLiteral("c1"));
    client.reenableAsync(QStringLiteral("c1"));
    client.reenableAsync(QStringLiteral("c2"));
    client.reenableVersionAsync(QStringLiteral("libre"), QStringLiteral("0.1.0"));
    client.alertsAsync();
    client.acknowledgeAlertAsync(QStringLiteral("{s}"), QStringLiteral("low"));
    client.snoozeAlertAsync(QStringLiteral("{s}"), QStringLiteral("high"), 60);
    client.setThresholdsAsync(QStringLiteral("{s}"), 60, 175, 60, -1);
    client.reportHandledAsync(QStringLiteral("c1"));
    client.credentialsChangedAsync(QStringLiteral("c1"));
    client.intervalsChangedAsync(QStringLiteral("c1"));
    client.configChangedAsync();
    client.startMaintenanceAsync(QStringLiteral("compaction"), QStringLiteral("{}"));
    client.cancelMaintenanceAsync();
    client.discoverAsync(QStringLiteral("probe"));
    client.pingAsync();
    client.stopAsync();
    QCOMPARE(started.count() + status.count() + discovered.count() + pinged.count(), 0);

    QTRY_COMPARE_WITH_TIMEOUT(stopped.count(), 1, 5000);
    QTRY_COMPARE_WITH_TIMEOUT(discovered.count(), 1, 5000);      // the delayed reply, sent later
    QTRY_COMPARE_WITH_TIMEOUT(pinged.count(), 1, 5000);

    QCOMPARE(started.first().at(0).toBool(), true);
    QCOMPARE(started.first().at(1).toString(), QStringLiteral("running"));
    QCOMPARE(status.first().at(1).toString(), QStringLiteral("{\"format\":1}"));
    QCOMPARE(retried.first().at(1).toString(), QStringLiteral("c1"));
    QCOMPARE(reenabled.size(), 2);
    QVERIFY(reenabled.at(0).at(0).toBool());
    bool seenTrue = false, seenFalse = false;                    // the answers repeat their argument
    for (int i = 0; i < reenabled.size(); ++i) {
        if (reenabled.at(i).at(1).toString() == QLatin1String("c1")) seenTrue = reenabled.at(i).at(2).toBool();
        if (reenabled.at(i).at(1).toString() == QLatin1String("c2")) seenFalse = !reenabled.at(i).at(2).toBool();
    }
    QVERIFY(seenTrue && seenFalse);
    QCOMPARE(reenabledV.first().at(1).toString(), QStringLiteral("libre"));
    QCOMPARE(reenabledV.first().at(2).toString(), QStringLiteral("0.1.0"));
    QVERIFY(reenabledV.first().at(3).toBool());
    QTRY_COMPARE_WITH_TIMEOUT(alertsSpy.count() + ackSpy.count() + snoozeSpy.count() + thresholdsSpy.count(), 4, 5000);
    QCOMPARE(alertsSpy.first().at(1).toString(), QStringLiteral("{\"format\":1}"));
    QCOMPARE(ackSpy.first().at(3).toString(), QStringLiteral("done"));
    QCOMPARE(ackSpy.first().at(2).toString(), QStringLiteral("low"));
    QCOMPARE(snoozeSpy.first().at(2).toString(), QStringLiteral("high"));
    QCOMPARE(thresholdsSpy.first().at(2).toString(), QStringLiteral("{\"result\":\"accepted\"}"));
    QVERIFY(api.calls.contains(QStringLiteral("Snooze:{s}:high:60")));
    QVERIFY(api.calls.contains(QStringLiteral("Thresholds:{s}:60:175:60:-1")));
    QCOMPARE(handled.first().at(1).toString(), QStringLiteral("c1"));
    QVERIFY(handled.first().at(2).toBool());
    QCOMPARE(credentials.first().at(1).toString(), QStringLiteral("c1"));
    QCOMPARE(intervals.first().at(1).toString(), QStringLiteral("c1"));
    QCOMPARE(config.first().at(1).toString(), QStringLiteral("restart-needed"));
    QCOMPARE(maintenance.first().at(1).toString(), QStringLiteral("compaction"));
    QCOMPARE(maintenance.first().at(2).toString(), QStringLiteral("unknown-kind"));
    QVERIFY(cancelled.first().at(0).toBool());
    QVERIFY(stopped.first().at(0).toBool());
    QVERIFY(pinged.first().at(0).toBool());
    QCOMPARE(discovered.first().at(0).toString(), QStringLiteral("probe"));
    QCOMPARE(discovered.first().at(1).toString(), QStringLiteral("{\"ok\":true,\"subjects\":[]}"));
    QCOMPARE(discovered.first().at(2).toString(), QString());
    QVERIFY(api.calls.contains(QStringLiteral("Discover:probe")));

    bus.unregisterObject(id.objectPath);
    bus.unregisterService(id.busName);
}

void TstDaemon::asyncClientReportsAMissingMethodAndAMissingService()
{
    if (!haveBus())
        QSKIP("no session bus");
    const ServiceIdentity old = ServiceIdentity::fromNames(QStringLiteral("it.michelebrami"),
                                                           QStringLiteral("libre-monitor-test-old"));
    OldService service;
    QDBusConnection bus = QDBusConnection::sessionBus();
    QVERIFY(bus.registerService(old.busName));
    QVERIFY(bus.registerObject(old.objectPath, &service, QDBusConnection::ExportAllSlots));

    ServiceClient client(old.busName, old.objectPath, clientBus());
    QSignalSpy discovered(&client, SIGNAL(discoverAnswered(QString,QString,QString)));
    QSignalSpy status(&client, SIGNAL(statusAnswered(bool,QString)));
    QSignalSpy alerts(&client, SIGNAL(alertsAnswered(bool,QString)));
    QSignalSpy failed(&client, SIGNAL(callFailed(QString,QString)));
    client.statusJsonAsync();                                   // the method that exists works
    client.discoverAsync(QStringLiteral("probe"));              // the one that does not
    client.alertsAsync();                                       // the case
    QTRY_COMPARE_WITH_TIMEOUT(discovered.count(), 1, 5000);
    QTRY_COMPARE_WITH_TIMEOUT(status.count(), 1, 5000);
    QTRY_COMPARE_WITH_TIMEOUT(alerts.count(), 1, 5000);
    QVERIFY(status.first().at(0).toBool());
    QCOMPARE(discovered.first().at(1).toString(), QString());
    QCOMPARE(discovered.first().at(2).toString(), QStringLiteral("not-available"));
    // the reason of each failure, by method; the answered call raised none.
    QVERIFY(!alerts.first().at(0).toBool());
    QCOMPARE(failed.count(), 2);
    QStringList methods;
    for (int i = 0; i < failed.count(); ++i) {
        methods << failed.at(i).at(0).toString();
        QCOMPARE(failed.at(i).at(1).toString(), QStringLiteral("org.freedesktop.DBus.Error.UnknownMethod"));
        QVERIFY(ServiceClient::isOlderServiceError(failed.at(i).at(1).toString()));
    }
    methods.sort();
    QCOMPARE(methods, QStringList() << QStringLiteral("Alerts") << QStringLiteral("Discover"));
    bus.unregisterObject(old.objectPath);
    bus.unregisterService(old.busName);

    // No service on the name at all: not ok, and nothing waits for it.
    ServiceClient nobody(QStringLiteral("it.michelebrami.libre-monitor-test-nobody"), QStringLiteral("/nobody"), clientBus());
    QSignalSpy start(&nobody, SIGNAL(startAnswered(bool,QString)));
    QSignalSpy gone(&nobody, SIGNAL(discoverAnswered(QString,QString,QString)));
    QSignalSpy nobodyFailed(&nobody, SIGNAL(callFailed(QString,QString)));
    nobody.startAsync();
    nobody.discoverAsync(QStringLiteral("probe"));
    QTRY_COMPARE_WITH_TIMEOUT(start.count(), 1, 8000);
    QTRY_COMPARE_WITH_TIMEOUT(gone.count(), 1, 8000);
    QVERIFY(!start.first().at(0).toBool());
    QCOMPARE(start.first().at(1).toString(), QString());
    QVERIFY(!gone.first().at(2).toString().isEmpty());
    // No service is not an older service.
    QCOMPARE(nobodyFailed.count(), 2);
    for (int i = 0; i < nobodyFailed.count(); ++i)
        QVERIFY(!ServiceClient::isOlderServiceError(nobodyFailed.at(i).at(1).toString()));
}

// ---- the UI guard ----------------------------------------------------

void TstDaemon::theFirstUiTakesTheLockASecondOneRaisesAndLeaves()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    UiGuard first(dir.path());
    QSignalSpy raised(&first, SIGNAL(raiseRequested()));
    QCOMPARE(first.acquire(), UiGuard::First);

    // A second UI in the same process stands in for a second process: the lock records the
    // process id, and a living process holds it.
    UiGuard second(dir.path());
    QCOMPARE(second.acquire(), UiGuard::SecondRaised);
    QTRY_COMPARE_WITH_TIMEOUT(raised.count(), 1, 3000);

    UiGuard third(dir.path());
    QCOMPARE(third.acquire(), UiGuard::SecondRaised);
    QTRY_COMPARE_WITH_TIMEOUT(raised.count(), 2, 3000);
}

void TstDaemon::aStaleLockFromAKilledUiIsRecovered()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    // What a killed UI leaves: a lock file naming a process that does not exist, and a socket file.
    QFile lock(dir.path() + "/ui.lock");
    QVERIFY(lock.open(QIODevice::WriteOnly));
    // QLockFile can only judge a lock stale when it names this host.
    lock.write(QByteArray("2147483646\nharbour-libre-monitor\n") + QSysInfo::machineHostName().toUtf8() + "\n");
    lock.close();
    QFile sock(dir.path() + "/ui.sock");
    QVERIFY(sock.open(QIODevice::WriteOnly));
    sock.close();
    UiGuard guard(dir.path());
    QCOMPARE(guard.acquire(), UiGuard::First);
}

// ---- the version stamp --------------------

namespace {

class ScriptedStampReader : public VersionStampReader
{
public:
    StampRead read() { ++reads; return next; }
    StampRead next = StampRead::present(QStringLiteral("0.1-3\n"));
    int reads = 0;
};

// Records the activation and whether the old service still owned the name at that moment: the
// new process can take the name only if the old one released it first.
class RecordingActivator : public ServiceActivator
{
public:
    explicit RecordingActivator(const QString &name) : name(name) {}
    bool activate(QString *how)
    {
        ++calls;
        ownedAtCall = QDBusConnection::sessionBus().interface()->isServiceRegistered(name).value();
        if (how) *how = QStringLiteral("test");
        return true;
    }
    QString name;
    int calls = 0;
    bool ownedAtCall = true;
};

struct StampRig
{
    explicit StampRig(const QString &suffix) : rig(suffix), activator(rig.options.identity.busName)
    {
        rig.options.appVersion = QStringLiteral("0.1-3");
        rig.platform.stamp = &stamp;
        rig.platform.activator = &activator;
        rig.platform.serviceSettings = &rig.settings;
    }
    // One wake-up a full heartbeat interval later: one heartbeat, so one check.
    void heartbeat()
    {
        rig.clock.advanceS(60);
        rig.wake.fire();
        QTest::qWait(20);
    }
    RuntimeRig rig;
    ScriptedStampReader stamp;
    RecordingActivator activator;
};

bool waitStopped(ServiceRuntime &runtime)
{
    int guard = 0;
    while (!runtime.stopped() && guard++ < 100) QTest::qWait(100);
    return runtime.stopped();
}

QVector<JournalEntry> journalEntries(const QString &dir, const QString &kind)
{
    Journal journal;
    if (!journal.openReadOnly(dir + "/journal.db"))
        return QVector<JournalEntry>();
    return journal.entries(kind);
}

} // namespace

void TstDaemon::theSameStampKeepsRunningChecksOncePerHeartbeatAndStatusCarriesTheVersion()
{
    if (!haveBus())
        QSKIP("no session bus");
    StampRig s(QStringLiteral("-stamp-same"));
    ServiceRuntime runtime(s.rig.options, s.rig.platform);
    QCOMPARE(runtime.begin(), -1);
    QCOMPARE(s.stamp.reads, 0);                 // not at the start: at the heartbeats
    for (int i = 0; i < 3; ++i)
        s.heartbeat();
    QCOMPARE(s.stamp.reads, 3);
    // A wake-up between heartbeats reads nothing (a wake-up stays cheap).
    s.rig.clock.advanceS(20);
    s.rig.wake.fire();
    QCOMPARE(s.stamp.reads, 3);
    QCOMPARE(runtime.lifecycle()->state(), ServiceState::Running);
    const QJsonObject status = QJsonDocument::fromJson(runtime.apiStatusJson().toUtf8()).object();
    QCOMPARE(status.value("service").toObject().value("app_version").toString(), QStringLiteral("0.1-3"));
    QCOMPARE(status.value("format").toInt(), 1);
    QCOMPARE(s.activator.calls, 0);
    stopAndWait(runtime);
    QCOMPARE(s.activator.calls, 0);             // an ordinary stop starts nothing
    QCOMPARE(s.rig.notifier.last().state, NoticeState::Stopped);    // and keeps its notice
    QCOMPARE(s.rig.notifier.last().stopReason, StopReason::requested());
    QCOMPARE(s.rig.notifier.withdrawals, 0);
}

void TstDaemon::aNewerStampStopsWithReasonUpgradedAndActivatesTheNameAfterReleasingIt()
{
    if (!haveBus())
        QSKIP("no session bus");
    StampRig s(QStringLiteral("-stamp-new"));
    ServiceRuntime runtime(s.rig.options, s.rig.platform);
    QCOMPARE(runtime.begin(), -1);
    s.heartbeat();
    QCOMPARE(runtime.lifecycle()->state(), ServiceState::Running);
    s.stamp.next = StampRead::present(QStringLiteral("0.1-4\n"));     // rpm -U replaced the package
    s.heartbeat();
    QVERIFY(waitStopped(runtime));
    QCOMPARE(s.activator.calls, 1);
    QVERIFY(!s.activator.ownedAtCall);          // the name was free for the new binary
    QCOMPARE(runtime.lifecycle()->stopReason(), StopReason::upgraded());
    // no "monitoring stopped" notice for an upgrade stop; the last one shown is the start's.
    for (int i = 0; i < s.rig.notifier.shown.size(); ++i)
        QVERIFY(s.rig.notifier.shown.at(i).state != NoticeState::Stopped);
    QCOMPARE(s.rig.notifier.last().state, NoticeState::Active);
    QCOMPARE(s.rig.notifier.withdrawals, 0);

    const QVector<JournalEntry> stops = journalEntries(s.rig.dir.path(), JournalKind::stop());
    QCOMPARE(stops.size(), 1);
    QCOMPARE(stops.first().extra.value("reason").toString(), QStringLiteral("upgraded"));
    QCOMPARE(stops.first().extra.value("app_version").toString(), QStringLiteral("0.1-3"));
    QCOMPARE(stops.first().extra.value("stamp").toString(), QStringLiteral("0.1-4"));
    // The loop guard's time is in the settings for the new process.
    JsonFileSettings again(s.rig.dir.path() + "/settings.json");
    QVERIFY(UtcTime::fromIso(again.value(VersionWatch::restartKey())).isValid());
}

void TstDaemon::aMissingStampStopsWithReasonPackageRemovedAndStartsNothing()
{
    if (!haveBus())
        QSKIP("no session bus");
    StampRig s(QStringLiteral("-stamp-gone"));
    ServiceRuntime runtime(s.rig.options, s.rig.platform);
    QCOMPARE(runtime.begin(), -1);
    s.stamp.next = StampRead::missing();        // rpm -e
    s.heartbeat();
    QVERIFY(waitStopped(runtime));
    QCOMPARE(s.activator.calls, 0);
    // ends quietly; the resident notice is taken away, no "stopped" notice.
    for (int i = 0; i < s.rig.notifier.shown.size(); ++i)
        QVERIFY(s.rig.notifier.shown.at(i).state != NoticeState::Stopped);
    QCOMPARE(s.rig.notifier.withdrawals, 1);
    const QVector<JournalEntry> stops = journalEntries(s.rig.dir.path(), JournalKind::stop());
    QCOMPARE(stops.size(), 1);
    QCOMPARE(stops.first().extra.value("reason").toString(), QStringLiteral("package-removed"));
    // The user's data stays (rollback rule): the files are closed, not removed.
    QVERIFY(QFile::exists(s.rig.dir.path() + "/journal.db"));
    QVERIFY(QFile::exists(s.rig.dir.path() + "/store.db"));
}

void TstDaemon::anUnreadableStampKeepsMonitoringAndIsJournalledOnce()
{
    if (!haveBus())
        QSKIP("no session bus");
    StampRig s(QStringLiteral("-stamp-odd"));
    ServiceRuntime runtime(s.rig.options, s.rig.platform);
    QCOMPARE(runtime.begin(), -1);
    s.stamp.next = StampRead::unreadable();
    s.heartbeat();
    s.heartbeat();
    s.stamp.next = StampRead::present(QString());          // an empty file
    s.heartbeat();
    QCOMPARE(runtime.lifecycle()->state(), ServiceState::Running);
    QCOMPARE(s.activator.calls, 0);
    stopAndWait(runtime);
    const QVector<JournalEntry> notes = journalEntries(s.rig.dir.path(), JournalKind::versionCheck());
    QCOMPARE(notes.size(), 1);
    QCOMPARE(notes.first().extra.value("result").toString(), QStringLiteral("stamp-unreadable"));
    QCOMPARE(journalEntries(s.rig.dir.path(), JournalKind::stop()).first().extra.value("reason").toString(),
             QStringLiteral("requested"));
}

void TstDaemon::noVersionCheckActsDuringAStopAlreadyInProgress()
{
    if (!haveBus())
        QSKIP("no session bus");
    StampRig s(QStringLiteral("-stamp-stop"));
    ServiceRuntime runtime(s.rig.options, s.rig.platform);
    QCOMPARE(runtime.begin(), -1);
    s.stamp.next = StampRead::present(QStringLiteral("0.1-4"));
    runtime.stop(StopReason::requested());
    s.heartbeat();                              // a late wake-up during the stop
    QVERIFY(waitStopped(runtime));
    QCOMPARE(s.stamp.reads, 0);
    QCOMPARE(s.activator.calls, 0);
    QCOMPARE(runtime.lifecycle()->stopReason(), StopReason::requested());
}

void TstDaemon::theRestartedServiceThatStillSeesAnotherVersionDoesNotLoop()
{
    if (!haveBus())
        QSKIP("no session bus");
    StampRig s(QStringLiteral("-stamp-loop"));
    s.stamp.next = StampRead::present(QStringLiteral("0.1-4"));
    {
        ServiceRuntime first(s.rig.options, s.rig.platform);
        QCOMPARE(first.begin(), -1);
        s.heartbeat();
        QVERIFY(waitStopped(first));
        QCOMPARE(s.activator.calls, 1);
    }
    // The process the activation started is still 0.1-3 (the stamp says 0.1-4): a broken install.
    // It reads the guard's time from the settings file the first one wrote.
    JsonFileSettings fresh(s.rig.dir.path() + "/settings.json");
    s.rig.platform.serviceSettings = &fresh;
    ServiceRuntime second(s.rig.options, s.rig.platform);
    QCOMPARE(second.begin(), -1);
    for (int i = 0; i < 5; ++i)
        s.heartbeat();
    QCOMPARE(second.lifecycle()->state(), ServiceState::Running);
    QCOMPARE(s.activator.calls, 1);
    // After 10 minutes one more restart is allowed (and then the guard holds again).
    for (int i = 0; i < 6 && !second.stopped() && second.lifecycle()->state() != ServiceState::Stopping; ++i)
        s.heartbeat();
    QVERIFY(waitStopped(second));
    QCOMPARE(s.activator.calls, 2);
    const QVector<JournalEntry> notes = journalEntries(s.rig.dir.path(), JournalKind::versionCheck());
    QCOMPARE(notes.size(), 1);
    QCOMPARE(notes.first().extra.value("result").toString(), QStringLiteral("restart-suppressed"));
    QCOMPARE(notes.first().extra.value("stamp").toString(), QStringLiteral("0.1-4"));
    const QVector<JournalEntry> starts = journalEntries(s.rig.dir.path(), JournalKind::start());
    QCOMPARE(starts.size(), 2);
    QCOMPARE(starts.last().cause, StartCause::afterStop());
}

void TstDaemon::theStampFileReaderTellsMissingFromUnreadable()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    QCOMPARE(FileVersionStamp(dir.path() + "/version").read().kind, StampRead::Missing);
    QCOMPARE(FileVersionStamp(dir.path() + "/gone/version").read().kind, StampRead::Missing);
    QFile f(dir.path() + "/version");
    QVERIFY(f.open(QIODevice::WriteOnly));
    f.write("0.1-3\n");
    f.close();
    StampRead r = FileVersionStamp(dir.path() + "/version").read();
    QCOMPARE(r.kind, StampRead::Present);
    QCOMPARE(r.text, QStringLiteral("0.1-3\n"));
    QVERIFY(QDir(dir.path()).mkdir("folder"));
    QCOMPARE(FileVersionStamp(dir.path() + "/folder").read().kind, StampRead::Unreadable);
    QFile big(dir.path() + "/big");
    QVERIFY(big.open(QIODevice::WriteOnly));
    big.write(QByteArray(4096, '1'));
    big.close();
    QCOMPARE(FileVersionStamp(dir.path() + "/big").read().text.size(), 256);
    // A file that exists but cannot be opened (not as root, which reads everything).
    QFile closed(dir.path() + "/closed");
    QVERIFY(closed.open(QIODevice::WriteOnly));
    closed.write("0.1-3\n");
    closed.close();
    QVERIFY(closed.setPermissions(QFileDevice::Permissions()));
    if (!closed.open(QIODevice::ReadOnly))
        QCOMPARE(FileVersionStamp(dir.path() + "/closed").read().kind, StampRead::Unreadable);
    closed.setPermissions(QFileDevice::ReadOwner | QFileDevice::WriteOwner);
}

// ---- compaction through the service -----------------------

namespace {

// Old synthetic one-minute samples of the current feed (200 days back, five per 5-minute slot, so
// compaction thins them) and recent ones, written into the store file before the service opens it.
qint64 seedSamples(const QString &storePath, int oldCount)
{
    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    const qint64 day = 86400000LL;
    const QUuid subject(QString::fromLatin1(kSubjectUuid));
    Store store;
    if (!store.open(storePath))
        return -1;
    IngestContext c;
    c.now = UtcTime::fromMSecsSinceEpoch(now);
    c.adapter = QStringLiteral("testadapter");
    c.adapterVersion = QStringLiteral("0.0.1");
    c.sessionId = QStringLiteral("s-1");
    auto sample = [&](qint64 at, double v) {
        IngestItem i;
        i.record.shape = Record::Sample;
        i.record.subject = subject;
        i.record.metric = metricGlucose();
        i.record.value = v;
        i.record.unit = QStringLiteral("mg/dL");
        i.record.measuredAt = UtcTime::fromMSecsSinceEpoch(at);
        i.record.ingestedAt = UtcTime::fromMSecsSinceEpoch(now);
        i.record.originalTimestamp = QStringLiteral("1/1/2026 11:55:00 AM");
        i.record.source = QStringLiteral("libre.current");
        i.record.origin = Origin::Adapter;
        i.raw.insert(QStringLiteral("marker"), 1);
        return i;
    };
    QVector<IngestItem> items;
    for (int k = 0; k < oldCount; ++k)
        items.append(sample(now - 200 * day + qint64(k) * 60000, 100 + k % 50));
    for (int k = 0; k < 60; ++k)
        items.append(sample(now - day + qint64(k) * 60000, 120));
    if (!store.ingest(items, c).ok)
        return -1;
    return store.recordCount();
}

qint64 recordsIn(const QString &storePath)
{
    Store store;
    return store.openReadOnly(storePath) ? store.recordCount() : -1;
}

// Runs one compaction through the D-Bus method and returns the MaintenanceFinished arguments.
QList<QVariant> compactOverTheBus(RuntimeRig &rig, ServiceRuntime &runtime, QList<QList<QVariant> > *progress)
{
    ServiceClient client(rig.options.identity.busName, rig.options.identity.objectPath, clientBus());
    QSignalSpy finished(&client, SIGNAL(maintenanceFinished(QString,bool,QString,QString,QString,QString)));
    QSignalSpy steps(&client, SIGNAL(maintenanceProgress(QString,QString,int,int,qlonglong,qlonglong,double)));
    QSignalSpy started(&client, SIGNAL(startMaintenanceAnswered(bool,QString,QString)));
    client.startMaintenanceAsync(QStringLiteral("compaction"), QStringLiteral("{\"window_months\":3}"));
    if (!started.wait(5000) && started.isEmpty())
        return QList<QVariant>();
    if (!started.first().at(2).toString().isEmpty())
        return QList<QVariant>() << QStringLiteral("refused:") + started.first().at(2).toString();
    for (int i = 0; i < 300 && finished.isEmpty(); ++i)
        QTest::qWait(100);
    Q_UNUSED(runtime);
    if (progress)
        for (int i = 0; i < steps.size(); ++i)
            progress->append(steps.at(i));
    return finished.isEmpty() ? QList<QVariant>() : finished.first();
}

} // namespace

void TstDaemon::aCompactionThroughTheServiceThinsOldSamplesAndReportsTheEstimate()
{
    if (!haveBus())
        QSKIP("no session bus");
    RuntimeRig rig(QStringLiteral("-compact"));
    rig.options.compactionFreeSpace = [](const QString &) { return qint64(1) << 40; };
    const QString storePath = rig.dir.path() + "/store.db";
    const qint64 before = seedSamples(storePath, 2000);
    QVERIFY(before > 2000);
    ServiceRuntime runtime(rig.options, rig.platform);
    QCOMPARE(runtime.begin(), -1);
    QList<QList<QVariant> > progress;
    const QList<QVariant> f = compactOverTheBus(rig, runtime, &progress);
    QCOMPARE(f.size(), 6);
    QCOMPARE(f.at(0).toString(), QStringLiteral("compaction"));
    QVERIFY2(f.at(1).toBool(), qPrintable(f.at(2).toString() + " " + f.at(3).toString()));
    const QJsonObject d = QJsonDocument::fromJson(f.at(5).toString().toUtf8()).object();
    QVERIFY(d.value("removed").toDouble() > 0);
    QVERIFY(d.value("records").toDouble() > 0);
    QCOMPARE(d.value("space").toString(), QStringLiteral("enough"));
    QVERIFY(d.value("rebuild_need_bytes").toDouble() > 0);
    QCOMPARE(d.value("missing_bytes").toDouble(), 0.0);
    QVERIFY(d.contains("bytes_freed"));
    QVERIFY(!f.at(5).toString().contains(QStringLiteral("archive")));
    // The phases of the new core, in order, and nothing of the archive.
    QStringList phases;
    for (int i = 0; i < progress.size(); ++i)
        if (phases.isEmpty() || phases.last() != progress.at(i).at(1).toString())
            phases << progress.at(i).at(1).toString();
    QCOMPARE(phases.first(), QStringLiteral("checking-space"));
    QVERIFY(phases.contains(QStringLiteral("compacting")));
    QCOMPARE(phases.last(), QStringLiteral("done"));
    QVERIFY(phases.filter(QStringLiteral("verif")).isEmpty());
    stopAndWait(runtime);
    const qint64 after = recordsIn(storePath);
    QVERIFY2(after < before && after >= 60, qPrintable(QString("%1 -> %2").arg(before).arg(after)));
}

void TstDaemon::aCompactionIsRefusedWhenTheRebuildWouldNotFit()
{
    if (!haveBus())
        QSKIP("no session bus");
    RuntimeRig rig(QStringLiteral("-compact-short"));
    rig.options.compactionFreeSpace = [](const QString &) { return qint64(1000); };
    const QString storePath = rig.dir.path() + "/store.db";
    const qint64 before = seedSamples(storePath, 2000);
    ServiceRuntime runtime(rig.options, rig.platform);
    QCOMPARE(runtime.begin(), -1);
    const QList<QVariant> f = compactOverTheBus(rig, runtime, 0);
    QCOMPARE(f.size(), 6);
    QVERIFY(!f.at(1).toBool());
    QCOMPARE(f.at(2).toString(), QStringLiteral("insufficient-space"));
    const QJsonObject d = QJsonDocument::fromJson(f.at(5).toString().toUtf8()).object();
    QCOMPARE(d.value("space").toString(), QStringLiteral("short"));
    QCOMPARE(d.value("free_bytes").toDouble(), 1000.0);
    QCOMPARE(d.value("missing_bytes").toDouble(), d.value("rebuild_need_bytes").toDouble() - 1000.0);
    QVERIFY(d.value("missing_bytes").toDouble() > 0);
    QVERIFY(!d.contains("removed"));
    QCOMPARE(runtime.lifecycle()->state(), ServiceState::Running);     // monitoring goes on
    stopAndWait(runtime);
    QCOMPARE(recordsIn(storePath), before);                             // nothing deleted
}

void TstDaemon::aCompactionIsRefusedWhenTheFreeSpaceIsUnknown()
{
    if (!haveBus())
        QSKIP("no session bus");
    RuntimeRig rig(QStringLiteral("-compact-unknown"));
    rig.options.compactionFreeSpace = [](const QString &) { return qint64(-1); };
    const QString storePath = rig.dir.path() + "/store.db";
    const qint64 before = seedSamples(storePath, 2000);
    ServiceRuntime runtime(rig.options, rig.platform);
    QCOMPARE(runtime.begin(), -1);
    const QList<QVariant> f = compactOverTheBus(rig, runtime, 0);
    QCOMPARE(f.size(), 6);
    QVERIFY(!f.at(1).toBool());
    // Decision:: the service never deletes without the space check, no override.
    QCOMPARE(f.at(2).toString(), QStringLiteral("free-space-unknown"));
    const QJsonObject d = QJsonDocument::fromJson(f.at(5).toString().toUtf8()).object();
    QCOMPARE(d.value("space").toString(), QStringLiteral("unknown"));
    QCOMPARE(d.value("free_bytes").toDouble(), -1.0);
    QCOMPARE(d.value("missing_bytes").toDouble(), 0.0);
    QCOMPARE(runtime.lifecycle()->state(), ServiceState::Running);
    stopAndWait(runtime);
    QCOMPARE(recordsIn(storePath), before);
}

void TstDaemon::theEstimateDetailsHaveTheirKeys()
{
    CompactionEstimate e;
    e.cutoff = UtcTime::fromMSecsSinceEpoch(1768000000000LL);
    e.records = 10; e.quarantine = 2; e.ingest = 3; e.bytes = 4000;
    e.storeBytes = 50000; e.rebuildNeedBytes = 48000; e.freeBytes = 40000;
    e.space = CompactionEstimate::SpaceShort;
    const QJsonObject o = compactionEstimateDetails(e);
    QCOMPARE(o.keys(), QStringList() << "bytes_estimated" << "cutoff" << "free_bytes" << "ingest"
             << "missing_bytes" << "quarantine" << "rebuild_need_bytes" << "records" << "space" << "store_bytes");
    QCOMPARE(o.value("missing_bytes").toDouble(), 8000.0);
    QCOMPARE(o.value("space").toString(), QStringLiteral("short"));
    QCOMPARE(o.value("cutoff").toString(), e.cutoff.toIso());
}

// ---- purge through the service -------------------

namespace {

// Three connections: c1 serves A alone (label "AB"), c2 serves B alone (label "B2"), c3 serves C (no
// label) and D together. Names are never read by the service (the key `name` is ignored).
const char *const kSubjectB = "{6a1b2c3d-4e5f-4a6b-8c7d-9e0f1a2b3c4d}";
const char *const kSubjectC = "{7b2c3d4e-5f6a-4b7c-9d8e-0f1a2b3c4d5e}";
const char *const kSubjectD = "{8c3d4e5f-6a7b-4c8d-ae9f-1a2b3c4d5e6f}";
const char *const kPurgeConfig =
    "{\"format\":1,\"connections\":["
    "{\"id\":\"c1\",\"adapter\":\"libre\",\"subjects\":[{\"id\":\"{d2f1c9a0-5b3e-4c7d-8a91-0e2b4c6d8f10}\","
    "\"ref\":\"opaque-1\",\"label\":\"AB\",\"name\":\"A Real Name\"}]},"
    "{\"id\":\"c2\",\"adapter\":\"libre\",\"subjects\":[{\"id\":\"{6a1b2c3d-4e5f-4a6b-8c7d-9e0f1a2b3c4d}\","
    "\"ref\":\"opaque-2\",\"label\":\"B2\"}]},"
    "{\"id\":\"c3\",\"adapter\":\"libre\",\"subjects\":[{\"id\":\"{7b2c3d4e-5f6a-4b7c-9d8e-0f1a2b3c4d5e}\","
    "\"ref\":\"opaque-3\"},{\"id\":\"{8c3d4e5f-6a7b-4c8d-ae9f-1a2b3c4d5e6f}\",\"ref\":\"opaque-4\",\"label\":\"D\"}]}]}";

QUuid subjectA() { return QUuid(QString::fromLatin1(kSubjectUuid)); }
QUuid subjectB() { return QUuid(QString::fromLatin1(kSubjectB)); }

void writePurgeConfig(RuntimeRig &rig)
{
    QFile f(rig.dir.path() + "/config.json");
    QVERIFY(f.open(QIODevice::WriteOnly | QIODevice::Truncate));
    f.write(kPurgeConfig);
}

// Synthetic one-minute samples of one Subject: `oldCount` 200 days back (older than every allowed age
// but 12 and 24 months: the tests purge at 3 months) and `recentCount` from a day back.
bool seedSubject(const QString &storePath, const QUuid &subject, int oldCount, int recentCount)
{
    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    const qint64 day = 86400000LL;
    Store store;
    if (!store.open(storePath))
        return false;
    IngestContext c;
    c.now = UtcTime::fromMSecsSinceEpoch(now);
    c.adapter = QStringLiteral("testadapter");
    c.adapterVersion = QStringLiteral("0.0.1");
    c.sessionId = QStringLiteral("s-1");
    QVector<IngestItem> items;
    for (int k = 0; k < oldCount + recentCount; ++k) {
        IngestItem i;
        i.record.shape = Record::Sample;
        i.record.subject = subject;
        i.record.metric = metricGlucose();
        i.record.value = 100 + k % 50;
        i.record.unit = QStringLiteral("mg/dL");
        const qint64 at = k < oldCount ? now - 200 * day + qint64(k) * 60000 : now - day + qint64(k - oldCount) * 60000;
        i.record.measuredAt = UtcTime::fromMSecsSinceEpoch(at);
        i.record.ingestedAt = UtcTime::fromMSecsSinceEpoch(now);
        i.record.originalTimestamp = QStringLiteral("1/1/2026 11:55:00 AM");
        i.record.source = QStringLiteral("libre.current");
        i.record.origin = Origin::Adapter;
        i.raw.insert(QStringLiteral("marker"), 1);
        items.append(i);
    }
    return store.ingest(items, c).ok;
}

// Old and recent alert, threshold and silent entries of one Subject in the service journal.
bool seedJournalOf(const QString &journalPath, const QUuid &subject)
{
    Journal journal;
    if (!journal.open(journalPath))
        return false;
    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    const qint64 day = 86400000LL;
    static int seq = 0;
    bool ok = true;
    foreach (qint64 when, QList<qint64>() << now - 200 * day << now - 2 * day) {
        AlertRecord r;
        r.id = QUuid::createUuid();
        r.subject = subject;
        r.episode = QUuid::createUuid();
        r.slot = AlertSlot::Low;
        r.type = AlertType::L;
        r.state = AlertState::Pending;
        r.from = AlertState::Normal;
        r.event = QStringLiteral("pending");
        r.time = UtcTime::fromMSecsSinceEpoch(when);
        r.since = r.time;
        r.seq = ++seq;
        ok = ok && journal.append(toJournalEntry(r));
        ThresholdChange t;
        t.id = QUuid::createUuid();
        t.subject = subject;
        t.boundary = Boundary::L;
        t.value = 75;
        t.previous = 70;
        t.time = UtcTime::fromMSecsSinceEpoch(when + 1000);
        ok = ok && writeThresholdChanges(&journal, QVector<ThresholdChange>() << t);
        SilentChange s;
        s.id = QUuid::createUuid();
        s.subject = subject;
        s.event = QStringLiteral("start");
        s.time = UtcTime::fromMSecsSinceEpoch(when + 2000);
        ok = ok && writeSilentChange(&journal, s);
    }
    return ok;
}

qint64 recordsOf(const QString &storePath, const QUuid &subject)
{
    Store store;
    return store.openReadOnly(storePath)
        ? store.countRecordsOfSubjectBefore(subject, UtcTime::fromMSecsSinceEpoch(QDateTime::currentMSecsSinceEpoch() + 86400000LL))
        : -1;
}

// The journal entries that carry this Subject (alert, threshold, silent), counted by a reader.
qint64 journalEntriesOf(const QString &journalPath, const QUuid &subject)
{
    Journal journal;
    if (!journal.openReadOnly(journalPath))
        return -1;
    qint64 n = 0;
    const QVector<JournalEntry> all = journal.entries();
    for (int i = 0; i < all.size(); ++i)
        if (QUuid(all.at(i).extra.value(QStringLiteral("subject")).toString()) == subject)
            ++n;
    return n;
}

// One maintenance run over the D-Bus interface; the MaintenanceFinished arguments, or
// ["refused:<key>"] when StartMaintenance refused it.
QList<QVariant> runOverTheBus(RuntimeRig &rig, const QString &kind, const QJsonObject &parameters,
                              QList<QList<QVariant> > *progress = 0)
{
    ServiceClient client(rig.options.identity.busName, rig.options.identity.objectPath, clientBus());
    QSignalSpy finished(&client, SIGNAL(maintenanceFinished(QString,bool,QString,QString,QString,QString)));
    QSignalSpy steps(&client, SIGNAL(maintenanceProgress(QString,QString,int,int,qlonglong,qlonglong,double)));
    QSignalSpy started(&client, SIGNAL(startMaintenanceAnswered(bool,QString,QString)));
    client.startMaintenanceAsync(kind, QString::fromUtf8(QJsonDocument(parameters).toJson(QJsonDocument::Compact)));
    if (!started.wait(5000) && started.isEmpty())
        return QList<QVariant>();
    if (!started.first().at(2).toString().isEmpty())
        return QList<QVariant>() << QStringLiteral("refused:") + started.first().at(2).toString();
    for (int i = 0; i < 300 && finished.isEmpty(); ++i)
        QTest::qWait(100);
    if (progress)
        for (int i = 0; i < steps.size(); ++i)
            progress->append(steps.at(i));
    return finished.isEmpty() ? QList<QVariant>() : finished.first();
}

QJsonObject detailsOf(const QList<QVariant> &finished)
{
    return finished.size() == 6 ? QJsonDocument::fromJson(finished.at(5).toString().toUtf8()).object() : QJsonObject();
}

QJsonObject purgeParameters(const QUuid &subject, int months, const QString &typed = QString(), bool withLabel = true)
{
    QJsonObject p;
    p.insert(QStringLiteral("subject"), subject.toString());
    p.insert(QStringLiteral("age_months"), months);
    if (withLabel)
        p.insert(QStringLiteral("typed_label"), typed);
    return p;
}

QString startOf(ServiceRuntime &runtime, const QString &kind, const QJsonObject &p)
{
    return runtime.apiStartMaintenance(kind, QString::fromUtf8(QJsonDocument(p).toJson(QJsonDocument::Compact)));
}

} // namespace

// the Subject's facts come from the configuration: its reference, and the connections
// that serve it alone (c3 serves two Subjects, so its log is never purged for either).
void TstDaemon::theConfigurationGivesThePurgeItsSubjectFacts()
{
    RuntimeRig rig(QStringLiteral("-ptarget"));
    writePurgeConfig(rig);
    ServiceRuntime runtime(rig.options, rig.platform);
    if (!haveBus())
        QSKIP("no session bus");
    QCOMPARE(runtime.begin(), -1);
    const PurgeTarget a = runtime.core()->purgeTarget(subjectA());
    QVERIFY(a.known);
    QCOMPARE(a.label, QStringLiteral("AB"));
    QCOMPARE(a.subjectRefs, QStringList() << "opaque-1");
    QCOMPARE(a.soleConnections, QStringList() << "c1");
    const PurgeTarget c = runtime.core()->purgeTarget(QUuid(QString::fromLatin1(kSubjectC)));
    QVERIFY(c.known);
    QVERIFY(c.label.isEmpty());
    QCOMPARE(c.subjectRefs, QStringList() << "opaque-3");
    QVERIFY(c.soleConnections.isEmpty());
    const PurgeTarget d = runtime.core()->purgeTarget(QUuid(QString::fromLatin1(kSubjectD)));
    QCOMPARE(d.label, QStringLiteral("D"));
    QVERIFY(d.soleConnections.isEmpty());
    QVERIFY(!runtime.core()->purgeTarget(QUuid::createUuid()).known);
    stopAndWait(runtime);
}

// and the age of REQ-STO-17: every refusal comes from StartMaintenance itself, nothing starts
// and nothing changes. The comparison is exact (no trimming, no case folding).
void TstDaemon::aPurgeIsRefusedBeforeItStartsWithoutTheExactLabel()
{
    if (!haveBus())
        QSKIP("no session bus");
    RuntimeRig rig(QStringLiteral("-pgate"));
    writePurgeConfig(rig);
    const QString storePath = rig.dir.path() + "/store.db";
    const QString journalPath = rig.dir.path() + "/journal.db";
    QVERIFY(seedSubject(storePath, subjectA(), 500, 30));
    QVERIFY(seedJournalOf(journalPath, subjectA()));
    ServiceRuntime runtime(rig.options, rig.platform);
    QCOMPARE(runtime.begin(), -1);
    const qint64 journalA = journalEntriesOf(journalPath, subjectA());     // after the engine's start entries
    const QString purge = QStringLiteral("purge");
    const QUuid a = subjectA();
    QCOMPARE(startOf(runtime, purge, purgeParameters(a, 3, "ab")), QStringLiteral("label-mismatch"));
    QCOMPARE(startOf(runtime, purge, purgeParameters(a, 3, "AB ")), QStringLiteral("label-mismatch"));
    QCOMPARE(startOf(runtime, purge, purgeParameters(a, 3, "")), QStringLiteral("label-mismatch"));
    QCOMPARE(startOf(runtime, purge, purgeParameters(a, 3, "B2")), QStringLiteral("label-mismatch"));   // another Subject's label
    QCOMPARE(startOf(runtime, purge, purgeParameters(a, 3, QString(), false)), QStringLiteral("label-mismatch"));
    QJsonObject numeric = purgeParameters(a, 3, QString(), false);
    numeric.insert(QStringLiteral("typed_label"), 12);
    QCOMPARE(startOf(runtime, purge, numeric), QStringLiteral("label-mismatch"));
    QCOMPARE(startOf(runtime, purge, purgeParameters(QUuid(QString::fromLatin1(kSubjectC)), 3, "")), QStringLiteral("no-label"));
    QCOMPARE(startOf(runtime, purge, purgeParameters(QUuid::createUuid(), 3, "AB")), QStringLiteral("unknown-subject"));
    QJsonObject noSubject = purgeParameters(a, 3, "AB");
    noSubject.insert(QStringLiteral("subject"), QStringLiteral("AB"));
    QCOMPARE(startOf(runtime, purge, noSubject), QStringLiteral("parameter-subject"));
    noSubject.remove(QStringLiteral("subject"));
    QCOMPARE(startOf(runtime, purge, noSubject), QStringLiteral("parameter-subject"));
    foreach (int bad, QList<int>() << 0 << 1 << 2 << 4 << 5 << 9 << 18 << 36 << -3)
        QCOMPARE(startOf(runtime, purge, purgeParameters(a, bad, "AB")), QStringLiteral("age-not-allowed"));
    QJsonObject text = purgeParameters(a, 3, "AB");
    text.insert(QStringLiteral("age_months"), QStringLiteral("3"));
    QCOMPARE(startOf(runtime, purge, text), QStringLiteral("age-not-allowed"));
    text.insert(QStringLiteral("age_months"), 3.5);
    QCOMPARE(startOf(runtime, purge, text), QStringLiteral("age-not-allowed"));
    text.remove(QStringLiteral("age_months"));
    QCOMPARE(startOf(runtime, purge, text), QStringLiteral("age-not-allowed"));
    // The estimate needs no label but the same Subject and age checks.
    QCOMPARE(startOf(runtime, QStringLiteral("purge-estimate"), purgeParameters(a, 5, QString(), false)), QStringLiteral("age-not-allowed"));
    QCOMPARE(startOf(runtime, QStringLiteral("purge-estimate"), purgeParameters(QUuid::createUuid(), 3, QString(), false)), QStringLiteral("unknown-subject"));
    QVERIFY(!runtime.maintenance()->isRunning());
    stopAndWait(runtime);
    QCOMPARE(recordsOf(storePath, a), qint64(530));
    QCOMPARE(journalEntriesOf(journalPath, a), journalA);
}

// The estimate (a dry run on read-only connections) changes nothing; the purge with the right label then
// removes exactly what it said, per table and per journal kind, of A only: B and A's recent data stay.
void TstDaemon::aPurgeEstimateChangesNothingAndThePurgeRemovesExactlyIt()
{
    if (!haveBus())
        QSKIP("no session bus");
    RuntimeRig rig(QStringLiteral("-prun"));
    writePurgeConfig(rig);
    rig.options.compactionFreeSpace = [](const QString &) { return qint64(1) << 40; };
    const QString storePath = rig.dir.path() + "/store.db";
    const QString journalPath = rig.dir.path() + "/journal.db";
    QVERIFY(seedSubject(storePath, subjectA(), 1500, 40));
    QVERIFY(seedSubject(storePath, subjectB(), 1500, 40));
    QVERIFY(seedJournalOf(journalPath, subjectA()));
    QVERIFY(seedJournalOf(journalPath, subjectB()));
    ServiceRuntime runtime(rig.options, rig.platform);
    QCOMPARE(runtime.begin(), -1);
    const qint64 journalA = journalEntriesOf(journalPath, subjectA());     // after the engine's start entries
    const qint64 journalB = journalEntriesOf(journalPath, subjectB());

    const QList<QVariant> est = runOverTheBus(rig, QStringLiteral("purge-estimate"), purgeParameters(subjectA(), 3, QString(), false));
    QCOMPARE(est.size(), 6);
    QCOMPARE(est.at(0).toString(), QStringLiteral("purge-estimate"));
    QVERIFY2(est.at(1).toBool(), qPrintable(est.at(2).toString()));
    const QJsonObject e = detailsOf(est);
    QCOMPARE(e.value("store_rows").toObject().value("record").toDouble(), 1500.0);
    QVERIFY(e.value("journal_entries").toObject().value("alert").toDouble() >= 1.0);   // the seeded one, and the engine's
    QCOMPARE(e.value("journal_entries").toObject().value("threshold").toDouble(), 1.0);
    QCOMPARE(e.value("journal_entries").toObject().value("silent").toDouble(), 1.0);
    QCOMPARE(e.value("space").toString(), QStringLiteral("enough"));
    QVERIFY(e.value("needed_bytes").toDouble() > 0);
    QCOMPARE(recordsOf(storePath, subjectA()), qint64(1540));        // the estimate changed nothing
    QCOMPARE(journalEntriesOf(journalPath, subjectA()), journalA);

    QList<QList<QVariant> > progress;
    const QList<QVariant> f = runOverTheBus(rig, QStringLiteral("purge"), purgeParameters(subjectA(), 3, "AB"), &progress);
    QCOMPARE(f.size(), 6);
    QCOMPARE(f.at(0).toString(), QStringLiteral("purge"));
    QVERIFY2(f.at(1).toBool(), qPrintable(f.at(2).toString() + " " + f.at(3).toString()));
    const QJsonObject d = detailsOf(f);
    QCOMPARE(d.value("removed").toObject(), e.value("store_rows").toObject());        // estimate equals result
    QCOMPARE(d.value("journal_removed").toObject(), e.value("journal_entries").toObject());
    QCOMPARE(d.value("removed_total").toDouble(), e.value("total_rows").toDouble());
    QVERIFY(d.value("store_part_done").toBool());
    QVERIFY(d.contains("bytes_freed") && d.contains("journal_bytes_freed"));
    // Neither the label nor the typed text travel back.
    QVERIFY(!f.at(5).toString().contains(QStringLiteral("AB")));
    QStringList phases;
    for (int i = 0; i < progress.size(); ++i)
        if (phases.isEmpty() || phases.last() != progress.at(i).at(1).toString())
            phases << progress.at(i).at(1).toString();
    QCOMPARE(phases.first(), QStringLiteral("checking-space"));
    QVERIFY(phases.contains(QStringLiteral("purging")));
    QCOMPARE(phases.last(), QStringLiteral("done"));
    QCOMPARE(runtime.lifecycle()->state(), ServiceState::Running);      // monitoring goes on
    stopAndWait(runtime);
    QCOMPARE(recordsOf(storePath, subjectA()), qint64(40));
    QCOMPARE(recordsOf(storePath, subjectB()), qint64(1540));           // the other Subject is untouched
    const QJsonObject j = d.value("journal_removed").toObject();
    QCOMPARE(journalEntriesOf(journalPath, subjectA()),
             journalA - qint64(j.value("alert").toDouble() + j.value("threshold").toDouble() + j.value("silent").toDouble()));
    QCOMPARE(journalEntriesOf(journalPath, subjectB()), journalB);      // the other Subject's entries stay
}

// the journal part fails (here the journal is opened read-only), the store part is done and
// says so; the same request again removes only the journal entries; a third run removes nothing.
void TstDaemon::aPurgeWhoseJournalPartFailedCanBeRepeated()
{
    if (!haveBus())
        QSKIP("no session bus");
    RuntimeRig rig(QStringLiteral("-prepeat"));
    writePurgeConfig(rig);
    rig.options.compactionFreeSpace = [](const QString &) { return qint64(1) << 40; };
    QSharedPointer<bool> failJournal(new bool(true));
    rig.options.purgeJournalOpener = [failJournal](Journal &j, const QString &path) {
        return *failJournal ? j.openReadOnly(path) : j.open(path);
    };
    const QString storePath = rig.dir.path() + "/store.db";
    const QString journalPath = rig.dir.path() + "/journal.db";
    QVERIFY(seedSubject(storePath, subjectA(), 800, 20));
    QVERIFY(seedJournalOf(journalPath, subjectA()));
    ServiceRuntime runtime(rig.options, rig.platform);
    QCOMPARE(runtime.begin(), -1);
    const qint64 journalA = journalEntriesOf(journalPath, subjectA());     // after the engine's start entries
    const QJsonObject p = purgeParameters(subjectA(), 3, "AB");

    QList<QVariant> f = runOverTheBus(rig, QStringLiteral("purge"), p);
    QCOMPARE(f.size(), 6);
    QVERIFY(!f.at(1).toBool());
    QCOMPARE(f.at(2).toString(), QStringLiteral("io"));
    QJsonObject d = detailsOf(f);
    QVERIFY(d.value("store_part_done").toBool());
    QCOMPARE(d.value("removed").toObject().value("record").toDouble(), 800.0);
    QCOMPARE(d.value("journal_removed").toObject().value("alert").toDouble(), 0.0);
    QCOMPARE(recordsOf(storePath, subjectA()), qint64(20));
    QCOMPARE(journalEntriesOf(journalPath, subjectA()), journalA);     // the journal is unchanged

    *failJournal = false;
    f = runOverTheBus(rig, QStringLiteral("purge"), p);
    QCOMPARE(f.size(), 6);
    QVERIFY2(f.at(1).toBool(), qPrintable(f.at(2).toString() + " " + f.at(3).toString()));
    d = detailsOf(f);
    QCOMPARE(d.value("removed").toObject().value("record").toDouble(), 0.0);
    const double gone = d.value("journal_removed").toObject().value("alert").toDouble()
             + d.value("journal_removed").toObject().value("threshold").toDouble()
             + d.value("journal_removed").toObject().value("silent").toDouble();
    QVERIFY(gone >= 3.0);
    QCOMPARE(journalEntriesOf(journalPath, subjectA()), journalA - qint64(gone));

    f = runOverTheBus(rig, QStringLiteral("purge"), p);
    QVERIFY(f.size() == 6 && f.at(1).toBool());
    QCOMPARE(detailsOf(f).value("removed_total").toDouble(), 0.0);
    QCOMPARE(runtime.lifecycle()->state(), ServiceState::Running);
    stopAndWait(runtime);
}

// REQ-STO-16 as for compaction: short space refused with the missing bytes, unknown space refused with
// no override; nothing changes either way.
void TstDaemon::aPurgeIsRefusedWhenTheSpaceIsShortOrUnknown()
{
    if (!haveBus())
        QSKIP("no session bus");
    RuntimeRig rig(QStringLiteral("-pspace"));
    writePurgeConfig(rig);
    QSharedPointer<qint64> free(new qint64(1000));
    rig.options.compactionFreeSpace = [free](const QString &) { return *free; };
    const QString storePath = rig.dir.path() + "/store.db";
    const QString journalPath = rig.dir.path() + "/journal.db";
    QVERIFY(seedSubject(storePath, subjectA(), 600, 20));
    QVERIFY(seedJournalOf(journalPath, subjectA()));
    ServiceRuntime runtime(rig.options, rig.platform);
    QCOMPARE(runtime.begin(), -1);
    const qint64 journalA = journalEntriesOf(journalPath, subjectA());     // after the engine's start entries
    const QJsonObject p = purgeParameters(subjectA(), 3, "AB");

    QList<QVariant> f = runOverTheBus(rig, QStringLiteral("purge"), p);
    QCOMPARE(f.size(), 6);
    QVERIFY(!f.at(1).toBool());
    QCOMPARE(f.at(2).toString(), QStringLiteral("insufficient-space"));
    QJsonObject d = detailsOf(f);
    QCOMPARE(d.value("space").toString(), QStringLiteral("short"));
    QVERIFY(d.value("missing_bytes").toDouble() > 0);
    QCOMPARE(d.value("missing_bytes").toDouble(), d.value("needed_bytes").toDouble() - 1000.0);
    QVERIFY(!d.value("store_part_done").toBool());

    *free = -1;
    f = runOverTheBus(rig, QStringLiteral("purge"), p);
    QCOMPARE(f.size(), 6);
    QCOMPARE(f.at(2).toString(), QStringLiteral("free-space-unknown"));
    QCOMPARE(detailsOf(f).value("space").toString(), QStringLiteral("unknown"));
    // The estimate reports the same verdict.
    f = runOverTheBus(rig, QStringLiteral("purge-estimate"), purgeParameters(subjectA(), 3, QString(), false));
    QVERIFY(f.size() == 6 && f.at(1).toBool());
    QCOMPARE(detailsOf(f).value("space").toString(), QStringLiteral("unknown"));
    QCOMPARE(runtime.lifecycle()->state(), ServiceState::Running);
    stopAndWait(runtime);
    QCOMPARE(recordsOf(storePath, subjectA()), qint64(620));
    QCOMPARE(journalEntriesOf(journalPath, subjectA()), journalA);
}

// REQ-STO-15: while a purge runs the service answers on the bus, and the reading queue's high-water mark
// (the event the scheduler raises when the store stays busy too long) stops the purge cleanly, with
// nothing changed. The free-space question is held open (in the worker) until the main thread has
// raised the event, so the timing is fixed.
void TstDaemon::aPurgeStopsAtTheQueueHighWaterMarkWhileTheServiceAnswers()
{
    if (!haveBus())
        QSKIP("no session bus");
    RuntimeRig rig(QStringLiteral("-phigh"));
    writePurgeConfig(rig);
    QSharedPointer<QAtomicInt> release(new QAtomicInt(0));
    QSharedPointer<QAtomicInt> asked(new QAtomicInt(0));
    rig.options.compactionFreeSpace = [release, asked](const QString &) {
        asked->store(1);
        for (int i = 0; i < 500 && !release->load(); ++i)
            QThread::msleep(10);
        return qint64(1) << 40;
    };
    const QString storePath = rig.dir.path() + "/store.db";
    const QString journalPath = rig.dir.path() + "/journal.db";
    QVERIFY(seedSubject(storePath, subjectA(), 600, 20));
    QVERIFY(seedJournalOf(journalPath, subjectA()));
    ServiceRuntime runtime(rig.options, rig.platform);
    QCOMPARE(runtime.begin(), -1);
    const qint64 journalA = journalEntriesOf(journalPath, subjectA());     // after the engine's start entries
    ServiceClient client(rig.options.identity.busName, rig.options.identity.objectPath, clientBus());
    QSignalSpy finished(&client, SIGNAL(maintenanceFinished(QString,bool,QString,QString,QString,QString)));
    QSignalSpy status(&client, SIGNAL(statusAnswered(bool,QString)));
    QCOMPARE(startOf(runtime, QStringLiteral("purge"), purgeParameters(subjectA(), 3, "AB")), QString());
    for (int i = 0; i < 200 && !asked->load(); ++i)
        QTest::qWait(10);
    QVERIFY(asked->load());
    // A second job is refused while this one runs.
    QCOMPARE(startOf(runtime, QStringLiteral("compaction"), QJsonObject()), QStringLiteral("maintenance-running"));
    client.statusJsonAsync();
    QVERIFY(status.wait(5000) || !status.isEmpty());
    QVERIFY(status.first().at(0).toBool());                             // the service answers meanwhile
    runtime.events()->readingQueueHighWater(500);
    release->store(1);
    for (int i = 0; i < 300 && finished.isEmpty(); ++i)
        QTest::qWait(20);
    QCOMPARE(finished.size(), 1);
    QVERIFY(!finished.first().at(1).toBool());
    QCOMPARE(finished.first().at(2).toString(), QStringLiteral("cancelled"));
    QCOMPARE(finished.first().at(4).toString(), QStringLiteral("queue-high-water"));
    QVERIFY(!detailsOf(finished.first()).value("store_part_done").toBool());
    QCOMPARE(runtime.lifecycle()->state(), ServiceState::Running);
    stopAndWait(runtime);
    QCOMPARE(recordsOf(storePath, subjectA()), qint64(620));
    QCOMPARE(journalEntriesOf(journalPath, subjectA()), journalA);
}

void TstDaemon::thePurgeDetailsHaveTheirKeys()
{
    PurgeEstimate e;
    e.subject = subjectA();
    e.months = 6;
    e.cutoff = UtcTime::fromMSecsSinceEpoch(1768000000000LL);
    e.records = 10; e.marks = 1; e.quarantine = 2;
    e.journal.insert(QStringLiteral("alert"), 4);
    e.bytes = 4000; e.storeBytes = 50000; e.rebuildNeedBytes = 48000; e.freeBytes = 40000;
    e.space = CompactionEstimate::SpaceShort;
    const QJsonObject o = purgeEstimateDetails(e);
    QCOMPARE(o.keys(), QStringList() << "age_months" << "bytes_estimated" << "cutoff" << "free_bytes"
             << "journal_bytes_estimated" << "journal_entries" << "journal_file_bytes" << "journal_free_bytes"
             << "journal_rebuild_need_bytes" << "missing_bytes" << "needed_bytes" << "rebuild_need_bytes"
             << "shared_directory" << "space" << "store_bytes" << "store_rows" << "subject" << "total_bytes_estimated"
             << "total_rows");
    QCOMPARE(o.value("store_rows").toObject().keys(), QStringList() << "ingest" << "mark" << "quarantine" << "record" << "sensor");
    QCOMPARE(o.value("journal_entries").toObject().keys(), QStringList() << "alert" << "silent" << "threshold");
    QCOMPARE(o.value("store_rows").toObject().value("sensor").toDouble(), 0.0);
    QCOMPARE(o.value("total_rows").toDouble(), 17.0);
    QCOMPARE(o.value("space").toString(), QStringLiteral("short"));
}

QTEST_GUILESS_MAIN(TstDaemon)
#include "tst_daemon.moc"
