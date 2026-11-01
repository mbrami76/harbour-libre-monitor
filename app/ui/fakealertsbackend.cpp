// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Michele Brami
#include "fakealertsbackend.h"

#ifdef LIBREMON_DEVSOURCE

#include <QDateTime>
#include <QTimer>

namespace lm {
namespace ui {

namespace {

const qint64 kMin = 60 * 1000;
// The scripted patients of fakebackend.cpp.
const char *const kAnna = "{aaaaaaaa-0000-4000-8000-000000000001}";
const char *const kMarco = "{aaaaaaaa-0000-4000-8000-000000000002}";
const char *const kSara = "{aaaaaaaa-0000-4000-8000-000000000004}";
const char *const kPia = "{aaaaaaaa-0000-4000-8000-000000000005}";

AlertFact alert(const char *subject, const char *slot, const char *level, const char *state, qint64 sinceMs,
                int limit, const char *cause = "")
{
    AlertFact a;
    a.subject = QString::fromLatin1(subject);
    a.slot = QString::fromLatin1(slot);
    a.level = QString::fromLatin1(level);
    a.state = QString::fromLatin1(state);
    a.acknowledged = a.state == QLatin1String("acknowledged");
    a.episode = a.subject + a.slot;
    a.sinceMs = a.levelSinceMs = sinceMs;
    a.limitMgdl = limit;
    a.cause = QString::fromLatin1(cause);
    return a;
}

} // namespace

FakeAlertsBackend::FakeAlertsBackend(FakeBackend *subjects, QObject *parent)
    : AlertsBackend(parent), m_subjects(subjects)
{
    connect(m_subjects, &FakeBackend::scenarioChanged, this, [this]() { reset(); emit changed(); });
    reset();
}

void FakeAlertsBackend::reset()
{
    const qint64 now = m_subjects->nowMs();
    m_alerts.clear();
    m_silent.clear();
    m_book.restore(QVector<ThresholdChange>());
    const QList<SubjectSnapshot> subs = m_subjects->subjects();
    for (int i = 0; i < subs.size(); ++i)
        m_subjects->setThresholdHistory(subs.at(i).id, QList<ThresholdHistoryEntry>());
    switch (m_subjects->scenario()) {
    case 0:
        m_alerts << alert(kMarco, "low", "LL", "active", now - 6 * kMin, 54);
        m_silent.insert(QString::fromLatin1(kAnna), now + 75 * kMin);
        break;
    case 1:
        m_alerts << alert(kAnna, "low", "L", "active", now - 12 * kMin, 70);
        m_alerts << alert(kMarco, "missing", "MISSING", "active", now - 10 * kMin, -1, "disabled-by-guard");
        break;
    case 3: {
        m_alerts << alert(kMarco, "low", "LL", "acknowledged", now - 9 * kMin, 54);
        AlertFact anna = alert(kAnna, "low", "L", "snoozed", now - 20 * kMin, 70);
        anna.snoozeUntilMs = now + 25 * kMin;
        m_alerts << anna;
        m_alerts << alert(kSara, "high", "H", "active", now - 31 * kMin, 180);
        m_alerts << alert(kPia, "high", "HH", "active", now - 4 * kMin, 250);
        m_silent.insert(QString::fromLatin1(kPia), 0);
        // One edit two days ago: the high limit of this patient was raised (the history shows it).
        ThresholdChange c;
        c.id = QUuid(QStringLiteral("{bbbbbbbb-0000-4000-8000-000000000001}"));
        c.subject = QUuid(QString::fromLatin1(kSara));
        c.boundary = Boundary::H;
        c.previous = 180;
        c.value = 200;
        c.time = UtcTime::fromMSecsSinceEpoch(now - 2 * 24 * 60 * kMin);
        m_book.apply(QVector<ThresholdChange>() << c);
        publishHistory(QString::fromLatin1(kSara));
        break;
    }
    default:
        break;
    }
}

AlertsFacts FakeAlertsBackend::facts() const
{
    AlertsFacts f;
    if (m_subjects->scenario() == 2 && m_subjects->serviceStatus() == ServiceStatus::Stopped) {
        f.error = QStringLiteral("service-not-running");
        return f;
    }
    f.known = true;
    f.alerts = m_alerts;
    const QList<SubjectSnapshot> subs = m_subjects->subjects();
    for (int i = 0; i < subs.size(); ++i) {
        PatientAlertFacts p;
        p.subject = subs.at(i).id;
        const Thresholds t = m_book.thresholdsFor(QUuid(p.subject));
        p.ll = t.ll; p.l = t.l; p.h = t.h; p.hh = t.hh;
        p.persistenceLowS = 0; p.persistenceHighS = 900; p.missingLimitS = 900;
        p.silent = m_silent.contains(p.subject);
        p.silentUntilMs = p.silent ? m_silent.value(p.subject) : 0;
        f.patients.append(p);
    }
    f.persistenceLow.minS = 0; f.persistenceLow.maxS = 900; f.persistenceLow.defaultS = 0;
    f.persistenceHigh.minS = 0; f.persistenceHigh.maxS = 3600; f.persistenceHigh.defaultS = 900;
    f.missingLimit.minS = 300; f.missingLimit.maxS = 3600; f.missingLimit.defaultS = 900;
    return f;
}

AlertFact *FakeAlertsBackend::find(const QString &subject, const QString &slot)
{
    for (int i = 0; i < m_alerts.size(); ++i)
        if (m_alerts.at(i).subject == subject && m_alerts.at(i).slot == slot)
            return &m_alerts[i];
    return 0;
}

// Every answer comes on the next turn of the event loop, as the service's would.
#define LM_FAKE_ANSWER(action, subject, word) \
    QTimer::singleShot(0, this, [this, subject]() { emit answered(QStringLiteral(action), subject, QStringLiteral(word)); emit changed(); })

void FakeAlertsBackend::acknowledge(const QString &subject, const QString &slot)
{
    AlertFact *a = find(subject, slot);
    if (!a || a->state == QLatin1String("snoozed")) {
        LM_FAKE_ANSWER("acknowledge", subject, "not-active");
        return;
    }
    if (a->acknowledged) {
        LM_FAKE_ANSWER("acknowledge", subject, "already-acknowledged");
        return;
    }
    a->acknowledged = true;
    a->state = QStringLiteral("acknowledged");
    a->sinceMs = m_subjects->nowMs();
    LM_FAKE_ANSWER("acknowledge", subject, "done");
}

void FakeAlertsBackend::snooze(const QString &subject, const QString &slot, int minutes)
{
    AlertFact *a = find(subject, slot);
    if (!a) {
        LM_FAKE_ANSWER("snooze", subject, "not-active");
        return;
    }
    if (a->level == QLatin1String("LL") || a->level == QLatin1String("HH")) {
        LM_FAKE_ANSWER("snooze", subject, "not-snoozable");
        return;
    }
    if (minutes != 15 && minutes != 30 && minutes != 60 && minutes != 120) {
        LM_FAKE_ANSWER("snooze", subject, "bad-duration");
        return;
    }
    a->state = QStringLiteral("snoozed");
    a->sinceMs = m_subjects->nowMs();
    a->snoozeUntilMs = a->sinceMs + minutes * kMin;
    LM_FAKE_ANSWER("snooze", subject, "done");
}

void FakeAlertsBackend::setSilent(const QString &subject, int minutes)
{
    const qint64 now = m_subjects->nowMs();
    qint64 end = 0;
    if (minutes > 0) {
        end = now + minutes * kMin;
    } else if (minutes == -1) {
        // The service's rule, played here only because there is no service: the next local
        // 06:00, or the one after when the next is less than an hour away. The pages never compute it.
        QDateTime t = QDateTime::fromMSecsSinceEpoch(now).toLocalTime();
        QDateTime six(t.date(), QTime(6, 0));
        if (six <= t)
            six = six.addDays(1);
        if (six.toMSecsSinceEpoch() - now < 60 * kMin)
            six = six.addDays(1);
        end = six.toMSecsSinceEpoch();
    } else if (minutes != 0) {
        LM_FAKE_ANSWER("silent", subject, "bad-duration");
        return;
    }
    m_silent.insert(subject, end);
    LM_FAKE_ANSWER("silent", subject, "done");
}

void FakeAlertsBackend::endSilent(const QString &subject)
{
    if (!m_silent.remove(subject)) {
        LM_FAKE_ANSWER("end-silent", subject, "not-silent");
        return;
    }
    LM_FAKE_ANSWER("end-silent", subject, "done");
}

void FakeAlertsBackend::publishHistory(const QString &subject)
{
    const QVector<ThresholdChange> h = m_book.history(QUuid(subject));
    QList<ThresholdHistoryEntry> out;
    for (int i = 0; i < h.size(); ++i) {
        ThresholdHistoryEntry e;
        e.timeMs = h.at(i).time.msecsSinceEpoch();
        e.boundary = boundaryName(h.at(i).boundary);
        e.value = h.at(i).value;
        e.previous = h.at(i).previous;
        out.append(e);
    }
    m_subjects->setThresholdHistory(subject, out);
}

void FakeAlertsBackend::setThresholds(const QString &subject, int l, int h, int typedL, int typedH)
{
    ThresholdAnswer a;
    bool known = false;
    const QList<SubjectSnapshot> subs = m_subjects->subjects();
    for (int i = 0; i < subs.size(); ++i)
        known = known || subs.at(i).id == subject;
    if (!known) {
        a.result = QStringLiteral("unknown-subject");
    } else {
        // The service's reading of the challenge: any number of 0 or more typed again is a confirmation.
        Confirmation conf;
        if (typedL >= 0 || typedH >= 0)
            conf = Confirmation::typed(typedL, typedH);
        const EditOutcome out = m_book.edit(QUuid(subject), l, h, conf,
                                            UtcTime::fromMSecsSinceEpoch(m_subjects->nowMs()));
        a.result = editResultName(out.result);
        if (out.result == EditResult::Accepted) {
            const Thresholds t = m_book.thresholdsFor(QUuid(subject));
            a.ll = t.ll; a.l = t.l; a.h = t.h; a.hh = t.hh;
            publishHistory(subject);
        } else if (out.result == EditResult::Inconsistent) {
            a.lMin = out.lMin; a.lMax = out.lMax; a.hMin = out.hMin; a.hMax = out.hMax;
        } else if (out.result == EditResult::NeedsConfirmation || out.result == EditResult::ConfirmationMismatch) {
            a.globalL = out.globalL; a.globalH = out.globalH;
        }
        if (out.hasOffending)
            a.offending = boundaryName(out.offending);
    }
    QTimer::singleShot(0, this, [this, subject, a]() { emit thresholdsAnswered(subject, a); emit changed(); });
}

#undef LM_FAKE_ANSWER

} // namespace ui
} // namespace lm

#endif // LIBREMON_DEVSOURCE
