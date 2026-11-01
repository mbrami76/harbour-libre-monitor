// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Michele Brami

// Tests of the provisioning mode: the secret
// and the configuration file are written by the code the user interface will call later, a
// second run replaces the first, the removal works, and NOTHING secret reaches the output, a
// Qt log message or an error key. Everything runs on fakes with INVENTED values: the secret
// store is MemorySecretStore (the fake of the CredentialWriter port, read back through the
// same load() the service uses), the terminal is a scripted prompter, the service is a
// recording fake. The one real piece of the terminal, the hidden prompt, is run against a
// pseudo-terminal. No real credential is involved anywhere.

#include "daemon/hostports.h"
#include "daemon/provisioning.h"
#include "daemon/provisionmode.h"
#include "daemon/serviceconfig.h"
#include "daemon/terminalprompt.h"

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QTemporaryDir>
#include <QThread>
#include <QtTest>

#include <fcntl.h>
#include <stdlib.h>
#include <termios.h>
#include <unistd.h>

using namespace lm;

namespace {

// Values that stand for the real ones; each is unusual enough to be searched for.
const QString kUser = QStringLiteral("user-sentinel-8841");
const QString kPassword = QStringLiteral("password-sentinel-5527");
const QString kRef = QStringLiteral("ref-sentinel-3190");

class ScriptedPrompter : public Prompter
{
public:
    ScriptedPrompter() : failWith() {}

    bool hidden(const QString &label, QString *value, QString *errorKey)
    {
        asked << label;
        if (!failWith.isEmpty()) {
            *errorKey = failWith;
            return false;
        }
        *value = answers.value(label);
        return true;
    }

    QHash<QString, QString> answers;
    QStringList asked;
    QString failWith;
};

class RecordingService : public ServiceCaller
{
public:
    RecordingService() : reachable(true), configCalls(0) {}

    bool configChanged(QString *result)
    {
        ++configCalls;
        *result = configResult;
        return reachable;
    }
    bool credentialsChanged(const QString &id)
    {
        credentialCalls << id;
        return reachable;
    }

    bool reachable;
    QString configResult;
    int configCalls;
    QStringList credentialCalls;
};

// Collects every message Qt would log, so a test can prove none carries a secret.
QStringList *gQtMessages = 0;
void collectMessage(QtMsgType, const QMessageLogContext &, const QString &msg)
{
    if (gQtMessages)
        *gQtMessages << msg;
}

struct Rig
{
    Rig() : provisioner(&secrets, QString())
    {
        QVERIFY2(dir.isValid(), "temporary directory");
        configPath = dir.path() + QStringLiteral("/conf/config.json");     // directory not yet there
        provisioner = Provisioner(&secrets, configPath);
        prompter.answers.insert(QStringLiteral("account name: "), kUser);
        prompter.answers.insert(QStringLiteral("password: "), kPassword);
        prompter.answers.insert(QStringLiteral("source reference: "), kRef);
    }

    // Runs the mode with the arguments after --provision; returns the exit code.
    int run(const QStringList &args)
    {
        lines.clear();
        qtMessages.clear();
        gQtMessages = &qtMessages;
        const QtMessageHandler old = qInstallMessageHandler(collectMessage);
        const int rc = runProvisionMode(parseProvisionOptions(args), &provisioner, &prompter,
                                        &service, [this](const QString &l) { lines << l; });
        qInstallMessageHandler(old);
        gQtMessages = 0;
        return rc;
    }

    static QStringList provisionArgs(const QString &id = QStringLiteral("c1"),
                                     const QString &ref = kRef)
    {
        QStringList a;
        a << QStringLiteral("--connection") << id;
        if (!ref.isEmpty())
            a << QStringLiteral("--subject-ref") << ref;
        return a;
    }

    QJsonObject readConfig() const
    {
        QFile f(configPath);
        if (!f.open(QIODevice::ReadOnly))
            return QJsonObject();
        return QJsonDocument::fromJson(f.readAll()).object();
    }
    QJsonObject connection(const QString &id) const
    {
        const QJsonArray a = readConfig().value(QStringLiteral("connections")).toArray();
        for (int i = 0; i < a.size(); ++i)
            if (a.at(i).toObject().value(QStringLiteral("id")).toString() == id)
                return a.at(i).toObject();
        return QJsonObject();
    }
    QString subjectId(const QString &id) const
    {
        return connection(id).value(QStringLiteral("subjects")).toArray().first().toObject()
            .value(QStringLiteral("id")).toString();
    }
    SecretLoad load(const QString &id)
    {
        SecretLoad out;
        secrets.load(id, [&out](const SecretLoad &r) { out = r; });
        return out;
    }
    void writeConfigFile(const QByteArray &content)
    {
        QDir().mkpath(QFileInfo(configPath).absolutePath());
        QFile f(configPath);
        QVERIFY(f.open(QIODevice::WriteOnly | QIODevice::Truncate));
        f.write(content);
    }
    // Everything the run produced that a person or a log could see.
    QString everythingVisible() const { return (lines + qtMessages).join(QLatin1Char('\n')); }

    QTemporaryDir dir;
    QString configPath;
    MemorySecretStore secrets;
    Provisioner provisioner;
    ScriptedPrompter prompter;
    RecordingService service;
    QStringList lines;
    QStringList qtMessages;
};

// Types into the master side of a pseudo-terminal after the prompt has switched echo off, the
// way a person would, and records what the terminal showed back.
class Typist : public QThread
{
public:
    Typist(int master, const QByteArray &text) : master(master), text(text) {}
    void run()
    {
        QThread::msleep(300);       // the prompt flushes typed-ahead input when it starts
        const ssize_t n = ::write(master, text.constData(), text.size());
        Q_UNUSED(n);
        QThread::msleep(300);
        char buf[512];
        const int flags = ::fcntl(master, F_GETFL);
        ::fcntl(master, F_SETFL, flags | O_NONBLOCK);
        ssize_t r;
        while ((r = ::read(master, buf, sizeof(buf))) > 0)
            shown.append(buf, int(r));
    }
    int master;
    QByteArray text;
    QByteArray shown;       // what came out of the terminal
};

bool openPty(int *master, int *slave)
{
    *master = ::posix_openpt(O_RDWR | O_NOCTTY);
    if (*master < 0 || ::grantpt(*master) != 0 || ::unlockpt(*master) != 0)
        return false;
    const char *name = ::ptsname(*master);
    if (!name)
        return false;
    *slave = ::open(name, O_RDWR | O_NOCTTY);
    return *slave >= 0;
}

} // namespace

class TstProvision : public QObject
{
    Q_OBJECT

private slots:
    // ---- options ----------------------------------------------------------------------------
    void optionsAreCheckedAndNeverEchoed();

    // ---- provisioning -----------------------------------------------------------------------
    void provisioningWritesTheSecretAndTheFileAndTellsTheService();
    void theFileIsReadableByTheOwnerOnlyAndParsesWithTheServicesReader();
    void nothingSecretReachesTheOutputOrALog();
    void aSecondRunReplacesTheFirstAndKeepsTheSubjectForTheSameReference();
    void aDifferentReferenceGetsANewSubject();
    void otherConnectionsAndUnknownKeysAreKept();
    void aBrokenFileIsNeverOverwrittenAndTheSecretIsTakenBack();
    void aFailingSecretStoreWritesNoFile();
    void theReservedProbeIdentifierIsNeverAConnection();
    void theReferenceIsAskedAtTheHiddenPromptWhenNotGiven();
    void promptFailuresAndEmptyInputWriteNothing();
    void aServiceThatCannotBeReachedIsReportedNotFatal();
    void aRestartNeededAnswerIsPassedOn();

    // ---- removal ----------------------------------------------------------------------------
    void removeDeletesTheSecretAndTheConnectionOnly();
    void removeWithNothingThereIsHarmless();
    void removeTriesBothPartsWhenOneFails();

    // ---- the hidden prompt --------------------------------------------------------------------
    void theTerminalEchoesNormallyWhichMakesTheNextTestMeaningful();
    void theHiddenPromptShowsNothingAndRestoresTheTerminal();
    void aPipeIsNotATerminal();
};

// ---- options -----------------------------------------------------------------------------------

void TstProvision::optionsAreCheckedAndNeverEchoed()
{
    ProvisionOptions o = parseProvisionOptions(Rig::provisionArgs());
    QVERIFY(o.error.isEmpty());
    QCOMPARE(o.connectionId, QStringLiteral("c1"));
    QCOMPARE(o.subjectRef, kRef);
    QVERIFY(!o.remove);

    o = parseProvisionOptions(QStringList() << "--remove" << "--connection" << "c1");
    QVERIFY(o.error.isEmpty() && o.remove);

    const QList<QStringList> bad = QList<QStringList>()
        << QStringList()                                                        // no connection
        << (QStringList() << "--connection")                                    // no value
        << (QStringList() << "--connection" << "a b")                           // bad id
        << (QStringList() << "--connection" << "../x")
        << (QStringList() << "--connection" << "probe")                         // reserved
        << (QStringList() << "--connection" << "c1" << "--password" << kPassword)
        << (QStringList() << "--connection" << "c1" << "--subject-ref" << "")
        << (QStringList() << "--remove" << "--connection" << "c1" << "--subject-ref" << kRef);
    for (int i = 0; i < bad.size(); ++i) {
        o = parseProvisionOptions(bad.at(i));
        QVERIFY2(!o.error.isEmpty(), qPrintable(QString::number(i)));
        QVERIFY(!o.error.contains(kPassword) && !o.error.contains(kRef));
        QVERIFY(!o.error.contains(QStringLiteral("a b")));
    }
}

// ---- provisioning ------------------------------------------------------------------------------

void TstProvision::provisioningWritesTheSecretAndTheFileAndTellsTheService()
{
    Rig rig;
    QCOMPARE(rig.run(Rig::provisionArgs()), 0);

    // What the service would read back: the same load() it calls at start.
    const SecretLoad s = rig.load(QStringLiteral("c1"));
    QCOMPARE(s.kind, SecretLoad::Ok);
    QCOMPARE(s.credentials.username, kUser);
    QCOMPARE(s.credentials.password, kPassword);
    QVERIFY(s.session.isEmpty());

    const ServiceConfig config = loadServiceConfig(rig.configPath);
    QVERIFY(config.ok);
    QCOMPARE(config.connections.size(), 1);
    QCOMPARE(config.connections.first().id, QStringLiteral("c1"));
    QCOMPARE(config.connections.first().adapterId, QStringLiteral("libre"));
    QCOMPARE(config.connections.first().subjects.size(), 1);
    QCOMPARE(config.connections.first().subjects.first().subjectRef, kRef);
    QVERIFY(!config.connections.first().subjects.first().id.isNull());

    QCOMPARE(rig.service.configCalls, 1);
    QCOMPARE(rig.service.credentialCalls, QStringList() << "c1");
    QCOMPARE(rig.lines, QStringList()
             << "secret stored" << "connection written"
             << "service told: configuration reloaded" << "service told: credentials changed");
    // Account name and password at the prompt, the reference from the argument.
    QCOMPARE(rig.prompter.asked, QStringList() << "account name: " << "password: ");
}

void TstProvision::theFileIsReadableByTheOwnerOnlyAndParsesWithTheServicesReader()
{
    Rig rig;
    QCOMPARE(rig.run(Rig::provisionArgs()), 0);
    QCOMPARE(QFile::permissions(rig.configPath) & (QFileDevice::ReadGroup | QFileDevice::WriteGroup
                                                   | QFileDevice::ReadOther | QFileDevice::WriteOther
                                                   | QFileDevice::ExeOwner),
             QFileDevice::Permissions());
    QVERIFY(QFile::permissions(rig.configPath) & QFileDevice::ReadOwner);

    // A file that existed with wider permissions is narrowed by the rewrite.
    QVERIFY(QFile::setPermissions(rig.configPath, QFileDevice::ReadOwner | QFileDevice::WriteOwner
                                                      | QFileDevice::ReadGroup | QFileDevice::ReadOther));
    QCOMPARE(rig.run(Rig::provisionArgs()), 0);
    QCOMPARE(QFile::permissions(rig.configPath) & (QFileDevice::ReadGroup | QFileDevice::ReadOther),
             QFileDevice::Permissions());
}

void TstProvision::nothingSecretReachesTheOutputOrALog()
{
    // The success path, the failure paths and the removal: after each, the lines the run
    // printed and every Qt message it logged are searched for the account name, the password,
    // the reference and the Subject's UUID.
    Rig rig;
    QStringList seen;

    QCOMPARE(rig.run(Rig::provisionArgs()), 0);
    seen << rig.everythingVisible();
    const QString uuid = rig.subjectId(QStringLiteral("c1"));
    QVERIFY(!uuid.isEmpty());

    rig.secrets.failStores(true);
    QCOMPARE(rig.run(Rig::provisionArgs()), 1);
    seen << rig.everythingVisible();
    rig.secrets.failStores(false);

    rig.writeConfigFile("{ broken");
    QCOMPARE(rig.run(Rig::provisionArgs(QStringLiteral("c2"))), 1);
    seen << rig.everythingVisible();
    rig.writeConfigFile(QByteArray("{\"format\":1,\"connections\":[]}"));

    rig.prompter.failWith = QStringLiteral("no-terminal");
    QCOMPARE(rig.run(Rig::provisionArgs()), 1);
    seen << rig.everythingVisible();
    rig.prompter.failWith.clear();

    rig.service.reachable = false;
    QCOMPARE(rig.run(Rig::provisionArgs()), 0);
    seen << rig.everythingVisible();
    rig.service.reachable = true;

    QCOMPARE(rig.run(QStringList() << "--remove" << "--connection" << "c1"), 0);
    seen << rig.everythingVisible();
    QCOMPARE(rig.run(QStringList() << "--connection" << "bad id"), 64);
    seen << rig.everythingVisible();

    const QString all = seen.join(QLatin1Char('\n'));
    QVERIFY(all.size() > 100);
    QVERIFY(!all.contains(kUser));
    QVERIFY(!all.contains(kPassword));
    QVERIFY(!all.contains(kRef));
    QVERIFY(!all.contains(uuid));
    QVERIFY(!all.contains(QStringLiteral("sentinel")));
    QVERIFY(!all.contains(rig.configPath));
}

void TstProvision::aSecondRunReplacesTheFirstAndKeepsTheSubjectForTheSameReference()
{
    Rig rig;
    QCOMPARE(rig.run(Rig::provisionArgs()), 0);
    const QString first = rig.subjectId(QStringLiteral("c1"));

    rig.prompter.answers.insert(QStringLiteral("password: "), QStringLiteral("another-password"));
    QCOMPARE(rig.run(Rig::provisionArgs()), 0);

    QCOMPARE(rig.secrets.storeCount(), 2);
    const SecretLoad s = rig.load(QStringLiteral("c1"));
    QCOMPARE(s.credentials.password, QStringLiteral("another-password"));
    QCOMPARE(rig.readConfig().value("connections").toArray().size(), 1);        // not two
    // The readings in the store are keyed by this UUID, so it must survive.
    QCOMPARE(rig.subjectId(QStringLiteral("c1")), first);
}

void TstProvision::aDifferentReferenceGetsANewSubject()
{
    Rig rig;
    QCOMPARE(rig.run(Rig::provisionArgs()), 0);
    const QString first = rig.subjectId(QStringLiteral("c1"));
    QCOMPARE(rig.run(Rig::provisionArgs(QStringLiteral("c1"), QStringLiteral("some-other-ref"))), 0);
    QVERIFY(rig.subjectId(QStringLiteral("c1")) != first);
    QCOMPARE(rig.connection(QStringLiteral("c1")).value("subjects").toArray().size(), 1);
}

void TstProvision::otherConnectionsAndUnknownKeysAreKept()
{
    Rig rig;
    // A file a later interface might have written: another connection, a name (personal
    // data, only ever in this file), a key this code does not know.
    rig.writeConfigFile(
        "{\"format\":1,\"theme\":\"dark\",\"connections\":["
        "{\"id\":\"c2\",\"adapter\":\"libre\",\"current_interval_s\":120,\"subjects\":"
        "[{\"id\":\"{d2f1c9a0-5b3e-4c7d-8a91-0e2b4c6d8f10}\",\"ref\":\"r2\",\"name\":\"A Real Name\"}]}]}");
    QCOMPARE(rig.run(Rig::provisionArgs()), 0);

    const QJsonObject root = rig.readConfig();
    QCOMPARE(root.value("theme").toString(), QStringLiteral("dark"));
    QCOMPARE(root.value("connections").toArray().size(), 2);
    const QJsonObject c2 = rig.connection(QStringLiteral("c2"));
    QCOMPARE(c2.value("current_interval_s").toInt(), 120);
    QCOMPARE(c2.value("subjects").toArray().first().toObject().value("name").toString(),
             QStringLiteral("A Real Name"));
    QVERIFY(loadServiceConfig(rig.configPath).ok);
    QCOMPARE(loadServiceConfig(rig.configPath).connections.size(), 2);
}

void TstProvision::aBrokenFileIsNeverOverwrittenAndTheSecretIsTakenBack()
{
    Rig rig;
    const QByteArray broken = "{ this is not json, but it is somebody's file";
    rig.writeConfigFile(broken);
    QCOMPARE(rig.run(Rig::provisionArgs()), 1);

    QFile f(rig.configPath);
    QVERIFY(f.open(QIODevice::ReadOnly));
    QCOMPARE(f.readAll(), broken);
    QVERIFY(!rig.secrets.hasSecret(QStringLiteral("c1")));      // no credential without a connection
    QVERIFY(rig.lines.contains(QStringLiteral("failed: config-invalid")));
    QVERIFY(!rig.lines.contains(QStringLiteral("secret stored")));
    QCOMPARE(rig.service.configCalls, 0);                       // nothing to tell
    QVERIFY(rig.service.credentialCalls.isEmpty());
}

void TstProvision::aFailingSecretStoreWritesNoFile()
{
    Rig rig;
    rig.secrets.failStores(true);
    QCOMPARE(rig.run(Rig::provisionArgs()), 1);
    QVERIFY(!QFile::exists(rig.configPath));
    QCOMPARE(rig.lines, QStringList() << "failed: secret-store-failed");
}

// `probe` is the secret name of the set-up page's one-time login test. A connection by
// that id would have its login read as a probe and removed after the first discovery, so it is
// refused everywhere a connection can be made, and nothing is written.
void TstProvision::theReservedProbeIdentifierIsNeverAConnection()
{
    QVERIFY(!isValidConnectionId(QStringLiteral("probe")));
    QVERIFY(isValidConnectionId(QStringLiteral("probe2")));
    QVERIFY(isValidConnectionId(QStringLiteral("my-probe")));

    Rig rig;
    QCOMPARE(rig.run(Rig::provisionArgs(QStringLiteral("probe"))), 64);
    QVERIFY(!QFile::exists(rig.configPath));
    QVERIFY(!rig.secrets.hasSecret(QStringLiteral("probe")));

    QString error;
    QVERIFY(!upsertConnection(rig.configPath, QStringLiteral("probe"), QStringLiteral("libre"), kRef, &error));
    QCOMPARE(error, QStringLiteral("request-invalid"));
    QVERIFY(!QFile::exists(rig.configPath));
}

void TstProvision::theReferenceIsAskedAtTheHiddenPromptWhenNotGiven()
{
    Rig rig;
    QCOMPARE(rig.run(Rig::provisionArgs(QStringLiteral("c1"), QString())), 0);
    QCOMPARE(rig.prompter.asked,
             QStringList() << "account name: " << "password: " << "source reference: ");
    QCOMPARE(loadServiceConfig(rig.configPath).connections.first().subjects.first().subjectRef, kRef);
}

void TstProvision::promptFailuresAndEmptyInputWriteNothing()
{
    Rig rig;
    rig.prompter.failWith = QStringLiteral("no-terminal");
    QCOMPARE(rig.run(Rig::provisionArgs()), 1);
    QCOMPARE(rig.lines, QStringList() << "failed: no-terminal");
    QVERIFY(!QFile::exists(rig.configPath) && !rig.secrets.hasSecret(QStringLiteral("c1")));

    rig.prompter.failWith.clear();
    rig.prompter.answers.insert(QStringLiteral("password: "), QString());
    QCOMPARE(rig.run(Rig::provisionArgs()), 1);
    QCOMPARE(rig.lines, QStringList() << "failed: input-empty");
    QVERIFY(!QFile::exists(rig.configPath) && !rig.secrets.hasSecret(QStringLiteral("c1")));
    QCOMPARE(rig.secrets.storeCount(), 0);
}

void TstProvision::aServiceThatCannotBeReachedIsReportedNotFatal()
{
    Rig rig;
    rig.service.reachable = false;
    QCOMPARE(rig.run(Rig::provisionArgs()), 0);         // the set-up itself worked
    QVERIFY(rig.secrets.hasSecret(QStringLiteral("c1")) && QFile::exists(rig.configPath));
    QCOMPARE(rig.lines, QStringList()
             << "secret stored" << "connection written"
             << "service not reached (configuration)" << "service not reached (credentials)");
}

void TstProvision::aRestartNeededAnswerIsPassedOn()
{
    Rig rig;
    rig.service.configResult = QStringLiteral("restart-needed");
    QCOMPARE(rig.run(Rig::provisionArgs()), 0);
    QVERIFY(rig.lines.contains(
        QStringLiteral("service told: configuration changed, restart needed (stop, then start)")));
}

// ---- removal -----------------------------------------------------------------------------------

void TstProvision::removeDeletesTheSecretAndTheConnectionOnly()
{
    Rig rig;
    QCOMPARE(rig.run(Rig::provisionArgs(QStringLiteral("c2"), QStringLiteral("r2"))), 0);
    QCOMPARE(rig.run(Rig::provisionArgs()), 0);
    QCOMPARE(rig.run(QStringList() << "--remove" << "--connection" << "c1"), 0);

    QVERIFY(!rig.secrets.hasSecret(QStringLiteral("c1")));
    QCOMPARE(rig.load(QStringLiteral("c1")).kind, SecretLoad::Missing);
    QVERIFY(rig.connection(QStringLiteral("c1")).isEmpty());
    QVERIFY(rig.secrets.hasSecret(QStringLiteral("c2")));               // the other one stays
    QVERIFY(!rig.connection(QStringLiteral("c2")).isEmpty());
    QCOMPARE(rig.lines, QStringList()
             << "secret removed" << "connection removed"
             << "service told: configuration reloaded" << "service told: credentials changed");
    QCOMPARE(rig.prompter.asked.size(), 4);     // nothing was asked for the removal (2 + 2 earlier)
}

void TstProvision::removeWithNothingThereIsHarmless()
{
    Rig rig;
    QCOMPARE(rig.run(QStringList() << "--remove" << "--connection" << "c1"), 0);
    QVERIFY(!QFile::exists(rig.configPath));        // not even created
    QVERIFY(rig.lines.contains(QStringLiteral("connection was not in the configuration")));
}

void TstProvision::removeTriesBothPartsWhenOneFails()
{
    Rig rig;
    QCOMPARE(rig.run(Rig::provisionArgs()), 0);
    rig.secrets.failRemoves(true);
    QCOMPARE(rig.run(QStringList() << "--remove" << "--connection" << "c1"), 1);
    QVERIFY(rig.connection(QStringLiteral("c1")).isEmpty());        // the file part still went
    QVERIFY(rig.lines.contains(QStringLiteral("connection removed")));
    QVERIFY(rig.lines.contains(QStringLiteral("failed: secret-store-failed")));
    QVERIFY(!rig.lines.contains(QStringLiteral("secret removed")));
}

// ---- the hidden prompt ---------------------------------------------------------------------------

void TstProvision::theTerminalEchoesNormallyWhichMakesTheNextTestMeaningful()
{
    int master, slave;
    if (!openPty(&master, &slave))
        QSKIP("no pseudo-terminal in this environment");
    Typist typist(master, "visible-text\n");
    typist.start();
    QVERIFY(typist.wait(5000));
    QVERIFY2(typist.shown.contains("visible-text"), "a plain terminal echoes what is typed");
    ::close(slave);
    ::close(master);
}

void TstProvision::theHiddenPromptShowsNothingAndRestoresTheTerminal()
{
    int master, slave;
    if (!openPty(&master, &slave))
        QSKIP("no pseudo-terminal in this environment");
    struct termios before;
    QCOMPARE(::tcgetattr(slave, &before), 0);
    QVERIFY(before.c_lflag & ECHO);

    Typist typist(master, "typed-in-secret\n");
    typist.start();
    QString line, key;
    QVERIFY(readHiddenLine(slave, slave, "password: ", &line, &key));
    QVERIFY(typist.wait(5000));

    QCOMPARE(line, QStringLiteral("typed-in-secret"));          // the program got it
    QVERIFY(typist.shown.contains("password: "));               // the person saw the question
    QVERIFY2(!typist.shown.contains("typed-in-secret"), "the typed text was echoed");
    struct termios after;
    QCOMPARE(::tcgetattr(slave, &after), 0);
    QVERIFY(after.c_lflag & ECHO);                              // the terminal is as it was
    ::close(slave);
    ::close(master);
}

void TstProvision::aPipeIsNotATerminal()
{
    int fds[2];
    QCOMPARE(::pipe(fds), 0);
    const char text[] = "piped-secret\n";
    QVERIFY(::write(fds[1], text, sizeof(text) - 1) > 0);
    QString line, key;
    QVERIFY(!readHiddenLine(fds[0], fds[1], "password: ", &line, &key));
    QCOMPARE(key, QStringLiteral("no-terminal"));
    QVERIFY(line.isEmpty());            // a secret is never taken from a pipe
    ::close(fds[0]);
    ::close(fds[1]);
}

QTEST_GUILESS_MAIN(TstProvision)
#include "tst_provision.moc"
