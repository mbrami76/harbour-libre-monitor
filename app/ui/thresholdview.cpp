// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Michele Brami
#include "thresholdview.h"

#include "alerts/thresholds.h"

#include <QJsonDocument>
#include <QJsonObject>
#include <QRegularExpression>

#include <algorithm>

namespace lm {
namespace ui {

namespace {

int intOf(const QJsonObject &o, const char *k)
{
    const QJsonValue v = o.value(QLatin1String(k));
    return v.isDouble() ? v.toInt(-1) : -1;
}

bool isMmol(const QString &unit) { return unit == QLatin1String("mmol/L"); }

// A whole step of the alert-time choices: 5 minutes, the step of the user documentation's examples.
const int kAlertTimeStepS = 5 * 60;

} // namespace

ThresholdAnswer parseThresholdAnswer(const QByteArray &json)
{
    ThresholdAnswer a;
    const QJsonDocument doc = QJsonDocument::fromJson(json);
    if (!doc.isObject() || !doc.object().value(QLatin1String("result")).isString()) {
        a.result = QStringLiteral("unreadable");
        return a;
    }
    const QJsonObject o = doc.object();
    a.result = o.value(QLatin1String("result")).toString();
    const QJsonValue th = o.value(QLatin1String("thresholds"));
    if (th.isObject()) {
        const QJsonObject t = th.toObject();
        a.ll = intOf(t, "ll"); a.l = intOf(t, "l"); a.h = intOf(t, "h"); a.hh = intOf(t, "hh");
    }
    a.offending = o.value(QLatin1String("offending")).toString();
    a.lMin = intOf(o, "l_min"); a.lMax = intOf(o, "l_max");
    a.hMin = intOf(o, "h_min"); a.hMax = intOf(o, "h_max");
    a.globalL = intOf(o, "global_l"); a.globalH = intOf(o, "global_h");
    return a;
}

QString thresholdNumber(int mgdl, const QString &unit, const QLocale &locale)
{
    QLocale plain(locale);
    plain.setNumberOptions(QLocale::OmitGroupSeparator);
    if (isMmol(unit))
        return plain.toString(lm::mgDlToMmolForDisplay(mgdl), 'f', 1);
    return plain.toString(mgdl);
}

QString thresholdWithUnit(int mgdl, const QString &unit, const QLocale &locale)
{
    return thresholdNumber(mgdl, unit, locale) + QLatin1Char(' ') + (isMmol(unit) ? QStringLiteral("mmol/L") : QStringLiteral("mg/dL"));
}

bool parseThresholdInput(const QString &text, const QString &unit, int *mgdl)
{
    const QString t = text.trimmed();
    if (isMmol(unit)) {
        // One or two digits, then at most two decimals after a point or a comma (an Italian keyboard
        // gives a comma). The core rounds once to whole mg/dL.
        static const QRegularExpression re(QStringLiteral("^\\d{1,2}([.,]\\d{1,2})?$"));
        if (!re.match(t).hasMatch())
            return false;
        QString c = t;
        c.replace(QLatin1Char(','), QLatin1Char('.'));
        bool ok = false;
        const double v = c.toDouble(&ok);
        return ok && lm::mmolEditToMgDl(v, mgdl);
    }
    static const QRegularExpression re(QStringLiteral("^\\d{1,4}$"));
    if (!re.match(t).hasMatch())
        return false;
    *mgdl = t.toInt();
    return true;
}

QVariantMap ThresholdMessage::toMap() const
{
    QVariantList list;
    for (int i = 0; i < parts.size(); ++i) {
        QVariantMap p;
        p.insert(QStringLiteral("key"), parts.at(i).first);
        p.insert(QStringLiteral("args"), parts.at(i).second);
        list.append(p);
    }
    QVariantMap m;
    m.insert(QStringLiteral("success"), success);
    m.insert(QStringLiteral("parts"), list);
    return m;
}

ThresholdMessage invalidInputMessage(const QString &boundary, const QString &unit)
{
    ThresholdMessage m;
    m.add(boundary == QLatin1String("H") ? QStringLiteral("ui-thr-err-invalid-high") : QStringLiteral("ui-thr-err-invalid-low"));
    m.add(isMmol(unit) ? QStringLiteral("ui-thr-hint-mmol") : QStringLiteral("ui-thr-hint-mgdl"));
    return m;
}

ThresholdMessage thresholdMessage(const ThresholdAnswer &a, const QString &unit, const QLocale &locale)
{
    ThresholdMessage m;
    const bool high = a.offending == QLatin1String("H");
    const QString r = a.result;
    if (r == QLatin1String("accepted")) {
        m.success = true;
        if (a.l > 0 && a.h > 0)
            m.add(QStringLiteral("ui-thr-saved"), QStringList() << thresholdWithUnit(a.l, unit, locale)
                                                                << thresholdWithUnit(a.h, unit, locale));
        else
            m.add(QStringLiteral("ui-thr-saved-plain"));
    } else if (r == QLatin1String("no-change")) {
        m.success = true;
        m.add(QStringLiteral("ui-thr-no-change"));
    } else if (r == QLatin1String("invalid-value")) {
        return invalidInputMessage(high ? QStringLiteral("H") : QStringLiteral("L"), unit);
    } else if (r == QLatin1String("inconsistent")) {
        m.add(high ? QStringLiteral("ui-thr-err-order-high") : QStringLiteral("ui-thr-err-order-low"));
        // The allowed range of the limit named. The service computes each range from the other limit as it
        // was asked for, so the other range would be built on the wrong value (on the emulator: "high from
        // 51" after a low limit of 50); it is given only when the named one is empty, which means the
        // other limit is the one to change.
        const bool lowRange = a.lMin > 0 && a.lMin <= a.lMax;
        const bool highRange = a.hMin > 0 && a.hMin <= a.hMax;
        const bool sayHigh = high ? highRange : !lowRange && highRange;
        const bool sayLow = high ? !highRange && lowRange : lowRange;
        if (sayLow)
            m.add(QStringLiteral("ui-thr-range-low"), QStringList() << thresholdWithUnit(a.lMin, unit, locale)
                                                                    << thresholdWithUnit(a.lMax, unit, locale));
        if (sayHigh)
            m.add(QStringLiteral("ui-thr-range-high"), QStringList() << thresholdWithUnit(a.hMin, unit, locale)
                                                                     << thresholdWithUnit(a.hMax, unit, locale));
    } else if (r == QLatin1String("needs-confirmation")) {
        m.add(QStringLiteral("ui-thr-err-not-confirmed"));
    } else if (r == QLatin1String("confirmation-mismatch")) {
        // Which limit, never with which number.
        m.add(high ? QStringLiteral("ui-thr-err-mismatch-high") : QStringLiteral("ui-thr-err-mismatch-low"));
    } else if (r == QLatin1String("unknown-subject")) {
        m.add(QStringLiteral("ui-thr-err-unknown-patient"));
    } else if (r == QLatin1String("not-saved")) {
        m.add(QStringLiteral("ui-thr-err-not-saved"));
    } else if (r == QLatin1String("service-not-running") || r == QLatin1String("call-failed")) {
        m.add(QStringLiteral("ui-thr-err-no-service"));
    } else if (r == QLatin1String("service-older")) {
        // An older service without SetThresholds: the strip's sentence, which says
        // what to do (nothing was changed: the service refused the unknown call).
        m.add(QStringLiteral("ui-service-older"));
    } else {
        m.add(QStringLiteral("ui-thr-err-refused"));
    }
    return m;
}

ChallengeNeed challengeFor(int wantedL, int wantedH, int currentL, int currentH, int globalL, int globalH)
{
    ChallengeNeed n;
    n.low = wantedL != currentL && wantedL < globalL;
    n.high = wantedH != currentH && wantedH > globalH;
    if (!n.low && !n.high) {
        n.low = wantedL != currentL;
        n.high = wantedH != currentH;
    }
    return n;
}

QList<int> alertTimeChoices(const AlertTimeRange &range, int current)
{
    QList<int> out;
    if (range.minS < 0 || range.maxS < 0 || range.maxS < range.minS)
        return out;
    out << range.minS << range.maxS;
    for (int s = (range.minS / kAlertTimeStepS + 1) * kAlertTimeStepS; s < range.maxS; s += kAlertTimeStepS)
        out << s;
    if (range.defaultS >= range.minS && range.defaultS <= range.maxS)
        out << range.defaultS;
    if (current >= range.minS && current <= range.maxS)
        out << current;
    std::sort(out.begin(), out.end());
    out.erase(std::unique(out.begin(), out.end()), out.end());
    return out;
}

QVariantList thresholdHistoryRows(const QList<ThresholdHistoryEntry> &entries, const QString &unit,
                                  const QLocale &locale)
{
    QVariantList out;
    for (int i = entries.size() - 1; i >= 0; --i) {
        const ThresholdHistoryEntry &e = entries.at(i);
        QVariantMap m;
        m.insert(QStringLiteral("timeMs"), e.timeMs);
        m.insert(QStringLiteral("boundary"), e.boundary == QLatin1String("H") ? QStringLiteral("high") : QStringLiteral("low"));
        m.insert(QStringLiteral("valueText"), thresholdWithUnit(e.value, unit, locale));
        m.insert(QStringLiteral("previousText"), thresholdWithUnit(e.previous, unit, locale));
        out.append(m);
    }
    return out;
}

} // namespace ui
} // namespace lm
