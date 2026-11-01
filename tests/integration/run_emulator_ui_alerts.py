#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Michele Brami
"""The alert pages of the interface on the EMULATOR, with the development package's own service and the
simulator. Never on a phone.

Python 3 standard library only. It reuses the method (run_emulator_alerts.py): the VM's audio
output off (checked) and the emulator's sinks muted before anything; the development package
(harbour-libre-monitor-dev, its own identity) installed over its earlier version; its Sailjail
permissions granted in the emulator's own settings file, as a tap on "Allow" would (a copy is kept,
EMULATOR ONLY); the development-only files dev-base-url (plain http to the PC) and dev-credentials
(the simulator's fake account); a synthetic patient that goes very low a few minutes after the start.

Then the interface (the same package, started from a shell with LM_DEV_SOURCE=service so the pages
use the service instead of the scripted source) is opened on given pages and screenshots are taken
through Lipstick's own screenshot method. The chat cannot tap: LM_DEV_ACTION does once what a tap
would do, through the same function the page's button calls. Checked and logged in words and counts:
the notification of the very low alert, the tile's alert and its Acknowledge button (screenshot), the
acknowledgement from the application closing the notification (the service's CloseNotification in
the recording), silent mode set from the page (screenshot of the page, the cover and the overview),
the confirmation without limit (screenshot, nothing sent), Do not disturb on and off showing and
hiding the strip. The screenshots are left in --out, never in the repository.

    python3 tests/integration/run_emulator_ui_alerts.py --rpm PATH [--port 18491] [--out /tmp/a8r2]
"""
import argparse
import json
import os
import re
import subprocess
import sys
import time
import uuid

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import run_emulator_alerts as m6  # noqa: E402  (emu, scp, ALLOW, alerts, slot, wait_for, Log, iso)
import run_integration as base  # noqa: E402
import run_service_integration as svc  # noqa: E402

SEED = 37
BUS = "DBUS_SESSION_BUS_ADDRESS=unix:path=/run/user/100000/dbus/user_bus_socket XDG_RUNTIME_DIR=/run/user/100000"
UI = "/usr/bin/harbour-libre-monitor-dev"


def wake():
    m6.emu("dbus-send --system --type=method_call --dest=com.nokia.mce /com/nokia/mce/request "
           "com.nokia.mce.request.req_display_state_on; dbus-send --system --type=method_call --dest=com.nokia.mce "
           "/com/nokia/mce/request com.nokia.mce.request.req_tklock_mode_change string:unlocked; "
           "dbus-send --system --type=method_call --dest=com.nokia.mce /com/nokia/mce/request "
           "com.nokia.mce.request.req_display_blanking_pause", user="root")


def shot(out, name):
    wake()
    m6.emu("mkdir -p /home/defaultuser/lmshots; chown defaultuser /home/defaultuser/lmshots; "
           "rm -f /home/defaultuser/lmshots/%s.png; %s runuser -u defaultuser -g privileged -- dbus-send --session "
           "--print-reply --dest=org.nemomobile.lipstick /org/nemomobile/lipstick/screenshot "
           "org.nemomobile.lipstick.saveScreenshot string:/home/defaultuser/lmshots/%s.png >/dev/null" % (name, BUS, name),
           user="root")
    local = os.path.join(out, name + ".png")
    subprocess.run(["scp", "-q", "-P", "2223", "-o", "StrictHostKeyChecking=no", "-o", "UserKnownHostsFile=/dev/null",
                    "-o", "LogLevel=ERROR", "-i", m6.KEY, "root@localhost:/home/defaultuser/lmshots/%s.png" % name, local])
    return local if os.path.exists(local) else None


def ui(env, wait_s=10):
    """Starts the interface afresh with these development-only variables; returns its error lines."""
    m6.emu("pkill -f '[/]usr/bin/harbour-libre-monitor-dev$'; sleep 1; %s LM_DEV_SOURCE=service nohup %s > /tmp/lm-ui.log 2>&1 &"
           % (env, UI))
    time.sleep(wait_s)
    return m6.emu("grep -i -E 'error|warning|ReferenceError|TypeError' /tmp/lm-ui.log | grep -v -i 'propertyCache\\|wayland' | head -12").strip()


def closes(out):
    """How many notifications the service closed, as the recording shows."""
    return int(m6.emu("grep -c 'member=CloseNotification' /tmp/lm-ui-monitor.txt").strip() or "0")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--rpm", required=True, help="the i486 development package")
    ap.add_argument("--port", type=int, default=18491)
    ap.add_argument("--out", default="/tmp/a8r2")
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

    t0 = time.time()
    scen = {
        "name": "emulator-ui-alerts", "seed": SEED, "start_utc": "now", "timezone": "Europe/Rome",
        "accounts": [{"email": "follower@example.invalid", "password": base.PASSWORD, "unit": "mg/dL",
                      "country": "IT", "subjects": ["a"]}],
        "subjects": [{"key": "a", "first_name": "Sim", "last_name": "Subject Emulator", "sensor": {"age_days": 5},
                      "profile": {"baseline": 115.0, "daily_amp": 0.0, "meals": [], "night_dip": None, "noise": 0.0,
                                  "drift_amp": 0.0, "excursions": [
                                      # sin² shape: low from about +9.5 min, very low from about +11 min
                                      # to +23 min (gen.py), long enough for every step below
                                      {"at_utc": m6.iso(t0 + 2 * 60), "duration_min": 30, "peak": 25}]}}],
    }
    scen_path = os.path.join(a.out, "scenario.json")
    with open(scen_path, "w") as f:
        json.dump(scen, f)
    sim = subprocess.Popen([sys.executable, "-m", "libresim", "--scenario", scen_path, "--host", "127.0.0.1",
                            "--port", str(a.port)], cwd=base.SIMDIR, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    S = None
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
        m6.write_config(S, svc.patient_ref(SEED, "a"), "en")
        m6.emu("nohup dbus-monitor --session \"interface='org.freedesktop.Notifications'\" > /tmp/lm-ui-monitor.txt 2>&1 & "
               "echo $! > /tmp/lm-ui-monitor.pid")
        log("start:", m6.emu("call Start").strip().replace("\n", " "))

        if not m6.wait_for(log, "very low active", lambda al: (m6.slot(al, "low") or {}).get("level") == "LL"
                           and (m6.slot(al, "low") or {}).get("state") == "active", 25 * 60):
            return 1
        time.sleep(15)
        notifies = int(m6.emu("grep -c 'member=Notify' /tmp/lm-ui-monitor.txt").strip() or "0")
        log("A notification calls so far:", notifies, "| very low posted:",
            "VERY LOW GLUCOSE" in m6.emu("grep -c 'VERY LOW GLUCOSE' /tmp/lm-ui-monitor.txt; grep -m1 'VERY LOW GLUCOSE' /tmp/lm-ui-monitor.txt"))

        # ---- B: the overview with the alert, the patient page, the alerts page ----------------------------
        log("B UI errors:", ui("") or "none")
        log("B shot:", shot(a.out, "r2-overview-alert"))
        log("B UI errors (patient page):", ui("LM_DEV_PAGE=SubjectPage:0") or "none")
        log("B shot:", shot(a.out, "r2-patient-alert"))
        log("B UI errors (alerts page):", ui("LM_DEV_PAGE=AlertsPage") or "none")
        log("B shot:", shot(a.out, "r2-alertspage"))

        # ---- C: the acknowledgement from the application closes the notification --------------------------
        before = closes(a.out)
        log("C UI errors (acknowledge-first):", ui("LM_DEV_ACTION=acknowledge-first", 14) or "none")
        log("C Alerts() low slot:", json.dumps({k: (m6.slot(m6.alerts(), "low") or {}).get(k) for k in ("level", "state", "acknowledged")}))
        after = closes(a.out)
        log("C CloseNotification calls before/after the acknowledgement:", before, after)
        log("C shot:", shot(a.out, "r2-overview-acknowledged"))

        # ---- D: silent mode from the page, the cover, the overview; the confirmation without limit ---------
        log("D UI errors (silent:hour):", ui("LM_DEV_ACTION=silent:hour", 14) or "none")
        th = (m6.alerts().get("thresholds") or [{}])[0]
        log("D Alerts() silent:", th.get("silent"), "until set:", bool(th.get("silent_until")))
        log("D shot:", shot(a.out, "r2-silentpage-hour"))
        log("D UI errors (cover):", ui("LM_DEV_PAGE=Cover") or "none")
        log("D shot:", shot(a.out, "r2-cover-silent"))
        log("D UI errors (overview):", ui("") or "none")
        log("D shot:", shot(a.out, "r2-overview-silent"))
        until_before = (m6.alerts().get("thresholds") or [{}])[0].get("silent_until")
        log("D UI errors (silent:no-limit):", ui("LM_DEV_ACTION=silent:no-limit", 12) or "none")
        log("D shot:", shot(a.out, "r2-silent-confirm"))
        until_after = (m6.alerts().get("thresholds") or [{}])[0].get("silent_until")
        log("D the confirmation alone sent nothing (the end is unchanged):", until_before == until_after and bool(until_after))
        log("D EndSilent:", m6.emu("call EndSilent string:%s" % S).strip())

        # ---- E: Do not disturb on and off, the strip follows -----------------------------------------------
        log("E UI errors (overview):", ui("") or "none")
        m6.emu("dconf write /lipstick/do_not_disturb true")
        time.sleep(4)
        log("E shot:", shot(a.out, "r2-dnd-on"))
        m6.emu("dconf write /lipstick/do_not_disturb false")
        time.sleep(4)
        log("E shot:", shot(a.out, "r2-dnd-off"))
        log("E UI log lines with errors:", m6.emu("grep -c -i -E 'error|ReferenceError|TypeError' /tmp/lm-ui.log").strip())
        # The window's peak resident memory with the overview, the strip and the alert parts loaded (the
        # figure to set against; the import of Nemo.Configuration is in it, not measured apart).
        log("E UI VmHWM:", m6.emu("grep VmHWM /proc/$(pgrep -f '[/]usr/bin/harbour-libre-monitor-dev$' | head -1)/status").strip())
        m6.emu("pkill -f '[/]usr/bin/harbour-libre-monitor-dev$'")
    finally:
        sim.terminate()
        m6.emu("kill $(cat /tmp/lm-ui-monitor.pid) 2>/dev/null; dconf write /lipstick/do_not_disturb false; call Stop >/dev/null 2>&1; "
               "rm -f %s/dev-credentials" % m6.DATA)
        subprocess.run(["scp", "-q", "-P", "2223", "-o", "StrictHostKeyChecking=no", "-o", "UserKnownHostsFile=/dev/null",
                        "-o", "LogLevel=ERROR", "-i", m6.KEY, "defaultuser@localhost:/tmp/lm-ui-monitor.txt",
                        os.path.join(a.out, "monitor.txt")])
    log("done")
    return 0


if __name__ == "__main__":
    sys.exit(main())
