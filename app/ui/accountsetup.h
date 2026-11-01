// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Michele Brami
#ifndef LM_UI_ACCOUNTSETUP_H
#define LM_UI_ACCOUNTSETUP_H

// The logic of the account page: enter an account, TEST it, choose the
// people to follow and check their units, and only then SAVE. Pure logic over an AccountBackend,
// so the host build can test every step with a scripted backend and no screen.
//
// States (a plain text, for the page): editing, testing, test-failed, test-ok, saving, saved,
// save-failed.
//
// Rules this class exists to keep:
//  - NOTHING is stored before Save, and Save is possible only after a successful test of the
//    SAME account name and password that are in the fields now: changing either field throws the
//    test result away (the page calls fieldsChanged()).
//  - The password is held only for the time between the test and the end of saving, in memory,
//    and is cleared when saving ends, on reset and when the fields change. It is never exposed
//    as a property, never logged.
//  - At least one person must be followed to save.
//  - Saving confirms the units shown: the user sees each unit and how it was determined and may
//    correct it first (REQ-SUB-05); a corrected unit gets the basis "user".

#include "accountbackend.h"

#include <QObject>
#include <QVariantList>

namespace lm {
namespace ui {

class AccountSetup : public QObject
{
    Q_OBJECT
    Q_PROPERTY(QString state READ state NOTIFY changed)
    Q_PROPERTY(QString errorKey READ errorKey NOTIFY changed)
    Q_PROPERTY(QVariantList subjects READ subjects NOTIFY changed)
    Q_PROPERTY(bool canTest READ canTest NOTIFY changed)
    Q_PROPERTY(bool canSave READ canSave NOTIFY changed)
    Q_PROPERTY(bool busy READ busy NOTIFY changed)

public:
    explicit AccountSetup(QObject *parent = 0);
    void setBackend(AccountBackend *backend);

    QString state() const { return m_state; }
    QString errorKey() const { return m_errorKey; }
    // One map per person found: ref (never shown), name, follow, unit, unitBasis.
    QVariantList subjects() const;
    bool canTest() const;
    bool canSave() const;
    bool busy() const { return m_state == QLatin1String("testing") || m_state == QLatin1String("saving"); }

    // The fields of the page. `fieldsChanged` must be called whenever either text changes.
    Q_INVOKABLE void fieldsChanged(const QString &username, const QString &password);
    Q_INVOKABLE void test();
    Q_INVOKABLE void setFollow(int index, bool follow);
    Q_INVOKABLE void setName(int index, const QString &name);
    Q_INVOKABLE void setUnit(int index, const QString &unit);
    Q_INVOKABLE void save();
    // Back to the start: forgets the test, the people and the password.
    Q_INVOKABLE void reset();
    // The page gives the translated word "Account", the label the connection gets at its
    // first save; the texts live in the QML, where the translation tool finds them.
    Q_INVOKABLE void setDefaultConnectionLabel(const QString &label) { m_defaultLabel = label.trimmed(); }

signals:
    void changed();

private slots:
    void onLoginTested(const lm::ui::LoginTestResult &result);
    void onSaved(bool ok, const QString &errorKey);

private:
    struct Person
    {
        Person() : follow(true) {}
        QString ref, name, unit, unitBasis;
        bool follow;
    };

    void setState(const QString &state, const QString &errorKey = QString());
    void forgetTest();

    AccountBackend *m_backend;
    QString m_state, m_errorKey;
    QString m_user, m_password;         // the fields as last reported by the page
    QString m_testedUser, m_testedPassword;
    QString m_defaultLabel;
    QList<Person> m_people;
};

} // namespace ui
} // namespace lm

#endif
