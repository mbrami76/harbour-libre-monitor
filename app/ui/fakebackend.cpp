// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Michele Brami
#include "fakebackend.h"

#ifdef LIBREMON_DEVSOURCE

#include <QTimer>

#include <cmath>

namespace lm {
namespace ui {

namespace {

const qint64 kMinute = 60LL * 1000;
const qint64 kHour = 60 * kMinute;

// One synthetic Subject: a smooth curve with a bit of ripple, around `base` mg/dL.
struct Profile
{
    const char *id;
    const char *name;
    const char *label;
    double base, amp, phase;
};

double curve(const Profile &p, qint64 t)
{
    return p.base + p.amp * std::sin(double(t) / 3.1e6 + p.phase) + 4.0 * std::sin(double(t) / 6.0e5 + p.phase * 2);
}

// A feed on a regular grid ending at `endMs`, `days` back; `lastValue` forces the newest value
// (so a scenario can put a Subject in a band without bending the whole curve).
QVector<PlotPoint> series(const Profile &p, qint64 endMs, qint64 stepMs, qint64 spanMs,
                          double lastValue, qint64 holeFrom = 0, qint64 holeTo = 0)
{
    QVector<PlotPoint> v;
    const qint64 first = endMs - spanMs;
    for (qint64 t = first - (first % stepMs); t <= endMs; t += stepMs) {
        if (holeTo > 0 && t >= holeFrom && t <= holeTo)
            continue;
        double val = curve(p, t);
        if (lastValue > 0) {
            // Blend the last 20 minutes to the wanted newest value.
            const double k = std::max(0.0, 1.0 - double(endMs - t) / double(20 * kMinute));
            val = val * (1.0 - k) + lastValue * k;
        }
        v.append(PlotPoint(t, std::max(40.0, std::min(400.0, val))));
    }
    return v;
}

SubjectSnapshot makeSubject(const Profile &p, qint64 now, qint64 readingAgeMs, double lastMgdl,
                            const QString &unit, int trendCode, bool withGap)
{
    SubjectSnapshot s;
    s.id = QString::fromLatin1(p.id);
    s.name = QString::fromUtf8(p.name);
    s.label = QString::fromLatin1(p.label);
    s.connection = QStringLiteral("c1");
    s.report.state = QStringLiteral("idle");
    s.hasReading = true;
    s.measuredAtMs = now - readingAgeMs;
    s.unit = unit;
    s.value = unit == QLatin1String("mmol/L") ? lastMgdl / 18.0156 : std::floor(lastMgdl + 0.5);
    s.hasTrend = trendCode > 0;
    s.trendCode = trendCode;
    const qint64 end = now - readingAgeMs;
    const qint64 hole0 = now - 6 * kHour, hole1 = now - 5 * kHour;
    s.current = series(p, end, kMinute, 3 * 24 * kHour, lastMgdl, withGap ? hole0 : 0, withGap ? hole1 : 0);
    s.history = series(p, end - 19 * kMinute, 5 * kMinute, 3 * 24 * kHour, 0, withGap ? hole0 : 0, withGap ? hole1 : 0);
    if (withGap)
        s.serviceGaps << qMakePair(hole0, hole1);
    s.firstDataMs = now - 3 * 24 * kHour;
    // An invented sensor (scripted, never a real identifier): started six days ago, warm-up of 60 minutes.
    s.sensor.known = true;
    s.sensor.id = QStringLiteral("scripted-sensor-") + QString::fromLatin1(p.label);
    s.sensor.activatedMs = now - 6 * 24 * kHour;
    s.sensor.warmupMinutes = 60;
    s.sensor.firstSeenMs = now - 6 * 24 * kHour + 70 * kMinute;
    s.sensor.lastSeenMs = now - readingAgeMs;
    if (QLatin1String(p.label) == QLatin1String("L")) {      // one patient with a sensor still warming up
        s.sensor.activatedMs = now - 30 * kMinute;
        s.sensor.firstSeenMs = now - 25 * kMinute;
    }
    return s;
}

const Profile kAnna = { "{aaaaaaaa-0000-4000-8000-000000000001}", "Anna", "A", 112, 28, 0.0 };
const Profile kMarco = { "{aaaaaaaa-0000-4000-8000-000000000002}", "Marco", "M", 96, 30, 1.7 };
const Profile kLuca = { "{aaaaaaaa-0000-4000-8000-000000000003}", "Luca", "L", 124, 34, 3.1 };
// Two more invented patients for the scenario that shows every band at once (colour review).
const Profile kSara = { "{aaaaaaaa-0000-4000-8000-000000000004}", "Sara", "S", 150, 30, 2.3 };
const Profile kPia = { "{aaaaaaaa-0000-4000-8000-000000000005}", "Pia", "", 200, 30, 0.9 };

// How many scenarios the script has (the development menu cycles through them).
const int kScenarioCount = 5;

} // namespace

FakeBackend::FakeBackend(QObject *parent)
    : Backend(parent), m_scenario(0), m_reenabled(false), m_retried(false), m_started(false), m_reportHandled(false)
{
}

void FakeBackend::setScenario(int s)
{
    m_scenario = ((s % kScenarioCount) + kScenarioCount) % kScenarioCount;
    m_reenabled = m_retried = m_started = m_reportHandled = false;
    emit scenarioChanged();
    emit changed();
}

void FakeBackend::nextScenario() { setScenario(m_scenario + 1); }

QString FakeBackend::scenarioName() const
{
    switch (m_scenario) {
    case 0: return QStringLiteral("Normal");
    case 1: return QStringLiteral("Problems");
    case 2: return QStringLiteral("Service stopped");
    case 3: return QStringLiteral("Every band");
    default: return QStringLiteral("No account");
    }
}

ServiceStatus FakeBackend::serviceStatus() const
{
    if (m_statusOverride != ServiceStatus::Connecting)
        return m_statusOverride;
    return (m_scenario == 2 && !m_started) ? ServiceStatus::Stopped : ServiceStatus::Running;
}

qint64 FakeBackend::lastHeartbeatMs() const
{
    return nowMs() - (m_scenario == 2 ? 25 * kMinute : kMinute);
}

QList<SubjectSnapshot> FakeBackend::subjects() const
{
    const qint64 now = nowMs();
    QList<SubjectSnapshot> out;
    if (m_scenario == 0) {
        out << makeSubject(kAnna, now, 40 * 1000, 112, QStringLiteral("mmol/L"), 3, true);
        SubjectSnapshot marco = makeSubject(kMarco, now, 20 * 1000, 48, QStringLiteral("mg/dL"), 1, false);
        out << marco;
        SubjectSnapshot luca = makeSubject(kLuca, now, 14 * kMinute, 122, QStringLiteral("mg/dL"), 3, false);
        luca.report.state = QStringLiteral("backoff");
        luca.report.cause = QStringLiteral("transient");
        luca.report.stale = true;
        luca.report.staleCause = QStringLiteral("source-unreachable");
        luca.report.staleDetail = QStringLiteral("transient");
        luca.report.staleSinceAgeS = 11 * 60;
        out << luca;
    } else if (m_scenario == 1) {
        out << makeSubject(kAnna, now, 30 * 1000, 63, QStringLiteral("mmol/L"), 2, false);     // L
        SubjectSnapshot marco = makeSubject(kMarco, now, 29 * kMinute, 98, QStringLiteral("mg/dL"), 3, false);
        if (!m_reenabled) {
            marco.report.state = QStringLiteral("disabled-by-guard");
            marco.report.cause = QStringLiteral("connection");
            marco.report.stale = true;
            marco.report.staleCause = QStringLiteral("disabled-by-guard");
            marco.report.staleSinceAgeS = 26 * 60;
            marco.reportProposed = !m_reportHandled;          // the second guard kill (REQ-GRD-15)
        } else {
            marco.measuredAtMs = now - 20 * 1000;
        }
        out << marco;
        SubjectSnapshot luca = makeSubject(kLuca, now, 3 * kHour, 130, QStringLiteral("mg/dL"), 0, false);
        luca.report.state = QStringLiteral("needs-attention");
        luca.report.cause = QStringLiteral("terms");
        luca.report.stale = true;
        luca.report.staleCause = QStringLiteral("needs-attention");
        luca.report.staleDetail = QStringLiteral("terms");
        luca.report.staleSinceAgeS = 3 * 3600;
        out << luca;
    } else if (m_scenario == 3) {
        // One fresh patient in each band, in configuration order very low, low, in range, high, very
        // high, each with another trend code: the colour and contrast review sees every badge at once.
        out << makeSubject(kMarco, now, 30 * 1000, 48, QStringLiteral("mg/dL"), 1, false);
        out << makeSubject(kAnna, now, 50 * 1000, 63, QStringLiteral("mmol/L"), 2, false);
        out << makeSubject(kLuca, now, 20 * 1000, 120, QStringLiteral("mg/dL"), 3, false);
        out << makeSubject(kSara, now, 40 * 1000, 210, QStringLiteral("mg/dL"), 4, false);
        out << makeSubject(kPia, now, 10 * 1000, 280, QStringLiteral("mg/dL"), 5, false);
    } else if (m_scenario == 4) {
        // No account set up: the empty state of the overview and the cover.
    } else {
        const qint64 age = m_started ? 40 * 1000 : 21 * kMinute;
        out << makeSubject(kAnna, now, age, 118, QStringLiteral("mmol/L"), 3, false);
        out << makeSubject(kMarco, now, age, 99, QStringLiteral("mg/dL"), 4, false);
        for (int i = 0; i < out.size() && !m_started; ++i) {
            out[i].report.stale = true;
            out[i].report.staleCause = QStringLiteral("service-not-running");
            out[i].report.staleSinceAgeS = 20 * 60;
        }
    }
    return out;
}

void FakeBackend::retry(const QString &) { m_retried = true; emit changed(); }
void FakeBackend::reenable(const QString &) { m_reenabled = true; emit changed(); }
void FakeBackend::reportHandled(const QString &) { m_reportHandled = true; emit changed(); }
void FakeBackend::startMonitoring() { m_started = true; emit changed(); }
void FakeBackend::askAgain() { emit changed(); }

void FakeBackend::requestThresholdHistory(const QString &subjectId)
{
    const QList<ThresholdHistoryEntry> entries = m_thresholdHistory.value(subjectId);
    QTimer::singleShot(0, this, [this, subjectId, entries]() { emit thresholdHistoryLoaded(subjectId, true, entries); });
}

void FakeBackend::setThresholdHistory(const QString &subjectId, const QList<ThresholdHistoryEntry> &entries)
{
    m_thresholdHistory.insert(subjectId, entries);
}

} // namespace ui
} // namespace lm

#endif // LIBREMON_DEVSOURCE
