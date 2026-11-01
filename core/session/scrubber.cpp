// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Michele Brami
#include "session/scrubber.h"

#include <QJsonArray>
#include <QJsonDocument>

namespace lm {

namespace {

QString jsonEscaped(const QString &s)
{
    QJsonArray array;
    array.append(s);
    QString text = QString::fromUtf8(QJsonDocument(array).toJson(QJsonDocument::Compact));
    // ["..."] -> the escaped content between the quotes
    return text.mid(2, text.size() - 4);
}

} // namespace

void Scrubber::add(const QString &secret, int minLength)
{
    if (secret.isEmpty() || secret.size() < minLength)
        return;
    QStringList forms;
    forms << secret << jsonEscaped(secret);
    for (int i = 0; i < forms.size(); ++i) {
        const QString &form = forms.at(i);
        if (form.isEmpty() || m_secrets.contains(form))
            continue;
        int at = 0;
        while (at < m_secrets.size() && m_secrets.at(at).size() >= form.size())
            ++at;
        m_secrets.insert(at, form);
    }
}

QString Scrubber::scrub(const QString &text) const
{
    QString out = text;
    for (int i = 0; i < m_secrets.size(); ++i)
        out.replace(m_secrets.at(i), redacted());
    return out;
}

} // namespace lm
