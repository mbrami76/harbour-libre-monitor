#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Michele Brami
"""The alert path with readings, on the emulator.

Python 3 standard library only. Not a QtTest and not part of the qmake build. About 100 minutes.

What it does. The DEVELOPMENT package (its own identity, harbour-libre-monitor-dev, built by
tools/devpackage/make-dev-package.sh for the i486 target) runs on the Sailfish OS emulator and fetches
readings from the simulator on this PC; the alarms therefore come from READINGS through the whole
chain (adapter, store, alert engine, notifier, the phone's notification server), not from the
developer feed. A dbus-monitor on the emulator records every call to the notification server and its
answers; at each step the script also reads the service's own notification log (numbers and hint
words only), the audio streams (pactl, read only) and the service's Alerts() and Status().

The steps follow the readings of a synthetic patient: a very low excursion (posting, texts in
English, icon, display-on, the tone's stream, alarm before resident notice, a kill -9 of the service
in the middle of the alarm, the acknowledgement through AcknowledgeAlert closing the entry); a low
excursion (silent mode set and ended through the D-Bus methods, then the alert clears without an
acknowledgement and leaves the "ended" entry); the language switched to Italian at ConfigChanged; a
very high excursion (Italian texts); a gap in the readings (the no-data alarm after the per-patient
limit of 5 minutes); Do not disturb switched on (the notice, then the notice at a restart) and off.

Safety. NO SOUND: the script refuses to run unless the virtual machine's audio output is off
(VBoxManage) and it mutes every sink of the emulator first. The service is pointed at the simulator by
the development-only file `dev-base-url` (plain http to the PC through VirtualBox's NAT address
10.0.2.2; the real source is https only) and reads the simulator's fake account from the
development-only file `dev-credentials`; both live in the development identity's own data folder.
The release application on the emulator and its data are not touched. Nothing contacts the real
source. The output holds kinds, words of the texts, hint names, counts and times, never a reading.

    python3 tests/integration/run_emulator_alerts.py --rpm PATH [--port 18490] [--out DIR]
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
REPO = os.path.dirname(os.path.dirname(HERE))
sys.path.insert(0, HERE)
sys.path.insert(0, os.path.join(REPO, "tools", "simulator"))
import run_integration as base  # noqa: E402  (PASSWORD, SIMDIR)
import run_service_integration as svc  # noqa: E402  (patient_ref)

VM = "SailfishOS-5.1.0.11"
KEY = os.path.expanduser("~/SailfishOS/vmshare/ssh/private_keys/sdk")
SSH = ["ssh", "-o", "ConnectTimeout=15", "-o", "StrictHostKeyChecking=no", "-o", "UserKnownHostsFile=/dev/null",
       "-o", "LogLevel=ERROR", "-i", KEY, "-p", "2223"]
APP = "libre-monitor-dev"
DATA = "/home/defaultuser/.local/share/it.michelebrami/" + APP
CONF = "/home/defaultuser/.config/it.michelebrami/" + APP
PRELUDE = ("BUS=it.michelebrami.libre-monitor-dev; OBJ=/it/michelebrami/libre_monitor_dev; "
           "IF=it.michelebrami.libre_monitor.Service; "
           "call() { m=$1; shift; dbus-send --session --print-reply=literal --reply-timeout=25000 "
           "--dest=$BUS $OBJ $IF.$m \"$@\" 2>&1; }; ")
SEED = 31
# EMULATOR ONLY: a new version of a package asks once, on the screen, to allow its declared Sailjail
# permissions, and the service cannot start until someone taps "Allow"; the chat cannot tap. This does
# what the tap does, for the development package only, in the emulator's own settings file (a copy is
# kept in /root). Never run on the author's phone.
ALLOW = r"""F=/home/.system/var/lib/sailjail/settings/user-100000.settings; A='[harbour-libre-monitor-dev]'
systemctl stop sailjaild; cp "$F" /root/sailjail-user.settings.bak
awk -v app="$A" '/^\[/ { s = ($0 == app) } s && /^Allowed=/ { print "Allowed=1"; next }
  s && /^Granted=/ { print "Granted=Internet;Secrets;"; next } { print }' /root/sailjail-user.settings.bak > "$F"
systemctl start sailjaild; sleep 2; grep -A1 -F "$A" "$F" | tail -1"""
MINUTE = 60.0


def emu(cmd, user="defaultuser", timeout=60):
    p = subprocess.run(SSH + ["%s@localhost" % user, PRELUDE + cmd], stdin=subprocess.DEVNULL,
                       stdout=subprocess.PIPE, stderr=subprocess.STDOUT, timeout=timeout)
    return p.stdout.decode("utf-8", "replace")


def scp(local, remote):
    subprocess.run(["scp", "-q", "-P", "2223", "-o", "StrictHostKeyChecking=no", "-o", "UserKnownHostsFile=/dev/null",
                    "-o", "LogLevel=ERROR", "-i", KEY, local, "defaultuser@localhost:" + remote], check=True)


def iso(epoch):
    return time.strftime("%Y-%m-%dT%H:%M:%SZ", time.gmtime(epoch))


class Log:
    def __init__(self, path):
        self.f = open(path, "w", encoding="utf-8")

    def __call__(self, *parts):
        line = "%s %s" % (time.strftime("%H:%M:%S"), " ".join(str(p) for p in parts))
        print(line, flush=True)
        self.f.write(line + "\n")
        self.f.flush()


def alerts(log=None):
    out = emu("call Alerts")
    m = re.search(r"\{.*\}", out, re.S)
    try:
        return json.loads(m.group(0)) if m else {}
    except ValueError:
        return {}


def slot(al, name):
    for a in al.get("alerts", []):
        if a.get("slot") == name:
            return a
    return None


def wait_for(log, what, pred, timeout_s, step_s=15):
    end = time.time() + timeout_s
    while time.time() < end:
        al = alerts()
        r = pred(al)
        if r:
            log("reached:", what)
            return r
        time.sleep(step_s)
    log("TIMEOUT waiting for:", what)
    return None


def probe(log, label):
    """What the phone side shows now: streams, the notification log's last lines, the alert list."""
    streams = emu("pactl list sink-inputs short | wc -l; pactl list sink-inputs | grep -E 'media.name|application.name' | sort | uniq -c")
    nlog = emu("tail -n 4 %s/notify.log 2>/dev/null" % DATA)
    al = alerts()
    kinds = ["%s:%s:%s%s" % (a.get("slot"), a.get("level"), a.get("state"), "" if not a.get("acknowledged") else "(ack)")
             for a in al.get("alerts", [])]
    th = (al.get("thresholds") or [{}])[0]
    log("PROBE", label, "| alerts", kinds, "| silent", th.get("silent"), "| dnd", al.get("do_not_disturb"))
    log("  streams:", " / ".join(l.strip() for l in streams.strip().splitlines()))
    for l in nlog.strip().splitlines():
        log("  notify.log:", l.strip())


def write_config(subject_id, ref, language):
    cfg = {"format": 1, "language": language, "alerts_show_value": False,
           "connections": [{"id": "c1", "adapter": "libre", "label": "Sim",
                            "subjects": [{"id": subject_id, "ref": ref,
                                          "alert_persistence_low_s": 0, "alert_persistence_high_s": 0,
                                          "missing_limit_s": 300}],
                            "current_interval_s": 60}]}
    local = "/tmp/a6-emu-config.json"
    with open(local, "w") as f:
        json.dump(cfg, f, indent=2)
    scp(local, CONF + "/config.json")
    emu("chmod 600 %s/config.json" % CONF)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--rpm", required=True, help="the i486 development package")
    ap.add_argument("--port", type=int, default=18490)
    ap.add_argument("--out", default="/tmp/a6-emu")
    a = ap.parse_args()
    os.makedirs(a.out, exist_ok=True)
    log = Log(os.path.join(a.out, "run.log"))

    # ---- no sound, or no run ---------------------------------------------------------------------
    subprocess.run(["VBoxManage", "controlvm", VM, "audioout", "off"], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    info = subprocess.run(["VBoxManage", "showvminfo", VM, "--machinereadable"], stdout=subprocess.PIPE).stdout.decode()
    if 'audio_out="off"' not in info:
        log("REFUSED: the virtual machine's audio output is not off")
        return 2
    log("audio_out off:", 'audio_out="off"' in info)
    log("sinks muted:", emu("for n in $(pactl list short sinks | cut -f1); do pactl set-sink-mute $n 1; done; "
                            "pactl list sinks | grep -c 'Mute: yes'").strip())

    # ---- the development package -----------------------------------------------------------------------
    scp(a.rpm, "/home/defaultuser/" + os.path.basename(a.rpm))
    log("install:", emu("rpm -U --force /home/defaultuser/%s 2>&1 | tail -2; rpm -q harbour-libre-monitor-dev harbour-libre-monitor"
                        % os.path.basename(a.rpm), user="root").strip().replace("\n", " | "))
    log("permissions:", emu(ALLOW, user="root").strip())
    # Any earlier development service (and its sandbox wrappers) goes; the bracket keeps pkill from
    # matching the shell that runs it. A new package version must also have its Sailjail permissions
    # granted once (the system asks on the screen; on the emulator: /root/a6-allow.sh, see the report).
    emu("call Stop >/dev/null 2>&1; sleep 3")
    emu("pkill -f '[h]arbour-libre-monitor-dev'; sleep 2; true", user="root")
    emu("mkdir -p %s %s; rm -f %s/notify.log %s/store.db* %s/journal.db* %s/settings.json %s/config.json"
        % (DATA, CONF, DATA, DATA, DATA, DATA, CONF))

    # ---- the simulator and the files that point the service at it ----------------------------------------
    t0 = time.time()
    scen = {
        "name": "emulator-alerts", "seed": SEED, "start_utc": "now", "timezone": "Europe/Rome",
        "accounts": [{"email": "follower@example.invalid", "password": base.PASSWORD, "unit": "mg/dL",
                      "country": "IT", "subjects": ["a"]}],
        "subjects": [{"key": "a", "first_name": "Sim", "last_name": "Subject Emulator", "sensor": {"age_days": 5},
                      "profile": {"baseline": 115.0, "daily_amp": 0.0, "meals": [], "night_dip": None, "noise": 0.0,
                                  "drift_amp": 0.0, "excursions": [
                                      {"at_utc": iso(t0 + 3 * MINUTE), "duration_min": 30, "peak": 42},
                                      {"at_utc": iso(t0 + 33 * MINUTE), "duration_min": 20, "peak": 62},
                                      {"at_utc": iso(t0 + 56 * MINUTE), "duration_min": 20, "peak": 275}]}}],
        "faults": [{"kind": "gap", "subject": "a", "start_utc": iso(t0 + 80 * MINUTE), "end_utc": iso(t0 + 93 * MINUTE)}],
    }
    scen_path = os.path.join(a.out, "scenario.json")
    with open(scen_path, "w") as f:
        json.dump(scen, f)
    sim = subprocess.Popen([sys.executable, "-m", "libresim", "--scenario", scen_path, "--host", "127.0.0.1",
                            "--port", str(a.port)], cwd=base.SIMDIR, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    try:
        time.sleep(2)
        url = "http://10.0.2.2:%d" % a.port
        log("simulator reachable from the emulator:", emu("curl -s -o /dev/null -w '%%{http_code}' %s/ ; echo" % url).strip())
        cred = "/tmp/a6-emu-cred"
        with open(cred, "w") as f:
            f.write("username=follower@example.invalid\npassword=%s\n" % base.PASSWORD)
        scp(cred, DATA + "/dev-credentials")
        emu("chmod 600 %s/dev-credentials; echo %s > %s/dev-base-url" % (DATA, url, DATA))
        subject_id = "{%s}" % uuid.uuid4()
        write_config(subject_id, svc.patient_ref(SEED, "a"), "en")
        S = subject_id

        # the recorder of every call to the notification server, from now to the end
        emu("nohup dbus-monitor --session \"interface='org.freedesktop.Notifications'\" > /tmp/lm-monitor.txt 2>&1 & echo $! > /tmp/lm-monitor.pid")
        log("start:", emu("call Start").strip().replace("\n", " "))
        time.sleep(20)
        log("adapter command line has the simulator:",
            "--base-url http://10.0.2.2" in emu("ps -eo args | grep '[-]-adapter libre'", user="root"))
        # (read as root: the sandboxed adapter's command line is not readable by the user's shell)
        st = emu("call Status")
        log("status:", re.findall(r'"state":"[a-z-]+"', st)[:3], "polled:", '"last_success_at":"2' in st)

        # ---- A: very low -------------------------------------------------------------------------------------
        if wait_for(log, "very low active", lambda al: (slot(al, "low") or {}).get("level") == "LL", 30 * MINUTE):
            time.sleep(20)
            probe(log, "A1 very low posted")
            pid = emu("ps -eo pid,args | awk '$2==\"/usr/bin/harbour-libre-monitor-dev\" && $3==\"--service\" && NF==3 {print $1}' | head -1").strip()
            emu("kill -9 %s" % pid)
            log("A2 kill -9 of the service, pid found:", bool(pid))
            time.sleep(3)
            log("A2 restart by activation:", emu("call Start").strip().replace("\n", " "))
            time.sleep(40)
            probe(log, "A2 after the restart")
            log("A3 AcknowledgeAlert:", emu("call AcknowledgeAlert string:%s string:low" % S).strip())
            time.sleep(5)
            probe(log, "A3 after the acknowledgement")

        # ---- B: low, silent mode, the ended entry -----------------------------------------------------
        if wait_for(log, "low active (new episode)",
                    lambda al: (slot(al, "low") or {}).get("level") == "L" and (slot(al, "low") or {}).get("state") == "active",
                    30 * MINUTE):
            time.sleep(20)
            probe(log, "B1 low posted")
            log("B2 SetSilent 60:", emu("call SetSilent string:%s int32:60" % S).strip())
            time.sleep(5)
            probe(log, "B2 silent")
            log("B3 EndSilent:", emu("call EndSilent string:%s" % S).strip())
            time.sleep(5)
            if wait_for(log, "low cleared without acknowledgement", lambda al: slot(al, "low") is None, 25 * MINUTE):
                time.sleep(15)
                probe(log, "B4 low ended")

        # ---- C: Italian -----------------------------------------------------------------------------------
        write_config(S, svc.patient_ref(SEED, "a"), "it")
        log("C ConfigChanged (language it):", repr(emu("call ConfigChanged").strip()))

        # ---- D: very high, in Italian -------------------------------------------------------------------------
        if wait_for(log, "very high active", lambda al: (slot(al, "high") or {}).get("level") == "HH", 35 * MINUTE):
            time.sleep(20)
            probe(log, "D1 very high posted")
            log("D2 AcknowledgeAlert:", emu("call AcknowledgeAlert string:%s string:high" % S).strip())
            time.sleep(5)
            probe(log, "D2 after the acknowledgement")

        # ---- E: no data -------------------------------------------------------------------------------------
        if wait_for(log, "no data active", lambda al: (slot(al, "missing") or {}).get("state") == "active", 40 * MINUTE):
            time.sleep(20)
            probe(log, "E1 no data posted")
            log("E2 AcknowledgeAlert:", emu("call AcknowledgeAlert string:%s string:missing" % S).strip())
            time.sleep(5)

        # ---- F: Do not disturb ---------------------------------------------------------------------------------
        emu("dconf write /lipstick/do_not_disturb true")
        time.sleep(5)
        probe(log, "F1 Do not disturb on")
        emu("call Stop >/dev/null")
        time.sleep(8)
        log("F2 restart with Do not disturb on:", emu("call Start").strip().replace("\n", " "))
        time.sleep(30)
        probe(log, "F2 after the restart")
        emu("dconf write /lipstick/do_not_disturb false")
        time.sleep(5)
        probe(log, "F3 Do not disturb off")

        log("stop:", emu("call Stop").strip())
        time.sleep(5)
    finally:
        sim.terminate()
        emu("kill $(cat /tmp/lm-monitor.pid) 2>/dev/null; dconf write /lipstick/do_not_disturb false; "
            "rm -f %s/dev-credentials" % DATA)
        subprocess.run(["scp", "-q", "-P", "2223", "-o", "StrictHostKeyChecking=no", "-o", "UserKnownHostsFile=/dev/null",
                        "-o", "LogLevel=ERROR", "-i", KEY, "defaultuser@localhost:/tmp/lm-monitor.txt",
                        os.path.join(a.out, "monitor.txt")])
        subprocess.run(["scp", "-q", "-P", "2223", "-o", "StrictHostKeyChecking=no", "-o", "UserKnownHostsFile=/dev/null",
                        "-o", "LogLevel=ERROR", "-i", KEY, "defaultuser@localhost:%s/notify.log" % DATA,
                        os.path.join(a.out, "notify.log")])
    log("done; analyse with tests/integration/notify_monitor_summary.py", os.path.join(a.out, "monitor.txt"))
    return 0


if __name__ == "__main__":
    sys.exit(main())
