// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Michele Brami
// Tests of the user-interface logic (app/ui): the direction table, the number formatting, the
// state inventory row by row, and the cover ranking. What they prove:
// the screens can never show a stale or unconfirmed value as current (REQ-UI-09, REQ-UI-05),
// and every state of the inventory has one answer.
#include "ui/coverrank.h"
#include "ui/direction.h"
#include "ui/glucoseformat.h"
#include "ui/subjectstate.h"
#include "ui/uiconstants.h"
#include "ui/sensorview.h"

#include <QtTest>

using namespace lm::ui;

namespace {

// A Subject whose service report is "all well" with a reading `ageS` seconds old.
SubjectInput fresh(Band band, qint64 ageS = 30)
{
    SubjectInput in;
    in.state = QStringLiteral("idle");
    in.hasReading = true;
    in.ageS = ageS;
    in.band = band;
    return in;
}

SubjectInput stale(const QString &cause, const QString &detail = QString())
{
    SubjectInput in;
    in.state = QStringLiteral("idle");
    in.stale = true;
    in.staleCause = cause;
    in.staleDetail = detail;
    in.hasReading = true;
    in.ageS = 4000;
    in.band = Band::N;          // the old reading was in range: it must still not be coloured
    return in;
}

// A Subject the service calls stale for a LATENESS cause, `sinceS` seconds ago, whose newest reading is
// `ageS` seconds old.
SubjectInput late(const QString &cause, qint64 sinceS, qint64 ageS, const QString &detail = QString())
{
    SubjectInput in;
    in.state = QStringLiteral("idle");
    in.stale = true;
    in.staleCause = cause;
    in.staleDetail = detail;
    in.staleSinceAgeS = sinceS;
    in.hasReading = true;
    in.ageS = ageS;
    in.band = Band::H;
    return in;
}

SubjectInput attention(const QString &detail)
{
    SubjectInput in;
    in.state = QStringLiteral("needs-attention");
    in.cause = detail;
    in.stale = true;
    in.staleCause = QStringLiteral("needs-attention");
    in.staleDetail = detail;
    in.hasReading = true;
    in.ageS = 4000;
    in.band = Band::N;
    return in;
}

}

class TstUiLogic : public QObject
{
    Q_OBJECT

private slots:
    // ---- the sensor block ----------------------------
    // The identifier is shown only as a short hash, never as stored; the warm-up end is activation plus
    // the warm-up and counts only while it lies ahead; an unknown part gives no warm-up line.
    void theSensorBlockShowsAShortHashAndTheWarmUpOnly()
    {
        SensorFacts f;
        f.known = true; f.id = QStringLiteral("SYNTHETIC0SERIAL"); f.activatedMs = 1000000; f.warmupMinutes = 60;
        f.firstSeenMs = 2000000; f.lastSeenMs = 3000000;
        QVariantMap m = sensorMap(f, 1000000 + 30 * 60000);
        QVERIFY(m.value("known").toBool());
        const QString sid = m.value("shortId").toString();
        QVERIFY2(QRegularExpression("^[0-9a-f]{4}-[0-9a-f]{4}$").match(sid).hasMatch(), qPrintable(sid));
        for (QVariantMap::const_iterator i = m.begin(); i != m.end(); ++i)
            QVERIFY(!i.value().toString().contains(QLatin1String("SYNTHETIC")));
        QCOMPARE(shortSensorId(f.id), sid);                                  // stable
        QVERIFY(shortSensorId(QStringLiteral("SYNTHETIC0SERIAL2")) != sid);  // tells sensors apart
        QCOMPARE(qint64(m.value("warmupEndMs").toDouble()), qint64(1000000 + 3600000));
        QVERIFY(m.value("warmingUp").toBool());
        QCOMPARE(qint64(m.value("firstSeenMs").toDouble()), qint64(2000000));
        QCOMPARE(qint64(m.value("lastSeenMs").toDouble()), qint64(3000000));
        m = sensorMap(f, 1000000 + 61 * 60000);
        QVERIFY(!m.value("warmingUp").toBool());                             // over: "Warm-up ended"
        f.warmupMinutes = -1;
        m = sensorMap(f, 1000000);
        QCOMPARE(m.value("warmupEndMs").toDouble(), -1.0);
        QVERIFY(!m.value("warmingUp").toBool());
        QVERIFY(!sensorMap(SensorFacts(), 0).value("known").toBool());
        QCOMPARE(sensorMap(SensorFacts(), 0).size(), 1);                     // nothing else for no sensor
        QVERIFY(shortSensorId(QString()).isEmpty());
    }
    // ---- direction (D-A) ------------------------------------------------------------
    void directionMapsTheOwnersTable()
    {
        QCOMPARE(directionOfCode(true, 1).kind, Direction::FallingFast);
        QCOMPARE(directionOfCode(true, 2).kind, Direction::Falling);
        QCOMPARE(directionOfCode(true, 3).kind, Direction::Steady);
        QCOMPARE(directionOfCode(true, 4).kind, Direction::Rising);
        QCOMPARE(directionOfCode(true, 5).kind, Direction::RisingFast);
        for (int c = 1; c <= 5; ++c)
            QVERIFY(directionOfCode(true, c).isArrow());
    }
    // codes 1 to 5 are the glyphs ↓ ↘ ≈ ↗ ↑, each one character followed by U+FE0E so a
    // font with an emoji form draws the plain text form.
    void theTrendGlyphsAreTheDecidedCharactersInTextPresentation()
    {
        const ushort glyph[] = { 0x2193, 0x2198, 0x2248, 0x2197, 0x2191 };
        for (int c = 1; c <= 5; ++c) {
            const QString s = directionOfCode(true, c).symbol();
            QCOMPARE(s.size(), 2);
            QCOMPARE(s.at(0).unicode(), glyph[c - 1]);
            QCOMPARE(s.at(1).unicode(), ushort(0xFE0E));
        }
    }
    void anUnknownCodeIsShownAsReceivedNotMapped()
    {
        const int odd[] = { 0, 6, 7, -1, 100 };
        for (unsigned i = 0; i < sizeof(odd) / sizeof(odd[0]); ++i) {
            const Direction d = directionOfCode(true, odd[i]);
            QCOMPARE(d.kind, Direction::UnknownCode);
            QVERIFY(!d.isArrow());
            QCOMPARE(d.symbol(), QString::number(odd[i]));
        }
    }
    void noCodeIsNull()
    {
        const Direction d = directionOfCode(false, 3);      // the value is ignored without a code
        QCOMPARE(d.kind, Direction::Null);
        QCOMPARE(d.symbol(), QStringLiteral("NULL"));
        QVERIFY(!d.isArrow());
    }
    void everyKindHasATextKey()
    {
        for (int c = 0; c <= 6; ++c)
            QVERIFY(directionOfCode(true, c).textKey().startsWith(QLatin1String("ui-dir-")));
        QVERIFY(directionOfCode(false, 0).textKey().startsWith(QLatin1String("ui-dir-")));
    }

    // ---- number formatting (REQ-DAT-12) --------------------------------------------------------
    void formatsInTheTargetUnitWithItsResolution()
    {
        const QLocale c(QLocale::C);
        QCOMPARE(formatGlucose(112, "mg/dL", "mg/dL", c), QStringLiteral("112"));
        QCOMPARE(formatGlucose(112, "mg/dL", "mmol/L", c), QStringLiteral("6.2"));
        QCOMPARE(formatGlucose(6.2, "mmol/L", "mg/dL", c), QStringLiteral("112"));
        QCOMPARE(formatGlucose(6.2, "mmol/L", "mmol/L", c), QStringLiteral("6.2"));
    }
    void followsTheLocaleSeparator()
    {
        QCOMPARE(formatGlucose(112, "mg/dL", "mmol/L", QLocale(QLocale::Italian, QLocale::Italy)),
                 QStringLiteral("6,2"));
        QCOMPARE(formatGlucose(1234, "mg/dL", "mg/dL", QLocale(QLocale::Italian, QLocale::Italy)),
                 QStringLiteral("1234"));      // no group separator in a glucose number
    }
    void anUnknownUnitGivesNoText()
    {
        QVERIFY(formatGlucose(5, "mg/L", "mmol/L", QLocale(QLocale::C)).isEmpty());
        QVERIFY(formatGlucose(5, "mmol/L", "stones", QLocale(QLocale::C)).isEmpty());
    }

    // ---- the state inventory, row by row ------------------------------
    void freshReadingsGiveTheirBand()
    {
        const struct { Band b; StateId id; const char *name; } rows[] = {
            { Band::LL, StateId::BandLL, "B-LL" }, { Band::L, StateId::BandL, "B-L" },
            { Band::N, StateId::BandN, "B-N" },    { Band::H, StateId::BandH, "B-H" },
            { Band::HH, StateId::BandHH, "B-HH" }, { Band::Unknown, StateId::BandUnknown, "B-U" } };
        for (unsigned i = 0; i < sizeof(rows) / sizeof(rows[0]); ++i) {
            const SubjectView v = resolveSubject(fresh(rows[i].b));
            QCOMPARE(v.id, rows[i].id);
            QCOMPARE(stateIdName(v.id), QString::fromLatin1(rows[i].name));
            QVERIFY(isFresh(v.id));
            QCOMPARE(v.band, rows[i].b);
        }
    }
    void noReadingYetIsNew()
    {
        SubjectInput in;
        in.state = QStringLiteral("idle");
        QCOMPARE(resolveSubject(in).id, StateId::NoDataYet);
    }
    void theUiAgeCheckOverridesAServiceThatSaysFresh()
    {
        // A dead service leaves "not stale" frozen: the reading must still not look current.
        SubjectInput in = fresh(Band::LL, kUiFreshMaxS + 1);
        const SubjectView v = resolveSubject(in);
        QCOMPARE(v.id, StateId::NoCurrentReading);
        QVERIFY(!isFresh(v.id));
        QCOMPARE(v.band, Band::Unknown);                            // no glucose colour
        QCOMPARE(resolveSubject(fresh(Band::LL, kUiFreshMaxS)).id, StateId::BandLL);   // the limit is inclusive
    }
    void aReadingFromTheFutureIsNeverCurrent()
    {
        const SubjectView v = resolveSubject(fresh(Band::N, -5));
        QVERIFY(!isFresh(v.id));
    }
    void staleCausesMapToTheirRows()
    {
        QCOMPARE(resolveSubject(stale("source-unreachable")).id, StateId::Unreachable);            // S
        QCOMPARE(resolveSubject(stale("no-new-readings")).id, StateId::NoNewReadings);             // S
        QCOMPARE(resolveSubject(stale("sensor-warm-up")).id, StateId::WarmUp);                     // S
        QCOMPARE(resolveSubject(stale("disabled-by-guard")).id, StateId::StoppedByGuard);          // S
        QCOMPARE(resolveSubject(stale("service-not-running")).id, StateId::ServiceGap);            // S
        QCOMPARE(resolveSubject(stale("credentials-locked")).id, StateId::CredentialsLocked);      // S
        QCOMPARE(resolveSubject(stale("something-new")).id, StateId::NoCurrentReading);            // ST-X
    }
    void staleNeverCarriesAGlucoseColour()
    {
        const char *causes[] = { "source-unreachable", "no-new-readings", "sensor-warm-up",
                                 "disabled-by-guard", "service-not-running", "credentials-locked",
                                 "needs-attention", "unknown-cause" };
        for (unsigned i = 0; i < sizeof(causes) / sizeof(causes[0]); ++i) {
            const SubjectView v = resolveSubject(stale(QString::fromLatin1(causes[i])));
            QCOMPARE(v.band, Band::Unknown);
            QVERIFY(!isFresh(v.id));
        }
    }
    // ---- the display grace for a late reading -----------------------------------------------------
    void aLateReadingKeepsItsBandJustUnderTheGrace()
    {
        const char *causes[] = { "source-unreachable", "no-new-readings" };
        for (unsigned i = 0; i < sizeof(causes) / sizeof(causes[0]); ++i) {
            SubjectView v = resolveSubject(late(causes[i], kUiStaleGraceS - 1, 290));
            QCOMPARE(v.id, StateId::BandH);                         // the band and its colour stay
            QCOMPARE(v.band, Band::H);
            QVERIFY(isFresh(v.id));
        }
        // Rate-limited is a detail of "cannot be reached": the same grace.
        QCOMPARE(resolveSubject(late("source-unreachable", 10, 200, "rate-limited")).id, StateId::BandH);
    }
    void aLateReadingShowsItsCauseJustOverTheGrace()
    {
        SubjectView v = resolveSubject(late("source-unreachable", kUiStaleGraceS, 310));
        QCOMPARE(v.id, StateId::Unreachable);
        QCOMPARE(v.band, Band::Unknown);                            // no glucose colour from here on
        QVERIFY(!isFresh(v.id));
        v = resolveSubject(late("no-new-readings", kUiStaleGraceS + 1, 310));
        QCOMPARE(v.id, StateId::NoNewReadings);
        v = resolveSubject(late("source-unreachable", kUiStaleGraceS + 30, 400, "rate-limited"));
        QCOMPARE(v.id, StateId::Unreachable);
        QVERIFY(v.rateLimited);
    }
    void theGraceCoversTheWholeStretchBetweenTheServicesLimitAndTheCause()
    {
        // The service flags a reading late at 180 s; the cause shows 120 s later, at about 300 s of age.
        QCOMPARE(resolveSubject(late("no-new-readings", 0, 180)).id, StateId::BandH);
        QCOMPARE(resolveSubject(late("no-new-readings", 60, 240)).id, StateId::BandH);
        QCOMPARE(resolveSubject(late("no-new-readings", 119, 299)).id, StateId::BandH);
        // The safety net of the UI's own age check is widened by the grace, never beyond it.
        QCOMPARE(resolveSubject(late("no-new-readings", 119, kUiFreshMaxS + kUiStaleGraceS)).id, StateId::BandH);
        QVERIFY(!isFresh(resolveSubject(late("no-new-readings", 119, kUiFreshMaxS + kUiStaleGraceS + 1)).id));
    }
    void aTimeSinceTheFlagThatIsUnknownGetsNoGrace()
    {
        QCOMPARE(resolveSubject(late("no-new-readings", -1, 200)).id, StateId::NoNewReadings);
    }
    void everyKnownStateShowsAtOnce()
    {
        // Not lateness: the grace never applies, whatever the time since the flag.
        SubjectInput in = late("sensor-warm-up", 5, 200);
        QCOMPARE(resolveSubject(in).id, StateId::WarmUp);
        in = late("service-not-running", 5, 200);
        QCOMPARE(resolveSubject(in).id, StateId::ServiceGap);
        in = late("credentials-locked", 5, 200);
        QCOMPARE(resolveSubject(in).id, StateId::CredentialsLocked);
        in = late("disabled-by-guard", 5, 200);
        QCOMPARE(resolveSubject(in).id, StateId::StoppedByGuard);
        in = late("disabled-by-guard", 5, 200);
        in.state = QStringLiteral("disabled-by-guard");
        QCOMPARE(resolveSubject(in).id, StateId::StoppedByGuard);
        QCOMPARE(resolveSubject(attention("terms")).id, StateId::TermsMustBeAccepted);
        SubjectInput none; none.state = QStringLiteral("idle");
        QCOMPARE(resolveSubject(none).id, StateId::NoDataYet);
        in = late("something-new", 5, 200);
        QCOMPARE(resolveSubject(in).id, StateId::NoCurrentReading);
    }
    void theGraceDoesNotHideAGuardKillButton()
    {
        SubjectInput kill = late("source-unreachable", 10, 200, "guard-kill");
        kill.state = QStringLiteral("backoff");
        kill.cause = QStringLiteral("guard-kill");
        const SubjectView v = resolveSubject(kill);
        QCOMPARE(v.id, StateId::BandH);                             // the band still shows
        QCOMPARE(v.guard, GuardStrip::Interrupted);                 // and so does the Retry strip (REQ-GRD-14)
        QVERIFY(v.showRetry);
    }
    void rateLimitedIsTheUnreachableRowWithItsOwnText()
    {
        const SubjectView v = resolveSubject(stale("source-unreachable", "rate-limited"));
        QCOMPARE(v.id, StateId::Unreachable);
        QVERIFY(v.rateLimited);
        QVERIFY(!resolveSubject(stale("source-unreachable", "transient")).rateLimited);
    }
    void retryIsOfferedAfterAWhileOfBeingUnreachable()
    {
        SubjectInput in = stale("source-unreachable");
        in.staleSinceAgeS = kUiRetryOfferS - 1;
        QVERIFY(!resolveSubject(in).showRetry);
        in.staleSinceAgeS = kUiRetryOfferS;
        QVERIFY(resolveSubject(in).showRetry);
    }
    void attentionStatesMapToTheirRowsAndButtons()
    {
        SubjectView v = resolveSubject(attention("auth-failed"));
        QCOMPARE(v.id, StateId::LoginRefused);
        QVERIFY(v.showEditAccount);
        v = resolveSubject(attention("terms"));
        QCOMPARE(v.id, StateId::TermsMustBeAccepted);
        QVERIFY(v.showRetry);
        v = resolveSubject(attention("protocol-changed"));
        QCOMPARE(v.id, StateId::SourceChanged);
        v = resolveSubject(attention("credentials-missing"));
        QCOMPARE(v.id, StateId::CredentialsMissing);
        QVERIFY(v.showSetUpAccount);
        v = resolveSubject(attention("subject-unavailable"));
        QCOMPARE(v.id, StateId::SubjectUnavailable);
        QVERIFY(v.showEditAccount);
        v = resolveSubject(attention("internal"));
        QCOMPARE(v.id, StateId::InternalProblem);
        QVERIFY(v.showRetry);
        v = resolveSubject(attention("adapter-rejected"));
        QCOMPARE(v.id, StateId::InternalProblem);
        v = resolveSubject(attention("a-cause-from-the-future"));
        QCOMPARE(v.id, StateId::InternalProblem);       // an unknown detail is never guessed
        v = resolveSubject(attention("credentials-locked"));
        QCOMPARE(v.id, StateId::CredentialsLocked);
        QVERIFY(v.showAskAgain);
    }
    void attentionWinsEvenBeforeTheStaleLimit()
    {
        // The state changed a moment ago: the service has not flagged staleness yet, but a value
        // from a Subject that needs the user must not look healthy.
        SubjectInput in = fresh(Band::N);
        in.state = QStringLiteral("needs-attention");
        in.cause = QStringLiteral("auth-failed");
        QCOMPARE(resolveSubject(in).id, StateId::LoginRefused);
    }
    void guardStripsAndTheirButtons()
    {
        SubjectInput in = stale("disabled-by-guard");
        in.state = QStringLiteral("disabled-by-guard");
        in.cause = QStringLiteral("connection");
        SubjectView v = resolveSubject(in);
        QCOMPARE(v.id, StateId::StoppedByGuard);
        QCOMPARE(v.guard, GuardStrip::ConnectionStopped);
        QVERIFY(v.showReenable);

        in.cause = QStringLiteral("adapter-version");
        v = resolveSubject(in);
        QCOMPARE(v.guard, GuardStrip::VersionStopped);
        QVERIFY(v.showReenable);

        SubjectInput kill = stale("source-unreachable", "guard-kill");
        kill.state = QStringLiteral("backoff");
        kill.cause = QStringLiteral("guard-kill");
        v = resolveSubject(kill);
        QCOMPARE(v.guard, GuardStrip::Interrupted);
        QVERIFY(v.showRetry);
        QVERIFY(!v.showReenable);
    }
    void otherBackoffCausesAreNotAGuardStrip()
    {
        SubjectInput in = stale("source-unreachable", "transient");
        in.state = QStringLiteral("backoff");
        in.cause = QStringLiteral("transient");
        QCOMPARE(resolveSubject(in).guard, GuardStrip::None);
    }
    void everyIdHasAUniqueName()
    {
        QSet<QString> names;
        for (int i = int(StateId::BandLL); i <= int(StateId::NoCurrentReading); ++i) {
            const QString n = stateIdName(StateId(i));
            QVERIFY(!n.isEmpty());
            QVERIFY(!names.contains(n));
            names.insert(n);
        }
    }

    // ---- cover ranking (D-D) ---------------------------------------------------------------------
    void coverRanksHypoThenNotMonitoredThenHyperThenFine()
    {
        // The author's order, 2026-10-16: hypoglycaemia first, then disconnection, then hyperglycaemia.
        QCOMPARE(coverRank(StateId::BandLL), 1);
        QCOMPARE(coverRank(StateId::BandL), 1);
        QCOMPARE(coverRank(StateId::Unreachable), 2);
        QCOMPARE(coverRank(StateId::StoppedByGuard), 2);
        QCOMPARE(coverRank(StateId::LoginRefused), 2);
        QCOMPARE(coverRank(StateId::NoCurrentReading), 2);
        QCOMPARE(coverRank(StateId::BandH), 3);
        QCOMPARE(coverRank(StateId::BandHH), 3);
        QCOMPARE(coverRank(StateId::BandN), 4);
    }
    void coverPicksTheMostUrgentSubject()
    {
        QList<CoverCandidate> s;
        CoverCandidate a; a.id = StateId::BandN;            s << a;
        CoverCandidate b; b.id = StateId::BandL; b.valueMgdl = 65;  s << b;
        CoverCandidate c; c.id = StateId::Unreachable; c.staleSinceAgeS = 600;  s << c;
        CoverCandidate d; d.id = StateId::BandHH; d.valueMgdl = 280; s << d;
        QCOMPARE(pickCoverSubject(s), 1);                   // the low first
        s.removeAt(1);
        QCOMPARE(pickCoverSubject(s), 1);                   // then the Subject nobody watches
        s.removeAt(1);
        QCOMPARE(pickCoverSubject(s), 1);                   // then the high
    }
    void coverShowsTheMostAttentionableWhenThereAreMany()
    {
        QList<CoverCandidate> s;
        for (int i = 0; i < 5; ++i) {
            CoverCandidate c; c.id = StateId::BandN; c.limitDistanceMgdl = 100 - i * 10;  // 100, 90, 80, 70, 60
            s << c;
        }
        s[1].id = StateId::BandHH; s[1].valueMgdl = 270;            // a high
        s[3].id = StateId::BandLL; s[3].valueMgdl = 50;             // a low
        s[0].id = StateId::BandL; s[0].valueMgdl = 66;              // a less urgent low
        const QList<int> top = orderForCover(s, 2);
        QCOMPARE(top.size(), 2);
        QCOMPARE(top[0], 3);                                        // the lowest low
        QCOMPARE(top[1], 0);                                        // the other low, before any high
        const QList<int> all = orderForCover(s, 10);
        QCOMPARE(all.size(), 5);
        QCOMPARE(all[2], 1);                                        // the high comes after the lows
        QCOMPARE(all[3], 4);                                        // then in range, nearer a limit first
        QCOMPARE(all[4], 2);
        QCOMPARE(orderForCover(QList<CoverCandidate>(), 2).size(), 0);
    }
    void coverOrderPutsUnmonitoredBetweenLowAndHigh()
    {
        QList<CoverCandidate> s;
        CoverCandidate a; a.id = StateId::BandH; a.valueMgdl = 200;  s << a;
        CoverCandidate b; b.id = StateId::Unreachable; b.staleSinceAgeS = 100; s << b;
        CoverCandidate c; c.id = StateId::LoginRefused; c.staleSinceAgeS = 900; s << c;
        CoverCandidate d; d.id = StateId::BandL; d.valueMgdl = 68;  s << d;
        const QList<int> o = orderForCover(s, 4);
        QCOMPARE(o[0], 3);                                          // the low
        QCOMPARE(o[1], 2);                                          // not monitored the longest
        QCOMPARE(o[2], 1);
        QCOMPARE(o[3], 0);                                          // the high last of the four
    }
    void coverTiesGoToTheLongerProblemThenTheOrder()
    {
        QList<CoverCandidate> s;
        CoverCandidate a; a.id = StateId::Unreachable; a.staleSinceAgeS = 100;  s << a;
        CoverCandidate b; b.id = StateId::NoNewReadings; b.staleSinceAgeS = 900;  s << b;
        CoverCandidate c; c.id = StateId::WarmUp; c.staleSinceAgeS = 900;  s << c;
        QCOMPARE(pickCoverSubject(s), 1);                   // longer problem; equal: earlier index
        QCOMPARE(pickCoverSubject(QList<CoverCandidate>()), -1);
    }
};


QTEST_APPLESS_MAIN(TstUiLogic)
#include "tst_uilogic.moc"
