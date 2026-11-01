// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Michele Brami
#ifndef LM_PHONEPORTS_H
#define LM_PHONEPORTS_H

// The service ports that exist only on the phone (
//): the keepalive wake-up, the resident notice with its remote action, and
// Sailfish Secrets. All three libraries (libkeepalive, libnemonotifications-qt5,
// libsailfishsecrets) passed the Harbour validator in the spikes. Built only for the
// device kits; the PC build uses hostports.h instead.
//
// STATUS: compiled and package-checked, NOT yet run on a device. What the spikes showed is
// stated in the comments; what they did not show is marked UNVERIFIED and is the subject of
// the test protocol on a phone.

#include "daemon/provisioning.h"
#include "service/alertnotification.h"
#include "service/donotdisturb.h"
#include "service/residentnotice.h"
#include "scheduler/schedulerports.h"
#include "service/serviceports.h"

#include <QHash>
#include <QList>
#include <QObject>

#include <memory>

class BackgroundActivity;
class Notification;
class QQmlEngine;

namespace lm {

// BackgroundActivity wake-ups, the only clock that runs while the phone sleeps (S3: 28 to 32 s
// for the 30 s frequency, even asleep). The activity is armed with wait(frequency); at the
// wake-up it enters the Running state, the handler runs, and the lifecycle arms it again.
class KeepaliveWakeSource : public QObject, public WakeSource
{
    Q_OBJECT
public:
    explicit KeepaliveWakeSource(QObject *parent = 0);
    ~KeepaliveWakeSource();

    // The values of BackgroundActivity::Frequency that are used (30 s, 2.5 min, 5 min, ...).
    // UNVERIFIED: that every listed frequency behaves as documented on the device;
    // S3 measured 30 s and 150 s only.
    QList<int> supportedFrequenciesS() const;
    void setHandler(const std::function<void()> &handler) { m_handler = handler; }
    void arm(int frequencyS);
    void cancel();

private slots:
    void onRunning();

private:
    std::unique_ptr<BackgroundActivity> m_activity;
    std::function<void()> m_handler;
};

// The one resident notice (REQ-ALR-06, S4): the same notification is replaced in place, its id is kept
// in the service's settings, and it carries a remote action that calls the service's D-Bus name, so a
// tap starts a dead service without a window. Its words are the composer's (core/service/residentnotice.h):
// the state of the monitor in the summary, the facts in the body, translated like every notification;
// they hold no name, no value and no credential.
class NemoNotifier : public Notifier
{
public:
    // The settings store keeps the notice id between runs; neither pointer is owned.
    NemoNotifier(ServiceSettingsStore *settings, const QString &busName,
                 const QString &objectPath, const QString &interfaceName,
                 const ResidentNoticeComposer *composer, const NotificationCatalog &catalog = NotificationCatalog());
    ~NemoNotifier();

    void show(const Notice &notice);
    void withdraw();

private:
    ServiceSettingsStore *m_settings;
    QString m_busName, m_objectPath, m_interface;
    const ResidentNoticeComposer *m_composer;
    NotificationCatalog m_catalog;      // the icon of each state of the notice
};

// The alert notifications on the phone: the platform half of
// AlertNotificationPresenter (core/service/alertnotification.h), which decides WHAT to post and which
// notification replaces which. This class only turns one request into a libnemonotifications call and
// the platform's `closed` signals back into the presenter's handler. It adds no policy: a sound, a
// vibration, a priority or a word is never chosen here (the catalogue and the texts do).
//
// What it has to get right, from the measurements: it keeps one
// Notification object per id (the latest) and listens only to that one, because the library sends
// `closed` with the reason "closed by call" to every OLDER object of an id the service re-published
// and closed; a notification re-published after the user deleted it comes back under a new id and the
// object of the old id is dropped. Alerts have no `default` action and no buttons (the phone shows
// none and a default action brings a launcher card, S9), no `resident` flag and no
// `x-nemo-user-removable=false` (refused), and the colour of the icon is never tinted.
//
// STATUS: compiled and package-checked; run on a phone.
class NemoAlertBackend : public QObject, public AlertNotificationBackend
{
public:
    // The identity of the service's own D-Bus interface, which the tap-closes action names.
    NemoAlertBackend(const QString &busName, const QString &objectPath, const QString &interfaceName,
                     QObject *parent = 0);

    quint32 publish(const NotificationRequest &request, quint32 replacesId);
    void close(quint32 id);
    void setClosedHandler(const ClosedHandler &handler) { m_handler = handler; }

private:
    void forget(quint32 id);

    QString m_busName, m_objectPath, m_interface;
    ClosedHandler m_handler;
    QHash<quint32, Notification *> m_current;       // the latest object of each id
    QList<Notification *> m_retired;                // older objects, silenced, deleted in due course
};

// Do not disturb, read from the dconf value `/lipstick/do_not_disturb`: the QML type `ConfigurationValue` of `Nemo.Configuration`, created from a string
// in a headless QQmlEngine inside the service (no permission; the Settings page uses the same type). A
// change reaches the service within a second. This is the first QML in the service: the Harbour check
// of the package must be run again. The value is a boolean in dconf; anything
// that does not read as "on" counts as off, and an unset value is off.
class NemoDoNotDisturbSource : public QObject, public DoNotDisturbSource
{
    Q_OBJECT
public:
    explicit NemoDoNotDisturbSource(QObject *parent = 0);
    ~NemoDoNotDisturbSource();

    bool isOn() const;
    void setChangeHandler(const ChangeHandler &handler) { m_handler = handler; }

private slots:
    void onValueChanged();

private:
    std::unique_ptr<QQmlEngine> m_engine;
    QObject *m_value;
    ChangeHandler m_handler;
};

// Credentials and sessions in Sailfish Secrets (REQ-SEC-01, REQ-SEC-05). One secret per
// connection in the collection `libremonitor`, default encrypted storage plugin, as a JSON
// object {username, password, session, expires}. PROVISIONAL layout: the
// UI creates the collection and the secret; the service reads them and rewrites
// only the session part. `Locked` is a normal answer after a phone restart (S5, code 60).
// Reads here never allow interaction; the approval is requested by SecretsApproval.
class SailfishSecretStore : public SecretStore
{
public:
    void load(const QString &connectionId, const LoadCallback &done);
    void saveSession(const QString &connectionId, const QString &session, const UtcTime &expiresAt);
    void clearSession(const QString &connectionId);
};

// Writes the credentials of a connection into the same collection and secret that
// SailfishSecretStore reads. It is the one implementation of "store the credentials":
// the provisioning mode of the binary and, later, the user interface's connection page both
// call it through the CredentialWriter port (provisioning.h). Calls block until the platform
// has answered, and they allow the system to ask the user (SystemInteraction), because after
// a phone restart even a write meets the locked collection and its one-time approval dialog.
//
// The collection is created when it does not exist: protected by the device lock and kept
// unlocked after the first use (REQ-SEC-05), with NO per-application access control. That is
// the way the collection was created and read successfully on the phone (the service,
// 2026-10-15); a collection for the user only is the better target and is UNVERIFIED.
class SailfishCredentialWriter : public CredentialWriter
{
public:
    bool store(const QString &connectionId, const QString &username, const QString &password,
               QString *errorKey);
    bool remove(const QString &connectionId, QString *errorKey);
};

// Raises the system's approval notification by reading with system interaction allowed (S5:
// "read allowing system interaction", then silent reads worked). UNVERIFIED:
// what happens when the notification is ignored or expires; the callback may never come.
class SecretsApproval : public ApprovalSource
{
public:
    void request(const QString &connectionId, const Done &done);
};

} // namespace lm

#endif
