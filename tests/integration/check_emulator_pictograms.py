#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Michele Brami
"""Checks that every pictogram the interface names exists.

Why: a Silica theme icon is asked for by name, and a name that does not exist draws NOTHING, with no
error. An early version had such a placeholder (`icon-m-key`, the theme has
`icon-m-keys`). This script collects every theme icon name the QML uses (the pictogram list in
app/qml/components/Style.js and any literal `image://theme/icon-...` in the pages, the cover action
included) and looks for each in the theme of the EMULATOR image, at every scale Silica may pick
(/usr/share/themes/sailfish-default/silica/z*/icons-monochrome/ and .../icons/ for the cover icons).
The own drawings (`own:<file>`) must exist in design/export/ui. Emulator only; it reads files and
changes nothing there.

    python3 tests/integration/check_emulator_pictograms.py      (exit 0 = all found)
"""
import glob
import os
import re
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.dirname(os.path.dirname(HERE))
KEY = os.path.expanduser("~/SailfishOS/vmshare/ssh/private_keys/sdk")
SSH = ["ssh", "-o", "ConnectTimeout=15", "-o", "StrictHostKeyChecking=no", "-o", "UserKnownHostsFile=/dev/null",
       "-o", "LogLevel=ERROR", "-i", KEY, "-p", "2223", "defaultuser@localhost"]
THEME = "/usr/share/themes/sailfish-default/silica"


def names_in_sources():
    silica, own = set(), set()
    files = [os.path.join(REPO, "app/qml/components/Style.js")]
    files += glob.glob(os.path.join(REPO, "app/qml/**/*.qml"), recursive=True)
    for path in files:
        text = open(path, encoding="utf-8").read()
        silica.update(re.findall(r'"(icon-(?:m|s|l|cover)-[a-z0-9-]+)"', text))
        silica.update(re.findall(r'image://theme/(icon-(?:m|s|l|cover)-[a-z0-9-]+)', text))
        own.update(re.findall(r'"own:([a-z0-9-]+)"', text))
    return sorted(silica), sorted(own)


def main():
    silica, own = names_in_sources()
    listing = subprocess.run(SSH + ["ls %s/z*/icons-monochrome %s/z*/icons 2>/dev/null" % (THEME, THEME)],
                             stdin=subprocess.DEVNULL, stdout=subprocess.PIPE, timeout=60).stdout.decode()
    scales = len(re.findall(r'^/.*:$', listing, re.M))
    present = set(re.findall(r'^(icon-[a-z0-9-]+)\.(?:png|svg)$', listing, re.M))
    missing = [n for n in silica if n not in present]
    own_missing = [n for n in own if not os.path.exists(os.path.join(REPO, "design/export/ui", n + ".svg"))]
    print("theme folders read: %d; Silica names used: %d; own drawings: %d" % (scales, len(silica), len(own)))
    for n in silica:
        print("  %-28s %s" % (n, "found" if n in present else "MISSING"))
    for n in own:
        print("  own:%-24s %s" % (n, "MISSING" if n in own_missing else "found"))
    ok = scales > 0 and not missing and not own_missing
    print("RESULT:", "pass" if ok else "FAIL")
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
