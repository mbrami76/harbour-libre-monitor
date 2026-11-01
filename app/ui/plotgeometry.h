// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Michele Brami
#ifndef LM_UI_PLOTGEOMETRY_H
#define LM_UI_PLOTGEOMETRY_H

// Everything the plot draws, as numbers in pixels.
//
// Pattern: the drawing item (a QQuickPaintedItem, later) only paints what this returns. All
// decisions live here, in plain C++ that the host build can test with no screen: where each
// point goes, where a line breaks, which marker and tick steps fit, where the zones and the
// threshold lines are, which stretches are service gaps or "no new reading".
//
// Assumptions: the scale is FIXED in mg/dL (kUiPlotYMinMgDl..kUiPlotYMaxMgDl) whatever the
// display unit: the display unit only changes the labels, which the caller
// writes. Series points arrive as exact mg/dL values and sorted by time (the store returns
// them so); reduction to the screen width is done here.

#include "plotreduce.h"

#include <QPair>
#include <QPointF>
#include <QVector>

namespace lm {
namespace ui {

struct PlotThresholds
{
    PlotThresholds() : ll(3.0 * 18.0156), l(3.9 * 18.0156), h(10.0 * 18.0156), hh(13.9 * 18.0156) {}   // the global defaults of requirements 9.3, exactly, in mg/dL
    double ll, l, h, hh;        // mg/dL
};

struct PlotInput
{
    PlotInput() : fromMs(0), toMs(1), nowMs(0), widthPx(0), heightPx(0), stale(false),
                  utcOffsetMs(0), stoppedSinceMs(-1), cursorMs(-1) {}

    qint64 fromMs, toMs;        // the view window (PlotView)
    qint64 nowMs;
    int widthPx, heightPx;      // the plot area
    QVector<PlotPoint> current; // `libre.current` feed, sorted
    QVector<PlotPoint> history; // `libre.graph` feed, sorted
    QVector<QPair<qint64, qint64> > serviceGaps;    // "monitoring not running" (journal gaps)
    bool stale;                 // the Subject is stale (section 2.1): grey zones, hollow ring
    qint64 utcOffsetMs;         // local time minus UTC, for ticks at local full hours
    qint64 stoppedSinceMs;      // the service is not running since then; -1 = running
    qint64 cursorMs;            // value tracking: the time under the finger; -1 = none
    PlotThresholds thresholds;
};

struct PlotSegment { QVector<QPointF> points; };    // one unbroken polyline

struct PlotMarker
{
    PlotMarker() : feedHistory(false) {}
    QPointF pos;
    bool feedHistory;           // false: filled circle (current), true: hollow square (history)
};

struct PlotTick
{
    PlotTick() : x(0), tMs(0), midnight(false) {}
    double x;
    qint64 tMs;
    bool midnight;              // the label should carry the date
};

struct PlotZone
{
    enum Band { BelowLL, LLtoL, Normal, HtoHH, AboveHH };
    PlotZone() : band(Normal), top(0), bottom(0), alpha(0), grey(false) {}
    Band band;
    double top, bottom;         // pixel rows, top < bottom
    double alpha;               // opacity of the tint
    bool grey;                  // drawn without hue (history view or stale)
};

struct PlotLine
{
    PlotLine() : y(0), solid(false) {}
    double y;
    bool solid;                 // LL, HH: solid; L, H: dotted
};

struct PlotBand
{
    enum Kind { ServiceGap, ServiceStopped, NoNewReading };
    PlotBand() : kind(ServiceGap), x0(0), x1(0) {}
    Kind kind;
    double x0, x1;
};

// The value-tracking cursor: a vertical line at the touched time and,
// per feed, the nearest real reading if there is one close enough (not inside a gap).
struct PlotCursor
{
    PlotCursor() : valid(false), x(0), tMs(0), hasCurrent(false), currentMgdl(0), hasHistory(false),
                   historyMgdl(0) {}
    bool valid;
    double x;
    qint64 tMs;                 // the time asked for
    bool hasCurrent;
    QPointF current;            // pixel position of the nearest current reading
    qint64 currentAtMs;
    double currentMgdl;
    bool hasHistory;
    QPointF history;
    qint64 historyAtMs;
    double historyMgdl;
};

// The lowest and highest reading of the shown feeds inside the window.
struct PlotExtremes
{
    PlotExtremes() : has(false), minMgdl(0), minAtMs(0), maxMgdl(0), maxAtMs(0) {}
    bool has;
    double minMgdl; qint64 minAtMs;
    double maxMgdl; qint64 maxAtMs;
};

// Where the lowest and highest reading of the window are on the plot.
struct PlotExtremeMarks
{
    PlotExtremeMarks() : has(false) {}
    bool has;
    QPointF low, high;
};

struct PlotDay { PlotDay() : x(0), tMs(0) {} double x; qint64 tMs; };

// A break of the line longer than kUiGapLabelS: the plot writes "no data"
// under it. From the current feed when it is shown, otherwise from the history feed.
struct PlotBreak { PlotBreak() : x0(0), x1(0) {} double x0, x1; };

struct PlotGeometry
{
    PlotGeometry() : hasRing(false), ringHollow(false), tickStepMs(0), markerStepMs(0) {}

    PlotExtremeMarks extremeMarks;  // the lowest and highest reading, marked on the curve
    QVector<PlotDay> dayStarts;     // vertical lines at local midnights inside the window
    QVector<PlotBreak> longBreaks;  // breaks labelled "no data"
    PlotCursor cursor;

    QVector<PlotSegment> current, history;
    QVector<PlotMarker> markers;
    QVector<PlotTick> ticks;
    qint64 tickStepMs, markerStepMs;
    QVector<PlotZone> zones;
    QVector<PlotLine> lines;
    QVector<PlotBand> bands;
    // The newest current reading, when it is in view: the place of the round band badge, or
    // of the unknown badge when the patient is stale (`ringHollow`: that reading is not the current
    // value). The painted item does not draw it; the page lays the badge over the plot there.
    bool hasRing;
    QPointF ring;
    bool ringHollow;
    // Readings outside the fixed scale, drawn clipped at the edge with an arrow (4.2).
    QVector<QPointF> offScaleHigh, offScaleLow;
};

// `greyZones` is true for a history view and for a stale Subject (decided by the caller from
// PlotView::isGrey() and the staleness of the Subject).
PlotGeometry computePlotGeometry(const PlotInput &in, bool greyZones);

// The reading nearest in time to `tMs` if it is within `maxDistMs` (so not across a gap); false
// when there is none. Used by the cursor and by the readout of the value-tracking mode.
bool nearestWithin(const QVector<PlotPoint> &pts, qint64 tMs, qint64 maxDistMs, PlotPoint *out);

// The extremes of one or more series inside [fromMs, toMs]; every reading counts, not only the
// reduced points the screen draws, so the numbers are exact.
PlotExtremes extremesOf(const QVector<PlotPoint> &a, const QVector<PlotPoint> &b, qint64 fromMs, qint64 toMs);

// The tick and marker steps of the ladder, exposed for tests: the smallest step
// of 5 min, 15 min, 1 h, 3 h, 6 h, 12 h, 1 d, 7 d whose spacing is at least `minPx` pixels.
qint64 ladderStepMs(double msPerPx, int minPx);

} // namespace ui
} // namespace lm

#endif
