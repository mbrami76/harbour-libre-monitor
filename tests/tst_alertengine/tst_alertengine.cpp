// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Michele Brami
//
// The alert engine against the table Each row is a
// small script of readings, ticks and user actions (the language is described in
// tests/support/alerttest.h) and the exact list of effects the engine must answer. The clock is
// manual; no row lets real time pass. Every value is synthetic.
//
// Defaults of the rig unless a row says otherwise: thresholds 54/70/180/250 mg/dL, persistence
// none for L and 15 min for H, missing-data limit 15 min, display staleness limit 3 min, a Subject
// registered at second 0. A reading arrives at the second it is measured.
#include "alerttest.h"

#include <QtTest>

using namespace lm;
using namespace lm::testing;

namespace {

QStringList L(const char *a = 0, const char *b = 0, const char *c = 0, const char *d = 0,
              const char *e = 0, const char *f = 0, const char *g = 0, const char *h = 0,
              const char *i = 0, const char *j = 0)
{
    QStringList l;
    const char *all[] = { a, b, c, d, e, f, g, h, i, j };
    for (int k = 0; k < 10; ++k)
        if (all[k]) l << QString::fromLatin1(all[k]);
    return l;
}

} // namespace

Q_DECLARE_METATYPE(QStringList)

class TstAlertEngine : public QObject
{
    Q_OBJECT

private slots:
    void initTestCase() { qRegisterMetaType<QStringList>(); }

    // ---- glucose alerts: crossing, persistence, levels ------------------------------------------
    void scripts_data()
    {
        QTest::addColumn<QString>("script");
        QTest::addColumn<QStringList>("expect");

        QTest::newRow("L: a crossing without persistence is active at once") <<
            "rr 0 120 60 100\n"
            "rr 180 300 60 69\n" <<
            L("J 180 low L active active",
              "N 180 low L");

        QTest::newRow("H: a crossing with persistence is pending, then active after 15 min") <<
            "rr 0 60 60 100\n"
            "rr 120 1200 60 181\n" <<
            L("J 120 high H pending pending",
              "J 1020 high H active active",
              "N 1020 high H");

        QTest::newRow("H: back within range before the persistence ends reverts") <<
            "rr 0 60 60 100\n"
            "rr 120 240 60 181\n"
            "rr 300 600 60 175\n" <<
            L("J 120 high H pending pending",
              "J 300 high H normal reverted");

        QTest::newRow("H pending: HH is active at once, persistence does not apply to it") <<
            "rr 0 60 60 100\n"
            "r 120 181\n"
            "rr 180 300 60 251\n" <<
            L("J 120 high H pending pending",
              "J 180 high HH active active",
              "N 180 high HH",
              "R 300 high HH #1");       // the HH reminder is 2 minutes

        QTest::newRow("L to LL: escalation is active again at once") <<
            "rr 0 60 60 100\n"
            "r 120 69\n"
            "r 180 53\n"
            "r 240 53\n" <<
            L("J 120 low L active active",
              "N 120 low L",
              "J 180 low LL active escalated",
              "N 180 low LL");

        QTest::newRow("LL: a reminder every 2 minutes") <<
            "rr 0 60 60 100\n"
            "rr 120 700 60 53\n" <<
            L("J 120 low LL active active",
              "N 120 low LL",
              "R 240 low LL #1",
              "R 360 low LL #2",
              "R 480 low LL #3",
              "R 600 low LL #4");

        QTest::newRow("L: a reminder every 15 minutes") <<
            "cfg 1 0 0 15\n"
            "rr 0 60 60 100\n"
            "rr 120 3000 60 69\n" <<
            L("J 120 low L active active",
              "N 120 low L",
              "R 1020 low L #1",
              "R 1920 low L #2",
              "R 2820 low L #3");

        QTest::newRow("H: a reminder every 30 minutes") <<
            "cfg 1 0 0 15\n"
            "rr 0 60 60 100\n"
            "rr 120 4000 60 181\n" <<
            L("J 120 high H active active",
              "N 120 high H",
              "R 1920 high H #1",
              "R 3720 high H #2");

        QTest::newRow("HH: a reminder every 2 minutes, like LL") <<
            "rr 0 60 60 100\n"
            "rr 120 700 60 251\n" <<
            L("J 120 high HH active active",
              "N 120 high HH",
              "R 240 high HH #1",
              "R 360 high HH #2",
              "R 480 high HH #3",
              "R 600 high HH #4");

        // ---- acknowledgement, snooze ------------------------------------------------------------
        QTest::newRow("acknowledging L stops the reminders; it stays acknowledged") <<
            "rr 0 60 60 100\n"
            "rr 120 180 60 69\n"
            "ack 200 low\n"
            "rr 240 2400 60 69\n" <<
            L("J 120 low L active active",
              "N 120 low L",
              "J 200 low L acknowledged acknowledged",
              "U 200 low L acknowledged");

        QTest::newRow("acknowledging LL: active again after 5 minutes while it holds") <<
            "rr 0 60 60 100\n"
            "rr 120 180 60 50\n"
            "ack 200 low\n"
            "rr 240 700 60 50\n" <<
            L("J 120 low LL active active",
              "N 120 low LL",
              "J 200 low LL acknowledged acknowledged",
              "U 200 low LL acknowledged",
              "J 500 low LL active re-alerted",
              "N 500 low LL",
              "R 620 low LL #1");

        QTest::newRow("acknowledging LL: not yet active again one second before the 5 minutes") <<
            "rr 0 60 60 100\n"
            "rr 120 180 60 50\n"
            "ack 200 low\n"
            "rr 240 480 60 50\n"
            "t 499\n" <<
            L("J 120 low LL active active",
              "N 120 low LL",
              "J 200 low LL acknowledged acknowledged",
              "U 200 low LL acknowledged");

        QTest::newRow("acknowledging HH: active again after 15 minutes while it holds (HH is not shortened)") <<
            "rr 0 60 60 100\n"
            "rr 120 180 60 251\n"
            "ack 200 high\n"
            "rr 240 1200 60 251\n" <<
            L("J 120 high HH active active",
              "N 120 high HH",
              "J 200 high HH acknowledged acknowledged",
              "U 200 high HH acknowledged",
              "J 1100 high HH active re-alerted",
              "N 1100 high HH");

        QTest::newRow("snoozing L for 15 minutes; it comes back when the snooze ends") <<
            "rr 0 60 60 100\n"
            "rr 120 180 60 69\n"
            "snz 200 low 15\n"
            "rr 240 1500 60 69\n" <<
            L("J 120 low L active active",
              "N 120 low L",
              "J 200 low L snoozed snoozed",
              "W 200 low L",
              "J 1100 low L active snooze-ended",
              "N 1100 low L");

        QTest::newRow("a snooze is cancelled at once by a more severe level") <<
            "rr 0 60 60 100\n"
            "r 120 69\n"
            "snz 150 low 60\n"
            "r 180 53\n"
            "r 240 53\n" <<
            L("J 120 low L active active",
              "N 120 low L",
              "J 150 low L snoozed snoozed",
              "W 150 low L",
              "J 180 low LL active escalated",
              "N 180 low LL");

        QTest::newRow("an acknowledged H rises to HH: active again at once") <<
            "cfg 1 0 0 15\n"
            "rr 0 60 60 100\n"
            "r 120 181\n"
            "ack 130 high\n"
            "r 180 251\n" <<
            L("J 120 high H active active",
              "N 120 high H",
              "J 130 high H acknowledged acknowledged",
              "U 130 high H acknowledged",
              "J 180 high HH active escalated",
              "N 180 high HH");

        QTest::newRow("refused actions say why and change nothing") <<
            "rr 0 60 60 100\n"
            "r 120 69\n"
            "snz 130 low 20\n"
            "ack 140 high\n"
            "ack 150 low\n"
            "ack 160 low\n" <<
            L("J 120 low L active active",
              "N 120 low L",
              "A 130 snz low bad-duration",
              "A 140 ack high not-active",
              "J 150 low L acknowledged acknowledged",
              "U 150 low L acknowledged",
              "A 160 ack low already-acknowledged");

        QTest::newRow("LL has no snooze") <<
            "rr 0 60 60 100\n"
            "r 120 50\n"
            "snz 130 low 15\n" <<
            L("J 120 low LL active active",
              "N 120 low LL",
              "A 130 snz low not-snoozable");

        QTest::newRow("HH has no snooze") <<
            "rr 0 60 60 100\n"
            "r 120 260\n"
            "snz 130 high 120\n" <<
            L("J 120 high HH active active",
              "N 120 high HH",
              "A 130 snz high not-snoozable");

        // ---- hysteresis and the confirmation count ----------------------------------------------
        QTest::newRow("hysteresis: back inside by 5 mg/dL for 2 readings; a dip in between resets") <<
            "rr 0 60 60 100\n"
            "r 120 69\n"
            "r 180 72\n"       // inside the threshold but not by 5: still holds
            "r 240 75\n"       // 75 = 70 + 5: first confirming reading
            "r 300 72\n"       // back in the margin: the count starts again
            "r 360 76\n"
            "r 420 77\n" <<    // second one in a row
            L("J 120 low L active active",
              "N 120 low L",
              "J 420 low L cleared cleared reason=recovered",
              "W 420 low L");

        QTest::newRow("hysteresis on the high side") <<
            "cfg 1 0 0 15\n"
            "rr 0 60 60 100\n"
            "r 120 181\n"
            "r 180 176\n"      // 176 > 175: still holds
            "r 240 175\n"      // 175 = 180 - 5: first
            "r 300 174\n" <<   // second
            L("J 120 high H active active",
              "N 120 high H",
              "J 300 high H cleared cleared reason=recovered",
              "W 300 high H");

        QTest::newRow("LL falls to L after 2 readings out of its margin but still below L") <<
            "rr 0 60 60 100\n"
            "r 120 53\n"
            "r 180 60\n"       // 60 is not below 54 + 5 = 59: LL no longer holds, L does
            "r 240 62\n" <<
            L("J 120 low LL active active",
              "N 120 low LL",
              "R 240 low LL #1",
              "J 240 low L active deescalated",
              "U 240 low L active");

        QTest::newRow("an acknowledged LL that falls to L stays acknowledged") <<
            "rr 0 60 60 100\n"
            "r 120 53\n"
            "ack 130 low\n"
            "r 180 62\n"
            "r 240 63\n" <<
            L("J 120 low LL active active",
              "N 120 low LL",
              "J 130 low LL acknowledged acknowledged",
              "U 130 low LL acknowledged",
              "J 240 low L acknowledged deescalated",
              "U 240 low L acknowledged");

        // ---- a threshold change under a pending or an active alert ------------------------------
        QTest::newRow("raising H during a pending H alert reverts it") <<
            "rr 0 60 60 100\n"
            "rr 120 180 60 181\n"
            "th 200 70 190\n" <<
            L("J 120 high H pending pending",
              "J 200 high H normal reverted");

        QTest::newRow("tightening H under a normal reading starts the persistence at the change") <<
            "rr 0 180 60 175\n"
            "th 200 70 170\n"
            "rr 240 1200 60 175\n" <<
            L("J 200 high H pending pending",
              "J 1100 high H active active",
              "N 1100 high H");

        QTest::newRow("raising H during an active alert: two readings inside it clear the alert") <<
            "cfg 1 0 0 15\n"
            "rr 0 60 60 100\n"
            "rr 120 240 60 190\n"
            "th 300 70 200\n"
            "r 360 190\n" <<
            L("J 120 high H active active",
              "N 120 high H",
              "J 360 high H cleared cleared reason=recovered",
              "W 360 high H");

        QTest::newRow("lowering L to a value above the reading raises an alert at the change") <<
            "rr 0 120 60 72\n"
            "th 150 75 180\n" <<
            L("J 150 low L active active",
              "N 150 low L");

        // ---- LL and HH are never delayed by any setting ---------------------------------------
        QTest::newRow("LL at once even with every delaying setting at its maximum") <<
            "cfg 1 15 60 60\n"
            "rr 0 60 60 100\n"
            "r 120 53\n" <<
            L("J 120 low LL active active",
              "N 120 low LL");

        QTest::newRow("HH at once even with every delaying setting at its maximum") <<
            "cfg 1 15 60 60\n"
            "rr 0 60 60 100\n"
            "r 120 251\n" <<
            L("J 120 high HH active active",
              "N 120 high HH");

        QTest::newRow("a long persistence of L does not hold back LL when the level falls") <<
            "cfg 1 15 60 15\n"
            "rr 0 60 60 100\n"
            "r 120 69\n"
            "r 180 53\n" <<
            L("J 120 low L pending pending",
              "J 180 low LL active active",
              "N 180 low LL");

        QTest::newRow("absurd persistence times are held to their limits (L 15 min)") <<
            "cfg 1 9999 0 15\n"
            "rr 0 60 60 100\n"
            "rr 120 1100 60 69\n" <<
            L("J 120 low L pending pending",
              "J 1020 low L active active",
              "N 1020 low L");

        QTest::newRow("absurd persistence of H is held to 60 minutes (the module's own bound)") <<
            "cfg 1 0 9999 15\n"
            "rr 0 60 60 100\n"
            "rr 120 3780 60 181\n" <<
            L("J 120 high H pending pending",
              "J 3720 high H active active",
              "N 3720 high H");

        QTest::newRow("a configured L persistence of 5 minutes") <<
            "cfg 1 5 15 15\n"
            "rr 0 60 60 100\n"
            "rr 120 600 60 69\n" <<
            L("J 120 low L pending pending",
              "J 420 low L active active",
              "N 420 low L");

        // ---- readings in mmol/L, ignored readings -----------------------------------------------
        QTest::newRow("a reading stored in mmol/L is converted exactly: 3.0 is L, 2.9 is LL") <<
            "rr 0 60 60 100\n"
            "rm 120 3.0\n"
            "rm 180 2.9\n" <<
            L("J 120 low L active active",
              "N 120 low L",
              "J 180 low LL active escalated",
              "N 180 low LL");

        QTest::newRow("a reading older than the newest one is ignored") <<
            "rr 0 60 60 100\n"
            "r 180 100\n"
            "r 150 50\n" <<
            L();

        // ---- silent mode -----------------------------------------------
        QTest::newRow("silent: set while LL sounds gives one silent Update; reminders are silent; a timed mode ends by itself") <<
            "rr 0 60 60 100\n"
            "r 120 50\n"
            "silent 150 10\n"                      // 10 minutes: until 750
            "rr 180 900 60 50\n" <<
            L("J 120 low LL active active",
              "N 120 low LL",
              "U 150 low LL active silent",
              "R 240 low LL #1 silent",
              "R 360 low LL #2 silent",
              "R 480 low LL #3 silent",
              "R 600 low LL #4 silent",
              "R 720 low LL #5 silent",
              "R 840 low LL #6");                   // the mode ended at 750: nothing was emitted then, this one sounds

        QTest::newRow("silent: a mode that starts when nothing is active emits nothing") <<
            "rr 0 120 60 100\n"
            "silent 130 0\n" << L();

        QTest::newRow("silent: a level that rises under the mode stays silent, LL is active at once") <<
            "silent 1 0\n"
            "rr 0 60 60 100\n"
            "r 120 69\n"
            "r 180 53\n"
            "r 240 53\n" <<
            L("J 120 low L active active",
              "N 120 low L silent",
              "J 180 low LL active escalated",
              "N 180 low LL silent");

        QTest::newRow("silent: an acknowledged slot is unaffected at the start") <<
            "rr 0 60 60 100\n"
            "r 120 69\n"
            "ack 130 low\n"
            "silent 140 0\n"
            "rr 180 700 60 69\n" <<
            L("J 120 low L active active",
              "N 120 low L",
              "J 130 low L acknowledged acknowledged",
              "U 130 low L acknowledged");

        QTest::newRow("silent: a snoozed slot is unaffected at the start; the end of the snooze is silent") <<
            "cfg 1 0 0 15\n"
            "rr 0 60 60 100\n"
            "r 120 190\n"
            "snz 130 high 15\n"
            "silent 140 0\n"
            "rr 180 1100 60 190\n" <<
            L("J 120 high H active active",
              "N 120 high H",
              "J 130 high H snoozed snoozed",
              "W 130 high H",
              "J 1030 high H active snooze-ended",
              "N 1030 high H silent");

        QTest::newRow("silent: missing data is never silent, neither its alert nor its reminders") <<
            "silent 1 0\n"
            "r 0 100\n"
            "t 3000\n" <<
            L("J 180.001 missing MISSING pending pending",
              "J 900.001 missing MISSING active active",
              "N 900.001 missing MISSING",
              "R 2700.001 missing MISSING #1");

        QTest::newRow("silent: a mode set while missing data is active does not touch it") <<
            "r 0 100\n"
            "t 1000\n"
            "silent 1100 0\n" <<
            L("J 180.001 missing MISSING pending pending",
              "J 900.001 missing MISSING active active",
              "N 900.001 missing MISSING");

        QTest::newRow("silent: LL and HH are still active at once and still cannot be snoozed") <<
            "silent 1 0\n"
            "cfg 2 15 60 60\n"
            "rr 0 60 60 100\n"
            "r 120 50\n"
            "snz 130 low 15\n"
            "r 180 260\n"
            "snz 190 high 15\n" <<
            L("J 120 low LL active active",
              "N 120 low LL silent",
              "A 130 snz low not-snoozable",
              "J 180 high HH active active",
              "N 180 high HH silent",
              "A 190 snz high not-snoozable");

        QTest::newRow("silent: ended by the user, the next reminder sounds and nothing is emitted at the end") <<
            "rr 0 60 60 100\n"
            "r 120 50\n"
            "silent 130 0\n"
            "rr 180 300 60 50\n"
            "unsilent 300\n"
            "rr 360 480 60 50\n" <<
            L("J 120 low LL active active",
              "N 120 low LL",
              "U 130 low LL active silent",
              "R 240 low LL #1 silent",
              "R 360 low LL #2",
              "R 480 low LL #3");

        // ---- the interruption notice -----------------------------------------------------------
        QTest::newRow("a gap of 20 minutes is announced") <<
            "rr 0 5000 60 100\ngap 5000 3000 4200\n" << L("I 3000 4200");
        QTest::newRow("a gap of 10 minutes is not") <<
            "rr 0 5000 60 100\ngap 5000 3000 3600\n" << L();
        QTest::newRow("a gap of exactly the limit is not (it has to be longer)") <<
            "rr 0 5000 60 100\ngap 5000 3000 3900\n" << L();

        // ---- missing data ----------------------------------------------------------------------
        QTest::newRow("missing: pending after the staleness limit, active after the limit, cleared by a reading") <<
            "r 0 100\n"
            "st 100 source-unreachable -\n"
            "t 200\n"
            "t 1000\n"
            "r 1100 100\n" <<
            L("J 180.001 missing MISSING pending pending reason=source-unreachable",
              "J 900.001 missing MISSING active active reason=source-unreachable",
              "N 900.001 missing MISSING cause=source-unreachable",
              "J 1100 missing MISSING cleared cleared reason=reading",
              "W 1100 missing MISSING");

        QTest::newRow("missing: a Subject that never had a reading counts from the start of monitoring") <<
            "st 10 no-new-readings -\n"
            "t 1000\n" <<
            L("J 180.001 missing MISSING pending pending reason=no-new-readings",
              "J 900.001 missing MISSING active active reason=no-new-readings",
              "N 900.001 missing MISSING cause=no-new-readings");

        QTest::newRow("missing: a reminder every 30 minutes") <<
            "r 0 100\n"
            "t 3000\n" <<
            L("J 180.001 missing MISSING pending pending",
              "J 900.001 missing MISSING active active",
              "N 900.001 missing MISSING",
              "R 2700.001 missing MISSING #1");

        QTest::newRow("missing: the limit is per Subject (5 minutes)") <<
            "cfg 1 0 15 5\n"
            "r 0 100\n"
            "t 1000\n" <<
            L("J 180.001 missing MISSING pending pending",
              "J 300.001 missing MISSING active active",
              "N 300.001 missing MISSING");

        QTest::newRow("missing: a limit below the minimum is held to 5 minutes") <<
            "cfg 1 0 15 1\n"
            "r 0 100\n"
            "t 1000\n" <<
            L("J 180.001 missing MISSING pending pending",
              "J 300.001 missing MISSING active active",
              "N 300.001 missing MISSING");

        QTest::newRow("missing: a limit above the maximum is held to 60 minutes") <<
            "cfg 1 0 15 120\n"
            "r 0 100\n"
            "t 4000\n" <<
            L("J 180.001 missing MISSING pending pending",
              "J 3600.001 missing MISSING active active",
              "N 3600.001 missing MISSING");

        QTest::newRow("missing: the message follows a change of the cause, quietly") <<
            "r 0 100\n"
            "st 10 source-unreachable -\n"
            "t 1000\n"
            "st 1200 credentials-locked -\n" <<
            L("J 180.001 missing MISSING pending pending reason=source-unreachable",
              "J 900.001 missing MISSING active active reason=source-unreachable",
              "N 900.001 missing MISSING cause=source-unreachable",
              "U 1200 missing MISSING active cause=credentials-locked");

        QTest::newRow("missing: snooze, and it comes back when the snooze ends") <<
            "r 0 100\n"
            "t 1000\n"
            "snz 1000 missing 15\n"
            "t 2000\n" <<
            L("J 180.001 missing MISSING pending pending",
              "J 900.001 missing MISSING active active",
              "N 900.001 missing MISSING",
              "J 1000 missing MISSING snoozed snoozed",
              "W 1000 missing MISSING",
              "J 1900 missing MISSING active snooze-ended",
              "N 1900 missing MISSING");

        QTest::newRow("missing: acknowledged stays acknowledged until a reading comes") <<
            "r 0 100\n"
            "t 1000\n"
            "ack 1000 missing\n"
            "t 99999\n" <<
            L("J 180.001 missing MISSING pending pending",
              "J 900.001 missing MISSING active active",
              "N 900.001 missing MISSING",
              "J 1000 missing MISSING acknowledged acknowledged",
              "U 1000 missing MISSING acknowledged");

        // ---- warm-up (REQ-ALR-03, REQ-SEN-03) ---------------------------------------------------
        QTest::newRow("warm-up: no missing-data alert is raised during it") <<
            "r 0 100\n"
            "st 10 sensor-warm-up -\n"
            "t 3000\n" <<
            L();

        QTest::newRow("warm-up: the clock restarts when it ends") <<
            "r 0 100\n"
            "st 10 sensor-warm-up -\n"
            "t 3000\n"
            "st 3600 no-new-readings -\n"
            "t 5000\n" <<
            L("J 3780.001 missing MISSING pending pending reason=no-new-readings",
              "J 4500.001 missing MISSING active active reason=no-new-readings",
              "N 4500.001 missing MISSING cause=no-new-readings");

        QTest::newRow("warm-up: a missing-data alert already active is withdrawn when one starts") <<
            "r 0 100\n"
            "st 10 source-unreachable -\n"
            "t 1000\n"
            "st 1200 sensor-warm-up -\n"
            "t 3000\n" <<
            L("J 180.001 missing MISSING pending pending reason=source-unreachable",
              "J 900.001 missing MISSING active active reason=source-unreachable",
              "N 900.001 missing MISSING cause=source-unreachable",
              "J 1200 missing MISSING cleared cleared reason=warm-up",
              "W 1200 missing MISSING");

        QTest::newRow("warm-up: a glucose alert is not suppressed by it") <<
            "r 0 100\n"
            "st 10 sensor-warm-up -\n"
            "r 120 50\n" <<
            L("J 120 low LL active active",
              "N 120 low LL");

        // ---- a mixed night ---------------------------------------------------------------------
        QTest::newRow("pending H needs a fresh reading to become active") <<
            "rr 0 60 60 100\n"
            "rr 120 240 60 181\n"
            "t 1100\n"
            "r 1200 190\n" <<
            L("J 120 high H pending pending",
              "J 420.001 missing MISSING pending pending",
              "J 1140.001 missing MISSING active active",
              "N 1140.001 missing MISSING",
              "J 1200 missing MISSING cleared cleared reason=reading",
              "W 1200 missing MISSING",
              "J 1200 high H active active",
              "N 1200 high H");
    }

    void scripts()
    {
        QFETCH(QString, script);
        QFETCH(QStringList, expect);
        AlertRig rig;
        const QStringList got = rig.run(script);
        QCOMPARE(got, expect);
    }

    // ---- missing data with the guard in each state: it fires whatever the guard does (REQ-ALR-02) ---
    void missingDataFiresWithEveryCauseOfRequirementSchedulerSeven_data()
    {
        QTest::addColumn<QString>("cause");
        QTest::newRow("1 cannot reach the source") << "source-unreachable";
        QTest::newRow("2 no new readings") << "no-new-readings";
        QTest::newRow("4 disabled by the guard") << "disabled-by-guard";
        QTest::newRow("5 service not running") << "service-not-running";
        QTest::newRow("6 waiting for the user's approval") << "credentials-locked";   // not held back
        QTest::newRow("7 monitoring stopped, action needed") << "needs-attention";
    }
    void missingDataFiresWithEveryCauseOfRequirementSchedulerSeven()
    {
        QFETCH(QString, cause);
        AlertRig rig;
        const QStringList got = rig.run(QStringLiteral("r 0 100\nst 10 %1 -\nt 1000\n").arg(cause));
        QStringList expect;
        expect << QStringLiteral("J 180.001 missing MISSING pending pending reason=%1").arg(cause)
               << QStringLiteral("J 900.001 missing MISSING active active reason=%1").arg(cause)
               << QStringLiteral("N 900.001 missing MISSING cause=%1").arg(cause);
        QCOMPARE(got, expect);
    }

    // ---- the boundary readings of section 8, with the defaults and with an edited L and H -----
    void boundaryReadings_data()
    {
        QTest::addColumn<int>("l");
        QTest::addColumn<int>("h");
        QTest::addColumn<double>("value");
        QTest::addColumn<QString>("low");
        QTest::addColumn<QString>("high");
        const char *d = "defaults";
        struct Row { int l, h; double v; const char *low; const char *high; };
        const Row rows[] = {
            { 70, 180, 53, "low:LL:active", "-" },
            { 70, 180, 54, "low:L:active", "-" },
            { 70, 180, 55, "low:L:active", "-" },
            { 70, 180, 69, "low:L:active", "-" },
            { 70, 180, 70, "-", "-" },
            { 70, 180, 71, "-", "-" },
            { 70, 180, 179, "-", "-" },
            { 70, 180, 180, "-", "-" },
            { 70, 180, 181, "-", "high:H:pending" },       // H waits 15 minutes
            { 70, 180, 249, "-", "high:H:pending" },
            { 70, 180, 250, "-", "high:H:pending" },
            { 70, 180, 251, "-", "high:HH:active" },
            // L loosened to 65 and H to 200
            { 65, 200, 53, "low:LL:active", "-" },
            { 65, 200, 54, "low:L:active", "-" },
            { 65, 200, 64, "low:L:active", "-" },
            { 65, 200, 65, "-", "-" },
            { 65, 200, 66, "-", "-" },
            { 65, 200, 70, "-", "-" },
            { 65, 200, 181, "-", "-" },
            { 65, 200, 199, "-", "-" },
            { 65, 200, 200, "-", "-" },
            { 65, 200, 201, "-", "high:H:pending" },
            { 65, 200, 250, "-", "high:H:pending" },
            { 65, 200, 251, "-", "high:HH:active" },
            // L tightened to 80 and H to 150
            { 80, 150, 79, "low:L:active", "-" },
            { 80, 150, 80, "-", "-" },
            { 80, 150, 150, "-", "-" },
            { 80, 150, 151, "-", "high:H:pending" },
        };
        for (size_t i = 0; i < sizeof(rows) / sizeof(rows[0]); ++i) {
            const Row &r = rows[i];
            QTest::newRow(qPrintable(QStringLiteral("L%1 H%2 value %3").arg(r.l).arg(r.h).arg(r.v)))
                << r.l << r.h << r.v << QString::fromLatin1(r.low) << QString::fromLatin1(r.high);
        }
        Q_UNUSED(d);
    }
    void boundaryReadings()
    {
        QFETCH(int, l); QFETCH(int, h); QFETCH(double, value);
        QFETCH(QString, low); QFETCH(QString, high);
        AlertRig rig;
        rig.run(QStringLiteral("th 1 %1 %2\nr 60 100\nr 120 %3\n").arg(l).arg(h).arg(value));
        QCOMPARE(rig.shape(AlertSlot::Low), low);
        QCOMPARE(rig.shape(AlertSlot::High), high);
    }

    // ---- LL and HH can be neither delayed nor silenced: the whole set of settings ---------------
    void noSettingDelaysOrSilencesLLAndHH()
    {
        // Every user-facing setting at its most delaying value, plus a snooze attempt.
        for (int urgent = 0; urgent < 2; ++urgent) {
            AlertRig rig;
            rig.run("cfg 1 99999 99999 99999\nr 60 100\n");
            rig.run(urgent ? "r 120 40\n" : "r 120 400\n");
            const AlertSlot slot = urgent ? AlertSlot::Low : AlertSlot::High;
            QCOMPARE(rig.view(slot).state, AlertState::Active);       // active in the very reading
            QCOMPARE(rig.view(slot).type, urgent ? AlertType::LL : AlertType::HH);
            const ActionOutcome snooze = rig.engine().snooze(rig.subject(), slot, 15, alertTime(130));
            QCOMPARE(snooze.result, ActionResult::NotSnoozable);
            QCOMPARE(rig.view(slot).state, AlertState::Active);
            // An acknowledgement pauses the reminders for 15 minutes and no longer.
            QCOMPARE(rig.engine().acknowledge(rig.subject(), slot, alertTime(140)).result, ActionResult::Done);
            QCOMPARE(rig.view(slot).state, AlertState::Acknowledged);
            rig.engine().tick(alertTime(140 + 15 * 60 + 1));
            QCOMPARE(rig.view(slot).state, AlertState::Active);
        }
    }

    void theThresholdsOfAConfigurationAreHeldToTheRules()
    {
        // LL and HH are the global values whatever a configuration says (REQ-THR-01) ...
        AlertRig rig;
        SubjectAlertConfig c;
        c.thresholds = Thresholds(10, 70, 180, 400);
        rig.configure(1, c);
        rig.run("r 60 100\nr 120 53\n");
        QCOMPARE(rig.shape(AlertSlot::Low), QString("low:LL:active"));    // 53 is below the global 54
        rig.run("r 180 251\n");
        QCOMPARE(rig.shape(AlertSlot::High), QString("high:HH:active"));   // above the global 250
        // ... and an inconsistent set of L and H falls back to the defaults.
        AlertRig other;
        SubjectAlertConfig bad;
        bad.thresholds = Thresholds(54, 200, 100, 250);
        other.configure(1, bad);
        other.run("r 60 100\nr 120 69\n");
        QCOMPARE(other.shape(AlertSlot::Low), QString("low:L:active"));   // L is 70 again
    }

    // ---- views and wake-ups -----------------------------------------------------------------------
    void theAlertsListHoldsOnlySlotsThatAreNotNormal()
    {
        AlertRig rig;
        rig.run("cfg 1 0 0 15\nrr 0 60 60 100\nr 120 69\nr 180 190\n");
        const QVector<AlertView> a = rig.engine().alerts();
        QCOMPARE(a.size(), 2);
        QCOMPARE(a.at(0).slot, AlertSlot::Low);
        QCOMPARE(a.at(1).slot, AlertSlot::High);
        QVERIFY(!a.at(0).episode.isNull());
        QVERIFY(a.at(0).episode != a.at(1).episode);
    }

    void aNewCrossingAfterAClearingIsANewEpisode()
    {
        AlertRig rig;
        rig.run("rr 0 60 60 100\nr 120 69\nr 180 80\nr 240 80\n");     // cleared by hysteresis
        const QUuid first = rig.records().first().episode;
        rig.run("r 300 69\n");
        const QVector<AlertRecord> r = rig.records();
        QCOMPARE(r.first().episode, r.at(1).episode);                   // active and cleared: one episode
        QVERIFY(r.last().episode != first);
    }

    void theNextWakeupIsTheNextDueTime()
    {
        AlertRig rig;
        QVERIFY(rig.engine().nextWakeup().isValid());                  // the missing-data clock runs from the start
        QCOMPARE(AlertRig::fmt(rig.engine().nextWakeup()), QString("180.001"));
        rig.run("rr 0 60 60 100\n");
        QCOMPARE(AlertRig::fmt(rig.engine().nextWakeup()), QString("240.001"));
        rig.run("r 120 50\n");                                         // LL: reminder in 2 minutes
        QCOMPARE(AlertRig::fmt(rig.engine().nextWakeup()), QString("240"));
        rig.run("ack 130 low\n");                                      // re-alert in 15 minutes
        QCOMPARE(AlertRig::fmt(rig.engine().nextWakeup()), QString("300.001"));   // the missing clock again
    }

    void aReadingWithAnUnknownUnitProvesLifeButRaisesNothing()
    {
        AlertRig rig;
        rig.run("r 0 100\nst 10 source-unreachable -\nt 1000\n");     // missing data is active
        rig.take();
        rig.reading(1100, 5, "furlongs");
        QCOMPARE(rig.take(), L("J 1100 missing MISSING cleared cleared reason=reading",
                               "W 1100 missing MISSING"));
        QCOMPARE(rig.shape(AlertSlot::Low), QString("-"));
    }

    void aGapThatIsOverBeforeTheServiceStartsRaisesNoMissingDataAlert()
    {
        // option B. The service starts at second 5000; the newest stored reading is
        // from second 0 (a long gap). The clock of missing data runs from the start, not from the
        // old reading, so nothing is raised until 180 s (and 15 min) after the start; the
        // interruption notice is what tells about the gap.
        AlertEngine e;
        const QUuid s = alertTestSubject();
        e.setSubject(s, SubjectAlertConfig(), alertTime(5000));
        StalenessInfo info;
        info.lastReadingAt = alertTime(0);
        info.cause = "service-not-running";
        QVERIFY(e.onStaleness(s, info, alertTime(5000)).isEmpty());
        QVERIFY(e.tick(alertTime(5100)).isEmpty());
        QCOMPARE(AlertRig::fmt(e.nextWakeup()), QString("5180.001"));
        QCOMPARE(e.tick(alertTime(5200)).size(), 1);               // pending, 180 s after the start
        QCOMPARE(e.view(s, AlertSlot::Missing).state, AlertState::Pending);
        // and a reading that arrives in time prevents it altogether
        AlertEngine f;
        f.setSubject(s, SubjectAlertConfig(), alertTime(5000));
        f.onStaleness(s, info, alertTime(5000));
        f.onReading(s, 100, "mg/dL", alertTime(5040), alertTime(5041));
        QVERIFY(f.tick(alertTime(5200)).isEmpty());
    }

    // ---- the facts the effects carry for the texts ----------------
    static double secondsOf(const UtcTime &t)
    {
        return t.isValid() ? double(t.msecsSinceEpoch() - kAlertTestStartMs) / 1000.0 : -1e9;
    }

    void aPendingHCarriesWhenThePendingBeganAndTheLimitInForce()
    {
        AlertRig rig;
        rig.run("rr 0 60 60 100\nrr 120 1100 60 181\n");           // pending at 120, active at 1020
        const AlertEffect n = rig.lastEffect(AlertEffect::Notify, AlertSlot::High);
        QCOMPARE(n.type, AlertType::H);
        QCOMPARE(secondsOf(n.levelSince), 120.0);                  // the first reading beyond 180, not 1020
        QCOMPARE(secondsOf(n.episodeSince), 120.0);
        QVERIFY(n.hasLimit);
        QCOMPARE(n.limit, 180);
        QVERIFY(!n.neverAcknowledged);
    }

    void aLevelThatRisesHasItsOwnSinceAndTheEpisodeKeepsTheFirst()
    {
        AlertRig rig;
        rig.run("rr 0 60 60 100\nr 120 69\nr 180 53\nr 240 53\n");   // L at 120, LL at 180
        const AlertEffect ll = rig.lastEffect(AlertEffect::Notify, AlertSlot::Low);
        QCOMPARE(ll.type, AlertType::LL);
        QCOMPARE(secondsOf(ll.levelSince), 180.0);
        QCOMPARE(secondsOf(ll.episodeSince), 120.0);
        QCOMPARE(ll.limit, 54);
        // the reminder of LL (300 s) carries the same facts
        rig.run("r 300 53\n");
        const AlertEffect r = rig.lastEffect(AlertEffect::Remind, AlertSlot::Low);
        QCOMPARE(secondsOf(r.levelSince), 180.0);
        QCOMPARE(secondsOf(r.episodeSince), 120.0);
        // lowered back to L after two readings: L's stretch is the whole episode again
        rig.run("r 360 62\nr 420 63\n");
        const AlertEffect u = rig.lastEffect(AlertEffect::Update, AlertSlot::Low);
        QCOMPARE(u.type, AlertType::L);
        QCOMPARE(secondsOf(u.levelSince), 120.0);
        QCOMPARE(u.limit, 70);
    }

    void anUrgentAlertThatStartsAtOnceHasOneSinceForBothLevels()
    {
        AlertRig rig;
        rig.run("rr 0 60 60 100\nr 120 50\n");
        const AlertEffect n = rig.lastEffect(AlertEffect::Notify, AlertSlot::Low);
        QCOMPARE(secondsOf(n.levelSince), 120.0);
        QCOMPARE(secondsOf(n.episodeSince), 120.0);
        QCOMPARE(n.limit, 54);
    }

    void theLimitIsTheSubjectsOwnBoundary()
    {
        AlertRig rig;
        rig.run("th 1 65 200\nrr 0 60 60 100\nr 120 64\nr 180 205\n");
        QCOMPARE(rig.lastEffect(AlertEffect::Notify, AlertSlot::Low).limit, 65);
        // H waits 15 minutes: the pending line has no notification; the limit is also in the view
        const AlertView h = rig.view(AlertSlot::High);
        QVERIFY(h.hasLimit);
        QCOMPARE(h.limit, 200);
    }

    void missingDataCarriesTheNewestReadingTheCauseAndNoLimit()
    {
        AlertRig rig;
        rig.run("r 0 100\nst 10 source-unreachable -\nt 1000\n");
        const AlertEffect n = rig.lastEffect(AlertEffect::Notify, AlertSlot::Missing);
        QCOMPARE(n.cause, QString("source-unreachable"));
        QCOMPARE(secondsOf(n.newestReadingAt), 0.0);
        QVERIFY(!n.hasLimit);                                      // a boundary is a setting; there is none
        QVERIFY(n.levelSince.isValid());
        QVERIFY(!n.silent);
    }

    void aWithdrawSaysWhetherNobodyAcknowledgedAndWhenItEnded()
    {
        // cleared without an acknowledgement: the "ended" entry is due
        AlertRig a;
        a.run("rr 0 60 60 100\nr 120 69\nr 180 80\nr 240 80\n");
        AlertEffect w = a.lastEffect(AlertEffect::Withdraw, AlertSlot::Low);
        QVERIFY(w.neverAcknowledged);
        QCOMPARE(secondsOf(w.clearedAt), 240.0);
        QCOMPARE(secondsOf(w.episodeSince), 120.0);                // "ended after 2 min" is clearedAt - episodeSince
        // acknowledged first: no entry
        AlertRig b;
        b.run("rr 0 60 60 100\nr 120 69\nack 130 low\nr 180 80\nr 240 80\n");
        w = b.lastEffect(AlertEffect::Withdraw, AlertSlot::Low);
        QVERIFY(!w.neverAcknowledged);
        // snoozed first: the user dealt with it, no entry either
        AlertRig c;
        c.run("rr 0 60 60 100\nr 120 69\nsnz 130 low 15\nr 180 80\nr 240 80\n");
        w = c.lastEffect(AlertEffect::Withdraw, AlertSlot::Low);
        QVERIFY(w.kind == AlertEffect::Withdraw);
        QVERIFY(!w.neverAcknowledged);
        // the Withdraw of the snooze itself is not a clearing
        AlertRig d;
        d.run("rr 0 60 60 100\nr 120 69\nsnz 130 low 15\n");
        QVERIFY(!d.lastEffect(AlertEffect::Withdraw, AlertSlot::Low).neverAcknowledged);
        // an acknowledgement before an escalation still counts for the episode
        AlertRig e;
        e.run("rr 0 60 60 100\nr 120 69\nack 130 low\nr 180 53\nr 240 90\nr 300 90\n");
        QVERIFY(!e.lastEffect(AlertEffect::Withdraw, AlertSlot::Low).neverAcknowledged);
        // missing data never gets an entry
        AlertRig m;
        m.run("r 0 100\nt 1000\nr 1100 100\n");
        w = m.lastEffect(AlertEffect::Withdraw, AlertSlot::Missing);
        QVERIFY(w.kind == AlertEffect::Withdraw);
        QVERIFY(!w.neverAcknowledged);
    }

    void aPendingAlertThatRevertsLeavesNothingToWithdraw()
    {
        AlertRig rig;
        rig.run("rr 0 60 60 100\nr 120 181\nr 180 150\n");
        QCOMPARE(rig.effects().size(), 2);                          // the two journal lines only
    }

    void theAlertListHoldsActiveAcknowledgedAndSnoozedAlertsWithTheirFacts()
    {
        AlertRig rig;
        rig.run("cfg 1 0 0 15\nrr 0 60 60 100\nr 120 69\nr 180 190\n");      // L and H active
        QVector<AlertView> a = rig.engine().alerts();
        QCOMPARE(a.size(), 2);
        QCOMPARE(a.at(0).type, AlertType::L);
        QCOMPARE(secondsOf(a.at(0).levelSince), 120.0);
        QVERIFY(!a.at(0).acknowledged);
        QCOMPARE(a.at(1).type, AlertType::H);
        QCOMPARE(secondsOf(a.at(1).levelSince), 180.0);
        rig.run("ack 200 low\nsnz 210 high 30\n");
        a = rig.engine().alerts();
        QCOMPARE(a.size(), 2);
        QCOMPARE(a.at(0).state, AlertState::Acknowledged);
        QVERIFY(a.at(0).acknowledged);
        QCOMPARE(a.at(1).state, AlertState::Snoozed);
        QVERIFY(!a.at(1).acknowledged);
        // a pending alert is not in the list
        AlertRig p;
        p.run("rr 0 60 60 100\nr 120 181\n");
        QCOMPARE(p.shape(AlertSlot::High), QString("high:H:pending"));
        QVERIFY(p.engine().alerts().isEmpty());
    }

    void anUnknownSubjectIsRefusedAndIgnored()
    {
        AlertEngine e;
        const QUuid nobody = alertTestSubject(99);
        QVERIFY(e.onReading(nobody, 50, "mg/dL", alertTime(1), alertTime(1)).isEmpty());
        QCOMPARE(e.acknowledge(nobody, AlertSlot::Low, alertTime(1)).result, ActionResult::UnknownSubject);
        QCOMPARE(e.snooze(nobody, AlertSlot::Low, 15, alertTime(1)).result, ActionResult::UnknownSubject);
    }

    void effectsNeverCarryAGlucoseValueOrAName()
    {
        // The engine's effects have no field for a value or a name; the journal record is checked
        // in tst_alertjournal. Here: the text lines of a whole episode hold the Subject's
        // sample only as a UUID, never as a number the reading had.
        AlertRig rig;
        const QStringList all = rig.run("rr 0 60 60 100\nr 120 53.7\nr 180 140\nr 240 141\n");
        for (int i = 0; i < all.size(); ++i) {
            QVERIFY2(!all.at(i).contains("53.7"), qPrintable(all.at(i)));
            QVERIFY2(!all.at(i).contains("140"), qPrintable(all.at(i)));
        }
    }
};

QTEST_APPLESS_MAIN(TstAlertEngine)
#include "tst_alertengine.moc"
