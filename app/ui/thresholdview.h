// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Michele Brami
#ifndef LM_UI_THRESHOLDVIEW_H
#define LM_UI_THRESHOLDVIEW_H

// The pure decisions of the thresholds page (REQ-THR-01 to 04):
// reading the service's SetThresholds answer, writing a limit in the user's
// unit, reading what the user typed, explaining every refusal in words with numbers, deciding which
// limit the challenge asks for, the choices of the three alert times, and the rows of the history.
// No QML, no D-Bus, no clock: the host tests try every case in both units.
//
// Rules kept here:
//  - The limits are WHOLE mg/dL; an edit in mmol/L is converted and rounded ONCE with the core's
//    mmolEditToMgDl, and the stored value is what is shown back. A number in
//    mmol/L on the screen is only a display (mgDlToMmolForDisplay), never compared.
//  - Every number shown carries the unit (REQ-THR-04); only the user's chosen unit is shown.
//  - The message after a challenge never contains the value asked for or the value typed again (
//    the screen does not show the value just asked for): a refused challenge says which limit did not
//    match, not with which number.
//  - The alert times are offered only inside the service's own range (Alerts() `limits`); no
//    number of seconds is written in the interface.

#include "alertsbackend.h"
#include "backend.h"

#include <QByteArray>
#include <QList>
#include <QLocale>
#include <QPair>
#include <QString>
#include <QStringList>
#include <QVariantList>
#include <QVariantMap>

namespace lm {
namespace ui {

// Reads the JSON answer of SetThresholds. A text this version cannot read gives result "unreadable".
ThresholdAnswer parseThresholdAnswer(const QByteArray &json);

// A limit in whole mg/dL written in `unit` ("mg/dL" whole, "mmol/L" one decimal) in `locale`, without
// and with the unit's name.
QString thresholdNumber(int mgdl, const QString &unit, const QLocale &locale);
QString thresholdWithUnit(int mgdl, const QString &unit, const QLocale &locale);

// What the user typed in `unit`, as whole mg/dL. mg/dL: a whole number (no decimals); mmol/L: a number
// with a point or a comma, rounded once by mmolEditToMgDl. False for anything else (empty, letters, a
// negative number); the page then says the value is not possible, without sending anything.
bool parseThresholdInput(const QString &text, const QString &unit, int *mgdl);

// One message of the page: a list of sentences, each a text key and its arguments (already written in
// the user's unit). The QML writes the sentences (ThresholdText.js) and joins them.
struct ThresholdMessage
{
    ThresholdMessage() : success(false) {}
    bool success;
    QList<QPair<QString, QStringList> > parts;

    void add(const QString &key, const QStringList &args = QStringList()) { parts.append(qMakePair(key, args)); }
    QVariantMap toMap() const;      // {success, parts: [{key, args}]}
};

// The words for an answer of the service. "needs-confirmation" is not a refusal the page shows (the page
// asks the challenge instead); it gives the text for a challenge the user cancelled.
ThresholdMessage thresholdMessage(const ThresholdAnswer &answer, const QString &unit, const QLocale &locale);
// The words when the typed text is not a possible value (nothing was sent). `boundary` is "L" or "H".
ThresholdMessage invalidInputMessage(const QString &boundary, const QString &unit);

// Which limits the challenge asks to type again: those that MOVE and become looser than the standard
// value (L lower than the standard low, H higher than the standard high), as the service judges
// (thresholdbook.cpp). When the service asked for a confirmation but none qualifies by this rule (the
// picture changed meanwhile), every limit that moves is asked, so the user is never left without a way.
struct ChallengeNeed
{
    ChallengeNeed() : low(false), high(false) {}
    bool low, high;
};
ChallengeNeed challengeFor(int wantedL, int wantedH, int currentL, int currentH, int globalL, int globalH);

// The choices of one alert time, in seconds, inside the service's range: the minimum, every whole
// 5 minutes in between, the maximum, the default and the value in force (when inside the range), sorted,
// each once. Empty when the range is not known (the page then says the times are not known).
QList<int> alertTimeChoices(const AlertTimeRange &range, int current);

// The history rows, NEWEST first: {timeMs, boundary ("low" or "high"), valueText, previousText}, the
// numbers with the unit. No comment (backend.h).
QVariantList thresholdHistoryRows(const QList<ThresholdHistoryEntry> &entries, const QString &unit,
                                  const QLocale &locale);

} // namespace ui
} // namespace lm

#endif
