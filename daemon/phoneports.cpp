// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Michele Brami
#include "phoneports.h"

#include <keepalive/backgroundactivity.h>
#include <notification.h>

#include <Sailfish/Secrets/createcollectionrequest.h>
#include <Sailfish/Secrets/deletesecretrequest.h>
#include <Sailfish/Secrets/secretmanager.h>
#include <Sailfish/Secrets/storedsecretrequest.h>
#include <Sailfish/Secrets/storesecretrequest.h>

#include <QJsonDocument>
#include <QQmlComponent>
#include <QQmlEngine>
#include <QJsonObject>
#include <QVariantMap>

namespace lm {

using namespace Sailfish::Secrets;

// ---- keepalive -------------------------------------------------------------------------------

KeepaliveWakeSource::KeepaliveWakeSource(QObject *parent)
    : QObject(parent), m_activity(new BackgroundActivity)
{
    connect(m_activity.get(), SIGNAL(running()), this, SLOT(onRunning()));
}

KeepaliveWakeSource::~KeepaliveWakeSource() {}

QList<int> KeepaliveWakeSource::supportedFrequenciesS() const
{
    QList<int> f;
    f << BackgroundActivity::ThirtySeconds << BackgroundActivity::TwoAndHalfMinutes
      << BackgroundActivity::FiveMinutes << BackgroundActivity::TenMinutes
      << BackgroundActivity::FifteenMinutes << BackgroundActivity::ThirtyMinutes
      << BackgroundActivity::OneHour;
    return f;
}

void KeepaliveWakeSource::arm(int frequencyS)
{
    // Arming again replaces the previous wait. The frequency is one of supportedFrequenciesS().
    m_activity->wait(static_cast<BackgroundActivity::Frequency>(frequencyS));
}

void KeepaliveWakeSource::cancel()
{
    m_activity->stop();
}

void KeepaliveWakeSource::onRunning()
{
    // The activity is Running now; the handler works and arms the next wait itself. If it
    // does not (the service is stopping), the activity is stopped so it holds no wake lock.
    if (m_handler)
        m_handler();
    if (m_activity->isRunning())
        m_activity->stop();
}

// ---- the notice -------------------------------------------------------------------------------

NemoNotifier::NemoNotifier(ServiceSettingsStore *settings, const QString &busName,
                           const QString &objectPath, const QString &interfaceName,
                           const ResidentNoticeComposer *composer, const NotificationCatalog &catalog)
    : m_settings(settings), m_busName(busName), m_objectPath(objectPath), m_interface(interfaceName),
      m_composer(composer), m_catalog(catalog)
{
}

NemoNotifier::~NemoNotifier() {}

void NemoNotifier::show(const Notice &n)
{
    // The words are the composer's, in the language of the configuration file.
    const NoteText text = m_composer ? m_composer->compose(n) : NoteText();
    const QString summary = text.summary;
    const QString body = text.body;

    Notification notice;
    notice.setAppName(QStringLiteral("Libre Monitor"));
    notice.setSummary(summary);
    notice.setBody(body);
    notice.setPreviewSummary(QString());            // never a banner: the resident notice is quiet
    notice.setPreviewBody(QString());
    notice.setHintValue(QStringLiteral("x-nemo-hidden"), false);
    notice.setResident(true);
    notice.setIsTransient(false);
    const QString icon = m_catalog.residentIconPath(n.state);
    if (!icon.isEmpty())
        notice.setAppIcon(icon);

    // The remote action: a tap calls Start() on the service's name, which activates a dead
    // service without a window (S4: started within 1 s, same notice updated in place).
    QVariantMap action;
    action.insert(QStringLiteral("name"), QStringLiteral("default"));
    action.insert(QStringLiteral("service"), m_busName);
    action.insert(QStringLiteral("path"), m_objectPath);
    action.insert(QStringLiteral("iface"), m_interface);
    action.insert(QStringLiteral("method"), QStringLiteral("Start"));
    notice.setRemoteActions(QVariantList() << action);

    // The same notice every time: the id from the last run, if any (kept in the
    // service settings). A notice the user swiped away is simply shown
    // again under the same id; the notification server decides whether the id is still
    // known. UNVERIFIED: the behaviour after a dismissal.
    const quint32 id = m_settings->value(QStringLiteral("notice.id")).toUInt();
    if (id != 0)
        notice.setReplacesId(id);
    notice.publish();
    const quint32 now = notice.replacesId();
    if (now != 0 && now != id)
        m_settings->setValue(QStringLiteral("notice.id"), QString::number(now));
}

// after a removal the notice is closed by its id (a close raises no banner) and the id is
// forgotten, so a later install starts with a new notice.
void NemoNotifier::withdraw()
{
    const quint32 id = m_settings->value(QStringLiteral("notice.id")).toUInt();
    if (id == 0)
        return;
    Notification notice;
    notice.setReplacesId(id);
    notice.close();
    m_settings->setValue(QStringLiteral("notice.id"), QString());
}

// ---- the alert notifications ------------------------------------------------------------------

NemoAlertBackend::NemoAlertBackend(const QString &busName, const QString &objectPath,
                                   const QString &interfaceName, QObject *parent)
    : QObject(parent), m_busName(busName), m_objectPath(objectPath), m_interface(interfaceName)
{
}

quint32 NemoAlertBackend::publish(const NotificationRequest &r, quint32 replacesId)
{
    Notification *n = new Notification(this);
    n->setAppName(QStringLiteral("Libre Monitor"));
    n->setSummary(r.summary);
    n->setBody(r.body);
    n->setCategory(r.hints.category);
    // Without a preview the notification is only added to the list; with one it also shows a banner.
    // The banner shows the same summary and body as the page.
    n->setPreviewSummary(r.hints.banner ? r.summary : QString());
    n->setPreviewBody(r.hints.banner ? r.body : QString());
    n->setResident(false);
    n->setIsTransient(false);
    if (!r.hints.iconPath.isEmpty())
        n->setAppIcon(r.hints.iconPath);                     // never `x-nemo-color`: it destroys the colours
    if (r.hints.displayOn)
        n->setHintValue(QStringLiteral("x-nemo-display-on"), true);
    if (r.hints.removable)
        n->setHintValue(QStringLiteral("x-nemo-user-removable"), true);
    n->setHintValue(QStringLiteral("urgency"), r.hints.urgency);
    if (r.hints.priority != 0)
        n->setHintValue(QStringLiteral("x-nemo-priority"), r.hints.priority);
    if (!r.hints.feedback.isEmpty())
        n->setHintValue(QStringLiteral("x-nemo-feedback"), r.hints.feedback);
    if (replacesId != 0)
        n->setReplacesId(replacesId);
    n->publish();
    const quint32 id = n->replacesId();
    if (id == 0) {
        delete n;
        return 0;
    }
    // The object that listens is the latest one of the id; the previous one is silenced.
    if (m_current.contains(id)) {
        Notification *old = m_current.take(id);
        old->disconnect(this);
        m_retired.append(old);
        while (m_retired.size() > 8)
            delete m_retired.takeFirst();
    }
    if (replacesId != 0 && id != replacesId)
        forget(replacesId);                                   // the old id was deleted by the user
    m_current.insert(id, n);
    connect(n, &Notification::closed, this, [this, id](uint reason) {
        if (m_handler)
            m_handler(id, reason);
    });
    return id;
}

void NemoAlertBackend::close(quint32 id)
{
    if (id == 0)
        return;
    Notification *n = m_current.value(id);
    if (n) {
        n->disconnect(this);                                  // no echo of our own close
        n->close();
        forget(id);
        return;
    }
    Notification tmp;
    tmp.setReplacesId(id);
    tmp.close();
}

void NemoAlertBackend::forget(quint32 id)
{
    Notification *n = m_current.take(id);
    if (n) {
        n->disconnect(this);
        m_retired.append(n);
        while (m_retired.size() > 8)
            delete m_retired.takeFirst();
    }
}

// ---- Do not disturb -------------------------------------------------------------------------------

NemoDoNotDisturbSource::NemoDoNotDisturbSource(QObject *parent)
    : QObject(parent), m_engine(new QQmlEngine), m_value(0)
{
    QQmlComponent component(m_engine.get());
    component.setData("import QtQml 2.2\nimport Nemo.Configuration 1.0\n"
                      "ConfigurationValue { key: \"/lipstick/do_not_disturb\"; defaultValue: false }", QUrl());
    m_value = component.create();
    if (m_value)
        connect(m_value, SIGNAL(valueChanged()), this, SLOT(onValueChanged()));
}

NemoDoNotDisturbSource::~NemoDoNotDisturbSource()
{
    delete m_value;
}

bool NemoDoNotDisturbSource::isOn() const
{
    if (!m_value)
        return false;
    const QVariant v = m_value->property("value");
    if (v.type() == QVariant::Bool)
        return v.toBool();
    const QString s = v.toString().toLower();
    return s == QLatin1String("true") || s == QLatin1String("1");
}

void NemoDoNotDisturbSource::onValueChanged()
{
    if (m_handler)
        m_handler(isOn());
}

// ---- Sailfish Secrets -------------------------------------------------------------------------

namespace {

const char *const kCollection = "libremonitor";

QString secretName(const QString &connectionId)
{
    return QStringLiteral("connection/") + connectionId;
}

Secret::Identifier identifierOf(const QString &connectionId)
{
    return Secret::Identifier(secretName(connectionId), QString::fromLatin1(kCollection),
                              SecretManager::DefaultEncryptedStoragePluginName);
}

bool isLockedError(Result::ErrorCode c)
{
    return c == Result::CollectionIsLockedError || c == Result::SecretsPluginIsLockedError
        || c == Result::SecretsDaemonLockedError
        || c == Result::OperationRequiresSystemUserInteraction
        || c == Result::OperationRequiresUserInteraction
        || c == Result::OperationRequiresApplicationUserInteraction;
}

bool isMissingError(Result::ErrorCode c)
{
    return c == Result::InvalidSecretIdentifierError || c == Result::InvalidCollectionError
        || c == Result::InvalidSecretError;
}

// Runs one StoredSecretRequest; `done` gets the finished request's result and secret. The
// request deletes itself when finished. The SecretManager is shared by all requests.
SecretManager *manager()
{
    static SecretManager *m = new SecretManager;
    return m;
}

void readSecret(const QString &connectionId, SecretManager::UserInteractionMode mode,
                const std::function<void(const Result &, const QByteArray &)> &done)
{
    StoredSecretRequest *req = new StoredSecretRequest;
    req->setManager(manager());
    req->setIdentifier(identifierOf(connectionId));
    req->setUserInteractionMode(mode);
    QObject::connect(req, &Request::statusChanged, [req, done]() {
        if (req->status() != Request::Finished)
            return;
        const Result result = req->result();
        const QByteArray data = req->secret().data();
        req->deleteLater();
        done(result, data);
    });
    req->startRequest();
}

} // namespace

void SailfishSecretStore::load(const QString &connectionId, const LoadCallback &done)
{
    readSecret(connectionId, SecretManager::PreventInteraction,
               [done](const Result &result, const QByteArray &data) {
        SecretLoad r;
        if (result.code() == Result::Succeeded) {
            const QJsonObject o = QJsonDocument::fromJson(data).object();
            r.credentials.username = o.value(QStringLiteral("username")).toString();
            r.credentials.password = o.value(QStringLiteral("password")).toString();
            r.session = o.value(QStringLiteral("session")).toString();
            const QString exp = o.value(QStringLiteral("expires")).toString();
            if (!exp.isEmpty())
                r.expiresAt = UtcTime::fromIso(exp);
            r.kind = (r.credentials.username.isEmpty() || r.credentials.password.isEmpty())
                ? SecretLoad::Missing : SecretLoad::Ok;
        } else if (isLockedError(result.errorCode())) {
            r.kind = SecretLoad::Locked;
        } else if (isMissingError(result.errorCode())) {
            r.kind = SecretLoad::Missing;
        } else {
            r.kind = SecretLoad::Error;
        }
        done(r);
    });
}

void SailfishSecretStore::saveSession(const QString &connectionId, const QString &session,
                                      const UtcTime &expiresAt)
{
    // Read-modify-write: the credentials stay exactly as the UI stored them; only the session
    // and its expiry change. A read that fails leaves the stored secret untouched.
    readSecret(connectionId, SecretManager::PreventInteraction,
               [connectionId, session, expiresAt](const Result &result, const QByteArray &data) {
        if (result.code() != Result::Succeeded)
            return;
        QJsonObject o = QJsonDocument::fromJson(data).object();
        o.insert(QStringLiteral("session"), session);
        o.insert(QStringLiteral("expires"), expiresAt.isValid() ? expiresAt.toIso() : QString());
        Secret secret(identifierOf(connectionId));
        secret.setData(QJsonDocument(o).toJson(QJsonDocument::Compact));
        // Storing over an existing secret fails (phone test 2026-10-15: "UNIQUE constraint
        // failed"), so the old one is deleted first and the new one stored when that is done.
        // If the store then fails the credentials are gone from the collection: the data is
        // kept in this closure and the store is tried once more before giving up.
        DeleteSecretRequest *del = new DeleteSecretRequest;
        del->setManager(manager());
        del->setIdentifier(identifierOf(connectionId));
        del->setUserInteractionMode(SecretManager::PreventInteraction);
        QObject::connect(del, &Request::statusChanged, [del, secret]() {
            if (del->status() != Request::Finished)
                return;
            del->deleteLater();
            for (int attempt = 0; attempt < 2; ++attempt) {
                StoreSecretRequest *req = new StoreSecretRequest;
                req->setManager(manager());
                req->setSecretStorageType(StoreSecretRequest::CollectionSecret);
                req->setUserInteractionMode(SecretManager::PreventInteraction);
                req->setSecret(secret);
                req->startRequest();
                req->waitForFinished();
                const bool ok = req->result().code() == Result::Succeeded;
                req->deleteLater();
                if (ok)
                    break;
            }
        });
        del->startRequest();
    });
}

void SailfishSecretStore::clearSession(const QString &connectionId)
{
    saveSession(connectionId, QString(), UtcTime());
}

bool SailfishCredentialWriter::store(const QString &connectionId, const QString &username,
                                     const QString &password, QString *errorKey)
{
    // 1. The collection, created on first use. "Already exists" is the normal case afterwards.
    CreateCollectionRequest cc;
    cc.setManager(manager());
    cc.setCollectionName(QString::fromLatin1(kCollection));
    cc.setCollectionLockType(CreateCollectionRequest::DeviceLock);
    cc.setStoragePluginName(SecretManager::DefaultEncryptedStoragePluginName);
    cc.setEncryptionPluginName(SecretManager::DefaultEncryptedStoragePluginName);
    cc.setDeviceLockUnlockSemantic(SecretManager::DeviceLockKeepUnlocked);
    cc.setAccessControlMode(SecretManager::NoAccessControlMode);
    cc.setUserInteractionMode(SecretManager::SystemInteraction);
    cc.startRequest();
    cc.waitForFinished();
    if (cc.result().code() != Result::Succeeded
        && cc.result().errorCode() != Result::CollectionAlreadyExistsError) {
        *errorKey = QStringLiteral("secret-collection-failed");
        return false;
    }

    // 2. Delete what is there: the phone refuses to store over an existing secret ("UNIQUE
    // constraint failed", 2026-10-15). Nothing there is not a failure, so the result is not
    // looked at; the store below says whether the whole thing worked.
    DeleteSecretRequest del;
    del.setManager(manager());
    del.setIdentifier(identifierOf(connectionId));
    del.setUserInteractionMode(SecretManager::SystemInteraction);
    del.startRequest();
    del.waitForFinished();

    // 3. The secret, in the layout that SailfishSecretStore::load() reads, with no
    // session yet: the first successful log-in writes it.
    QJsonObject o;
    o.insert(QStringLiteral("username"), username);
    o.insert(QStringLiteral("password"), password);
    o.insert(QStringLiteral("session"), QString());
    o.insert(QStringLiteral("expires"), QString());
    Secret secret(identifierOf(connectionId));
    secret.setData(QJsonDocument(o).toJson(QJsonDocument::Compact));
    StoreSecretRequest req;
    req.setManager(manager());
    req.setSecretStorageType(StoreSecretRequest::CollectionSecret);
    req.setUserInteractionMode(SecretManager::SystemInteraction);
    req.setSecret(secret);
    req.startRequest();
    req.waitForFinished();
    if (req.result().code() != Result::Succeeded) {
        // A fixed key only: the platform's own error text is never passed on.
        *errorKey = QStringLiteral("secret-store-failed");
        return false;
    }
    return true;
}

bool SailfishCredentialWriter::remove(const QString &connectionId, QString *errorKey)
{
    DeleteSecretRequest del;
    del.setManager(manager());
    del.setIdentifier(identifierOf(connectionId));
    del.setUserInteractionMode(SecretManager::SystemInteraction);
    del.startRequest();
    del.waitForFinished();
    // A secret or a collection that is not there is already what the caller wants.
    if (del.result().code() == Result::Succeeded || isMissingError(del.result().errorCode()))
        return true;
    *errorKey = QStringLiteral("secret-store-failed");
    return false;
}

void SecretsApproval::request(const QString &connectionId, const Done &done)
{
    readSecret(connectionId, SecretManager::SystemInteraction,
               [done](const Result &result, const QByteArray &) {
        done(result.code() == Result::Succeeded);
    });
}

} // namespace lm
