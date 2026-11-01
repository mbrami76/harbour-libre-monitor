// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Michele Brami
#ifndef LM_PLOTITEM_H
#define LM_PLOTITEM_H

// The item that paints the glucose plot.
//
// Pattern: a QQuickPaintedItem that only PAINTS. Every decision (where a point goes, where a
// line breaks, which tick step fits, the zones and threshold lines, grey or coloured) is made by
// the tested logic in app/ui (PlotController -> computePlotGeometry); this class turns the
// numbers it returns into QPainter calls. Why a painted item and not Canvas: the drawing code
// then stays in C++ next to its tests, and the item costs one texture of its own size.
//
// Colours: lines, markers and labels use the colours the page passes in (the Silica theme
// colours), so they follow the ambience. The only hues are the zone tints, which are the
// glucose bands' own colours (REQ-UI-03) and are drawn grey in a history view or when the
// Subject is stale (REQ-UI-05): the geometry says which. The other colour of the plot, the round
// band badge of the newest reading, is the author's drawing, laid over the item by the page at the
// place this item publishes (newestX, newestY). Every size, line style and opacity is a named
// constant in app/ui/uiconstants.h, multiplied by the phone's pixel ratio.

#include "ui/plotcontroller.h"
#include "ui/plotgeometry.h"

#include <QColor>
#include <QQuickPaintedItem>

namespace lm {

class PlotItem : public QQuickPaintedItem
{
    Q_OBJECT
    Q_PROPERTY(QObject *controller READ controller WRITE setController NOTIFY controllerChanged)
    Q_PROPERTY(QString displayUnit READ displayUnit WRITE setDisplayUnit NOTIFY styleChanged)
    Q_PROPERTY(bool axisLeft READ axisLeft WRITE setAxisLeft NOTIFY styleChanged)
    Q_PROPERTY(QColor lineColor READ lineColor WRITE setLineColor NOTIFY styleChanged)
    Q_PROPERTY(QColor labelColor READ labelColor WRITE setLabelColor NOTIFY styleChanged)
    Q_PROPERTY(QColor bubbleColor READ bubbleColor WRITE setBubbleColor NOTIFY styleChanged)
    // Where the drawn plot area starts and how wide it is inside this item (the axis numbers take
    // the rest), so the page can align its window times with the plot, not with the item.
    Q_PROPERTY(qreal plotAreaLeft READ plotLeft NOTIFY styleChanged)
    Q_PROPERTY(qreal plotAreaWidth READ plotAreaWidth NOTIFY styleChanged)
    // The threshold lines are in the theme's secondary colour.
    Q_PROPERTY(QColor thresholdColor READ thresholdColor WRITE setThresholdColor NOTIFY styleChanged)
    // Silica's Theme.pixelRatio: the symbol sizes are reference pixels (uiconstants.h).
    Q_PROPERTY(qreal pixelRatio READ pixelRatio WRITE setPixelRatio NOTIFY styleChanged)
    // The words drawn inside the plot, translated by the page (the item has no text of its own):
    // "no data" under a long break, the labels of the hatched and the dotted bands.
    Q_PROPERTY(QString noDataText MEMBER m_noDataText NOTIFY styleChanged)
    Q_PROPERTY(QString gapText MEMBER m_gapText NOTIFY styleChanged)
    Q_PROPERTY(QString stoppedText MEMBER m_stoppedText NOTIFY styleChanged)
    Q_PROPERTY(QString noNewText MEMBER m_noNewText NOTIFY styleChanged)
    // Where the newest current reading is, in this item's coordinates, so the page can lay the round
    // band badge over it (the item does not paint it: the badge is the author's SVG drawing).
    // `newestStale`: that reading is not the current value, the page shows the unknown badge.
    Q_PROPERTY(bool newestVisible READ newestVisible NOTIFY newestChanged)
    Q_PROPERTY(qreal newestX READ newestX NOTIFY newestChanged)
    Q_PROPERTY(qreal newestY READ newestY NOTIFY newestChanged)
    Q_PROPERTY(bool newestStale READ newestStale NOTIFY newestChanged)
    // The phone's 12 or 24 hour setting, known to the page (Silica's Format), for the hour labels.
    Q_PROPERTY(bool hour24 READ hour24 WRITE setHour24 NOTIFY styleChanged)
    Q_PROPERTY(int bubblePixelSize READ bubblePixelSize WRITE setBubblePixelSize NOTIFY styleChanged)
    Q_PROPERTY(int labelPixelSize READ labelPixelSize WRITE setLabelPixelSize NOTIFY styleChanged)

public:
    explicit PlotItem(QQuickItem *parent = 0);

    QObject *controller() const { return m_controller; }
    void setController(QObject *c);
    QString displayUnit() const { return m_unit; }
    void setDisplayUnit(const QString &u);
    bool axisLeft() const { return m_axisLeft; }
    void setAxisLeft(bool v);
    QColor lineColor() const { return m_line; }
    void setLineColor(const QColor &c);
    QColor labelColor() const { return m_label; }
    void setLabelColor(const QColor &c);
    QColor bubbleColor() const { return m_bubble; }
    void setBubbleColor(const QColor &c);
    QColor thresholdColor() const { return m_threshold; }
    void setThresholdColor(const QColor &c);
    qreal pixelRatio() const { return m_ratio; }
    void setPixelRatio(qreal r);
    bool newestVisible() const { return m_geo.hasRing; }
    qreal newestX() const { return m_geo.hasRing ? m_origin.x() + m_geo.ring.x() : 0; }
    qreal newestY() const { return m_geo.hasRing ? m_origin.y() + m_geo.ring.y() : 0; }
    bool newestStale() const { return m_geo.ringHollow; }
    bool hour24() const { return m_hour24; }
    void setHour24(bool on);
    int bubblePixelSize() const { return m_bubblePx > 0 ? m_bubblePx : m_labelPx; }
    void setBubblePixelSize(int px);
    int labelPixelSize() const { return m_labelPx; }
    void setLabelPixelSize(int px);

    void paint(QPainter *painter);
    qreal plotLeft() const;

    // Value tracking: the finger is at `x` (item coordinates); the item converts it to a
    // fraction of the plot area (its margins are the item's business) and tells the controller.
    Q_INVOKABLE void trackAt(qreal x);

protected:
    void geometryChanged(const QRectF &newGeometry, const QRectF &oldGeometry);

signals:
    void controllerChanged();
    void styleChanged();
    void newestChanged();

private slots:
    // Recomputes the geometry on the GUI thread when the data, the view, the size or the style changed,
    // and asks for a repaint. paint() runs on the render thread while the GUI thread waits, and only
    // draws this cached geometry, so the geometry is computed once per change and the newest-point
    // properties are set on the GUI thread.
    void recompute();

private:
    ui::PlotController *m_controller;
    QString m_unit;
    bool m_axisLeft;
    QColor m_line, m_label, m_bubble;
    int m_labelPx;
    int m_bubblePx;
    QColor m_threshold;
    qreal m_ratio;
    QString m_noDataText, m_gapText, m_stoppedText, m_noNewText;
    bool m_hour24;
    ui::PlotGeometry m_geo;     // the cached geometry of the plot area (recompute())
    QPointF m_origin;           // where the plot area starts inside the item
    int m_plotW, m_plotH;
    int plotWidth() const;
    qreal plotAreaWidth() const { return plotWidth(); }
};

} // namespace lm

#endif
