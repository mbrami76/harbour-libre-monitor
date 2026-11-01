// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Michele Brami
#include "store/store.h"

#include "store/dbutil.h"
#include "store/migration.h"

#include <QJsonDocument>
#include <QSqlDatabase>
#include <QSqlError>
#include <QSqlQuery>
#include <QVariant>

namespace lm {

namespace {

struct ReasonName { Validation::Reason reason; const char *name; };
const ReasonName kReasons[] = {
    { Validation::MissingSubject, "MissingSubject" },
    { Validation::UnknownMetric, "UnknownMetric" },
    { Validation::ShapeMismatch, "ShapeMismatch" },
    { Validation::MissingUnit, "MissingUnit" },
    { Validation::UnitNotAccepted, "UnitNotAccepted" },
    { Validation::ValueNotFinite, "ValueNotFinite" },
    { Validation::ValueOutOfRange, "ValueOutOfRange" },
    { Validation::InvalidPayload, "InvalidPayload" },
    { Validation::InvalidDuration, "InvalidDuration" },
    { Validation::MissingMeasurementTime, "MissingMeasurementTime" },
    { Validation::MeasurementTimeInFuture, "MeasurementTimeInFuture" },
    { Validation::MissingIngestTime, "MissingIngestTime" },
    { Validation::MissingOriginalTimestamp, "MissingOriginalTimestamp" },
    { Validation::MissingSource, "MissingSource" },
    { Validation::MissingEntryId, "MissingEntryId" },
    { Validation::MissingRuleVersion, "MissingRuleVersion" },
    { Validation::UndeclaredMetric, "UndeclaredMetric" },
    { Validation::UndeclaredUnit, "UndeclaredUnit" },
    { Validation::UndeclaredFeed, "UndeclaredFeed" },
    { Validation::MalformedMeasurementTime, "MalformedMeasurementTime" }
};

using dbutil::compactJson;
using dbutil::notNull;
using dbutil::nullable;
using dbutil::parseJson;
using dbutil::timeFrom;
using dbutil::timeOrNull;
using dbutil::uuidFrom;
using dbutil::uuidText;

Origin originFrom(const QString &name)
{
    if (name == QLatin1String("manual")) return Origin::Manual;
    if (name == QLatin1String("derived")) return Origin::Derived;
    return Origin::Adapter;
}

// The schema of a new store, version kStoreSchemaVersion (3). A store of an older version is
// brought to this shape by the steps of migration.cpp, and the test of the migration checks
// that both give the same tables, columns and triggers.
const char *kSchema[] = {
    "CREATE TABLE meta(key TEXT PRIMARY KEY, value TEXT NOT NULL) WITHOUT ROWID",
    "CREATE TABLE subject(id TEXT PRIMARY KEY) WITHOUT ROWID",
    "CREATE TABLE record("
    " subject TEXT NOT NULL, metric TEXT NOT NULL, measured_at INTEGER NOT NULL,"
    " source TEXT NOT NULL, entry_id TEXT NOT NULL DEFAULT '',"
    " shape INTEGER NOT NULL, value REAL, payload TEXT, duration_ms INTEGER, unit TEXT,"
    " ingested_at INTEGER NOT NULL, original_timestamp TEXT NOT NULL, origin TEXT NOT NULL,"
    " rule_version TEXT,"
    " PRIMARY KEY(subject, metric, measured_at, source, entry_id)) WITHOUT ROWID",
    "CREATE TABLE mark("
    " id TEXT PRIMARY KEY, time INTEGER NOT NULL,"
    " t_subject TEXT NOT NULL, t_metric TEXT NOT NULL, t_measured_at INTEGER NOT NULL,"
    " t_source TEXT NOT NULL, t_entry_id TEXT NOT NULL DEFAULT '',"
    " state TEXT NOT NULL, comment TEXT NOT NULL, author TEXT) WITHOUT ROWID",
    "CREATE INDEX mark_target ON mark(t_subject, t_metric, t_measured_at, t_source, t_entry_id)",
    "CREATE TABLE sensor("
    " subject TEXT NOT NULL, sensor_id TEXT NOT NULL, activated_at INTEGER,"
    " warmup_minutes INTEGER, model TEXT, first_seen INTEGER NOT NULL, last_seen INTEGER NOT NULL,"
    " PRIMARY KEY(subject, sensor_id)) WITHOUT ROWID",
    "CREATE TABLE quarantine("
    " id TEXT PRIMARY KEY, received_at INTEGER NOT NULL, adapter TEXT NOT NULL,"
    " adapter_version TEXT NOT NULL, reason TEXT NOT NULL, detail TEXT NOT NULL,"
    " record TEXT NOT NULL) WITHOUT ROWID",
    "CREATE TABLE ingest_log("
    " id TEXT PRIMARY KEY, time INTEGER NOT NULL, event TEXT NOT NULL, adapter TEXT NOT NULL,"
    " adapter_version TEXT NOT NULL, session_id TEXT NOT NULL, detail TEXT NOT NULL,"
    " extra TEXT NOT NULL) WITHOUT ROWID",
    // archive_id and archive_sha256 are set only on records written under schema 1, when a
    // compaction relied on an archive file; this version leaves them empty.
    "CREATE TABLE compaction("
    " id TEXT PRIMARY KEY, time INTEGER NOT NULL, range_from INTEGER NOT NULL, range_to INTEGER NOT NULL,"
    " removed TEXT NOT NULL, archive_id TEXT, archive_sha256 TEXT,"
    " rule_version TEXT NOT NULL) WITHOUT ROWID",
    // The reimport records of schema 1 (an archive read back into the store). This version
    // never writes the table; it stays so that a migrated store keeps those records and
    // a new store has the same shape as a migrated one (asks whether a
    // later schema may drop it).
    "CREATE TABLE reimport("
    " id TEXT PRIMARY KEY, time INTEGER NOT NULL, archive_id TEXT NOT NULL, archive_sha256 TEXT NOT NULL,"
    " format_version TEXT NOT NULL, counts TEXT NOT NULL) WITHOUT ROWID",
    "CREATE INDEX mark_time ON mark(time, id)",
    "CREATE INDEX quarantine_time ON quarantine(received_at, id)",
    "CREATE INDEX ingest_log_time ON ingest_log(time, id)",
    // Append-only (REQ-DAT-04): these rows are never changed. Deleting is left to compaction
    // (REQ-STO-06, recorded) and to the purge of one Subject's old data (REQ-STO-17,
    // which by design leaves no record).
    "CREATE TRIGGER record_no_update BEFORE UPDATE ON record"
    " BEGIN SELECT RAISE(ABORT, 'record is append-only'); END",
    "CREATE TRIGGER mark_no_update BEFORE UPDATE ON mark"
    " BEGIN SELECT RAISE(ABORT, 'mark is append-only'); END",
    "CREATE TRIGGER quarantine_no_update BEFORE UPDATE ON quarantine"
    " BEGIN SELECT RAISE(ABORT, 'quarantine is append-only'); END",
    "CREATE TRIGGER ingest_log_no_update BEFORE UPDATE ON ingest_log"
    " BEGIN SELECT RAISE(ABORT, 'ingest log is append-only'); END",
    // Nothing is deleted except by compaction or by a purge (REQ-DAT-04, REQ-STO-17): inside
    // their transaction a flag row of `meta` is set ('compacting' or 'purging'). Compaction
    // may then remove records, quarantine and the ingest log; a purge may also remove marks
    // and sensor records (schema 3). Compaction and reimport records never go. The text
    // of these triggers is repeated in the migration step 2 to 3 (migration.cpp).
    "CREATE TRIGGER record_no_delete BEFORE DELETE ON record"
    " WHEN IFNULL((SELECT value FROM meta WHERE key='compacting'), '0') <> '1'"
    " AND IFNULL((SELECT value FROM meta WHERE key='purging'), '0') <> '1'"
    " BEGIN SELECT RAISE(ABORT, 'records are deleted only by compaction or a purge'); END",
    "CREATE TRIGGER quarantine_no_delete BEFORE DELETE ON quarantine"
    " WHEN IFNULL((SELECT value FROM meta WHERE key='compacting'), '0') <> '1'"
    " AND IFNULL((SELECT value FROM meta WHERE key='purging'), '0') <> '1'"
    " BEGIN SELECT RAISE(ABORT, 'quarantine is deleted only by compaction or a purge'); END",
    "CREATE TRIGGER ingest_log_no_delete BEFORE DELETE ON ingest_log"
    " WHEN IFNULL((SELECT value FROM meta WHERE key='compacting'), '0') <> '1'"
    " AND IFNULL((SELECT value FROM meta WHERE key='purging'), '0') <> '1'"
    " BEGIN SELECT RAISE(ABORT, 'the ingest log is deleted only by compaction or a purge'); END",
    "CREATE TRIGGER mark_no_delete BEFORE DELETE ON mark"
    " WHEN IFNULL((SELECT value FROM meta WHERE key='purging'), '0') <> '1'"
    " BEGIN SELECT RAISE(ABORT, 'marks are deleted only by a purge'); END",
    "CREATE TRIGGER sensor_no_delete BEFORE DELETE ON sensor"
    " WHEN IFNULL((SELECT value FROM meta WHERE key='purging'), '0') <> '1'"
    " BEGIN SELECT RAISE(ABORT, 'sensor records are deleted only by a purge'); END",
    "CREATE TRIGGER compaction_no_delete BEFORE DELETE ON compaction"
    " BEGIN SELECT RAISE(ABORT, 'compaction records are never deleted'); END",
    "CREATE TRIGGER reimport_no_delete BEFORE DELETE ON reimport"
    " BEGIN SELECT RAISE(ABORT, 'reimport records are never deleted'); END",
    "CREATE TRIGGER compaction_no_update BEFORE UPDATE ON compaction"
    " BEGIN SELECT RAISE(ABORT, 'compaction records are append-only'); END",
    "CREATE TRIGGER reimport_no_update BEFORE UPDATE ON reimport"
    " BEGIN SELECT RAISE(ABORT, 'reimport records are append-only'); END"
};

const char *kRecordColumns =
    "subject, metric, measured_at, source, entry_id, shape, value, payload, duration_ms,"
    " unit, ingested_at, original_timestamp, origin, rule_version";

Record recordFromRow(QSqlQuery &q)
{
    Record r;
    r.subject = uuidFrom(q.value(0).toString());
    r.metric = q.value(1).toString();
    r.measuredAt = UtcTime::fromMSecsSinceEpoch(q.value(2).toLongLong());
    r.source = q.value(3).toString();
    r.entryId = q.value(4).toString();
    r.shape = q.value(5).toInt() == 0 ? Record::Sample : Record::Event;
    r.value = q.value(6).isNull() ? 0.0 : q.value(6).toDouble();
    r.payload = q.value(7).isNull() ? QJsonObject() : parseJson(q.value(7).toString());
    r.durationMs = q.value(8).isNull() ? -1 : q.value(8).toLongLong();
    r.unit = q.value(9).isNull() ? QString() : q.value(9).toString();
    r.ingestedAt = UtcTime::fromMSecsSinceEpoch(q.value(10).toLongLong());
    r.originalTimestamp = q.value(11).toString();
    r.origin = originFrom(q.value(12).toString());
    r.ruleVersion = q.value(13).isNull() ? QString() : q.value(13).toString();
    return r;
}

} // namespace

QString validationReasonName(Validation::Reason reason)
{
    for (size_t i = 0; i < sizeof(kReasons) / sizeof(kReasons[0]); ++i)
        if (kReasons[i].reason == reason)
            return QString::fromLatin1(kReasons[i].name);
    return QString();
}

bool validationReasonFromName(const QString &name, Validation::Reason *reason)
{
    for (size_t i = 0; i < sizeof(kReasons) / sizeof(kReasons[0]); ++i)
        if (name == QLatin1String(kReasons[i].name)) {
            *reason = kReasons[i].reason;
            return true;
        }
    return false;
}

// Writes records with the key rules of REQ-DAT-06 over prepared statements, inside a
// transaction the caller owns: a new key is added; the same key with the same value and unit
// (or payload) is a duplicate and changes nothing; the same key with another value is a
// conflict and the first record stays (append-only, REQ-DAT-04).
class RecordWriter
{
public:
    enum Outcome { Added, Duplicate, Conflict, Failed };

    explicit RecordWriter(const QSqlDatabase &db) : m_sel(db), m_ins(db), m_subject(db)
    {
        m_sel.prepare(QStringLiteral("SELECT value, unit, payload FROM record WHERE subject=? AND metric=?"
                                     " AND measured_at=? AND source=? AND entry_id=?"));
        m_ins.prepare(QStringLiteral("INSERT INTO record(") + QLatin1String(kRecordColumns)
                      + QStringLiteral(") VALUES(?,?,?,?,?,?,?,?,?,?,?,?,?,?)"));
        m_subject.prepare(QStringLiteral("INSERT OR IGNORE INTO subject(id) VALUES(?)"));
    }

    // Failed sets *error.
    Outcome put(const Record &r, QString *error)
    {
        const RecordKey key = r.key();
        const QString payload = r.shape == Record::Event ? compactJson(r.payload) : QString();

        m_sel.addBindValue(uuidText(key.subject));
        m_sel.addBindValue(key.metric);
        m_sel.addBindValue(qlonglong(key.measuredAtMs));
        m_sel.addBindValue(key.source);
        m_sel.addBindValue(notNull(key.entryId));
        if (!m_sel.exec()) {
            *error = m_sel.lastError().text();
            return Failed;
        }
        if (m_sel.next()) {
            const bool same = r.shape == Record::Sample
                ? (m_sel.value(0).toDouble() == r.value && m_sel.value(1).toString() == r.unit)
                : (m_sel.value(2).toString() == payload && m_sel.value(1).toString() == r.unit);
            m_sel.finish();
            return same ? Duplicate : Conflict;
        }
        m_sel.finish();

        m_subject.addBindValue(uuidText(r.subject));
        if (!m_subject.exec()) {
            *error = m_subject.lastError().text();
            return Failed;
        }
        m_ins.addBindValue(uuidText(key.subject));
        m_ins.addBindValue(key.metric);
        m_ins.addBindValue(qlonglong(key.measuredAtMs));
        m_ins.addBindValue(key.source);
        m_ins.addBindValue(notNull(key.entryId));
        m_ins.addBindValue(r.shape == Record::Sample ? 0 : 1);
        m_ins.addBindValue(r.shape == Record::Sample ? QVariant(r.value) : QVariant(QVariant::Double));
        m_ins.addBindValue(r.shape == Record::Event ? QVariant(payload) : QVariant(QVariant::String));
        m_ins.addBindValue(r.durationMs >= 0 ? QVariant(qlonglong(r.durationMs)) : QVariant(QVariant::LongLong));
        m_ins.addBindValue(r.unit.isEmpty() ? QVariant(QVariant::String) : QVariant(r.unit));
        m_ins.addBindValue(qlonglong(r.ingestedAt.msecsSinceEpoch()));
        m_ins.addBindValue(notNull(r.originalTimestamp));
        m_ins.addBindValue(originName(r.origin));
        m_ins.addBindValue(r.ruleVersion.isEmpty() ? QVariant(QVariant::String) : QVariant(r.ruleVersion));
        if (!m_ins.exec()) {
            *error = m_ins.lastError().text();
            return Failed;
        }
        return Added;
    }

private:
    QSqlQuery m_sel;
    QSqlQuery m_ins;
    QSqlQuery m_subject;
};

class StorePrivate
{
public:
    StorePrivate() : schema(0), readOnly(false), snapshot(false), compacting(false), purging(false),
        deleteRecord(0), deleteQuarantine(0), deleteIngest(0), deleteMark(0), deleteSensor(0) {}
    ~StorePrivate() { delete deleteRecord; delete deleteQuarantine; delete deleteIngest; delete deleteMark; delete deleteSensor; }

    bool exec(const QString &sql);
    bool fail(const QString &message);
    bool appendIngestEntry(const IngestEntry &entry);
    bool appendQuarantine(const QuarantineEntry &entry);
    bool ensureSubject(const QUuid &subject);

    QString connection;
    QString path;
    QString error;
    int schema;
    bool readOnly;
    bool snapshot;
    bool compacting;            // a deleting transaction (compaction or purge) is open
    bool purging;               // ... and it is a purge
    QSqlQuery *deleteRecord;
    QSqlQuery *deleteQuarantine;
    QSqlQuery *deleteIngest;
    QSqlQuery *deleteMark;
    QSqlQuery *deleteSensor;
};

bool StorePrivate::fail(const QString &message)
{
    error = message;
    return false;
}

bool StorePrivate::exec(const QString &sql)
{
    QSqlQuery q(QSqlDatabase::database(connection));
    if (!q.exec(sql))
        return fail(q.lastError().text());
    return true;
}

bool StorePrivate::ensureSubject(const QUuid &subject)
{
    QSqlQuery q(QSqlDatabase::database(connection));
    q.prepare(QStringLiteral("INSERT OR IGNORE INTO subject(id) VALUES(?)"));
    q.addBindValue(uuidText(subject));
    return q.exec() || fail(q.lastError().text());
}

bool StorePrivate::appendIngestEntry(const IngestEntry &entry)
{
    QSqlQuery q(QSqlDatabase::database(connection));
    q.prepare(QStringLiteral("INSERT OR IGNORE INTO ingest_log VALUES(?,?,?,?,?,?,?,?)"));
    q.addBindValue(uuidText(entry.id.isNull() ? QUuid::createUuid() : entry.id));
    q.addBindValue(qlonglong(entry.time.msecsSinceEpoch()));
    q.addBindValue(notNull(entry.event));
    q.addBindValue(notNull(entry.adapter));
    q.addBindValue(notNull(entry.adapterVersion));
    q.addBindValue(notNull(entry.sessionId));
    q.addBindValue(notNull(entry.detail));
    q.addBindValue(compactJson(entry.extra));
    return q.exec() || fail(q.lastError().text());
}

bool StorePrivate::appendQuarantine(const QuarantineEntry &entry)
{
    QSqlQuery q(QSqlDatabase::database(connection));
    q.prepare(QStringLiteral("INSERT OR IGNORE INTO quarantine VALUES(?,?,?,?,?,?,?)"));
    q.addBindValue(uuidText(entry.id.isNull() ? QUuid::createUuid() : entry.id));
    q.addBindValue(qlonglong(entry.receivedAt.msecsSinceEpoch()));
    q.addBindValue(notNull(entry.adapter));
    q.addBindValue(notNull(entry.adapterVersion));
    q.addBindValue(notNull(entry.reason));
    q.addBindValue(notNull(entry.detail));
    q.addBindValue(compactJson(entry.record));
    return q.exec() || fail(q.lastError().text());
}

Store::Store() : d(new StorePrivate) {}

Store::~Store()
{
    close();
    delete d;
}

bool Store::isOpen() const { return !d->connection.isEmpty(); }
QString Store::lastError() const { return d->error; }
int Store::schemaVersion() const { return d->schema; }

QString Store::path() const { return d->path; }

qint64 Store::sizeBytes() const
{
    return d->connection.isEmpty() ? 0 : dbutil::sizeBytes(d->connection, d->path);
}

static bool openStore(StorePrivate *d, const QString &path, bool readOnly)
{
    const char *const *schema = kSchema;
    const int count = int(sizeof(kSchema) / sizeof(kSchema[0]));
    int version = 0;
    QString error;
    const QString name = dbutil::openDatabase(path, readOnly, schema, count, kStoreSchemaVersion,
                                              &error, &version);
    if (name.isEmpty()) {
        d->error = error;
        return false;
    }
    // A store of an older version is brought up to date by the writer, step by step (REQ-STO-10).
    // A reader leaves it as it is: it only uses what the versions share.
    if (!readOnly && version < kStoreSchemaVersion) {
        const MigrationResult m = migrateStore(name, version, kStoreSchemaVersion, MigrationRegistry::builtIn());
        if (!m.ok) {
            dbutil::closeDatabase(name);
            d->error = m.error;
            return false;
        }
        version = m.version;
    }
    d->connection = name;
    d->path = path;
    d->schema = version;
    d->readOnly = readOnly;
    d->error.clear();
    return true;
}

bool Store::open(const QString &path)
{
    close();
    return openStore(d, path, false);
}

bool Store::openReadOnly(const QString &path)
{
    close();
    return openStore(d, path, true);
}

void Store::close()
{
    if (d->connection.isEmpty())
        return;
    const QString name = d->connection;
    d->connection.clear();
    d->path.clear();
    d->schema = 0;
    d->snapshot = false;
    delete d->deleteRecord;
    delete d->deleteQuarantine;
    delete d->deleteIngest;
    delete d->deleteMark;
    delete d->deleteSensor;
    d->deleteRecord = d->deleteQuarantine = d->deleteIngest = d->deleteMark = d->deleteSensor = 0;
    d->compacting = false;
    d->purging = false;
    dbutil::closeDatabase(name);
}

IngestResult Store::ingest(const QVector<IngestItem> &items, const IngestContext &context)
{
    IngestResult result;
    if (!isOpen()) {
        d->error = QStringLiteral("the store is not open");
        return result;
    }
    QSqlDatabase db = QSqlDatabase::database(d->connection);
    if (!db.transaction()) {
        d->error = db.lastError().text();
        return result;
    }

    bool ok = true;
    RecordWriter writer(db);
    for (int i = 0; ok && i < items.size(); ++i) {
        const IngestItem &item = items.at(i);
        Validation v = item.rejected;
        if (v.ok())
            v = validateRecord(item.record, context.now, context.clockSkewToleranceMs);

        if (!v.ok()) {
            QuarantineEntry entry;
            entry.receivedAt = context.now;
            entry.adapter = context.adapter;
            entry.adapterVersion = context.adapterVersion;
            entry.reason = validationReasonName(v.reason);
            entry.detail = v.detail;
            entry.record = item.raw;
            ok = d->appendQuarantine(entry);
            ++result.quarantined;
            continue;
        }

        QString error;
        const RecordWriter::Outcome outcome = writer.put(item.record, &error);
        if (outcome == RecordWriter::Failed) {
            ok = d->fail(error);
            break;
        }
        if (outcome == RecordWriter::Added) {
            ++result.added;
        } else if (outcome == RecordWriter::Duplicate) {
            ++result.duplicate;
        } else {
            ++result.conflict;
            IngestEntry log;
            log.time = context.now;
            log.event = QStringLiteral("conflict");
            log.adapter = context.adapter;
            log.adapterVersion = context.adapterVersion;
            log.sessionId = context.sessionId;
            log.detail = QStringLiteral("record key exists with a different value, unit or payload");
            ok = d->appendIngestEntry(log);
        }
    }

    if (ok && !db.commit())
        ok = d->fail(db.lastError().text());
    if (!ok) {
        db.rollback();
        return IngestResult();
    }
    result.ok = true;
    d->error.clear();
    return result;
}

bool Store::appendMark(const Mark &mark)
{
    if (!isOpen()) return d->fail(QStringLiteral("the store is not open"));
    if (mark.comment.trimmed().isEmpty())
        return d->fail(QStringLiteral("a mark needs a comment"));
    if (!mark.time.isValid() || mark.target.subject.isNull() || mark.target.metric.isEmpty()
        || mark.target.source.isEmpty())
        return d->fail(QStringLiteral("a mark needs a time and a complete target"));

    QSqlQuery q(QSqlDatabase::database(d->connection));
    q.prepare(QStringLiteral("INSERT OR IGNORE INTO mark VALUES(?,?,?,?,?,?,?,?,?,?)"));
    q.addBindValue(uuidText(mark.id.isNull() ? QUuid::createUuid() : mark.id));
    q.addBindValue(qlonglong(mark.time.msecsSinceEpoch()));
    q.addBindValue(uuidText(mark.target.subject));
    q.addBindValue(mark.target.metric);
    q.addBindValue(qlonglong(mark.target.measuredAtMs));
    q.addBindValue(mark.target.source);
    q.addBindValue(notNull(mark.target.entryId));
    q.addBindValue(mark.ignored ? QStringLiteral("ignored") : QStringLiteral("active"));
    q.addBindValue(mark.comment);
    q.addBindValue(nullable(mark.author));
    if (!q.exec())
        return d->fail(q.lastError().text());
    d->error.clear();
    return true;
}

bool Store::appendIngestEntry(const IngestEntry &entry)
{
    if (!isOpen()) return d->fail(QStringLiteral("the store is not open"));
    if (!entry.time.isValid()) return d->fail(QStringLiteral("an ingest entry needs a time"));
    if (!d->appendIngestEntry(entry)) return false;
    d->error.clear();
    return true;
}

bool Store::noteSensor(const Sensor &s)
{
    if (!isOpen()) return d->fail(QStringLiteral("the store is not open"));
    if (s.subject.isNull() || s.sensorId.isEmpty() || !s.firstSeen.isValid() || !s.lastSeen.isValid())
        return d->fail(QStringLiteral("a sensor needs a Subject, an id and first and last seen"));

    QSqlDatabase db = QSqlDatabase::database(d->connection);
    if (!db.transaction()) return d->fail(db.lastError().text());
    bool ok = d->ensureSubject(s.subject);
    if (ok) {
        QSqlQuery q(db);
        q.prepare(QStringLiteral("INSERT OR IGNORE INTO sensor VALUES(?,?,?,?,?,?,?)"));
        q.addBindValue(uuidText(s.subject));
        q.addBindValue(s.sensorId);
        q.addBindValue(timeOrNull(s.activatedAt));
        q.addBindValue(s.warmupMinutes >= 0 ? QVariant(s.warmupMinutes) : QVariant(QVariant::Int));
        q.addBindValue(nullable(s.model));
        q.addBindValue(qlonglong(s.firstSeen.msecsSinceEpoch()));
        q.addBindValue(qlonglong(s.lastSeen.msecsSinceEpoch()));
        ok = q.exec() || d->fail(q.lastError().text());
    }
    if (ok) {
        QSqlQuery q(db);
        q.prepare(QStringLiteral("UPDATE sensor SET first_seen=MIN(first_seen, ?), last_seen=MAX(last_seen, ?)"
                                 " WHERE subject=? AND sensor_id=?"));
        q.addBindValue(qlonglong(s.firstSeen.msecsSinceEpoch()));
        q.addBindValue(qlonglong(s.lastSeen.msecsSinceEpoch()));
        q.addBindValue(uuidText(s.subject));
        q.addBindValue(s.sensorId);
        ok = q.exec() || d->fail(q.lastError().text());
    }
    if (ok && !db.commit())
        ok = d->fail(db.lastError().text());
    if (!ok) {
        db.rollback();
        return false;
    }
    d->error.clear();
    return true;
}

namespace {

// REQ-DAT-07: a record is hidden when the latest mark for its key says "ignored".
const char *kEffectiveFilter =
    " AND NOT EXISTS (SELECT 1 FROM mark m WHERE m.t_subject=r.subject AND m.t_metric=r.metric"
    " AND m.t_measured_at=r.measured_at AND m.t_source=r.source AND m.t_entry_id=r.entry_id"
    " AND m.state='ignored' AND NOT EXISTS (SELECT 1 FROM mark m2"
    " WHERE m2.t_subject=m.t_subject AND m2.t_metric=m.t_metric AND m2.t_measured_at=m.t_measured_at"
    " AND m2.t_source=m.t_source AND m2.t_entry_id=m.t_entry_id"
    " AND (m2.time>m.time OR (m2.time=m.time AND m2.id>m.id))))";

} // namespace

QVector<Record> Store::records(const RecordQuery &query)
{
    QVector<Record> out;
    if (!isOpen()) { d->fail(QStringLiteral("the store is not open")); return out; }

    QString sql = QStringLiteral("SELECT ") + QLatin1String(kRecordColumns) + QStringLiteral(" FROM record r WHERE 1=1");
    QVariantList args;
    if (!query.subject.isNull()) { sql += QStringLiteral(" AND r.subject=?"); args << uuidText(query.subject); }
    if (!query.metric.isEmpty()) { sql += QStringLiteral(" AND r.metric=?"); args << query.metric; }
    if (!query.source.isEmpty()) { sql += QStringLiteral(" AND r.source=?"); args << query.source; }
    if (query.from.isValid()) { sql += QStringLiteral(" AND r.measured_at>=?"); args << qlonglong(query.from.msecsSinceEpoch()); }
    if (query.to.isValid()) { sql += QStringLiteral(" AND r.measured_at<=?"); args << qlonglong(query.to.msecsSinceEpoch()); }
    if (query.effective) sql += QLatin1String(kEffectiveFilter);
    sql += query.newestFirst
        ? QStringLiteral(" ORDER BY r.measured_at DESC, r.source DESC, r.entry_id DESC")
        : QStringLiteral(" ORDER BY r.measured_at, r.source, r.entry_id");
    if (query.limit > 0) sql += QStringLiteral(" LIMIT ") + QString::number(query.limit);

    QSqlQuery q(QSqlDatabase::database(d->connection));
    q.setForwardOnly(true);
    q.prepare(sql);
    foreach (const QVariant &a, args) q.addBindValue(a);
    if (!q.exec()) { d->fail(q.lastError().text()); return out; }
    while (q.next())
        out.append(recordFromRow(q));
    return out;
}

bool Store::latest(const QUuid &subject, const QString &metric, const QString &source,
                   bool effective, Record *out)
{
    RecordQuery q;
    q.subject = subject;
    q.metric = metric;
    q.source = source;
    q.effective = effective;
    q.limit = 1;
    q.newestFirst = true;
    const QVector<Record> found = records(q);
    if (found.isEmpty())
        return false;
    *out = found.first();
    return true;
}

QList<QUuid> Store::subjects()
{
    QList<QUuid> out;
    if (!isOpen()) return out;
    QSqlQuery q(QSqlDatabase::database(d->connection));
    q.exec(QStringLiteral("SELECT id FROM subject ORDER BY id"));
    while (q.next())
        out.append(uuidFrom(q.value(0).toString()));
    return out;
}

SubjectStats Store::subjectStats(const QUuid &subject)
{
    SubjectStats s;
    if (!isOpen()) return s;
    QSqlQuery q(QSqlDatabase::database(d->connection));
    q.prepare(QStringLiteral("SELECT MIN(measured_at), MAX(measured_at), COUNT(*) FROM record WHERE subject=?"));
    q.addBindValue(uuidText(subject));
    if (q.exec() && q.next() && q.value(2).toLongLong() > 0) {
        s.first = timeFrom(q.value(0));
        s.last = timeFrom(q.value(1));
        s.records = q.value(2).toLongLong();
    }
    return s;
}

qint64 Store::recordCount()
{
    if (!isOpen()) return 0;
    QSqlQuery q(QSqlDatabase::database(d->connection));
    q.exec(QStringLiteral("SELECT COUNT(*) FROM record"));
    return q.next() ? q.value(0).toLongLong() : 0;
}

namespace {

Mark markFromRow(QSqlQuery &q)
{
    Mark m;
    m.id = uuidFrom(q.value(0).toString());
    m.time = timeFrom(q.value(1));
    m.target.subject = uuidFrom(q.value(2).toString());
    m.target.metric = q.value(3).toString();
    m.target.measuredAtMs = q.value(4).toLongLong();
    m.target.source = q.value(5).toString();
    m.target.entryId = q.value(6).toString();
    m.ignored = q.value(7).toString() == QLatin1String("ignored");
    m.comment = q.value(8).toString();
    m.author = q.value(9).isNull() ? QString() : q.value(9).toString();
    return m;
}

Sensor sensorFromRow(QSqlQuery &q)
{
    Sensor s;
    s.subject = uuidFrom(q.value(0).toString());
    s.sensorId = q.value(1).toString();
    s.activatedAt = timeFrom(q.value(2));
    s.warmupMinutes = q.value(3).isNull() ? -1 : q.value(3).toInt();
    s.model = q.value(4).isNull() ? QString() : q.value(4).toString();
    s.firstSeen = timeFrom(q.value(5));
    s.lastSeen = timeFrom(q.value(6));
    return s;
}

QuarantineEntry quarantineFromRow(QSqlQuery &q)
{
    QuarantineEntry e;
    e.id = uuidFrom(q.value(0).toString());
    e.receivedAt = timeFrom(q.value(1));
    e.adapter = q.value(2).toString();
    e.adapterVersion = q.value(3).toString();
    e.reason = q.value(4).toString();
    e.detail = q.value(5).toString();
    e.record = parseJson(q.value(6).toString());
    return e;
}

IngestEntry ingestFromRow(QSqlQuery &q)
{
    IngestEntry e;
    e.id = uuidFrom(q.value(0).toString());
    e.time = timeFrom(q.value(1));
    e.event = q.value(2).toString();
    e.adapter = q.value(3).toString();
    e.adapterVersion = q.value(4).toString();
    e.sessionId = q.value(5).toString();
    e.detail = q.value(6).toString();
    e.extra = parseJson(q.value(7).toString());
    return e;
}

CompactionEntry compactionFromRow(QSqlQuery &q)
{
    CompactionEntry e;
    e.id = uuidFrom(q.value(0).toString());
    e.time = timeFrom(q.value(1));
    e.rangeFrom = timeFrom(q.value(2));
    e.rangeTo = timeFrom(q.value(3));
    e.removed = parseJson(q.value(4).toString());
    e.ruleVersion = q.value(5).toString();
    return e;
}

// Runs a SELECT forward-only and hands each row to fn: nothing is kept in memory.
template <typename Fn>
bool iterate(StorePrivate *d, const QString &sql, Fn fn)
{
    if (d->connection.isEmpty())
        return d->fail(QStringLiteral("the store is not open"));
    QSqlQuery q(QSqlDatabase::database(d->connection));
    q.setForwardOnly(true);
    if (!q.exec(sql))
        return d->fail(q.lastError().text());
    while (q.next())
        fn(q);
    return true;
}

} // namespace

bool Store::forEachRecord(const RecordFn &fn)
{
    return iterate(d, QStringLiteral("SELECT ") + QLatin1String(kRecordColumns)
                   + QStringLiteral(" FROM record ORDER BY subject, metric, measured_at, source, entry_id"),
                   [&](QSqlQuery &q) { fn(recordFromRow(q)); });
}

bool Store::forEachMark(const MarkFn &fn)
{
    return iterate(d, QStringLiteral("SELECT id, time, t_subject, t_metric, t_measured_at, t_source,"
                                     " t_entry_id, state, comment, author FROM mark ORDER BY time, id"),
                   [&](QSqlQuery &q) { fn(markFromRow(q)); });
}

bool Store::forEachSensor(const SensorFn &fn)
{
    return iterate(d, QStringLiteral("SELECT subject, sensor_id, activated_at, warmup_minutes, model,"
                                     " first_seen, last_seen FROM sensor ORDER BY subject, sensor_id"),
                   [&](QSqlQuery &q) { fn(sensorFromRow(q)); });
}

bool Store::forEachQuarantine(const QuarantineFn &fn)
{
    return iterate(d, QStringLiteral("SELECT id, received_at, adapter, adapter_version, reason, detail,"
                                     " record FROM quarantine ORDER BY received_at, id"),
                   [&](QSqlQuery &q) { fn(quarantineFromRow(q)); });
}

bool Store::forEachIngestEntry(const IngestFn &fn)
{
    return iterate(d, QStringLiteral("SELECT id, time, event, adapter, adapter_version, session_id,"
                                     " detail, extra FROM ingest_log ORDER BY time, id"),
                   [&](QSqlQuery &q) { fn(ingestFromRow(q)); });
}

bool Store::forEachCompaction(const CompactionFn &fn)
{
    return iterate(d, QStringLiteral("SELECT id, time, range_from, range_to, removed, rule_version"
                                     " FROM compaction ORDER BY time, id"),
                   [&](QSqlQuery &q) { fn(compactionFromRow(q)); });
}

QVector<Mark> Store::marks()
{
    QVector<Mark> out;
    forEachMark([&](const Mark &m) { out.append(m); });
    return out;
}

QVector<Sensor> Store::sensors()
{
    QVector<Sensor> out;
    forEachSensor([&](const Sensor &s) { out.append(s); });
    return out;
}

QVector<QuarantineEntry> Store::quarantine()
{
    QVector<QuarantineEntry> out;
    forEachQuarantine([&](const QuarantineEntry &e) { out.append(e); });
    return out;
}

QVector<IngestEntry> Store::ingestLog()
{
    QVector<IngestEntry> out;
    forEachIngestEntry([&](const IngestEntry &e) { out.append(e); });
    return out;
}

bool Store::beginSnapshot()
{
    if (!isOpen()) return d->fail(QStringLiteral("the store is not open"));
    if (d->snapshot) return d->fail(QStringLiteral("a snapshot is already open"));
    QSqlDatabase db = QSqlDatabase::database(d->connection);
    if (!db.transaction()) return d->fail(db.lastError().text());
    // A deferred transaction starts its snapshot at the first read.
    QSqlQuery q(db);
    if (!q.exec(QStringLiteral("SELECT COUNT(*) FROM subject"))) {
        db.rollback();
        return d->fail(q.lastError().text());
    }
    d->snapshot = true;
    return true;
}

void Store::endSnapshot()
{
    if (!isOpen() || !d->snapshot)
        return;
    QSqlDatabase::database(d->connection).rollback();
    d->snapshot = false;
}

qint64 Store::rowCount(const QString &type)
{
    static const char *tables[][2] = {
        { "subject", "subject" }, { "record", "record" }, { "mark", "mark" }, { "sensor", "sensor" },
        { "quarantine", "quarantine" }, { "ingest", "ingest_log" }, { "compaction", "compaction" }
    };
    if (!isOpen()) return 0;
    for (size_t i = 0; i < sizeof(tables) / sizeof(tables[0]); ++i) {
        if (type == QLatin1String(tables[i][0])) {
            QSqlQuery q(QSqlDatabase::database(d->connection));
            q.exec(QStringLiteral("SELECT COUNT(*) FROM ") + QLatin1String(tables[i][1]));
            return q.next() ? q.value(0).toLongLong() : 0;
        }
    }
    return 0;
}

// ---- compaction and purge: the deleting transaction ------------------------------------------

namespace {

const char *flagOf(bool purge) { return purge ? "purging" : "compacting"; }

// Opens the one write transaction of a compaction or a purge, sets its flag row (the triggers
// allow the deletes only while it is there) and prepares the delete statements.
bool beginDeleting(StorePrivate *d, bool purge)
{
    if (d->connection.isEmpty()) return d->fail(QStringLiteral("the store is not open"));
    if (d->readOnly) return d->fail(QStringLiteral("the store is open read-only"));
    if (d->compacting || d->snapshot) return d->fail(QStringLiteral("a compaction, a purge or a snapshot is open"));
    QSqlDatabase db = QSqlDatabase::database(d->connection);
    if (!db.transaction()) return d->fail(db.lastError().text());
    QString error;
    if (!dbutil::exec(d->connection, QStringLiteral("INSERT OR REPLACE INTO meta VALUES('%1', '1')")
                                         .arg(QLatin1String(flagOf(purge))), &error)) {
        db.rollback();
        return d->fail(error);
    }
    d->deleteRecord = new QSqlQuery(db);
    d->deleteRecord->prepare(QStringLiteral("DELETE FROM record WHERE subject=? AND metric=? AND measured_at=?"
                                            " AND source=? AND entry_id=?"));
    d->deleteQuarantine = new QSqlQuery(db);
    d->deleteQuarantine->prepare(QStringLiteral("DELETE FROM quarantine WHERE id=?"));
    d->deleteIngest = new QSqlQuery(db);
    d->deleteIngest->prepare(QStringLiteral("DELETE FROM ingest_log WHERE id=?"));
    if (purge) {
        d->deleteMark = new QSqlQuery(db);
        d->deleteMark->prepare(QStringLiteral("DELETE FROM mark WHERE id=? AND time < ?"));
        // A sensor row is widened in place by noteSensor, so the condition on last_seen is
        // checked again at the delete: a sensor seen again after the snapshot stays.
        d->deleteSensor = new QSqlQuery(db);
        d->deleteSensor->prepare(QStringLiteral("DELETE FROM sensor WHERE subject=? AND sensor_id=? AND last_seen < ?"));
    }
    d->compacting = true;
    d->purging = purge;
    return true;
}

void dropDeleteStatements(StorePrivate *d)
{
    delete d->deleteRecord;
    delete d->deleteQuarantine;
    delete d->deleteIngest;
    delete d->deleteMark;
    delete d->deleteSensor;
    d->deleteRecord = d->deleteQuarantine = d->deleteIngest = d->deleteMark = d->deleteSensor = 0;
    d->compacting = false;
    d->purging = false;
}

// Commits, then gives the space back: deleting leaves rows in most pages, so releasing the
// empty pages would free little; the file is rewritten (VACUUM, which needs free space of
// about the size of what is left, REQ-STO-16), then the write-ahead log is emptied. Best
// effort after the commit: the deletion itself is done.
bool commitDeleting(StorePrivate *d)
{
    QSqlDatabase db = QSqlDatabase::database(d->connection);
    const bool purge = d->purging;
    dropDeleteStatements(d);
    QString error;
    // The flag only ever exists inside the transaction.
    if (!dbutil::exec(d->connection, QStringLiteral("DELETE FROM meta WHERE key='%1'").arg(QLatin1String(flagOf(purge))), &error)
        || !db.commit()) {
        const QString err = error.isEmpty() ? db.lastError().text() : error;
        db.rollback();
        return d->fail(err);
    }
    QString ignored;
    dbutil::exec(d->connection, QStringLiteral("PRAGMA wal_checkpoint(TRUNCATE)"), &ignored);
    dbutil::exec(d->connection, QStringLiteral("VACUUM"), &ignored);
    dbutil::exec(d->connection, QStringLiteral("PRAGMA wal_checkpoint(TRUNCATE)"), &ignored);
    d->error.clear();
    return true;
}

} // namespace

bool Store::beginCompaction() { return beginDeleting(d, false); }
bool Store::beginPurge() { return beginDeleting(d, true); }

bool Store::removeRecord(const RecordKey &key)
{
    if (!d->compacting) return d->fail(QStringLiteral("no compaction or purge is open"));
    d->error.clear();
    d->deleteRecord->addBindValue(uuidText(key.subject));
    d->deleteRecord->addBindValue(key.metric);
    d->deleteRecord->addBindValue(qlonglong(key.measuredAtMs));
    d->deleteRecord->addBindValue(key.source);
    d->deleteRecord->addBindValue(notNull(key.entryId));
    if (!d->deleteRecord->exec()) return d->fail(d->deleteRecord->lastError().text());
    return d->deleteRecord->numRowsAffected() == 1;
}

bool Store::removeQuarantineEntry(const QUuid &id)
{
    if (!d->compacting) return d->fail(QStringLiteral("no compaction or purge is open"));
    d->error.clear();
    d->deleteQuarantine->addBindValue(uuidText(id));
    if (!d->deleteQuarantine->exec()) return d->fail(d->deleteQuarantine->lastError().text());
    return d->deleteQuarantine->numRowsAffected() == 1;
}

bool Store::removeIngestEntry(const QUuid &id)
{
    if (!d->compacting) return d->fail(QStringLiteral("no compaction or purge is open"));
    d->error.clear();
    d->deleteIngest->addBindValue(uuidText(id));
    if (!d->deleteIngest->exec()) return d->fail(d->deleteIngest->lastError().text());
    return d->deleteIngest->numRowsAffected() == 1;
}

bool Store::removeMark(const QUuid &id, const UtcTime &before)
{
    if (!d->purging) return d->fail(QStringLiteral("no purge is open"));
    d->error.clear();
    d->deleteMark->addBindValue(uuidText(id));
    d->deleteMark->addBindValue(qlonglong(before.msecsSinceEpoch()));
    if (!d->deleteMark->exec()) return d->fail(d->deleteMark->lastError().text());
    return d->deleteMark->numRowsAffected() == 1;
}

int Store::removeSensor(const QUuid &subject, const QString &sensorId, const UtcTime &before)
{
    if (!d->purging) { d->fail(QStringLiteral("no purge is open")); return -1; }
    d->error.clear();
    d->deleteSensor->addBindValue(uuidText(subject));
    d->deleteSensor->addBindValue(sensorId);
    d->deleteSensor->addBindValue(qlonglong(before.msecsSinceEpoch()));
    if (!d->deleteSensor->exec()) { d->fail(d->deleteSensor->lastError().text()); return -1; }
    return d->deleteSensor->numRowsAffected();
}

bool Store::recordCompaction(const CompactionEntry &e)
{
    if (!d->compacting || d->purging) return d->fail(QStringLiteral("no compaction is open"));
    if (!e.time.isValid() || !e.rangeFrom.isValid() || !e.rangeTo.isValid() || e.ruleVersion.isEmpty())
        return d->fail(QStringLiteral("a compaction record needs time, range and rule version"));
    QSqlQuery q(QSqlDatabase::database(d->connection));
    q.prepare(QStringLiteral("INSERT INTO compaction(id, time, range_from, range_to, removed, rule_version)"
                             " VALUES(?,?,?,?,?,?)"));
    q.addBindValue(uuidText(e.id.isNull() ? QUuid::createUuid() : e.id));
    q.addBindValue(qlonglong(e.time.msecsSinceEpoch()));
    q.addBindValue(qlonglong(e.rangeFrom.msecsSinceEpoch()));
    q.addBindValue(qlonglong(e.rangeTo.msecsSinceEpoch()));
    q.addBindValue(compactJson(e.removed));
    q.addBindValue(e.ruleVersion);
    if (!q.exec()) return d->fail(q.lastError().text());
    return true;
}

bool Store::commitCompaction()
{
    if (!d->compacting || d->purging) return d->fail(QStringLiteral("no compaction is open"));
    return commitDeleting(d);
}

void Store::rollbackCompaction()
{
    if (!d->compacting || d->purging)
        return;
    dropDeleteStatements(d);
    QSqlDatabase::database(d->connection).rollback();
}

bool Store::commitPurge()
{
    if (!d->purging) return d->fail(QStringLiteral("no purge is open"));
    return commitDeleting(d);
}

void Store::rollbackPurge()
{
    if (!d->purging)
        return;
    dropDeleteStatements(d);
    QSqlDatabase::database(d->connection).rollback();
}

qint64 Store::countRecordsOfSubjectBefore(const QUuid &subject, const UtcTime &time)
{
    if (!isOpen() || !time.isValid()) return 0;
    QSqlQuery q(QSqlDatabase::database(d->connection));
    q.prepare(QStringLiteral("SELECT COUNT(*) FROM record WHERE subject=? AND measured_at < ?"));
    q.addBindValue(uuidText(subject));
    q.addBindValue(qlonglong(time.msecsSinceEpoch()));
    return q.exec() && q.next() ? q.value(0).toLongLong() : 0;
}

bool Store::forEachRecordOfSubjectBefore(const QUuid &subject, const UtcTime &time,
                                         const std::function<void(const Record &)> &fn)
{
    return iterate(d, QStringLiteral("SELECT ") + QLatin1String(kRecordColumns)
                   + QStringLiteral(" FROM record WHERE subject='") + uuidText(subject)
                   + QStringLiteral("' AND measured_at < ") + QString::number(time.msecsSinceEpoch())
                   + QStringLiteral(" ORDER BY subject, metric, measured_at, source, entry_id"),
                   [&](QSqlQuery &q) { fn(recordFromRow(q)); });
}

qint64 Store::countRecordsOlderThan(const UtcTime &time)
{
    if (!isOpen() || !time.isValid()) return 0;
    QSqlQuery q(QSqlDatabase::database(d->connection));
    q.prepare(QStringLiteral("SELECT COUNT(*) FROM record WHERE measured_at < ?"));
    q.addBindValue(qlonglong(time.msecsSinceEpoch()));
    return q.exec() && q.next() ? q.value(0).toLongLong() : 0;
}

bool Store::forEachRecordOlderThan(const UtcTime &time, const std::function<void(const Record &)> &fn)
{
    return iterate(d, QStringLiteral("SELECT ") + QLatin1String(kRecordColumns)
                   + QStringLiteral(" FROM record WHERE measured_at < ") + QString::number(time.msecsSinceEpoch())
                   + QStringLiteral(" ORDER BY subject, metric, measured_at, source, entry_id"),
                   [&](QSqlQuery &q) { fn(recordFromRow(q)); });
}

} // namespace lm
