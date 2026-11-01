// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Michele Brami
#ifndef LM_JOURNAL_H
#define LM_JOURNAL_H

#include "time/utctime.h"

#include <QMap>

#include <functional>

#include <QJsonObject>
#include <QString>
#include <QUuid>
#include <QVector>

namespace lm {

// The service journal (REQ-SCH-09, REQ-SCH-10, REQ-SCH-14): a
// database file of its own, separate from the measurement store. The service appends
// heartbeat, start and stop entries; the weekly storage check appends its result and prunes.
// Append-only except pruning, which is recorded.

// Version 2: the database refuses every delete except pruning (heartbeats) and the purge
// of one Subject's alert, threshold and silent entries. Older journals are migrated by the
// writer (journalMigrations, REQ-STO-10).
const int kJournalSchemaVersion = 2;

namespace JournalKind {
inline QString heartbeat() { return QStringLiteral("heartbeat"); }
inline QString start() { return QStringLiteral("start"); }
inline QString stop() { return QStringLiteral("stop"); }
inline QString prune() { return QStringLiteral("prune"); }
// PROVISIONAL: the result of the weekly storage check
// (REQ-STO-01).
inline QString storageCheck() { return QStringLiteral("storage-check"); }
// The state of a connection or of an adapter version, written by the guard and the
// scheduler. Append-only and never pruned: the
// state of a key is the last entry by wall time and id, and "clear" ends it. See
// ConnectionStateKey for the fields, which travel in JournalEntry::extra, except the reason,
// which uses the "cause" column of the entry.
inline QString connectionState() { return QStringLiteral("connection-state"); }
// The alert engine's transitions and the caregiver's threshold changes (
//). Both append-only and never pruned. An `alert` entry is one
// transition of one slot of one Subject; the state of a slot is its last entry (by wall time, then
// `seq`), as for `connection-state`. A `threshold` entry is one change of the L or H boundary of
// one Subject; the value in force is the last entry for that boundary. Their fields travel in
// JournalEntry::extra (AlertKey, ThresholdKey), because the journal table is closed.
// Silent mode of a Subject: the start, the end and the
// expiry of the mode, append-only and never pruned; the state of a Subject is its last line (by
// wall time, then `seq`), folded at start. Keys in SilentKey.
inline QString silent() { return QStringLiteral("silent"); }
// The service's check of its package's version stamp (core/service/versionwatch.h):
// one entry when the stamp cannot be read (monitoring goes on) or when a second self-restart for an
// upgrade within 10 minutes was refused. Keys in VersionCheckKey (result, app_version, stamp); no
// name, no value. Not pruned.
inline QString versionCheck() { return QStringLiteral("version-check"); }
inline QString alert() { return QStringLiteral("alert"); }
inline QString threshold() { return QStringLiteral("threshold"); }
}

// Keys of the "extra" object of an alert entry. No glucose value, name or comment is ever written
// here; `measured_at` only points at the reading in the store.
namespace AlertKey {
inline QString subject() { return QStringLiteral("subject"); }          // opaque Subject UUID
inline QString slot() { return QStringLiteral("slot"); }                // low, high, missing
inline QString level() { return QStringLiteral("alert"); }              // LL, L, H, HH, MISSING
inline QString state() { return QStringLiteral("state"); }              // the state AFTER the transition
inline QString from() { return QStringLiteral("from"); }                // the state before it
inline QString event() { return QStringLiteral("event"); }              // pending, active, escalated, ...
inline QString episode() { return QStringLiteral("episode"); }          // UUID, one per alert occurrence
inline QString seq() { return QStringLiteral("seq"); }                  // orders entries of equal wall time
inline QString measuredAt() { return QStringLiteral("measured_at"); }   // UTC ISO, optional
inline QString since() { return QStringLiteral("since"); }              // UTC ISO, pending and active
inline QString until() { return QStringLiteral("until"); }              // UTC ISO, snoozed and acknowledged LL/HH
inline QString thresholds() { return QStringLiteral("thresholds"); }    // id of the threshold entry in force, optional
inline QString reason() { return QStringLiteral("reason"); }            // missing: the staleness cause; cleared: why
inline QString levelSince() { return QStringLiteral("level_since"); }   // UTC ISO: first reading beyond the limit of this level (texts)
inline QString episodeSince() { return QStringLiteral("episode_since"); } // UTC ISO: the same for the first level of the episode
inline QString acknowledged() { return QStringLiteral("acknowledged_in_episode"); } // boolean, true after an acknowledgement or a snooze
}

// Keys of the "extra" object of a silent entry: the Subject's UUID, no name.
namespace SilentKey {
inline QString subject() { return QStringLiteral("subject"); }
inline QString event() { return QStringLiteral("event"); }          // start, end or expired
inline QString until() { return QStringLiteral("until"); }          // UTC ISO; absent for a mode without limit
inline QString seq() { return QStringLiteral("seq"); }              // orders lines of equal wall time
}

// Keys of the "extra" object of a threshold entry: time (the entry's wall time), Subject, boundary,
// new and previous value. No comment.
namespace ThresholdKey {
inline QString subject() { return QStringLiteral("subject"); }
inline QString boundary() { return QStringLiteral("boundary"); }        // L or H
inline QString value() { return QStringLiteral("value"); }              // whole mg/dL, new
inline QString previous() { return QStringLiteral("previous"); }        // whole mg/dL, before
// The caregiver's optional note of the entries written by an older version. Old entries keep it and are
// read without it; a new entry that carries it is refused (Journal::append).
inline QString legacyComment() { return QStringLiteral("comment"); }
}

// Keys and values of the "extra" object of a connection-state entry. Why they sit in "extra":
// the journal table has no column for them and the schema is closed.
namespace ConnectionStateKey {
inline QString scope() { return QStringLiteral("scope"); }              // "connection" or "adapter-version"
inline QString connection() { return QStringLiteral("connection"); }    // opaque id, "" for the version scope
inline QString adapter() { return QStringLiteral("adapter"); }
inline QString adapterVersion() { return QStringLiteral("adapter_version"); }
inline QString state() { return QStringLiteral("state"); }              // disabled, attention, hold, clear
inline QString until() { return QStringLiteral("until"); }              // UTC ISO time, hold only
inline QString detail() { return QStringLiteral("detail"); }            // no credentials, tokens, names, values
}

// Causes of a start entry (REQ-SCH-10).
namespace StartCause {
inline QString firstStart() { return QStringLiteral("first-start"); }
inline QString afterStop() { return QStringLiteral("after-stop"); }
inline QString afterMissingStop() { return QStringLiteral("after-missing-stop"); }
inline QString afterRestart() { return QStringLiteral("after-restart"); }
}

struct JournalEntry
{
    JournalEntry() : bootTimeMs(-1), monotonicMs(-1), removed(-1) {}

    QUuid id;               // generated when null
    QString kind;
    UtcTime wallTime;
    qint64 bootTimeMs;      // the two counters of REQ-SCH-09; -1 = none (prune, storage-check)
    qint64 monotonicMs;
    QString cause;          // start only
    UtcTime rangeFrom;      // prune only: the range pruned
    UtcTime rangeTo;
    qint64 removed;         // prune only: heartbeats removed
    QJsonObject extra;      // storage-check: store_bytes, free_bytes, threshold_bytes, low
};

// A gap (REQ-SCH-10): from the last heartbeat or stop entry of a run to the next start.
struct JournalGap
{
    UtcTime from;
    UtcTime to;
    bool operator==(const JournalGap &o) const { return from == o.from && to == o.to; }
};

struct PruneResult
{
    PruneResult() : ok(false), removed(0) {}

    bool ok;
    qint64 removed;
};

class JournalPrivate;

class Journal
{
public:
    Journal();
    ~Journal();

    bool open(const QString &path);         // "" or ":memory:" for tests
    bool openReadOnly(const QString &path); // for readers in other threads, see Store::openReadOnly
    void close();
    bool isOpen() const;
    QString lastError() const;
    QString path() const;
    qint64 sizeBytes() const;

    // Checks the entry against its kind (heartbeat, start and stop carry both counters, a
    // start a known cause, a prune a range and a count) and appends it. A repeated id is
    // ignored. Unknown kinds are refused.
    bool append(const JournalEntry &entry);

    // All entries by wall time and id, or only those of one kind.
    QVector<JournalEntry> entries(const QString &kind = QString());
    qint64 count();

    // REQ-SCH-14: removes the heartbeats older than one calendar month before "now", except
    // the last heartbeat of each run (a run lasts from a start entry to the next start or
    // stop entry), then appends a prune entry (range and number removed). Start, stop,
    // prune and storage-check entries are never removed. Nothing removed: no prune entry.
    // One transaction. What goes is only the suspend detail of REQ-SCH-11 for periods older
    // than a month, which is diagnostic.
    PruneResult pruneJournal(const UtcTime &now);

    // The gaps of REQ-SCH-10, for display and for the pruning test: each from the last
    // heartbeat, stop or start entry of a run to the start entry that follows it.
    QVector<JournalGap> gaps();

    // Purge (REQ-STO-17): the only deletion of entries other than pruned heartbeats, and
    // the only exception to the append-only rule of the journal. It removes, in one
    // transaction, the alert, threshold and silent entries of one Subject with a wall time
    // before the cut-off, and nothing else: the database's delete guard checks the same three
    // conditions. No record of the purge is written. Then the file is rewritten (VACUUM).
    // Called by purgeSubject (purge.h) after the store's part. Repeating it removes nothing more.
    bool countSubjectEntriesBefore(const QUuid &subject, const UtcTime &cutoff, QMap<QString, qint64> *perKind);
    bool purgeSubjectEntries(const QUuid &subject, const UtcTime &cutoff, QMap<QString, qint64> *removed);

    typedef std::function<void(const JournalEntry &)> EntryFn;
    bool forEachEntry(const EntryFn &fn);   // by wall time and id, streaming

private:
    Journal(const Journal &);
    Journal &operator=(const Journal &);
    JournalPrivate *d;
};

class MigrationRegistry;
// The migration steps of the journal: 1 to 2.
const MigrationRegistry &journalMigrations();

// The instant one calendar month before "now" (same day of month, clamped to the length of
// the month), in UTC. The cut-off of REQ-SCH-14.
UtcTime oneCalendarMonthBefore(const UtcTime &now);

} // namespace lm

#endif
