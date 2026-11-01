// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Michele Brami
#include "direction.h"

namespace lm {
namespace ui {

QString Direction::textKey() const
{
    switch (kind) {
    case FallingFast: return QStringLiteral("ui-dir-falling-fast");
    case Falling: return QStringLiteral("ui-dir-falling");
    case Steady: return QStringLiteral("ui-dir-steady");
    case Rising: return QStringLiteral("ui-dir-rising");
    case RisingFast: return QStringLiteral("ui-dir-rising-fast");
    case UnknownCode: return QStringLiteral("ui-dir-unknown-code");
    case Null: return QStringLiteral("ui-dir-null");
    }
    return QString();
}

// The trend glyphs decided with the round badge: one plain
// Unicode character per code, each followed by U+FE0E (VARIATION SELECTOR-15, "text presentation"), so
// that a font which also has a coloured emoji for the arrow draws the plain text form. The emulator's
// font test drew all five as text; they were accepted as they are.
QString Direction::symbol() const
{
    switch (kind) {
    case FallingFast: return QString::fromUtf8("\xE2\x86\x93\xEF\xB8\x8E");     // U+2193 down arrow
    case Falling: return QString::fromUtf8("\xE2\x86\x98\xEF\xB8\x8E");         // U+2198 south east arrow
    case Steady: return QString::fromUtf8("\xE2\x89\x88\xEF\xB8\x8E");          // U+2248 almost equal to
    case Rising: return QString::fromUtf8("\xE2\x86\x97\xEF\xB8\x8E");          // U+2197 north east arrow
    case RisingFast: return QString::fromUtf8("\xE2\x86\x91\xEF\xB8\x8E");      // U+2191 up arrow
    case UnknownCode: return QString::number(code);
    case Null: return QStringLiteral("NULL");
    }
    return QString();
}

Direction directionOfCode(bool hasCode, int code)
{
    Direction d;
    if (!hasCode) {
        d.kind = Direction::Null;
        return d;
    }
    d.code = code;
    // The provisional mapping on 2026-10-16. Anything outside
    // 1..5 is shown as received, never mapped to an arrow.
    switch (code) {
    case 1: d.kind = Direction::FallingFast; break;
    case 2: d.kind = Direction::Falling; break;
    case 3: d.kind = Direction::Steady; break;
    case 4: d.kind = Direction::Rising; break;
    case 5: d.kind = Direction::RisingFast; break;
    default: d.kind = Direction::UnknownCode; break;
    }
    return d;
}

} // namespace ui
} // namespace lm
