// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Michele Brami
#ifndef LM_SCRUBBER_H
#define LM_SCRUBBER_H

#include <QString>
#include <QStringList>

namespace lm {

// Replaces known secrets in text that is about to be shown or logged (REQ-ADP-11,
// REQ-ADP-12, REQ-SEC-06): the credentials and session blobs this process handled, and
// the personal names it received. Each secret is also matched in its JSON-escaped form.
// Memory only; the list is never written anywhere.
class Scrubber
{
public:
    // Empty secrets are ignored. Anything shorter than minLength is ignored too (used
    // for names, where a one- or two-letter name would mangle ordinary words).
    void add(const QString &secret, int minLength = 1);
    void clear() { m_secrets.clear(); }

    QString scrub(const QString &text) const;

    static QString redacted() { return QStringLiteral("[redacted]"); }

private:
    QStringList m_secrets;      // longest first
};

} // namespace lm

#endif
