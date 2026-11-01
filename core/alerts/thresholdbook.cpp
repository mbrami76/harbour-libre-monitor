// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Michele Brami
#include "alerts/thresholdbook.h"

#include <algorithm>

namespace lm {

QString boundaryName(Boundary b) { return b == Boundary::L ? QStringLiteral("L") : QStringLiteral("H"); }

bool boundaryFromName(const QString &name, Boundary *b)
{
    if (name == QLatin1String("L")) { *b = Boundary::L; return true; }
    if (name == QLatin1String("H")) { *b = Boundary::H; return true; }
    return false;
}

QString editResultName(EditResult r)
{
    switch (r) {
    case EditResult::Accepted: return QStringLiteral("accepted");
    case EditResult::NoChange: return QStringLiteral("no-change");
    case EditResult::InvalidValue: return QStringLiteral("invalid-value");
    case EditResult::Inconsistent: return QStringLiteral("inconsistent");
    case EditResult::NeedsConfirmation: return QStringLiteral("needs-confirmation");
    case EditResult::ConfirmationMismatch: return QStringLiteral("confirmation-mismatch");
    case EditResult::UnknownSubject: return QStringLiteral("unknown-subject");
    case EditResult::NotSaved: return QStringLiteral("not-saved");
    }
    return QString();
}

ThresholdBook::ThresholdBook(const IdFactory &ids)
    : m_ids(ids ? ids : IdFactory([]() { return QUuid::createUuid(); }))
{
}

bool ThresholdBook::before(const ThresholdChange &a, const ThresholdChange &b)
{
    if (a.time != b.time)
        return a.time < b.time;
    return a.id.toString() < b.id.toString();
}

void ThresholdBook::insertSorted(SubjectLedger *ledger, const ThresholdChange &c)
{
    QVector<ThresholdChange>::iterator at =
        std::upper_bound(ledger->entries.begin(), ledger->entries.end(), c, &ThresholdBook::before);
    ledger->entries.insert(at, c);
}

Thresholds ThresholdBook::thresholdsFor(const QUuid &subject) const
{
    Thresholds t;
    const QHash<QUuid, SubjectLedger>::const_iterator it = m_subjects.constFind(subject);
    if (it == m_subjects.constEnd())
        return t;
    // Entries are in time order, so the last one of each boundary is the one in force.
    const QVector<ThresholdChange> &e = it->entries;
    for (int i = 0; i < e.size(); ++i) {
        if (e.at(i).boundary == Boundary::L) t.l = e.at(i).value;
        else t.h = e.at(i).value;
    }
    return t;
}

QUuid ThresholdBook::idInForce(const QUuid &subject) const
{
    const QHash<QUuid, SubjectLedger>::const_iterator it = m_subjects.constFind(subject);
    if (it == m_subjects.constEnd() || it->entries.isEmpty())
        return QUuid();
    return it->entries.last().id;
}

EditOutcome ThresholdBook::check(const QUuid &subject, int l, int h, const Confirmation &conf,
                                 const UtcTime &now) const
{
    EditOutcome out;
    const Thresholds current = thresholdsFor(subject);
    out.thresholds = current;

    // A whole positive number in a plausible range. 1000 mg/dL is above anything a meter shows;
    // the cap only stops a typo from becoming a threshold, consistency does the real work.
    if (l <= 0 || h <= 0 || l > 1000 || h > 1000 || !now.isValid()) {
        out.result = EditResult::InvalidValue;
        out.hasOffending = true;
        out.offending = (l <= 0 || l > 1000) ? Boundary::L : Boundary::H;
        return out;
    }
    const Thresholds wanted(kGlobalLL, l, h, kGlobalHH);
    if (!wanted.isConsistent()) {
        out.result = EditResult::Inconsistent;
        out.lMin = kGlobalLL + 1;
        out.lMax = h - 1;
        out.hMin = l + 1;
        out.hMax = kGlobalHH - 1;
        out.hasOffending = true;
        out.offending = (l <= kGlobalLL || l >= h) ? Boundary::L : Boundary::H;
        return out;
    }
    if (l == current.l && h == current.h) {
        out.result = EditResult::NoChange;
        return out;
    }

    // Looser than the global value: L lower, or H higher (REQ-THR-02). The reference is the GLOBAL
    // value, not the previous one, so going from 60 to 65 is still a deviation that needs a reason.
    // Only a boundary that MOVES is judged: tightening L while an older, already explained,
    // loosened H stays as it is must not ask for the explanation again.
    const bool looserL = l != current.l && l < kGlobalL;
    const bool looserH = h != current.h && h > kGlobalH;
    if (looserL || looserH) {
        if (!conf.confirmed) {
            out.result = EditResult::NeedsConfirmation;
            return out;
        }
        // The challenge: the value typed again must be the one asked for. The UI does not show it
        // while the caregiver types, so a blind confirmation cannot pass.
        if ((looserL && conf.typedL != l) || (looserH && conf.typedH != h)) {
            out.result = EditResult::ConfirmationMismatch;
            out.hasOffending = true;
            out.offending = (looserL && conf.typedL != l) ? Boundary::L : Boundary::H;
            return out;
        }
    }

    // One entry per boundary that moves. A boundary kept as it is gets no entry.
    if (l != current.l) {
        ThresholdChange c;
        c.id = m_ids();
        c.subject = subject;
        c.boundary = Boundary::L;
        c.value = l;
        c.previous = current.l;
        c.time = now;
        out.changes.append(c);
    }
    if (h != current.h) {
        ThresholdChange c;
        c.id = m_ids();
        c.subject = subject;
        c.boundary = Boundary::H;
        c.value = h;
        c.previous = current.h;
        c.time = now;
        out.changes.append(c);
    }
    out.thresholds = wanted;
    out.result = EditResult::Accepted;
    return out;
}

void ThresholdBook::apply(const QVector<ThresholdChange> &changes)
{
    for (int i = 0; i < changes.size(); ++i) {
        const ThresholdChange &c = changes.at(i);
        SubjectLedger &ledger = m_subjects[c.subject];
        bool seen = false;
        for (int j = 0; j < ledger.entries.size() && !seen; ++j)
            seen = ledger.entries.at(j).id == c.id;
        if (!seen)
            insertSorted(&ledger, c);
    }
}

EditOutcome ThresholdBook::edit(const QUuid &subject, int l, int h, const Confirmation &conf,
                                const UtcTime &now)
{
    EditOutcome out = check(subject, l, h, conf, now);
    if (out.result == EditResult::Accepted)
        apply(out.changes);
    return out;
}

QVector<ThresholdChange> ThresholdBook::history(const QUuid &subject) const
{
    const QHash<QUuid, SubjectLedger>::const_iterator it = m_subjects.constFind(subject);
    return it == m_subjects.constEnd() ? QVector<ThresholdChange>() : it->entries;
}

void ThresholdBook::restore(const QVector<ThresholdChange> &entries)
{
    m_subjects.clear();
    apply(entries);
}

} // namespace lm
