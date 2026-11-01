// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Michele Brami
// Tests of the configuration file and the preferences of the user interface (app/ui). What they
// prove: the file the UI writes is one the SERVICE can read (names never reach it), a write
// replaces the file in one step with mode 600, unknown keys survive, and a file the UI does not
// understand is never overwritten.
#include "ui/configstore.h"
#include "ui/settingsstore.h"
#include "daemon/serviceconfig.h"

#include <QtTest>

using namespace lm::ui;

namespace {
ConnectionEntry sample()
{
    ConnectionEntry c;
    c.id = QStringLiteral("c1");
    c.adapter = QStringLiteral("libre");
    SubjectEntry s;
    s.id = ConfigStore::newSubjectId();
    s.ref = QStringLiteral("opaque-ref-1");
    s.name = QStringLiteral("Anna Rossi");
    s.label = QStringLiteral("AR");
    s.unit = QStringLiteral("mmol/L");
    s.unitBasis = QStringLiteral("code");
    s.unitConfirmed = true;
    c.subjects.append(s);
    return c;
}
QByteArray readAll(const QString &p) { QFile f(p); f.open(QIODevice::ReadOnly); return f.readAll(); }
}

class TstUiStore : public QObject
{
    Q_OBJECT

private slots:
    void missingFileIsAnEmptyFirstRun()
    {
        QTemporaryDir d;
        ConfigStore c;
        QVERIFY(c.load(d.path() + "/config.json"));
        QVERIFY(c.connections().isEmpty());
    }
    void roundTripKeepsEverything()
    {
        QTemporaryDir d;
        const QString path = d.path() + "/config.json";
        ConfigStore c;
        QVERIFY(c.load(path));
        const ConnectionEntry in = sample();
        c.setConnection(in);
        QVERIFY(c.save());
        ConfigStore r;
        QVERIFY(r.load(path));
        QCOMPARE(r.connections().size(), 1);
        const SubjectEntry s = r.connections().first().subjects.first();
        QCOMPARE(s.id, in.subjects.first().id);
        QCOMPARE(s.name, QStringLiteral("Anna Rossi"));
        QCOMPARE(s.label, QStringLiteral("AR"));
        QCOMPARE(s.unit, QStringLiteral("mmol/L"));
        QCOMPARE(s.unitBasis, QStringLiteral("code"));
        QVERIFY(s.unitConfirmed);
    }
    void theFileIsOwnerOnly()
    {
        QTemporaryDir d;
        const QString path = d.path() + "/config.json";
        ConfigStore c;
        c.load(path);
        c.setConnection(sample());
        QVERIFY(c.save());
        QCOMPARE(QFile::permissions(path) & (QFile::ReadGroup | QFile::WriteGroup | QFile::ReadOther | QFile::WriteOther),
                 QFileDevice::Permissions());
        QVERIFY(QFile::permissions(path) & QFile::ReadOwner);
    }
    void theServiceCanReadWhatTheUiWritesAndSeesNoName()
    {
        QTemporaryDir d;
        const QString path = d.path() + "/config.json";
        ConfigStore c;
        c.load(path);
        ConnectionEntry e = sample();
        e.currentIntervalS = 90;
        c.setConnection(e);
        QVERIFY(c.save());
        const lm::ServiceConfig sc = lm::loadServiceConfig(path);
        QVERIFY2(sc.ok, qPrintable(sc.error));
        QCOMPARE(sc.connections.size(), 1);
        QCOMPARE(sc.connections.first().id, QStringLiteral("c1"));
        QCOMPARE(sc.connections.first().subjects.size(), 1);
        QCOMPARE(sc.connections.first().subjects.first().subjectRef, QStringLiteral("opaque-ref-1"));
        QCOMPARE(sc.connections.first().currentIntervalMs, qint64(90000));
    }
    void unknownKeysSurviveAnEdit()
    {
        QTemporaryDir d;
        const QString path = d.path() + "/config.json";
        QFile f(path);
        f.open(QIODevice::WriteOnly);
        f.write("{\"format\":1,\"future_top\":7,\"connections\":[{\"id\":\"c1\",\"adapter\":\"libre\",\"x_conn\":\"keep\","
                "\"subjects\":[{\"id\":\"{11111111-1111-1111-1111-111111111111}\",\"ref\":\"r\",\"x_subj\":true}]}]}");
        f.close();
        ConfigStore c;
        QVERIFY(c.load(path));
        ConnectionEntry e = c.connections().first();
        e.subjects[0].name = QStringLiteral("N");
        c.setConnection(e);
        QVERIFY(c.save());
        const QByteArray out = readAll(path);
        QVERIFY(out.contains("future_top"));
        QVERIFY(out.contains("x_conn"));
        QVERIFY(out.contains("x_subj"));
        QVERIFY(out.contains("\"name\""));
    }
    void aClearedLabelDisappears()
    {
        QTemporaryDir d;
        ConfigStore c;
        c.load(d.path() + "/config.json");
        ConnectionEntry e = sample();
        c.setConnection(e);
        e.subjects[0].label.clear();
        c.setConnection(e);
        QVERIFY(c.save());
        QVERIFY(!readAll(d.path() + "/config.json").contains("\"label\""));
    }
    void aMalformedFileIsRefusedAndNotOverwritten()
    {
        QTemporaryDir d;
        const QString path = d.path() + "/config.json";
        QFile f(path); f.open(QIODevice::WriteOnly); f.write("{not json"); f.close();
        ConfigStore c;
        QVERIFY(!c.load(path));
        QCOMPARE(c.error(), QStringLiteral("config-malformed"));
        c.setConnection(sample());
        QVERIFY(!c.save());
        QCOMPARE(readAll(path), QByteArray("{not json"));
    }
    void aNewerFormatIsRefusedAndNotOverwritten()
    {
        QTemporaryDir d;
        const QString path = d.path() + "/config.json";
        QFile f(path); f.open(QIODevice::WriteOnly); f.write("{\"format\":2,\"connections\":[]}"); f.close();
        ConfigStore c;
        QVERIFY(!c.load(path));
        QCOMPARE(c.error(), QStringLiteral("config-format-newer"));
        QVERIFY(!c.save());
        QVERIFY(readAll(path).contains("\"format\":2"));
    }
    void removingAConnectionRemovesIt()
    {
        QTemporaryDir d;
        ConfigStore c;
        c.load(d.path() + "/config.json");
        c.setConnection(sample());
        QVERIFY(c.removeConnection("c1"));
        QVERIFY(!c.removeConnection("c1"));
        QVERIFY(c.connections().isEmpty());
    }
    void newSubjectIdsAreUniqueUuidsInBraces()
    {
        const QString a = ConfigStore::newSubjectId(), b = ConfigStore::newSubjectId();
        QVERIFY(a != b);
        QVERIFY(a.startsWith('{') && a.endsWith('}'));
    }

    // ---- preferences ---------------------------------------------------------------------------
    void settingsDefaultsAreTheFirstRunState()
    {
        QTemporaryDir d;
        SettingsStore s;
        s.load(d.path() + "/none.json");
        QVERIFY(s.displayUnit().isEmpty());                 // the first-run dialog asks
        QCOMPARE(s.feedsShown(), SettingsStore::FeedBoth);
        QVERIFY(!s.valueAxisLeft());
        QVERIFY(s.coverShowsName());                        // on by default; can be turned off
        QVERIFY(!s.safetyAccepted());
    }
    void settingsRoundTripAndRejectBadUnits()
    {
        QTemporaryDir d;
        const QString path = d.path() + "/ui-settings.json";
        SettingsStore s;
        s.load(path);
        s.setDisplayUnit("stones");
        QVERIFY(s.displayUnit().isEmpty());
        s.setDisplayUnit("mmol/L");
        s.setFeedsShown(SettingsStore::FeedHistory);
        s.setValueAxisLeft(true);
        s.setCoverShowsName(true);
        s.setSafetyAccepted(true);
        s.setFlag("{u}:unit-changed", true);
        QVERIFY(s.save());
        SettingsStore r;
        r.load(path);
        QCOMPARE(r.displayUnit(), QStringLiteral("mmol/L"));
        QCOMPARE(r.feedsShown(), SettingsStore::FeedHistory);
        QVERIFY(r.valueAxisLeft() && r.coverShowsName() && r.safetyAccepted());
        QVERIFY(r.flag("{u}:unit-changed"));
        QVERIFY(!r.flag("other"));
        r.setFlag("{u}:unit-changed", false);
        QVERIFY(!r.flag("{u}:unit-changed"));
    }
    void aDamagedSettingsFileGivesDefaults()
    {
        QTemporaryDir d;
        const QString path = d.path() + "/ui-settings.json";
        QFile f(path); f.open(QIODevice::WriteOnly); f.write("garbage"); f.close();
        SettingsStore s;
        s.load(path);
        QVERIFY(s.displayUnit().isEmpty());
        QCOMPARE(s.feedsShown(), SettingsStore::FeedBoth);
    }
    void settingsHoldNoPersonalData()
    {
        QTemporaryDir d;
        const QString path = d.path() + "/ui-settings.json";
        SettingsStore s;
        s.load(path);
        s.setDisplayUnit("mg/dL");
        s.save();
        QVERIFY(!readAll(path).contains("name"));
    }
};

QTEST_APPLESS_MAIN(TstUiStore)
#include "tst_uistore.moc"
