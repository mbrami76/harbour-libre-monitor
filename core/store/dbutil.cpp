// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Michele Brami
#include "store/dbutil.h"

#include <QFileInfo>
#include <QJsonDocument>
#include <QSqlDatabase>
#include <QSqlError>
#include <QSqlQuery>

namespace lm {
namespace dbutil {

QString uuidText(const QUuid &id)
{
    return id.isNull() ? QString() : id.toString().mid(1, 36);
}

QUuid uuidFrom(const QString &text)
{
    return QUuid(QLatin1Char('{') + text + QLatin1Char('}'));
}

QString notNull(const QString &s)
{
    return s.isNull() ? QStringLiteral("") : s;
}

QVariant nullable(const QString &s)
{
    return s.isNull() ? QVariant(QVariant::String) : QVariant(s);
}

QVariant timeOrNull(const UtcTime &t)
{
    return t.isValid() ? QVariant(qlonglong(t.msecsSinceEpoch())) : QVariant(QVariant::LongLong);
}

UtcTime timeFrom(const QVariant &v)
{
    return v.isNull() ? UtcTime() : UtcTime::fromMSecsSinceEpoch(v.toLongLong());
}

QString compactJson(const QJsonObject &o)
{
    return QString::fromUtf8(QJsonDocument(o).toJson(QJsonDocument::Compact));
}

QJsonObject parseJson(const QString &text)
{
    return QJsonDocument::fromJson(text.toUtf8()).object();
}

bool exec(const QString &connection, const QString &sql, QString *error)
{
    QSqlQuery q(QSqlDatabase::database(connection));
    if (!q.exec(sql)) {
        *error = q.lastError().text();
        return false;
    }
    return true;
}

void closeDatabase(const QString &connection)
{
    if (connection.isEmpty())
        return;
    {
        QSqlDatabase db = QSqlDatabase::database(connection, false);
        if (db.isOpen())
            db.close();
    }
    QSqlDatabase::removeDatabase(connection);
}

QString openDatabase(const QString &path, bool readOnly, const char *const *schema,
                     int schemaCount, int version, QString *error, int *foundVersion)
{
    const QString name = QStringLiteral("lmdb-") + uuidText(QUuid::createUuid());
    {
        QSqlDatabase db = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), name);
        db.setDatabaseName(path.isEmpty() ? QStringLiteral(":memory:") : path);
        if (readOnly)
            db.setConnectOptions(QStringLiteral("QSQLITE_OPEN_READONLY"));
        if (!db.open()) {
            *error = db.lastError().text();
            db = QSqlDatabase();
            QSqlDatabase::removeDatabase(name);
            return QString();
        }
    }

    bool ok = true;
    if (!readOnly)
        ok = exec(name, QStringLiteral("PRAGMA journal_mode=WAL"), error)
            && exec(name, QStringLiteral("PRAGMA synchronous=FULL"), error);

    int existing = 0;
    if (ok) {
        QSqlQuery q(QSqlDatabase::database(name));
        q.exec(QStringLiteral("SELECT name FROM sqlite_master WHERE type='table' AND name='meta'"));
        if (q.next()) {
            QSqlQuery v(QSqlDatabase::database(name));
            v.exec(QStringLiteral("SELECT value FROM meta WHERE key='store_schema'"));
            existing = v.next() ? v.value(0).toInt() : -1;
        }
    }
    if (ok && existing > version) {
        *error = QStringLiteral("the store schema is newer than this application");
        ok = false;
    } else if (ok && existing < 0) {
        *error = QStringLiteral("the store has no schema version");
        ok = false;
    } else if (ok && existing == 0 && readOnly) {
        *error = QStringLiteral("the store does not exist");
        ok = false;
    } else if (ok && existing == 0) {
        QSqlDatabase db = QSqlDatabase::database(name);
        ok = db.transaction();
        if (!ok)
            *error = db.lastError().text();
        for (int i = 0; ok && i < schemaCount; ++i)
            ok = exec(name, QString::fromLatin1(schema[i]), error);
        if (ok) {
            QSqlQuery q(db);
            q.prepare(QStringLiteral("INSERT INTO meta VALUES('store_schema', ?)"));
            q.addBindValue(QString::number(version));
            ok = q.exec();
            if (!ok)
                *error = q.lastError().text();
        }
        if (ok) {
            ok = db.commit();
            if (!ok)
                *error = db.lastError().text();
        } else {
            db.rollback();
        }
        existing = version;
    }
    if (!ok) {
        closeDatabase(name);
        return QString();
    }
    *foundVersion = existing;
    return name;
}

qint64 sizeBytes(const QString &connection, const QString &path)
{
    if (!path.isEmpty() && path != QLatin1String(":memory:")) {
        qint64 total = QFileInfo(path).size();
        const QFileInfo wal(path + QStringLiteral("-wal"));
        if (wal.exists())
            total += wal.size();
        return total;
    }
    QSqlQuery q(QSqlDatabase::database(connection));
    q.exec(QStringLiteral("SELECT page_count * page_size FROM pragma_page_count(), pragma_page_size()"));
    return q.next() ? q.value(0).toLongLong() : 0;
}

} // namespace dbutil
} // namespace lm
