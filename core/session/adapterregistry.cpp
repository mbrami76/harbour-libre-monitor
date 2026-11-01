// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Michele Brami
#include "session/adapterregistry.h"

namespace lm {

AdapterRegistry::AdapterRegistry()
{
    AdapterEntry libre;
    libre.id = QStringLiteral("libre");
    libre.version = QStringLiteral("0.1.0");        // equals libreAdapterVersion(), see tst_libreadapter
    libre.hostExecutable = QStringLiteral("libre-adapter-host");
    m_entries.append(libre);
}

const AdapterRegistry &AdapterRegistry::instance()
{
    static const AdapterRegistry registry;
    return registry;
}

const AdapterEntry *AdapterRegistry::find(const QString &id) const
{
    for (int i = 0; i < m_entries.size(); ++i)
        if (m_entries.at(i).id == id)
            return &m_entries.at(i);
    return 0;
}

QStringList AdapterRegistry::ids() const
{
    QStringList ids;
    for (int i = 0; i < m_entries.size(); ++i)
        ids << m_entries.at(i).id;
    return ids;
}

} // namespace lm
