// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Michele Brami
#include "plotreduce.h"

namespace lm {
namespace ui {

namespace {
// The pixel column of a time; a point exactly at the right edge belongs to the last column,
// so there are never more than widthPx columns.
qint64 columnOf(qint64 t, qint64 fromMs, double msPerPx, int widthPx)
{
    const qint64 c = static_cast<qint64>(double(t - fromMs) / msPerPx);
    return c >= widthPx ? widthPx - 1 : c;
}
}

QVector<PlotPoint> reduceSeries(const QVector<PlotPoint> &points, qint64 fromMs, qint64 toMs,
                                int widthPx, int perPixel)
{
    QVector<PlotPoint> inside;
    for (int i = 0; i < points.size(); ++i)
        if (points.at(i).tMs >= fromMs && points.at(i).tMs <= toMs)
            inside.append(points.at(i));
    if (widthPx <= 0 || toMs <= fromMs || perPixel <= 0 || inside.size() <= widthPx * perPixel)
        return inside;

    const double msPerPx = double(toMs - fromMs) / double(widthPx);
    QVector<PlotPoint> out;
    int i = 0;
    while (i < inside.size()) {
        const qint64 column = columnOf(inside.at(i).tMs, fromMs, msPerPx, widthPx);
        int lo = i, hi = i, j = i;
        while (j < inside.size() && columnOf(inside.at(j).tMs, fromMs, msPerPx, widthPx) == column) {
            if (inside.at(j).mgdl < inside.at(lo).mgdl) lo = j;
            if (inside.at(j).mgdl > inside.at(hi).mgdl) hi = j;
            ++j;
        }
        // Time order inside the column; one point when the minimum is the maximum.
        if (lo == hi) {
            out.append(inside.at(lo));
        } else if (lo < hi) {
            out.append(inside.at(lo)); out.append(inside.at(hi));
        } else {
            out.append(inside.at(hi)); out.append(inside.at(lo));
        }
        i = j;
    }
    return out;
}

} // namespace ui
} // namespace lm
