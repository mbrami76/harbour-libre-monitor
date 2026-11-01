// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Michele Brami
#include "store/migration.h"

#include <QSqlDatabase>
#include <QSqlError>
#include <QSqlQuery>
#include <QVariant>

namespace lm {

namespace {

bool run(const QString &connection, const char *sql, QString *error)
{
    QSqlQuery q(QSqlDatabase::database(connection));
    if (!q.exec(QString::fromLatin1(sql))) {
        *error = q.lastError().text();
        return false;
    }
    return true;
}

qint64 count(const QString &connection, const char *table, bool *ok)
{
    QSqlQuery q(QSqlDatabase::database(connection));
    *ok = q.exec(QStringLiteral("SELECT COUNT(*) FROM ") + QLatin1String(table)) && q.next();
    return *ok ? q.value(0).toLongLong() : 0;
}

// Step 1 to 2: the application no longer writes archive files, so a new
// compaction record has no archive to name. In schema 1 the columns archive_id and
// archive_sha256 of the table `compaction` were required; in schema 2 they may be empty
// (NULL). Rows written under schema 1 keep their values: a record of what was done to the
// data is never changed or deleted, and the archive it names may still exist on the
// user's own storage. SQLite cannot drop a NOT NULL constraint in place, so the table is
// copied aside, created again and filled back; its two triggers are created again with it.
// The table `reimport` is left as it is (its rows are records of earlier reimports; the
// application no longer writes it). This text is frozen: it describes schema 1 and 2 as they
// were, and must not follow later changes of the schema in store.cpp.
bool schema1to2(const QString &connection, QString *error)
{
    bool ok = false;
    const qint64 before = count(connection, "compaction", &ok);
    if (!ok) {
        *error = QStringLiteral("the compaction table cannot be read");
        return false;
    }
    if (!run(connection, "CREATE TABLE compaction_copy AS SELECT id, time, range_from, range_to, removed,"
                         " archive_id, archive_sha256, rule_version FROM compaction", error)
        || !run(connection, "DROP TRIGGER compaction_no_delete", error)
        || !run(connection, "DROP TRIGGER compaction_no_update", error)
        || !run(connection, "DROP TABLE compaction", error)
        || !run(connection, "CREATE TABLE compaction("
                            " id TEXT PRIMARY KEY, time INTEGER NOT NULL, range_from INTEGER NOT NULL, range_to INTEGER NOT NULL,"
                            " removed TEXT NOT NULL, archive_id TEXT, archive_sha256 TEXT,"
                            " rule_version TEXT NOT NULL) WITHOUT ROWID", error)
        || !run(connection, "INSERT INTO compaction(id, time, range_from, range_to, removed, archive_id,"
                            " archive_sha256, rule_version) SELECT id, time, range_from, range_to, removed,"
                            " archive_id, archive_sha256, rule_version FROM compaction_copy", error)
        || !run(connection, "DROP TABLE compaction_copy", error)
        || !run(connection, "CREATE TRIGGER compaction_no_delete BEFORE DELETE ON compaction"
                            " BEGIN SELECT RAISE(ABORT, 'compaction records are never deleted'); END", error)
        || !run(connection, "CREATE TRIGGER compaction_no_update BEFORE UPDATE ON compaction"
                            " BEGIN SELECT RAISE(ABORT, 'compaction records are append-only'); END", error))
        return false;
    const qint64 after = count(connection, "compaction", &ok);
    if (!ok || after != before) {
        *error = QStringLiteral("the compaction records were not all copied");
        return false;
    }
    return true;
}

// Step 2 to 3: the purge of one Subject's old data (REQ-STO-17) may delete
// marks and sensor records, which schema 2 refused to delete in every case. The five delete
// triggers are created again: records, quarantine and the ingest log may go while the flag row
// 'compacting' or 'purging' is set; marks and sensor records only while 'purging' is set.
// No row changes. This text is frozen: it describes schema 3 as it was created.
bool schema2to3(const QString &connection, QString *error)
{
    const char *drop[] = { "record_no_delete", "quarantine_no_delete", "ingest_log_no_delete",
                           "mark_no_delete", "sensor_no_delete" };
    for (size_t i = 0; i < sizeof(drop) / sizeof(drop[0]); ++i)
        if (!run(connection, QByteArray("DROP TRIGGER ").append(drop[i]).constData(), error))
            return false;
    return run(connection, "CREATE TRIGGER record_no_delete BEFORE DELETE ON record"
                           " WHEN IFNULL((SELECT value FROM meta WHERE key='compacting'), '0') <> '1'"
                           " AND IFNULL((SELECT value FROM meta WHERE key='purging'), '0') <> '1'"
                           " BEGIN SELECT RAISE(ABORT, 'records are deleted only by compaction or a purge'); END", error)
        && run(connection, "CREATE TRIGGER quarantine_no_delete BEFORE DELETE ON quarantine"
                           " WHEN IFNULL((SELECT value FROM meta WHERE key='compacting'), '0') <> '1'"
                           " AND IFNULL((SELECT value FROM meta WHERE key='purging'), '0') <> '1'"
                           " BEGIN SELECT RAISE(ABORT, 'quarantine is deleted only by compaction or a purge'); END", error)
        && run(connection, "CREATE TRIGGER ingest_log_no_delete BEFORE DELETE ON ingest_log"
                           " WHEN IFNULL((SELECT value FROM meta WHERE key='compacting'), '0') <> '1'"
                           " AND IFNULL((SELECT value FROM meta WHERE key='purging'), '0') <> '1'"
                           " BEGIN SELECT RAISE(ABORT, 'the ingest log is deleted only by compaction or a purge'); END", error)
        && run(connection, "CREATE TRIGGER mark_no_delete BEFORE DELETE ON mark"
                           " WHEN IFNULL((SELECT value FROM meta WHERE key='purging'), '0') <> '1'"
                           " BEGIN SELECT RAISE(ABORT, 'marks are deleted only by a purge'); END", error)
        && run(connection, "CREATE TRIGGER sensor_no_delete BEFORE DELETE ON sensor"
                           " WHEN IFNULL((SELECT value FROM meta WHERE key='purging'), '0') <> '1'"
                           " BEGIN SELECT RAISE(ABORT, 'sensor records are deleted only by a purge'); END", error);
}

} // namespace

void MigrationRegistry::add(const MigrationStep &step)
{
    for (int i = 0; i < m_steps.size(); ++i) {
        if (m_steps[i].fromVersion == step.fromVersion) {
            m_steps[i] = step;
            return;
        }
    }
    m_steps.append(step);
}

const MigrationStep *MigrationRegistry::find(int fromVersion) const
{
    for (int i = 0; i < m_steps.size(); ++i)
        if (m_steps[i].fromVersion == fromVersion)
            return &m_steps[i];
    return 0;
}

const MigrationRegistry &MigrationRegistry::builtIn()
{
    static const MigrationRegistry registry = []() {
        MigrationRegistry r;
        MigrationStep s;
        s.fromVersion = 1;
        s.description = QStringLiteral("1 to 2: the archive of a compaction record may be empty");
        s.apply = schema1to2;
        r.add(s);
        MigrationStep t;
        t.fromVersion = 2;
        t.description = QStringLiteral("2 to 3: a purge may delete marks and sensor records");
        t.apply = schema2to3;
        r.add(t);
        return r;
    }();
    return registry;
}

MigrationResult migrateStore(const QString &connection, int fromVersion, int toVersion,
                             const MigrationRegistry &registry)
{
    MigrationResult result;
    result.version = fromVersion;
    if (fromVersion >= toVersion) {
        result.ok = true;                   // nothing to do
        return result;
    }
    // Every step must exist before anything changes.
    for (int v = fromVersion; v < toVersion; ++v) {
        if (!registry.find(v)) {
            result.error = QStringLiteral("no migration step from store schema ") + QString::number(v);
            return result;
        }
    }
    QSqlDatabase db = QSqlDatabase::database(connection);
    for (int v = fromVersion; v < toVersion; ++v) {
        const MigrationStep *step = registry.find(v);
        QString error;
        bool ok = db.transaction();
        if (!ok)
            error = db.lastError().text();
        if (ok)
            ok = step->apply(connection, &error);
        if (ok) {
            QSqlQuery q(db);
            q.prepare(QStringLiteral("UPDATE meta SET value=? WHERE key='store_schema'"));
            q.addBindValue(QString::number(v + 1));
            ok = q.exec() && q.numRowsAffected() == 1;
            if (!ok)
                error = q.lastError().text().isEmpty() ? QStringLiteral("the schema version cannot be written")
                                                       : q.lastError().text();
        }
        if (ok) {
            ok = db.commit();
            if (!ok)
                error = db.lastError().text();
        }
        if (!ok) {
            db.rollback();
            result.error = QStringLiteral("store schema step ") + QString::number(v) + QStringLiteral(" to ")
                + QString::number(v + 1) + QStringLiteral(" failed: ") + error;
            return result;
        }
        result.version = v + 1;
        result.applied << step->description;
    }
    result.ok = true;
    return result;
}

} // namespace lm
