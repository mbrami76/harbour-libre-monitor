// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Michele Brami
//
// Migration of the store's schema (REQ-STO-10). A phone keeps its store across upgrades,
// so a store written by an earlier version must open in this one with every row intact. These
// tests open the fixture store of each earlier schema version (tests/fixtures/store/vN), check
// that the migration keeps every row, gives the same shape as a new store and leaves a store
// that works; and they check the framework itself with test-only steps: one step at a time,
// a failing step rolled back, a missing step refused before anything changes. Synthetic data.
#include "record/registry.h"
#include "store/compactor.h"
#include "store/migration.h"
#include "store/store.h"

#include <QFile>
#include <QMap>
#include <QSqlDatabase>
#include <QSqlError>
#include <QSqlQuery>
#include <QSqlRecord>
#include <QTemporaryDir>
#include <QTextStream>
#include <QtTest>

using namespace lm;

namespace {

const QUuid kSubject("{d2f1c9a0-5b3e-4c7d-8a91-0e2b4c6d8f10}");
const qint64 kT0 = 1769904000000LL;                     // 2026-02-01T00:00:00Z, the fixture's slot
const qint64 kNow = 1781525400000LL;                    // 2026-06-15T12:10:00Z

// Runs "fn" on a raw Qt SQL connection to "path" (outside the Store, as a test tool).
template <typename Fn>
bool withRaw(const QString &path, Fn fn)
{
    bool ok = false;
    {
        QSqlDatabase db = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), QStringLiteral("raw"));
        db.setDatabaseName(path);
        if (db.open())
            ok = fn(db);
        db.close();
    }
    QSqlDatabase::removeDatabase(QStringLiteral("raw"));
    return ok;
}

// Creates the database "path" from a fixture file: one SQL statement per line, "--" comments.
bool loadFixture(const QString &fixture, const QString &path)
{
    QFile f(fixture);
    if (!f.open(QIODevice::ReadOnly | QIODevice::Text)) {
        qWarning("fixture missing: %s", qPrintable(fixture));
        return false;
    }
    QTextStream in(&f);
    in.setCodec("UTF-8");
    QStringList statements;
    while (!in.atEnd()) {
        const QString line = in.readLine().trimmed();
        if (!line.isEmpty() && !line.startsWith(QLatin1String("--")))
            statements << line;
    }
    return withRaw(path, [&](QSqlDatabase &db) {
        if (!db.transaction())
            return false;
        foreach (const QString &s, statements) {
            QSqlQuery q(db);
            if (!q.exec(s)) {
                qWarning("fixture statement failed: %s", qPrintable(q.lastError().text()));
                db.rollback();
                return false;
            }
        }
        return db.commit();
    });
}

QString fixture(int version)
{
    return QStringLiteral(LM_STORE_FIXTURES_DIR) + QStringLiteral("/v%1/store-v%1.sql").arg(version);
}

QStringList rows(const QString &path, const QString &sql)
{
    QStringList out;
    withRaw(path, [&](QSqlDatabase &db) {
        QSqlQuery q(db);
        if (!q.exec(sql))
            qWarning("%s", qPrintable(q.lastError().text()));
        while (q.next()) {
            QStringList cells;
            for (int i = 0; i < q.record().count(); ++i)
                cells << (q.value(i).isNull() ? QStringLiteral("NULL") : q.value(i).toString());
            out << cells.join(QLatin1Char('|'));
        }
        return true;
    });
    return out;
}

qint64 scalar(const QString &path, const QString &sql)
{
    const QStringList r = rows(path, sql);
    return r.isEmpty() ? -1 : r.first().toLongLong();
}

// The shape of a database: every table with its columns (name, type, not null, default,
// primary key), every index with its columns, every trigger with its text.
QStringList shape(const QString &path)
{
    QStringList out;
    const QStringList tables = rows(path, "SELECT name FROM sqlite_master WHERE type='table' ORDER BY name");
    foreach (const QString &t, tables) {
        const QStringList columns = rows(path, QStringLiteral("PRAGMA table_info(%1)").arg(t));
        foreach (const QString &c, columns)
            out << QStringLiteral("table %1: %2").arg(t, c);
    }
    const QStringList indexes = rows(path, "SELECT name, tbl_name FROM sqlite_master WHERE type='index'"
                                           " AND sql IS NOT NULL ORDER BY name");
    foreach (const QString &i, indexes) {
        const QStringList columns = rows(path, QStringLiteral("PRAGMA index_info(%1)").arg(i.section(QLatin1Char('|'), 0, 0)));
        foreach (const QString &c, columns)
            out << QStringLiteral("index %1: %2").arg(i, c);
    }
    const QStringList triggers = rows(path, "SELECT name, tbl_name, sql FROM sqlite_master WHERE type='trigger' ORDER BY name");
    foreach (const QString &tr, triggers)
        out << QStringLiteral("trigger ") + tr.simplified();
    return out;
}

// Runs migrateStore on a raw connection to "path".
MigrationResult migrate(const QString &path, int from, int to, const MigrationRegistry &reg)
{
    MigrationResult r;
    withRaw(path, [&](QSqlDatabase &db) {
        r = migrateStore(db.connectionName(), from, to, reg);
        return true;
    });
    return r;
}

} // namespace

class TstStoreMigration : public QObject
{
    Q_OBJECT

    QTemporaryDir *m_dir;

    QString path(const QString &name) const { return m_dir->path() + "/" + name; }

private slots:
    void init() { m_dir = new QTemporaryDir; QVERIFY(m_dir->isValid()); }
    void cleanup() { delete m_dir; }

    // The application ships a step and a fixture for every earlier version, and only those.
    void everyEarlierVersionHasItsStepAndFixture()
    {
        QCOMPARE(kStoreSchemaVersion, 3);
        const MigrationRegistry &r = MigrationRegistry::builtIn();
        QCOMPARE(r.count(), kStoreSchemaVersion - 1);
        for (int v = 1; v < kStoreSchemaVersion; ++v) {
            QVERIFY2(r.find(v), qPrintable(QString::number(v)));
            QVERIFY2(QFile::exists(fixture(v)), qPrintable(fixture(v)));
        }
    }

    // REQ-STO-10: the fixture store of schema 1 opens, is migrated to the current version, and
    // keeps every row of every table, the records of what was done to the data included.
    void theVersion1StoreOpensWithEveryRow()
    {
        const QString p = path("v1.db");
        QVERIFY(loadFixture(fixture(1), p));
        QCOMPARE(scalar(p, "SELECT value FROM meta WHERE key='store_schema'"), qint64(1));
        const QStringList oldCompaction = rows(p, "SELECT * FROM compaction");
        const QStringList oldReimport = rows(p, "SELECT * FROM reimport");
        QCOMPARE(oldCompaction.size(), 1);
        QCOMPARE(oldReimport.size(), 1);

        Store s;
        QVERIFY2(s.open(p), qPrintable(s.lastError()));
        QCOMPARE(s.schemaVersion(), kStoreSchemaVersion);
        QCOMPARE(s.rowCount("subject"), qint64(1));
        QCOMPARE(s.rowCount("record"), qint64(7));
        QCOMPARE(s.rowCount("mark"), qint64(1));
        QCOMPARE(s.rowCount("sensor"), qint64(1));
        QCOMPARE(s.rowCount("quarantine"), qint64(1));
        QCOMPARE(s.rowCount("ingest"), qint64(1));
        QCOMPARE(s.rowCount("compaction"), qint64(1));

        // The records read as they were written; the mark still hides its record.
        RecordQuery all;
        all.effective = false;
        const QVector<Record> raw = s.records(all);
        QCOMPARE(raw.size(), 7);
        RecordQuery effective;
        QCOMPARE(s.records(effective).size(), 6);
        QCOMPARE(raw.first().subject, kSubject);
        QCOMPARE(raw.first().value, 100.0);
        QCOMPARE(raw.first().unit, QString("mg/dL"));
        QCOMPARE(raw.first().measuredAt.msecsSinceEpoch(), kT0);
        QCOMPARE(raw.first().originalTimestamp, QString("2/1/2026 12:00:00 AM"));
        QVector<CompactionEntry> entries;
        QVERIFY(s.forEachCompaction([&](const CompactionEntry &e) { entries << e; }));
        QCOMPARE(entries.size(), 1);
        QCOMPARE(entries[0].ruleVersion, QString("1"));
        QCOMPARE(qint64(entries[0].removed.value("record").toDouble()), qint64(12));
        s.close();

        // Byte for byte, the old compaction and reimport records are still there.
        QCOMPARE(rows(p, "SELECT * FROM compaction"), oldCompaction);
        QCOMPARE(rows(p, "SELECT * FROM reimport"), oldReimport);
        QCOMPARE(scalar(p, "SELECT value FROM meta WHERE key='store_schema'"), qint64(kStoreSchemaVersion));
    }

    // A migrated store and a new one have the same tables, columns, indexes and triggers, so
    // that the rest of the code (and the next migration step) sees one shape per version.
    void aMigratedStoreHasTheShapeOfANewOne()
    {
        const QString migrated = path("v1.db");
        QVERIFY(loadFixture(fixture(1), migrated));
        {
            Store s;
            QVERIFY2(s.open(migrated), qPrintable(s.lastError()));
        }
        const QString fresh = path("new.db");
        {
            Store s;
            QVERIFY(s.open(fresh));
        }
        const QStringList a = shape(migrated);
        const QStringList b = shape(fresh);
        QVERIFY(a.size() > 30);
        foreach (const QString &x, a) if (!b.contains(x)) qWarning("only in the migrated store: %s", qPrintable(x));
        foreach (const QString &x, b) if (!a.contains(x)) qWarning("only in the new store: %s", qPrintable(x));
        QCOMPARE(a, b);
    }

    // The migrated store works: compaction thins the fixture's old slot, writes its record
    // without an archive, and the database still refuses to delete or change the old records.
    void aMigratedStoreCompactsAndKeepsItsHistory()
    {
        const QString p = path("v1.db");
        QVERIFY(loadFixture(fixture(1), p));
        Store s;
        QVERIFY2(s.open(p), qPrintable(s.lastError()));
        CompactionOptions o;
        o.now = UtcTime::fromMSecsSinceEpoch(kNow);
        o.freeSpace = [](const QString &) { return qint64(1) << 40; };
        const CompactionReport r = compactStore(s, o);
        QVERIFY2(r.ok, qPrintable(r.failure.message));
        // Six one-minute samples of the current feed: minutes 0 to 4 are one 5-minute slot, of
        // which minute 0 stays; minute 5 is alone in the next slot and stays; the event
        // stays; the old quarantine and ingest-log entries go.
        QCOMPARE(r.removed.value("record"), qint64(4));
        QCOMPARE(r.removed.value("quarantine"), qint64(1));
        QCOMPARE(r.removed.value("ingest"), qint64(1));
        QCOMPARE(s.rowCount("record"), qint64(3));
        QCOMPARE(s.rowCount("compaction"), qint64(2));
        QCOMPARE(s.rowCount("mark"), qint64(1));
        s.close();
        QCOMPARE(rows(p, "SELECT archive_id IS NULL, archive_sha256 IS NULL FROM compaction ORDER BY time"),
                 QStringList() << "0|0" << "1|1");             // the old one keeps its archive, the new one names none
        QVERIFY(withRaw(p, [](QSqlDatabase &db) {
            QSqlQuery q(db);
            return !q.exec("DELETE FROM compaction") && !q.exec("DELETE FROM reimport")
                && !q.exec("UPDATE compaction SET rule_version='2'");
        }));
    }

    // A reader (the window) opens an old store without changing it; the service migrates it.
    void aReaderLeavesAnOldStoreAsItIs()
    {
        const QString p = path("v1.db");
        QVERIFY(loadFixture(fixture(1), p));
        Store r;
        QVERIFY2(r.openReadOnly(p), qPrintable(r.lastError()));
        QCOMPARE(r.schemaVersion(), 1);
        QCOMPARE(r.recordCount(), qint64(7));
        QVector<CompactionEntry> entries;
        QVERIFY(r.forEachCompaction([&](const CompactionEntry &e) { entries << e; }));
        QCOMPARE(entries.size(), 1);
        CompactionEstimate e;
        QString error;
        QVERIFY2(estimateCompaction(r, FullResolutionWindow(), UtcTime::fromMSecsSinceEpoch(kNow), &e, &error,
                                    [](const QString &) { return qint64(1) << 40; }), qPrintable(error));
        QCOMPARE(e.records, qint64(4));
        r.close();
        QCOMPARE(scalar(p, "SELECT value FROM meta WHERE key='store_schema'"), qint64(1));
    }

    // A step that fails is rolled back: the store stays at the version before it, readable,
    // with every row, and the open says why. Here the store lacks a trigger the step expects.
    void aFailingRealStepLeavesTheOldStoreIntact()
    {
        const QString p = path("v1.db");
        QVERIFY(loadFixture(fixture(1), p));
        QVERIFY(withRaw(p, [](QSqlDatabase &db) { QSqlQuery q(db); return q.exec("DROP TRIGGER compaction_no_update"); }));
        const QStringList before = rows(p, "SELECT * FROM compaction");
        Store s;
        QVERIFY(!s.open(p));
        QVERIFY(!s.isOpen());
        QVERIFY2(s.lastError().contains("1 to 2"), qPrintable(s.lastError()));
        QCOMPARE(scalar(p, "SELECT value FROM meta WHERE key='store_schema'"), qint64(1));
        QCOMPARE(rows(p, "SELECT * FROM compaction"), before);
        QCOMPARE(scalar(p, "SELECT COUNT(*) FROM sqlite_master WHERE name='compaction_copy'"), qint64(0));
        QVERIFY(s.openReadOnly(p));
        QCOMPARE(s.recordCount(), qint64(7));
    }

    // REQ-STO-10 for schema 2: every row stays (the two compaction records, the reimport record),
    // and after the step 2 to 3 the database lets a purge, and only a purge, delete marks and
    // sensor records; compaction still cannot.
    void theVersion2StoreOpensWithEveryRowAndThePurgeTriggers()
    {
        const QString p = path("v2.db");
        QVERIFY(loadFixture(fixture(2), p));
        QCOMPARE(scalar(p, "SELECT value FROM meta WHERE key='store_schema'"), qint64(2));
        const QStringList tables = QStringList() << "subject" << "record" << "mark" << "sensor" << "quarantine"
                                                 << "ingest_log" << "compaction" << "reimport";
        QMap<QString, QStringList> before;
        foreach (const QString &t, tables)
            before[t] = rows(p, QStringLiteral("SELECT * FROM %1 ORDER BY 1").arg(t));
        {
            Store s;
            QVERIFY2(s.open(p), qPrintable(s.lastError()));
            QCOMPARE(s.schemaVersion(), 3);
            QCOMPARE(s.rowCount("compaction"), qint64(2));
        }
        foreach (const QString &t, tables)
            QCOMPARE(rows(p, QStringLiteral("SELECT * FROM %1 ORDER BY 1").arg(t)), before[t]);
        QVERIFY(withRaw(p, [](QSqlDatabase &db) {
            QSqlQuery q(db);
            bool ok = !q.exec("DELETE FROM mark") && !q.exec("DELETE FROM sensor");
            ok = ok && db.transaction();
            ok = ok && q.exec("INSERT OR REPLACE INTO meta VALUES('compacting', '1')");
            ok = ok && !q.exec("DELETE FROM mark") && !q.exec("DELETE FROM sensor") && q.exec("DELETE FROM quarantine");
            db.rollback();
            ok = ok && db.transaction();
            ok = ok && q.exec("INSERT OR REPLACE INTO meta VALUES('purging', '1')");
            ok = ok && q.exec("DELETE FROM mark") && q.exec("DELETE FROM sensor") && q.exec("DELETE FROM record")
                && q.exec("DELETE FROM quarantine") && q.exec("DELETE FROM ingest_log")
                && !q.exec("DELETE FROM compaction") && !q.exec("DELETE FROM reimport");
            db.rollback();
            return ok;
        }));
        QCOMPARE(rows(p, "SELECT * FROM mark ORDER BY 1"), before["mark"]);
    }

    void aMigratedVersion2StoreHasTheShapeOfANewOne()
    {
        const QString migrated = path("v2.db");
        QVERIFY(loadFixture(fixture(2), migrated));
        {
            Store s;
            QVERIFY2(s.open(migrated), qPrintable(s.lastError()));
        }
        const QString fresh = path("new.db");
        {
            Store s;
            QVERIFY(s.open(fresh));
        }
        QCOMPARE(shape(migrated), shape(fresh));
    }

    // A failing step 2 to 3 leaves the store at schema 2, intact.
    void aFailingStep2To3LeavesTheStoreAtVersion2()
    {
        const QString p = path("v2.db");
        QVERIFY(loadFixture(fixture(2), p));
        QVERIFY(withRaw(p, [](QSqlDatabase &db) { QSqlQuery q(db); return q.exec("DROP TRIGGER mark_no_delete"); }));
        Store s;
        QVERIFY(!s.open(p));
        QVERIFY2(s.lastError().contains("2 to 3"), qPrintable(s.lastError()));
        QCOMPARE(scalar(p, "SELECT value FROM meta WHERE key='store_schema'"), qint64(2));
        QCOMPARE(scalar(p, "SELECT COUNT(*) FROM sqlite_master WHERE type='trigger' AND name='record_no_delete'"
                           " AND sql LIKE '%purging%'"), qint64(0));
        QVERIFY(s.openReadOnly(p));
        QCOMPARE(s.recordCount(), qint64(7));
    }

    // The framework with test-only steps on a database of its own: steps run one after the
    // other, each with its version; a failing step stops the chain at the version before it;
    // a missing step is found before anything changes.
    void theFrameworkGoesOneStepAtATime()
    {
        const QString p = path("fake.db");
        QVERIFY(withRaw(p, [](QSqlDatabase &db) {
            QSqlQuery q(db);
            return q.exec("CREATE TABLE meta(key TEXT PRIMARY KEY, value TEXT NOT NULL)")
                && q.exec("INSERT INTO meta VALUES('store_schema', '1')")
                && q.exec("CREATE TABLE t(x INTEGER)") && q.exec("INSERT INTO t VALUES(1)");
        }));
        QStringList order;
        bool failSecond = true;
        MigrationRegistry reg;
        MigrationStep s1;
        s1.fromVersion = 1;
        s1.description = "1 to 2";
        s1.apply = [&](const QString &c, QString *error) {
            order << "1";
            QSqlQuery q(QSqlDatabase::database(c));
            if (!q.exec("ALTER TABLE t ADD COLUMN y INTEGER")) { *error = q.lastError().text(); return false; }
            return true;
        };
        MigrationStep s2;
        s2.fromVersion = 2;
        s2.description = "2 to 3";
        s2.apply = [&](const QString &c, QString *error) {
            order << "2";
            QSqlQuery q(QSqlDatabase::database(c));
            q.exec("CREATE TABLE u(z INTEGER)");
            if (failSecond) { *error = "synthetic failure"; return false; }
            return true;
        };
        reg.add(s1);

        // A missing step (2 to 3): refused before step 1 runs.
        MigrationResult r = migrate(p, 1, 3, reg);
        QVERIFY(!r.ok);
        QCOMPARE(r.version, 1);
        QVERIFY2(r.error.contains("from store schema 2"), qPrintable(r.error));
        QVERIFY(order.isEmpty());
        QCOMPARE(scalar(p, "SELECT value FROM meta WHERE key='store_schema'"), qint64(1));

        // Step 2 fails: step 1 is kept (version 2), step 2 is rolled back (no table u).
        reg.add(s2);
        r = migrate(p, 1, 3, reg);
        QVERIFY(!r.ok);
        QCOMPARE(r.version, 2);
        QCOMPARE(r.applied, QStringList() << "1 to 2");
        QVERIFY2(r.error.contains("synthetic failure"), qPrintable(r.error));
        QCOMPARE(order, QStringList() << "1" << "2");
        QCOMPARE(scalar(p, "SELECT value FROM meta WHERE key='store_schema'"), qint64(2));
        QCOMPARE(scalar(p, "SELECT COUNT(*) FROM sqlite_master WHERE name='u'"), qint64(0));
        QCOMPARE(scalar(p, "SELECT COUNT(*) FROM t"), qint64(1));

        // Again from version 2, with the step fixed: only step 2 runs.
        failSecond = false;
        order.clear();
        r = migrate(p, 2, 3, reg);
        QVERIFY2(r.ok, qPrintable(r.error));
        QCOMPARE(r.version, 3);
        QCOMPARE(order, QStringList() << "2");
        QCOMPARE(scalar(p, "SELECT value FROM meta WHERE key='store_schema'"), qint64(3));
        QCOMPARE(scalar(p, "SELECT COUNT(*) FROM sqlite_master WHERE name='u'"), qint64(1));

        // Nothing to do is a success that changes nothing.
        r = migrate(p, 3, 3, reg);
        QVERIFY(r.ok);
        QCOMPARE(r.version, 3);
        QCOMPARE(order, QStringList() << "2");
    }
};

QTEST_APPLESS_MAIN(TstStoreMigration)
#include "tst_storemigration.moc"
