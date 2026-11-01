// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Michele Brami
#ifndef LM_UI_PLOTVIEW_H
#define LM_UI_PLOTVIEW_H

// Which stretch of time the plot shows, and what dragging and pinching do to it
//.
//
// Pattern: a small value-type state machine with no time source of its own: every call gets
// "now" as an argument, so a test can move the clock. The view is a window [to - width, to].
// In the LIVE state `to` is always "now" (it follows the clock); in HISTORY the user has
// scrolled back and `to` is fixed until the user returns. Only the plot goes grey in history;
// the big value above it never does (the caller decides, this class only reports the state).
//
// Assumptions: times are UTC milliseconds; the caller passes the measurement time of the
// Subject's first stored reading as the limit of the past (the plot says "no data before"
// beyond it).

#include <QtGlobal>

namespace lm {
namespace ui {

class PlotView
{
public:
    PlotView();

    // The measurement time of the oldest stored reading; <= 0 = unknown (no limit).
    void setFirstDataMs(qint64 ms) { m_firstDataMs = ms; }

    qint64 widthMs() const { return m_widthMs; }
    // The right edge of the window at `nowMs`.
    qint64 toMs(qint64 nowMs) const { return m_live ? nowMs : m_toMs; }
    qint64 fromMs(qint64 nowMs) const { return toMs(nowMs) - m_widthMs; }

    // The view ends at the current time (within kUiLiveSnapMs): the plot is coloured.
    bool isLive() const { return m_live; }
    // Not the normal live 12 h view (scrolled back OR zoomed): a tap on the value reading box would
    // change it. There is no play button and no icon any more.
    bool canReturnToNow() const;
    // The plot is grey: the view is not at the current time.
    bool isGrey() const { return !m_live; }

    // Moves the window by `deltaMs` (negative = back in time). A move that ends within
    // kUiLiveSnapMs of `nowMs` makes the view live again.
    void panBy(qint64 deltaMs, qint64 nowMs);
    // Multiplies the zoom by `factor` (> 1 zooms in) keeping the time at `anchorFraction`
    // (0 = left edge, 1 = right edge) where it is. A live view keeps its right edge at now.
    void zoomBy(double factor, double anchorFraction, qint64 nowMs);
    // The return to the current reading: live, 12 h, the zoom reset. The page calls it when the
    // value reading box is tapped and when a patient page opens.
    void backToNow();

    // True when the window reaches before the first stored reading, for the "no data before
    // this" text.
    bool showsBeforeFirstData(qint64 nowMs) const;

private:
    void clamp(qint64 nowMs);

    bool m_live;
    qint64 m_toMs;          // meaningful only in history
    qint64 m_widthMs;
    qint64 m_firstDataMs;
};

} // namespace ui
} // namespace lm

#endif
