// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Michele Brami
#include "plotitem.h"

#include "ui/plotgeometry.h"
#include "ui/uiconstants.h"
#include "units/glucoseunits.h"

#include <QDateTime>
#include <QLocale>
#include <QFontMetrics>
#include <QPainter>
#include <QPen>

#include <algorithm>
#include <cmath>

namespace lm {

namespace {

// The band colours (the badge fills). The zone tints are these
// colours at a low opacity; they are the only hues in the plot.
QColor zoneColor(ui::PlotZone::Band band)
{
    switch (band) {
    case ui::PlotZone::BelowLL: return QColor(0x8E, 0x0F, 0x1B);
    case ui::PlotZone::LLtoL: return QColor(0xFF, 0x9F, 0x1C);
    case ui::PlotZone::Normal: return QColor(0x1F, 0x6F, 0x3A);
    case ui::PlotZone::HtoHH: return QColor(0xF2, 0xD1, 0x2E);
    case ui::PlotZone::AboveHH: return QColor(0x5B, 0x2A, 0x86);
    }
    return QColor();
}

} // namespace

PlotItem::PlotItem(QQuickItem *parent)
    : QQuickPaintedItem(parent), m_controller(0), m_unit(QStringLiteral("mmol/L")), m_axisLeft(false),
      m_line(Qt::white), m_label(Qt::lightGray), m_bubble(QColor(0, 0, 0, 200)), m_labelPx(18), m_bubblePx(0),
      m_threshold(Qt::lightGray), m_ratio(1.0), m_hour24(true), m_plotW(0), m_plotH(0)
{
    setAntialiasing(true);
    // Any change of style or size changes the drawing: recompute once, then repaint.
    connect(this, &PlotItem::styleChanged, this, &PlotItem::recompute);
}

void PlotItem::setController(QObject *c)
{
    ui::PlotController *pc = qobject_cast<ui::PlotController *>(c);
    if (pc == m_controller)
        return;
    if (m_controller)
        disconnect(m_controller, 0, this, 0);
    m_controller = pc;
    if (m_controller)
        connect(m_controller, &ui::PlotController::changed, this, &PlotItem::recompute);
    emit controllerChanged();
    recompute();
}

void PlotItem::setDisplayUnit(const QString &u) { if (u != m_unit) { m_unit = u; emit styleChanged(); } }
void PlotItem::setAxisLeft(bool v) { if (v != m_axisLeft) { m_axisLeft = v; emit styleChanged(); } }
void PlotItem::setLineColor(const QColor &c) { m_line = c; emit styleChanged(); }
void PlotItem::setThresholdColor(const QColor &c) { m_threshold = c; emit styleChanged(); }
void PlotItem::setPixelRatio(qreal r) { if (r > 0 && r != m_ratio) { m_ratio = r; emit styleChanged(); } }
void PlotItem::setHour24(bool on) { if (on != m_hour24) { m_hour24 = on; emit styleChanged(); } }
void PlotItem::setBubblePixelSize(int px) { if (px != m_bubblePx) { m_bubblePx = px; emit styleChanged(); } }
void PlotItem::setBubbleColor(const QColor &c) { m_bubble = c; emit styleChanged(); }
void PlotItem::setLabelColor(const QColor &c) { m_label = c; emit styleChanged(); }
void PlotItem::setLabelPixelSize(int px) { if (px != m_labelPx) { m_labelPx = px; emit styleChanged(); } }

void PlotItem::geometryChanged(const QRectF &n, const QRectF &o)
{
    QQuickPaintedItem::geometryChanged(n, o);
    emit styleChanged();        // the plot area moved or resized: tell the page that aligns text to it
}

int PlotItem::plotWidth() const { return int(width()) - m_labelPx * 3; }
qreal PlotItem::plotLeft() const { return m_axisLeft ? m_labelPx * 3 : 0; }

void PlotItem::trackAt(qreal x)
{
    if (!m_controller || plotWidth() < 20)
        return;
    m_controller->setCursorFraction((x - plotLeft()) / double(plotWidth()));
}

void PlotItem::recompute()
{
    const bool hadNewest = m_geo.hasRing;
    const QPointF oldNewest = m_geo.ring;
    const bool oldStale = m_geo.ringHollow;
    // Room for the time labels along the top and the value numbers at one side.
    const int topMargin = m_labelPx + 8;
    const int sideMargin = m_labelPx * 3;
    m_plotW = int(width()) - sideMargin;
    m_plotH = int(height()) - topMargin - 4;
    m_origin = QPointF(m_axisLeft ? sideMargin : 0, topMargin);
    if (!m_controller || width() < 40 || height() < 40 || m_plotW < 20 || m_plotH < 20)
        m_geo = ui::PlotGeometry();
    else
        m_geo = m_controller->geometry(m_plotW, m_plotH);
    if (hadNewest != m_geo.hasRing || oldNewest != m_geo.ring || oldStale != m_geo.ringHollow)
        emit newestChanged();
    update();
}

namespace {

// The hatch lines at 45 degrees rising to the right, `spacing` apart
// measured horizontally, inside `r` only.
void drawHatch(QPainter *p, const QRectF &r, const QColor &c, qreal stroke, qreal spacing)
{
    p->save();
    p->setClipRect(r, Qt::IntersectClip);
    p->setPen(QPen(c, stroke, Qt::SolidLine, Qt::FlatCap));
    for (qreal x = r.left() - r.height(); x < r.right(); x += spacing)
        p->drawLine(QPointF(x, r.bottom()), QPointF(x + r.height(), r.top()));
    p->restore();
}

// A label centred at the top of a band, left out when it does not fit.
void drawBandLabel(QPainter *p, const QRectF &band, qreal y, const QString &text, const QColor &c)
{
    if (text.isEmpty())
        return;
    const QFontMetrics fm(p->font());
    const qreal w = fm.width(text) + 4;
    if (w > band.width())
        return;
    p->setPen(c);
    p->drawText(QRectF(band.center().x() - w / 2, y, w, fm.height()), Qt::AlignCenter, text);
}

} // namespace

void PlotItem::paint(QPainter *p)
{
    const ui::PlotGeometry &g = m_geo;
    if (!m_controller || g.zones.isEmpty())
        return;
    const int plotW = m_plotW, plotH = m_plotH;
    const QPointF origin = m_origin;
    const qreal k = m_ratio;        // reference pixels to device pixels
    const int sideMargin = m_labelPx * 3;   // the value numbers' column (as in recompute())

    p->setRenderHint(QPainter::Antialiasing, true);
    QFont font = p->font();
    font.setPixelSize(m_labelPx);
    p->setFont(font);

    p->save();
    p->translate(origin);
    p->setClipRect(QRectF(0, 0, plotW, plotH));

    // Zones: the band hue at low opacity, or grey when the geometry says so (REQ-UI-05).
    for (int i = 0; i < g.zones.size(); ++i) {
        const ui::PlotZone &z = g.zones.at(i);
        QColor c = z.grey ? m_line : zoneColor(z.band);
        // Grey zones use the strong text colour at a low opacity so they stay visible on a
        // light ambience (the pale secondary colour vanished there).
        c.setAlphaF(z.alpha * (z.grey ? 0.9 : 1.0));
        p->fillRect(QRectF(0, z.top, plotW, z.bottom - z.top), c);
    }

    // Threshold lines: low and high dotted, very low and very high continuous, the theme's
    // secondary colour, no letters. A dash pattern is in units of the pen width.
    for (int i = 0; i < g.lines.size(); ++i) {
        QPen pen(m_threshold, ui::kUiThresholdLinePx * k);
        if (!g.lines.at(i).solid) {
            pen.setCapStyle(Qt::RoundCap);
            pen.setDashPattern(QVector<qreal>() << ui::kUiThresholdDotOnPx / ui::kUiThresholdLinePx
                                                << ui::kUiThresholdDotOffPx / ui::kUiThresholdLinePx);
        }
        p->setPen(pen);
        p->drawLine(QPointF(0, g.lines.at(i).y), QPointF(plotW, g.lines.at(i).y));
    }

    // The start of each local day: a thin vertical line with the date at the foot.
    {
        QColor dc = m_line;
        dc.setAlphaF(0.55);
        p->setPen(QPen(dc, 1.0 * k, Qt::SolidLine));
        for (int i = 0; i < g.dayStarts.size(); ++i)
            p->drawLine(QPointF(g.dayStarts.at(i).x, 0), QPointF(g.dayStarts.at(i).x, plotH));
    }

    // Bands: hatched for a service gap or a stopped service, dotted for "no new reading"
    // each with its label at the top when it fits.
    QFont small = font;
    small.setPixelSize(std::max(8, int(m_labelPx * 0.8)));
    for (int i = 0; i < g.bands.size(); ++i) {
        const ui::PlotBand &b = g.bands.at(i);
        const QRectF r(b.x0, 0, b.x1 - b.x0, plotH);
        if (b.kind == ui::PlotBand::NoNewReading) {
            QColor dot = m_line;
            dot.setAlphaF(ui::kUiNoNewOpacity);
            p->setPen(Qt::NoPen);
            p->setBrush(dot);
            const qreal grid = ui::kUiNoNewGridPx * k, d = ui::kUiNoNewDotPx * k;
            for (qreal y = grid / 2; y < plotH; y += grid)
                for (qreal x = r.left() + grid / 2; x < r.right(); x += grid)
                    p->drawEllipse(QPointF(x, y), d / 2, d / 2);
            QPen edge(m_line, 1.5 * k);
            edge.setDashPattern(QVector<qreal>() << 2.0 / 1.5 << 4.0 / 1.5);
            p->setPen(edge);
            p->drawLine(QPointF(r.left(), 0), QPointF(r.left(), plotH));
            p->setFont(small);
            drawBandLabel(p, r, 2, m_noNewText, m_label);
            p->setFont(font);
        } else {
            QColor hc = m_line;
            hc.setAlphaF(ui::kUiHatchOpacity);
            drawHatch(p, r, hc, ui::kUiHatchStrokePx * k, ui::kUiHatchSpacingPx * k);
            p->setFont(small);
            drawBandLabel(p, r, 2, b.kind == ui::PlotBand::ServiceStopped ? m_stoppedText : m_gapText, m_label);
            p->setFont(font);
        }
    }
    p->setBrush(Qt::NoBrush);

    // "no data" under a long break of the line.
    p->setFont(small);
    for (int i = 0; i < g.longBreaks.size(); ++i) {
        const QRectF r(g.longBreaks.at(i).x0, 0, g.longBreaks.at(i).x1 - g.longBreaks.at(i).x0, plotH);
        drawBandLabel(p, r, plotH - QFontMetrics(small).height() - 2, m_noDataText, m_label);
    }
    p->setFont(font);

    // The feeds: history dashed (8 on, 6 off), thinner and at 70 %; current solid with round
    // joins. Each segment is its own polyline, so a dash pattern restarts at each break. Neutral
    // colour for both.
    QColor hist = m_line;
    hist.setAlphaF(ui::kUiHistoryOpacity);
    QPen dashed(hist, ui::kUiLineHistoryPx * k, Qt::SolidLine, Qt::FlatCap);
    dashed.setDashPattern(QVector<qreal>() << ui::kUiDashOnPx / ui::kUiLineHistoryPx
                                           << ui::kUiDashOffPx / ui::kUiLineHistoryPx);
    p->setPen(dashed);
    for (int s = 0; s < g.history.size(); ++s)
        p->drawPolyline(g.history.at(s).points.constData(), g.history.at(s).points.size());
    p->setPen(QPen(m_line, ui::kUiLineCurrentPx * k, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
    for (int s = 0; s < g.current.size(); ++s)
        p->drawPolyline(g.current.at(s).points.constData(), g.current.at(s).points.size());

    // Markers: a filled circle for the current feed, a hollow square for the history feed
    //.
    for (int i = 0; i < g.markers.size(); ++i) {
        const ui::PlotMarker &m = g.markers.at(i);
        if (m.feedHistory) {
            const qreal side = ui::kUiMarkerHistoryPx * k;
            p->setPen(QPen(hist, ui::kUiMarkerHistoryStrokePx * k, Qt::SolidLine, Qt::SquareCap, Qt::MiterJoin));
            p->setBrush(Qt::NoBrush);
            p->drawRect(QRectF(m.pos.x() - side / 2, m.pos.y() - side / 2, side, side));
        } else {
            const qreal r = ui::kUiMarkerCurrentPx * k / 2;
            p->setPen(Qt::NoPen);
            p->setBrush(m_line);
            p->drawEllipse(m.pos, r, r);
        }
    }
    p->setBrush(Qt::NoBrush);

    // The newest current reading is NOT painted here: the page lays the round band badge (or the
    // unknown badge) over the plot at newestX, newestY.

    // The lowest and highest reading of the window: a diamond ring on the curve. No numbers here:
    // the values are read with value tracking.
    if (g.extremeMarks.has) {
        p->setBrush(Qt::NoBrush);
        p->setPen(QPen(m_line, 3.5));
        const QPointF pts[2] = { g.extremeMarks.low, g.extremeMarks.high };
        for (int i = 0; i < 2; ++i) {
            const qreal r = 14.0;
            p->drawPolygon(QPolygonF() << pts[i] + QPointF(0, -r) << pts[i] + QPointF(r, 0)
                                       << pts[i] + QPointF(0, r) << pts[i] + QPointF(-r, 0));
        }
    }

    // The value-tracking cursor: a vertical line and a mark on each feed's nearest reading.
    if (g.cursor.valid) {
        p->setPen(QPen(m_line, 1.5, Qt::DashDotLine));
        p->drawLine(QPointF(g.cursor.x, 0), QPointF(g.cursor.x, plotH));
        p->setBrush(Qt::NoBrush);
        p->setPen(QPen(m_line, 2.5));
        if (g.cursor.hasCurrent)
            p->drawEllipse(g.cursor.current, 6.0, 6.0);
        if (g.cursor.hasHistory)
            p->drawRect(QRectF(g.cursor.history.x() - 6, g.cursor.history.y() - 6, 12, 12));

        // The readout travels with the cursor, inside the plot: the time of the finger and the
        // reading of each feed, marked with the same symbols as the markers (a filled circle
        // for the current feed, a square for the history feed), so no words are needed.
        QStringList lines;
        const QDateTime when = QDateTime::fromMSecsSinceEpoch(g.cursor.tMs);
        lines << when.toString(QStringLiteral("yyyy-MM-dd hh:mm"));
        QString values;
        if (g.cursor.hasCurrent)
            values += QString::fromUtf8("\xE2\x97\x8F ") + m_controller->format(g.cursor.currentMgdl);
        if (g.cursor.hasHistory)
            values += (values.isEmpty() ? QString() : QStringLiteral("  ")) + QString::fromUtf8("\xE2\x96\xA1 ") + m_controller->format(g.cursor.historyMgdl);
        if (values.isEmpty())
            values = QString::fromUtf8("\xE2\x80\x94");        // no reading here (a gap)
        lines << values;
        QFont mono(QStringLiteral("monospace"));
        mono.setPixelSize(bubblePixelSize());
        p->setFont(mono);
        const QFontMetrics fm(mono);
        int w = 0;
        for (int i = 0; i < lines.size(); ++i)
            w = std::max(w, fm.width(lines.at(i)));
        w += 16;
        const int h = fm.height() * lines.size() + 10;
        // Right of the cursor when there is room, else left of it.
        qreal bx = g.cursor.x + 10;
        if (bx + w > plotW)
            bx = g.cursor.x - 10 - w;
        bx = std::max<qreal>(0, bx);
        const QRectF box(bx, 4, w, h);
        p->setPen(QPen(m_line, 1.0));
        p->setBrush(m_bubble);
        p->drawRoundedRect(box, 6, 6);
        p->setPen(m_line);
        for (int i = 0; i < lines.size(); ++i)
            p->drawText(QRectF(box.x() + 8, box.y() + 5 + i * fm.height(), w, fm.height()),
                        Qt::AlignLeft | Qt::AlignVCenter, lines.at(i));
        p->setFont(font);
    }

    // Readings off the fixed scale: an open chevron at the edge where the line leaves the
    // plot, pointing out of it, so they are not read as in range.
    {
        const qreal cw = ui::kUiChevronWidthPx * k / 2, ch = ui::kUiChevronHeightPx * k;
        p->setPen(QPen(m_line, ui::kUiChevronStrokePx * k, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
        for (int i = 0; i < g.offScaleHigh.size(); ++i) {
            const QPointF a(g.offScaleHigh.at(i).x(), 2 * k);
            p->drawPolyline(QPolygonF() << a + QPointF(-cw, ch) << a << a + QPointF(cw, ch));
        }
        for (int i = 0; i < g.offScaleLow.size(); ++i) {
            const QPointF a(g.offScaleLow.at(i).x(), plotH - 2 * k);
            p->drawPolyline(QPolygonF() << a + QPointF(-cw, -ch) << a << a + QPointF(cw, -ch));
        }
    }
    p->restore();

    // Date at the foot of each day line, in the phone's short date format. It stays INSIDE the plot:
    // to the right of its line, and left out when it does not fit (an overflowing label looked like a
    // stray mark at the plot's corner, and one on the left of its line was read as another day's).
    p->setPen(m_label);
    {
        const QLocale dloc = QLocale::system();
        const QFontMetrics dfm(p->font());
        for (int i = 0; i < g.dayStarts.size(); ++i) {
            const QDateTime dt = QDateTime::fromMSecsSinceEpoch(g.dayStarts.at(i).tMs);
            const QString text = dloc.toString(dt.date(), QLocale::ShortFormat);
            const qreal w = dfm.width(text) + 4;
            const qreal x = g.dayStarts.at(i).x + 3;
            if (x + w > plotW)
                continue;               // does not fit to the right of its line: left out
            p->drawText(QRectF(origin.x() + x, origin.y() + plotH - m_labelPx - 4, w, m_labelPx + 2),
                        Qt::AlignLeft | Qt::AlignBottom, text);
        }
    }

    // Time labels along the top only the HOUR as an integer, in the
    // phone's 12 or 24 hour style, so the row stays uncluttered; the days are told by the day lines.
    // Never minutes, also when zoomed in; steps of a day or more show the day of the month.
    p->setPen(m_label);
    const QLocale loc = QLocale::system();
    for (int i = 0; i < g.ticks.size(); ++i) {
        const ui::PlotTick &t = g.ticks.at(i);
        const QDateTime dt = QDateTime::fromMSecsSinceEpoch(t.tMs);
        const int h = dt.time().hour();
        QString text;
        if (g.tickStepMs >= 24LL * 3600 * 1000) {
            text = QString::number(dt.date().day());
        } else {
            // The hour only, never minutes (the ticks are never finer than an hour).
            const QString ampm = h < 12 ? loc.amText() : loc.pmText();
            if (m_hour24) {
                text = QString::number(h);
            } else {
                const int h12 = h % 12 == 0 ? 12 : h % 12;
                text = QString::number(h12) + QLatin1Char(' ') + ampm;
            }
        }
        const qreal x = origin.x() + t.x;
        p->drawText(QRectF(x - 40, 0, 80, m_labelPx + 4), Qt::AlignHCenter | Qt::AlignTop, text);
    }

    // Value numbers at the chosen side: every 2 mmol/L or 50 mg/dL of the fixed scale, in the
    // display unit, rounded for display only (REQ-DAT-12).
    const bool mmol = m_unit == QLatin1String("mmol/L");
    const double step = mmol ? 2.0 : 50.0;
    const double minV = mmol ? ui::kUiPlotYMinMgDl / kMgDlPerMmolL : ui::kUiPlotYMinMgDl;
    const double maxV = mmol ? ui::kUiPlotYMaxMgDl / kMgDlPerMmolL : ui::kUiPlotYMaxMgDl;
    const qreal numX = m_axisLeft ? 0 : plotW + 4;
    for (double v = std::ceil(minV / step) * step; v <= maxV; v += step) {
        const double mg = mmol ? v * kMgDlPerMmolL : v;
        const double y = origin.y() + plotH - (mg - ui::kUiPlotYMinMgDl) / (ui::kUiPlotYMaxMgDl - ui::kUiPlotYMinMgDl) * plotH;
        p->drawText(QRectF(numX, y - m_labelPx / 2.0 - 1, sideMargin - 6, m_labelPx + 2),
                    (m_axisLeft ? Qt::AlignRight : Qt::AlignLeft) | Qt::AlignVCenter,
                    QLocale::system().toString(v, 'f', 0));
    }
}

} // namespace lm
