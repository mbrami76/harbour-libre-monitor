// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Michele Brami
#include "configsettings.h"

#include "service/subjectlabel.h"

#include <QVariantMap>

namespace lm {
namespace ui {

ConfigSettings::ConfigSettings(QObject *parent) : QObject(parent) {}

void ConfigSettings::load(const QString &path, const QString &startLanguage)
{
    m_path = path;
    m_startLanguage = startLanguage;
    m_store.load(path);
    m_errorKey = m_store.isValid() ? QString() : QStringLiteral("config-unreadable");
    emit changed();
}

bool ConfigSettings::hasAccount() const { return !m_store.connections().isEmpty(); }

QString ConfigSettings::connectionLabel() const
{
    const QList<ConnectionEntry> c = m_store.connections();
    return c.isEmpty() ? QString() : c.first().label;
}

QVariantList ConfigSettings::patients() const
{
    // The place in the file counts connections in file order and their patients in file order: the same
    // count the service makes for P1, P2 (core/service/subjectlabel.h), so the page shows the same name
    // the notifications will.
    QVariantList out;
    const QList<ConnectionEntry> conns = m_store.connections();
    int position = 0;
    for (int i = 0; i < conns.size(); ++i)
        for (int j = 0; j < conns.at(i).subjects.size(); ++j, ++position) {
            const SubjectEntry &s = conns.at(i).subjects.at(j);
            QVariantMap m;
            m.insert(QStringLiteral("subjectId"), s.id);
            m.insert(QStringLiteral("name"), s.name);
            m.insert(QStringLiteral("label"), s.label);
            m.insert(QStringLiteral("shortLabel"), lm::patientLabel(s.label, position));
            m.insert(QStringLiteral("defaultLabel"), lm::patientLabel(QString(), position));
            m.insert(QStringLiteral("position"), position);
            out.append(m);
        }
    return out;
}

void ConfigSettings::fail(const QString &key)
{
    m_errorKey = key;
    emit changed();
}

void ConfigSettings::clearError()
{
    if (m_errorKey.isEmpty())
        return;
    m_errorKey.clear();
    emit changed();
}

bool ConfigSettings::commit()
{
    if (!m_store.isValid()) {
        fail(QStringLiteral("config-unreadable"));
        return false;
    }
    if (!m_store.save()) {
        // Read the file again so the page shows what is really in it, not the change that failed.
        m_store.load(m_path);
        fail(QStringLiteral("config-write-failed"));
        return false;
    }
    m_errorKey.clear();
    emit changed();
    emit written();
    return true;
}

bool ConfigSettings::setLanguage(const QString &language)
{
    // Only the languages this version has a translation file for; anything else is the phone's language.
    const QString l = language.trimmed();
    if (!l.isEmpty() && l != QLatin1String("en") && l != QLatin1String("it"))
        return false;
    if (!m_store.isValid()) { fail(QStringLiteral("config-unreadable")); return false; }
    if (m_store.language() == l)
        return true;
    m_store.setLanguage(l);
    return commit();
}

bool ConfigSettings::setAlertsShowValue(bool on)
{
    if (!m_store.isValid()) { fail(QStringLiteral("config-unreadable")); return false; }
    if (m_store.alertsShowValue() == on)
        return true;
    m_store.setAlertsShowValue(on);
    return commit();
}

bool ConfigSettings::setConnectionLabel(const QString &label, const QString &defaultLabel)
{
    if (!m_store.isValid()) { fail(QStringLiteral("config-unreadable")); return false; }
    const QList<ConnectionEntry> c = m_store.connections();
    if (c.isEmpty())
        return false;
    QString l = label.trimmed();
    if (l.isEmpty())
        l = defaultLabel.trimmed();
    if (l.size() > kConnectionLabelMaxChars) {
        fail(QStringLiteral("account-label-too-long"));
        return false;
    }
    if (c.first().label == l)
        return true;
    m_store.setConnectionLabel(c.first().id, l);
    return commit();
}

bool ConfigSettings::setPatientLabel(const QString &subjectId, const QString &label)
{
    if (!m_store.isValid()) { fail(QStringLiteral("config-unreadable")); return false; }
    const QString l = label.trimmed();
    // Refused, never shortened: a longer text may be a name typed in the wrong field.
    if (l.size() > kPatientLabelMaxChars) {
        fail(QStringLiteral("label-too-long"));
        return false;
    }
    if (!m_store.setSubjectLabel(subjectId, l)) {
        fail(QStringLiteral("unknown-patient"));
        return false;
    }
    return commit();
}

const char *ConfigSettings::alertTimeKey(const QString &kind)
{
    if (kind == QLatin1String("low"))
        return AlertTimeKey::persistenceLow();
    if (kind == QLatin1String("high"))
        return AlertTimeKey::persistenceHigh();
    if (kind == QLatin1String("missing"))
        return AlertTimeKey::missingLimit();
    return 0;
}

bool ConfigSettings::setAlertTime(const QString &subjectId, const QString &kind, int seconds)
{
    const char *key = alertTimeKey(kind);
    if (!key || seconds < 0)
        return false;
    if (!m_store.isValid()) { fail(QStringLiteral("config-unreadable")); return false; }
    // The value chosen is written as it is, the default included, so that it stays what the user saw
    // even if a later version changes the default (Decisions taken for review).
    if (!m_store.setSubjectAlertTime(subjectId, key, seconds)) {
        fail(QStringLiteral("unknown-patient"));
        return false;
    }
    return commit();
}

void ConfigSettings::configApplied(bool ok, const QString &result)
{
    if (!ok)
        fail(QStringLiteral("service-no-answer"));
    else if (result == QLatin1String("config-invalid"))
        fail(QStringLiteral("service-config-invalid"));
}

} // namespace ui
} // namespace lm
