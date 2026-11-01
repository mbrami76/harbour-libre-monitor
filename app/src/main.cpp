// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Michele Brami

// The one binary of the application, in three modes (
// REQ-ARC-07, REQ-ADP-15):
//
//   (no mode argument)        the user interface
//   --service                 the headless monitoring service, started by D-Bus activation
//   --adapter <id>            an adapter process, started by the service
//   --provision [options]     set up or remove a connection from a terminal
//
// The mode is parsed FIRST and nothing else happens before it is known: adapter mode must not
// open the store, read the configuration file or touch the secret store, so no Qt GUI object
// and no library that does those things is created before this decision.

#ifdef QT_QML_DEBUG
#include <QtQuick>
#endif

#include <sailfishapp.h>

#include "daemon/adaptermode.h"
#include "daemon/applicationprovision.h"
#include "daemon/applicationservice.h"
#include "daemon/processmode.h"
#include "daemon/uiguard.h"
#include "plotitem.h"
#ifdef LM_PHONE_PORTS
#include "daemon/phoneports.h"
#endif
#include "real/realaccountbackend.h"
#include "real/realalertsbackend.h"
#include "real/realbackend.h"
#include "real/realmaintenancebackend.h"
#include "real/servicelink.h"
#include "ui/accountbackend.h"
#include "ui/accountsetup.h"
#include "ui/alertmodel.h"
#include "ui/configsettings.h"
#include "ui/configstore.h"
#include "ui/thresholdeditor.h"
#include "service/notificationtexts.h"
#include "ui/fakealertsbackend.h"
#include "ui/appsettings.h"
#include "ui/fakeaccountbackend.h"
#include "ui/backend.h"
#include "ui/fakebackend.h"
#include "ui/fakemaintenancebackend.h"
#include "ui/storagecontroller.h"
#include "ui/purgecontroller.h"
#include "ui/reportcontroller.h"
#include "ui/monitoringinfo.h"
#include "ui/plotcontroller.h"
#include "ui/subjectmodel.h"

#include <QCoreApplication>
#include <QDir>
#include <QGuiApplication>
#include <QQmlContext>
#include <QQuickItem>
#include <QtQml>
#include <QQuickView>
#include <QScopedPointer>
#include <QStandardPaths>
#include <QTimer>

#include <cstdio>

namespace {

// What a release build shows until the real backend (the monitoring service over D-Bus and the
// store reader) is written: no Subjects and a service that has not answered. It exists so the
// pages run, honestly empty, in a build without the development flag; it is never a source of
// readings. The scripted source (ui/fakebackend.h) is compiled only with `devsource`.
class EmptyBackend : public lm::ui::Backend
{
public:
    QList<lm::ui::SubjectSnapshot> subjects() const { return QList<lm::ui::SubjectSnapshot>(); }
    lm::ui::ServiceStatus serviceStatus() const { return lm::ui::ServiceStatus::Connecting; }
    void retry(const QString &) {}
    void reenable(const QString &) {}
    void startMonitoring() {}
    void askAgain() {}
};

// The account page's backend in a build without the development flag, until the real one (the
// service's login test over D-Bus, the secret store, the configuration file) is written: both
// calls answer at once that the function is not available, so the page says so plainly instead of
// pretending to work.
class UnavailableAccountBackend : public lm::ui::AccountBackend
{
public:
    void testLogin(const QString &, const QString &)
    {
        QTimer::singleShot(0, this, [this]() {
            lm::ui::LoginTestResult r;
            r.errorKey = QStringLiteral("not-available");
            emit loginTested(r);
        });
    }
    void save(const lm::ui::SaveRequest &)
    {
        QTimer::singleShot(0, this, [this]() { emit saved(false, QStringLiteral("not-available")); });
    }
};

} // namespace

int main(int argc, char *argv[])
{
    QStringList arguments;
    for (int i = 0; i < argc; ++i)
        arguments << QString::fromLocal8Bit(argv[i]);
    const lm::ProcessMode mode = lm::parseProcessMode(arguments);

    switch (mode.kind) {
    case lm::ProcessMode::Invalid:
        // A fixed text: never the offending argument, which could be anything.
        std::fprintf(stderr, "harbour-libre-monitor: %s\n", qPrintable(mode.error));
        return 64;

    case lm::ProcessMode::Adapter: {
        // Only the core application object: no GUI, no store, no configuration, no secrets.
        QCoreApplication app(argc, argv);
        return lm::runAdapterMode(mode.adapterId, mode.adapterOptions);
    }

    case lm::ProcessMode::Provision: {
        // A terminal tool: no window and no display needed, only the core application object
        // (it runs over SSH under Sailjail). Like adapter mode it never opens the store.
        QCoreApplication app(argc, argv);
        return lm::runApplicationProvisioning(mode.provisionOptions);
    }

    case lm::ProcessMode::Service: {
        // The same application object the interface uses, so the tooling that starts the
        // process sees what it expects; it is never given a window.
        QScopedPointer<QGuiApplication> app(SailfishApp::application(argc, argv));
        return lm::runApplicationService();
    }

    case lm::ProcessMode::Ui:
        break;
    }

    // User-interface mode. A tap on the icon while the headless service runs starts a process
    // of its own (X-Nemo-Single-Instance=no in the desktop file), so a second tap
    // would start a second UI: the guard lets only one UI live and brings its window forward.
    QScopedPointer<QGuiApplication> app(SailfishApp::application(argc, argv));
    const QString dataDir = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation);
    QDir().mkpath(dataDir);
    lm::UiGuard guard(dataDir);
    if (guard.acquire() != lm::UiGuard::First)
        return 0;       // another UI runs (and was asked to come forward)

    // The objects the QML pages read. The pages hold no logic: the
    // models decide, the pages present (REQ-UI-07).
    lm::ui::AppSettings settings;
    settings.load(dataDir + QStringLiteral("/ui-settings.json"));
    const QString configPath = QStandardPaths::writableLocation(QStandardPaths::AppConfigLocation) + QStringLiteral("/config.json");
    // The language the user chose in the settings: written into the
    // configuration file so that the service writes the notifications in it, and taken by the window here,
    // at its start, so the pages and the alerts speak the same language. Empty: the phone's language,
    // which SailfishApp has already loaded. Qt 5.6 cannot translate a running window again, so a change
    // takes effect at the next start (the settings page says so). The same loader as the service's
    // (NotificationTranslator: English underneath, the language on top), the same files, and the package's
    // own file name, derived as the service derives it (daemon/applicationservice.cpp), so a development
    // package finds its own files.
    QString startLanguage;
    {
        lm::ui::ConfigStore startConfig;
        if (startConfig.load(configPath))
            startLanguage = startConfig.language();
    }
    lm::NotificationTranslator windowTranslator;
    if (!startLanguage.isEmpty()) {
        const QString appName = QCoreApplication::applicationName();
        const QString packageName = appName.isEmpty() ? QStringLiteral("harbour-libre-monitor")
            : (appName.startsWith(QLatin1String("harbour-")) ? appName : QStringLiteral("harbour-") + appName);
        windowTranslator.load(SailfishApp::pathTo(QStringLiteral("translations")).toLocalFile(), packageName, startLanguage);
    }
    const QLocale uiLocale = startLanguage.isEmpty() ? QLocale::system() : QLocale(startLanguage);
    // The data source. A development build (`devsource`) runs on scripted data with no service at
    // all (REQ-SIM-06); every other build uses the real source: the monitoring service over D-Bus,
    // The store read-only and the configuration file. There is no runtime switch between them.
    QObject *fake = 0;       // the scripted source, only in a development build
    lm::ui::Backend *backend = 0;
    lm::ui::AlertsBackend *alertsBackend = 0;
    QScopedPointer<lm::real::ServiceLink> serviceLink;
    QScopedPointer<lm::real::RealBackend> realBackend;
    QScopedPointer<lm::real::RealAlertsBackend> realAlerts;
    QScopedPointer<lm::ui::MaintenanceBackend> maintenanceBackend;
    bool useService = true;
#ifdef LIBREMON_DEVSOURCE
    lm::ui::FakeBackend fakeBackendImpl;
    lm::ui::FakeAlertsBackend fakeAlerts(&fakeBackendImpl);
    // DEVELOPMENT BUILDS ONLY: with
    // LM_DEV_SOURCE=service the pages run on the development package's own monitoring service, which
    // fetches from the simulator on the PC (the development-only files dev-base-url and
    // dev-credentials), so the alerts, silent mode and Do not disturb can be seen on the emulator with
    // real notifications. Without it a development build runs on the scripted source. A release build
    // has neither the variable nor the scripted source: it always uses the service.
    useService = qgetenv("LM_DEV_SOURCE") == "service";
    if (!useService) {
        backend = &fakeBackendImpl;
        alertsBackend = &fakeAlerts;
        fake = &fakeBackendImpl;
        lm::ui::FakeMaintenanceBackend *fakeMaintenance = new lm::ui::FakeMaintenanceBackend;
        // LM_DEV_STORAGE: the scripted space check of the
        // storage page, "short", "unknown" or "low".
        fakeMaintenance->setSpaceScript(QString::fromLocal8Bit(qgetenv("LM_DEV_STORAGE")));
        // LM_DEV_PURGE: the scripted purge, "short", "unknown",
        // "nothing", "repeat" or "slow".
        fakeMaintenance->setPurgeScript(QString::fromLocal8Bit(qgetenv("LM_DEV_PURGE")));
        maintenanceBackend.reset(fakeMaintenance);
        // LM_DEV_SERVICE (development builds only): the scripted service is "older" or "newer" than this
        // window (the states), so the strip can be looked at on the emulator.
        const QByteArray devService = qgetenv("LM_DEV_SERVICE");
        if (devService == "older")
            fakeBackendImpl.setServiceStatusOverride(lm::ui::ServiceStatus::ServiceOlder);
        else if (devService == "newer")
            fakeBackendImpl.setServiceStatusOverride(lm::ui::ServiceStatus::ServiceNewer);
    }
    lm::ui::FakeAccountBackend accountBackend;
    lm::ui::AccountBackend *accountBackendPtr = &accountBackend;
#endif
    if (useService) {
        serviceLink.reset(new lm::real::ServiceLink);
        realBackend.reset(new lm::real::RealBackend(serviceLink.data(), configPath, dataDir + QStringLiteral("/store.db"),
                                                    dataDir + QStringLiteral("/journal.db")));
        realAlerts.reset(new lm::real::RealAlertsBackend(serviceLink.data()));
        // Storage and maintenance, the diagnostic report: the work in the
        // service, the facts read here read-only.
        maintenanceBackend.reset(new lm::real::RealMaintenanceBackend(serviceLink.data(), dataDir + QStringLiteral("/store.db"),
                                                                      dataDir + QStringLiteral("/journal.db")));
        backend = realBackend.data();
        alertsBackend = realAlerts.data();
    }
#ifndef LIBREMON_DEVSOURCE
#ifdef LM_PHONE_PORTS
    // The secret store of the phone, the same writer the provisioning mode uses (one implementation
    // of "store a login"), and the D-Bus port to the service.
    lm::SailfishCredentialWriter credentialWriter;
    QScopedPointer<lm::real::AccountServicePort> servicePort(lm::real::makeDBusAccountServicePort());
    lm::real::RealAccountBackend accountBackend(&credentialWriter, servicePort.data(), configPath);
    // After a successful save the configuration changed: read it again.
    lm::real::RealBackend *reloadTarget = realBackend.data();
    QObject::connect(&accountBackend, &lm::ui::AccountBackend::saved, [reloadTarget](bool ok, const QString &) {
        if (ok)
            reloadTarget->reloadConfiguration();
    });
    lm::ui::AccountBackend *accountBackendPtr = &accountBackend;
#else
    UnavailableAccountBackend accountBackend;
    lm::ui::AccountBackend *accountBackendPtr = &accountBackend;
#endif
#endif
    lm::ui::SubjectModel subjects;
    subjects.setLocale(uiLocale);
    subjects.setDisplayUnit(settings.displayUnit());
    subjects.setBackend(backend);
    subjects.setAlertsBackend(alertsBackend);
    // The alerts list, the alerts of a patient, silent mode and the actions.
    lm::ui::AlertModel alerts;
    alerts.setBackends(backend, alertsBackend);
    lm::ui::AccountSetup account;
    account.setBackend(accountBackendPtr);
    // The settings the service reads from the configuration file (language, the value in alerts, the
    // account's label, the patients' labels, the alert times) and the thresholds page.
    lm::ui::ConfigSettings configSettings;
    configSettings.load(configPath, startLanguage);
    lm::ui::ThresholdEditor thresholds;
    thresholds.setBackends(backend, alertsBackend);
    thresholds.setLocale(uiLocale);
    thresholds.setDisplayUnit(settings.displayUnit());
    QObject::connect(accountBackendPtr, &lm::ui::AccountBackend::saved, [&configSettings, configPath, startLanguage](bool ok, const QString &) {
        if (ok)
            configSettings.load(configPath, startLanguage);       // the new account and patients
    });
    lm::ui::MonitoringInfo monitoring;
    monitoring.setBackend(backend);
    // The storage page: the facts and the estimate, and compaction in the service. Nothing
    // is exported or imported, so the page uses no folder.
    lm::ui::StorageController storage;
    storage.setBackend(maintenanceBackend.data());
    // The purge page: one Subject at a time, the service's estimate, the
    // confirmation and the typed label; the work in the service.
    lm::ui::PurgeController purge;
    purge.setBackend(maintenanceBackend.data());
    // The diagnostic report dialog: the file goes to the Documents folder, the only use of the
    // permission `Documents` of the desktop file left (the packaging checks it). The developer's address
    // has one source for this dialog and About: the build define LM_DEVELOPER_CONTACT, which common.pri
    // takes from the e-mail address of the spec's Packager line (done from 0.1-3). The
    // empty fallback below only serves a build without common.pri; the dialog and About then show no address.
#ifndef LM_DEVELOPER_CONTACT
#define LM_DEVELOPER_CONTACT ""
#endif
    lm::ui::ReportController report;
    report.setFolder(QStandardPaths::writableLocation(QStandardPaths::DocumentsLocation));
    report.setDeveloperContact(QStringLiteral(LM_DEVELOPER_CONTACT));
    report.setBackend(maintenanceBackend.data());
    lm::ui::PlotController plot;
    plot.setBackend(backend);
    plot.setFeeds(settings.feeds());
    plot.setDisplayUnit(settings.displayUnit());
    QObject::connect(&settings, &lm::ui::AppSettings::changed, [&]() {
        subjects.setDisplayUnit(settings.displayUnit());
        plot.setFeeds(settings.feeds());
        plot.setDisplayUnit(settings.displayUnit());
        thresholds.setDisplayUnit(settings.displayUnit());
    });
    qmlRegisterType<lm::PlotItem>("LibreMonitor", 1, 0, "GlucosePlot");

    QScopedPointer<QQuickView> view(SailfishApp::createView());
    QQmlContext *ctx = view->rootContext();
    ctx->setContextProperty(QStringLiteral("subjects"), &subjects);
    ctx->setContextProperty(QStringLiteral("settings"), &settings);
    ctx->setContextProperty(QStringLiteral("monitoring"), &monitoring);
    ctx->setContextProperty(QStringLiteral("account"), &account);
    ctx->setContextProperty(QStringLiteral("plot"), &plot);
    ctx->setContextProperty(QStringLiteral("alerts"), &alerts);
    ctx->setContextProperty(QStringLiteral("configSettings"), &configSettings);
    ctx->setContextProperty(QStringLiteral("thresholds"), &thresholds);
    ctx->setContextProperty(QStringLiteral("storage"), &storage);
    ctx->setContextProperty(QStringLiteral("purge"), &purge);
    ctx->setContextProperty(QStringLiteral("report"), &report);
    // Only a development build has the scenario switch and the SIMULATED DATA bar (REQ-SIM-09).
#ifdef LIBREMON_DEVSOURCE
    ctx->setContextProperty(QStringLiteral("devsource"), true);
#else
    ctx->setContextProperty(QStringLiteral("devsource"), false);
#endif
    ctx->setContextProperty(QStringLiteral("fakeBackend"), fake);
    // The version of this build, for About and the first-run page: LM_APP_VERSION is the spec's
    // Version-Release ("1.0.0-1"), read by common.pri from the spec itself. There is no stage word.
#ifndef LM_APP_VERSION
#define LM_APP_VERSION ""
#endif
    ctx->setContextProperty(QStringLiteral("appVersion"), QStringLiteral(LM_APP_VERSION));
#ifdef LIBREMON_DEVSOURCE
    // DEVELOPMENT BUILDS ONLY: nobody can tap
    // on the emulator for the chat, so a page is chosen at start by two environment variables, and the
    // screenshots of the colour and contrast review are taken of it. LM_DEV_SCENARIO picks a scenario of
    // the scripted source; LM_DEV_PAGE names a page ("SubjectPage:<row>", "SettingsPage", "AboutPage",
    // "AccountPage", "FirstRunDialog", or "Cover" for the cover drawn inside a page). The code and the
    // variable names exist only with `devsource`: a release binary has neither (checked with `strings` when the release package is built).
    {
        bool ok = false;
        const int scenario = qgetenv("LM_DEV_SCENARIO").toInt(&ok);
        if (ok)
            fakeBackendImpl.setScenario(scenario);
        ctx->setContextProperty(QStringLiteral("devStartPage"), QString::fromLocal8Bit(qgetenv("LM_DEV_PAGE")));
        // LM_DEV_ACTION: what a tap would do, done once when the alerts are known, through
        // the same function the page's button calls: "acknowledge-first" (the first alert not acknowledged)
        // or "silent:<choice>" (the first patient's silent page, then that choice). The chat cannot tap
        // on the emulator; this lets it see the application's own path to the service.
        ctx->setContextProperty(QStringLiteral("devAction"), QString::fromLocal8Bit(qgetenv("LM_DEV_ACTION")));
    }
#else
    ctx->setContextProperty(QStringLiteral("devStartPage"), QString());
    ctx->setContextProperty(QStringLiteral("devAction"), QString());
#endif
    if (realBackend) {
        // A change of the configuration file from the settings or the thresholds page: the service is told
        // (ConfigChanged applies these keys without a restart), the pages read
        // the file again, and the alerts are asked again for the times now in force. A service that asks
        // for a restart (it never should for these keys) is stopped and started once, as after an account
        // change.
        lm::real::ServiceLink *link = serviceLink.data();
        lm::real::RealBackend *reload = realBackend.data();
        lm::real::RealAlertsBackend *again = realAlerts.data();
        QObject::connect(&configSettings, &lm::ui::ConfigSettings::written, [link, reload]() {
            link->configChanged();
            reload->reloadConfiguration();
        });
        QObject::connect(link, &lm::real::ServiceLink::configChangeAnswered, [link, again, &configSettings](bool ok, const QString &result) {
            configSettings.configApplied(ok, result);
            if (ok && result == QLatin1String("restart-needed")) {
                QMetaObject::Connection *once = new QMetaObject::Connection;
                *once = QObject::connect(link, &lm::real::ServiceLink::reachableChanged, [link, once](bool reachable) {
                    if (reachable)
                        return;
                    QObject::disconnect(*once);
                    delete once;
                    link->start();
                });
                link->stop();
            }
            again->refresh();
        });
        // The window's own version, compared with the service's Status().service.app_version: a service of
        // another build (an upgrade left the old process running) is shown as "older" with what to do
        // The window never restarts it by itself.
        realBackend->setOwnVersion(QStringLiteral(LM_APP_VERSION));
        // Opening the UI starts the service (REQ-UI-11), loads the picture and asks for the alerts.
        realBackend->begin();
        realAlerts->refresh();
        // A purge removed old readings and the Subject's old alert and threshold entries: load the
        // picture again from the store (a refresh only appends what is new) and ask for the alerts again.
        QObject::connect(&purge, &lm::ui::PurgeController::purgeFinished, [reload, again](const QString &) {
            reload->reloadConfiguration();
            again->refresh();
        });
    }
    // Back to the front: catch up on what the service signalled meanwhile (not polling: an event), and
    // land on the overview when an alert arrived while the window was away.
    lm::real::RealBackend *frontBackend = realBackend.data();
    lm::real::RealAlertsBackend *frontAlerts = realAlerts.data();
    QObject::connect(app.data(), &QGuiApplication::applicationStateChanged,
                     [frontBackend, frontAlerts, &alerts, &storage](Qt::ApplicationState st) {
        const bool active = st == Qt::ApplicationActive;
        if (active && frontBackend) {
            frontBackend->refreshAll();
            frontAlerts->refresh();
        }
        if (active)
            storage.refresh();          // the low-space line of the overview: one read, not a poll
        alerts.setApplicationActive(active);
    });
    view->setSource(SailfishApp::pathToMainQml());
#ifdef LIBREMON_DEVSOURCE
    // A page that fails to load shows only an empty window; in a development build say why.
    // QML errors name files and lines, never data (no names, values or credentials).
    if (view->status() == QQuickView::Error) {
        const QList<QQmlError> errors = view->errors();
        for (int i = 0; i < errors.size(); ++i)
            std::fprintf(stderr, "QML error: %s\n", qPrintable(errors.at(i).toString()));
    }
    // The other pages load only when opened, so a mistake in one would show up late. In a
    // development build compile each of them once now and report the errors.
    {
        const char *pages[] = { "SubjectPage", "FirstRunDialog", "SettingsPage", "AboutPage", "AccountPage",
                                "AlertsPage", "SilentPage", "SilentConfirmDialog", "ThresholdsPage",
                                "ThresholdChallengeDialog", "StoragePage", "DiagnosticReportDialog", "PurgePage" };
        for (unsigned i = 0; i < sizeof(pages) / sizeof(pages[0]); ++i) {
            QQmlComponent c(view->engine(), SailfishApp::pathTo(QStringLiteral("qml/pages/%1.qml").arg(QLatin1String(pages[i]))));
            if (c.isError())
                for (int k = 0; k < c.errors().size(); ++k)
                    std::fprintf(stderr, "QML error: %s\n", qPrintable(c.errors().at(k).toString()));
        }
    }
#endif
    view->showFullScreen();
    // ApplicationWindow.activate() raises the window.
    QObject::connect(&guard, &lm::UiGuard::raiseRequested, [&view]() {
        if (view->rootObject())
            QMetaObject::invokeMethod(view->rootObject(), "activate");
    });
    return app->exec();
}
