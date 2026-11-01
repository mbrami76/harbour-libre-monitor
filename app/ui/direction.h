// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Michele Brami
#ifndef LM_UI_DIRECTION_H
#define LM_UI_DIRECTION_H

// The direction of the glucose value (rising, falling, steady), from the source's trend arrow
// of the newest current reading (D-A).
//
// Why a table in one function: the meaning of the arrow's values is NOT verified.
// The author decided to map them provisionally (3 steady, 1 and 5 fast, 2 and 4
// slow) and to test with real data. If the test shows another meaning, the change is the one
// table in direction.cpp and its test.
//
// The arrow is never stored: it belongs to one reading and is only handed to the UI in memory.
// An unknown code is never guessed: the UI shows what was received (the code, or NULL).

#include <QString>

namespace lm {
namespace ui {

struct Direction
{
    enum Kind { FallingFast, Falling, Steady, Rising, RisingFast, UnknownCode, Null };

    Direction() : kind(Null), code(0) {}

    Kind kind;
    int code;           // the received code, for UnknownCode

    bool isArrow() const { return kind != UnknownCode && kind != Null; }
    // The text key of the word ("ui-dir-rising",...),
    QString textKey() const;
    // An arrow character for an arrow kind; for UnknownCode the code as text; "NULL" for Null.
    QString symbol() const;
};

// `hasCode` is false when the source sent no arrow (or the service has none yet).
Direction directionOfCode(bool hasCode, int code);

} // namespace ui
} // namespace lm

#endif
