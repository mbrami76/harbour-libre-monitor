// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Michele Brami
#include "provisioning.h"

#include "daemon/discovery.h"

#include "session/adapterregistry.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QRegularExpression>
#include <QSaveFile>
#include <QUuid>

namespace lm {

namespace {

const qint64 kMaxConfigBytes = 1 << 20;     // the same bound the service's reader uses

// Reads the file into a JSON object. A missing file is an empty configuration; a file that
// exists but does not have the expected shape is an error and is never replaced, because
// replacing it could throw away a user's configuration.
bool readRoot(const QString &path, QJsonObject *root, QString *errorKey)
{
    QFile f(path);
    if (!f.exists()) {
        QJsonObject fresh;
        fresh.insert(QStringLiteral("format"), 1);
        fresh.insert(QStringLiteral("connections"), QJsonArray());
        *root = fresh;
        return true;
    }
    if (!f.open(QIODevice::ReadOnly) || f.size() > kMaxConfigBytes) {
        *errorKey = QStringLiteral("config-unreadable");
        return false;
    }
    QJsonParseError pe;
    const QJsonDocument doc = QJsonDocument::fromJson(f.readAll(), &pe);
    if (pe.error != QJsonParseError::NoError || !doc.isObject()
        || doc.object().value(QStringLiteral("format")).toInt(-1) != 1
        || !doc.object().value(QStringLiteral("connections")).isArray()) {
        *errorKey = QStringLiteral("config-invalid");
        return false;
    }
    *root = doc.object();
    return true;
}

// Writes through a temporary file and a rename, so a crash never leaves half a file, and
// with the user as the only reader from the first byte (the file holds a reference that
// identifies a person).
bool writeRoot(const QString &path, const QJsonObject &root, QString *errorKey)
{
    if (!QDir().mkpath(QFileInfo(path).absolutePath())) {
        *errorKey = QStringLiteral("config-unwritable");
        return false;
    }
    QSaveFile f(path);
    if (!f.open(QIODevice::WriteOnly)
        || !f.setPermissions(QFileDevice::ReadOwner | QFileDevice::WriteOwner)) {
        *errorKey = QStringLiteral("config-unwritable");
        return false;
    }
    f.write(QJsonDocument(root).toJson(QJsonDocument::Indented));
    if (!f.commit()) {
        *errorKey = QStringLiteral("config-unwritable");
        return false;
    }
    // A file that existed keeps its old permissions through the rename, so set them again.
    QFile::setPermissions(path, QFileDevice::ReadOwner | QFileDevice::WriteOwner);
    return true;
}

} // namespace

bool isValidConnectionId(const QString &id)
{
    static const QRegularExpression ok(QStringLiteral("^[A-Za-z0-9_-]{1,64}$"));
    // `probe` is the reserved identifier of the one-time login test; it is never a
    // connection, so it can never be provisioned or removed as one.
    return ok.match(id).hasMatch() && id != probeConnectionId();
}

bool upsertConnection(const QString &configPath, const QString &connectionId,
                      const QString &adapterId, const QString &subjectRef, QString *errorKey)
{
    if (!isValidConnectionId(connectionId) || subjectRef.isEmpty()
        || !AdapterRegistry::instance().find(adapterId)) {
        *errorKey = QStringLiteral("request-invalid");
        return false;
    }
    QJsonObject root;
    if (!readRoot(configPath, &root, errorKey))
        return false;

    QJsonArray connections = root.value(QStringLiteral("connections")).toArray();
    int index = -1;
    for (int i = 0; i < connections.size(); ++i)
        if (connections.at(i).toObject().value(QStringLiteral("id")).toString() == connectionId)
            index = i;
    QJsonObject connection = index >= 0 ? connections.at(index).toObject() : QJsonObject();

    // Keep the Subject's UUID when the same reference is set up again; the store knows the
    // Subject only by this UUID, so a new one would orphan the readings already stored.
    QString subjectId;
    const QJsonArray old = connection.value(QStringLiteral("subjects")).toArray();
    for (int i = 0; i < old.size(); ++i) {
        const QJsonObject s = old.at(i).toObject();
        if (s.value(QStringLiteral("ref")).toString() == subjectRef)
            subjectId = s.value(QStringLiteral("id")).toString();
    }
    if (QUuid(subjectId).isNull())
        subjectId = QUuid::createUuid().toString();

    QJsonObject subject;
    subject.insert(QStringLiteral("id"), subjectId);
    subject.insert(QStringLiteral("ref"), subjectRef);
    connection.insert(QStringLiteral("id"), connectionId);
    connection.insert(QStringLiteral("adapter"), adapterId);
    connection.insert(QStringLiteral("subjects"), QJsonArray() << subject);

    if (index >= 0)
        connections.replace(index, connection);
    else
        connections.append(connection);
    root.insert(QStringLiteral("connections"), connections);
    return writeRoot(configPath, root, errorKey);
}

bool removeConnection(const QString &configPath, const QString &connectionId, bool *found,
                      QString *errorKey)
{
    *found = false;
    if (!isValidConnectionId(connectionId)) {
        *errorKey = QStringLiteral("request-invalid");
        return false;
    }
    if (!QFile::exists(configPath))
        return true;
    QJsonObject root;
    if (!readRoot(configPath, &root, errorKey))
        return false;
    QJsonArray connections = root.value(QStringLiteral("connections")).toArray();
    for (int i = connections.size() - 1; i >= 0; --i)
        if (connections.at(i).toObject().value(QStringLiteral("id")).toString() == connectionId) {
            connections.removeAt(i);
            *found = true;
        }
    if (!*found)
        return true;
    root.insert(QStringLiteral("connections"), connections);
    return writeRoot(configPath, root, errorKey);
}

ProvisionResult Provisioner::provision(const ProvisionRequest &r)
{
    ProvisionResult out;
    if (!isValidConnectionId(r.connectionId) || r.subjectRef.isEmpty() || r.username.isEmpty()
        || r.password.isEmpty() || !AdapterRegistry::instance().find(r.adapterId)) {
        out.errorKey = QStringLiteral("request-invalid");
        return out;
    }
    if (!m_writer->store(r.connectionId, r.username, r.password, &out.errorKey)) {
        if (out.errorKey.isEmpty())
            out.errorKey = QStringLiteral("secret-store-failed");
        return out;
    }
    out.secretDone = true;
    if (!upsertConnection(m_configPath, r.connectionId, r.adapterId, r.subjectRef, &out.errorKey)) {
        // Take the secret back: a credential with no connection is a half set-up.
        QString ignored;
        if (m_writer->remove(r.connectionId, &ignored))
            out.secretDone = false;
        return out;
    }
    out.connectionDone = true;
    out.ok = true;
    return out;
}

ProvisionResult Provisioner::remove(const QString &connectionId)
{
    ProvisionResult out;
    if (!isValidConnectionId(connectionId)) {
        out.errorKey = QStringLiteral("request-invalid");
        return out;
    }
    QString secretError, fileError;
    const bool secretOk = m_writer->remove(connectionId, &secretError);
    bool found = false;
    const bool fileOk = removeConnection(m_configPath, connectionId, &found, &fileError);
    out.secretDone = secretOk;
    out.connectionDone = fileOk && found;
    out.ok = secretOk && fileOk;
    if (!secretOk)
        out.errorKey = secretError.isEmpty() ? QStringLiteral("secret-store-failed") : secretError;
    else if (!fileOk)
        out.errorKey = fileError;
    return out;
}

} // namespace lm
