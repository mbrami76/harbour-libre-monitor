// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Michele Brami
// Tests of the plot logic (app/ui): the view window with zoom and scrolling back, the reduction
// of a long series, and the geometry (fixed scale, breaks, ticks, zones, bands). What they
// prove: the vertical scale never moves, a history view is grey and live again after the tap on the
// value reading box, peaks survive a wide zoom, and nothing is drawn as a stored reading that is not one.
#include "ui/plotgeometry.h"
#include "ui/plotreduce.h"
#include "ui/plotview.h"
#include "ui/uiconstants.h"

#include <QtTest>

using namespace lm::ui;

namespace {
const qint64 kNow = 1700000000000LL;
const qint64 kMinute = 60LL * 1000;
const qint64 kHour = 60 * kMinute;

QVector<PlotPoint> feed(qint64 from, qint64 to, qint64 step, double base = 110)
{
    QVector<PlotPoint> v;
    for (qint64 t = from; t <= to; t += step)
        v.append(PlotPoint(t, base + 30 * std::sin(double(t) / 3e6)));
    return v;
}

PlotInput input(const PlotView &view, int w = 540, int h = 400)
{
    PlotInput in;
    in.fromMs = view.fromMs(kNow);
    in.toMs = view.toMs(kNow);
    in.nowMs = kNow;
    in.widthPx = w;
    in.heightPx = h;
    return in;
}
}

class TstUiPlot : public QObject
{
    Q_OBJECT

private slots:
    // ---- the view ------------------------------------------------------------------------------
    void startsLiveWithTwelveHours()
    {
        PlotView v;
        QVERIFY(v.isLive());
        QVERIFY(!v.isGrey());
        QVERIFY(!v.canReturnToNow());
        QCOMPARE(v.widthMs(), kUiPlotWindowMs);
        QCOMPARE(v.toMs(kNow), kNow);
        QCOMPARE(v.fromMs(kNow), kNow - kUiPlotWindowMs);
    }
    void scrollingBackIsHistoryAndGrey()
    {
        PlotView v;
        v.panBy(-3 * kHour, kNow);
        QVERIFY(!v.isLive());
        QVERIFY(v.isGrey());
        QVERIFY(v.canReturnToNow());
        QCOMPARE(v.toMs(kNow), kNow - 3 * kHour);
        // A history view does not follow the clock.
        QCOMPARE(v.toMs(kNow + 10 * kMinute), kNow - 3 * kHour);
    }
    void scrollingBackToTheEdgeGoesLive()
    {
        PlotView v;
        v.panBy(-3 * kHour, kNow);
        v.panBy(3 * kHour - kUiLiveSnapMs / 2, kNow);
        QVERIFY(v.isLive());
        QVERIFY(!v.isGrey());
    }
    void neverIntoTheFuture()
    {
        PlotView v;
        v.panBy(-2 * kHour, kNow);
        v.panBy(10 * kHour, kNow);
        QVERIFY(v.isLive());
        QCOMPARE(v.toMs(kNow), kNow);
    }
    void notBeforeTheFirstReading()
    {
        PlotView v;
        v.setFirstDataMs(kNow - 20 * kHour);
        v.panBy(-100 * kHour, kNow);
        // Allowed margin: 5% of the width before the first reading.
        QVERIFY(v.fromMs(kNow) >= kNow - 20 * kHour - v.widthMs() / 20 - 1);
        QVERIFY(v.showsBeforeFirstData(kNow));
    }
    void zoomingALiveViewKeepsTheRightEdgeAtNow()
    {
        PlotView v;
        v.zoomBy(4.0, 0.3, kNow);
        QVERIFY(v.isLive());
        QVERIFY(!v.isGrey());                   // a zoomed live view ends now: not grey
        QCOMPARE(v.widthMs(), kUiPlotWindowMs / 4);
        QVERIFY(v.canReturnToNow());            // but the tap on the reading box returns to 12 h
        QCOMPARE(v.toMs(kNow), kNow);
    }
    void zoomLimits()
    {
        PlotView v;
        v.zoomBy(1e6, 0.5, kNow);
        QCOMPARE(v.widthMs(), kUiZoomMinMs);
        v.zoomBy(1e-9, 0.5, kNow);
        QCOMPARE(v.widthMs(), kUiZoomMaxMs);
        v.zoomBy(0.0, 0.5, kNow);               // nonsense is ignored
        v.zoomBy(-2.0, 0.5, kNow);
        QCOMPARE(v.widthMs(), kUiZoomMaxMs);
    }
    void zoomingAHistoryViewKeepsTheTimeUnderTheFingers()
    {
        PlotView v;
        v.panBy(-5 * kHour, kNow);
        const qint64 anchorBefore = v.fromMs(kNow) + qint64(0.25 * double(v.widthMs()));
        v.zoomBy(2.0, 0.25, kNow);
        const qint64 anchorAfter = v.fromMs(kNow) + qint64(0.25 * double(v.widthMs()));
        QVERIFY(std::llabs(anchorAfter - anchorBefore) < 2);
        QVERIFY(v.isGrey());
    }
    // a tap on the value reading box returns to the current reading and resets the zoom.
    void returningToNowRestoresLiveAndTwelveHours()
    {
        PlotView v;
        v.panBy(-5 * kHour, kNow);
        v.zoomBy(3.0, 0.5, kNow);
        QVERIFY(v.canReturnToNow());
        v.backToNow();
        QVERIFY(v.isLive());
        QVERIFY(!v.isGrey());
        QVERIFY(!v.canReturnToNow());
        QCOMPARE(v.widthMs(), kUiPlotWindowMs);
    }

    // ---- reduction ---------------------------------------------------------------------------------
    void smallSeriesIsUnchanged()
    {
        const QVector<PlotPoint> p = feed(kNow - 12 * kHour, kNow, kMinute);        // 721 points
        const QVector<PlotPoint> r = reduceSeries(p, kNow - 12 * kHour, kNow, 540, kUiPointsPerPx);
        QCOMPARE(r.size(), p.size());
    }
    void longSeriesIsBoundedAndKeepsPeaksAndLows()
    {
        QVector<PlotPoint> p = feed(kNow - 14 * 24 * kHour, kNow, kMinute);         // about 20,000
        p[10000].mgdl = 399.0;                                                      // a peak
        p[15000].mgdl = 41.0;                                                       // a dip
        const QVector<PlotPoint> r = reduceSeries(p, kNow - 14 * 24 * kHour, kNow, 540, kUiPointsPerPx);
        QVERIFY(r.size() <= 540 * kUiPointsPerPx);
        bool peak = false, dip = false;
        for (int i = 0; i < r.size(); ++i) {
            peak = peak || r.at(i).mgdl == 399.0;
            dip = dip || r.at(i).mgdl == 41.0;
            if (i > 0) QVERIFY(r.at(i).tMs > r.at(i - 1).tMs);                       // time order
        }
        QVERIFY(peak);
        QVERIFY(dip);
    }
    void everyReducedPointIsARealPoint()
    {
        const QVector<PlotPoint> p = feed(kNow - 5 * 24 * kHour, kNow, kMinute, 100);
        const QVector<PlotPoint> r = reduceSeries(p, kNow - 5 * 24 * kHour, kNow, 300, 2);
        QSet<qint64> times;
        for (int i = 0; i < p.size(); ++i) times.insert(p.at(i).tMs);
        for (int i = 0; i < r.size(); ++i) QVERIFY(times.contains(r.at(i).tMs));
    }
    void pointsOutsideTheWindowAreDropped()
    {
        const QVector<PlotPoint> p = feed(kNow - 10 * kHour, kNow, kMinute);
        const QVector<PlotPoint> r = reduceSeries(p, kNow - kHour, kNow, 540, 2);
        QCOMPARE(r.size(), 61);
    }

    // ---- geometry ------------------------------------------------------------------------------------
    void theScaleIsFixedAndClips()
    {
        PlotView v;
        PlotInput in = input(v);
        in.current << PlotPoint(kNow - kMinute, 120) << PlotPoint(kNow, 350) << PlotPoint(kNow - 2 * kMinute, 30);
        std::sort(in.current.begin(), in.current.end(), [](const PlotPoint &a, const PlotPoint &b){ return a.tMs < b.tMs; });
        const PlotGeometry g = computePlotGeometry(in, false);
        QVERIFY(!g.current.isEmpty());
        for (int s = 0; s < g.current.size(); ++s)
            for (int i = 0; i < g.current[s].points.size(); ++i) {
                QVERIFY(g.current[s].points[i].y() >= 0.0);
                QVERIFY(g.current[s].points[i].y() <= double(in.heightPx));
            }
        QCOMPARE(g.offScaleHigh.size(), 1);
        QCOMPARE(g.offScaleLow.size(), 1);
        // The same reading has the same height at any zoom: only x changes.
        PlotView z;
        z.zoomBy(8.0, 0.5, kNow);
        PlotInput in2 = input(z);
        in2.current << PlotPoint(kNow - kMinute, 120);
        PlotInput in3 = input(v);
        in3.current << PlotPoint(kNow - kMinute, 120);
        QCOMPARE(computePlotGeometry(in2, false).current[0].points[0].y(),
                 computePlotGeometry(in3, false).current[0].points[0].y());
    }
    void aLineBreaksAtAGap()
    {
        PlotView v;
        PlotInput in = input(v);
        in.current = feed(kNow - 2 * kHour, kNow - kHour, kMinute);
        in.current += feed(kNow - 30 * kMinute, kNow, kMinute);                     // 30 min hole
        const PlotGeometry g = computePlotGeometry(in, false);
        QCOMPARE(g.current.size(), 2);
    }
    void historyFeedHasItsOwnSpacingForBreaks()
    {
        PlotView v;
        PlotInput in = input(v);
        in.history = feed(kNow - 3 * kHour, kNow, 5 * kMinute);                      // no break at 5 min
        QCOMPARE(computePlotGeometry(in, false).history.size(), 1);
    }
    void ticksAtLocalFullHoursWithTheLadder()
    {
        PlotView v;
        PlotInput in = input(v);
        in.utcOffsetMs = 2 * kHour;
        const PlotGeometry g = computePlotGeometry(in, false);
        QCOMPARE(g.tickStepMs, 3 * kHour);                                           // 12 h over 540 px
        QVERIFY(g.ticks.size() >= 4);
        for (int i = 0; i < g.ticks.size(); ++i)
            QCOMPARE((g.ticks[i].tMs + in.utcOffsetMs) % (3 * kHour), qint64(0));
        QCOMPARE(ladderStepMs(1.0, 64), 5 * kMinute);                                // 1 ms per px: smallest step
        QCOMPARE(ladderStepMs(1e12, 64), 7 * 24 * kHour);                            // the largest
    }
    void theTopLabelsAreNeverFinerThanAnHour()
    {
        PlotView v;
        v.zoomBy(1e6, 0.5, kNow);                                    // the closest zoom, 30 minutes
        PlotInput in = input(v);
        in.utcOffsetMs = 2 * kHour;
        const PlotGeometry g = computePlotGeometry(in, false);
        QVERIFY(g.tickStepMs >= kHour);
        for (int i = 0; i < g.ticks.size(); ++i)
            QCOMPARE((g.ticks[i].tMs + in.utcOffsetMs) % kHour, qint64(0));     // whole hours only
        QVERIFY(g.ticks.size() <= 1);                                // at most one in 30 minutes
    }
    void markersAreRealSamplesAtLeastApartAndNotInAGap()
    {
        PlotView v;
        PlotInput in = input(v);
        in.current = feed(kNow - 12 * kHour, kNow - 6 * kHour, kMinute);
        in.current += feed(kNow - 3 * kHour, kNow, kMinute);
        const PlotGeometry g = computePlotGeometry(in, false);
        QVERIFY(g.markerStepMs >= kUiMarkerMinPx * double(in.toMs - in.fromMs) / in.widthPx - 1);
        QVERIFY(!g.markers.isEmpty());
        for (int i = 1; i < g.markers.size(); ++i)
            if (!g.markers[i].feedHistory && !g.markers[i - 1].feedHistory)
                QVERIFY(g.markers[i].pos.x() - g.markers[i - 1].pos.x() >= kUiMarkerMinPx - 1.0);
        // None sits in the empty stretch between kNow-6h and kNow-3h.
        const double x6 = double(kNow - 6 * kHour - in.fromMs) / (double(in.toMs - in.fromMs) / in.widthPx);
        const double x3 = double(kNow - 3 * kHour - in.fromMs) / (double(in.toMs - in.fromMs) / in.widthPx);
        for (int i = 0; i < g.markers.size(); ++i)
            QVERIFY(g.markers[i].pos.x() <= x6 + 1.0 || g.markers[i].pos.x() >= x3 - 1.0);
    }
    void zonesCoverTheHeightAndStepInOpacity()
    {
        PlotView v;
        const PlotGeometry g = computePlotGeometry(input(v), false);
        QCOMPARE(g.zones.size(), 5);
        QCOMPARE(g.zones.first().top, 0.0);
        QCOMPARE(g.zones.last().bottom, 400.0);
        for (int i = 1; i < g.zones.size(); ++i)
            QCOMPARE(g.zones[i].top, g.zones[i - 1].bottom);                         // no hole, no overlap
        // Opacity is strongest at the ends and quietest in the middle (monotonic outwards).
        QVERIFY(g.zones[2].alpha < g.zones[1].alpha && g.zones[1].alpha < g.zones[0].alpha);
        QVERIFY(g.zones[2].alpha < g.zones[3].alpha && g.zones[3].alpha < g.zones[4].alpha);
        QCOMPARE(g.zones[0].alpha, g.zones[4].alpha);
        QVERIFY(!g.zones[0].grey);
        QVERIFY(computePlotGeometry(input(v), true).zones[2].grey);
    }
    void thresholdLinesAreSolidForUrgentAndDottedOtherwise()
    {
        PlotView v;
        const PlotGeometry g = computePlotGeometry(input(v), false);
        QCOMPARE(g.lines.size(), 4);                // HH, H, L, LL from the top
        QVERIFY(g.lines[0].solid);
        QVERIFY(!g.lines[1].solid);
        QVERIFY(!g.lines[2].solid);
        QVERIFY(g.lines[3].solid);
        QVERIFY(g.lines[0].y < g.lines[1].y && g.lines[1].y < g.lines[2].y && g.lines[2].y < g.lines[3].y);
    }
    void serviceGapsAreBandsInsideTheView()
    {
        PlotView v;
        PlotInput in = input(v);
        in.serviceGaps << qMakePair(kNow - 6 * kHour, kNow - 5 * kHour)
                       << qMakePair(kNow - 30 * kHour, kNow - 20 * kHour)                // outside
                       << qMakePair(kNow - 13 * kHour, kNow - 11 * kHour);               // clipped
        const PlotGeometry g = computePlotGeometry(in, false);
        int n = 0;
        for (int i = 0; i < g.bands.size(); ++i)
            if (g.bands[i].kind == PlotBand::ServiceGap) { ++n; QVERIFY(g.bands[i].x0 >= 0.0); }
        QCOMPARE(n, 2);
    }
    // the plot in a gap. The connection to the source was lost for 40 minutes while the service
    // kept running; afterwards the source delivered the graph of that stretch. Proved: the feeds stay
    // separate, so the graph (history) line runs THROUGH the gap in one piece while the current line
    // breaks there; no shading is drawn for the absence of one feed's data (the subject is fresh
    // again); the only shading is the service's own gap from the journal, exactly at its times.
    void aLostConnectionKeepsTheGraphLineAndShadesOnlyTheJournalGap()
    {
        PlotView v;
        PlotInput in = input(v);
        const qint64 lostAt = kNow - 3 * kHour, backAt = lostAt + 40 * kMinute;
        in.current = feed(kNow - 6 * kHour, lostAt, kMinute) + feed(backAt, kNow, kMinute);
        in.history = feed(kNow - 6 * kHour, kNow - 20 * kMinute, 5 * kMinute);   // lags, no hole
        PlotGeometry g = computePlotGeometry(in, false);
        QCOMPARE(g.current.size(), 2);              // the current line breaks at the lost stretch
        QCOMPARE(g.history.size(), 1);              // the graph line is drawn through it
        QVERIFY(g.bands.isEmpty());                 // no shading for missing data of one feed
        // A service gap of the journal (monitoring not running) elsewhere: shaded exactly there.
        in.serviceGaps << qMakePair(kNow - 9 * kHour, kNow - 8 * kHour);
        g = computePlotGeometry(in, false);
        QCOMPARE(g.bands.size(), 1);
        QCOMPARE(g.bands[0].kind, PlotBand::ServiceGap);
        const double msPerPx = double(in.toMs - in.fromMs) / in.widthPx;
        QCOMPARE(g.bands[0].x0, double(kNow - 9 * kHour - in.fromMs) / msPerPx);
        QCOMPARE(g.bands[0].x1, double(kNow - 8 * kHour - in.fromMs) / msPerPx);
        QCOMPARE(g.history.size(), 1);              // the shading does not cut the graph line
    }
    // a break longer than kUiGapLabelS (20 min) is labelled "no data"; a shorter
    // break only breaks the line. The label spans the break, inside the window.
    void onlyABreakLongerThanTwentyMinutesIsLabelled()
    {
        PlotView v;
        PlotInput in = input(v);
        in.current = feed(kNow - 6 * kHour, kNow - 4 * kHour, kMinute)
                   + feed(kNow - 4 * kHour + 10 * kMinute, kNow - 2 * kHour, kMinute)     // 10 min: no label
                   + feed(kNow - 2 * kHour + 40 * kMinute, kNow, kMinute);                // 40 min: label
        const PlotGeometry g = computePlotGeometry(in, false);
        QCOMPARE(g.current.size(), 3);                  // both breaks break the line
        QCOMPARE(g.longBreaks.size(), 1);
        const double msPerPx = double(in.toMs - in.fromMs) / in.widthPx;
        QCOMPARE(g.longBreaks[0].x0, double(kNow - 2 * kHour - in.fromMs) / msPerPx);
        QCOMPARE(g.longBreaks[0].x1, double(kNow - 2 * kHour + 40 * kMinute - in.fromMs) / msPerPx);
        QVERIFY(kUiGapLabelS == 1200);
    }
    // a reading off the fixed scale is clipped to the edge and its chevron sits
    // at that reading's time; the newest reading's badge goes where the newest point is.
    void theChevronAndTheNewestBadgeSitWhereTheirReadingsAre()
    {
        PlotView v;
        PlotInput in = input(v);
        in.current = feed(kNow - 2 * kHour, kNow, kMinute);
        in.current[30].mgdl = 350.0;                    // above the scale
        in.current[60].mgdl = 30.0;                     // below it
        const PlotGeometry g = computePlotGeometry(in, false);
        const double msPerPx = double(in.toMs - in.fromMs) / in.widthPx;
        QCOMPARE(g.offScaleHigh.size(), 1);
        QCOMPARE(g.offScaleLow.size(), 1);
        QCOMPARE(g.offScaleHigh[0].x(), double(in.current[30].tMs - in.fromMs) / msPerPx);
        QVERIFY(qAbs(g.offScaleHigh[0].y()) < 1e-9);                     // clipped to the top
        QVERIFY(qAbs(g.offScaleLow[0].y() - double(in.heightPx)) < 1e-9); // and to the bottom
        QVERIFY(g.hasRing && !g.ringHollow);
        QCOMPARE(g.ring.x(), double(in.current.last().tMs - in.fromMs) / msPerPx);
    }
    // While the source is still unreachable (stale now) the "no new reading" band starts at the newest
    // CURRENT reading and covers no graph reading that exists (the graph lags the current feed).
    void theNoNewReadingBandStartsAtTheNewestCurrentReading()
    {
        PlotView v;
        PlotInput in = input(v);
        in.current = feed(kNow - 6 * kHour, kNow - 30 * kMinute, kMinute);
        in.history = feed(kNow - 6 * kHour, kNow - 45 * kMinute, 5 * kMinute);
        in.stale = true;
        const PlotGeometry g = computePlotGeometry(in, true);
        QCOMPARE(g.bands.size(), 1);
        QCOMPARE(g.bands[0].kind, PlotBand::NoNewReading);
        const double msPerPx = double(in.toMs - in.fromMs) / in.widthPx;
        QCOMPARE(g.bands[0].x0, double(in.current.last().tMs - in.fromMs) / msPerPx);
        QVERIFY(double(in.history.last().tMs - in.fromMs) / msPerPx < g.bands[0].x0);
    }
    void staleShowsNoNewReadingAndAHollowRing()
    {
        PlotView v;
        PlotInput in = input(v);
        in.current = feed(kNow - 3 * kHour, kNow - 20 * kMinute, kMinute);
        in.stale = true;
        PlotGeometry g = computePlotGeometry(in, true);
        QVERIFY(g.hasRing);
        QVERIFY(g.ringHollow);
        bool band = false;
        for (int i = 0; i < g.bands.size(); ++i) band = band || g.bands[i].kind == PlotBand::NoNewReading;
        QVERIFY(band);
        in.stale = false;
        g = computePlotGeometry(in, false);
        QVERIFY(!g.ringHollow);
    }
    void stoppedServiceShowsAStoppedBandInsteadOfNoNewReading()
    {
        PlotView v;
        PlotInput in = input(v);
        in.current = feed(kNow - 3 * kHour, kNow - 20 * kMinute, kMinute);
        in.stale = true;
        in.stoppedSinceMs = kNow - 25 * kMinute;
        const PlotGeometry g = computePlotGeometry(in, true);
        QCOMPARE(g.bands.size(), 1);
        QCOMPARE(g.bands[0].kind, PlotBand::ServiceStopped);
    }
    void aHistoryViewHasNoRingAndNoBandsAtNow()
    {
        PlotView v;
        v.panBy(-5 * kHour, kNow);
        PlotInput in = input(v);
        in.current = feed(kNow - 30 * kHour, kNow, kMinute);
        in.stale = true;
        const PlotGeometry g = computePlotGeometry(in, true);
        QVERIFY(!g.hasRing);                         // the newest reading is outside the window
        for (int i = 0; i < g.bands.size(); ++i)
            QVERIFY(g.bands[i].kind != PlotBand::NoNewReading);   // now is outside the window
    }
    // ---- day lines, cursor, extremes -----------------------------------
    void dayStartsAreLocalMidnightsInsideTheWindow()
    {
        PlotView v;
        v.zoomBy(1.0 / 4.0, 0.5, kNow);                              // 48 h window, live
        PlotInput in = input(v);
        in.utcOffsetMs = 2 * kHour;
        const PlotGeometry g = computePlotGeometry(in, false);
        QVERIFY(g.dayStarts.size() >= 2 && g.dayStarts.size() <= 3);
        for (int i = 0; i < g.dayStarts.size(); ++i) {
            QCOMPARE((g.dayStarts[i].tMs + in.utcOffsetMs) % (24 * kHour), qint64(0));
            QVERIFY(g.dayStarts[i].tMs >= in.fromMs && g.dayStarts[i].tMs <= in.toMs);
            QVERIFY(g.dayStarts[i].x >= 0.0 && g.dayStarts[i].x <= in.widthPx);
        }
        // A 30 minute window has at most one, usually none.
        PlotView z;
        z.zoomBy(1e6, 0.5, kNow);
        QVERIFY(computePlotGeometry(input(z), false).dayStarts.size() <= 1);
    }
    void theCursorFindsTheNearestRealReadingsAndNotAcrossAGap()
    {
        PlotView v;
        PlotInput in = input(v);
        in.current = feed(kNow - 3 * kHour, kNow - 2 * kHour, kMinute);
        in.history = feed(kNow - 3 * kHour, kNow, 5 * kMinute);
        in.cursorMs = kNow - 2 * kHour - 30 * 1000;                  // between two current readings
        PlotGeometry g = computePlotGeometry(in, false);
        QVERIFY(g.cursor.valid);
        QVERIFY(g.cursor.hasCurrent && g.cursor.hasHistory);
        QVERIFY(std::llabs(g.cursor.currentAtMs - in.cursorMs) <= kMinute);
        QVERIFY(g.cursor.x >= 0.0 && g.cursor.x <= in.widthPx);
        // Far from any current reading (the feed ended an hour ago): none for it, history still.
        in.cursorMs = kNow - 30 * kMinute;
        g = computePlotGeometry(in, false);
        QVERIFY(g.cursor.valid && !g.cursor.hasCurrent && g.cursor.hasHistory);
        in.cursorMs = -1;
        QVERIFY(!computePlotGeometry(in, false).cursor.valid);
        in.cursorMs = kNow + kHour;                                  // outside the window
        QVERIFY(!computePlotGeometry(in, false).cursor.valid);
    }
    void nearestWithinRespectsItsLimit()
    {
        const QVector<PlotPoint> p = feed(kNow - kHour, kNow, kMinute);
        PlotPoint out;
        QVERIFY(nearestWithin(p, kNow - 10 * kMinute + 20 * 1000, 30 * 1000, &out));
        QCOMPARE(out.tMs, kNow - 10 * kMinute);
        QVERIFY(!nearestWithin(p, kNow + 2 * kHour, 30 * 1000, &out));
        QVERIFY(!nearestWithin(QVector<PlotPoint>(), kNow, 1000000, &out));
    }
    void extremesCoverEveryReadingOfAllFeedsInTheWindow()
    {
        QVector<PlotPoint> a = feed(kNow - 5 * kHour, kNow, kMinute, 100);
        a[100].mgdl = 61.0;                                         // the low, at a[100]
        QVector<PlotPoint> b = feed(kNow - 5 * kHour, kNow, 5 * kMinute, 100);
        b[10].mgdl = 205.0;                                         // the high, in the other feed
        const PlotExtremes e = extremesOf(a, b, kNow - 5 * kHour, kNow);
        QVERIFY(e.has);
        QCOMPARE(e.minMgdl, 61.0);
        QCOMPARE(e.minAtMs, a[100].tMs);
        QCOMPARE(e.maxMgdl, 205.0);
        QCOMPARE(e.maxAtMs, b[10].tMs);
        // Narrower window: the extremes outside it do not count.
        const PlotExtremes n = extremesOf(a, b, kNow - kHour, kNow);
        QVERIFY(n.has && n.minMgdl > 61.0 && n.maxMgdl < 205.0);
        QVERIFY(!extremesOf(QVector<PlotPoint>(), QVector<PlotPoint>(), 0, kNow).has);
    }
    void theExtremesAreMarkedOnTheCurve()
    {
        PlotView v;
        PlotInput in = input(v);
        in.current = feed(kNow - 5 * kHour, kNow, kMinute, 100);
        in.current[100].mgdl = 61.0;
        in.current[200].mgdl = 190.0;
        const PlotGeometry g = computePlotGeometry(in, false);
        QVERIFY(g.extremeMarks.has);
        QVERIFY(g.extremeMarks.low.y() > g.extremeMarks.high.y());           // the low is lower on the screen
        const double msPerPx = double(in.toMs - in.fromMs) / in.widthPx;
        QCOMPARE(g.extremeMarks.low.x(), double(in.current[100].tMs - in.fromMs) / msPerPx);
        QCOMPARE(g.extremeMarks.high.x(), double(in.current[200].tMs - in.fromMs) / msPerPx);
        QVERIFY(!computePlotGeometry(input(v), false).extremeMarks.has);     // no data, no marks
    }
    void emptyOrDegenerateInputGivesAnEmptyDrawing()
    {
        PlotInput in;
        QVERIFY(computePlotGeometry(in, false).zones.isEmpty());
        PlotView v;
        PlotInput e = input(v);
        const PlotGeometry g = computePlotGeometry(e, false);
        QVERIFY(g.current.isEmpty() && g.history.isEmpty() && g.markers.isEmpty() && !g.hasRing);
        QCOMPARE(g.zones.size(), 5);
    }
};

QTEST_APPLESS_MAIN(TstUiPlot)
#include "tst_uiplot.moc"
