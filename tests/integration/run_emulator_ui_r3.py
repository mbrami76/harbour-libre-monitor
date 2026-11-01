#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Michele Brami
"""The pages of  (thresholds, alert times, settings) and the two checks carried over from R2 on the
EMULATOR, with the development package's own service and the simulator. Never on
a phone.

Python 3 standard library only; the method of R2 (run_emulator_ui_alerts.py) and the service
(run_emulator_alerts.py): the VM's audio output off (checked, or no run) and the emulator's sinks muted;
the development package (its own identity) installed; its Sailjail permissions granted in the emulator's own
settings file (EMULATOR ONLY, a copy kept); dev-base-url and dev-credentials point at the simulator on the
PC; a synthetic patient goes very low a few minutes after the start.

What it looks at, by screenshots through Lipstick (left in --out, never in the repository) and by words and
counts in the log (no value, no name):
  1. the R2 pages in ITALIAN (the language written into the configuration file, which the window takes at its
     start since R3): the tile with the alert and Acknowledge, the patient page, the alerts page, silent mode;
  2. the fix of a4cf8e7: the reading box right after a restart of the service (no trend yet);
  3. the R3 pages in both languages and both ambiences: the thresholds page (top and history), the settings
     (top and patients); the challenge, a wrong value typed again, an impossible order, an accepted looser
     limit with its history, done through LM_DEV_ACTION (the page's own Save and the challenge's accept);
     the limit in force read back from Alerts() (said as changed or not, never as a number).

    python3 tests/integration/run_emulator_ui_r3.py --rpm PATH [--port 18493] [--out /tmp/a8r3]
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
import run_emulator_ui_alerts as r2  # noqa: E402  (wake, shot, ui)
import run_integration as base  # noqa: E402
import run_service_integration as svc  # noqa: E402

SEED = 41


def ambience(name):
    m6.emu("dbus-send --session --type=method_call --dest=com.jolla.ambienced /com/jolla/ambienced "
           "com.jolla.ambienced.setAmbience string:file:///usr/share/ambience/%s/%s.ambience" % (name, name))
    time.sleep(4)


def ui_settings(unit):
    """The UI's own preferences of the development identity: the display unit, the statement accepted."""
    local = "/tmp/a8r3-ui-settings.json"
    with open(local, "w") as f:
        json.dump({"display_unit": unit, "safety_accepted": True, "cover_shows_name": True}, f)
    m6.scp(local, m6.DATA + "/ui-settings.json")


def low_limit():
    th = (m6.alerts().get("thresholds") or [{}])[0]
    return th.get("l")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--rpm", required=True, help="the i486 development package")
    ap.add_argument("--port", type=int, default=18493)
    ap.add_argument("--out", default="/tmp/a8r3")
    a = ap.parse_args()
    os.makedirs(a.out, exist_ok=True)
    log = m6.Log(os.path.join(a.out, "run.log"))

    # ---- no sound, or no run ---------------------------------------------------------------------
    subprocess.run(["VBoxManage", "controlvm", m6.VM, "audioout", "off"], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    info = subprocess.run(["VBoxManage", "showvminfo", m6.VM, "--machinereadable"], stdout=subprocess.PIPE).stdout.decode()
    if 'audio_out="off"' not in info:
        log("REFUSED: the virtual machine's audio output is not off")
        return 2
    log("audio_out off:", True)
    log("sinks muted:", m6.emu("for n in $(pactl list short sinks | cut -f1); do pactl set-sink-mute $n 1; done; "
                               "echo $(pactl list sinks | grep -c 'Mute: yes') of $(pactl list short sinks | wc -l)").strip())

    # ---- the development package and its permissions (EMULATOR ONLY) --------------------------------------
    m6.scp(a.rpm, "/home/defaultuser/" + os.path.basename(a.rpm))
    log("install:", m6.emu("rpm -U --force /home/defaultuser/%s 2>&1 | tail -2; rpm -q harbour-libre-monitor-dev"
                           % os.path.basename(a.rpm), user="root").strip().replace("\n", " | "))
    log("permissions:", m6.emu(m6.ALLOW, user="root").strip())
    m6.emu("call Stop >/dev/null 2>&1; sleep 3")
    m6.emu("pkill -f '[h]arbour-libre-monitor-dev'; sleep 2; true", user="root")
    m6.emu("mkdir -p %s %s; rm -f %s/notify.log %s/store.db* %s/journal.db* %s/config.json; dconf write /lipstick/do_not_disturb false"
           % (m6.DATA, m6.CONF, m6.DATA, m6.DATA, m6.DATA, m6.CONF))
    ui_settings("mg/dL")

    t0 = time.time()
    scen = {
        "name": "emulator-ui-r3", "seed": SEED, "start_utc": "now", "timezone": "Europe/Rome",
        "accounts": [{"email": "follower@example.invalid", "password": base.PASSWORD, "unit": "mg/dL",
                      "country": "IT", "subjects": ["a"]}],
        "subjects": [{"key": "a", "first_name": "Sim", "last_name": "Subject Emulator", "sensor": {"age_days": 5},
                      "profile": {"baseline": 115.0, "daily_amp": 0.0, "meals": [], "night_dip": None, "noise": 0.0,
                                  "drift_amp": 0.0, "excursions": [
                                      {"at_utc": m6.iso(t0 + 2 * 60), "duration_min": 40, "peak": 25}]}}],
    }
    scen_path = os.path.join(a.out, "scenario.json")
    with open(scen_path, "w") as f:
        json.dump(scen, f)
    sim = subprocess.Popen([sys.executable, "-m", "libresim", "--scenario", scen_path, "--host", "127.0.0.1",
                            "--port", str(a.port)], cwd=base.SIMDIR, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    shots = []

    def shot(name):
        p = r2.shot(a.out, name)
        shots.append(name if p else name + " (missing)")
        log("shot:", name, "ok" if p else "MISSING")

    def page(env, name, wait_s=10):
        err = r2.ui(env, wait_s)
        log("UI", env or "(overview)", "errors:", err or "none")
        shot(name)

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
        m6.write_config(S, ref, "it")
        log("start:", m6.emu("call Start").strip().replace("\n", " "))

        # ---- 2. the reading box right after a restart of the service (the fix of a4cf8e7) -------------------
        time.sleep(90)                                  # a few readings with their trend first
        r2.ui("", 6)
        m6.emu("call Stop >/dev/null 2>&1; sleep 4; call Start >/dev/null 2>&1")
        time.sleep(3)
        shot("r3-it-airy-overview-after-restart")
        page("LM_DEV_PAGE=SubjectPage:0", "r3-it-airy-patient-after-restart", 6)

        if not m6.wait_for(log, "very low active", lambda al: (m6.slot(al, "low") or {}).get("level") == "LL"
                           and (m6.slot(al, "low") or {}).get("state") == "active", 25 * 60):
            return 1
        time.sleep(10)

        # ---- 1 and 3: Italian, light then dark ambience ------------------------------------------------------
        for amb in ("airy", "flare"):
            ambience(amb)
            page("", "r3-it-%s-overview-alert" % amb)
            page("LM_DEV_PAGE=SubjectPage:0", "r3-it-%s-patient" % amb)
            page("LM_DEV_PAGE=AlertsPage", "r3-it-%s-alertspage" % amb)
            if amb == "airy":
                page("LM_DEV_ACTION=silent:hour", "r3-it-airy-silentpage", 14)
                page("LM_DEV_PAGE=Cover", "r3-it-airy-cover-silent")
                log("EndSilent:", m6.emu("call EndSilent string:%s" % S).strip())
            page("LM_DEV_PAGE=ThresholdsPage:0", "r3-it-%s-thresholds" % amb)
            page("LM_DEV_PAGE=ThresholdsPage:0:end", "r3-it-%s-thresholds-end" % amb, 12)
            page("LM_DEV_PAGE=SettingsPage", "r3-it-%s-settings" % amb)
            page("LM_DEV_PAGE=SettingsPage:end", "r3-it-%s-settings-end" % amb, 12)

        # ---- 3. the edit on the thresholds page, through its own Save and the challenge's accept (flare) -----
        before = low_limit()
        page("LM_DEV_ACTION=thr:62:180", "r3-it-flare-challenge", 16)
        log("challenge open, nothing sent yet: low limit unchanged", low_limit() == before)
        page("LM_DEV_ACTION=thr:62:180:61", "r3-it-flare-mismatch", 20)
        log("a wrong value typed again: low limit unchanged", low_limit() == before)
        page("LM_DEV_ACTION=thr:50:180", "r3-it-flare-order", 14)
        log("an impossible order: low limit unchanged", low_limit() == before)
        page("LM_DEV_ACTION=thr:62:180:62", "r3-it-flare-accepted", 20)
        log("the looser limit after the challenge: low limit changed", low_limit() != before)
        page("LM_DEV_PAGE=ThresholdsPage:0:end", "r3-it-flare-history", 12)
        log("UI VmHWM (thresholds page):", m6.emu("grep VmHWM /proc/$(pgrep -f '[/]usr/bin/harbour-libre-monitor-dev$' | head -1)/status").strip())

        # ---- 3. English and mmol/L, both ambiences ----------------------------------------------------------
        m6.write_config(S, ref, "en")
        log("ConfigChanged:", m6.emu("call ConfigChanged").strip().replace("\n", " ") or "(empty: applied)")
        ui_settings("mmol/L")
        for amb in ("airy", "flare"):
            ambience(amb)
            page("LM_DEV_PAGE=ThresholdsPage:0", "r3-en-%s-thresholds" % amb)
            page("LM_DEV_PAGE=ThresholdsPage:0:end", "r3-en-%s-thresholds-end" % amb, 12)
            page("LM_DEV_PAGE=SettingsPage", "r3-en-%s-settings" % amb)
            page("LM_DEV_PAGE=SettingsPage:end", "r3-en-%s-settings-end" % amb, 12)
        page("LM_DEV_ACTION=thr:3,4:10", "r3-en-flare-challenge-mmol", 16)
        page("LM_DEV_ACTION=thr:3,9:10", "r3-en-flare-back-to-standard", 16)
        log("back to the standard low limit:", low_limit() == 70)
        log("UI log lines with errors:", m6.emu("grep -c -i -E 'error|ReferenceError|TypeError' /tmp/lm-ui.log").strip())
        m6.emu("pkill -f '[/]usr/bin/harbour-libre-monitor-dev$'")
    finally:
        sim.terminate()
        ambience("airy")
        m6.emu("dconf write /lipstick/do_not_disturb false; call Stop >/dev/null 2>&1; rm -f %s/dev-credentials" % m6.DATA)
    log("shots:", len(shots), "|", " ".join(shots))
    log("done")
    return 0


if __name__ == "__main__":
    sys.exit(main())
