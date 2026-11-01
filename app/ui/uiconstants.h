// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Michele Brami
#ifndef LM_UI_UICONSTANTS_H
#define LM_UI_UICONSTANTS_H

// Every tunable number of the user interface in one place.
// All values are PROVISIONAL starting points: none is measured on the phone. Each name starts
// with kUi so a test or a reviewer can point at it. They are plain constants
// (not settings) because nothing here is meant to be changed by the user.

namespace lm {
namespace ui {

// A reading is shown "as current" only while it is at most this old, whatever the service
// says (REQ-UI-09). The service's own staleness limit is 3 readings (180 s); this is wider
// on purpose: it is the safety net for a dead service, whose "not stale" flag is frozen.
const int kUiFreshMaxS = 300;

// The display grace for a LATE reading: the service calls a Subject stale after three minutes,
// but this source often delivers a reading two to four minutes late, and the cause text
// flashed in place of the band. The screen waits this long after the service's stale flag before it
// shows the cause and takes the colour from the value. It applies only to the lateness causes (the
// source cannot be reached, the source has no new readings, rate-limited); every known state shows at
// once. Display only: the service's limit, the trend arrow and the alerts are not changed. PROVISIONAL.
const int kUiStaleGraceS = 120;

// How often ages and the time axis are recomputed while the window is visible. Display only:
// it touches neither the store nor D-Bus, so it is not polling (REQ-ARC-03).
const int kUiDisplayTickS = 15;

// After this long in "the source cannot be reached" the Retry button is offered as well.
const int kUiRetryOfferS = 900;

// The cover returns to the most urgent Subject after the user stepped through them by hand.
const int kUiCoverManualHoldS = 30;


// ---- the plot ------------------------------------------------

// The normal live view: the last 12 hours, ending now (REQ-UI-01).
const long long kUiPlotWindowMs = 12LL * 3600 * 1000;
// Zoom limits: 30 minutes to 14 days.
const long long kUiZoomMinMs = 1800LL * 1000;
const long long kUiZoomMaxMs = 1209600LL * 1000;
// A view whose right edge is less than this before "now" still counts as live.
const long long kUiLiveSnapMs = 60LL * 1000;
// The value scale is fixed, in mg/dL: 40 is the sensor's lower reading limit.
const double kUiPlotYMinMgDl = 40.0;
const double kUiPlotYMaxMgDl = 300.0;
// Markers and ticks keep at least this many pixels apart.
const int kUiMarkerMinPx = 24;
const int kUiTickMinPx = 64;
// A series is reduced to at most this many points per pixel column.
const int kUiPointsPerPx = 2;
// A line breaks where two points are further apart than this many nominal spacings (4.3).
const int kUiGapFactor = 3;
// Opacity of the zone tints behind the plot: the urgent zones beyond LL and HH
// are the strongest, N the quietest.
const double kUiZoneAlphaUrgent = 0.22;
const double kUiZoneAlphaOuter = 0.14;
const double kUiZoneAlphaNormal = 0.08;

// ---- the plot symbols (Draft 0.1) -------------------------
// Lengths are REFERENCE pixels of a 540 px wide screen (Silica's Theme.pixelRatio = 1); the painted
// plot multiplies them by the phone's pixel ratio. Opacities are fractions of the theme colour. Each
// constant carries the symbol ID so a change of the design is one line here.
// the marker of the current feed, a filled circle.
const double kUiMarkerCurrentPx = 9.0;
// the marker of the history feed, a hollow square, and its stroke; the history feed's opacity.
const double kUiMarkerHistoryPx = 8.0;
const double kUiMarkerHistoryStrokePx = 2.0;
const double kUiHistoryOpacity = 0.70;
// the newest reading, the round band badge (the unknown badge when not current), blinking
// between full and kUiBlinkLowOpacity over kUiBlinkPeriodMs while it is current.
const double kUiNewestBadgePx = 28.0;
const int kUiBlinkPeriodMs = 2000;
const double kUiBlinkLowOpacity = 0.45;
// the lines of the feeds: current solid, history dashed (on, off).
const double kUiLineCurrentPx = 3.0;
const double kUiLineHistoryPx = 2.0;
const double kUiDashOnPx = 8.0;
const double kUiDashOffPx = 6.0;
// threshold lines, low and high dotted (on, off), very low and very high continuous.
const double kUiThresholdLinePx = 2.0;
const double kUiThresholdDotOnPx = 2.0;
const double kUiThresholdDotOffPx = 4.0;
// the hatch of a service gap or a stopped service: 45 degree lines, horizontal spacing, opacity.
const double kUiHatchStrokePx = 1.5;
const double kUiHatchSpacingPx = 8.0;
const double kUiHatchOpacity = 0.35;
// the "no new reading" band: dots on a grid, opacity; its left edge is a dotted line.
const double kUiNoNewDotPx = 1.5;
const double kUiNoNewGridPx = 6.0;
const double kUiNoNewOpacity = 0.30;
// the chevron of a reading off the fixed scale.
const double kUiChevronWidthPx = 10.0;
const double kUiChevronHeightPx = 6.0;
const double kUiChevronStrokePx = 2.0;
// a break longer than this is labelled "no data" under it (seconds).
const int kUiGapLabelS = 1200;
// the legend swatches (not drawn yet: the legend is words, on the About page).
const double kUiLegendSwatchWidthPx = 28.0;
const double kUiLegendSwatchHeightPx = 12.0;

} // namespace ui
} // namespace lm

#endif
