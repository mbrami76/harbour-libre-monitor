// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Michele Brami
#include "plotcontroller.h"

#include "glucoseformat.h"

#include "subjectmodel.h"
#include "uiconstants.h"

#include <QDateTime>

namespace lm {
namespace ui {

PlotController::PlotController(QObject *parent)
    : QObject(parent), m_backend(0), m_feeds(2), m_tracking(false),
      m_unit(QStringLiteral("mmol/L")), m_cursorMs(-1)
{
}

void PlotController::setBackend(Backend *backend)
{
    if (m_backend)
        disconnect(m_backend, 0, this, 0);
    m_backend = backend;
    // New readings move a LIVE view (its right edge is "now") but never a history view
    // (docs 4.7); both repaint, the view itself decides what moves.
    if (m_backend)
        connect(m_backend, &Backend::changed, this, &PlotController::changed);
}

qint64 PlotController::now() const
{
    return m_backend ? m_backend->nowMs() : QDateTime::currentMSecsSinceEpoch();
}

bool PlotController::findSubject(SubjectSnapshot *out) const
{
    if (!m_backend)
        return false;
    const QList<SubjectSnapshot> all = m_backend->subjects();
    for (int i = 0; i < all.size(); ++i)
        if (all.at(i).id == m_subject) {
            *out = all.at(i);
            return true;
        }
    return false;
}

void PlotController::setSubjectId(const QString &id)
{
    if (id == m_subject)
        return;
    m_subject = id;
    // A different Subject starts from the normal live view (the view is not kept, docs 4.7).
    m_view = PlotView();
    SubjectSnapshot s;
    if (findSubject(&s))
        m_view.setFirstDataMs(s.firstDataMs);
    emit changed();
}

void PlotController::setFeeds(int feeds)
{
    if (feeds == m_feeds || feeds < 0 || feeds > 2)
        return;
    m_feeds = feeds;
    emit changed();
}

void PlotController::setTracking(bool on)
{
    if (on == m_tracking)
        return;
    m_tracking = on;
    if (!on)
        m_cursorMs = -1;        // leaving the mode removes the cursor
    emit changed();
}

void PlotController::setDisplayUnit(const QString &unit)
{
    if ((unit != QLatin1String("mg/dL") && unit != QLatin1String("mmol/L")) || unit == m_unit)
        return;
    m_unit = unit;
    emit changed();
}

QString PlotController::format(double mgdl) const
{
    return formatGlucose(mgdl, QStringLiteral("mg/dL"), m_unit, QLocale::system());
}

void PlotController::setCursorFraction(double fraction)
{
    const double f = fraction < 0 ? 0 : (fraction > 1 ? 1 : fraction);
    const qint64 now_ = now();
    m_cursorMs = m_view.fromMs(now_) + static_cast<qint64>(f * double(m_view.widthMs()));
    emit changed();
}

namespace {
// The feeds the user chose to see (0 current, 1 history, 2 both), as in geometry().
bool wantCurrent(int feeds) { return feeds == 0 || feeds == 2; }
bool wantHistory(int feeds) { return feeds == 1 || feeds == 2; }
}

double PlotController::cursorCurrentMgdl() const
{
    SubjectSnapshot s;
    PlotPoint p;
    if (m_cursorMs < 0 || !wantCurrent(m_feeds) || !findSubject(&s) ||
        !nearestWithin(s.current, m_cursorMs, 60000LL * kUiGapFactor, &p))
        return -1;
    return p.mgdl;
}

double PlotController::cursorHistoryMgdl() const
{
    SubjectSnapshot s;
    PlotPoint p;
    if (m_cursorMs < 0 || !wantHistory(m_feeds) || !findSubject(&s) ||
        !nearestWithin(s.history, m_cursorMs, 300000LL * kUiGapFactor, &p))
        return -1;
    return p.mgdl;
}

PlotExtremes PlotController::extremes() const
{
    SubjectSnapshot s;
    if (!findSubject(&s))
        return PlotExtremes();
    const qint64 n = now();
    static const QVector<PlotPoint> none;
    return extremesOf(wantCurrent(m_feeds) ? s.current : none, wantHistory(m_feeds) ? s.history : none,
                      m_view.fromMs(n), m_view.toMs(n));
}

bool PlotController::hasExtremes() const { return extremes().has; }
double PlotController::minMgdl() const { return extremes().minMgdl; }
qlonglong PlotController::minAtMs() const { return extremes().minAtMs; }
double PlotController::maxMgdl() const { return extremes().maxMgdl; }
qlonglong PlotController::maxAtMs() const { return extremes().maxAtMs; }

bool PlotController::isLive() const { return m_view.isLive(); }
bool PlotController::isGrey() const { return m_view.isGrey(); }
bool PlotController::canReturnToNow() const { return m_view.canReturnToNow(); }
bool PlotController::beforeFirstData() const { return m_view.showsBeforeFirstData(now()); }
qlonglong PlotController::fromMs() const { return m_view.fromMs(now()); }
qlonglong PlotController::toMs() const { return m_view.toMs(now()); }

bool PlotController::hasData() const
{
    SubjectSnapshot s;
    return findSubject(&s) && (!s.current.isEmpty() || !s.history.isEmpty());
}

void PlotController::panByFraction(double fraction)
{
    // Dragging the picture to the right shows EARLIER time: the window moves back.
    m_view.panBy(-static_cast<qint64>(fraction * double(m_view.widthMs())), now());
    emit changed();
}

void PlotController::zoomBy(double factor, double anchorFraction)
{
    m_view.zoomBy(factor, anchorFraction, now());
    emit changed();
}

void PlotController::backToNow()
{
    m_view.backToNow();
    emit changed();
}

void PlotController::tick()
{
    emit changed();
}

PlotGeometry PlotController::geometry(int widthPx, int heightPx) const
{
    SubjectSnapshot s;
    if (!findSubject(&s))
        return PlotGeometry();
    const qint64 n = now();
    PlotInput in;
    in.fromMs = m_view.fromMs(n);
    in.toMs = m_view.toMs(n);
    in.nowMs = n;
    in.widthPx = widthPx;
    in.heightPx = heightPx;
    if (m_feeds == 0 || m_feeds == 2) in.current = s.current;
    if (m_feeds == 1 || m_feeds == 2) in.history = s.history;
    in.serviceGaps = s.serviceGaps;
    in.cursorMs = m_tracking ? m_cursorMs : -1;
    const SubjectView view = viewOfSnapshot(s, n);
    in.stale = !isFresh(view.id);
    in.utcOffsetMs = qint64(QDateTime::fromMSecsSinceEpoch(n).offsetFromUtc()) * 1000;
    if (m_backend && m_backend->serviceStatus() == ServiceStatus::Stopped)
        in.stoppedSinceMs = m_backend->lastHeartbeatMs() > 0 ? m_backend->lastHeartbeatMs() : n;
    // Grey: a history view, or stale data (docs 4.1 and 4.7, REQ-UI-05).
    return computePlotGeometry(in, m_view.isGrey() || in.stale);
}

} // namespace ui
} // namespace lm
