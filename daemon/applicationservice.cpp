// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Michele Brami
#include "applicationservice.h"

#include "daemon/commonports.h"
#include "daemon/serviceprocess.h"

#ifdef LM_PHONE_PORTS
#include "daemon/phoneports.h"
#include "daemon/devlog.h"
#else
#include "daemon/hostports.h"
#endif

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QStandardPaths>

#if defined(LIBREMON_DEVSOURCE) && defined(LM_PHONE_PORTS)
#include <QHash>
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace lm {

#if defined(LIBREMON_DEVSOURCE) && defined(LM_PHONE_PORTS)
namespace {

// DEVELOPMENT BUILDS ONLY, on a device (the emulator or a test phone), and only together with the
// simulator address of `dev-base-url` (below): the simulator's FAKE account read from the file
// `dev-credentials` of the development package's own data directory ("username=..." and "password=..."
// lines, the format of the PC host's --credentials-file), instead of the phone's secret store. Why: a
// development package must not write into the secret collection or the configuration of the release
// identity, and the provisioning mode writes to the release identity (applicationprovision.cpp); with
// this file the emulator run needs neither. The file must be a regular file of this user with no
// permission for others (mode 0600), as on the PC. Sessions are kept in memory only. Release builds
// have neither this class nor the file name.
class DevFileSecrets : public SecretStore
{
public:
    explicit DevFileSecrets(const QString &path) : m_path(path) {}
    void load(const QString &connectionId, const LoadCallback &done)
    {
        SecretLoad r;
        struct stat st;
        QFile f(m_path);
        if (::lstat(QFile::encodeName(m_path).constData(), &st) != 0) {
            r.kind = SecretLoad::Missing;
        } else if (!S_ISREG(st.st_mode) || st.st_uid != ::getuid() || (st.st_mode & (S_IRWXG | S_IRWXO)) != 0
                   || !f.open(QIODevice::ReadOnly)) {
            r.kind = SecretLoad::Error;
        } else {
            const QList<QByteArray> lines = f.readAll().split('\n');
            for (int i = 0; i < lines.size(); ++i) {
                const QString line = QString::fromUtf8(lines.at(i)).trimmed();
                if (line.startsWith(QLatin1String("username=")))
                    r.credentials.username = line.mid(9);
                else if (line.startsWith(QLatin1String("password=")))
                    r.credentials.password = line.mid(9);
            }
            r.kind = r.credentials.username.isEmpty() || r.credentials.password.isEmpty() ? SecretLoad::Error
                                                                                          : SecretLoad::Ok;
            r.session = m_session.value(connectionId);
            r.expiresAt = m_expires.value(connectionId);
        }
        done(r);
    }
    void saveSession(const QString &connectionId, const QString &session, const UtcTime &expiresAt)
    {
        m_session.insert(connectionId, session);
        m_expires.insert(connectionId, expiresAt);
    }
    void clearSession(const QString &connectionId)
    {
        m_session.remove(connectionId);
        m_expires.remove(connectionId);
    }

private:
    QString m_path;
    QHash<QString, QString> m_session;
    QHash<QString, UtcTime> m_expires;
};

} // namespace
#endif

int runApplicationService()
{
    // The data directory is private to the application under Sailjail; the configuration file
    // is written by the user interface and may not exist yet.
    const QString dataDir = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation);
    const QString configDir = QStandardPaths::writableLocation(QStandardPaths::AppConfigLocation);
    if (dataDir.isEmpty())
        return ServiceExitOpenFailed;

    RuntimeOptions options;
    options.identity = ServiceIdentity::forThisApplication();
    options.core.dataDirectory = dataDir;
    options.core.configPath = configDir + QStringLiteral("/config.json");
    // DEVELOPMENT BUILDS ONLY (flag devsource, REQ-SIM-06; absent from a release package, which
    // has neither this code nor the file name). A development package on the emulator or a test phone
    // is pointed at the simulator on the PC by one line in the file `dev-base-url` of its OWN data
    // directory (the development identity's, never the release one's). Only a plain `http://` address
    // is taken: the real source is reached over https only, so this switch cannot lead to it, and a
    // file with anything else is ignored. Without the file nothing changes. Added for,
    // item 7 (alarms from readings on the emulator).
    QString devBaseUrl;
#ifdef LIBREMON_DEVSOURCE
    {
        QFile f(dataDir + QStringLiteral("/dev-base-url"));
        if (f.open(QIODevice::ReadOnly | QIODevice::Text)) {
            const QString line = QString::fromUtf8(f.readLine(512)).trimmed();
            if (line.startsWith(QLatin1String("http://")) && !line.contains(QLatin1Char(' ')))
                devBaseUrl = line;
        }
    }
#endif
    // Adapters are this very binary in adapter mode, started by absolute path (S2: Sailjail
    // starts only the exact Exec line, so the application starts this mode itself).
    options.core.adapterLaunch = [devBaseUrl](const QString &adapterId) {
        AdapterLaunch l;
        l.program = QCoreApplication::applicationFilePath();
        l.arguments << QStringLiteral("--adapter") << adapterId;
        if (!devBaseUrl.isEmpty())
            l.arguments << QStringLiteral("--base-url") << devBaseUrl;
        return l;
    };
    // The package's version from the build define of common.pri ( "0.1-2"); it goes into
    // the diagnostic report, Status() and the version stamp check. QCoreApplication::applicationVersion() is never set
    // in this application, so it was empty before.
    options.appVersion = QStringLiteral(LM_APP_VERSION);
    // The package's own folder holds the compiled translations and the notification icons.
    // The package's folder under /usr/share. Under Sailjail the application name is the desktop file's
    // (`libre-monitor`, `libre-monitor-dev`); a plain run has the executable's (`harbour-...`). The package
    // name is the application name with the `harbour-` prefix (found on the phone: the first version of
    // this line fell back to the release package's folder, which has no icons).
    const QString appName = QCoreApplication::applicationName();
    const QString packageName = appName.isEmpty() ? QStringLiteral("harbour-libre-monitor")
        : (appName.startsWith(QLatin1String("harbour-")) ? appName : QStringLiteral("harbour-") + appName);
    const QString shareDir = QStringLiteral("/usr/share/") + packageName;
    options.core.translationsDir = shareDir + QStringLiteral("/translations");
    options.core.translationBase = packageName;    // the package's own name (a development package has another)
    options.core.iconDirectory = shareDir + QStringLiteral("/icons/");

    SystemServiceClock clock;
    QrandRandom random;
    JsonFileSettings settings(dataDir + QStringLiteral("/settings.json"));
    DBusNameOwner names(options.identity.busName);

    ServicePlatform platform;
    platform.clock = &clock;
    platform.random = &random;
    platform.connectionSettings = &settings;
    platform.names = &names;

#ifdef LM_PHONE_PORTS
    KeepaliveWakeSource wake;
    SecretsApproval approval;
    // The alert notifications: the presenter decides what to post, the
    // backend posts it; the hints are the catalogue's (core/service/notificationcatalog.cpp).
    SubjectLabelBook labels;
    NemoAlertBackend nemoBackend(options.identity.busName, options.identity.objectPath, options.identity.interfaceName);
#ifdef LIBREMON_DEVSOURCE
    // Development builds keep a log of what the notifier asks of the phone (numbers and hint words only).
    LoggingNoteBackend alertBackend(&nemoBackend, dataDir + QStringLiteral("/notify.log"));
#else
    NemoAlertBackend &alertBackend = nemoBackend;
#endif
    AlertNotificationPresenter alertNotifier(&alertBackend, &settings, &labels, &clock, NotificationCatalog(options.core.iconDirectory));
    platform.alerts = &alertNotifier;
    platform.presenter = &alertNotifier;
    platform.labels = &labels;
    // The resident notice says the state of the monitor and the alerts; its words are composed from the
    // presenter's texts and the facts the service core supplies.
    ResidentNoticeComposer residentComposer(&alertNotifier);
    platform.resident = &residentComposer;
    NemoNotifier notifier(&settings, options.identity.busName, options.identity.objectPath,
                          options.identity.interfaceName, &residentComposer,
                          NotificationCatalog(options.core.iconDirectory));
    NemoDoNotDisturbSource dnd;
    platform.dnd = &dnd;
    SailfishSecretStore phoneSecrets;
    SailfishCredentialWriter credentials;       // only to remove the probe secret
#ifdef LIBREMON_DEVSOURCE
    // The simulator's fake account from a file, only when the simulator address is set too (above), so
    // the file can never be used against the real source.
    DevFileSecrets devSecrets(dataDir + QStringLiteral("/dev-credentials"));
    SecretStore &secrets = (!devBaseUrl.isEmpty() && QFile::exists(dataDir + QStringLiteral("/dev-credentials")))
        ? static_cast<SecretStore &>(devSecrets) : static_cast<SecretStore &>(phoneSecrets);
#else
    SailfishSecretStore &secrets = phoneSecrets;
#endif
#else
    TimerWakeSource wake;
    ConsoleNotifier notifier;
    ImmediateApproval approval;
    MemorySecretStore secrets;
    CredentialWriter &credentials = secrets;
#endif
    platform.serviceSettings = &settings;
#ifdef LM_PHONE_PORTS
    // the package's version stamp in its own folder (a development
    // package has its own folder and stamp), checked at each heartbeat; a stop for an upgrade starts
    // the new binary by one activation of this identity's name. Device builds only: a binary run on
    // the PC has no package, and a missing stamp would stop it at its first heartbeat.
    FileVersionStamp stamp(shareDir + QStringLiteral("/version"));
    DBusSelfActivator activator(options.identity.busName, options.identity.objectPath);
    platform.stamp = &stamp;
    platform.activator = &activator;
#endif
    platform.wake = &wake;
    platform.notifier = &notifier;
    platform.approval = &approval;
    platform.secrets = &secrets;
    platform.credentials = &credentials;
    return runServiceProcess(options, platform);
}

} // namespace lm
