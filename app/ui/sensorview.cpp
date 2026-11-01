// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Michele Brami
#include "sensorview.h"

#include <QCryptographicHash>

namespace lm {
namespace ui {

QString shortSensorId(const QString &storedId)
{
    if (storedId.isEmpty())
        return QString();
    const QByteArray hex = QCryptographicHash::hash(storedId.toUtf8(), QCryptographicHash::Sha256).toHex();
    return QString::fromLatin1(hex.left(4)) + QLatin1Char('-') + QString::fromLatin1(hex.mid(4, 4));
}

QVariantMap sensorMap(const SensorFacts &f, qint64 nowMs)
{
    QVariantMap m;
    m.insert(QStringLiteral("known"), f.known);
    if (!f.known)
        return m;
    m.insert(QStringLiteral("shortId"), shortSensorId(f.id));
    m.insert(QStringLiteral("activatedMs"), double(f.activatedMs));
    // The warm-up end is shown only when both parts are known; the service's scheduler uses the same sum
    // for its warm-up cause (core/scheduler, sensorReadyMs), so the page and the no-data alerts agree.
    const qint64 end = (f.activatedMs >= 0 && f.warmupMinutes >= 0) ? f.activatedMs + qint64(f.warmupMinutes) * 60000 : -1;
    m.insert(QStringLiteral("warmupEndMs"), double(end));
    m.insert(QStringLiteral("warmingUp"), end >= 0 && nowMs < end);
    m.insert(QStringLiteral("firstSeenMs"), double(f.firstSeenMs));
    m.insert(QStringLiteral("lastSeenMs"), double(f.lastSeenMs));
    return m;
}

} // namespace ui
} // namespace lm
