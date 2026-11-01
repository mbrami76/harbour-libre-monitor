// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Michele Brami
#ifndef LM_UI_PLOTCONTROLLER_H
#define LM_UI_PLOTCONTROLLER_H

// Glue between the plot logic and the screen: holds the view window of the Subject page
// (PlotView), takes the Subject's series from the backend and returns the geometry to paint.
//
//
// Pattern: model/controller. The painted item (app/src) asks geometry(w, h) and the page's
// gesture areas call panByFraction / zoomBy / backToNow; the numbers and decisions are in
// plotview.h and plotgeometry.h, which have their own tests. The controller only wires them to
// the Subject chosen on the page.

#include "backend.h"
#include "plotgeometry.h"
#include "plotview.h"

#include <QObject>

namespace lm {
namespace ui {

class PlotController : public QObject
{
    Q_OBJECT
    Q_PROPERTY(QString subjectId READ subjectId WRITE setSubjectId NOTIFY changed)
    Q_PROPERTY(bool live READ isLive NOTIFY changed)
    Q_PROPERTY(bool grey READ isGrey NOTIFY changed)
    Q_PROPERTY(bool hasData READ hasData NOTIFY changed)
    Q_PROPERTY(bool beforeFirstData READ beforeFirstData NOTIFY changed)
    Q_PROPERTY(qlonglong fromMs READ fromMs NOTIFY changed)
    Q_PROPERTY(qlonglong toMs READ toMs NOTIFY changed)
    // 0 current only, 1 history only, 2 both (the user's choice)
    Q_PROPERTY(int feeds READ feeds WRITE setFeeds NOTIFY changed)
    // Value tracking: while on, a drag on the plot moves a cursor
    // and shows the values under it instead of scrolling in time.
    Q_PROPERTY(bool tracking READ tracking WRITE setTracking NOTIFY changed)
    Q_PROPERTY(QString displayUnit READ displayUnit WRITE setDisplayUnit NOTIFY changed)
    Q_PROPERTY(bool cursorValid READ cursorValid NOTIFY changed)
    Q_PROPERTY(qlonglong cursorTimeMs READ cursorTimeMs NOTIFY changed)
    Q_PROPERTY(double cursorCurrentMgdl READ cursorCurrentMgdl NOTIFY changed)      // -1: none there
    Q_PROPERTY(double cursorHistoryMgdl READ cursorHistoryMgdl NOTIFY changed)      // -1: none there
    // Lowest and highest reading of the shown feeds in the whole window.
    Q_PROPERTY(bool hasExtremes READ hasExtremes NOTIFY changed)
    Q_PROPERTY(double minMgdl READ minMgdl NOTIFY changed)
    Q_PROPERTY(qlonglong minAtMs READ minAtMs NOTIFY changed)
    Q_PROPERTY(double maxMgdl READ maxMgdl NOTIFY changed)
    Q_PROPERTY(qlonglong maxAtMs READ maxAtMs NOTIFY changed)

public:
    explicit PlotController(QObject *parent = 0);

    void setBackend(Backend *backend);
    QString subjectId() const { return m_subject; }
    void setSubjectId(const QString &id);
    int feeds() const { return m_feeds; }
    void setFeeds(int feeds);

    bool tracking() const { return m_tracking; }
    void setTracking(bool on);
    QString displayUnit() const { return m_unit; }
    void setDisplayUnit(const QString &unit);
    bool cursorValid() const { return m_cursorMs >= 0 && m_cursorMs >= fromMs() && m_cursorMs <= toMs(); }
    qlonglong cursorTimeMs() const { return m_cursorMs; }
    double cursorCurrentMgdl() const;
    double cursorHistoryMgdl() const;
    bool hasExtremes() const;
    double minMgdl() const;
    qlonglong minAtMs() const;
    double maxMgdl() const;
    qlonglong maxAtMs() const;

    // The value as text in the display unit and the phone's locale ("6,2" or "112"); the
    // stored value is converted exactly and rounded for display only (REQ-DAT-12).
    Q_INVOKABLE QString format(double mgdl) const;
    // The finger is at `fraction` (0 left edge, 1 right edge) of the window: place the cursor.
    Q_INVOKABLE void setCursorFraction(double fraction);

    bool isLive() const;
    bool isGrey() const;
    // The view is not the normal live 12 h view (no button shows it; for tests and logic).
    bool canReturnToNow() const;
    bool hasData() const;
    bool beforeFirstData() const;
    qlonglong fromMs() const;
    qlonglong toMs() const;

    // A drag of `fraction` of the plot's width (positive = to the right = back in time).
    Q_INVOKABLE void panByFraction(double fraction);
    Q_INVOKABLE void zoomBy(double factor, double anchorFraction);
    Q_INVOKABLE void backToNow();
    // The display tick: the live window moved, repaint.
    Q_INVOKABLE void tick();

    PlotGeometry geometry(int widthPx, int heightPx) const;

signals:
    void changed();

private:
    bool findSubject(SubjectSnapshot *out) const;
    qint64 now() const;

    Backend *m_backend;
    QString m_subject;
    PlotView m_view;
    int m_feeds;
    bool m_tracking;
    QString m_unit;
    qint64 m_cursorMs;          // -1 = none
    PlotExtremes extremes() const;
};

} // namespace ui
} // namespace lm

#endif
