#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Michele Brami
"""Generate the derived and the small own icons of design/.

Writes into src/ (SVG masters) and export/ (SVG and PNG), then loads every new
SVG with Qt SVG as a check. Needs python3, inkscape (PNG), PyQt5 (load check).
Run from anywhere: python3 design/build.py

Not covered here: the first masters (badges, notices, no-data, launcher), made
earlier by throw-away scripts and kept as they are.
"""
import re, subprocess, sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent
SRC, EXP = ROOT / "src", ROOT / "export"

SLATE, RING, MARK = "#566070", "#E4E8ED", "#F6F7F9"          # neutral notification family
SPDX = ("<!-- SPDX-License-Identifier: GPL-3.0-or-later -->"
        "<!-- Copyright (C) 2026 Michele Brami -->")
HEAD = '<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 48 48">'


def write(path, text):
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(text + "\n", encoding="utf-8")
    return path


def png(svg, out, size):
    out.parent.mkdir(parents=True, exist_ok=True)
    subprocess.run(["inkscape", str(svg), "-o", str(out), "-w", str(size), "-h", str(size)],
                   check=True, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)


# --- to N09: alert ended, unacknowledged (derived from the badges) --------------
# Neutral plate (as the notices), the badge's own glyph in the light mark colour, and a
# dashed light ring instead of the solid one: "no longer current". (A bar under the glyph
# was tried and dropped: a triangle over a bar reads as the media symbols eject/download.)
def ended(band):
    badge = (SRC / f"alert-{band}.svg").read_text()
    polys = re.findall(r'<polygon points="([^"]+)"', badge)
    assert polys, band
    glyph = "".join(f'<polygon points="{p}" fill="{MARK}" stroke="{MARK}" '
                    f'stroke-width="1.6" stroke-linejoin="round"/>' for p in polys)
    return (SPDX + HEAD +
            f'<circle cx="24" cy="24" r="23" fill="{SLATE}"/>'
            f'<circle cx="24" cy="24" r="19.2" fill="none" stroke="{RING}" stroke-width="2.2" '
            f'stroke-dasharray="3 2.027"/>'
            f'<g transform="translate(24 24) scale(0.8) translate(-24 -24)">{glyph}</g></svg>')


# --- Own state pictograms (outline family, guide 2.1): 48 grid, stroke 3, round -----------
# White strokes on transparency like the Silica monochrome icons: the application tints them.
STROKE = 'fill="none" stroke="#FFFFFF" stroke-width="3" stroke-linecap="round" stroke-linejoin="round"'

# problem: round exclamation mark (not a triangle:)
S06 = (SPDX + HEAD + f'<circle cx="24" cy="24" r="17" {STROKE}/>'
       f'<line x1="24" y1="14.5" x2="24" y2="26" {STROKE}/>'
       '<circle cx="24" cy="32.5" r="2.2" fill="#FFFFFF"/></svg>')

# stopped for safety: shield with a bar, the one pictogram with a closed frame
S09 = (SPDX + HEAD +
       f'<path d="M24 6 L38 11 V23 C38 32 32 38 24 42 C16 38 10 32 10 23 V11 Z" {STROKE}/>'
       f'<line x1="17" y1="24" x2="31" y2="24" {STROKE}/></svg>')

# sensor: the round patch seen from above, a ring with a ringed housing in the middle
# (a side view was tried and dropped: it read as a flying saucer)
X1 = (SPDX + HEAD + f'<circle cx="24" cy="24" r="17" {STROKE}/>'
      f'<circle cx="24" cy="24" r="6.5" {STROKE}/></svg>')

PICTOS = {"state-problem": S06, "state-guard": S09, "sensor": X1}


def main():
    made = []
    for band in ("ll", "l", "h", "hh"):
        s = write(SRC / f"ended-{band}.svg", ended(band))
        png(s, EXP / "notify" / f"ended-{band}.png", 256)
        made.append(s)
    for name, text in PICTOS.items():
        s = write(SRC / f"{name}.svg", text)
        write(EXP / "ui" / f"{name}.svg", text)
        png(s, EXP / "ui" / f"{name}-72.png", 72)
        made.append(s)

    # check: Qt SVG (Tiny profile subset) must load every new drawing
    print("written:", ", ".join(s.name for s in made))
    try:
        from PyQt5.QtSvg import QSvgRenderer
    except ImportError:
        print("PyQt5.QtSvg not available: Qt SVG load check skipped")
        return 0
    bad = [s.name for s in made if not QSvgRenderer(str(s)).isValid()]
    print("Qt SVG load check:", "FAILED " + ", ".join(bad) if bad else "all valid")
    return 1 if bad else 0


if __name__ == "__main__":
    sys.exit(main())
