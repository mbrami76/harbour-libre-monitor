#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Michele Brami
"""The purge page in every state, and the thresholds page without its note field, on the EMULATOR (
; REQ-STO-17). Never on a phone.

The method of run_emulator_ui_x1.py: the VM's audio output off (checked, or no run) and every sink muted
(counted, or no run); the development package (its own identity) installed with its Sailjail permissions
granted in the emulator's own settings file (EMULATOR ONLY). The window runs on the SCRIPTED source (no
service, no network): LM_DEV_PURGE scripts the service's purge (short, unknown, nothing, slow, repeat) and
LM_DEV_ACTION=purge:<row>:<step>:<typed> opens the page of a patient of the configuration and plays the
user's taps (confirm, gate with a typed text, run). The configuration holds two invented patients, one with
the label "AB" and one without a label. English, then Italian (the language of the configuration file).
Each start of the window is checked for QML errors and warnings in its log; screenshots go to --out, never
into the repository; the log holds words and counts only.

    python3 tests/integration/run_emulator_ui_y1.py --rpm PATH [--out /tmp/y1app]
"""
import argparse
import json
import os
import subprocess
import sys
import time
import uuid

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import run_emulator_alerts as m6  # noqa: E402
import run_emulator_ui_alerts as r2  # noqa: E402  (shot, UI)
import run_emulator_ui_r3 as r3  # noqa: E402  (ambience, ui_settings)
import run_emulator_ui_r4 as r4  # noqa: E402  (ALLOW)

QML_PROBLEMS = "grep -i -E 'qml|error|warning|ReferenceError|TypeError|Unable to assign|is not defined' /tmp/lm-ui.log " \
               "| grep -v -i 'propertyCache\\|wayland' | head -12"


def write_config(language):
    """Two invented patients in one connection: the first with the label AB, the second without one."""
    subjects = [{"id": "{%s}" % uuid.uuid4(), "ref": "ref-a", "name": "Sim Alpha", "label": "AB"},
                {"id": "{%s}" % uuid.uuid4(), "ref": "ref-b", "name": "Sim Beta"}]
    cfg = {"format": 1, "language": language, "alerts_show_value": False,
           "connections": [{"id": "c1", "adapter": "libre", "label": "Sim", "subjects": subjects,
                            "current_interval_s": 60}]}
    local = "/tmp/y1-emu-config.json"
    with open(local, "w") as f:
        json.dump(cfg, f, indent=2)
    m6.scp(local, m6.CONF + "/config.json")
    m6.emu("chmod 600 %s/config.json" % m6.CONF)


def ui(env, wait_s):
    m6.emu("pkill -f '[/]usr/bin/harbour-libre-monitor-dev$'; sleep 1; %s nohup %s > /tmp/lm-ui.log 2>&1 &" % (env, r2.UI))
    time.sleep(wait_s)
    return m6.emu(QML_PROBLEMS).strip()


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--rpm", required=True, help="the i486 development package")
    ap.add_argument("--out", default="/tmp/y1app")
    a = ap.parse_args()
    os.makedirs(a.out, exist_ok=True)
    log = m6.Log(os.path.join(a.out, "run.log"))

    subprocess.run(["VBoxManage", "controlvm", m6.VM, "audioout", "off"], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    info = subprocess.run(["VBoxManage", "showvminfo", m6.VM, "--machinereadable"], stdout=subprocess.PIPE).stdout.decode()
    if 'audio_out="off"' not in info:
        log("REFUSED: the virtual machine's audio output is not off")
        return 2
    log("audio_out off:", True)
    muted = m6.emu("for n in $(pactl list short sinks | cut -f1); do pactl set-sink-mute $n 1; done; "
                   "echo $(pactl list sinks | grep -c 'Mute: yes') of $(pactl list short sinks | wc -l)").strip()
    log("sinks muted:", muted)
    parts = muted.split(" of ")
    if len(parts) != 2 or parts[0] != parts[1]:
        log("REFUSED: not every sink is muted")
        return 2

    m6.scp(a.rpm, "/home/defaultuser/" + os.path.basename(a.rpm))
    log("install:", m6.emu("rpm -U --force /home/defaultuser/%s 2>&1 | tail -2; rpm -q harbour-libre-monitor-dev"
                           % os.path.basename(a.rpm), user="root").strip().replace("\n", " | "))
    log("permissions granted:", m6.emu(r4.ALLOW, user="root").strip())
    m6.emu("call Stop >/dev/null 2>&1; sleep 2")
    m6.emu("pkill -f '[h]arbour-libre-monitor-dev'; sleep 2; true", user="root")
    m6.emu("mkdir -p %s %s; rm -f %s/dev-credentials %s/dev-base-url" % (m6.DATA, m6.CONF, m6.DATA, m6.DATA))
    r3.ui_settings("mg/dL")
    r3.ambience("airy")
    shots, problems = [], []

    def page(env, name, wait_s=8):
        err = ui(env, wait_s)
        log("UI", env or "(overview)", "QML problems:", err or "none")
        if err:
            problems.append(name)
        p = r2.shot(a.out, name)
        shots.append(name if p else name + " (missing)")
        log("shot:", name, "ok" if p else "MISSING")

    try:
        for lang in ("en", "it"):
            write_config(lang)
            page("LM_DEV_PAGE=StoragePage:end", "y1-%s-storage-list" % lang, 10)
            page("LM_DEV_ACTION=purge:0", "y1-%s-estimate" % lang)
            page("LM_DEV_PURGE=short LM_DEV_ACTION=purge:0", "y1-%s-short" % lang)
            page("LM_DEV_PURGE=unknown LM_DEV_ACTION=purge:0", "y1-%s-unknown" % lang)
            page("LM_DEV_PURGE=nothing LM_DEV_ACTION=purge:0", "y1-%s-nothing" % lang)
            page("LM_DEV_ACTION=purge:1", "y1-%s-no-label" % lang)
            page("LM_DEV_ACTION=purge:0:confirm", "y1-%s-confirm" % lang)
            page("LM_DEV_ACTION=purge:0:gate:ab", "y1-%s-gate-mismatch" % lang)
            page("LM_DEV_ACTION=purge:0:gate:AB", "y1-%s-gate-match" % lang)
            page("LM_DEV_PURGE=slow LM_DEV_ACTION=purge:0:run", "y1-%s-progress" % lang, 7)
            page("LM_DEV_ACTION=purge:0:run", "y1-%s-done" % lang, 10)
            page("LM_DEV_PURGE=repeat LM_DEV_ACTION=purge:0:run", "y1-%s-repeat" % lang, 10)
            page("LM_DEV_PAGE=ThresholdsPage:0", "y1-%s-thresholds" % lang, 8)
            page("LM_DEV_PAGE=ThresholdsPage:0:end", "y1-%s-thresholds-end" % lang, 10)
        r3.ambience("flare")
        page("LM_DEV_ACTION=purge:0:confirm", "y1-it-flare-confirm")
        page("LM_DEV_ACTION=purge:0:gate:AB", "y1-it-flare-gate")
        m6.emu("pkill -f '[/]usr/bin/harbour-libre-monitor-dev$'")
    finally:
        r3.ambience("airy")
    log("shots:", len(shots), "|", " ".join(shots))
    log("starts with QML problems:", len(problems), "|", " ".join(problems) or "none")
    log("done")
    return 0 if not problems else 1


if __name__ == "__main__":
    sys.exit(main())
