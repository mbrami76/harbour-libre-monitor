// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Michele Brami
#ifndef LM_MIGRATION_H
#define LM_MIGRATION_H

#include <QString>
#include <QStringList>
#include <QVector>

#include <functional>

namespace lm {

// Migration of the store's schema (REQ-STO-10). Every change to the data model that
// raises the store's schema version ships a step "N to N+1" here, and a test that opens a
// fixture store of each earlier version (tests/fixtures/store/). A store written by an older
// version of the application is brought up to date when the service opens it for writing
// (Store::open), one step after the other and never in one jump, so each step only has to
// know the version right before it.
//
// Pattern: a registry of small, pure steps over an open SQLite connection. Each step runs in
// a transaction of its own together with the new version number in `meta`, so a crash or a
// failure leaves the store at the last complete version, readable and ready to try again; it
// never leaves a half-changed schema. A step must keep every row it is given (records of what
// was done to the data are never deleted): it may reshape a table, not empty it.
//
// The steps of this application: 1 to 2 and 2 to 3, see migration.cpp.

// Changes the schema of the database behind the Qt SQL connection "connection" from version
// fromVersion to fromVersion + 1. Returns false with a message (never record content). The
// framework owns the transaction and the version number; the step must not commit.
typedef std::function<bool(const QString &connection, QString *error)> SchemaChange;

struct MigrationStep
{
    MigrationStep() : fromVersion(0) {}

    int fromVersion;            // the step produces fromVersion + 1
    QString description;        // for the documentation and the error message
    SchemaChange apply;
};

class MigrationRegistry
{
public:
    void add(const MigrationStep &step);        // replaces a step of the same fromVersion
    const MigrationStep *find(int fromVersion) const;
    int count() const { return m_steps.size(); }

    // The steps of this application.
    static const MigrationRegistry &builtIn();

private:
    QVector<MigrationStep> m_steps;
};

struct MigrationResult
{
    MigrationResult() : ok(false), version(0) {}

    bool ok;
    QString error;              // which step failed and why
    int version;                // the version the store is at now (the last complete step)
    QStringList applied;        // descriptions of the steps that ran
};

// Brings the database of "connection" from fromVersion to toVersion with the steps of
// "registry". Every step must exist before anything changes; a failing step is rolled back
// and the store stays at the version before it.
MigrationResult migrateStore(const QString &connection, int fromVersion, int toVersion,
                             const MigrationRegistry &registry);

} // namespace lm

#endif
