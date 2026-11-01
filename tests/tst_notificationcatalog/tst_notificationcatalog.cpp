// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Michele Brami
//
// The catalogue of the notifications. Every row of
// is checked here against the document's table, so a hint
// cannot change without a test saying so. What each row rests on is measured on the phone
// (MA-S9-xx, MA-U2-xx, MA-U2b-xx); this program only proves the table says what the document says.
#include "service/notificationcatalog.h"

#include <QtTest>

using namespace lm;

class TstNotificationCatalog : public QObject
{
    Q_OBJECT

private slots:
    // The five alarms: urgency, priority, sound, wake-up.
    void theAlarmsFollowTheTableOfSection2()
    {
        struct Row { NoteKind kind; int urgency; int priority; const char *feedback; const char *icon; };
        const Row rows[] = {
            { NoteKind::UrgentLow,  2, 125, "clock",           "alert-ll" },
            { NoteKind::UrgentHigh, 2, 125, "clock",           "alert-hh" },
            { NoteKind::Low,        2, 100, "calendar",        "alert-l" },
            { NoteKind::High,       2, 100, "calendar",        "alert-h" },
            { NoteKind::NoData,     2, 100, "general_warning", "alert-nodata" },
        };
        for (size_t i = 0; i < sizeof(rows) / sizeof(rows[0]); ++i) {
            const NoteStyle &s = NotificationCatalog::style(rows[i].kind);
            const QString where = NotificationCatalog::name(rows[i].kind);
            QVERIFY2(s.urgency == rows[i].urgency, qPrintable(where));
            QVERIFY2(s.priority == rows[i].priority, qPrintable(where));
            QVERIFY2(s.feedback == QLatin1String(rows[i].feedback), qPrintable(where));
            QVERIFY2(s.icon == QLatin1String(rows[i].icon), qPrintable(where));
            QVERIFY2(s.displayOn && s.banner && !s.removable, qPrintable(where));       // every alert wakes the screen
            QVERIFY(NotificationCatalog::isAlarm(rows[i].kind));
        }
    }

    // The notices and the page-only entries.
    void theNoticesFollowTheTableOfSection2()
    {
        // page only: urgency 0, priority 50, no feedback, no banner
        const NoteKind pageOnly[] = { NoteKind::Interruption, NoteKind::GuardDisabled, NoteKind::ReportProposed,
                                      NoteKind::EndedUrgentLow, NoteKind::EndedUrgentHigh, NoteKind::EndedLow, NoteKind::EndedHigh };
        for (size_t i = 0; i < sizeof(pageOnly) / sizeof(pageOnly[0]); ++i) {
            const NoteStyle &s = NotificationCatalog::style(pageOnly[i]);
            const QString where = NotificationCatalog::name(pageOnly[i]);
            QVERIFY2(s.urgency == 0 && s.priority == 50 && s.feedback.isEmpty() && !s.banner && !s.displayOn, qPrintable(where));
            QVERIFY2(!NotificationCatalog::isAlarm(pageOnly[i]), qPrintable(where));
        }
        // only the "ended" entries are removable by the user
        QVERIFY(NotificationCatalog::style(NoteKind::EndedLow).removable);
        QVERIFY(!NotificationCatalog::style(NoteKind::Interruption).removable);
        const NoteStyle &appr = NotificationCatalog::style(NoteKind::WaitingApproval);
        QCOMPARE(appr.urgency, 1);
        QCOMPARE(appr.priority, 100);
        QCOMPARE(appr.feedback, QStringLiteral("general_warning"));
        QVERIFY(appr.banner);
        const NoteStyle &silent = NotificationCatalog::style(NoteKind::AlertsSilent);
        QCOMPARE(silent.urgency, 1);
        QCOMPARE(silent.priority, 50);
        QVERIFY(silent.feedback.isEmpty());
        QVERIFY(silent.banner);
    }

    // The icons are the author's designs in design/export/notify/ (read only for this module) and are
    // installed from there; the catalogue only names them. Every kind must name a file that exists
    // there, as a 256 px PNG with its own colours.
    void everyIconNamedExistsAsA256pxPng()
    {
        for (int i = 0; i < NotificationCatalog::all().size(); ++i) {
            const NoteKind k = NotificationCatalog::all().at(i);
            const QString file = NotificationCatalog::style(k).icon + QStringLiteral(".png");
            const QString path = QStringLiteral(LM_REPO_DIR "/design/export/notify/") + file;
            QVERIFY2(QFile::exists(path), qPrintable(NotificationCatalog::name(k) + ": " + file));
            QImage img(path);
            QVERIFY2(!img.isNull(), qPrintable(file));
            QVERIFY2(img.width() == 256 && img.height() == 256, qPrintable(file));
            QVERIFY2(img.hasAlphaChannel(), qPrintable(file));
        }
    }

    void theNoticesUseTheThreeIconsOfD100()
    {
        QCOMPARE(NotificationCatalog::style(NoteKind::AlertsSilent).icon, QStringLiteral("notice-1"));
        QCOMPARE(NotificationCatalog::style(NoteKind::WaitingApproval).icon, QStringLiteral("notice-2"));
        QCOMPARE(NotificationCatalog::style(NoteKind::Interruption).icon, QStringLiteral("notice-2"));
        QCOMPARE(NotificationCatalog::style(NoteKind::ReportProposed).icon, QStringLiteral("notice-2"));
        QCOMPARE(NotificationCatalog::style(NoteKind::GuardDisabled).icon, QStringLiteral("notice-3"));
        const NotificationCatalog cat(QStringLiteral("/i"));
        QCOMPARE(cat.residentIconPath(NoticeState::Active), QStringLiteral("/i/notice-1.png"));
        QCOMPARE(cat.residentIconPath(NoticeState::AwaitingApproval), QStringLiteral("/i/notice-2.png"));
        QCOMPARE(cat.residentIconPath(NoticeState::Attention), QStringLiteral("/i/notice-2.png"));
        QCOMPARE(cat.residentIconPath(NoticeState::Stopped), QStringLiteral("/i/notice-3.png"));
        QVERIFY(NotificationCatalog().residentIconPath(NoticeState::Active).isEmpty());
        QVERIFY(QFile::exists(QStringLiteral(LM_REPO_DIR "/design/export/notify/notice-1.png")));
        QVERIFY(QFile::exists(QStringLiteral(LM_REPO_DIR "/design/export/notify/notice-2.png")));
        QVERIFY(QFile::exists(QStringLiteral(LM_REPO_DIR "/design/export/notify/notice-3.png")));
    }

    // The icon has its own colours: the phone code never sets `x-nemo-color` (it tints the icon and
    // destroys the colours; MA-S9). A guard on the source, because the hint cannot be seen from a host test.
    void noCodeSetsTheColourHint()
    {
        QFile f(QStringLiteral(LM_REPO_DIR "/daemon/phoneports.cpp"));
        QVERIFY(f.open(QIODevice::ReadOnly));
        const QStringList lines = QString::fromUtf8(f.readAll()).split(QLatin1Char('\n'));
        for (int i = 0; i < lines.size(); ++i) {
            const QString l = lines.at(i).trimmed();
            if (l.startsWith(QLatin1String("//")))
                continue;
            const int cut = l.indexOf(QLatin1String("//"));
            QVERIFY2(!(cut < 0 ? l : l.left(cut)).contains(QLatin1String("x-nemo-color")), qPrintable(l));
        }
    }

    void theRankIsStrictAndOnlyForAlarms()
    {
        QCOMPARE(NotificationCatalog::rankOf(NoteKind::UrgentLow), 1);
        QCOMPARE(NotificationCatalog::rankOf(NoteKind::UrgentHigh), 2);
        QCOMPARE(NotificationCatalog::rankOf(NoteKind::NoData), 3);
        QCOMPARE(NotificationCatalog::rankOf(NoteKind::Low), 4);
        QCOMPARE(NotificationCatalog::rankOf(NoteKind::High), 5);
        QCOMPARE(NotificationCatalog::rankOf(NoteKind::Interruption), 0);
        QVERIFY(NotificationCatalog::isUrgentAlarm(NoteKind::UrgentLow) && NotificationCatalog::isUrgentAlarm(NoteKind::UrgentHigh));
        QVERIFY(!NotificationCatalog::isUrgentAlarm(NoteKind::NoData) && !NotificationCatalog::isUrgentAlarm(NoteKind::Low));
    }

    void alertTypesMapToKindsAndEndedKinds()
    {
        QCOMPARE(NotificationCatalog::alarmKindOf(AlertType::LL), NoteKind::UrgentLow);
        QCOMPARE(NotificationCatalog::alarmKindOf(AlertType::HH), NoteKind::UrgentHigh);
        QCOMPARE(NotificationCatalog::alarmKindOf(AlertType::Missing), NoteKind::NoData);
        NoteKind k = NoteKind::NoData;
        QVERIFY(NotificationCatalog::endedKindOf(AlertType::LL, &k) && k == NoteKind::EndedUrgentLow);
        QVERIFY(NotificationCatalog::endedKindOf(AlertType::H, &k) && k == NoteKind::EndedHigh);
        QVERIFY(!NotificationCatalog::endedKindOf(AlertType::Missing, &k));      // no data leaves no ended entry
    }

    // The three sounds an arbiter can ask for.
    void aSoundIsTheOwnEventTheShortEventOrNone()
    {
        const NotificationCatalog cat(QStringLiteral("/usr/share/x/icons"));
        QCOMPARE(cat.hints(NoteKind::UrgentLow, Sound::Own).feedback, QStringLiteral("clock"));
        QCOMPARE(cat.hints(NoteKind::UrgentLow, Sound::Short).feedback, QStringLiteral("calendar"));   // never the long tone
        QCOMPARE(cat.hints(NoteKind::UrgentHigh, Sound::Short).feedback, QStringLiteral("calendar"));
        QCOMPARE(cat.hints(NoteKind::NoData, Sound::Short).feedback, QStringLiteral("general_warning"));
        QCOMPARE(cat.hints(NoteKind::Low, Sound::Short).feedback, QStringLiteral("calendar"));
        for (int i = 0; i < NotificationCatalog::all().size(); ++i)
            QVERIFY(cat.hints(NotificationCatalog::all().at(i), Sound::None).feedback.isEmpty());
        QCOMPARE(cat.hints(NoteKind::WaitingApproval, Sound::Short).feedback, QStringLiteral("general_warning"));
    }

    void everyKindHasARowAnIconAndTheSameCategory()
    {
        const NotificationCatalog cat(QStringLiteral("/usr/share/harbour-libre-monitor/icons/"));
        QCOMPARE(NotificationCatalog::all().size(), 14);
        QStringList names;
        for (int i = 0; i < NotificationCatalog::all().size(); ++i) {
            const NoteKind k = NotificationCatalog::all().at(i);
            names << NotificationCatalog::name(k);
            const NoteHints h = cat.hints(k, Sound::Own);
            QVERIFY(h.iconPath.startsWith(QStringLiteral("/usr/share/harbour-libre-monitor/icons/")));
            QVERIFY(h.iconPath.endsWith(QStringLiteral(".png")));
            QCOMPARE(h.category, QStringLiteral("x-libre-monitor.alert"));
            QCOMPARE(h.urgency == 0, !h.banner);        // page only means urgency 0 and the other way round
        }
        QCOMPARE(names.removeDuplicates(), 0);
        // No icon folder: no file is named, the platform shows the application's icon.
        QVERIFY(NotificationCatalog().hints(NoteKind::Low, Sound::Own).iconPath.isEmpty());
        // A folder without the trailing slash works the same.
        QCOMPARE(NotificationCatalog(QStringLiteral("/d")).hints(NoteKind::Low, Sound::Own).iconPath, QStringLiteral("/d/alert-l.png"));
    }
};

QTEST_APPLESS_MAIN(TstNotificationCatalog)
#include "tst_notificationcatalog.moc"
