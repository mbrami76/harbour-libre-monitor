// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Michele Brami
#include "service/residentnotice.h"

namespace lm {

NoteText ResidentNoticeComposer::compose(const Notice &n) const
{
    const NotificationTexts texts = m_presenter ? m_presenter->texts() : NotificationTexts();
    ResidentInput in = m_source ? m_source->facts() : ResidentInput();
    in.updated = n.lastHeartbeat;
    in.cause = n.cause;
    in.stoppedAt = n.stoppedAt;
    switch (n.state) {
    case NoticeState::Active:
        return texts.residentActive(in);
    case NoticeState::AwaitingApproval:
        // The first waiting connection, by the labels of its Subjects (the notice that asks for the
        // approval says the same).
        in.connectionLabel = m_presenter ? m_presenter->subjectLabelsOf(n.connection) : QStringLiteral("?");
        return texts.residentAwaitingApproval(in);
    case NoticeState::Attention:
        return texts.residentAttention(in);
    case NoticeState::Stopped:
        return texts.residentStopped(in);
    }
    return NoteText();
}

} // namespace lm
