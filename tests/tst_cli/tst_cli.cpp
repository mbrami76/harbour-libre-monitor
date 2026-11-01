// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Michele Brami
//
// Command-line program: arguments, secret files, exit codes, output shapes.
#include "credentials.h"
#include "options.h"
#include "output.h"
#include "runner.h"

#include <QTemporaryDir>
#include <QtTest>

#include <sys/stat.h>
#include <unistd.h>

using namespace lmcli;

Q_DECLARE_METATYPE(lm::ErrorClass)

namespace {

bool writeFile(const QString &path, const QByteArray &content, mode_t mode)
{
    QFile::remove(path);
    QFile f(path);
    if (!f.open(QIODevice::WriteOnly))
        return false;
    f.write(content);
    f.close();
    return ::chmod(QFile::encodeName(path).constData(), mode) == 0;
}

lm::Record glucose(double value, const QString &unit, const QString &time)
{
    lm::Record r;
    r.metric = "glucose";
    r.value = value;
    r.unit = unit;
    r.measuredAt = lm::UtcTime::fromIso(time);
    r.source = "libre.current";
    return r;
}

} // namespace

class TstCli : public QObject
{
    Q_OBJECT

private slots:
    // ---- Arguments ----------------------------------------------------------------

    void parsesDiscover()
    {
        Options o;
        QString e;
        QVERIFY(parseArguments(QStringList() << "discover" << "--json" << "--verbose"
                                             << "--credentials-file" << "/x/c" << "--session-file=/x/s"
                                             << "--adapter" << "/x/a", &o, &e));
        QCOMPARE(o.command, QString("discover"));
        QVERIFY(o.json);
        QVERIFY(o.verbose);
        QCOMPARE(o.credentialsFile, QString("/x/c"));
        QCOMPARE(o.sessionFile, QString("/x/s"));
        QCOMPARE(o.adapterPath, QString("/x/a"));
        QCOMPARE(o.subjectIndex, -1);
    }

    void parsesCurrentWithSubject()
    {
        Options o;
        QString e;
        QVERIFY(parseArguments(QStringList() << "current" << "--subject" << "2", &o, &e));
        QCOMPARE(o.subjectIndex, 2);
    }

    void parsesRange()
    {
        Options o;
        QString e;
        QVERIFY(parseArguments(QStringList() << "range" << "--from" << "2026-01-01T00:00:00Z"
                                             << "--to=2026-01-01T12:00:00Z", &o, &e));
        QCOMPARE(o.from.toIso(), QString("2026-01-01T00:00:00.000Z"));
        QCOMPARE(o.to.toIso(), QString("2026-01-01T12:00:00.000Z"));
    }

    void rejectsBadArguments_data()
    {
        QTest::addColumn<QStringList>("args");
        QTest::newRow("nothing") << QStringList();
        QTest::newRow("unknown command") << (QStringList() << "login");
        QTest::newRow("unknown option") << (QStringList() << "discover" << "--frobnicate");
        QTest::newRow("positional") << (QStringList() << "discover" << "extra");
        QTest::newRow("missing value") << (QStringList() << "discover" << "--adapter");
        QTest::newRow("flag with value") << (QStringList() << "discover" << "--json=yes");
        QTest::newRow("bad subject") << (QStringList() << "current" << "--subject" << "x");
        QTest::newRow("negative subject") << (QStringList() << "current" << "--subject" << "-1");
        QTest::newRow("range without times") << (QStringList() << "range");
        QTest::newRow("range one time") << (QStringList() << "range" << "--from" << "2026-01-01T00:00:00Z");
        QTest::newRow("range not UTC") << (QStringList() << "range" << "--from" << "2026-01-01T00:00:00+01:00"
                                                          << "--to" << "2026-01-02T00:00:00Z");
        QTest::newRow("range reversed") << (QStringList() << "range" << "--from" << "2026-01-02T00:00:00Z"
                                                          << "--to" << "2026-01-01T00:00:00Z");
        QTest::newRow("times on current") << (QStringList() << "current" << "--from" << "2026-01-01T00:00:00Z");
    }

    void rejectsBadArguments()
    {
        QFETCH(QStringList, args);
        Options o;
        QString e;
        QVERIFY(!parseArguments(args, &o, &e));
        QVERIFY(!e.isEmpty());
    }

    void credentialsAreNeverOptions_data()
    {
        QTest::addColumn<QString>("arg");
        QTest::newRow("password") << "--password=hunter2-synthetic";
        QTest::newRow("password separate") << "--password";
        QTest::newRow("username") << "--username=alice@example.invalid";
        QTest::newRow("token") << "--token=abc123";
        QTest::newRow("session") << "--session=SESSION-BLOB-synthetic";
    }

    void credentialsAreNeverOptions()
    {
        QFETCH(QString, arg);
        Options o;
        QString e;
        QVERIFY(!parseArguments(QStringList() << "discover" << arg, &o, &e));
        // The message names the option and never repeats its value (REQ-ADP-11).
        QVERIFY(!e.contains("hunter2"));
        QVERIFY(!e.contains("alice@"));
        QVERIFY(!e.contains("abc123"));
        QVERIFY(!e.contains("SESSION-BLOB"));
    }

    void baseUrlOnlyInDevsourceBuilds()
    {
        Options o;
        QString e;
#ifdef LIBREMON_DEVSOURCE
        QVERIFY(parseArguments(QStringList() << "discover" << "--base-url" << "http://127.0.0.1:8080", &o, &e));
        QCOMPARE(o.baseUrl, QString("http://127.0.0.1:8080"));
        QVERIFY(usageText().contains("--base-url"));
#else
        QVERIFY(!parseArguments(QStringList() << "discover" << "--base-url" << "http://127.0.0.1:8080", &o, &e));
        QVERIFY(!usageText().contains("--base-url"));
#endif
    }

    void usageTextListsTheExitCodes()
    {
        const QString u = usageText();
        QVERIFY(u.contains("--credentials-file"));
        QVERIFY(u.contains("21 contract violation"));
        QVERIFY(!u.contains("--password"));
    }

    // ---- Exit codes ---------------------------------------------------------------

    void exitCodes_data()
    {
        QTest::addColumn<lm::ErrorClass>("errorClass");
        QTest::addColumn<int>("code");
        QTest::newRow("auth") << lm::ErrorClass::AuthFailed << 10;
        QTest::newRow("terms") << lm::ErrorClass::TermsMustBeAccepted << 11;
        QTest::newRow("rate") << lm::ErrorClass::RateLimited << 12;
        QTest::newRow("transient") << lm::ErrorClass::Transient << 13;
        QTest::newRow("protocol") << lm::ErrorClass::ProtocolChanged << 14;
        QTest::newRow("invalid") << lm::ErrorClass::InvalidCommand << 15;
    }

    void exitCodes()
    {
        QFETCH(lm::ErrorClass, errorClass);
        QFETCH(int, code);
        QCOMPARE(exitCodeFor(errorClass), code);
        QCOMPARE(int(ExitUsage), 2);
        QCOMPARE(int(ExitAdmissionRejected), 20);
        QCOMPARE(int(ExitContractViolation), 21);
        QCOMPARE(int(ExitOk), 0);
    }

    // ---- Secret files -------------------------------------------------------------

    void privateFileRule()
    {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        const QString path = (dir.path() + "/" + "c");
        QString e;

        QVERIFY(writeFile(path, "x", 0600));
        QVERIFY(checkPrivateFile(path, &e));
        QVERIFY(writeFile(path, "x", 0400));
        QVERIFY(checkPrivateFile(path, &e));

        const mode_t bad[] = { 0640, 0604, 0660, 0666, 0644, 0610, 0601 };
        for (size_t i = 0; i < sizeof(bad) / sizeof(bad[0]); ++i) {
            QVERIFY(writeFile(path, "x", bad[i]));
            QVERIFY2(!checkPrivateFile(path, &e), qPrintable(QString::number(bad[i], 8)));
            QVERIFY(e.contains("chmod 600"));
        }

        QVERIFY(!checkPrivateFile((dir.path() + "/" + "missing"), &e));
        QVERIFY(!checkPrivateFile(dir.path(), &e));         // a directory
        QVERIFY(writeFile(path, "x", 0600));
        QVERIFY(QFile::link(path, (dir.path() + "/" + "link")));
        QVERIFY(!checkPrivateFile((dir.path() + "/" + "link"), &e));   // a symbolic link
    }

    void readsCredentials()
    {
        QTemporaryDir dir;
        const QString path = (dir.path() + "/" + "c");
        QVERIFY(writeFile(path, "# synthetic\nusername=alice@example.invalid\r\npassword=pa ss=word #1\n", 0600));
        lm::Credentials c;
        QString e;
        QVERIFY(readCredentialsFile(path, &c, &e));
        QCOMPARE(c.username, QString("alice@example.invalid"));
        QCOMPARE(c.password, QString("pa ss=word #1"));      // taken as it is
    }

    void refusesLooseCredentialsFileWithoutReadingIt()
    {
        QTemporaryDir dir;
        const QString path = (dir.path() + "/" + "c");
        QVERIFY(writeFile(path, "username=a\npassword=hunter2-synthetic\n", 0644));
        lm::Credentials c;
        QString e;
        QVERIFY(!readCredentialsFile(path, &c, &e));
        QVERIFY(!e.contains("hunter2"));
        QVERIFY(c.password.isEmpty());
    }

    void credentialsFileErrors_data()
    {
        QTest::addColumn<QByteArray>("content");
        QTest::newRow("empty") << QByteArray();
        QTest::newRow("no password") << QByteArray("username=a\n");
        QTest::newRow("no username") << QByteArray("password=a\n");
        QTest::newRow("empty password") << QByteArray("username=a\npassword=\n");
        QTest::newRow("twice") << QByteArray("username=a\nusername=b\npassword=c\n");
        QTest::newRow("stray line") << QByteArray("username=a\npassword=hunter2-synthetic\nhunter3\n");
    }

    void credentialsFileErrors()
    {
        QFETCH(QByteArray, content);
        lm::Credentials c;
        QString e;
        QVERIFY(!parseCredentials(content, &c, &e));
        QVERIFY(!e.contains("hunter"));         // an error never quotes the file
    }

    void sessionFile()
    {
        QTemporaryDir dir;
        const QString path = (dir.path() + "/" + "s");
        QString session, e;
        QVERIFY(readSessionFile(path, &session, &e));       // missing is fine
        QVERIFY(session.isEmpty());

        QVERIFY(writeSessionFile(path, "SESSION-BLOB-synthetic", &e));
        struct stat st;
        QCOMPARE(::stat(QFile::encodeName(path).constData(), &st), 0);
        QCOMPARE(int(st.st_mode & 0777), 0600);
        QVERIFY(readSessionFile(path, &session, &e));
        QCOMPARE(session, QString("SESSION-BLOB-synthetic"));

        QVERIFY(writeFile(path, "SESSION-BLOB-synthetic\n", 0644));
        QVERIFY(!readSessionFile(path, &session, &e));      // the same mode rule
    }

    void sessionFileIsNotWrittenThroughASymbolicLink()
    {
        QTemporaryDir dir;
        const QString target = (dir.path() + "/" + "target");
        QVERIFY(writeFile(target, "keep", 0600));
        QVERIFY(QFile::link(target, (dir.path() + "/" + "s")));
        QString e;
        QVERIFY(!writeSessionFile((dir.path() + "/" + "s"), "SESSION", &e));
        QFile f(target);
        QVERIFY(f.open(QIODevice::ReadOnly));
        QCOMPARE(f.readAll(), QByteArray("keep"));
    }

    // ---- Output -------------------------------------------------------------------

    void valueUsesTheNativeResolution()
    {
        QCOMPARE(formatValue(112, "mg/dL"), QString("112 mg/dL"));
        QCOMPARE(formatValue(6.2, "mmol/L"), QString("6.2 mmol/L"));
    }

    void ageIsInWholeMinutesAndNeverNegative()
    {
        const lm::UtcTime t = lm::UtcTime::fromIso("2026-01-01T12:00:00Z");
        QCOMPARE(ageMinutes(t, lm::UtcTime::fromIso("2026-01-01T12:05:59Z")), qint64(5));
        QCOMPARE(ageMinutes(t, lm::UtcTime::fromIso("2026-01-01T11:59:00Z")), qint64(0));
    }

    void currentOutputText()
    {
        SubjectResult r;
        r.index = 0;
        r.info.displayName = "Synthetic Person";
        r.records << glucose(110, "mg/dL", "2026-01-01T11:50:00Z") << glucose(112, "mg/dL", "2026-01-01T12:00:00Z");
        r.sensor.insert("id", "abcdef0123456789");
        r.sensor.insert("warmup_minutes", 60);
        const QString out = formatCurrent(QList<SubjectResult>() << r, lm::UtcTime::fromIso("2026-01-01T12:05:00Z"));
        QVERIFY(out.contains("Subject 0: Synthetic Person"));
        QVERIFY(out.contains("112 mg/dL"));
        QVERIFY(!out.contains("110 mg/dL"));
        QVERIFY(out.contains("2026-01-01T12:00:00.000Z (5 min ago)"));
        QVERIFY(out.contains("libre.current"));
        QVERIFY(out.contains("id abcdef01"));
        QVERIFY(out.contains("warm-up 60 min"));
    }

    void currentOutputWithoutAValue()
    {
        SubjectResult r;
        r.info.displayName = "Synthetic Person";
        QVERIFY(formatCurrent(QList<SubjectResult>() << r, lm::UtcTime::now()).contains("no current value"));
    }

    void currentJsonShape()
    {
        SubjectResult r;
        r.index = 1;
        r.info.displayName = "Synthetic Person";
        r.records << glucose(112, "mg/dL", "2026-01-01T12:00:00Z");
        const QJsonObject root = currentJson(QList<SubjectResult>() << r, lm::UtcTime::fromIso("2026-01-01T12:05:00Z"));
        const QJsonObject s = root.value("subjects").toArray().at(0).toObject();
        QCOMPARE(s.value("index").toInt(), 1);
        QCOMPARE(s.value("display_name").toString(), QString("Synthetic Person"));
        const QJsonObject c = s.value("current").toObject();
        QCOMPARE(c.value("value").toDouble(), 112.0);
        QCOMPARE(c.value("unit").toString(), QString("mg/dL"));
        QCOMPARE(c.value("measured_at").toString(), QString("2026-01-01T12:00:00.000Z"));
        QCOMPARE(c.value("age_minutes").toInt(), 5);
        QCOMPARE(c.value("source").toString(), QString("libre.current"));
        QVERIFY(s.value("sensor").isNull());
    }

    void discoverOutputAndJson()
    {
        lm::SubjectInfo a, b;
        a.subjectRef = "S-1"; a.displayName = "Synthetic Person"; a.unit = "mg/dL"; a.unitBasis = "code";
        b.subjectRef = "S-2"; b.displayName = "Other Person"; b.unitBasis = "none";
        const QString text = formatDiscover(QList<lm::SubjectInfo>() << a << b);
        QVERIFY(text.contains("0  Synthetic Person  unit: mg/dL  basis: code"));
        QVERIFY(text.contains("1  Other Person  unit: undetermined  basis: none"));
        QVERIFY(!text.contains("S-1"));         // the source reference is not shown
        const QJsonObject j = discoverJson(QList<lm::SubjectInfo>() << a << b);
        const QJsonArray list = j.value("subjects").toArray();
        QCOMPARE(list.size(), 2);
        QCOMPARE(list.at(0).toObject().value("unit").toString(), QString("mg/dL"));
        QVERIFY(list.at(1).toObject().value("unit").isNull());
        QCOMPARE(list.at(1).toObject().value("unit_basis").toString(), QString("none"));
        QVERIFY(!list.at(0).toObject().contains("subject_ref"));        // off unless asked for
        const QJsonArray withRef = discoverJson(QList<lm::SubjectInfo>() << a << b, true)
                                       .value("subjects").toArray();
        QCOMPARE(withRef.at(0).toObject().value("subject_ref").toString(), QString("S-1"));
        QCOMPARE(withRef.at(1).toObject().value("subject_ref").toString(), QString("S-2"));
    }

    void showRefOnlyWithDiscoverJson()
    {
        Options o;
        QString e;
        QVERIFY(parseArguments(QStringList() << "discover" << "--json" << "--show-ref", &o, &e));
        QVERIFY(o.showRef);
        QVERIFY(!parseArguments(QStringList() << "discover" << "--show-ref", &o, &e));
        QVERIFY(!parseArguments(QStringList() << "current" << "--json" << "--show-ref", &o, &e));
        QVERIFY(parseArguments(QStringList() << "discover" << "--json", &o, &e));
        QVERIFY(!o.showRef);
    }

    void rangeOutput()
    {
        SubjectResult r;
        r.info.displayName = "Synthetic Person";
        r.records << glucose(120, "mg/dL", "2026-01-01T10:05:00Z") << glucose(115, "mg/dL", "2026-01-01T10:00:00Z");
        r.received = 3; r.valid = 2; r.quarantined = 1;
        lm::FeedCoverage c;
        c.feed = "graph";
        c.from = lm::UtcTime::fromIso("2026-01-01T00:30:00Z");
        c.to = lm::UtcTime::fromIso("2026-01-01T10:05:00Z");
        r.coverage << c;
        const QString out = formatRange(QList<SubjectResult>() << r);
        QVERIFY(out.indexOf("115 mg/dL") < out.indexOf("120 mg/dL"));    // sorted by time
        QVERIFY(out.contains("3 received, 2 valid, 1 rejected"));
        QVERIFY(out.contains("coverage graph"));
        const QJsonObject s = rangeJson(QList<SubjectResult>() << r).value("subjects").toArray().at(0).toObject();
        QCOMPARE(s.value("records").toArray().size(), 2);
        QCOMPARE(s.value("records").toArray().at(0).toObject().value("value").toDouble(), 115.0);
        QCOMPARE(s.value("records_count").toObject().value("rejected").toInt(), 1);
        QCOMPARE(s.value("coverage").toArray().at(0).toObject().value("feed").toString(), QString("graph"));
        QVERIFY(s.value("watermark").isNull());
    }

    // ---- Hard violation: exit 21 without polling -------------------

    // An "adapter" that prints something that is not a protocol line and then waits. The
    // policy answers kill to the hard violation, the driver ends the process at once, and
    // the program exits with 21 instead of waiting for the hello timeout.
    void aHardViolationEndsTheRunWithExit21()
    {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        const QString adapter = dir.path() + "/adapter.sh";
        QVERIFY(writeFile(adapter, "#!/bin/sh\necho 'not a protocol line'\nexec sleep 30\n", 0700));
        const QString creds = dir.path() + "/creds";
        QVERIFY(writeFile(creds, "username=user@example.invalid\npassword=synthetic\n", 0600));
        Options o;
        QString e;
        QVERIFY(parseArguments(QStringList() << "discover" << "--credentials-file" << creds, &o, &e));

        CliRunner runner(o, adapter);
        QSignalSpy spy(&runner, SIGNAL(finished(int)));
        QTimer::singleShot(0, &runner, SLOT(run()));
        QVERIFY(spy.wait(8000));      // far below the 10 s hello timeout
        QCOMPARE(spy.at(0).at(0).toInt(), 21);
    }
};

QTEST_MAIN(TstCli)
#include "tst_cli.moc"
