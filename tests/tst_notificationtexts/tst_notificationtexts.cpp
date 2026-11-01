// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Michele Brami
//
// The words of the notifications in English and Italian. The translation files are compiled with
// lrelease exactly as the package does it and loaded by the service's own translator. What is
// proved: every kind of notification has its summary and body in both languages, the value and
// the trend appear only with the setting, no summary carries a label or the application's name,
// plurals use the language's forms, the staleness cause is the user interface's own text, a
// language change needs no restart, and the English in the source equals the English in the
// translation file. Every value and label is synthetic.
#include "service/notificationtexts.h"
#include "service/alertnotification.h"
#include "service/subjectlabel.h"

#include <QProcess>
#include <QRegularExpression>
#include <QTemporaryDir>
#include <QtTest>

using namespace lm;

namespace {

QString zoneTime(const UtcTime &t)
{
    return QDateTime::fromMSecsSinceEpoch(t.msecsSinceEpoch(), Qt::UTC).toString(QStringLiteral("HH:mm"));
}

UtcTime at(int h, int m) { return UtcTime::fromIso(QStringLiteral("2026-10-18T%1:%2:00Z").arg(h, 2, 10, QLatin1Char('0')).arg(m, 2, 10, QLatin1Char('0'))); }

AlarmTextInput alarmInput(bool showValue)
{
    AlarmTextInput in;
    in.label = QStringLiteral("ABC");
    in.showValue = showValue;
    in.haveValue = true;
    in.value = 52;
    in.unit = QStringLiteral("mg/dL");
    in.trendCode = 2;
    in.facts.levelSince = at(3, 12);
    in.facts.episodeSince = at(3, 12);
    in.facts.limit = 54;
    in.facts.lastReadingAt = at(3, 12);
    in.facts.cause = QStringLiteral("source-unreachable");
    in.now = at(3, 20);
    return in;
}

} // namespace

class TstNotificationTexts : public QObject
{
    Q_OBJECT

    QTemporaryDir m_dir;
    NotificationTranslator m_translator;

    void use(const char *language)
    {
        QCOMPARE(m_translator.load(m_dir.path(), QStringLiteral("harbour-libre-monitor"), QString::fromLatin1(language)),
                 QString::fromLatin1(language));
    }
    NotificationTexts texts() const { return NotificationTexts(zoneTime); }

private slots:
    // The five glyphs, each followed by U+FE0E; the single table, and an unknown code gives none.
    void theTrendGlyphsAreTheFiveOfD98WithTheTextSelector()
    {
        const ushort want[] = { 0x2193, 0x2198, 0x2248, 0x2197, 0x2191 };
        for (int code = 1; code <= 5; ++code) {
            const QString g = NotificationTexts::trendArrow(code);
            QCOMPARE(g.size(), 2);
            QCOMPARE(g.at(0).unicode(), want[code - 1]);
            QCOMPARE(g.at(1).unicode(), ushort(0xFE0E));
        }
        QVERIFY(NotificationTexts::trendArrow(0).isEmpty());
        QVERIFY(NotificationTexts::trendArrow(6).isEmpty());
        QVERIFY(NotificationTexts::trendArrow(-1).isEmpty());
        QCOMPARE(NotificationTexts::valueText(100, QStringLiteral("mg/dL"), 3),
                 QString::fromUtf8("100 mg/dL \xE2\x89\x88\xEF\xB8\x8E"));
    }

    void initTestCase()
    {
        QVERIFY(m_dir.isValid());
        const char *files[] = { "harbour-libre-monitor", "harbour-libre-monitor-it" };
        for (int i = 0; i < 2; ++i) {
            QProcess p;
            p.start(QStringLiteral("lrelease"), QStringList() << QStringLiteral("-idbased")
                    << QString(LM_TRANSLATIONS_DIR) + QLatin1Char('/') + QLatin1String(files[i]) + QStringLiteral(".ts")
                    << QStringLiteral("-qm") << m_dir.path() + QLatin1Char('/') + QLatin1String(files[i]) + QStringLiteral(".qm"));
            QVERIFY2(p.waitForFinished(30000), "lrelease did not run");
            QCOMPARE(p.exitCode(), 0);
        }
    }

    // ---- the alarms ----------------------------------------------------------------------

    void theFiveAlarmsInEnglishWithoutTheValue()
    {
        use("en");
        const NotificationTexts t = texts();
        const AlarmTextInput in = alarmInput(false);
        const QString ack = QStringLiteral("In Events, tap this entry to acknowledge.");
        QCOMPARE(t.alarm(NoteKind::UrgentLow, in).summary, QStringLiteral("VERY LOW GLUCOSE"));
        QCOMPARE(t.alarm(NoteKind::UrgentLow, in).body,
                 QStringLiteral("ABC\nBelow 54 mg/dL since 03:12 (8 min).\n") + ack);
        QCOMPARE(t.alarm(NoteKind::Low, in).summary, QStringLiteral("LOW GLUCOSE"));
        QCOMPARE(t.alarm(NoteKind::Low, in).body, QStringLiteral("ABC\nBelow 54 mg/dL since 03:12 (8 min).\n") + ack);
        AlarmTextInput high = in;
        high.facts.limit = 180;
        QCOMPARE(t.alarm(NoteKind::High, high).summary, QStringLiteral("HIGH GLUCOSE"));
        QCOMPARE(t.alarm(NoteKind::High, high).body, QStringLiteral("ABC\nAbove 180 mg/dL since 03:12 (8 min).\n") + ack);
        high.facts.limit = 250;
        QCOMPARE(t.alarm(NoteKind::UrgentHigh, high).summary, QStringLiteral("VERY HIGH GLUCOSE"));
        QCOMPARE(t.alarm(NoteKind::UrgentHigh, high).body, QStringLiteral("ABC\nAbove 250 mg/dL since 03:12 (8 min).\n") + ack);
        QCOMPARE(t.alarm(NoteKind::NoData, in).summary, QStringLiteral("NO GLUCOSE DATA"));
        QCOMPARE(t.alarm(NoteKind::NoData, in).body,
                 QStringLiteral("ABC\nNo reading since 03:12 (8 min): The source cannot be reached.\n") + ack);
    }

    void theFiveAlarmsInItalian()
    {
        use("it");
        const NotificationTexts t = texts();
        const AlarmTextInput in = alarmInput(false);
        const QString ack = QStringLiteral("In Eventi, tocca questa voce per confermare.");
        QCOMPARE(t.alarm(NoteKind::UrgentLow, in).summary, QStringLiteral("GLICEMIA MOLTO BASSA"));
        QCOMPARE(t.alarm(NoteKind::UrgentLow, in).body, QStringLiteral("ABC\nSotto 54 mg/dL dalle 03:12 (8 min).\n") + ack);
        QCOMPARE(t.alarm(NoteKind::Low, in).summary, QStringLiteral("GLICEMIA BASSA"));
        AlarmTextInput high = in;
        high.facts.limit = 180;
        QCOMPARE(t.alarm(NoteKind::High, high).summary, QStringLiteral("GLICEMIA ALTA"));
        QCOMPARE(t.alarm(NoteKind::High, high).body, QStringLiteral("ABC\nSopra 180 mg/dL dalle 03:12 (8 min).\n") + ack);
        QCOMPARE(t.alarm(NoteKind::UrgentHigh, high).summary, QStringLiteral("GLICEMIA MOLTO ALTA"));
        QCOMPARE(t.alarm(NoteKind::NoData, in).summary, QStringLiteral("NESSUN DATO GLICEMIA"));
        QCOMPARE(t.alarm(NoteKind::NoData, in).body,
                 QStringLiteral("ABC\nNessuna lettura dalle 03:12 (8 min): La fonte non è raggiungibile.\n") + ack);
    }

    // How a patient is named on line 1 of an alarm and in the approval notice, in both
    // languages: (1) the short label of the configuration; (2) no label: P and the position in the
    // file; (3) a "label" too long to be one (it may be a name typed in the wrong field): treated as
    // absent, never shortened, so no part of it reaches the lock screen. The same pure function is the
    // one the cover can call (core/service/subjectlabel.h). Synthetic labels only.
    void thePatientIsNamedByLabelOrPositionInBothLanguages_data()
    {
        QTest::addColumn<QString>("language");
        QTest::newRow("English") << QStringLiteral("en");
        QTest::newRow("Italian") << QStringLiteral("it");
    }
    void thePatientIsNamedByLabelOrPositionInBothLanguages()
    {
        QFETCH(QString, language);
        use(language.toLatin1().constData());
        const QUuid a = QUuid::createUuid(), b = QUuid::createUuid(), c = QUuid::createUuid();
        QHash<QUuid, QString> labels;
        labels.insert(a, QStringLiteral(" AB "));
        labels.insert(c, QStringLiteral("Annabella"));
        SubjectLabelBook book;
        book.set(QList<QUuid>() << a << b << c, labels);
        QCOMPARE(book.labelOf(a), QStringLiteral("AB"));
        QCOMPARE(book.labelOf(b), QStringLiteral("P2"));
        QCOMPARE(book.labelOf(c), QStringLiteral("P3"));
        QCOMPARE(book.labelOf(QUuid::createUuid()), QStringLiteral("?"));

        const NotificationTexts t = texts();
        const QUuid all[] = { a, b, c };
        for (int i = 0; i < 3; ++i) {
            AlarmTextInput in = alarmInput(false);
            in.label = book.labelOf(all[i]);
            const QString body = t.alarm(NoteKind::UrgentLow, in).body;
            QCOMPARE(body.section(QLatin1Char('\n'), 0, 0), book.labelOf(all[i]));
            QVERIFY(!body.contains(QStringLiteral("Anna")));
            QVERIFY(t.waitingForApproval(book.labelOf(all[i])).body.startsWith(book.labelOf(all[i]) + QLatin1Char(' ')));
        }
        // With the value shown, the label still leads the first line.
        AlarmTextInput shown = alarmInput(true);
        shown.label = book.labelOf(b);
        QVERIFY(t.alarm(NoteKind::Low, shown).body.startsWith(QStringLiteral("P2 · 52 mg/dL")));
        // The pure function by itself: trimmed, at most three characters, positions from 1.
        QCOMPARE(patientLabel(QStringLiteral("XYZ"), 4), QStringLiteral("XYZ"));
        QCOMPARE(patientLabel(QStringLiteral("WXYZ"), 4), QStringLiteral("P5"));
        QCOMPARE(patientLabel(QStringLiteral("   "), 0), QStringLiteral("P1"));
        QCOMPARE(patientLabel(QString(), -1), QStringLiteral("?"));
    }

    // The setting "Show name and value in alerts": only then the value, the unit and the arrow.
    void theValueAppearsOnlyWithTheSetting()
    {
        use("en");
        const NotificationTexts t = texts();
        const QString off = t.alarm(NoteKind::UrgentLow, alarmInput(false)).body;
        const QString on = t.alarm(NoteKind::UrgentLow, alarmInput(true)).body;
        QVERIFY(!off.contains(QStringLiteral("52")));
        QVERIFY(on.startsWith(QString::fromUtf8("ABC \xC2\xB7 52 mg/dL \xE2\x86\x98\xEF\xB8\x8E\n")));    // ABC · 52 mg/dL ↘ and U+FE0E
        // The setting on but no reading to read: the label alone, never an invented value.
        AlarmTextInput none = alarmInput(true);
        none.haveValue = false;
        QVERIFY(t.alarm(NoteKind::UrgentLow, none).body.startsWith(QStringLiteral("ABC\n")));
        // A value in mmol/L keeps one decimal, and its limit is shown in mmol/L too.
        AlarmTextInput mm = alarmInput(true);
        mm.value = 2.9;
        mm.unit = QStringLiteral("mmol/L");
        mm.trendCode = -1;
        const QString body = t.alarm(NoteKind::UrgentLow, mm).body;
        QVERIFY(body.startsWith(QString::fromUtf8("ABC \xC2\xB7 2.9 mmol/L\n")));
        QVERIFY(body.contains(QStringLiteral("Below 3.0 mmol/L")));
    }

    void noSummaryCarriesALabelOrTheApplicationName()
    {
        const char *langs[] = { "en", "it" };
        for (int l = 0; l < 2; ++l) {
            use(langs[l]);
            const NotificationTexts t = texts();
            QStringList summaries;
            const AlarmTextInput in = alarmInput(true);
            for (int i = 0; i < NotificationCatalog::all().size(); ++i) {
                const NoteKind k = NotificationCatalog::all().at(i);
                if (NotificationCatalog::isAlarm(k)) summaries << t.alarm(k, in).summary;
            }
            summaries << t.waitingForApproval(QStringLiteral("ABC")).summary << t.reportProposed().summary
                      << t.alertsWillBeSilent().summary << t.interruption(at(1, 0), at(2, 0)).summary
                      << t.guardDisabled(QStringLiteral("ABC"), 3, QStringLiteral("score")).summary;
            for (int i = 0; i < summaries.size(); ++i) {
                const QString s = summaries.at(i);
                QVERIFY2(!s.contains(QStringLiteral("ABC")), qPrintable(s));
                QVERIFY2(!s.contains(QStringLiteral("Libre"), Qt::CaseInsensitive), qPrintable(s));
                QVERIFY2(s == s.toUpper(), qPrintable(s));                       // capitals
                QVERIFY2(!s.contains(QRegularExpression(QStringLiteral("[0-9]"))), qPrintable(s));
                QVERIFY2(!s.startsWith(QStringLiteral("note-")) && !s.startsWith(QStringLiteral("ui-")), qPrintable(s));   // translated, not an id
            }
        }
    }

    // The cause is the user interface's own text of the staleness cause, in the language of the notification.
    void theCauseIsTheUserInterfacesOwnText()
    {
        struct Row { const char *key; const char *en; const char *it; };
        const Row rows[] = {
            { "source-unreachable", "The source cannot be reached", "La fonte non è raggiungibile" },
            { "no-new-readings", "The source has no new readings", nullptr },
            { "sensor-warm-up", "The sensor is warming up", nullptr },
            { "credentials-locked", "The stored login waits for your approval", nullptr },
            { "something-new", "No current reading", "Nessuna lettura attuale" },
        };
        for (size_t i = 0; i < sizeof(rows) / sizeof(rows[0]); ++i) {
            use("en");
            QCOMPARE(NotificationTexts::causeText(QString::fromLatin1(rows[i].key)), QString::fromLatin1(rows[i].en));
            use("it");
            const QString it = NotificationTexts::causeText(QString::fromLatin1(rows[i].key));
            QVERIFY(it != QString::fromLatin1(rows[i].en) || rows[i].it == nullptr);
            QVERIFY(!it.startsWith(QStringLiteral("ui-")));
            if (rows[i].it)
                QCOMPARE(it, QString::fromUtf8(rows[i].it));
        }
    }

    // ---- the notices ------------------------------------------------------------------------

    void theNoticesInEnglish()
    {
        use("en");
        const NotificationTexts t = texts();
        QCOMPARE(t.waitingForApproval(QStringLiteral("ABC")).summary, QStringLiteral("APPROVE NEW PATIENT"));
        QCOMPARE(t.waitingForApproval(QStringLiteral("ABC")).body,
                 QStringLiteral("ABC is waiting to be monitored.\nNo alerts until you approve in the app."));
        QCOMPARE(t.interruption(at(1, 5), at(2, 30)).summary, QStringLiteral("GLUCOSE MONITOR WAS DOWN"));
        QCOMPARE(t.interruption(at(1, 5), at(2, 30)).body,
                 QStringLiteral("No readings were collected from 01:05 to 02:30.\nAlerts could not fire during that time."));
        QCOMPARE(t.guardDisabled(QStringLiteral("Home"), 3, QStringLiteral("no-new-readings")).summary, QStringLiteral("GLUCOSE SOURCE DISABLED"));
        QCOMPARE(t.guardDisabled(QStringLiteral("Home"), 3, QStringLiteral("no-new-readings")).body,
                 QStringLiteral("Home was disabled after 3 failed attempts.\nCause: The source has no new readings. Re-enable it in the app."));
        QVERIFY(t.guardDisabled(QStringLiteral("Home"), 1, QStringLiteral("x")).body.contains(QStringLiteral("after 1 failed attempt.")));
        QCOMPARE(t.reportProposed().summary, QStringLiteral("REVIEW DIAGNOSTIC REPORT"));
        QCOMPARE(t.reportProposed().body,
                 QStringLiteral("A diagnostic report was prepared after repeated problems.\nOpen the app to review it."));
        QCOMPARE(t.alertsWillBeSilent().summary, QStringLiteral("GLUCOSE ALERTS SILENT"));
        QCOMPARE(t.alertsWillBeSilent().body,
                 QStringLiteral("Do not disturb is on: alerts will show, but without sound or vibration.\nTurn it off if you want to be woken."));
    }

    void theNoticesInItalian()
    {
        use("it");
        const NotificationTexts t = texts();
        QCOMPARE(t.waitingForApproval(QStringLiteral("ABC")).summary, QStringLiteral("APPROVA NUOVO PAZIENTE"));
        QCOMPARE(t.waitingForApproval(QStringLiteral("ABC")).body,
                 QStringLiteral("ABC attende di essere monitorato.\nNessun allarme finché non approvi nell'app."));
        QCOMPARE(t.interruption(at(1, 5), at(2, 30)).summary, QStringLiteral("MONITOR GLICEMIA FERMO"));
        QCOMPARE(t.interruption(at(1, 5), at(2, 30)).body,
                 QStringLiteral("Nessuna lettura dalle 01:05 alle 02:30.\nIn quel periodo gli allarmi non potevano scattare."));
        QCOMPARE(t.guardDisabled(QStringLiteral("Casa"), 3, QStringLiteral("sensor-warm-up")).summary, QStringLiteral("FONTE GLICEMIA SPENTA"));
        QCOMPARE(t.guardDisabled(QStringLiteral("Casa"), 3, QStringLiteral("sensor-warm-up")).body,
                 QStringLiteral("Casa è stata disattivata dopo 3 tentativi falliti.\nCausa: Il sensore si sta attivando. Riattivala nell'app."));
        QVERIFY(t.guardDisabled(QStringLiteral("Casa"), 1, QStringLiteral("x")).body.contains(QStringLiteral("dopo 1 tentativo fallito.")));
        QCOMPARE(t.reportProposed().summary, QStringLiteral("VEDI REPORT DIAGNOSTICO"));
        QCOMPARE(t.alertsWillBeSilent().summary, QStringLiteral("ALLARMI GLICEMIA MUTI"));
        QCOMPARE(t.alertsWillBeSilent().body,
                 QStringLiteral("Non disturbare è attivo: gli allarmi compaiono ma senza suono né vibrazione.\nDisattivalo se vuoi essere svegliato."));
    }

    void theEndedEntriesInBothLanguages()
    {
        EffectFacts f;
        f.episodeSince = at(3, 12);
        f.clearedAt = at(3, 40);
        use("en");
        QCOMPARE(texts().ended(NoteKind::EndedUrgentLow, QStringLiteral("ABC"), f).summary, QStringLiteral("VERY LOW GLUCOSE ENDED"));
        QCOMPARE(texts().ended(NoteKind::EndedUrgentLow, QStringLiteral("ABC"), f).body,
                 QStringLiteral("ABC\nThe alert ended at 03:40 after 28 min.\nYou did not acknowledge it."));
        QCOMPARE(texts().ended(NoteKind::EndedLow, QStringLiteral("ABC"), f).summary, QStringLiteral("LOW GLUCOSE ENDED"));
        QCOMPARE(texts().ended(NoteKind::EndedHigh, QStringLiteral("ABC"), f).summary, QStringLiteral("HIGH GLUCOSE ENDED"));
        QCOMPARE(texts().ended(NoteKind::EndedUrgentHigh, QStringLiteral("ABC"), f).summary, QStringLiteral("VERY HIGH GLUCOSE ENDED"));
        use("it");
        QCOMPARE(texts().ended(NoteKind::EndedUrgentLow, QStringLiteral("ABC"), f).summary, QStringLiteral("FINE GLICEMIA MOLTO BASSA"));
        QCOMPARE(texts().ended(NoteKind::EndedLow, QStringLiteral("ABC"), f).body,
                 QStringLiteral("ABC\nL'allarme è terminato alle 03:40 dopo 28 min.\nNon l'hai confermato."));
        QCOMPARE(texts().ended(NoteKind::EndedHigh, QStringLiteral("ABC"), f).summary, QStringLiteral("FINE GLICEMIA ALTA"));
        QCOMPARE(texts().ended(NoteKind::EndedUrgentHigh, QStringLiteral("ABC"), f).summary, QStringLiteral("FINE GLICEMIA MOLTO ALTA"));
    }

    // ---- the resident notice ----------------------------------------------------------------

    void theResidentNoticeInItsFourStatesInEnglish()
    {
        use("en");
        const NotificationTexts t = texts();
        ResidentInput in;
        in.updated = at(7, 5);
        in.patients = 2;
        QCOMPARE(t.residentActive(in).summary, QStringLiteral("GLUCOSE MONITOR ACTIVE"));
        QCOMPARE(t.residentActive(in).body, QStringLiteral("Updated 07:05 \xC2\xB7 2 patients monitored.\nNo active alerts.").replace(QStringLiteral("\xC2\xB7"), QString::fromUtf8("\xC2\xB7")));
        in.patients = 1;
        QVERIFY(t.residentActive(in).body.startsWith(QString::fromUtf8("Updated 07:05 \xC2\xB7 1 patient monitored.")));
        ResidentAlert a, b;
        a.kind = NoteKind::UrgentLow; a.label = QStringLiteral("ABC");
        b.kind = NoteKind::High; b.label = QStringLiteral("DEF"); b.acknowledged = true;
        in.alerts << a << b;
        in.patients = 2;
        in.silentLabels << QStringLiteral("DEF");
        in.doNotDisturb = true;
        QCOMPARE(t.residentActive(in).body,
                 QString::fromUtf8("Updated 07:05 \xC2\xB7 2 patients monitored.\nAlerts: ABC very low, DEF high (acknowledged).\nSilent: DEF. Do not disturb is on."));
        in.silentLabels.clear();
        QVERIFY(t.residentActive(in).body.endsWith(QStringLiteral("\nDo not disturb is on.")));
        in.doNotDisturb = false;
        QCOMPARE(t.residentActive(in).body.split(QLatin1Char('\n')).size(), 2);     // no third line when nothing applies
        in.connectionLabel = QStringLiteral("GHI");
        QCOMPARE(t.residentAwaitingApproval(in).summary, QStringLiteral("APPROVE NEW PATIENT"));
        QVERIFY(t.residentAwaitingApproval(in).body.startsWith(QStringLiteral("GHI is waiting")));
        in.cause = QStringLiteral("store-open-failed");
        QCOMPARE(t.residentAttention(in).summary, QStringLiteral("MONITOR NEEDS ATTENTION"));
        QCOMPARE(t.residentAttention(in).body, QStringLiteral("Monitoring cannot start: An internal problem stopped the reading.\nOpen the app to fix it."));
        in.stoppedAt = at(8, 0);
        QCOMPARE(t.residentStopped(in).summary, QStringLiteral("GLUCOSE ALERTS STOPPED"));
        QCOMPARE(t.residentStopped(in).body,
                 QStringLiteral("Monitoring was stopped at 08:00.\nNo readings are collected and no alerts will fire.\nOpen the app to start it again."));
    }

    void theResidentNoticeInItalian()
    {
        use("it");
        const NotificationTexts t = texts();
        ResidentInput in;
        in.updated = at(7, 5);
        in.patients = 2;
        ResidentAlert a, b;
        a.kind = NoteKind::UrgentHigh; a.label = QStringLiteral("ABC");
        b.kind = NoteKind::Low; b.label = QStringLiteral("DEF"); b.acknowledged = true;
        in.alerts << a << b;
        in.silentLabels << QStringLiteral("DEF");
        in.doNotDisturb = true;
        QCOMPARE(t.residentActive(in).summary, QStringLiteral("MONITOR GLICEMIA ATTIVO"));
        QCOMPARE(t.residentActive(in).body,
                 QString::fromUtf8("Aggiornato 07:05 \xC2\xB7 2 pazienti monitorati.\nAllarmi: ABC molto alta, DEF bassa (confermato).\nSilenzioso: DEF. Non disturbare \xC3\xA8 attivo."));
        in.alerts.clear();
        QVERIFY(t.residentActive(in).body.contains(QStringLiteral("Nessun allarme attivo.")));
        QCOMPARE(t.residentAttention(in).summary, QStringLiteral("MONITOR DA CONTROLLARE"));
        in.stoppedAt = at(8, 0);
        QCOMPARE(t.residentStopped(in).summary, QStringLiteral("ALLARMI GLICEMIA FERMI"));
        QCOMPARE(t.residentStopped(in).body,
                 QStringLiteral("Monitoraggio fermato alle 08:00.\nNessuna lettura e nessun allarme.\nApri l'app per riavviarlo."));
    }

    // ---- the language -------------------------------------------------------------------------

    void theLanguageChangesWithoutARestartAndFallsBackToEnglish()
    {
        const AlarmTextInput in = alarmInput(false);
        use("it");
        QCOMPARE(texts().alarm(NoteKind::Low, in).summary, QStringLiteral("GLICEMIA BASSA"));
        use("en");
        QCOMPARE(texts().alarm(NoteKind::Low, in).summary, QStringLiteral("LOW GLUCOSE"));
        use("it");
        QCOMPARE(texts().alarm(NoteKind::Low, in).summary, QStringLiteral("GLICEMIA BASSA"));
        // A language nobody translated: English, and the loader says so.
        QCOMPARE(m_translator.load(m_dir.path(), QStringLiteral("harbour-libre-monitor"), QStringLiteral("xx")), QStringLiteral("en"));
        QCOMPARE(texts().alarm(NoteKind::Low, in).summary, QStringLiteral("LOW GLUCOSE"));
        // A regional code finds the language file ("it_IT" is Italian).
        QCOMPARE(m_translator.load(m_dir.path(), QStringLiteral("harbour-libre-monitor"), QStringLiteral("it_IT")), QStringLiteral("it"));
        QCOMPARE(texts().alarm(NoteKind::Low, in).summary, QStringLiteral("GLICEMIA BASSA"));
    }

    // The package build runs `lupdate -noobsolete` on the folders the sailfishapp_i18n feature knows and
    // rewrites the .ts files in the source tree: a text lupdate does not find is DROPPED, and the package
    // would then carry notification texts that are only their identifiers (found in).
    // This runs the same command on a copy, with the folders the project lists, and checks that every
    // notification text survives it.
    void theLupdateOfThePackageBuildKeepsTheNotificationTexts()
    {
        QTemporaryDir work;
        QVERIFY(work.isValid());
        const QString root = QString::fromLatin1(LM_SOURCE_ROOT);
        QDir().mkpath(work.path() + QStringLiteral("/app/translations"));
        const QString ts = work.path() + QStringLiteral("/app/translations/harbour-libre-monitor.ts");
        QVERIFY(QFile::copy(root + QStringLiteral("/app/translations/harbour-libre-monitor.ts"), ts));
        // the folders of the feature (src, qml) plus the one app.pro adds through TRANSLATION_SOURCES
        QFile pro(root + QStringLiteral("/app/app.pro"));
        QVERIFY(pro.open(QIODevice::ReadOnly));
        QVERIFY2(QString::fromUtf8(pro.readAll()).contains(QStringLiteral("TRANSLATION_SOURCES += $$PWD/../core/service")),
                 "app.pro must add the service's folder to the sources lupdate reads");
        QProcess p;
        p.start(QStringLiteral("lupdate"), QStringList() << QStringLiteral("-noobsolete")
                << root + QStringLiteral("/core/service") << root + QStringLiteral("/app/src") << root + QStringLiteral("/app/qml")
                << QStringLiteral("-ts") << ts);
        QVERIFY2(p.waitForFinished(60000), "lupdate did not run");
        QCOMPARE(p.exitCode(), 0);
        QFile out(ts);
        QVERIFY(out.open(QIODevice::ReadOnly));
        const QString text = QString::fromUtf8(out.readAll());
        QFile src(QString::fromLatin1(LM_TEXTS_SOURCE));
        QVERIFY(src.open(QIODevice::ReadOnly));
        const QRegularExpression use(QStringLiteral("qtTrId\\(\"(note-[^\"]+)\""));
        QRegularExpressionMatchIterator it = use.globalMatch(QString::fromUtf8(src.readAll()));
        int n = 0;
        while (it.hasNext()) {
            const QString id = it.next().captured(1);
            QVERIFY2(text.contains(QStringLiteral("<message id=\"") + id + QLatin1Char('"')), qPrintable(id + QStringLiteral(" was dropped by lupdate")));
            ++n;
        }
        QVERIFY(n >= 45);
    }

    // ---- the source and the files agree ----------------------------------------------------------

    void everyTextInTheSourceIsInBothTranslationFilesWithTheSameEnglish()
    {
        QFile src(QString::fromLatin1(LM_TEXTS_SOURCE));
        QVERIFY(src.open(QIODevice::ReadOnly));
        const QStringList lines = QString::fromUtf8(src.readAll()).split(QLatin1Char('\n'));
        QMap<QString, QString> english;              // id -> the //% text
        QString pending;
        bool have = false;
        const QRegularExpression comment(QStringLiteral("^\\s*//% \"(.*)\"\\s*$"));
        const QRegularExpression use(QStringLiteral("qtTrId\\(\"([^\"]+)\""));
        for (int i = 0; i < lines.size(); ++i) {
            QRegularExpressionMatch m = comment.match(lines.at(i));
            if (m.hasMatch()) { pending = m.captured(1); have = true; continue; }
            m = use.match(lines.at(i));
            if (m.hasMatch() && have) { english.insert(m.captured(1), pending); have = false; }
        }
        QVERIFY(english.size() >= 45);
        const char *files[] = { "harbour-libre-monitor.ts", "harbour-libre-monitor-it.ts" };
        for (int f = 0; f < 2; ++f) {
            QFile ts(QString(LM_TRANSLATIONS_DIR) + QLatin1Char('/') + QLatin1String(files[f]));
            QVERIFY(ts.open(QIODevice::ReadOnly));
            const QString text = QString::fromUtf8(ts.readAll());
            for (QMap<QString, QString>::const_iterator it = english.constBegin(); it != english.constEnd(); ++it) {
                const QString id = QStringLiteral("<message id=\"") + it.key() + QLatin1Char('"');
                QVERIFY2(text.contains(id), qPrintable(QLatin1String(files[f]) + QStringLiteral(": missing ") + it.key()));
                QString en = it.value();
                en.replace(QStringLiteral("&"), QStringLiteral("&amp;")).replace(QStringLiteral("<"), QStringLiteral("&lt;"));
                const int at = text.indexOf(id);
                QVERIFY2(text.mid(at, 700).contains(QStringLiteral("<source>") + en + QStringLiteral("</source>")),
                         qPrintable(it.key() + QStringLiteral(": the English of the source differs from the file")));
            }
        }
    }
};

QTEST_MAIN(TstNotificationTexts)
#include "tst_notificationtexts.moc"
