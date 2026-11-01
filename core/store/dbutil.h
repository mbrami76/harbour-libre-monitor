// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Michele Brami
#ifndef LM_DBUTIL_H
#define LM_DBUTIL_H

// Internal helpers shared by the store and the journal (not part of the module API).

#include "time/utctime.h"

#include <QJsonObject>
#include <QString>
#include <QUuid>
#include <QVariant>

namespace lm {
namespace dbutil {

// Opens (or, for a writer, creates) a SQLite database through Qt SQL under a new connection
// name. A writer sets WAL and synchronous=FULL and creates the schema when the
// file is new; a read-only open never writes (no pragma, no schema) and fails on a file
// without the schema. A schema version above "version" is refused. Returns the connection
// name, empty on error (then *error is set).
QString openDatabase(const QString &path, bool readOnly, const char *const *schema,
                     int schemaCount, int version, QString *error, int *foundVersion);

void closeDatabase(const QString &connection);

bool exec(const QString &connection, const QString &sql, QString *error);

// Size of the database on disk including its WAL; for an in-memory database the page total.
qint64 sizeBytes(const QString &connection, const QString &path);

QString uuidText(const QUuid &id);        // lower case, no braces; empty for a null id
QUuid uuidFrom(const QString &text);
QString notNull(const QString &s);        // a null QString would bind as SQL NULL
QVariant nullable(const QString &s);
QVariant timeOrNull(const UtcTime &t);
UtcTime timeFrom(const QVariant &v);
QString compactJson(const QJsonObject &o);
QJsonObject parseJson(const QString &text);

} // namespace dbutil
} // namespace lm

#endif
