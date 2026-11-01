// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Michele Brami
#ifndef LM_UI_CONFIGSETTINGS_H
#define LM_UI_CONFIGSETTINGS_H

// The settings that live in the CONFIGURATION FILE, because the monitoring service reads them too
//:
//  - the language of the notification texts (REQ-ALR-10; "" = the phone's language), which the window
//    also takes at its next start, so the pages and the alerts speak the same language;
//  - "Show the value in alerts" (`alerts_show_value`, off by default);
//  - the label of the account (the connection's `label`, the name the guard and approval notices use);
//  - the short label of each patient (1 to 3 characters), the name a locked screen shows; without one the
//    patient is P1, P2 by its place in the file (lm::patientLabel, the function the service uses);
//  - the three alert times of each patient (the thresholds page writes them through here).
// The display preferences that the service never reads stay in the UI's own file (settingsstore.h).
//
// Pattern: a QML-facing object over ConfigStore. Every change is written at once (a settings page has no
// "OK"), atomically and with the other keys kept (ConfigStore's rules), and then `written()` is emitted:
// the application tells the service (`ConfigChanged`, which applies these keys without a restart) and
// reads the file again for the pages. The service's answer comes back through configApplied().
//
// Rules this class keeps: a label of more than three characters is refused, never shortened (the
// service would ignore it and use P1, and a shortened name could leak part of a name onto a lock
// screen); a file that cannot be read is never overwritten (ConfigStore refuses it); the full name is
// never written by this class (only the account page writes names).

#include "configstore.h"

#include <QObject>
#include <QVariantList>

namespace lm {
namespace ui {

class ConfigSettings : public QObject
{
    Q_OBJECT
    // False when the file exists but cannot be read (damaged or newer): nothing can be changed then.
    Q_PROPERTY(bool available READ available NOTIFY changed)
    Q_PROPERTY(QString language READ language NOTIFY changed)
    // The language the window took at its start (main.cpp): when it differs from `language` the page
    // says that the pages change at the next start (Qt 5.6 cannot translate a running window again).
    Q_PROPERTY(QString startLanguage READ startLanguage CONSTANT)
    Q_PROPERTY(bool alertsShowValue READ alertsShowValue NOTIFY changed)
    Q_PROPERTY(bool hasAccount READ hasAccount NOTIFY changed)
    Q_PROPERTY(QString connectionLabel READ connectionLabel NOTIFY changed)
    // One map per patient in the order of the file: subjectId, name, label, shortLabel (the label or P1,
    // P2 ...), defaultLabel (the P-name it has without a label), position.
    Q_PROPERTY(QVariantList patients READ patients NOTIFY changed)
    // The last thing that went wrong, as a text key for the page ("" = nothing): "config-unreadable",
    // "config-write-failed", "label-too-long", "account-label-too-long", "unknown-patient",
    // "service-config-invalid" (the service could not read the file after the change),
    // "service-no-answer".
    Q_PROPERTY(QString errorKey READ errorKey NOTIFY changed)

public:
    explicit ConfigSettings(QObject *parent = 0);

    // Reads the file; `startLanguage` is the language the window was started in.
    void load(const QString &path, const QString &startLanguage = QString());

    bool available() const { return m_store.isValid(); }
    QString language() const { return m_store.language(); }
    QString startLanguage() const { return m_startLanguage; }
    bool alertsShowValue() const { return m_store.alertsShowValue(); }
    bool hasAccount() const;
    QString connectionLabel() const;
    QVariantList patients() const;
    QString errorKey() const { return m_errorKey; }

    // "" (the phone's language), "en" or "it": the languages that have a translation file.
    Q_INVOKABLE bool setLanguage(const QString &language);
    Q_INVOKABLE bool setAlertsShowValue(bool on);
    // The first account's label; an empty text puts `defaultLabel` (the translated word "Account")
    // back, so a guard notice never falls back to the opaque connection id.
    Q_INVOKABLE bool setConnectionLabel(const QString &label, const QString &defaultLabel);
    // 0 to 3 characters after trimming; empty removes the label (the patient is then P1, P2 ...).
    Q_INVOKABLE bool setPatientLabel(const QString &subjectId, const QString &label);
    // `kind` is "low", "high" or "missing" (the three keys of the service); `seconds` a whole number of
    // seconds the thresholds page offered from the service's own range.
    Q_INVOKABLE bool setAlertTime(const QString &subjectId, const QString &kind, int seconds);
    Q_INVOKABLE void clearError();

    // The configuration key of an alert-time kind, or 0 for an unknown kind.
    static const char *alertTimeKey(const QString &kind);

public slots:
    // The service's answer to ConfigChanged ("" applied, "restart-needed", "config-invalid"); `ok` false
    // when it did not answer. The file is written either way; the page says what the service did.
    void configApplied(bool ok, const QString &result);

signals:
    void changed();
    // The file was written: tell the service and read it again (main.cpp wires it).
    void written();

private:
    bool commit();
    void fail(const QString &key);

    ConfigStore m_store;
    QString m_path;
    QString m_startLanguage;
    QString m_errorKey;
};

} // namespace ui
} // namespace lm

#endif
