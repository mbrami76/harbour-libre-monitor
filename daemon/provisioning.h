// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Michele Brami
#ifndef LM_PROVISIONING_H
#define LM_PROVISIONING_H

// Setting up and removing a connection: the credentials go into the secret store, the
// connection and its Subject go into the configuration file. This is THE code that does it: the provisioning mode of the
// binary (provisionmode.h) calls it today, and the user interface calls the same
// functions later from its "set up a connection" page, so there is one implementation and no
// second path to the secret (REQ-SEC-01, REQ-SEC-05).
//
// Pattern: ports and adapters. The secret store is a small port (CredentialWriter) with a
// real implementation on the phone (phoneports.h, Sailfish Secrets) and a fake for the tests
// (hostports.h, MemorySecretStore); the configuration file is written here, with plain file
// code that the tests run for real in a temporary directory.
//
// What this code never does: print, log or return a credential, a name or a source
// reference. It reports fixed keys such as "secret-store-failed" only (REQ-SEC-01): no credential,
// token, name or reference in any output or log.
//
// Assumptions: the configuration file has the minimal shape of daemon/serviceconfig.h
// ( written by the user interface, read by the service); keys this code does not know
// are kept as they are, because a later user interface may store more there (for example the
// name of the Subject, which is personal data and lives only in this file, REQ-SUB-01); the
// file holds a source reference, which identifies a person, so it is created readable by the
// user only.

#include <QStringList>

namespace lm {

// Where credentials go. The real one is Sailfish Secrets; one secret per connection, named
// `connection/<id>`, in the collection `libremonitor`. Calls block until done:
// both users (a terminal command and a page that is saving) wait for the answer anyway.
class CredentialWriter
{
public:
    virtual ~CredentialWriter() {}

    // Stores the account name and the password, with an empty session, REPLACING any secret
    // that is already stored for the connection (the phone refuses to store over an existing
    // secret with "UNIQUE constraint failed", found on 2026-10-15, so the old one is deleted
    // first). On failure `errorKey` is a fixed key, never an error text from the platform.
    virtual bool store(const QString &connectionId, const QString &username,
                       const QString &password, QString *errorKey) = 0;

    // Deletes the secret. A secret that is not there counts as removed.
    virtual bool remove(const QString &connectionId, QString *errorKey) = 0;
};

// A connection identifier becomes part of a secret name and of the configuration file, so it
// is limited to letters, digits, underscore and hyphen, 1 to 64 characters.
bool isValidConnectionId(const QString &id);

// ---- the configuration file --------------------------------------------------------------

// Adds the connection to the file, or updates it when the id exists; creates the file (mode
// 600) and its directory when they do not exist. The connection gets exactly one Subject with
// the given source reference. The Subject keeps its UUID when the connection already had a
// Subject with the same reference (so the readings already stored stay attached to it),
// otherwise it gets a new UUID. Other connections and unknown keys stay as they are. A file
// that exists but is not valid is never overwritten (`errorKey` "config-invalid").
bool upsertConnection(const QString &configPath, const QString &connectionId,
                      const QString &adapterId, const QString &subjectRef, QString *errorKey);

// Removes the connection from the file. `found` tells whether it was there (a missing file
// or connection is not an error). The Subject's readings in the store are not touched.
bool removeConnection(const QString &configPath, const QString &connectionId, bool *found,
                      QString *errorKey);

// ---- the two operations -------------------------------------------------------------------

struct ProvisionRequest
{
    QString connectionId;
    QString adapterId;          // a built-in adapter, "libre"
    QString subjectRef;         // the source's identifier of the person followed
    QString username;
    QString password;
};

struct ProvisionResult
{
    ProvisionResult() : ok(false), secretDone(false), connectionDone(false) {}

    bool ok;
    bool secretDone;            // the secret was stored (or removed)
    bool connectionDone;        // the file was written (or the connection was in it and went)
    QString errorKey;           // fixed key when !ok
};

class Provisioner
{
public:
    // Neither pointer is owned. `configPath` is the service's configuration file.
    Provisioner(CredentialWriter *writer, const QString &configPath)
        : m_writer(writer), m_configPath(configPath) {}

    // Secret first, then the file; when the file cannot be written the secret is taken back,
    // so a failed run leaves nothing half-set-up. Running it again for the same connection
    // replaces the earlier setup.
    ProvisionResult provision(const ProvisionRequest &request);

    // Both parts are tried even when the first fails, so that one broken part does not keep
    // the other one on the phone.
    ProvisionResult remove(const QString &connectionId);

private:
    CredentialWriter *m_writer;
    QString m_configPath;
};

} // namespace lm

#endif
