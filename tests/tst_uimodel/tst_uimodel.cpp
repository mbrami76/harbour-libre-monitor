// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Michele Brami
// Tests of the list model, the plot controller and the scripted development backend (app/ui).
// What they prove: the rows the QML reads carry the decisions of the state inventory (no band
// text, no direction and no "current" flag for a stale Subject), the display unit and locale
// change the value text, ages move with the clock, the cover picks the most urgent Subject, and
// a history view of the plot is grey and not moved by new data.
#include "service/subjectlabel.h"
#include "ui/band.h"
#include "ui/fakebackend.h"
#include "ui/plotcontroller.h"
#include "ui/subjectmodel.h"
#include "ui/uiconstants.h"

#include <QSignalSpy>
#include <QtTest>

using namespace lm::ui;

namespace {

// A backend whose clock and data the test sets.
class TestBackend : public Backend
{
public:
    QList<SubjectSnapshot> list;
    ServiceStatus status = ServiceStatus::Running;
    qint64 now = 1700000000000LL;
    int retried = 0, reenabled = 0;
    QStringList handledReports;
    QList<SubjectSnapshot> subjects() const { return list; }
    ServiceStatus serviceStatus() const { return status; }
    qint64 nowMs() const { return now; }
    void retry(const QString &) { ++retried; }
    void reenable(const QString &) { ++reenabled; }
    void reportHandled(const QString &id) { handledReports << id; }
    void startMonitoring() {}
    void askAgain() {}
    void poke() { emit changed(); }
};

SubjectSnapshot subject(const QString &id, double mgdl, qint64 ageMs, qint64 now)
{
    SubjectSnapshot s;
    s.id = id;
    s.name = QStringLiteral("Name ") + id;
    s.report.state = QStringLiteral("idle");
    s.hasReading = true;
    s.measuredAtMs = now - ageMs;
    s.value = mgdl;
    s.unit = QStringLiteral("mg/dL");
    s.hasTrend = true;
    s.trendCode = 3;
    return s;
}

QVariant role(const SubjectModel &m, int row, const char *name)
{
    const QHash<int, QByteArray> r = m.roleNames();
    for (QHash<int, QByteArray>::const_iterator it = r.begin(); it != r.end(); ++it)
        if (it.value() == name)
            return m.data(m.index(row), it.key());
    return QVariant();
}

}

class TstUiModel : public QObject
{
    Q_OBJECT

private slots:
    // ---- band (moved to core/alerts/band.h, rule) ----------------------------
    // Three expectations first version changed (thresholds are whole mg/dL,
    // compared strictly on the stored value): 54 mg/dL is no longer LL, and a stored 10.0 or 13.9
    // mmol/L is one step higher because it converts to 180.16 and 250.42 mg/dL. Filed as;
    // the full boundary table is in tst_alertthresholds.
    void bandsFollowTheGlobalDefaultsAndAreStrictAtTheEdges()
    {
        QCOMPARE(bandOf(2.9, "mmol/L"), Band::LL);
        QCOMPARE(bandOf(3.0, "mmol/L"), Band::L);          // "below 3.0" is LL, 3.0 itself is L
        QCOMPARE(bandOf(3.8, "mmol/L"), Band::L);
        QCOMPARE(bandOf(3.9, "mmol/L"), Band::N);
        QCOMPARE(bandOf(10.0, "mmol/L"), Band::H);         // 180.16 mg/dL is above 180
        QCOMPARE(bandOf(10.1, "mmol/L"), Band::H);
        QCOMPARE(bandOf(13.9, "mmol/L"), Band::HH);        // 250.42 mg/dL is above 250
        QCOMPARE(bandOf(14.0, "mmol/L"), Band::HH);
        QCOMPARE(bandOf(112, "mg/dL"), Band::N);
        QCOMPARE(bandOf(53, "mg/dL"), Band::LL);
        QCOMPARE(bandOf(54.0, "mg/dL"), Band::L);          // "below 54" is LL, 54 itself is L
        QCOMPARE(bandOf(55, "mg/dL"), Band::L);
        QCOMPARE(bandOf(5, "furlongs"), Band::Unknown);
        QCOMPARE(bandOf(std::nan(""), "mg/dL"), Band::Unknown);
    }

    // ---- the list model --------------------------------------------------------------------------------
    void rowsCarryTheDecisions()
    {
        TestBackend b;
        b.list << subject("{a}", 112, 30000, b.now);                         // fresh, N
        SubjectSnapshot stale = subject("{b}", 112, 20 * 60000, b.now);       // 20 min old, service says stale
        stale.report.stale = true;
        stale.report.staleCause = QStringLiteral("no-new-readings");
        b.list << stale;
        SubjectModel m;
        m.setLocale(QLocale(QLocale::C));
        m.setBackend(&b);
        QCOMPARE(m.rowCount(), 2);
        QCOMPARE(role(m, 0, "stateName").toString(), QStringLiteral("B-N"));
        QCOMPARE(role(m, 0, "bandKey").toString(), QStringLiteral("n"));
        QCOMPARE(role(m, 0, "valueText").toString(), QStringLiteral("6.2"));        // default mmol/L
        QVERIFY(role(m, 0, "fresh").toBool());
        QVERIFY(role(m, 0, "directionIsArrow").toBool());
        QCOMPARE(role(m, 1, "stateName").toString(), QStringLiteral("ST-2"));
        QVERIFY(role(m, 1, "bandKey").toString().isEmpty());                    // no colour on stale
        QVERIFY(role(m, 1, "directionSymbol").toString().isEmpty());                // no arrow on stale
        QVERIFY(!role(m, 1, "fresh").toBool());
        QCOMPARE(role(m, 1, "valueText").toString(), QStringLiteral("6.2"));        // number stays readable
        QCOMPARE(m.attentionCount(), 1);
    }
    void displayUnitAndLocaleChangeTheText()
    {
        TestBackend b;
        b.list << subject("{a}", 112, 30000, b.now);
        SubjectModel m;
        m.setBackend(&b);
        m.setLocale(QLocale(QLocale::Italian, QLocale::Italy));
        QCOMPARE(role(m, 0, "valueText").toString(), QStringLiteral("6,2"));
        m.setDisplayUnit("mg/dL");
        QCOMPARE(role(m, 0, "valueText").toString(), QStringLiteral("112"));
        QCOMPARE(role(m, 0, "unitText").toString(), QStringLiteral("mg/dL"));
        m.setDisplayUnit("stones");                          // ignored
        QCOMPARE(m.displayUnit(), QStringLiteral("mg/dL"));
    }
    void aTickMakesAValueStopLookingCurrent()
    {
        TestBackend b;
        b.list << subject("{a}", 112, 30000, b.now);
        SubjectModel m;
        m.setBackend(&b);
        QVERIFY(role(m, 0, "fresh").toBool());
        b.now += (kUiFreshMaxS + 10) * 1000LL;               // nothing arrived, the service says nothing
        m.refresh();                                          // the display tick
        QVERIFY(!role(m, 0, "fresh").toBool());               // REQ-UI-09
        QVERIFY(role(m, 0, "bandKey").toString().isEmpty());
        QVERIFY(role(m, 0, "ageSeconds").toLongLong() > kUiFreshMaxS);
    }
    void aMissingTrendIsNullOnAFreshReading()
    {
        TestBackend b;
        SubjectSnapshot s = subject("{a}", 112, 30000, b.now);
        s.hasTrend = false;
        b.list << s;
        SubjectModel m;
        m.setBackend(&b);
        QCOMPARE(role(m, 0, "directionSymbol").toString(), QStringLiteral("NULL"));
        QVERIFY(!role(m, 0, "directionIsArrow").toBool());
    }
    void buttonsReachTheBackend()
    {
        TestBackend b;
        b.list << subject("{a}", 112, 30000, b.now);
        SubjectModel m;
        m.setBackend(&b);
        m.retry("{a}");
        m.reenable("{a}");
        QCOMPARE(b.retried, 1);
        QCOMPARE(b.reenabled, 1);
        QCOMPARE(m.indexOfSubject("{a}"), 0);
        QCOMPARE(m.indexOfSubject("{x}"), -1);
        QCOMPARE(m.get(0).value("subjectId").toString(), QStringLiteral("{a}"));
        QVERIFY(m.get(7).isEmpty());
    }
    void theCoverPicksTheMostUrgentAndStepsByHandThenReturns()
    {
        TestBackend b;
        b.list << subject("{n}", 112, 30000, b.now);          // N
        b.list << subject("{ll}", 48, 30000, b.now);          // LL
        b.list << subject("{l}", 63, 30000, b.now);           // L
        SubjectModel m;
        m.setBackend(&b);
        QCOMPARE(m.coverIndex(), 1);
        m.stepCover(1);
        QCOMPARE(m.coverIndex(), 2);
        m.stepCover(-2);
        QCOMPARE(m.coverIndex(), 0);
        b.now += (kUiCoverManualHoldS + 1) * 1000LL;
        m.refresh();
        QCOMPARE(m.coverIndex(), 1);                          // the hold ran out
    }
    void aBackendChangeRefreshesTheModel()
    {
        TestBackend b;
        SubjectModel m;
        m.setBackend(&b);
        QCOMPARE(m.rowCount(), 0);
        QCOMPARE(m.coverIndex(), -1);
        QSignalSpy spy(&m, SIGNAL(changed()));
        b.list << subject("{a}", 112, 30000, b.now);
        b.poke();
        QCOMPARE(m.rowCount(), 1);
        QVERIFY(spy.count() >= 1);
    }

    // ---- the plot controller -------------------------------------------------------------------------------
    void historyViewIsGreyAndNotMovedByNewData()
    {
        TestBackend b;
        SubjectSnapshot s = subject("{a}", 112, 30000, b.now);
        for (qint64 t = b.now - 30 * 3600000LL; t <= b.now - 30000; t += 60000)
            s.current.append(PlotPoint(t, 110));
        b.list << s;
        PlotController c;
        c.setBackend(&b);
        c.setSubjectId("{a}");
        QVERIFY(c.isLive() && !c.isGrey() && !c.canReturnToNow());
        QVERIFY(c.hasData());
        c.panByFraction(0.5);                                 // drag right: back in time
        QVERIFY(!c.isLive() && c.isGrey() && c.canReturnToNow());
        const qlonglong to = c.toMs();
        b.now += 10 * 60000;                                  // time passes, data arrive
        b.poke();
        QCOMPARE(c.toMs(), to);                               // a history view stays where it is
        const PlotGeometry g = c.geometry(540, 400);
        QVERIFY(g.zones[2].grey);
        b.list[0].measuredAtMs = b.now - 30000;                // a new reading arrived meanwhile
        c.backToNow();
        QVERIFY(c.isLive() && !c.isGrey() && !c.canReturnToNow());
        QVERIFY(!c.geometry(540, 400).zones[2].grey);
    }
    void aStaleSubjectPlotIsGreyEvenWhenLive()
    {
        TestBackend b;
        SubjectSnapshot s = subject("{a}", 112, 30 * 60000, b.now);
        s.report.stale = true;
        s.report.staleCause = QStringLiteral("no-new-readings");
        for (qint64 t = b.now - 3 * 3600000LL; t <= b.now - 30 * 60000; t += 60000)
            s.current.append(PlotPoint(t, 110));
        b.list << s;
        PlotController c;
        c.setBackend(&b);
        c.setSubjectId("{a}");
        QVERIFY(c.isLive());
        const PlotGeometry g = c.geometry(540, 400);
        QVERIFY(g.zones[0].grey);
        QVERIFY(g.ringHollow);
    }
    void feedsSettingSelectsTheSeries()
    {
        TestBackend b;
        SubjectSnapshot s = subject("{a}", 112, 30000, b.now);
        for (qint64 t = b.now - 3 * 3600000LL; t <= b.now; t += 60000) s.current.append(PlotPoint(t, 110));
        for (qint64 t = b.now - 3 * 3600000LL; t <= b.now; t += 300000) s.history.append(PlotPoint(t, 105));
        b.list << s;
        PlotController c;
        c.setBackend(&b);
        c.setSubjectId("{a}");
        c.setFeeds(0);
        QVERIFY(!c.geometry(540, 400).current.isEmpty() && c.geometry(540, 400).history.isEmpty());
        c.setFeeds(1);
        QVERIFY(c.geometry(540, 400).current.isEmpty() && !c.geometry(540, 400).history.isEmpty());
        c.setFeeds(2);
        QVERIFY(!c.geometry(540, 400).current.isEmpty() && !c.geometry(540, 400).history.isEmpty());
        c.setFeeds(7);                                        // ignored
        QCOMPARE(c.feeds(), 2);
    }
    void unknownSubjectGivesAnEmptyPlot()
    {
        TestBackend b;
        PlotController c;
        c.setBackend(&b);
        c.setSubjectId("{nobody}");
        QVERIFY(!c.hasData());
        QVERIFY(c.geometry(540, 400).zones.isEmpty());
    }

    // ---- the scripted development backend --------------------------------------------------------------------
    void scenarioZeroIsInTheStatesTheMockUpShows()
    {
        FakeBackend f;
        SubjectModel m;
        m.setLocale(QLocale(QLocale::C));
        m.setBackend(&f);
        QCOMPARE(m.rowCount(), 3);
        QCOMPARE(role(m, 0, "stateName").toString(), QStringLiteral("B-N"));
        QCOMPARE(role(m, 1, "stateName").toString(), QStringLiteral("B-LL"));
        QCOMPARE(role(m, 2, "stateName").toString(), QStringLiteral("ST-1"));
        QCOMPARE(m.coverIndex(), 1);                                          // the urgent low
        QCOMPARE(f.serviceStatus(), ServiceStatus::Running);
    }
    void scenarioOneHasAGuardStopWhichReenableClears()
    {
        FakeBackend f;
        f.setScenario(1);
        SubjectModel m;
        m.setBackend(&f);
        QCOMPARE(role(m, 0, "stateName").toString(), QStringLiteral("B-L"));
        QCOMPARE(role(m, 1, "stateName").toString(), QStringLiteral("ST-4"));
        QVERIFY(role(m, 1, "showReenable").toBool());
        QCOMPARE(role(m, 2, "stateName").toString(), QStringLiteral("ST-7b"));
        m.reenable(role(m, 1, "subjectId").toString());
        QCOMPARE(role(m, 1, "stateName").toString(), QStringLiteral("B-N"));
    }
    void scenarioTwoIsAStoppedServiceAndStartFixesIt()
    {
        FakeBackend f;
        f.setScenario(2);
        SubjectModel m;
        m.setBackend(&f);
        QCOMPARE(f.serviceStatus(), ServiceStatus::Stopped);
        QCOMPARE(role(m, 0, "stateName").toString(), QStringLiteral("ST-5"));
        f.startMonitoring();
        QCOMPARE(f.serviceStatus(), ServiceStatus::Running);
        QCOMPARE(role(m, 0, "stateName").toString().left(2), QStringLiteral("B-"));
    }
    void scenariosCycle()
    {
        FakeBackend f;
        QCOMPARE(f.scenario(), 0);
        f.nextScenario(); f.nextScenario(); f.nextScenario(); f.nextScenario();
        QCOMPARE(f.scenario(), 4);
        QCOMPARE(f.subjects().size(), 0);           // the empty state: no account set up
        f.nextScenario();
        QCOMPARE(f.scenario(), 0);
        QVERIFY(!f.scenarioName().isEmpty());
    }
    // The scenario of the colour review: one fresh patient in each of the five bands,
    // in the order very low, low, in range, high, very high.
    void scenarioThreeShowsEveryBandFresh()
    {
        FakeBackend f;
        f.setScenario(3);
        SubjectModel m;
        m.setBackend(&f);
        QCOMPARE(m.rowCount(), 5);
        const char *ids[] = { "B-LL", "B-L", "B-N", "B-H", "B-HH" };
        for (int i = 0; i < 5; ++i) {
            QCOMPARE(m.data(m.index(i), SubjectModel::StateNameRole).toString(), QString::fromLatin1(ids[i]));
            QVERIFY(m.data(m.index(i), SubjectModel::FreshRole).toBool());
        }
    }
    // a patient is named where a stranger may read it (the cover with names off, a chip, a
    // page without a name) by the function the notifications use: the user's short label, or P1, P2 by
    // the place in the configuration file, never the cover's own ranking.
    void theShortLabelIsTheNotificationsLabel()
    {
        FakeBackend f;
        f.setScenario(3);
        SubjectModel m;
        m.setBackend(&f);
        QCOMPARE(m.data(m.index(0), SubjectModel::ShortLabelRole).toString(), QStringLiteral("M"));   // its label
        QCOMPARE(m.data(m.index(4), SubjectModel::ShortLabelRole).toString(), QStringLiteral("P5"));  // no label: its place
        const QVariantList cover = m.coverRows(2);
        QCOMPARE(cover.size(), 2);
        for (int i = 0; i < cover.size(); ++i) {
            const QVariantMap row = cover.at(i).toMap();
            const int place = m.indexOfSubject(row.value("subjectId").toString());
            QCOMPARE(row.value("shortLabel").toString(), lm::patientLabel(row.value("label").toString(), place));
        }
    }
    void fakeSeriesAreSortedAndHaveAServiceGap()
    {
        FakeBackend f;
        const QList<SubjectSnapshot> s = f.subjects();
        QVERIFY(s.first().current.size() > 1000);
        for (int i = 1; i < s.first().current.size(); ++i)
            QVERIFY(s.first().current[i].tMs > s.first().current[i - 1].tMs);
        QCOMPARE(s.first().serviceGaps.size(), 1);
    }
    void aLateReadingKeepsBandTrendAndMarkerOnTheRowAndTheCoverWithinTheGrace()
    {
        TestBackend b;
        SubjectSnapshot s = subject("{1}", 250, 4 * 60 * 1000, b.now);     // a high, 4 minutes old
        s.report.stale = true;
        s.report.staleCause = "no-new-readings";
        s.report.staleSinceAgeS = kUiStaleGraceS - 5;
        b.list << s;
        SubjectModel m; m.setBackend(&b);
        QCOMPARE(role(m, 0, "fresh").toBool(), true);                       // still shown as current
        QCOMPARE(role(m, 0, "bandKey").toString(), QStringLiteral("h"));
        QVERIFY(role(m, 0, "directionSymbol").toString().length() > 0);     // the service still sends the arrow
        const QVariantList cover = m.coverRows(2);
        QCOMPARE(cover.size(), 1);
        QCOMPARE(cover[0].toMap()["fresh"].toBool(), true);                 // the cover as well
        QCOMPARE(cover[0].toMap()["bandKey"].toString(), QStringLiteral("h"));
        // Past the grace the same Subject shows its cause, without band, arrow or colour.
        b.list[0].report.staleSinceAgeS = kUiStaleGraceS + 1;
        b.poke();
        QCOMPARE(role(m, 0, "fresh").toBool(), false);
        QCOMPARE(role(m, 0, "bandKey").toString(), QString());
        QCOMPARE(role(m, 0, "stateKey").toString(), QStringLiteral("ui-stale-no-new"));
        QCOMPARE(m.coverRows(2)[0].toMap()["fresh"].toBool(), false);
        // A cause that is a known state shows at once, whatever the time since the flag.
        b.list[0].report.staleCause = "sensor-warm-up";
        b.list[0].report.staleSinceAgeS = 3;
        b.poke();
        QCOMPARE(role(m, 0, "fresh").toBool(), false);
    }
    void theTrendWordStaysForTheSameReadingWhileTheArrowGoes()
    {
        TestBackend b;
        SubjectSnapshot s = subject("{1}", 120, 90 * 1000, b.now);          // trend code 3: steady
        b.list << s;
        SubjectModel m; m.setBackend(&b);
        QCOMPARE(role(m, 0, "directionKey").toString(), QStringLiteral("ui-dir-steady"));
        QVERIFY(role(m, 0, "directionSymbol").toString().length() > 0);
        // The service drops the arrow after its limit (no trend for the same reading, still within the grace).
        b.list[0].hasTrend = false;
        b.list[0].report.stale = true;
        b.list[0].report.staleCause = "no-new-readings";
        b.list[0].report.staleSinceAgeS = 30;
        b.now += 200 * 1000;
        b.poke();
        QCOMPARE(role(m, 0, "fresh").toBool(), true);
        QCOMPARE(role(m, 0, "directionKey").toString(), QStringLiteral("ui-dir-steady"));   // the description stays
        QCOMPARE(role(m, 0, "directionSymbol").toString(), QString());                      // the arrow does not
        QCOMPARE(role(m, 0, "directionIsArrow").toBool(), false);
        // A NEW reading without an arrow does not borrow the old one.
        b.list[0].measuredAtMs = b.now - 10 * 1000;
        b.list[0].report.stale = false;
        b.poke();
        QCOMPARE(role(m, 0, "directionKey").toString(), QStringLiteral("ui-dir-null"));
    }
    void thePlotStaysColouredWhileTheReadingIsLateWithinTheGrace()
    {
        TestBackend b;
        SubjectSnapshot s = subject("{1}", 250, 4 * 60 * 1000, b.now);
        s.report.stale = true; s.report.staleCause = "no-new-readings"; s.report.staleSinceAgeS = 30;
        for (int i = 0; i < 60; ++i)
            s.current << PlotPoint(b.now - (i + 4) * 60000LL, 200 + (i % 7));
        std::reverse(s.current.begin(), s.current.end());
        b.list << s;
        PlotController pc; pc.setBackend(&b); pc.setSubjectId("{1}");
        PlotGeometry g = pc.geometry(500, 400);
        QVERIFY(!g.zones.isEmpty());
        QVERIFY(!g.zones[0].grey);                                          // zones keep their hue in the grace
        QVERIFY(!g.ringHollow);                                             // and the marker is the live one
        b.list[0].report.staleSinceAgeS = kUiStaleGraceS + 1;
        g = pc.geometry(500, 400);
        QVERIFY(g.zones[0].grey);                                           // past it they go grey
        QVERIFY(g.ringHollow);
    }
    void aReportProposalIsARowFlagAndDecliningTellsTheBackend()
    {
        TestBackend b;
        SubjectSnapshot s = subject("{1}", 110, 20000, b.now);
        s.reportProposed = true;
        b.list << s << subject("{2}", 110, 20000, b.now);
        SubjectModel m; m.setBackend(&b);
        QCOMPARE(role(m, 0, "reportProposed").toBool(), true);
        QCOMPARE(role(m, 1, "reportProposed").toBool(), false);
        m.declineReport("{1}");
        QCOMPARE(b.handledReports, QStringList() << "{1}");
    }
    void aBackendWithoutTheConceptIgnoresDecliningTheReport()
    {
        // The base class does nothing: a source or a service without ReportHandled is not an error.
        class Plain : public Backend {
        public:
            QList<SubjectSnapshot> subjects() const { return QList<SubjectSnapshot>(); }
            ServiceStatus serviceStatus() const { return ServiceStatus::Running; }
            void retry(const QString &) {} void reenable(const QString &) {}
            void startMonitoring() {} void askAgain() {}
        } p;
        p.reportHandled("{1}");                                     // must simply return
        QVERIFY(true);
    }
};

QTEST_APPLESS_MAIN(TstUiModel)
#include "tst_uimodel.moc"
