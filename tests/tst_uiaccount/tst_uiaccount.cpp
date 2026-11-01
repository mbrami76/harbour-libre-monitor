// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Michele Brami
// Tests of the account page logic (app/ui/accountsetup): test before saving, nothing stored until
// Save, a test only counts for the credentials it was made with, at least one person followed,
// units confirmed or corrected, and the password forgotten after use. A scripted backend stands
// for the service and the secret store.
#include "ui/accounterrors.h"
#include "ui/accountsetup.h"

#include <QtTest>

using namespace lm::ui;

namespace {

// Records what the page asked for and answers when the test says so.
class ScriptedBackend : public AccountBackend
{
public:
    ScriptedBackend() : tests(0), saves(0) {}
    void testLogin(const QString &user, const QString &pass) { ++tests; lastUser = user; lastPass = pass; }
    void save(const SaveRequest &r) { ++saves; lastSave = r; }
    void answerTest(const LoginTestResult &r) { emit loginTested(r); }
    void answerSave(bool ok, const QString &key = QString()) { emit saved(ok, key); }

    int tests, saves;
    QString lastUser, lastPass;
    SaveRequest lastSave;
};

LoginTestResult found()
{
    LoginTestResult r;
    r.ok = true;
    DiscoveredSubject a; a.ref = "r1"; a.displayName = "Anna"; a.unit = "mmol/L"; a.unitBasis = "code";
    DiscoveredSubject b; b.ref = "r2"; b.displayName = "Marco"; b.unit = "mg/dL"; b.unitBasis = "relation";
    r.subjects << a << b;
    return r;
}

LoginTestResult failed(const char *key)
{
    LoginTestResult r;
    r.errorKey = QString::fromLatin1(key);
    return r;
}

}

namespace {
QString readFile(const QString &path) { QFile f(path); f.open(QIODevice::ReadOnly); return QString::fromUtf8(f.readAll()); }
// The translation of a text identifier in a .ts file: empty when missing or unfinished.
QString translationOf(const QString &ts, const QString &id)
{
    const int at = ts.indexOf("<message id=\"" + id + "\"");
    if (at < 0) return QString();
    const int end = ts.indexOf("</message>", at);
    const QString block = ts.mid(at, end - at);
    if (block.contains("type=\"unfinished\"")) return QString();
    const int a = block.indexOf("<translation>");
    const int b = block.indexOf("</translation>");
    return a < 0 || b < 0 ? QString() : block.mid(a + 13, b - a - 13);
}
}

class TstUiAccount : public QObject
{
    Q_OBJECT

private slots:
    // ---- every error key has a text, in every place that needs one -----------------------------------
    void everyErrorKeyHasATextInEnglishItalianAndThePageTable()
    {
        const QString en = readFile(QString(LM_TRANSLATIONS_DIR) + "/harbour-libre-monitor.ts");
        const QString it = readFile(QString(LM_TRANSLATIONS_DIR) + "/harbour-libre-monitor-it.ts");
        const QString words = readFile(QString(LM_QML_COMPONENTS_DIR) + "/StateText.js");
        QVERIFY2(!en.isEmpty() && !it.isEmpty() && !words.isEmpty(), "a source file could not be read");
        const QStringList keys = knownAccountErrorKeys();
        QVERIFY(keys.size() >= 17);
        for (int i = 0; i < keys.size(); ++i) {
            const QString id = accountErrorTextId(keys.at(i));
            const QString where = keys.at(i) + " -> " + id;
            QVERIFY2(!translationOf(en, id).isEmpty(), qPrintable(where + ": no English text"));
            QVERIFY2(!translationOf(it, id).isEmpty(), qPrintable(where + ": no Italian text"));
            QVERIFY2(words.contains("case \"" + keys.at(i) + "\":"), qPrintable(where + ": the page table has no case"));
            QVERIFY2(words.contains("qsTrId(\"" + id + "\")"), qPrintable(where + ": the page table does not use the id"));
        }
    }
    void anUnknownKeyShowsTheInternalText()
    {
        QCOMPARE(normalizedAccountError("something-from-a-newer-service"), QStringLiteral("internal"));
        QCOMPARE(accountErrorTextId("something-from-a-newer-service"), accountErrorTextId("internal"));
        QCOMPARE(normalizedAccountError(""), QStringLiteral("internal"));
        // And through the page logic: a failed test with an unknown key is stored as "internal".
        ScriptedBackend b; AccountSetup a; a.setBackend(&b);
        a.fieldsChanged("me@example", "pw");
        a.test();
        b.answerTest(failed("brand-new-class"));
        QCOMPARE(a.errorKey(), QStringLiteral("internal"));
    }
    void theNewServiceKeysAreKnown()
    {
        const char *keys[] = { "unknown-connection", "credentials-missing", "secrets-locked", "stopped-by-guard",
                               "service-stopping", "service-not-running", "internal",
                               "auth-failed", "terms-must-be-accepted", "rate-limited", "unreachable", "protocol-changed" };
        for (unsigned i = 0; i < sizeof(keys) / sizeof(keys[0]); ++i)
            QCOMPARE(normalizedAccountError(QString::fromLatin1(keys[i])), QString::fromLatin1(keys[i]));
        QVERIFY(accountErrorTextId("secrets-locked") != accountErrorTextId("stopped-by-guard"));
    }

    void cannotTestWithoutBothFields()
    {
        ScriptedBackend b; AccountSetup a; a.setBackend(&b);
        QVERIFY(!a.canTest());
        a.fieldsChanged("me@example", "");
        QVERIFY(!a.canTest());
        a.fieldsChanged("  ", "pw");
        QVERIFY(!a.canTest());
        a.fieldsChanged("me@example", "pw");
        QVERIFY(a.canTest());
    }
    void testingStoresNothingAndAsksTheBackend()
    {
        ScriptedBackend b; AccountSetup a; a.setBackend(&b);
        a.fieldsChanged("me@example", "pw");
        a.test();
        QCOMPARE(a.state(), QStringLiteral("testing"));
        QVERIFY(a.busy());
        QVERIFY(!a.canTest());                          // no second test while one runs
        QCOMPARE(b.tests, 1);
        QCOMPARE(b.lastUser, QStringLiteral("me@example"));
        QCOMPARE(b.lastPass, QStringLiteral("pw"));
        QCOMPARE(b.saves, 0);                           // nothing is stored by a test
    }
    void aSuccessfulTestShowsThePeopleAndAllowsSaving()
    {
        ScriptedBackend b; AccountSetup a; a.setBackend(&b);
        a.fieldsChanged("me@example", "pw");
        a.test();
        b.answerTest(found());
        QCOMPARE(a.state(), QStringLiteral("test-ok"));
        QCOMPARE(a.subjects().size(), 2);
        QCOMPARE(a.subjects()[0].toMap()["name"].toString(), QStringLiteral("Anna"));
        QVERIFY(a.subjects()[0].toMap()["follow"].toBool());        // followed by default
        QCOMPARE(a.subjects()[1].toMap()["unitBasis"].toString(), QStringLiteral("relation"));
        QVERIFY(a.canSave());
        QVERIFY(!a.subjects()[0].toMap().contains("ref"));          // the reference is never shown
    }
    void aFailedTestKeepsItsKeyAndNeverAllowsSaving()
    {
        const char *keys[] = { "auth-failed", "terms-must-be-accepted", "rate-limited", "unreachable", "protocol-changed" };
        for (unsigned i = 0; i < sizeof(keys) / sizeof(keys[0]); ++i) {
            ScriptedBackend b; AccountSetup a; a.setBackend(&b);
            a.fieldsChanged("me@example", "pw");
            a.test();
            b.answerTest(failed(keys[i]));
            QCOMPARE(a.state(), QStringLiteral("test-failed"));
            QCOMPARE(a.errorKey(), QString::fromLatin1(keys[i]));
            QVERIFY(!a.canSave());
            QVERIFY(a.canTest());                       // the user may fix and try again
        }
    }
    void anAccountThatSeesNobodyIsAFailureWithItsOwnKey()
    {
        ScriptedBackend b; AccountSetup a; a.setBackend(&b);
        a.fieldsChanged("me@example", "pw");
        a.test();
        LoginTestResult r; r.ok = true;
        b.answerTest(r);
        QCOMPARE(a.state(), QStringLiteral("test-failed"));
        QCOMPARE(a.errorKey(), QStringLiteral("no-subjects"));
    }
    void changingEitherFieldThrowsTheTestAway()
    {
        ScriptedBackend b; AccountSetup a; a.setBackend(&b);
        a.fieldsChanged("me@example", "pw");
        a.test();
        b.answerTest(found());
        QVERIFY(a.canSave());
        a.fieldsChanged("me@example", "pw2");
        QCOMPARE(a.state(), QStringLiteral("editing"));
        QVERIFY(!a.canSave());
        QVERIFY(a.subjects().isEmpty());
        // Same for the account name.
        a.test();
        b.answerTest(found());
        QVERIFY(a.canSave());
        a.fieldsChanged("other@example", "pw2");
        QVERIFY(!a.canSave());
    }
    void saveNeedsAtLeastOnePersonFollowed()
    {
        ScriptedBackend b; AccountSetup a; a.setBackend(&b);
        a.fieldsChanged("me@example", "pw");
        a.test();
        b.answerTest(found());
        a.setFollow(0, false);
        QVERIFY(a.canSave());
        a.setFollow(1, false);
        QVERIFY(!a.canSave());
        a.save();
        QCOMPARE(b.saves, 0);
        a.setFollow(1, true);
        QVERIFY(a.canSave());
    }
    void saveSendsOnlyTheFollowedWithEditedNamesAndCorrectedUnits()
    {
        ScriptedBackend b; AccountSetup a; a.setBackend(&b);
        a.fieldsChanged("me@example", "pw");
        a.test();
        b.answerTest(found());
        a.setFollow(0, false);                          // not Anna
        a.setName(1, "  Marco B.  ");
        a.setUnit(1, "mmol/L");                         // corrected by the user
        a.setUnit(1, "stones");                         // nonsense is ignored
        a.save();
        QCOMPARE(a.state(), QStringLiteral("saving"));
        QCOMPARE(b.saves, 1);
        QCOMPARE(b.lastSave.connectionId, QStringLiteral("c1"));
        QCOMPARE(b.lastSave.username, QStringLiteral("me@example"));
        QCOMPARE(b.lastSave.password, QStringLiteral("pw"));
        QCOMPARE(b.lastSave.subjects.size(), 1);
        QCOMPARE(b.lastSave.subjects[0].ref, QStringLiteral("r2"));
        QCOMPARE(b.lastSave.subjects[0].name, QStringLiteral("Marco B."));
        QCOMPARE(b.lastSave.subjects[0].unit, QStringLiteral("mmol/L"));
        QCOMPARE(b.lastSave.subjects[0].unitBasis, QStringLiteral("user"));
        QVERIFY(b.lastSave.subjects[0].unitConfirmed);
    }
    void anUntouchedUnitKeepsItsBasis()
    {
        ScriptedBackend b; AccountSetup a; a.setBackend(&b);
        a.fieldsChanged("me@example", "pw");
        a.test();
        b.answerTest(found());
        a.save();
        QCOMPARE(b.lastSave.subjects.size(), 2);
        QCOMPARE(b.lastSave.subjects[0].unitBasis, QStringLiteral("code"));
        QCOMPARE(b.lastSave.subjects[1].unitBasis, QStringLiteral("relation"));
    }
    void aSuccessfulSaveEndsSavedAndForgetsThePassword()
    {
        ScriptedBackend b; AccountSetup a; a.setBackend(&b);
        a.fieldsChanged("me@example", "pw");
        a.test();
        b.answerTest(found());
        a.save();
        b.answerSave(true);
        QCOMPARE(a.state(), QStringLiteral("saved"));
        QVERIFY(a.subjects().isEmpty());
        QVERIFY(!a.canSave());
        QVERIFY(!a.canTest());                          // the password field was cleared inside
    }
    // the save carries the connection's default label, the translated word
    // "Account" the page hands over; the backend writes it only when the connection has none yet.
    void theSaveCarriesTheDefaultLabelOfTheAccount()
    {
        ScriptedBackend b; AccountSetup a; a.setBackend(&b);
        a.fieldsChanged("me@example", "pw");
        a.test();
        b.answerTest(found());
        a.setDefaultConnectionLabel(QStringLiteral(" Account "));
        a.save();
        QCOMPARE(b.saves, 1);
        QCOMPARE(b.lastSave.connectionLabel, QStringLiteral("Account"));
        QCOMPARE(b.lastSave.connectionId, QStringLiteral("c1"));
    }
    void aFailedSaveKeepsThePeopleButNeedsThePasswordAgain()
    {
        ScriptedBackend b; AccountSetup a; a.setBackend(&b);
        a.fieldsChanged("me@example", "pw");
        a.test();
        b.answerTest(found());
        a.save();
        b.answerSave(false, "secret-store-failed");
        QCOMPARE(a.state(), QStringLiteral("save-failed"));
        QCOMPARE(a.errorKey(), QStringLiteral("secret-store-failed"));
        QVERIFY(!a.canSave());                          // the test result needs the password again
        QVERIFY(!a.canTest());
        a.fieldsChanged("me@example", "pw");
        QVERIFY(a.canTest());
    }
    void lateAnswersAfterAResetAreIgnored()
    {
        ScriptedBackend b; AccountSetup a; a.setBackend(&b);
        a.fieldsChanged("me@example", "pw");
        a.test();
        a.reset();
        b.answerTest(found());
        QCOMPARE(a.state(), QStringLiteral("editing"));
        QVERIFY(a.subjects().isEmpty());
    }
    void editingWhileTestingKeepsTheTestState()
    {
        ScriptedBackend b; AccountSetup a; a.setBackend(&b);
        a.fieldsChanged("me@example", "pw");
        a.test();
        a.fieldsChanged("me@example", "px");            // the page disables fields, but be safe
        QCOMPARE(a.state(), QStringLiteral("testing"));
        b.answerTest(found());
        QVERIFY(!a.canSave());                          // the fields differ from what was tested
    }
};

QTEST_APPLESS_MAIN(TstUiAccount)
#include "tst_uiaccount.moc"
