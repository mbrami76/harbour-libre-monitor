#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Michele Brami
"""The pages of  (sensor, storage and maintenance, the diagnostic report) on the EMULATOR, with the
development package's own service and the simulator. Never on a phone. Since the
 there is no export: the storage page's own checks are in
run_emulator_ui_x1.py; here the storage page is only looked at.

The method of R2 and R3 (run_emulator_ui_alerts.py, run_emulator_ui_r3.py): the VM's audio output off
(checked, or no run) and the sinks muted; the development package (its own identity) installed; its Sailjail
permissions granted in the emulator's own settings file (EMULATOR ONLY, a copy kept), now with Documents;
dev-base-url and dev-credentials point at the simulator on the PC.

What it looks at, by screenshots (left in --out, never in the repository) and by words and counts in the log
(no value, no name):
  1. the sensor block on the patient page (scrolled to its bottom);
  2. the storage page (the export of R4 was removed);
  3. the report dialog wired to the real service: DiagnosticReport answers not-proposed (no guard stop);
  4. the two open points of R3: the order refusal's range text, the Italian direction line after a restart.

    python3 tests/integration/run_emulator_ui_r4.py --rpm PATH [--qml FILE] [--port 18494] [--out /tmp/a8r4]
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
import run_emulator_ui_r3 as r3  # noqa: E402  (ambience, ui_settings)
import run_integration as base  # noqa: E402
import run_service_integration as svc  # noqa: E402

SEED = 47
# EMULATOR ONLY: what the tap on "Allow" does,.
# The Permissions= line must follow the desktop file too, or sailjaild sees a changed application at its
# start and resets the grant to "not allowed" (seen in this run's first attempt).
ALLOW = m6.ALLOW.replace("Granted=Internet;Secrets;", "Granted=Internet;Secrets;Documents;").replace(
    '{ print "Granted=Internet;Secrets;Documents;"; next }',
    '{ print "Granted=Internet;Secrets;Documents;"; next }\n  s && /^Permissions=/ { print "Permissions=Internet;Secrets;Documents;"; next }')
DOCS = "/home/defaultuser/Documents"


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--rpm", required=True, help="the i486 development package")
    ap.add_argument("--qml", default="", help="a newer main QML file to put over the package's (development only)")
    ap.add_argument("--port", type=int, default=18494)
    ap.add_argument("--out", default="/tmp/a8r4")
    a = ap.parse_args()
    os.makedirs(a.out, exist_ok=True)
    log = m6.Log(os.path.join(a.out, "run.log"))

    subprocess.run(["VBoxManage", "controlvm", m6.VM, "audioout", "off"], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    info = subprocess.run(["VBoxManage", "showvminfo", m6.VM, "--machinereadable"], stdout=subprocess.PIPE).stdout.decode()
    if 'audio_out="off"' not in info:
        log("REFUSED: the virtual machine's audio output is not off")
        return 2
    log("audio_out off:", True)
    log("sinks muted:", m6.emu("for n in $(pactl list short sinks | cut -f1); do pactl set-sink-mute $n 1; done; "
                               "echo $(pactl list sinks | grep -c 'Mute: yes') of $(pactl list short sinks | wc -l)").strip())

    m6.scp(a.rpm, "/home/defaultuser/" + os.path.basename(a.rpm))
    log("install:", m6.emu("rpm -U --force /home/defaultuser/%s 2>&1 | tail -2; rpm -q harbour-libre-monitor-dev"
                           % os.path.basename(a.rpm), user="root").strip().replace("\n", " | "))
    if a.qml:
        m6.scp(a.qml, "/home/defaultuser/lm-main.qml")
        log("main QML replaced:", m6.emu("cp /home/defaultuser/lm-main.qml /usr/share/harbour-libre-monitor-dev/qml/"
                                         "harbour-libre-monitor-dev.qml && echo yes", user="root").strip())
    log("desktop permissions:", m6.emu("grep ^Permissions /usr/share/applications/harbour-libre-monitor-dev.desktop").strip())
    log("permissions granted:", m6.emu(ALLOW, user="root").strip())
    m6.emu("call Stop >/dev/null 2>&1; sleep 3")
    m6.emu("pkill -f '[h]arbour-libre-monitor-dev'; sleep 2; true", user="root")
    m6.emu("mkdir -p %s %s %s; rm -f %s/notify.log %s/store.db* %s/journal.db* %s/config.json "
           "%s/libre-monitor-report-*; dconf write /lipstick/do_not_disturb false"
           % (m6.DATA, m6.CONF, DOCS, m6.DATA, m6.DATA, m6.DATA, m6.CONF, DOCS))
    r3.ui_settings("mg/dL")

    t0 = time.time()
    scen = {
        "name": "emulator-ui-r4", "seed": SEED, "start_utc": "now", "timezone": "Europe/Rome",
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
        m6.write_config(S, ref, "en")
        log("start:", m6.emu("call Start").strip().replace("\n", " "))
        time.sleep(120)                                 # readings and the sensor noted
        log("service inside Sailjail:", m6.emu("ps -eo args | grep -c '[f]irejail.*harbour-libre-monitor-dev'").strip(),
            "| service processes:", m6.emu("pgrep -fc '[h]arbour-libre-monitor-dev --service'").strip())
        log("Status() has a sensor:", '"sensor":{' in m6.emu("call Status"))
        r3.ambience("airy")

        # ---- 1. the sensor block ------------------------------------------------------------------------
        page("LM_DEV_PAGE=SubjectPage:0:end", "r4-en-airy-sensor", 12)
        # ---- 2. the storage page ---------------------------------------------------------------------------
        page("LM_DEV_PAGE=StoragePage", "r4-en-airy-storage", 10)
        page("LM_DEV_PAGE=StoragePage:end", "r4-en-airy-storage-end", 12)
        r3.ambience("flare")
        page("LM_DEV_PAGE=StoragePage", "r4-en-flare-storage", 10)
        # ---- 3. the report dialog against the real service: no proposal, so "not-proposed" ---------------
        page("LM_DEV_ACTION=report:0", "r4-en-flare-report-not-proposed", 10)
        log("report files written:", m6.emu("ls %s/libre-monitor-report-* 2>/dev/null | wc -l" % DOCS).strip())
        # ---- 4. R3's open points: the order refusal, the Italian line after a restart ---------------------
        page("LM_DEV_ACTION=thr:50:180", "r4-en-flare-order-refusal", 14)
        m6.write_config(S, ref, "it")
        m6.emu("call Stop >/dev/null 2>&1; sleep 4; call Start >/dev/null 2>&1")
        time.sleep(3)
        page("LM_DEV_PAGE=SubjectPage:0", "r4-it-flare-patient-after-restart", 8)
        page("LM_DEV_PAGE=StoragePage", "r4-it-flare-storage", 10)
        page("LM_DEV_PAGE=SubjectPage:0:end", "r4-it-flare-sensor", 12)
        log("UI VmHWM:", m6.emu("grep VmHWM /proc/$(pgrep -f '[/]usr/bin/harbour-libre-monitor-dev$' | head -1)/status").strip())
        log("UI log lines with errors:", m6.emu("grep -c -i -E 'error|ReferenceError|TypeError' /tmp/lm-ui.log").strip())
        m6.emu("pkill -f '[/]usr/bin/harbour-libre-monitor-dev$'")
    finally:
        sim.terminate()
        r3.ambience("airy")
        m6.emu("call Stop >/dev/null 2>&1; rm -f %s/dev-credentials" % m6.DATA)
    log("shots:", len(shots), "|", " ".join(shots))
    log("done")
    return 0


if __name__ == "__main__":
    sys.exit(main())
