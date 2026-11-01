// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Michele Brami
#ifndef LM_REAL_REALBACKEND_H
#define LM_REAL_REALBACKEND_H

// The real data source of the screens: the monitoring service over D-Bus (state, buttons, signals),
// the store and the journal read-only (readings, plot, gaps), and the configuration file (names).
// It implements the same Backend interface as the scripted one, so the pages and models do not
// change (REQ-ARC-02: the UI and the service meet only through the store
// plus a light signal).
//
// Pattern: an event-driven cache. Nothing here polls (REQ-ARC-03): the UI asks the service for its
// Status() at start, after each of the service's own signals (coalesced: a burst of signals makes
// one call), after the app came back to the front and after the service reappeared; and it reads
// the store for a Subject after that Subject's DataChanged (coalesced the same way). Every answer
// updates the cache and emits changed(); the models recompute from the cache and the clock.
//
// Names: only the configuration file has them (REQ-SUB-02); they come in through ConfigStore and
// go to the screens, never into a log. Nothing is written by this class.

#include "servicelink.h"
#include "statusparser.h"
#include "snapshotbuilder.h"
#include "storeworker.h"
#include "ui/backend.h"
#include "ui/configstore.h"

#include <QHash>
#include <QTimer>

namespace lm {
namespace real {

class RealBackend : public ui::Backend
{
    Q_OBJECT
public:
    // The paths of the configuration file and of the service's store and journal ( the
    // configuration lives in AppConfigLocation).
    RealBackend(ServiceLink *link, const QString &configPath, const QString &storePath,
                const QString &journalPath, QObject *parent = 0);

    // This window's own build version (LM_APP_VERSION, "0.1-3"), compared with the service's
    // `Status().service.app_version`. Empty (the default) compares nothing: the
    // tests that do not set it see no "older service".
    void setOwnVersion(const QString &version);
    // Begins: reads the configuration, asks the service to start and tells it the UI is here.
    void begin();
    // The application came back to the front: catch up with what happened meanwhile.
    Q_INVOKABLE void refreshAll();
    // The configuration file changed (the account page saved): read it again.
    Q_INVOKABLE void reloadConfiguration();

    QList<ui::SubjectSnapshot> subjects() const;
    ui::ServiceStatus serviceStatus() const;
    qint64 lastHeartbeatMs() const;

    void retry(const QString &subjectId);
    void reenable(const QString &subjectId);
    void reportHandled(const QString &subjectId);
    void startMonitoring();
    void stopMonitoring();
    void askAgain();
    void requestThresholdHistory(const QString &subjectId);

private slots:
    void onStartAnswered(bool ok, const QString &state);
    void onStatusAnswered(bool ok, const QByteArray &json);
    void onReachableChanged(bool reachable);
    void onCallFailed(const QString &method, const QString &errorName);
    void onSubjectLoaded(const QString &id, const lm::real::SubjectData &data);
    void onGapsLoaded(const lm::real::GapList &gaps);
    void onDataChanged(const QString &subject);
    void onStatusSignal();
    void flushStatus();
    void flushData();

private:
    QList<ConfigSubject> configSubjects() const;
    qint64 windowFromMs() const;
    void loadAll();

    ServiceLink *m_link;
    StoreWorker m_store;
    QString m_configPath;
    ui::ConfigStore m_config;

    ServiceReport m_report;
    bool m_haveReport, m_answered, m_reachable, m_callFailed;
    // An older service, the two ways name: `m_ownVersion` differs from the report's
    // app_version (checked at each read, serviceVersionRelation; a newer one is ServiceNewer), or a call of this window was answered "unknown
    // method" (`m_olderByCall`, kept until the service's name loses its holder: a new process may be new).
    QString m_ownVersion;
    bool m_olderByCall;
    QHash<QString, SubjectData> m_data;
    QVector<QPair<qint64, qint64> > m_gaps;

    QTimer m_statusTimer, m_dataTimer;          // coalesce bursts of the service's signals
    QStringList m_dirty;                        // Subjects whose store reading is stale
};

} // namespace real
} // namespace lm

#endif
