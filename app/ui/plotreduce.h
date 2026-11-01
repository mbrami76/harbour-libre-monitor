// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Michele Brami
#ifndef LM_UI_PLOTREDUCE_H
#define LM_UI_PLOTREDUCE_H

// Reduces a series to what the screen can show.
//
// Why: a 14 day view of the one-a-minute feed is 20,000 points on a screen about 540 pixels
// wide. Drawing and keeping them all costs memory for nothing. The reduction keeps, per pixel
// column, the LOWEST and the HIGHEST point, so peaks and dips survive, the same idea as the
// thinning rule of the store (REQ-STO-07: minimum and maximum kept). Nothing is averaged: a
// shown point is always a real stored reading.

#include <QVector>
#include <QtGlobal>

namespace lm {
namespace ui {

struct PlotPoint
{
    PlotPoint() : tMs(0), mgdl(0) {}
    PlotPoint(qint64 t, double v) : tMs(t), mgdl(v) {}

    qint64 tMs;         // measurement time, UTC milliseconds
    double mgdl;        // the stored value converted to mg/dL (exactly; REQ-DAT-12)
};

// `points` must be sorted by time. Returns them unchanged when there are at most
// `perPixel * widthPx` of them; otherwise per pixel column of [fromMs, toMs] the minimum and
// maximum point, in time order. Points outside the window are kept out.
QVector<PlotPoint> reduceSeries(const QVector<PlotPoint> &points, qint64 fromMs, qint64 toMs,
                                int widthPx, int perPixel);

} // namespace ui
} // namespace lm

#endif
