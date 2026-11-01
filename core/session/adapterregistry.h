// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Michele Brami
#ifndef LM_ADAPTERREGISTRY_H
#define LM_ADAPTERREGISTRY_H

#include <QList>
#include <QString>
#include <QStringList>

namespace lm {

// One built-in adapter (REQ-ADP-05, REQ-ADM-01). Out-of-tree adapters are not supported
// (REQ-ADP-16). `hostExecutable` is the development host executable that runs the adapter
// loop (host builds). In the packaged application the adapter is not a separate file: the
// service starts the application's own binary in adapter mode with `--adapter <id>`
// (REQ-ADP-15).
//
// `version` is the adapter's own version string as it will send it in `hello`. The
// service tells the scheduler this value at start, so that a connection stopped by a
// protocol change can try once more after an update without having to spawn the old adapter
// first. It is data here because the core must not depend on the adapter module; a test in
// the adapter module compares it with the adapter's own constant so the two cannot drift.
struct AdapterEntry
{
    QString id;                 // as sent in hello; also the argument after --adapter
    QString version;            // as sent in hello
    QString hostExecutable;     // file name, resolved by the caller
};

// The smallest static table: adapter identifier to what starts it.
class AdapterRegistry
{
public:
    static const AdapterRegistry &instance();

    const AdapterEntry *find(const QString &id) const;
    QStringList ids() const;

private:
    AdapterRegistry();
    QList<AdapterEntry> m_entries;
};

} // namespace lm

#endif
