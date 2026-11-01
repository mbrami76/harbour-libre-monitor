// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Michele Brami
#include "coverrank.h"

namespace lm {
namespace ui {

int coverRank(StateId id)
{
    switch (id) {
    case StateId::BandLL:
    case StateId::BandL:
        return 1;       // hypoglycaemia first: a low can become dangerous fastest
    case StateId::BandH:
    case StateId::BandHH:
        return 3;       // hyperglycaemia after the blind Subjects
    case StateId::BandN:
    case StateId::BandUnknown:
    case StateId::NoDataYet:
        return 4;
    default:
        return 2;       // every stale or attention state: not being monitored
    }
}

int pickCoverSubject(const QList<CoverCandidate> &subjects)
{
    const QList<int> first = orderForCover(subjects, 1);
    return first.isEmpty() ? -1 : first.first();
}

QList<int> orderForCover(const QList<CoverCandidate> &subjects, int maxRows)
{
    QList<int> order;
    for (int i = 0; i < subjects.size(); ++i)
        order.append(i);
    // A stable insertion sort: short lists, and a stable order keeps the configuration order for ties.
    for (int i = 1; i < order.size(); ++i) {
        const int cur = order.at(i);
        int j = i - 1;
        while (j >= 0) {
            const CoverCandidate &a = subjects.at(order.at(j));
            const CoverCandidate &b = subjects.at(cur);
            const int ra = coverRank(a.id), rb = coverRank(b.id);
            bool bFirst;
            if (rb != ra)
                bFirst = rb < ra;
            else if (ra == 1)
                bFirst = b.valueMgdl < a.valueMgdl;                  // the lower low first
            else if (ra == 2)
                bFirst = b.staleSinceAgeS > a.staleSinceAgeS;       // not monitored for longer
            else if (ra == 3)
                bFirst = b.valueMgdl > a.valueMgdl;                  // the higher high first
            else
                bFirst = b.limitDistanceMgdl < a.limitDistanceMgdl;  // nearer a limit
            if (!bFirst)
                break;
            order[j + 1] = order.at(j);
            --j;
        }
        order[j + 1] = cur;
    }
    while (order.size() > maxRows && maxRows >= 0)
        order.removeLast();
    return order;
}

} // namespace ui
} // namespace lm
