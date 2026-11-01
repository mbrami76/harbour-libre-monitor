// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Michele Brami
#ifndef LM_STORE_H
#define LM_STORE_H

#include "record/record.h"
#include "time/utctime.h"

#include <functional>

#include <QJsonObject>
#include <QList>
#include <QString>
#include <QUuid>
#include <QVector>

namespace lm {

// The measurement store (REQ-DAT-*): append-only persistence of
// records, marks, sensors, quarantine and the ingest log in one SQLite database (Qt SQL,
// WAL, synchronous=FULL). It holds only opaque Subject UUIDs, never a name
// (REQ-SUB-01), and knows nothing about adapters, the scheduler or the configuration file.
//
// Append-only (REQ-DAT-04): existing rows of records, marks, quarantine and the ingest log
// are never changed; the database refuses an UPDATE of them. Only compaction deletes, and it is
// recorded (compactor.h).
// Methods return false or an empty result and set lastError(); an error text never repeats
// record content.

// The version of the store's schema. A store of an older version is migrated step by step when
// it is opened for writing (migration.h, REQ-STO-10); a newer one is refused. Version 2:
// a compaction record no longer needs an archive. Version 3: a purge may delete marks
// and sensor records.
const int kStoreSchemaVersion = 3;

// Name of a Validation::Reason as stored in a quarantine entry, and back.
QString validationReasonName(Validation::Reason reason);
bool validationReasonFromName(const QString &name, Validation::Reason *reason);

// One item of an ingest batch. "raw" is the record exactly as received from the adapter; it
// is what quarantine keeps unmodified (REQ-DAT-08). If the caller could not even build a
// Record from it, it sets "rejected" to the reason and "record" is ignored.
struct IngestItem
{
    Record record;
    QJsonObject raw;
    Validation rejected;    // reason != Ok: quarantine without validating again
};

// Who delivered a batch, for quarantine and ingest-log entries (REQ-GRD-10, REQ-GRD-11).
struct IngestContext
{
    IngestContext() : clockSkewToleranceMs(kDefaultClockSkewToleranceMs) {}

    UtcTime now;                    // the clock for validation, receipt and log times
    qint64 clockSkewToleranceMs;
    QString adapter;
    QString adapterVersion;
    QString sessionId;
};

struct IngestResult
{
    IngestResult() : ok(false), added(0), duplicate(0), conflict(0), quarantined(0) {}

    bool ok;                // false: nothing was written (the batch is atomic)
    int added;
    int duplicate;          // same key, same value and unit: nothing written
    int conflict;           // same key, different value or unit: first kept, logged
    int quarantined;
};

// An entry of the ingest log (REQ-GRD-10, REQ-GRD-11). Keys beyond the known ones are kept in
// "extra" unchanged.
struct IngestEntry
{
    QUuid id;               // generated when null
    UtcTime time;
    QString event;
    QString adapter;
    QString adapterVersion;
    QString sessionId;
    QString detail;         // no credentials, tokens, names or glucose values
    QJsonObject extra;
};

struct QuarantineEntry
{
    QUuid id;
    UtcTime receivedAt;
    QString adapter;
    QString adapterVersion;
    QString reason;         // validationReasonName()
    QString detail;
    QJsonObject record;     // unmodified
};

// Overlay mark of REQ-DAT-07. For one target the mark with the latest time wins; equal
// times: the greater id in text order (the rule of the effective view, store.cpp).
struct Mark
{
    Mark() : ignored(true) {}

    QUuid id;               // generated when null
    UtcTime time;
    RecordKey target;
    bool ignored;           // false = state "active" (un-ignoring)
    QString comment;        // mandatory
    QString author;         // optional, null = none
};

// Sensor entity (REQ-SEN-01), stored only; first and last seen are kept up to date by
// noteSensor and are not authoritative.
struct Sensor
{
    QUuid subject;
    QString sensorId;       // hash of the serial number
    UtcTime activatedAt;    // invalid = unknown
    int warmupMinutes;      // -1 = unknown
    QString model;          // null = unknown
    UtcTime firstSeen;
    UtcTime lastSeen;

    Sensor() : warmupMinutes(-1) {}
};

// A query on the records. Times are inclusive; an invalid time leaves that end open.
struct RecordQuery
{
    RecordQuery() : effective(true), limit(0), newestFirst(false) {}

    QUuid subject;          // null = all Subjects
    QString metric;         // empty = all
    QString source;         // empty = all feeds (the feed is part of the source tag)
    UtcTime from;
    UtcTime to;
    bool effective;         // true: effective view, false: raw view (REQ-DAT-07)
    int limit;              // 0 = no limit
    bool newestFirst;
};

// The record of one compaction run (REQ-STO-06): when, the time range of what was removed,
// the counts removed per kind and the version of the thinning rule. Written by compaction in
// its own transaction, never changed, never removed. (Records written under schema 1 also
// name the archive the run relied on; that column is kept in the database for them, but this
// version neither writes nor reads it.)
struct CompactionEntry
{
    QUuid id;               // generated when null
    UtcTime time;
    UtcTime rangeFrom;
    UtcTime rangeTo;
    QJsonObject removed;    // counts removed per kind
    QString ruleVersion;
};

struct SubjectStats
{
    SubjectStats() : records(0) {}

    UtcTime first;          // earliest and latest measurement time
    UtcTime last;
    qint64 records;
};

class StorePrivate;

class Store
{
public:
    Store();
    ~Store();

    // Opens or creates the database at "path". Refuses a database whose schema is newer
    // than kStoreSchemaVersion; migrates an older one step by step (migration.h), and fails
    // if a step fails (the store then stays at the last complete version). The path "" or
    // ":memory:" gives a throw-away store for tests.
    bool open(const QString &path);
    // Opens an existing database without ever writing to it (no pragma, no schema, no
    // migration, no transaction that writes). The window and compaction's decisions read
    // through such connections of their own: a Qt SQL connection belongs to one thread.
    // An older schema is accepted: the reading calls use only what schema 1 and 2 share.
    bool openReadOnly(const QString &path);
    void close();
    QString path() const;
    qint64 sizeBytes() const;       // on disk, including the write-ahead log
    bool isOpen() const;
    QString lastError() const;
    int schemaVersion() const;

    // Validates every item, appends the valid ones, quarantines the others, in one
    // transaction. A conflict keeps the first record and writes an ingest-log entry.
    IngestResult ingest(const QVector<IngestItem> &items, const IngestContext &context);

    bool appendMark(const Mark &mark);              // a repeated id is ignored
    bool appendIngestEntry(const IngestEntry &entry);
    bool noteSensor(const Sensor &sensor);          // inserts, or widens first/last seen

    // Reading.
    QVector<Record> records(const RecordQuery &query);
    bool latest(const QUuid &subject, const QString &metric, const QString &source,
                bool effective, Record *out);
    QList<QUuid> subjects();
    SubjectStats subjectStats(const QUuid &subject);
    QVector<Mark> marks();                           // by time, id
    QVector<Sensor> sensors();
    QVector<QuarantineEntry> quarantine();           // by received_at, id
    QVector<IngestEntry> ingestLog();                // by time, id
    qint64 recordCount();

    // Compaction (REQ-STO-05 to 07). The database refuses every DELETE except inside this
    // transaction, and then only on records, quarantine and the ingest log: marks, sensors and
    // compaction records (and the reimport records of schema 1) can never be deleted by it. One
    // transaction, all or nothing, with the compaction record in it. Used by compactStore;
    // the procedure and its safeguards are in compactor.h.
    bool beginCompaction();
    bool removeRecord(const RecordKey &key);
    bool removeQuarantineEntry(const QUuid &id);
    bool removeIngestEntry(const QUuid &id);
    bool recordCompaction(const CompactionEntry &entry);
    bool commitCompaction();       // commits, rebuilds the file (VACUUM) and empties the log
    void rollbackCompaction();

    // Purge of one Subject's old data (REQ-STO-17). The same kind of transaction as a
    // compaction, with its own flag, in which marks and sensor records may also be deleted.
    // No record of the purge is written (the author's choice). Used by purgeSubject; the
    // procedure and its safeguards are in purge.h. removeRecord, removeQuarantineEntry and
    // removeIngestEntry work inside it too.
    bool beginPurge();
    bool removeMark(const QUuid &id, const UtcTime &before);        // only if its time is before "before"
    // Removes the sensor row if its last_seen is before "before"; 1 removed, 0 kept, -1 error.
    int removeSensor(const QUuid &subject, const QString &sensorId, const UtcTime &before);
    bool commitPurge();            // commits, rebuilds the file (VACUUM) and empties the log
    void rollbackPurge();
    qint64 countRecordsOfSubjectBefore(const QUuid &subject, const UtcTime &time);
    bool forEachRecordOfSubjectBefore(const QUuid &subject, const UtcTime &time,
                                      const std::function<void(const Record &)> &fn);   // key order
    qint64 countRecordsOlderThan(const UtcTime &time);      // measurement time strictly before
    bool forEachRecordOlderThan(const UtcTime &time, const std::function<void(const Record &)> &fn);   // key order

    // Snapshot: while it is open every read sees the store as it was when it began, whatever
    // the service ingests in the meantime (WAL read transaction). Not nested. A connection
    // with a snapshot open must not write.
    bool beginSnapshot();
    void endSnapshot();

    // Rows of a table, by name: subject, record, mark, sensor, quarantine, ingest, compaction.
    qint64 rowCount(const QString &type);

    // Streaming iteration: nothing is held in memory beyond the current row, so a year of data
    // can be walked on the phone. Records by key (Subject, kind, time, source, entry id), marks
    // and ingest entries by time and id, sensors by Subject and id, quarantine by received_at
    // and id, compaction records by time and id.
    typedef std::function<void(const Record &)> RecordFn;
    typedef std::function<void(const Mark &)> MarkFn;
    typedef std::function<void(const Sensor &)> SensorFn;
    typedef std::function<void(const QuarantineEntry &)> QuarantineFn;
    typedef std::function<void(const IngestEntry &)> IngestFn;
    typedef std::function<void(const CompactionEntry &)> CompactionFn;
    bool forEachRecord(const RecordFn &fn);
    bool forEachMark(const MarkFn &fn);
    bool forEachSensor(const SensorFn &fn);
    bool forEachQuarantine(const QuarantineFn &fn);
    bool forEachIngestEntry(const IngestFn &fn);
    bool forEachCompaction(const CompactionFn &fn);

private:
    Store(const Store &);
    Store &operator=(const Store &);
    StorePrivate *d;
};

} // namespace lm

#endif
