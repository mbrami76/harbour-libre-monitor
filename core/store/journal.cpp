// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Michele Brami
#include "store/journal.h"

#include "alerts/alerttypes.h"
#include "store/dbutil.h"
#include "store/migration.h"

#include <QDateTime>
#include <QSqlDatabase>
#include <QSqlError>
#include <QSqlQuery>

namespace lm {

namespace {

using dbutil::compactJson;
using dbutil::notNull;
using dbutil::nullable;
using dbutil::parseJson;
using dbutil::timeFrom;
using dbutil::timeOrNull;
using dbutil::uuidFrom;
using dbutil::uuidText;

// The delete guard of schema 2. A subject is matched in the entry's extra object as written by
// the alert journal (compact JSON, "subject":"<uuid>"); a missing row gives a pattern that compact
// JSON cannot contain (a raw newline), so nothing matches. Every term is NOT NULL on purpose: a
// NULL condition would let the delete through.
#define LM_JOURNAL_DELETE_TRIGGER \
    "CREATE TRIGGER journal_no_delete BEFORE DELETE ON journal" \
    " WHEN NOT ((OLD.kind = 'heartbeat'" \
    " AND IFNULL((SELECT value FROM meta WHERE key='pruning'), '0') = '1')" \
    " OR (OLD.kind IN ('alert', 'threshold', 'silent')" \
    " AND OLD.wall_time < IFNULL((SELECT CAST(value AS INTEGER) FROM meta WHERE key='purging_before'), -9223372036854775807)" \
    " AND instr(OLD.extra, IFNULL('\"subject\":\"' || (SELECT value FROM meta WHERE key='purging_subject') || '\"', char(10))) > 0))" \
    " BEGIN SELECT RAISE(ABORT, 'journal entries are deleted only by pruning or by a purge'); END"

const char *kSchema[] = {
    "CREATE TABLE meta(key TEXT PRIMARY KEY, value TEXT NOT NULL) WITHOUT ROWID",
    "CREATE TABLE journal("
    " id TEXT PRIMARY KEY, kind TEXT NOT NULL, wall_time INTEGER NOT NULL,"
    " boot_time_ms INTEGER, monotonic_ms INTEGER, cause TEXT,"
    " range_from INTEGER, range_to INTEGER, removed INTEGER, extra TEXT NOT NULL) WITHOUT ROWID",
    "CREATE INDEX journal_time ON journal(wall_time, id)",
    "CREATE INDEX journal_kind_time ON journal(kind, wall_time)",
    // Entries are never changed; only pruning deletes (heartbeats), and it is recorded.
    "CREATE TRIGGER journal_no_update BEFORE UPDATE ON journal"
    " BEGIN SELECT RAISE(ABORT, 'the journal is append-only'); END",
    // Schema 2: the database refuses every delete except two. Pruning (REQ-SCH-14) may
    // delete heartbeats while its flag row 'pruning' is set. The purge of one Subject's old data
    // (REQ-STO-17) may delete that Subject's alert, threshold and silent entries older than its
    // cut-off while the rows 'purging_subject' and 'purging_before' name them. The text is
    // used again by the migration step 1 to 2 below.
    LM_JOURNAL_DELETE_TRIGGER
};

const char *kColumns =
    "id, kind, wall_time, boot_time_ms, monotonic_ms, cause, range_from, range_to, removed, extra";

JournalEntry entryFromRow(QSqlQuery &q)
{
    JournalEntry e;
    e.id = uuidFrom(q.value(0).toString());
    e.kind = q.value(1).toString();
    e.wallTime = timeFrom(q.value(2));
    e.bootTimeMs = q.value(3).isNull() ? -1 : q.value(3).toLongLong();
    e.monotonicMs = q.value(4).isNull() ? -1 : q.value(4).toLongLong();
    e.cause = q.value(5).isNull() ? QString() : q.value(5).toString();
    e.rangeFrom = timeFrom(q.value(6));
    e.rangeTo = timeFrom(q.value(7));
    e.removed = q.value(8).isNull() ? -1 : q.value(8).toLongLong();
    e.extra = parseJson(q.value(9).toString());
    return e;
}

bool knownCause(const QString &c)
{
    return c == StartCause::firstStart() || c == StartCause::afterStop()
        || c == StartCause::afterMissingStop() || c == StartCause::afterRestart();
}

// A connection-state entry must name its scope and its state, carry the connection id
// (empty only for the version scope) and, for a hold, a valid end time. Anything else would
// make the fold "last entry wins" ambiguous, so it is refused here, not at read time.
bool validConnectionState(const QJsonObject &x, QString *why)
{
    const QString scope = x.value(ConnectionStateKey::scope()).toString();
    const QString state = x.value(ConnectionStateKey::state()).toString();
    if (scope != QLatin1String("connection") && scope != QLatin1String("adapter-version")) {
        *why = QStringLiteral("a connection-state entry needs the scope connection or adapter-version");
        return false;
    }
    if (state != QLatin1String("disabled") && state != QLatin1String("attention")
        && state != QLatin1String("hold") && state != QLatin1String("clear")) {
        *why = QStringLiteral("a connection-state entry needs a known state");
        return false;
    }
    if (!x.value(ConnectionStateKey::connection()).isString()) {
        *why = QStringLiteral("a connection-state entry needs the connection key (empty for the version scope)");
        return false;
    }
    if (scope == QLatin1String("connection") && x.value(ConnectionStateKey::connection()).toString().isEmpty()) {
        *why = QStringLiteral("a connection-scope entry needs a connection id");
        return false;
    }
    if (scope == QLatin1String("adapter-version")
        && (x.value(ConnectionStateKey::adapter()).toString().isEmpty()
            || x.value(ConnectionStateKey::adapterVersion()).toString().isEmpty())) {
        *why = QStringLiteral("a version-scope entry needs the adapter and its version");
        return false;
    }
    if (state == QLatin1String("hold")
        && !UtcTime::fromIso(x.value(ConnectionStateKey::until()).toString()).isValid()) {
        *why = QStringLiteral("a hold needs a valid end time");
        return false;
    }
    return true;
}

bool validUuidKey(const QJsonObject &x, const QString &key)
{
    const QJsonValue v = x.value(key);
    return v.isString() && !QUuid(v.toString()).isNull();
}

bool validWholeNumberKey(const QJsonObject &x, const QString &key, double minimum)
{
    const QJsonValue v = x.value(key);
    return v.isDouble() && v.toDouble() >= minimum && v.toDouble() == double(qint64(v.toDouble()));
}

// An alert entry is folded at start (the last entry per Subject and slot wins), so a line that
// cannot be read back would silently drop a state. It is refused when written, not when read.
bool validAlert(const QJsonObject &x, QString *why)
{
    AlertSlot slot;
    AlertType type;
    AlertState state, from;
    if (!validUuidKey(x, AlertKey::subject()) || !validUuidKey(x, AlertKey::episode())) {
        *why = QStringLiteral("an alert entry needs the Subject and the episode as UUIDs");
        return false;
    }
    if (!alertSlotFromName(x.value(AlertKey::slot()).toString(), &slot)
        || !alertTypeFromName(x.value(AlertKey::level()).toString(), &type)
        || !alertStateFromName(x.value(AlertKey::state()).toString(), &state)
        || !alertStateFromName(x.value(AlertKey::from()).toString(), &from)) {
        *why = QStringLiteral("an alert entry needs a known slot, alert, state and previous state");
        return false;
    }
    if (slotOf(type) != slot) {
        *why = QStringLiteral("an alert entry's alert does not belong to its slot");
        return false;
    }
    if (x.value(AlertKey::event()).toString().isEmpty() || !validWholeNumberKey(x, AlertKey::seq(), 0)) {
        *why = QStringLiteral("an alert entry needs an event name and a sequence number");
        return false;
    }
    const QString times[] = { AlertKey::measuredAt(), AlertKey::since(), AlertKey::until(),
                              AlertKey::levelSince(), AlertKey::episodeSince() };
    for (size_t i = 0; i < sizeof(times) / sizeof(times[0]); ++i) {
        const QJsonValue v = x.value(times[i]);
        if (!v.isUndefined() && !(v.isString() && UtcTime::fromIso(v.toString()).isValid())) {
            *why = QStringLiteral("an alert entry's time must be a valid UTC time");
            return false;
        }
    }
    if (x.contains(AlertKey::acknowledged()) && !x.value(AlertKey::acknowledged()).isBool()) {
        *why = QStringLiteral("an alert entry's acknowledged flag must be a boolean");
        return false;
    }
    if (x.contains(AlertKey::thresholds()) && !validUuidKey(x, AlertKey::thresholds())) {
        *why = QStringLiteral("an alert entry's thresholds id must be a UUID");
        return false;
    }
    return true;
}

bool validSilent(const QJsonObject &x, QString *why)
{
    const QString event = x.value(SilentKey::event()).toString();
    if (!validUuidKey(x, SilentKey::subject())
        || (event != QLatin1String("start") && event != QLatin1String("end") && event != QLatin1String("expired"))
        || !validWholeNumberKey(x, SilentKey::seq(), 0)) {
        *why = QStringLiteral("a silent entry needs the Subject as a UUID, the event start, end or expired and a sequence number");
        return false;
    }
    const QJsonValue until = x.value(SilentKey::until());
    if (!until.isUndefined() && !(until.isString() && UtcTime::fromIso(until.toString()).isValid())) {
        *why = QStringLiteral("a silent entry's end time must be a valid UTC time");
        return false;
    }
    return true;
}

bool validThreshold(const QJsonObject &x, QString *why)
{
    const QString boundary = x.value(ThresholdKey::boundary()).toString();
    if (!validUuidKey(x, ThresholdKey::subject())
        || (boundary != QLatin1String("L") && boundary != QLatin1String("H"))
        || !validWholeNumberKey(x, ThresholdKey::value(), 1)
        || !validWholeNumberKey(x, ThresholdKey::previous(), 1)) {
        *why = QStringLiteral("a threshold entry needs the Subject, boundary L or H and whole new and previous values");
        return false;
    }
    // The caregiver's note on a threshold change was removed: nothing writes one any more,
    // and a new entry that still brings one is refused, so that no free text enters the journal by
    // a forgotten path. Entries written by an older version keep theirs; this check runs only on insert,
    // so they stay readable.
    if (x.contains(ThresholdKey::legacyComment())) {
        *why = QStringLiteral("a threshold entry carries no comment");
        return false;
    }
    return true;
}

} // namespace

class JournalPrivate
{
public:
    JournalPrivate() : readOnly(false) {}

    bool fail(const QString &m) { error = m; return false; }
    bool insert(const JournalEntry &e);

    QString connection;
    QString path;
    QString error;
    bool readOnly;
};

bool JournalPrivate::insert(const JournalEntry &e)
{
    QSqlQuery q(QSqlDatabase::database(connection));
    q.prepare(QStringLiteral("INSERT OR IGNORE INTO journal(") + QLatin1String(kColumns)
              + QStringLiteral(") VALUES(?,?,?,?,?,?,?,?,?,?)"));
    q.addBindValue(uuidText(e.id.isNull() ? QUuid::createUuid() : e.id));
    q.addBindValue(e.kind);
    q.addBindValue(qlonglong(e.wallTime.msecsSinceEpoch()));
    q.addBindValue(e.bootTimeMs >= 0 ? QVariant(qlonglong(e.bootTimeMs)) : QVariant(QVariant::LongLong));
    q.addBindValue(e.monotonicMs >= 0 ? QVariant(qlonglong(e.monotonicMs)) : QVariant(QVariant::LongLong));
    q.addBindValue(nullable(e.cause));
    q.addBindValue(timeOrNull(e.rangeFrom));
    q.addBindValue(timeOrNull(e.rangeTo));
    q.addBindValue(e.removed >= 0 ? QVariant(qlonglong(e.removed)) : QVariant(QVariant::LongLong));
    q.addBindValue(compactJson(e.extra));
    return q.exec() || fail(q.lastError().text());
}

UtcTime oneCalendarMonthBefore(const UtcTime &now)
{
    if (!now.isValid())
        return UtcTime();
    const QDateTime t = QDateTime::fromMSecsSinceEpoch(now.msecsSinceEpoch(), Qt::UTC);
    // QDate::addMonths clamps to the length of the target month.
    return UtcTime::fromMSecsSinceEpoch(QDateTime(t.date().addMonths(-1), t.time(), Qt::UTC).toMSecsSinceEpoch());
}

Journal::Journal() : d(new JournalPrivate) {}

Journal::~Journal()
{
    close();
    delete d;
}

bool Journal::isOpen() const { return !d->connection.isEmpty(); }
QString Journal::lastError() const { return d->error; }
QString Journal::path() const { return d->path; }

qint64 Journal::sizeBytes() const
{
    return d->connection.isEmpty() ? 0 : dbutil::sizeBytes(d->connection, d->path);
}

// Step 1 to 2 of the journal: the delete guard. No row changes. Frozen text.
static bool journal1to2(const QString &connection, QString *error)
{
    QSqlQuery q(QSqlDatabase::database(connection));
    if (!q.exec(QStringLiteral(LM_JOURNAL_DELETE_TRIGGER))) {
        *error = q.lastError().text();
        return false;
    }
    return true;
}

const MigrationRegistry &journalMigrations()
{
    static const MigrationRegistry registry = []() {
        MigrationRegistry r;
        MigrationStep s;
        s.fromVersion = 1;
        s.description = QStringLiteral("journal 1 to 2: deletes refused except pruning and the purge");
        s.apply = journal1to2;
        r.add(s);
        return r;
    }();
    return registry;
}

static bool openJournal(JournalPrivate *d, const QString &path, bool readOnly)
{
    int version = 0;
    QString error;
    const QString name = dbutil::openDatabase(path, readOnly, kSchema,
                                              int(sizeof(kSchema) / sizeof(kSchema[0])),
                                              kJournalSchemaVersion, &error, &version);
    if (name.isEmpty()) {
        d->error = error;
        return false;
    }
    // A journal of an older version is brought up to date by the writer, step by step
    // (REQ-STO-10, as the store); a reader leaves it as it is.
    if (!readOnly && version < kJournalSchemaVersion) {
        const MigrationResult m = migrateStore(name, version, kJournalSchemaVersion, journalMigrations());
        if (!m.ok) {
            dbutil::closeDatabase(name);
            d->error = m.error;
            return false;
        }
    }
    d->connection = name;
    d->path = path;
    d->readOnly = readOnly;
    d->error.clear();
    return true;
}

bool Journal::open(const QString &path)
{
    close();
    return openJournal(d, path, false);
}

bool Journal::openReadOnly(const QString &path)
{
    close();
    return openJournal(d, path, true);
}

void Journal::close()
{
    if (d->connection.isEmpty())
        return;
    const QString name = d->connection;
    d->connection.clear();
    d->path.clear();
    dbutil::closeDatabase(name);
}

bool Journal::append(const JournalEntry &e)
{
    if (!isOpen()) return d->fail(QStringLiteral("the journal is not open"));
    if (!e.wallTime.isValid()) return d->fail(QStringLiteral("a journal entry needs a wall time"));

    const bool counters = e.kind == JournalKind::heartbeat() || e.kind == JournalKind::start()
        || e.kind == JournalKind::stop();
    if (counters) {
        if (e.bootTimeMs < 0 || e.monotonicMs < 0)
            return d->fail(QStringLiteral("heartbeat, start and stop entries need both counters"));
    } else if (e.kind == JournalKind::connectionState()) {
        QString why;
        if (!validConnectionState(e.extra, &why)) return d->fail(why);
    } else if (e.kind == JournalKind::alert()) {
        QString why;
        if (!validAlert(e.extra, &why)) return d->fail(why);
    } else if (e.kind == JournalKind::silent()) {
        QString why;
        if (!validSilent(e.extra, &why)) return d->fail(why);
    } else if (e.kind == JournalKind::threshold()) {
        QString why;
        if (!validThreshold(e.extra, &why)) return d->fail(why);
    } else if (e.kind != JournalKind::prune() && e.kind != JournalKind::storageCheck()
               && e.kind != JournalKind::versionCheck()) {
        return d->fail(QStringLiteral("unknown journal entry kind"));
    }
    if (e.kind == JournalKind::start() && !knownCause(e.cause))
        return d->fail(QStringLiteral("a start entry needs a known cause"));
    // The "cause" column is shared: the cause of a start entry, and the reason of a
    // connection-state entry.
    if (e.kind != JournalKind::start() && e.kind != JournalKind::connectionState() && !e.cause.isNull())
        return d->fail(QStringLiteral("only a start or connection-state entry has a cause"));
    if (e.kind == JournalKind::prune()
        && (!e.rangeFrom.isValid() || !e.rangeTo.isValid() || e.removed < 0))
        return d->fail(QStringLiteral("a prune entry needs a range and a count"));
    if (e.kind != JournalKind::prune()
        && (e.rangeFrom.isValid() || e.rangeTo.isValid() || e.removed >= 0))
        return d->fail(QStringLiteral("only a prune entry has a range and a count"));
    static const char *reserved[] = { "type", "id", "kind", "wall_time", "boot_time_ms",
                                      "monotonic_ms", "cause", "range", "removed" };
    for (size_t i = 0; i < sizeof(reserved) / sizeof(reserved[0]); ++i)
        if (e.extra.contains(QLatin1String(reserved[i])))
            return d->fail(QStringLiteral("an extra key collides with a journal key"));

    if (!d->insert(e)) return false;
    d->error.clear();
    return true;
}

bool Journal::forEachEntry(const EntryFn &fn)
{
    if (!isOpen()) return d->fail(QStringLiteral("the journal is not open"));
    QSqlQuery q(QSqlDatabase::database(d->connection));
    q.setForwardOnly(true);
    if (!q.exec(QStringLiteral("SELECT ") + QLatin1String(kColumns)
                + QStringLiteral(" FROM journal ORDER BY wall_time, id")))
        return d->fail(q.lastError().text());
    while (q.next())
        fn(entryFromRow(q));
    return true;
}

QVector<JournalEntry> Journal::entries(const QString &kind)
{
    QVector<JournalEntry> out;
    forEachEntry([&](const JournalEntry &e) {
        if (kind.isEmpty() || e.kind == kind)
            out.append(e);
    });
    return out;
}

qint64 Journal::count()
{
    if (!isOpen()) return 0;
    QSqlQuery q(QSqlDatabase::database(d->connection));
    q.exec(QStringLiteral("SELECT COUNT(*) FROM journal"));
    return q.next() ? q.value(0).toLongLong() : 0;
}

PruneResult Journal::pruneJournal(const UtcTime &now)
{
    PruneResult result;
    if (!isOpen()) { d->fail(QStringLiteral("the journal is not open")); return result; }
    if (!now.isValid()) { d->fail(QStringLiteral("pruning needs the current time")); return result; }
    const qlonglong cutoff = oneCalendarMonthBefore(now).msecsSinceEpoch();

    // A heartbeat is removable when it is older than the cut-off and a later heartbeat
    // exists in the same run, that is, before the next start or stop entry. The last
    // heartbeat of a run therefore stays and every gap keeps its start (REQ-SCH-10).
    const QString where = QStringLiteral(
        " FROM journal WHERE kind='heartbeat' AND wall_time < ? AND EXISTS ("
        " SELECT 1 FROM journal h2 WHERE h2.kind='heartbeat'"
        " AND (h2.wall_time > journal.wall_time OR (h2.wall_time = journal.wall_time AND h2.id > journal.id))"
        " AND h2.wall_time < COALESCE((SELECT MIN(b.wall_time) FROM journal b"
        " WHERE b.kind IN ('start','stop') AND b.wall_time > journal.wall_time), 9223372036854775807))");

    QSqlDatabase db = QSqlDatabase::database(d->connection);
    if (!db.transaction()) { d->fail(db.lastError().text()); return result; }

    // The delete guard lets heartbeats go only while this flag row is set (schema 2); it exists
    // only inside this transaction.
    QString flagError;
    bool ok = dbutil::exec(d->connection, QStringLiteral("INSERT OR REPLACE INTO meta VALUES('pruning', '1')"), &flagError)
        || d->fail(flagError);
    JournalEntry entry;
    if (ok) {
        QSqlQuery q(db);
        q.prepare(QStringLiteral("SELECT MIN(wall_time), MAX(wall_time), COUNT(*)") + where);
        q.addBindValue(cutoff);
        ok = q.exec() && q.next();
        if (!ok) d->fail(q.lastError().text());
        else {
            result.removed = q.value(2).toLongLong();
            entry.rangeFrom = timeFrom(q.value(0));
            entry.rangeTo = timeFrom(q.value(1));
        }
    }
    if (ok && result.removed > 0) {
        QSqlQuery del(db);
        del.prepare(QStringLiteral("DELETE") + where);
        del.addBindValue(cutoff);
        ok = del.exec();
        if (!ok) d->fail(del.lastError().text());
        if (ok) {
            entry.kind = JournalKind::prune();
            entry.wallTime = now;
            entry.removed = result.removed;
            ok = d->insert(entry);
        }
    }
    if (ok && !dbutil::exec(d->connection, QStringLiteral("DELETE FROM meta WHERE key='pruning'"), &flagError))
        ok = d->fail(flagError);
    if (ok && !db.commit()) ok = d->fail(db.lastError().text());
    if (!ok) {
        db.rollback();
        return PruneResult();
    }
    result.ok = true;
    d->error.clear();
    return result;
}

QVector<JournalGap> Journal::gaps()
{
    QVector<JournalGap> out;
    UtcTime lastAlive;
    forEachEntry([&](const JournalEntry &e) {
        if (e.kind == JournalKind::start()) {
            if (lastAlive.isValid() && lastAlive < e.wallTime) {
                JournalGap g;
                g.from = lastAlive;
                g.to = e.wallTime;
                out.append(g);
            }
            lastAlive = e.wallTime;
        } else if (e.kind == JournalKind::heartbeat() || e.kind == JournalKind::stop()) {
            lastAlive = e.wallTime;
        }
    });
    return out;
}

// ---- purge of one Subject's entries (REQ-STO-17) -----------------------------------------

namespace {

// The one predicate of the count and the delete: the same match as the delete guard.
const char *kPurgeWhere =
    " FROM journal WHERE kind IN ('alert', 'threshold', 'silent') AND wall_time < ?"
    " AND instr(extra, ?) > 0";

QString subjectPattern(const QUuid &subject)
{
    return QStringLiteral("\"subject\":\"") + uuidText(subject) + QStringLiteral("\"");
}

} // namespace

bool Journal::countSubjectEntriesBefore(const QUuid &subject, const UtcTime &cutoff, QMap<QString, qint64> *perKind)
{
    perKind->clear();
    if (!isOpen()) return d->fail(QStringLiteral("the journal is not open"));
    if (subject.isNull() || !cutoff.isValid()) return d->fail(QStringLiteral("a Subject and a cut-off are needed"));
    QSqlQuery q(QSqlDatabase::database(d->connection));
    q.prepare(QStringLiteral("SELECT kind, COUNT(*)") + QLatin1String(kPurgeWhere) + QStringLiteral(" GROUP BY kind"));
    q.addBindValue(qlonglong(cutoff.msecsSinceEpoch()));
    q.addBindValue(subjectPattern(subject));
    if (!q.exec()) return d->fail(q.lastError().text());
    while (q.next())
        perKind->insert(q.value(0).toString(), q.value(1).toLongLong());
    return true;
}

bool Journal::purgeSubjectEntries(const QUuid &subject, const UtcTime &cutoff, QMap<QString, qint64> *removed)
{
    removed->clear();
    if (!isOpen()) return d->fail(QStringLiteral("the journal is not open"));
    if (d->readOnly) return d->fail(QStringLiteral("the journal is open read-only"));
    QMap<QString, qint64> counts;
    if (!countSubjectEntriesBefore(subject, cutoff, &counts)) return false;
    qint64 total = 0;
    foreach (qint64 n, counts) total += n;
    if (total == 0)
        return true;                    // nothing to do: nothing changes, not even the file

    QSqlDatabase db = QSqlDatabase::database(d->connection);
    if (!db.transaction()) return d->fail(db.lastError().text());
    QString error;
    bool ok = dbutil::exec(d->connection, QStringLiteral("INSERT OR REPLACE INTO meta VALUES('purging_subject', '%1')")
                                              .arg(uuidText(subject)), &error)
        && dbutil::exec(d->connection, QStringLiteral("INSERT OR REPLACE INTO meta VALUES('purging_before', '%1')")
                                           .arg(cutoff.msecsSinceEpoch()), &error);
    if (ok) {
        QSqlQuery del(db);
        del.prepare(QStringLiteral("DELETE") + QLatin1String(kPurgeWhere));
        del.addBindValue(qlonglong(cutoff.msecsSinceEpoch()));
        del.addBindValue(subjectPattern(subject));
        ok = del.exec();
        if (!ok)
            error = del.lastError().text();
        else if (del.numRowsAffected() != total) {
            ok = false;
            error = QStringLiteral("the journal changed while it was purged");
        }
    }
    // The rows that open the guard exist only inside this transaction; no record of the purge.
    ok = ok && dbutil::exec(d->connection, QStringLiteral("DELETE FROM meta WHERE key IN ('purging_subject', 'purging_before')"), &error);
    if (ok && !db.commit()) {
        ok = false;
        error = db.lastError().text();
    }
    if (!ok) {
        db.rollback();
        return d->fail(error);
    }
    *removed = counts;
    // Give the space back, as the store does after a deletion. Best effort: the purge is committed.
    QString ignored;
    dbutil::exec(d->connection, QStringLiteral("PRAGMA wal_checkpoint(TRUNCATE)"), &ignored);
    dbutil::exec(d->connection, QStringLiteral("VACUUM"), &ignored);
    dbutil::exec(d->connection, QStringLiteral("PRAGMA wal_checkpoint(TRUNCATE)"), &ignored);
    d->error.clear();
    return true;
}

} // namespace lm
