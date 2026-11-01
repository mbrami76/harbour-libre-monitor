// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Michele Brami
#include "thresholdeditor.h"

#include "alerts/thresholds.h"

namespace lm {
namespace ui {

ThresholdEditor::ThresholdEditor(QObject *parent)
    : QObject(parent), m_data(0), m_alerts(0), m_unit(QStringLiteral("mmol/L")), m_state(QStringLiteral("idle")),
      m_pendingL(-1), m_pendingH(-1), m_historyState(QStringLiteral("loading"))
{
}

void ThresholdEditor::setBackends(Backend *data, AlertsBackend *alerts)
{
    if (m_data)
        disconnect(m_data, 0, this, 0);
    if (m_alerts)
        disconnect(m_alerts, 0, this, 0);
    m_data = data;
    m_alerts = alerts;
    if (m_data)
        connect(m_data, &Backend::thresholdHistoryLoaded, this, &ThresholdEditor::onHistory);
    if (m_alerts) {
        connect(m_alerts, &AlertsBackend::changed, this, &ThresholdEditor::changed);
        connect(m_alerts, &AlertsBackend::thresholdsAnswered, this, &ThresholdEditor::onAnswered);
    }
    emit changed();
}

void ThresholdEditor::setDisplayUnit(const QString &unit)
{
    const QString u = unit == QLatin1String("mg/dL") ? unit : QStringLiteral("mmol/L");
    if (u == m_unit)
        return;
    m_unit = u;
    emit changed();
}

void ThresholdEditor::setLocale(const QLocale &locale)
{
    m_locale = locale;
    emit changed();
}

bool ThresholdEditor::patient(PatientAlertFacts *out) const
{
    if (!m_alerts || m_subject.isEmpty())
        return false;
    const AlertsFacts f = m_alerts->facts();
    const PatientAlertFacts *p = f.known ? f.patient(m_subject) : 0;
    if (!p)
        return false;
    *out = *p;
    return true;
}

bool ThresholdEditor::known() const
{
    PatientAlertFacts p;
    return patient(&p) && p.l > 0 && p.h > 0;
}

QString ThresholdEditor::lowText() const
{
    PatientAlertFacts p;
    return patient(&p) && p.l > 0 ? thresholdNumber(p.l, m_unit, m_locale) : QString();
}

QString ThresholdEditor::highText() const
{
    PatientAlertFacts p;
    return patient(&p) && p.h > 0 ? thresholdNumber(p.h, m_unit, m_locale) : QString();
}

// The standard values are the core's global ones (core/alerts/thresholds.h): one source of the numbers,
// the same the service compares against for "looser" (REQ-THR-02).
QString ThresholdEditor::standardLowText() const { return thresholdWithUnit(lm::kGlobalL, m_unit, m_locale); }
QString ThresholdEditor::standardHighText() const { return thresholdWithUnit(lm::kGlobalH, m_unit, m_locale); }

QString ThresholdEditor::veryLowText() const
{
    PatientAlertFacts p;
    return thresholdWithUnit(patient(&p) && p.ll > 0 ? p.ll : lm::kGlobalLL, m_unit, m_locale);
}

QString ThresholdEditor::veryHighText() const
{
    PatientAlertFacts p;
    return thresholdWithUnit(patient(&p) && p.hh > 0 ? p.hh : lm::kGlobalHH, m_unit, m_locale);
}

void ThresholdEditor::open(const QString &subjectId)
{
    m_subject = subjectId;
    m_state = QStringLiteral("idle");
    m_need = ChallengeNeed();
    m_message = ThresholdMessage();
    m_pendingL = m_pendingH = -1;
    m_history.clear();
    m_historyState = QStringLiteral("loading");
    emit changed();
    if (m_data)
        m_data->requestThresholdHistory(subjectId);
}

void ThresholdEditor::save(const QString &lowText, const QString &highText)
{
    if (!m_alerts || m_subject.isEmpty() || m_state != QLatin1String("idle"))
        return;
    int l = -1, h = -1;
    if (!parseThresholdInput(lowText, m_unit, &l)) {
        m_message = invalidInputMessage(QStringLiteral("L"), m_unit);
        emit changed();
        return;
    }
    if (!parseThresholdInput(highText, m_unit, &h)) {
        m_message = invalidInputMessage(QStringLiteral("H"), m_unit);
        emit changed();
        return;
    }
    m_pendingL = l;
    m_pendingH = h;
    m_message = ThresholdMessage();
    m_state = QStringLiteral("sending");
    emit changed();
    m_alerts->setThresholds(m_subject, l, h, -1, -1);
}

void ThresholdEditor::confirmChallenge(const QString &typedLowText, const QString &typedHighText)
{
    if (!m_alerts || m_state != QLatin1String("challenge"))
        return;
    // A text that is not a number is sent as "nothing typed" (-1): the service then answers
    // confirmation-mismatch for that limit, the same as a wrong number. The typed values live only in
    // these two locals.
    int typedL = -1, typedH = -1;
    if (m_need.low && !parseThresholdInput(typedLowText, m_unit, &typedL))
        typedL = -1;
    if (m_need.high && !parseThresholdInput(typedHighText, m_unit, &typedH))
        typedH = -1;
    // The service counts any number of 0 or more as a confirmation; a limit not asked for gets 0 when the
    // other one was typed, so that only the limit asked for can mismatch.
    if (typedL < 0 && typedH < 0) {
        typedL = m_need.low ? -1 : 0;
        typedH = m_need.high ? -1 : 0;
        if (typedL < 0 && typedH < 0)
            typedL = 0;             // still a confirmation: the service then names the limit that did not match
    } else {
        if (!m_need.low) typedL = 0;
        if (!m_need.high) typedH = 0;
    }
    m_state = QStringLiteral("sending");
    m_need = ChallengeNeed();
    emit changed();
    m_alerts->setThresholds(m_subject, m_pendingL, m_pendingH, typedL, typedH);
}

void ThresholdEditor::cancelChallenge()
{
    if (m_state != QLatin1String("challenge"))
        return;
    ThresholdAnswer a;
    a.result = QStringLiteral("needs-confirmation");
    m_message = thresholdMessage(a, m_unit, m_locale);
    finishEdit();
}

void ThresholdEditor::clearMessage()
{
    if (m_message.parts.isEmpty())
        return;
    m_message = ThresholdMessage();
    emit changed();
}

void ThresholdEditor::finishEdit()
{
    m_state = QStringLiteral("idle");
    m_need = ChallengeNeed();
    m_pendingL = m_pendingH = -1;
    emit changed();
}

void ThresholdEditor::onAnswered(const QString &subject, const ThresholdAnswer &answer)
{
    if (subject != m_subject || m_state != QLatin1String("sending"))
        return;                 // an answer for another page, or after the page was left
    if (answer.result == QLatin1String("needs-confirmation")) {
        PatientAlertFacts p;
        const bool have = patient(&p);
        const int curL = have && p.l > 0 ? p.l : lm::kGlobalL;
        const int curH = have && p.h > 0 ? p.h : lm::kGlobalH;
        const int gL = answer.globalL > 0 ? answer.globalL : lm::kGlobalL;
        const int gH = answer.globalH > 0 ? answer.globalH : lm::kGlobalH;
        m_need = challengeFor(m_pendingL, m_pendingH, curL, curH, gL, gH);
        m_state = QStringLiteral("challenge");
        m_message = ThresholdMessage();
        emit changed();
        return;
    }
    m_message = thresholdMessage(answer, m_unit, m_locale);
    const bool accepted = answer.result == QLatin1String("accepted");
    finishEdit();
    if (accepted && m_data) {
        m_historyState = QStringLiteral("loading");
        m_data->requestThresholdHistory(m_subject);
    }
}

void ThresholdEditor::onHistory(const QString &subject, bool ok, const QList<ThresholdHistoryEntry> &entries)
{
    if (subject != m_subject)
        return;
    m_history = ok ? entries : QList<ThresholdHistoryEntry>();
    m_historyState = ok ? QStringLiteral("ready") : QStringLiteral("unreadable");
    emit changed();
}

bool ThresholdEditor::timeFacts(const QString &kind, AlertTimeRange *range, int *current) const
{
    PatientAlertFacts p;
    if (!patient(&p))
        return false;
    const AlertsFacts f = m_alerts->facts();
    if (kind == QLatin1String("low")) {
        *range = f.persistenceLow;
        *current = p.persistenceLowS;
    } else if (kind == QLatin1String("high")) {
        *range = f.persistenceHigh;
        *current = p.persistenceHighS;
    } else if (kind == QLatin1String("missing")) {
        *range = f.missingLimit;
        *current = p.missingLimitS;
    } else {
        return false;
    }
    return range->minS >= 0 && range->maxS >= range->minS;
}

bool ThresholdEditor::timesKnown() const
{
    AlertTimeRange r;
    int c = -1;
    return timeFacts(QStringLiteral("low"), &r, &c) && timeFacts(QStringLiteral("high"), &r, &c)
        && timeFacts(QStringLiteral("missing"), &r, &c);
}

QVariantList ThresholdEditor::timeChoices(const QString &kind) const
{
    QVariantList out;
    AlertTimeRange r;
    int current = -1;
    if (!timeFacts(kind, &r, &current))
        return out;
    const QList<int> choices = alertTimeChoices(r, current);
    for (int i = 0; i < choices.size(); ++i) {
        QVariantMap m;
        m.insert(QStringLiteral("seconds"), choices.at(i));
        m.insert(QStringLiteral("isDefault"), choices.at(i) == r.defaultS);
        out.append(m);
    }
    return out;
}

int ThresholdEditor::timeIndex(const QString &kind) const
{
    AlertTimeRange r;
    int current = -1;
    if (!timeFacts(kind, &r, &current))
        return -1;
    return alertTimeChoices(r, current).indexOf(current);
}

} // namespace ui
} // namespace lm
