// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Michele Brami
#include "plotview.h"

#include "uiconstants.h"

#include <algorithm>
#include <cmath>

namespace lm {
namespace ui {

PlotView::PlotView()
    : m_live(true), m_toMs(0), m_widthMs(kUiPlotWindowMs), m_firstDataMs(0)
{
}

bool PlotView::canReturnToNow() const
{
    return !m_live || m_widthMs != kUiPlotWindowMs;
}

void PlotView::clamp(qint64 nowMs)
{
    m_widthMs = std::max<qint64>(kUiZoomMinMs, std::min<qint64>(kUiZoomMaxMs, m_widthMs));
    if (m_live)
        return;
    // Never into the future.
    if (m_toMs > nowMs)
        m_toMs = nowMs;
    // Not further back than the first stored reading, with 5% of the width as margin.
    if (m_firstDataMs > 0) {
        const qint64 minFrom = m_firstDataMs - m_widthMs / 20;
        if (m_toMs - m_widthMs < minFrom)
            m_toMs = minFrom + m_widthMs;
        // The data may be younger than the window: never push the edge past now.
        if (m_toMs > nowMs)
            m_toMs = nowMs;
    }
    if (nowMs - m_toMs <= kUiLiveSnapMs)
        m_live = true;
}

void PlotView::panBy(qint64 deltaMs, qint64 nowMs)
{
    const qint64 to = toMs(nowMs) + deltaMs;
    m_live = false;
    m_toMs = to;
    clamp(nowMs);
}

void PlotView::zoomBy(double factor, double anchorFraction, qint64 nowMs)
{
    if (!(factor > 0.0) || !std::isfinite(factor))
        return;
    const qint64 oldWidth = m_widthMs;
    const qint64 oldTo = toMs(nowMs);
    qint64 width = static_cast<qint64>(std::llround(double(oldWidth) / factor));
    width = std::max<qint64>(kUiZoomMinMs, std::min<qint64>(kUiZoomMaxMs, width));
    if (m_live) {
        // A live view keeps its right edge at the current time (docs 4.7).
        m_widthMs = width;
        return;
    }
    const double f = std::max(0.0, std::min(1.0, anchorFraction));
    const qint64 anchorT = oldTo - oldWidth + static_cast<qint64>(std::llround(f * double(oldWidth)));
    m_widthMs = width;
    m_toMs = anchorT - static_cast<qint64>(std::llround(f * double(width))) + width;
    clamp(nowMs);
}

void PlotView::backToNow()
{
    m_live = true;
    m_widthMs = kUiPlotWindowMs;
}

bool PlotView::showsBeforeFirstData(qint64 nowMs) const
{
    return m_firstDataMs > 0 && fromMs(nowMs) < m_firstDataMs;
}

} // namespace ui
} // namespace lm
