#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Michele Brami
"""The interface's alerts against the HOST service and the SIMULATOR.

Python 3 standard library only; not a QtTest and not part of the qmake build. It writes a synthetic
scenario (one patient who goes very low a minute after the start and stays there for half an hour),
starts the simulator, and runs ui_alerts_session.sh inside the SDK build engine: a private session bus,
the host service started by D-Bus activation against the simulator, and the interface's live test
(tst_uireal liveHostServiceWithTheSimulator) given the service's name. The live test drives the real
alerts backend of the interface (app/real/realalertsbackend) exactly as the pages do: Alerts() at the
start and on the service's signals, Acknowledge, Snooze, SetSilent and EndSilent.

What is checked here: the live test ran and passed (not skipped), and no name, password or glucose
value of the synthetic account is in its output. The real source is never contacted.

    python3 tests/integration/run_ui_alerts_integration.py [--build-dir DIR] [--bridge ADDR] [--functions F ...]

Round R3: --functions liveThresholdsAndSettingsWithTheHostService runs the thresholds page's controller
(the challenge, the refusals, the history read from the service's journal) and the settings of the
configuration file (the alert times, the language, the value in alerts, a label, applied by ConfigChanged)
against the same host service; it does not wait for the excursion.
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

import run_integration as base  # noqa: E402
import run_service_integration as svc  # noqa: E402


def iso(epoch):
    return time.strftime("%Y-%m-%dT%H:%M:%SZ", time.gmtime(epoch))


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--build-dir", default=os.path.expanduser("~/SFOS-DEV/harbour-libre-monitor-development/build/host-alpha8-r2-i486"))
    ap.add_argument("--target", default="SailfishOS-5.1.0.11-i486")
    ap.add_argument("--bridge", default=None)
    ap.add_argument("--wait-s", type=int, default=900)
    ap.add_argument("--functions", nargs="+", default=["liveHostServiceWithTheSimulator"])
    a = ap.parse_args()

    os.makedirs(os.path.join(a.build_dir, "it"), exist_ok=True)
    rep = base.Report()
    env = base.Env(a.build_dir, a.target, a.bridge or base.docker_bridge())
    try:
        t0 = time.time()
        scenario = {
            "name": "ui-alerts-run", "seed": 23, "start_utc": "now", "timezone": "UTC",
            "accounts": [{"email": "follower@example.invalid", "password": base.PASSWORD,
                          "unit": "mg/dL", "country": "IT", "subjects": ["a"]}],
            "subjects": [{"key": "a", "first_name": "Sim", "last_name": "Subject Interface",
                          "sensor": {"age_days": 5},
                          "profile": {"baseline": 110.0, "daily_amp": 0.0, "meals": [], "night_dip": None,
                                      "noise": 0.0, "drift_amp": 0.0,
                                      "excursions": [{"at_utc": iso(t0 + 60), "duration_min": 40, "peak": 40}]}}],
        }
        scen_path = os.path.join(env.work, "ui_alerts_run.json")
        with open(scen_path, "w", encoding="utf-8") as f:
            json.dump(scenario, f)
        env.stop_sim()
        env.port += 1
        env.sim = subprocess.Popen(
            [sys.executable, "-m", "libresim", "--scenario", scen_path, "--host", env.bridge,
             "--port", str(env.port)], cwd=base.SIMDIR, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        with open(env.pidfile, "w") as f:
            f.write(str(env.sim.pid))
        import socket
        deadline = time.time() + 15
        while time.time() < deadline:
            try:
                socket.create_connection((env.bridge, env.port), timeout=0.5).close()
                break
            except OSError:
                time.sleep(0.1)

        ref = svc.patient_ref(scenario["seed"], "a")
        data = os.path.join(env.work, "data")
        os.makedirs(data)
        subject_id = "{%s}" % uuid.uuid4()
        with open(os.path.join(data, "config.json"), "w") as f:
            json.dump({"format": 1, "connections": [{
                "id": "c1", "adapter": "libre", "subjects": [{"id": subject_id, "ref": ref}],
                "current_interval_s": 60}]}, f)
        env.credentials("follower@example.invalid", name="cred")

        script = os.path.join(HERE, "ui_alerts_session.sh")
        cmd = ["sfdk", "-c", "target=" + a.target, "build-shell", "sh", script, a.build_dir, env.work,
               "http://%s:%d" % (env.bridge, env.port), subject_id, str(a.wait_s), " ".join(a.functions)]
        try:
            p = subprocess.run(cmd, cwd=a.build_dir, stdin=subprocess.DEVNULL, stdout=subprocess.PIPE,
                               stderr=subprocess.PIPE, timeout=a.wait_s + 600)
            out = p.stdout.decode("utf-8", "replace")
            err = p.stderr.decode("utf-8", "replace")
        except subprocess.TimeoutExpired as e:
            out = (e.stdout or b"").decode("utf-8", "replace")
            err = (e.stderr or b"").decode("utf-8", "replace") + "\n(timed out)"
        sec = svc.sections(out)
        if os.environ.get("LM_DEBUG"):
            print(out[-6000:])
        live = sec.get("live", [])
        for l in live:
            if l.startswith("PASS") or l.startswith("FAIL") or l.startswith("SKIP") or "LIVE" in l or l.startswith("Totals") \
                    or "Loc:" in l or "Actual" in l or "Expected" in l:
                print("  " + l.strip())
        if not any(l.startswith("Totals:") for l in live):
            print("the live section, as printed:")
            for l in (live or ["(empty)"])[-30:]:
                print("  | " + l)
            print("  | stderr: " + err[-600:].replace("\n", "\n  | "))
        rep.check("session script ran to its end", "end" in sec, out[-400:] + err[-400:])
        for fn in a.functions:
            rep.check("the live test %s passed (not skipped)" % fn,
                      any(l.startswith("PASS   : TstUiReal::%s()" % fn) for l in live), "\n".join(live[-12:]))
        rep.check("no live function failed or was skipped",
                  any(re.match(r"Totals: \d+ passed, 0 failed, 0 skipped", l) for l in live), "\n".join(live[-12:]))
        if "liveHostServiceWithTheSimulator" in a.functions:
            rep.check("the very low alert reached the interface by the service's signal", any("LIVE very low active" in l for l in live))
            rep.check("acknowledged from the application", any("LIVE acknowledged from the application" in l for l in live))
            rep.check("silent mode set and ended from the application", any("LIVE silent mode set and ended" in l for l in live))
        if "liveThresholdsAndSettingsWithTheHostService" in a.functions:
            rep.check("the challenge was asked and a wrong value refused", any("LIVE challenge asked" in l for l in live))
            rep.check("an impossible order refused with the allowed range", any("LIVE impossible order refused" in l for l in live))
            rep.check("a looser limit accepted after the challenge, history from the journal",
                      any("LIVE looser limit accepted" in l for l in live))
            rep.check("the alert times applied without a restart and clamped by the service",
                      any("LIVE alert times applied" in l for l in live))
            rep.check("language, value in alerts and label applied", any("LIVE ConfigChanged answers" in l for l in live))
        base.no_leak(rep, "the session", out, err, extra=("Subject Interface",))
        print("service stderr (not alert lines):")
        for l in sec.get("service stderr (alert lines only counted)", [])[:12]:
            print("  " + l.strip())
    finally:
        env.cleanup()
    print("%d checks, %d failed" % (rep.count, len(rep.failed)))
    return 1 if rep.failed else 0


if __name__ == "__main__":
    sys.exit(main())
