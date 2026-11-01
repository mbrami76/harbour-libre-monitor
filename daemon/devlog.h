// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Michele Brami
#ifndef LM_DEVLOG_H
#define LM_DEVLOG_H

// A LOG OF WHAT THE NOTIFIER ASKS OF THE PHONE, for the phone check. Only in
// development builds (flag devsource, REQ-SIM-06): a release package neither contains nor writes it.
//
// It wraps the real backend and appends one line per post, close and `closed` signal to a file in the
// service's data directory, so a developer can read afterwards what was asked (the ids, the
// replacement, the urgency, priority, feedback and banner flags) and what the platform reported (who
// closed what and why) next to what the author saw and heard. It NEVER writes a summary, a body, a
// label, a value or a name: only numbers and hint words. Pattern: decorator.

#ifdef LIBREMON_DEVSOURCE

#include "service/alertnotification.h"
#include "time/utctime.h"

#include <QFile>
#include <QDateTime>

namespace lm {

class LoggingNoteBackend : public AlertNotificationBackend
{
public:
    // Neither is owned. `path` is the log file (appended to).
    LoggingNoteBackend(AlertNotificationBackend *inner, const QString &path) : m_inner(inner), m_path(path) {}

    quint32 publish(const NotificationRequest &r, quint32 replacesId)
    {
        const quint32 id = m_inner->publish(r, replacesId);
        write(QStringLiteral("post id=%1 replaces=%2 urgency=%3 priority=%4 feedback=%5 banner=%6 display_on=%7 removable=%8")
                  .arg(id).arg(replacesId).arg(r.hints.urgency).arg(r.hints.priority)
                  .arg(r.hints.feedback.isEmpty() ? QStringLiteral("none") : r.hints.feedback)
                  .arg(r.hints.banner ? 1 : 0).arg(r.hints.displayOn ? 1 : 0).arg(r.hints.removable ? 1 : 0));
        return id;
    }
    void close(quint32 id)
    {
        m_inner->close(id);
        write(QStringLiteral("close id=%1").arg(id));
    }
    void setClosedHandler(const ClosedHandler &handler)
    {
        m_inner->setClosedHandler([this, handler](quint32 id, uint reason) {
            write(QStringLiteral("closed id=%1 reason=%2").arg(id).arg(reason));
            if (handler)
                handler(id, reason);
        });
    }

private:
    void write(const QString &line)
    {
        QFile f(m_path);
        if (!f.open(QIODevice::WriteOnly | QIODevice::Append | QIODevice::Text))
            return;
        f.write((QDateTime::currentDateTimeUtc().toString(Qt::ISODate) + QLatin1Char(' ') + line + QLatin1Char('\n')).toUtf8());
    }

    AlertNotificationBackend *m_inner;
    QString m_path;
};

} // namespace lm

#endif // LIBREMON_DEVSOURCE

#endif
