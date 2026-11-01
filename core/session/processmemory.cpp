// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Michele Brami
#include "session/processmemory.h"

#include <QFile>

namespace lm {

qint64 parseVmHwmBytes(const QByteArray &statusText)
{
    const QList<QByteArray> lines = statusText.split('\n');
    for (int i = 0; i < lines.size(); ++i) {
        const QByteArray &line = lines.at(i);
        if (!line.startsWith("VmHWM:"))
            continue;
        // "VmHWM:     26124 kB": a number and the unit the kernel always writes.
        const QList<QByteArray> parts = line.mid(6).simplified().split(' ');
        if (parts.size() != 2 || parts.at(1) != "kB")
            return -1;
        bool ok = false;
        const qint64 kb = parts.at(0).toLongLong(&ok);
        if (!ok || kb < 0 || kb > (Q_INT64_C(1) << 40))
            return -1;
        return kb * 1024;
    }
    return -1;
}

qint64 readPeakResidentBytes(qint64 pid)
{
    if (pid <= 0)
        return -1;
    QFile file(QStringLiteral("/proc/%1/status").arg(pid));
    if (!file.open(QIODevice::ReadOnly))
        return -1;
    // The file is a few hundred bytes of text; QFile::readAll() copes with the kernel's
    // zero-size report for /proc files.
    return parseVmHwmBytes(file.readAll());
}

} // namespace lm
