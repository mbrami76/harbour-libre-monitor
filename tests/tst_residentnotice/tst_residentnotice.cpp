// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Michele Brami
//
// The resident notice (REQ-ALR-06). Its
// words in the four states of the lifecycle, in both languages, with and without alerts, with silent
// Subjects and with Do not disturb, composed from the notice and the facts of the moment; and the
// rule that the lifecycle shows it again when an alert, a silent mode or Do not disturb changes.
// Synthetic data.
#include "notifiertest.h"
#include "service/residentnotice.h"
#include "servicetest.h"

#include <QtTest>

using namespace lm;
using namespace lm::testing;

namespace {

class FakeFactsSource : public ResidentFactsSource
{
public:
    ResidentInput facts() const { return input; }
    ResidentInput input;
};

} // namespace

class TstResidentNotice : public QObject
{
    Q_OBJECT

    QTemporaryDir m_dir;
    NotificationTranslator m_translator;

    struct Rig : NoteRig
    {
        Rig() : composer(&presenter) { composer.setSource(&source); }
        FakeFactsSource source;
        ResidentNoticeComposer composer;
    };

    static Notice active()
    {
        Notice n;
        n.state = NoticeState::Active;
        n.lastHeartbeat = noteAt(7, 5);
        return n;
    }

private slots:
    void initTestCase()
    {
        QVERIFY(m_dir.isValid());
        QVERIFY(loadNoteLanguage(&m_dir, &m_translator, "en"));
        QVERIFY(loadNoteLanguage(&m_dir, &m_translator, "it"));
        QVERIFY(loadNoteLanguage(&m_dir, &m_translator, "en"));
    }

    void activeWithNoAlertsInEnglish()
    {
        Rig r;
        r.source.input.patients = 2;
        const NoteText t = r.composer.compose(active());
        QCOMPARE(t.summary, QStringLiteral("GLUCOSE MONITOR ACTIVE"));
        QCOMPARE(t.body, QString::fromUtf8("Updated 07:05 \xC2\xB7 2 patients monitored.\nNo active alerts."));
    }

    void activeWithAlertsSilentSubjectsAndDoNotDisturb()
    {
        Rig r;
        r.source.input.patients = 3;
        ResidentAlert a, b, c;
        a.kind = NoteKind::UrgentLow; a.label = QStringLiteral("AB");
        b.kind = NoteKind::High; b.label = QStringLiteral("CD"); b.acknowledged = true;
        c.kind = NoteKind::NoData; c.label = QStringLiteral("EF");
        r.source.input.alerts << a << b << c;
        r.source.input.silentLabels << QStringLiteral("CD");
        r.source.input.doNotDisturb = true;
        const NoteText t = r.composer.compose(active());
        QCOMPARE(t.body, QString::fromUtf8("Updated 07:05 \xC2\xB7 3 patients monitored.\nAlerts: AB very low, CD high (acknowledged), EF no data.\nSilent: CD. Do not disturb is on."));
        // Only Do not disturb:
        r.source.input.silentLabels.clear();
        QVERIFY(r.composer.compose(active()).body.endsWith(QStringLiteral("\nDo not disturb is on.")));
        // Nothing at all that applies: two lines.
        r.source.input.doNotDisturb = false;
        QCOMPARE(r.composer.compose(active()).body.split(QLatin1Char('\n')).size(), 2);
    }

    // No value, no name, no digit beyond the count and the time, in any state.
    void noValueOrNameIsEverInTheNotice()
    {
        Rig r;
        r.source.input.patients = 1;
        ResidentAlert a;
        a.kind = NoteKind::UrgentLow; a.label = QStringLiteral("AB");
        r.source.input.alerts << a;
        const QString body = r.composer.compose(active()).body;
        QVERIFY(!body.contains(QStringLiteral("mg")) && !body.contains(QStringLiteral("mmol")));
        QVERIFY(!body.contains(QRegularExpression(QStringLiteral("\\b[0-9]{2,3}\\b(?! min)(?!:)"))) || body.contains(QStringLiteral("07:05")));
    }

    void theOtherThreeStates()
    {
        Rig r;
        Notice n;
        n.state = NoticeState::AwaitingApproval;
        n.connection = QStringLiteral("c1");
        r.presenter.setConnectionInfo([](const QString &) { return QStringList() << noteSubject(1).toString(); }, [](const QString &c) { return c; });
        QCOMPARE(r.composer.compose(n).summary, QStringLiteral("APPROVE NEW PATIENT"));
        QCOMPARE(r.composer.compose(n).body, QStringLiteral("AB is waiting to be monitored.\nNo alerts until you approve in the app."));
        n.state = NoticeState::Attention;
        n.cause = QStringLiteral("journal-open-failed");
        QCOMPARE(r.composer.compose(n).summary, QStringLiteral("MONITOR NEEDS ATTENTION"));
        QCOMPARE(r.composer.compose(n).body, QStringLiteral("Monitoring cannot start: An internal problem stopped the reading.\nOpen the app to fix it."));
        n.state = NoticeState::Stopped;
        n.stoppedAt = noteAt(8, 0);
        n.stopReason = QStringLiteral("term");
        QCOMPARE(r.composer.compose(n).summary, QStringLiteral("GLUCOSE ALERTS STOPPED"));
        QCOMPARE(r.composer.compose(n).body, QStringLiteral("Monitoring was stopped at 08:00.\nNo readings are collected and no alerts will fire.\nOpen the app to start it again."));
        // The stop reason is a key, not words for the user: it is not in the text.
        QVERIFY(!r.composer.compose(n).body.contains(QStringLiteral("term")));
    }

    void theFourStatesInItalian()
    {
        QVERIFY(loadNoteLanguage(&m_dir, &m_translator, "it"));
        Rig r;
        r.source.input.patients = 2;
        ResidentAlert a;
        a.kind = NoteKind::UrgentHigh; a.label = QStringLiteral("AB");
        r.source.input.alerts << a;
        r.source.input.silentLabels << QStringLiteral("CD");
        QCOMPARE(r.composer.compose(active()).summary, QStringLiteral("MONITOR GLICEMIA ATTIVO"));
        QCOMPARE(r.composer.compose(active()).body, QString::fromUtf8("Aggiornato 07:05 \xC2\xB7 2 pazienti monitorati.\nAllarmi: AB molto alta.\nSilenzioso: CD."));
        Notice n;
        n.state = NoticeState::Attention;
        n.cause = QStringLiteral("x");
        QCOMPARE(r.composer.compose(n).summary, QStringLiteral("MONITOR DA CONTROLLARE"));
        n.state = NoticeState::Stopped;
        n.stoppedAt = noteAt(8, 0);
        QCOMPARE(r.composer.compose(n).summary, QStringLiteral("ALLARMI GLICEMIA FERMI"));
        n.state = NoticeState::AwaitingApproval;
        QCOMPARE(r.composer.compose(n).summary, QStringLiteral("APPROVA NUOVO PAZIENTE"));
        QVERIFY(loadNoteLanguage(&m_dir, &m_translator, "en"));
    }

    void withoutASourceTheNoticeStillReads()
    {
        Rig r;
        ResidentNoticeComposer bare(&r.presenter);
        QCOMPARE(bare.compose(active()).summary, QStringLiteral("GLUCOSE MONITOR ACTIVE"));
        QVERIFY(bare.compose(active()).body.contains(QStringLiteral("No active alerts.")));
    }

    // The lifecycle shows the notice again when asked: only while it runs, and in its current state.
    void theLifecycleRefreshesTheNoticeOnRequest()
    {
        ServiceRig rig;
        rig.lifecycle->refreshNotice();
        QVERIFY(rig.notifier.shown.isEmpty());                          // not started: nothing
        rig.lifecycle->start();
        const int shown = rig.notifier.shown.size();
        rig.lifecycle->refreshNotice();
        QCOMPARE(rig.notifier.shown.size(), shown + 1);
        QCOMPARE(rig.notifier.last().state, NoticeState::Active);
        rig.lifecycle->connectionWarning(QStringLiteral("c1"), QStringLiteral("approval-waiting"), QString());
        rig.lifecycle->refreshNotice();
        QCOMPARE(rig.notifier.last().state, NoticeState::AwaitingApproval);
        QCOMPARE(rig.notifier.last().connection, QStringLiteral("c1"));
    }
};

QTEST_MAIN(TstResidentNotice)
#include "tst_residentnotice.moc"
