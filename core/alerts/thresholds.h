// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Michele Brami
#ifndef LM_THRESHOLDS_H
#define LM_THRESHOLDS_H

// The four glucose thresholds (REQ-THR-01).
//
// They are WHOLE NUMBERS OF mg/dL, held and compared as numbers: LL 54, L 70, H 180, HH 250. The
// requirements state the defaults in mmol/L (3.0, 3.9, 10.0, 13.9), but converted exactly those
// are 54.05, 70.26, 180.16 and 250.42 mg/dL, which would make a reading of 70 count as low and a
// reading of 54 as urgent low, while the consensus the defaults come from says "below 70" and
// "below 54" (still to be checked against the source text). The mg/dL numbers
// are the thresholds; the mmol/L figures are only a display.
//
// A caregiver may type an edit in mmol/L with one decimal. It is converted and ROUNDED ONCE to a
// whole mg/dL at the moment it is made, and that rounded number is what is stored, compared and
// shown back. Nothing here ever compares a rounded display value.

#include <QString>

namespace lm {

// The global values. LL and HH are always these (REQ-THR-01); L and H are the starting point of
// every Subject and the reference for "looser than the global value" (REQ-THR-02).
// PROVISIONAL: from the consensus as the author remembers it, not yet checked.
const int kGlobalLL = 54;
const int kGlobalL = 70;
const int kGlobalH = 180;
const int kGlobalHH = 250;

struct Thresholds
{
    Thresholds() : ll(kGlobalLL), l(kGlobalL), h(kGlobalH), hh(kGlobalHH) {}
    Thresholds(int ll_, int l_, int h_, int hh_) : ll(ll_), l(l_), h(h_), hh(hh_) {}

    int ll, l, h, hh;   // mg/dL; below ll: LL, below l: L, above h: H, above hh: HH

    // LL < L < H < HH, all positive. Anything else is refused when it
    // is edited and never reaches the engine.
    bool isConsistent() const { return ll > 0 && ll < l && l < h && h < hh; }

    bool operator==(const Thresholds &o) const
    { return ll == o.ll && l == o.l && h == o.h && hh == o.hh; }
    bool operator!=(const Thresholds &o) const { return !(*this == o); }
};

// Converts a caregiver's edit in mmol/L to whole mg/dL, rounded ONCE (half away from zero, on the
// decimal text, as REQ-DAT-12 rounds). False for a value that is not a finite positive number.
bool mmolEditToMgDl(double mmol, int *mgdl);

// The one-decimal mmol/L figure to SHOW for a stored threshold (display only, never compared).
double mgDlToMmolForDisplay(int mgdl);

} // namespace lm

#endif
