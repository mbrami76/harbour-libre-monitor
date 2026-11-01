// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Michele Brami

// Host-only development executable: the monitoring service on the PC, against the simulator
// (REQ-SIM-*, the service). It is the same runtime as the application's service mode
// (daemon/serviceprocess.h) with the stand-in ports of hostports.h. It exists only in host
// builds, which imply the flag `devsource`, so its options (a base URL for the simulated
// source, a credentials file, fast wake-ups) never reach a release or Harbour package.
//
//   lm-service --data-dir DIR [--config FILE] [--adapter-program PATH] [--base-url URL]
//              [--credentials-file FILE] [--probe-credentials-file FILE]
//              [--wake-frequencies S,S,...] [--heartbeat-s N] [--name-suffix TEXT] [--dump-journal]
//
//   --data-dir          store.db, journal.db and settings.json (default: ./lm-service-data)
//   --config            the connections file (default: DIR/config.json)
//   --adapter-program   the adapter host executable (default: libre-adapter-host next to this)
//   --base-url          the simulator, passed on to the adapter (SIMULATED SOURCE is printed)
//   --credentials-file  "username=" and "password=" lines for EVERY connection; refused unless
//                       it is a regular file owned by the user without group or other access
//   --probe-credentials-file  where a test puts the login that the set-up page would store under
//                       the secret name `probe`; same file rules. It stands in for the
//                       UI's writer: the service reads it for `Discover("probe")` and then
//                       DELETES it, as it removes the probe secret on the phone
//   --wake-frequencies  the wake-up frequencies in seconds, to make a test fast
//   --heartbeat-s       heartbeat interval in seconds (default 60)
//   --name-suffix       appended to the bus name, so tests do not meet a real service
//   --dump-journal      print the lifecycle entries of DIR/journal.db as JSON lines and exit
//   --dump-alerts       print the `alert` and `threshold` entries of DIR/journal.db and exit
//   --alert-scale N     divide every duration of the alert engine by N (persistence, reminders,
//                       re-alert, missing-data limit), so an integration run in real time takes
//                       minutes; LL and HH have nothing to scale and stay immediate
//
// Exit codes: 0 stop or second instance, 64 usage, 70 could not open, 71 no session bus.

#include "daemon/commonports.h"
#include "daemon/discovery.h"
#include "daemon/hostports.h"
#include "daemon/serviceprocess.h"
#include "store/journal.h"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>

#include <cstdio>
#include <sys/stat.h>
#include <unistd.h>

using namespace lm;

namespace {

int usage(const char *why)
{
    std::fprintf(stderr, "lm-service: %s\n", why);
    return 64;
}

// The same rule as the command-line program (cli/credentials.cpp): a regular file, owned by
// the user, with no permission for group or others.
bool readCredentialsFile(const QString &path, QString *user, QString *password)
{
    struct stat st;
    if (::lstat(QFile::encodeName(path).constData(), &st) != 0 || !S_ISREG(st.st_mode)
        || st.st_uid != ::getuid() || (st.st_mode & (S_IRWXG | S_IRWXO)) != 0)
        return false;
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly))
        return false;
    const QList<QByteArray> lines = f.readAll().split('\n');
    for (int i = 0; i < lines.size(); ++i) {
        const QString line = QString::fromUtf8(lines.at(i));
        if (line.startsWith(QLatin1String("username=")))
            *user = line.mid(9);
        else if (line.startsWith(QLatin1String("password=")))
            *password = line.mid(9);
    }
    return !user->isEmpty() && !password->isEmpty();
}

// The probe secret on the PC. The real one lives in Sailfish Secrets and is written by the
// user interface; here a test writes a file (synthetic login, user-only) and the service reads
// it when asked for the secret `probe` and deletes it when told to remove it. Every other
// secret is the in-memory store's. Host executable only, like the credentials file above.
class ProbeFileSecrets : public SecretStore, public CredentialWriter
{
public:
    ProbeFileSecrets(MemorySecretStore *inner, const QString &path) : m_inner(inner), m_path(path) {}

    void load(const QString &connectionId, const LoadCallback &done)
    {
        if (connectionId != probeConnectionId() || m_path.isEmpty()) {
            m_inner->load(connectionId, done);
            return;
        }
        SecretLoad l;
        QString user, password;
        if (!QFile::exists(m_path)) {
            l.kind = SecretLoad::Missing;
        } else if (readCredentialsFile(m_path, &user, &password)) {
            l.kind = SecretLoad::Ok;
            l.credentials.username = user;
            l.credentials.password = password;
        } else {
            l.kind = SecretLoad::Error;
        }
        done(l);
    }
    void saveSession(const QString &c, const QString &s, const UtcTime &e) { m_inner->saveSession(c, s, e); }
    void clearSession(const QString &c) { m_inner->clearSession(c); }

    bool store(const QString &c, const QString &u, const QString &p, QString *e) { return m_inner->store(c, u, p, e); }
    bool remove(const QString &connectionId, QString *errorKey)
    {
        if (connectionId == probeConnectionId() && !m_path.isEmpty())
            return !QFile::exists(m_path) || QFile::remove(m_path);
        return m_inner->remove(connectionId, errorKey);
    }

private:
    MemorySecretStore *m_inner;
    QString m_path;
};

int dumpJournal(const QString &dataDir)
{
    Journal journal;
    if (!journal.openReadOnly(dataDir + QStringLiteral("/journal.db")))
        return 70;
    const QVector<JournalEntry> all = journal.entries();
    for (int i = 0; i < all.size(); ++i) {
        const JournalEntry &e = all.at(i);
        if (e.kind != JournalKind::heartbeat() && e.kind != JournalKind::start() && e.kind != JournalKind::stop())
            continue;
        QJsonObject o;
        o.insert(QStringLiteral("kind"), e.kind);
        o.insert(QStringLiteral("wall_time"), e.wallTime.toIso());
        o.insert(QStringLiteral("boot_time_ms"), double(e.bootTimeMs));
        o.insert(QStringLiteral("monotonic_ms"), double(e.monotonicMs));
        o.insert(QStringLiteral("cause"), e.cause);
        o.insert(QStringLiteral("extra"), e.extra);
        std::printf("%s\n", QJsonDocument(o).toJson(QJsonDocument::Compact).constData());
    }
    return 0;
}

// The alert and threshold lines of the journal, for the integration run: the kind, the time and
// the extra keys (no glucose value, name or comment-free data beyond what the entry holds).
int dumpAlertEntries(const QString &dataDir)
{
    Journal journal;
    if (!journal.openReadOnly(dataDir + QStringLiteral("/journal.db")))
        return 70;
    const QVector<JournalEntry> all = journal.entries();
    for (int i = 0; i < all.size(); ++i) {
        const JournalEntry &e = all.at(i);
        if (e.kind != JournalKind::alert() && e.kind != JournalKind::threshold())
            continue;
        QJsonObject o;
        o.insert(QStringLiteral("kind"), e.kind);
        o.insert(QStringLiteral("wall_time"), e.wallTime.toIso());
        o.insert(QStringLiteral("extra"), e.extra);
        std::printf("%s\n", QJsonDocument(o).toJson(QJsonDocument::Compact).constData());
    }
    return 0;
}

} // namespace

int main(int argc, char *argv[])
{
    QCoreApplication app(argc, argv);

    QString dataDir = QStringLiteral("lm-service-data");
    QString configPath, adapterProgram, baseUrl, credentialsFile, probeCredentialsFile, suffix;
    QList<int> frequencies;
    int heartbeatS = 60;
    bool dump = false, dumpAlerts = false;
    int alertScale = 1;

    const QStringList args = app.arguments();
    for (int i = 1; i < args.size(); ++i) {
        const QString a = args.at(i);
        const bool more = i + 1 < args.size();
        if (a == QLatin1String("--dump-journal")) dump = true;
        else if (a == QLatin1String("--dump-alerts")) dumpAlerts = true;
        else if (a == QLatin1String("--alert-scale") && more) alertScale = args.at(++i).toInt();
        else if (a == QLatin1String("--data-dir") && more) dataDir = args.at(++i);
        else if (a == QLatin1String("--config") && more) configPath = args.at(++i);
        else if (a == QLatin1String("--adapter-program") && more) adapterProgram = args.at(++i);
        else if (a == QLatin1String("--base-url") && more) baseUrl = args.at(++i);
        else if (a == QLatin1String("--credentials-file") && more) credentialsFile = args.at(++i);
        else if (a == QLatin1String("--probe-credentials-file") && more) probeCredentialsFile = args.at(++i);
        else if (a == QLatin1String("--name-suffix") && more) suffix = args.at(++i);
        else if (a == QLatin1String("--heartbeat-s") && more) heartbeatS = args.at(++i).toInt();
        else if (a == QLatin1String("--wake-frequencies") && more) {
            const QStringList parts = args.at(++i).split(QLatin1Char(','), QString::SkipEmptyParts);
            for (int k = 0; k < parts.size(); ++k) frequencies << parts.at(k).toInt();
        } else return usage("unknown or incomplete option");
    }
    dataDir = QDir(dataDir).absolutePath();
    if (dump)
        return dumpJournal(dataDir);
    if (dumpAlerts)
        return dumpAlertEntries(dataDir);
    if (alertScale < 1)
        return usage("--alert-scale must be at least 1");
    if (configPath.isEmpty())
        configPath = dataDir + QStringLiteral("/config.json");
    if (adapterProgram.isEmpty())
        adapterProgram = QCoreApplication::applicationDirPath() + QStringLiteral("/../adapters/libre/host/libre-adapter-host");
    if (heartbeatS < 1)
        return usage("--heartbeat-s must be at least 1");

    SystemServiceClock clock;
    QrandRandom random;
    TimerWakeSource wake(frequencies);
    ConsoleNotifier notifier;
    ConsoleAlertNotifier alertNotifier;
    ImmediateApproval approval;
    MemorySecretStore secrets;
    JsonFileSettings settings(dataDir + QStringLiteral("/settings.json"));

    // Credentials for the simulator's fake accounts, for every connection of the config.
    if (!credentialsFile.isEmpty()) {
        QString user, password;
        if (!readCredentialsFile(credentialsFile, &user, &password))
            return usage("the credentials file is not usable");
        const ServiceConfig cfg = loadServiceConfig(configPath);
        for (int i = 0; i < cfg.connections.size(); ++i)
            secrets.setCredentials(cfg.connections.at(i).id, user, password);
    }
    if (!baseUrl.isEmpty())
        std::fprintf(stderr, "SIMULATED SOURCE: --base-url is set, no real data\n");

    RuntimeOptions options;
    options.identity = ServiceIdentity::fromNames(QStringLiteral("it.michelebrami"),
                                                  QStringLiteral("libre-monitor-host") + suffix);
    options.core.dataDirectory = dataDir;
    options.core.configPath = configPath;
    options.core.adapterLaunch = [adapterProgram, baseUrl](const QString &) {
        AdapterLaunch l;
        l.program = adapterProgram;
        if (!baseUrl.isEmpty())
            l.arguments << QStringLiteral("--base-url") << baseUrl;
        return l;
    };
    options.core.alerts = scaledAlertSettings(AlertSettings(), alertScale);
    options.lifecycle.heartbeatIntervalS = heartbeatS;
    options.appVersion = QStringLiteral("host");

    DBusNameOwner names(options.identity.busName);
    ProbeFileSecrets probeSecrets(&secrets, probeCredentialsFile);
    ServicePlatform platform;
    platform.clock = &clock;
    platform.random = &random;
    platform.wake = &wake;
    platform.notifier = &notifier;
    platform.alerts = &alertNotifier;
    platform.approval = &approval;
    platform.secrets = &probeSecrets;
    platform.credentials = &probeSecrets;
    platform.connectionSettings = &settings;
    platform.names = &names;
    return runServiceProcess(options, platform);
}
