// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Michele Brami
#include "plotgeometry.h"

#include "uiconstants.h"

#include <algorithm>
#include <cmath>

namespace lm {
namespace ui {

namespace {

const qint64 kMin = 60LL * 1000;
const qint64 kHour = 60 * kMin;
const qint64 kDay = 24 * kHour;

// Nominal spacing of the feeds: a line breaks after kUiGapFactor of them (REQ-SCH-12 defaults).
const qint64 kCurrentSpacingMs = 60LL * 1000;
const qint64 kHistorySpacingMs = 300LL * 1000;

struct Mapper
{
    Mapper(const PlotInput &in)
        : from(in.fromMs), msPerPx(double(in.toMs - in.fromMs) / std::max(1, in.widthPx)),
          h(in.heightPx) {}
    double x(qint64 t) const { return double(t - from) / msPerPx; }
    // The scale is fixed; a value outside is clipped to the edge.
    double y(double mgdl) const
    {
        const double v = std::max(kUiPlotYMinMgDl, std::min(kUiPlotYMaxMgDl, mgdl));
        return double(h) - (v - kUiPlotYMinMgDl) / (kUiPlotYMaxMgDl - kUiPlotYMinMgDl) * double(h);
    }
    qint64 from;
    double msPerPx;
    int h;
};

// Splits the reduced points into unbroken polylines.
QVector<PlotSegment> segments(const QVector<PlotPoint> &pts, qint64 spacingMs, const Mapper &m,
                              QVector<QPointF> *high, QVector<QPointF> *low)
{
    QVector<PlotSegment> out;
    PlotSegment cur;
    qint64 prev = 0;
    for (int i = 0; i < pts.size(); ++i) {
        const PlotPoint &p = pts.at(i);
        if (!cur.points.isEmpty() && p.tMs - prev > spacingMs * kUiGapFactor) {
            out.append(cur);
            cur = PlotSegment();
        }
        const QPointF pos(m.x(p.tMs), m.y(p.mgdl));
        cur.points.append(pos);
        if (p.mgdl > kUiPlotYMaxMgDl) high->append(pos);
        if (p.mgdl < kUiPlotYMinMgDl) low->append(pos);
        prev = p.tMs;
    }
    if (!cur.points.isEmpty())
        out.append(cur);
    return out;
}

// The point of `pts` nearest in time to `t`; -1 when empty.
int nearest(const QVector<PlotPoint> &pts, qint64 t)
{
    if (pts.isEmpty()) return -1;
    int lo = 0, hi = pts.size() - 1;
    while (lo < hi) {
        const int mid = (lo + hi) / 2;
        if (pts.at(mid).tMs < t) lo = mid + 1; else hi = mid;
    }
    // `lo` is the first point at or after t; the earlier neighbour may be nearer (ties: earlier).
    if (lo > 0 && t - pts.at(lo - 1).tMs <= pts.at(lo).tMs - t)
        --lo;
    return lo;
}

} // namespace

qint64 ladderStepMs(double msPerPx, int minPx)
{
    static const qint64 steps[] = { 5 * kMin, 15 * kMin, kHour, 3 * kHour, 6 * kHour, 12 * kHour,
                                    kDay, 7 * kDay };
    for (unsigned i = 0; i < sizeof(steps) / sizeof(steps[0]); ++i)
        if (double(steps[i]) / msPerPx >= double(minPx))
            return steps[i];
    return 7 * kDay;
}

bool nearestWithin(const QVector<PlotPoint> &pts, qint64 tMs, qint64 maxDistMs, PlotPoint *out)
{
    const int idx = nearest(pts, tMs);
    if (idx < 0 || std::llabs(pts.at(idx).tMs - tMs) > maxDistMs)
        return false;
    *out = pts.at(idx);
    return true;
}

PlotExtremes extremesOf(const QVector<PlotPoint> &a, const QVector<PlotPoint> &b, qint64 fromMs, qint64 toMs)
{
    PlotExtremes e;
    const QVector<PlotPoint> *all[2] = { &a, &b };
    for (int s = 0; s < 2; ++s)
        for (int i = 0; i < all[s]->size(); ++i) {
            const PlotPoint &p = all[s]->at(i);
            if (p.tMs < fromMs || p.tMs > toMs) continue;
            if (!e.has) { e.has = true; e.minMgdl = e.maxMgdl = p.mgdl; e.minAtMs = e.maxAtMs = p.tMs; continue; }
            if (p.mgdl < e.minMgdl) { e.minMgdl = p.mgdl; e.minAtMs = p.tMs; }
            if (p.mgdl > e.maxMgdl) { e.maxMgdl = p.mgdl; e.maxAtMs = p.tMs; }
        }
    return e;
}

PlotGeometry computePlotGeometry(const PlotInput &in, bool greyZones)
{
    PlotGeometry g;
    if (in.widthPx <= 0 || in.heightPx <= 0 || in.toMs <= in.fromMs)
        return g;
    const Mapper m(in);

    // ---- zones and threshold lines (4.1) -------------------------------------------------------
    const PlotThresholds &t = in.thresholds;
    const double edges[] = { kUiPlotYMaxMgDl, t.hh, t.h, t.l, t.ll, kUiPlotYMinMgDl };
    const PlotZone::Band bands[] = { PlotZone::AboveHH, PlotZone::HtoHH, PlotZone::Normal,
                                     PlotZone::LLtoL, PlotZone::BelowLL };
    const double alphas[] = { kUiZoneAlphaUrgent, kUiZoneAlphaOuter, kUiZoneAlphaNormal,
                              kUiZoneAlphaOuter, kUiZoneAlphaUrgent };
    for (int i = 0; i < 5; ++i) {
        PlotZone z;
        z.band = bands[i];
        z.top = m.y(edges[i]);
        z.bottom = m.y(edges[i + 1]);
        z.alpha = alphas[i];
        z.grey = greyZones;
        g.zones.append(z);
    }
    PlotLine l;
    l.y = m.y(t.hh); l.solid = true;  g.lines.append(l);
    l.y = m.y(t.h);  l.solid = false; g.lines.append(l);
    l.y = m.y(t.l);  l.solid = false; g.lines.append(l);
    l.y = m.y(t.ll); l.solid = true;  g.lines.append(l);

    // ---- series (4.1, 4.3, 4.7) -----------------------------------------------------------------
    const QVector<PlotPoint> cur = reduceSeries(in.current, in.fromMs, in.toMs, in.widthPx, kUiPointsPerPx);
    const QVector<PlotPoint> his = reduceSeries(in.history, in.fromMs, in.toMs, in.widthPx, kUiPointsPerPx);
    g.current = segments(cur, kCurrentSpacingMs, m, &g.offScaleHigh, &g.offScaleLow);
    g.history = segments(his, kHistorySpacingMs, m, &g.offScaleHigh, &g.offScaleLow);

    // ---- markers at a regular time step, on the real sample nearest to it ---------------------
    // (the full series, not the reduced one, so a marker is always a stored reading)
    g.markerStepMs = ladderStepMs(m.msPerPx, kUiMarkerMinPx);
    const QVector<PlotPoint> *feeds[2] = { &in.current, &in.history };
    for (int f = 0; f < 2; ++f) {
        const QVector<PlotPoint> &pts = *feeds[f];
        if (pts.isEmpty()) continue;
        const qint64 first = ((in.fromMs + in.utcOffsetMs) / g.markerStepMs) * g.markerStepMs - in.utcOffsetMs;
        for (qint64 s = first; s <= in.toMs; s += g.markerStepMs) {
            if (s < in.fromMs) continue;
            const int idx = nearest(pts, s);
            if (idx < 0) continue;
            const PlotPoint &p = pts.at(idx);
            // Only where the sample really is near the step: no marker in a gap.
            const qint64 maxDist = (f == 0 ? kCurrentSpacingMs : kHistorySpacingMs) * kUiGapFactor;
            if (std::llabs(p.tMs - s) > maxDist || p.tMs < in.fromMs || p.tMs > in.toMs) continue;
            PlotMarker mk;
            mk.pos = QPointF(m.x(p.tMs), m.y(p.mgdl));
            mk.feedHistory = f == 1;
            g.markers.append(mk);
        }
    }

    // ---- time ticks along the top (4.2) ----------------------------------------------------------
    // The labels along the top are whole hours or days, never minutes: the step is
    // never finer than an hour, so a zoomed view has fewer labels, and the window's start and end tell
    // where it is.
    g.tickStepMs = std::max(ladderStepMs(m.msPerPx, kUiTickMinPx), kHour);
    const qint64 firstTick = ((in.fromMs + in.utcOffsetMs) / g.tickStepMs) * g.tickStepMs - in.utcOffsetMs;
    for (qint64 s = firstTick; s <= in.toMs; s += g.tickStepMs) {
        if (s < in.fromMs) continue;
        PlotTick tk;
        tk.tMs = s;
        tk.x = m.x(s);
        tk.midnight = ((s + in.utcOffsetMs) % kDay) == 0;
        g.ticks.append(tk);
    }

    // ---- bands: service gaps, a stopped service, no new reading (4.3) ----------------------------
    for (int i = 0; i < in.serviceGaps.size(); ++i) {
        const qint64 a = std::max(in.serviceGaps.at(i).first, in.fromMs);
        const qint64 b = std::min(in.serviceGaps.at(i).second, in.toMs);
        if (b <= a) continue;
        PlotBand band;
        band.kind = PlotBand::ServiceGap;
        band.x0 = m.x(a); band.x1 = m.x(b);
        if (band.x1 - band.x0 >= 1.0)       // a gap shorter than a pixel is not drawn
            g.bands.append(band);
    }
    const bool nowInView = in.nowMs >= in.fromMs && in.nowMs <= in.toMs;
    if (in.stoppedSinceMs >= 0 && nowInView) {
        PlotBand band;
        band.kind = PlotBand::ServiceStopped;
        band.x0 = m.x(std::max(in.stoppedSinceMs, in.fromMs)); band.x1 = m.x(in.nowMs);
        g.bands.append(band);
    } else if (in.stale && nowInView && !in.current.isEmpty()) {
        const qint64 last = in.current.last().tMs;
        if (last < in.nowMs) {
            PlotBand band;
            band.kind = PlotBand::NoNewReading;
            band.x0 = m.x(std::max(last, in.fromMs)); band.x1 = m.x(in.nowMs);
            g.bands.append(band);
        }
    }

    // ---- long breaks, labelled "no data" -----------------------------------
    // From the feed that is shown, the current one first (one label per break, not one per feed). A
    // break counts where two consecutive stored readings are further apart than kUiGapLabelS; only the
    // part inside the window is kept, and a break narrower than a pixel is not labelled.
    {
        const QVector<PlotPoint> &pts = !in.current.isEmpty() ? in.current : in.history;
        for (int i = 1; i < pts.size(); ++i) {
            const qint64 a = pts.at(i - 1).tMs, b = pts.at(i).tMs;
            if (b - a <= qint64(kUiGapLabelS) * 1000 || b <= in.fromMs || a >= in.toMs)
                continue;
            PlotBreak br;
            br.x0 = m.x(std::max(a, in.fromMs));
            br.x1 = m.x(std::min(b, in.toMs));
            if (br.x1 - br.x0 >= 1.0)
                g.longBreaks.append(br);
        }
    }

    // ---- the lowest and the highest reading of the window, marked ------------------------
    {
        const PlotExtremes e = extremesOf(in.current, in.history, in.fromMs, in.toMs);
        if (e.has) {
            g.extremeMarks.has = true;
            g.extremeMarks.low = QPointF(m.x(e.minAtMs), m.y(e.minMgdl));
            g.extremeMarks.high = QPointF(m.x(e.maxAtMs), m.y(e.maxMgdl));
        }
    }

    // ---- the start of each local day: a vertical line --------------------------------
    {
        const qint64 firstMidnight = ((in.fromMs + in.utcOffsetMs + kDay - 1) / kDay) * kDay - in.utcOffsetMs;
        for (qint64 d = firstMidnight; d <= in.toMs; d += kDay) {
            if (d < in.fromMs) continue;
            PlotDay pd;
            pd.tMs = d;
            pd.x = m.x(d);
            g.dayStarts.append(pd);
        }
    }

    // ---- the value-tracking cursor ---------------------------------------------------------------------
    if (in.cursorMs >= in.fromMs && in.cursorMs <= in.toMs) {
        g.cursor.valid = true;
        g.cursor.tMs = in.cursorMs;
        g.cursor.x = m.x(in.cursorMs);
        const QVector<PlotPoint> *feeds2[2] = { &in.current, &in.history };
        const qint64 spacing[2] = { kCurrentSpacingMs, kHistorySpacingMs };
        for (int f = 0; f < 2; ++f) {
            const int idx = nearest(*feeds2[f], in.cursorMs);
            if (idx < 0) continue;
            const PlotPoint &p = feeds2[f]->at(idx);
            // Not inside a gap: the reading must be within a few nominal spacings of the finger.
            if (std::llabs(p.tMs - in.cursorMs) > spacing[f] * kUiGapFactor) continue;
            const QPointF pos(m.x(p.tMs), m.y(p.mgdl));
            if (f == 0) { g.cursor.hasCurrent = true; g.cursor.current = pos; g.cursor.currentAtMs = p.tMs; g.cursor.currentMgdl = p.mgdl; }
            else { g.cursor.hasHistory = true; g.cursor.history = pos; g.cursor.historyAtMs = p.tMs; g.cursor.historyMgdl = p.mgdl; }
        }
    }

    // ---- the ring on the newest current reading, only if it is in view ---------------------------
    if (!in.current.isEmpty()) {
        const PlotPoint &p = in.current.last();
        if (p.tMs >= in.fromMs && p.tMs <= in.toMs) {
            g.hasRing = true;
            g.ring = QPointF(m.x(p.tMs), m.y(p.mgdl));
            g.ringHollow = in.stale;
        }
    }
    return g;
}

} // namespace ui
} // namespace lm
