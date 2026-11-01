#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Michele Brami
"""The storage page without export and import, and the "older" and "newer" service states, on the EMULATOR
. Never on a phone.

The method of R2 to R4 (run_emulator_ui_r4.py): the VM's audio output off (checked, or no run) and every
sink muted (counted); the development package (its own identity) installed; its Sailjail permissions granted
in the emulator's own settings file (EMULATOR ONLY); dev-base-url and dev-credentials point at the simulator
on the PC. Synthetic data only.

Two sources:
  A. the development package's own SERVICE (LM_DEV_SOURCE=service) with the simulator: the storage page
     with the core's real estimate, rebuild need and space check (the store holds only recent readings, so
     the estimate removes nothing: the page says there is nothing to compact);
  B. the SCRIPTED source (no service): the storage page with an estimate (Compact enabled), the confirmation
     (LM_DEV_ACTION=compact-confirm), the space short and unknown (LM_DEV_STORAGE), the low-space line on
     the overview (LM_DEV_STORAGE=low), and the strip with an older and a newer service (LM_DEV_SERVICE);
     in English and in Italian (the language written in the configuration file).
Each start of the window is checked for QML errors and warnings in its log; screenshots go to --out, never
into the repository; the log holds counts and words only (no value, no name).

    python3 tests/integration/run_emulator_ui_x1.py --rpm PATH [--port 18495] [--out /tmp/x1app]
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
import run_emulator_ui_r4 as r4  # noqa: E402  (ALLOW with Documents)
import run_integration as base  # noqa: E402
import run_service_integration as svc  # noqa: E402

SEED = 53
# QML problems in the window's log; Wayland and property-cache chatter of the emulator are not ours.
QML_PROBLEMS = "grep -i -E 'qml|error|warning|ReferenceError|TypeError|Unable to assign|is not defined' /tmp/lm-ui.log " \
               "| grep -v -i 'propertyCache\\|wayland' | head -12"


def ui(env, wait_s, service):
    """Starts the window afresh with these development-only variables; returns its QML problem lines."""
    src = "LM_DEV_SOURCE=service" if service else ""
    m6.emu("pkill -f '[/]usr/bin/harbour-libre-monitor-dev$'; sleep 1; %s %s nohup %s > /tmp/lm-ui.log 2>&1 &"
           % (env, src, r2.UI))
    time.sleep(wait_s)
    return m6.emu(QML_PROBLEMS).strip()


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--rpm", required=True, help="the i486 development package")
    ap.add_argument("--port", type=int, default=18495)
    ap.add_argument("--out", default="/tmp/x1app")
    ap.add_argument("--quick", action="store_true", help="only the confirmation, scripted, in English and Italian")
    a = ap.parse_args()
    os.makedirs(a.out, exist_ok=True)
    log = m6.Log(os.path.join(a.out, "run.log"))

    # ---- no sound, or no run ----------------------------------------------------------------------------
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
    m6.emu("call Stop >/dev/null 2>&1; sleep 3")
    m6.emu("pkill -f '[h]arbour-libre-monitor-dev'; sleep 2; true", user="root")
    m6.emu("mkdir -p %s %s; rm -f %s/notify.log %s/store.db* %s/journal.db* %s/config.json; "
           "dconf write /lipstick/do_not_disturb false" % (m6.DATA, m6.CONF, m6.DATA, m6.DATA, m6.DATA, m6.CONF))
    r3.ui_settings("mg/dL")

    scen = {
        "name": "emulator-ui-x1", "seed": SEED, "start_utc": "now", "timezone": "Europe/Rome",
        "accounts": [{"email": "follower@example.invalid", "password": base.PASSWORD, "unit": "mg/dL",
                      "country": "IT", "subjects": ["a"]}],
        "subjects": [{"key": "a", "first_name": "Sim", "last_name": "Subject Emulator", "sensor": {"age_days": 5},
                      "profile": {"baseline": 115.0, "daily_amp": 0.0, "meals": [], "night_dip": None, "noise": 0.0,
                                  "drift_amp": 0.0, "excursions": []}}],
    }
    scen_path = os.path.join(a.out, "scenario.json")
    with open(scen_path, "w") as f:
        json.dump(scen, f)
    sim = subprocess.Popen([sys.executable, "-m", "libresim", "--scenario", scen_path, "--host", "127.0.0.1",
                            "--port", str(a.port)], cwd=base.SIMDIR, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    shots, problems = [], []

    def page(env, name, wait_s=10, service=False):
        err = ui(env, wait_s, service)
        log("UI", ("[service] " if service else "[scripted] ") + (env or "(overview)"), "QML problems:", err or "none")
        if err:
            problems.append(name)
        p = r2.shot(a.out, name)
        shots.append(name if p else name + " (missing)")
        log("shot:", name, "ok" if p else "MISSING")

    try:
        time.sleep(2)
        url = "http://10.0.2.2:%d" % a.port
        cred = os.path.join(a.out, "cred")
        with open(cred, "w") as f:
            f.write("username=follower@example.invalid\npassword=%s\n" % base.PASSWORD)
        m6.scp(cred, m6.DATA + "/dev-credentials")
        os.remove(cred)
        m6.emu("chmod 600 %s/dev-credentials; echo %s > %s/dev-base-url" % (m6.DATA, url, m6.DATA))
        S = "{%s}" % uuid.uuid4()
        ref = svc.patient_ref(SEED, "a")
        m6.write_config(S, ref, "en")
        log("start:", m6.emu("call Start").strip().replace("\n", " "))
        if a.quick:
            m6.emu("call Stop >/dev/null 2>&1; sleep 3")
            r3.ambience("airy")
            page("LM_DEV_ACTION=compact-confirm", "x1q-en-confirm", 8)
            m6.write_config(S, ref, "it")
            page("LM_DEV_ACTION=compact-confirm", "x1q-it-confirm", 8)
            m6.emu("pkill -f '[/]usr/bin/harbour-libre-monitor-dev$'")
            log("starts with QML problems:", len(problems), "|", " ".join(problems) or "none")
            return 0 if not problems else 1
        time.sleep(90)                                  # some readings in the store
        log("service processes:", m6.emu("pgrep -f '[h]arbour-libre-monitor-dev --service' | wc -l").strip())
        status = m6.emu("call Status")
        log("Status() names app_version:", '"app_version"' in status)
        r3.ambience("airy")

        # ---- A. the service's own facts ----------------------------------------------------------------
        page("LM_DEV_PAGE=StoragePage", "x1-en-service-storage", 12, service=True)
        page("LM_DEV_PAGE=StoragePage:end", "x1-en-service-storage-end", 14, service=True)
        page("", "x1-en-service-overview", 10, service=True)
        m6.emu("pkill -f '[/]usr/bin/harbour-libre-monitor-dev$'")
        m6.emu("call Stop >/dev/null 2>&1; sleep 3")

        # ---- B. the scripted source, English ----------------------------------------------------------
        page("LM_DEV_PAGE=StoragePage", "x1-en-storage", 8)
        page("LM_DEV_PAGE=StoragePage:end", "x1-en-storage-end", 10)
        page("LM_DEV_ACTION=compact-confirm", "x1-en-confirm", 8)
        page("LM_DEV_STORAGE=short LM_DEV_PAGE=StoragePage:end", "x1-en-storage-short", 10)
        page("LM_DEV_STORAGE=unknown LM_DEV_PAGE=StoragePage:end", "x1-en-storage-unknown", 10)
        page("LM_DEV_STORAGE=low", "x1-en-overview-low", 8)
        page("LM_DEV_STORAGE=low LM_DEV_PAGE=StoragePage", "x1-en-storage-low", 8)
        page("LM_DEV_SERVICE=older", "x1-en-overview-older", 8)
        page("LM_DEV_SERVICE=newer", "x1-en-overview-newer", 8)
        r3.ambience("flare")
        page("LM_DEV_ACTION=compact-confirm", "x1-en-flare-confirm", 8)
        page("LM_DEV_STORAGE=short LM_DEV_PAGE=StoragePage:end", "x1-en-flare-storage-short", 10)
        r3.ambience("airy")

        # ---- B. the scripted source, Italian ----------------------------------------------------------
        m6.write_config(S, ref, "it")
        page("LM_DEV_PAGE=StoragePage", "x1-it-storage", 8)
        page("LM_DEV_PAGE=StoragePage:end", "x1-it-storage-end", 10)
        page("LM_DEV_ACTION=compact-confirm", "x1-it-confirm", 8)
        page("LM_DEV_STORAGE=short LM_DEV_PAGE=StoragePage:end", "x1-it-storage-short", 10)
        page("LM_DEV_STORAGE=unknown LM_DEV_PAGE=StoragePage:end", "x1-it-storage-unknown", 10)
        page("LM_DEV_STORAGE=low", "x1-it-overview-low", 8)
        page("LM_DEV_SERVICE=older", "x1-it-overview-older", 8)
        page("LM_DEV_SERVICE=newer", "x1-it-overview-newer", 8)
        log("UI log lines with QML problems (last start):", m6.emu(QML_PROBLEMS + " | wc -l").strip())
        m6.emu("pkill -f '[/]usr/bin/harbour-libre-monitor-dev$'")
    finally:
        sim.terminate()
        r3.ambience("airy")
        m6.emu("call Stop >/dev/null 2>&1; rm -f %s/dev-credentials" % m6.DATA)
    log("shots:", len(shots), "|", " ".join(shots))
    log("starts with QML problems:", len(problems), "|", " ".join(problems) or "none")
    log("done")
    return 0 if not problems else 1


if __name__ == "__main__":
    sys.exit(main())
