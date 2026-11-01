#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Michele Brami
"""Integration run of the alert engine inside the service, against the simulator.

Python 3 standard library only. Not a QtTest and not part of the qmake build. It takes about 60
minutes because it runs in REAL time: the simulated Subject goes through a low and an urgent low,
a high and an urgent high, and a stretch with no new readings, one reading a minute, while the host
service (lm-service, daemon/host) polls it and the alert coordinator judges every reading. The
durations of the alert engine (persistence, reminders, re-alert, the missing-data limit) are divided
by a scale (--alert-scale, host executable only) so that they fit; the thresholds are the real ones
(54, 70, 180, 250 mg/dL), and LL and HH have nothing to scale. The service is killed with kill -9
while the urgent high alert is active and started again by D-Bus, and the state must come back.

The simulator writes its excursions relative to the start of the run, so this script writes the
scenario itself (a flat Subject, synthetic) into the work directory. The real source is never
contacted and every account is synthetic.

What is checked: the sequence of journal lines of the low slot, the high slot and the missing-data
slot; reminders of the urgent low; the restore after kill -9; that a notification carries only a UUID,
the slot, the level word and the state; that no name, password or glucose value is in the output.

    python3 tests/integration/run_alert_integration.py [--build-dir DIR] [--bridge ADDR] [--scale N]
"""
import argparse
import datetime
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
    return datetime.datetime.utcfromtimestamp(epoch).strftime("%Y-%m-%dT%H:%M:%SZ")


def sections(text):
    return svc.sections(text)


def jlines(lines):
    out = []
    for l in lines:
        l = l.strip()
        if l.startswith("{"):
            try:
                out.append(json.loads(l))
            except ValueError:
                pass
    return out


def subsequence(wanted, seq):
    it = iter(seq)
    return all(any(w == s for s in it) for w in wanted)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--build-dir", default=os.path.expanduser("~/SFOS-DEV/harbour-libre-monitor-development/build/host-alerts-i486"))
    ap.add_argument("--target", default="SailfishOS-5.1.0.11-i486")
    ap.add_argument("--bridge", default=None)
    ap.add_argument("--scale", type=int, default=30)
    a = ap.parse_args()

    os.makedirs(os.path.join(a.build_dir, "it"), exist_ok=True)
    rep = base.Report()
    env = base.Env(a.build_dir, a.target, a.bridge or base.docker_bridge())
    try:
        t0 = time.time()
        minute = 60.0
        scenario = {
            "name": "alerts-run", "seed": 11, "start_utc": "now", "timezone": "UTC",
            "accounts": [{"email": "follower@example.invalid", "password": base.PASSWORD,
                          "unit": "mg/dL", "country": "IT", "subjects": ["a"]}],
            "subjects": [{"key": "a", "first_name": "Sim", "last_name": "Subject Alerts",
                          "sensor": {"age_days": 5},
                          "profile": {"baseline": 110.0, "daily_amp": 0.0, "meals": [], "night_dip": None,
                                      "noise": 0.0, "drift_amp": 0.0,
                                      "excursions": [
                                          # a low that goes urgent low and comes back, slowly enough for one reading a minute to
                                          # see L, then LL, then L again: L about +8.6, LL +10.6 to +15.4, L until +17.4
                                          {"at_utc": iso(t0 + 3 * minute), "duration_min": 20, "peak": 44},
                                          # a high that goes urgent high: H from +28.2, HH +29.6 to +36.4
                                          {"at_utc": iso(t0 + 25 * minute), "duration_min": 16, "peak": 330}]}}],
            "faults": [{"kind": "gap", "subject": "a", "start_utc": iso(t0 + 45 * minute),
                        "end_utc": iso(t0 + 51 * minute)}],
        }
        scen_path = os.path.join(env.work, "alerts_run.json")
        with open(scen_path, "w", encoding="utf-8") as f:
            json.dump(scenario, f)
        # the simulator, from this scenario file
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
        t0 = time.time()        # the simulator's "now"; the excursions above are within a second of it

        ref = svc.patient_ref(scenario["seed"], "a")
        data = os.path.join(env.work, "data")
        os.makedirs(data)
        subject_id = "{%s}" % uuid.uuid4()
        with open(os.path.join(data, "config.json"), "w") as f:
            json.dump({"format": 1, "connections": [{
                "id": "c1", "adapter": "libre", "subjects": [{"id": subject_id, "ref": ref}],
                "current_interval_s": 60}]}, f)
        env.credentials("follower@example.invalid", name="cred")

        kill_at = int(t0 + 33 * minute)
        end_at = int(t0 + 57 * minute)
        script = os.path.join(HERE, "alert_session.sh")
        set_at = int(t0 + 1 * minute)           # the threshold edit, before the excursions
        act_at = int(t0 + 48 * minute)          # the acknowledgement and snooze, in the gap
        cmd = ["sfdk", "-c", "target=" + a.target, "build-shell", "sh", script, a.build_dir, env.work,
               "http://%s:%d" % (env.bridge, env.port), str(kill_at), str(end_at), str(a.scale),
               subject_id, str(set_at), str(act_at)]
        try:
            p = subprocess.run(cmd, cwd=a.build_dir, stdin=subprocess.DEVNULL, stdout=subprocess.PIPE,
                               stderr=subprocess.PIPE, timeout=60 * 60)
            out = p.stdout.decode("utf-8", "replace")
            err = p.stderr.decode("utf-8", "replace")
        except subprocess.TimeoutExpired as e:
            out = (e.stdout or b"").decode("utf-8", "replace")
            err = (e.stderr or b"").decode("utf-8", "replace") + "\n(timed out)"
        sec = sections(out)
        if os.environ.get("LM_DEBUG"):
            print(out[-6000:])

        rep.check("session script ran to its end", "end" in sec, out[-400:] + err[-400:])
        rep.check("the service started and polled the simulator", "POLLED ok" in sec.get("start", []),
                  "\n".join(sec.get("start", [])))
        rep.check("kill -9 stopped the service and D-Bus started it again",
                  "KILLED ok" in sec.get("kill", []) and "RESTARTED ok" in sec.get("kill", []),
                  "\n".join(sec.get("kill", [])))
        rep.check("the service stopped on SIGTERM", "TERM_EXIT ok" in sec.get("term", []), "\n".join(sec.get("term", [])))

        # ---- the D-Bus methods ---------------------------------------
        def answers(section):
            out = {}
            for l in sec.get(section, []):
                m = re.match(r"^ANS (\S+) (.*)$", l)
                if m:
                    out[m.group(1)] = m.group(2)
            return out

        def js(text):
            try:
                return json.loads(text)
            except ValueError:
                return {}

        th = answers("thresholds")
        rep.check("SetThresholds: L 71 (no challenge needed above the standard 70) is accepted",
                  js(th.get("set_l_71", "")).get("result") == "accepted", str(th.get("set_l_71")))
        refused = js(th.get("set_l_65_refused", ""))
        rep.check("SetThresholds: 65 is refused for want of the value typed again, with the standard values",
                  refused.get("result") == "needs-confirmation" and refused.get("global_l") == 70, str(refused))
        rep.check("SetThresholds: an unknown Subject is refused in words",
                  js(th.get("set_unknown_subject", "")).get("result") == "unknown-subject", str(th.get("set_unknown_subject")))
        after_edit = js(th.get("alerts_after_edit", ""))
        rep.check("Alerts() lists the thresholds in force, L now 71",
                  [t.get("l") for t in after_edit.get("thresholds", [])] == [71], str(after_edit))
        restored = js(answers("restored").get("alerts_after_restart", ""))
        rep.check("after kill -9 the thresholds are still L 71 and the urgent high alert is still listed as active",
                  [t.get("l") for t in restored.get("thresholds", [])] == [71]
                  and any(s.get("level") == "HH" and s.get("state") == "active" for s in restored.get("alerts", [])), str(restored))
        act = answers("actions")
        def slot_state(key):
            return [s.get("state") for s in js(act.get(key, "")).get("alerts", []) if s.get("slot") == "missing"]
        rep.check("the missing-data alert is listed as active, then acknowledged, then snoozed",
                  slot_state("alerts_missing_active") == ["active"] and slot_state("alerts_missing_acknowledged") == ["acknowledged"]
                  and slot_state("alerts_missing_snoozed") == ["snoozed"], str(act)[:500])
        rep.check("AcknowledgeAlert and SnoozeAlert answer in words",
                  act.get("ack_missing") == "done" and act.get("ack_missing_again") == "already-acknowledged"
                  and act.get("ack_bad_slot") == "bad-slot" and act.get("snooze_bad_duration") == "bad-duration"
                  and act.get("snooze_missing") == "done", str({k: v for k, v in act.items() if not k.startswith("alerts")}))
        snoozed = [s for s in js(act.get("alerts_missing_snoozed", "")).get("alerts", []) if s.get("slot") == "missing"]
        rep.check("a snoozed slot says until when", bool(snoozed) and bool(snoozed[0].get("snooze_until")), str(snoozed))
        sigs = sec.get("signals", [])
        if sigs and sigs[0].strip() == "NO_MONITOR":
            print("note: no dbus-monitor in the build engine; the signal AlertChanged was not checked")
        else:
            text = "\n".join(sigs)
            seen = re.findall(r'string "(low|high|missing)"\s+string "(\w+)"', text)
            rep.check("the signal AlertChanged announced the low, the high and the missing-data slots changing state",
                      all(any(s == slot for s, _ in seen) for slot in ("low", "high", "missing"))
                      and {"active", "acknowledged", "snoozed", "cleared"} <= {st for _, st in seen}, str(seen)[:400])
            rep.check("the signal names the Subject by the key of the configuration",
                      subject_id.lower() in text.lower(), text[:300])

        entries = [e for e in jlines(sec.get("alerts", [])) if e["kind"] == "alert"]
        thr = [e["extra"] for e in jlines(sec.get("alerts", [])) if e["kind"] == "threshold"]
        rep.check("the threshold edit is in the journal (L 71, previous 70) and carries no comment",
                  len(thr) == 1 and thr[0].get("boundary") == "L" and thr[0].get("value") == 71 and thr[0].get("previous") == 70
                  and "comment" not in thr[0],
                  str(thr))
        ex = [e["extra"] for e in entries]
        rep.check("the journal holds alert entries", len(ex) >= 8, "%d entries" % len(ex))

        ex.sort(key=lambda x: x["seq"])         # the engine's own order (equal wall times)

        def seq(slot):
            return [(x["alert"], x["state"], x["event"]) for x in ex if x["slot"] == slot]

        low = seq("low")
        high = seq("high")
        missing = seq("missing")
        print("low   :", low)
        print("high  :", high)
        print("missing:", missing)

        rep.check("low slot: L active, then LL (escalated), then back to L, then cleared (hysteresis)",
                  subsequence([("L", "active", "active"), ("LL", "active", "escalated"),
                               ("L", "active", "deescalated"), ("L", "cleared", "cleared")], low), str(low))
        rep.check("low slot: LL is active at once, never pending",
                  not [x for x in low if x[0] == "LL" and x[1] == "pending"], str(low))
        rep.check("high slot: H pending (persistence), then HH active, then cleared",
                  subsequence([("H", "pending"), ("HH", "active")], [(x[0], x[1]) for x in high])
                  and any(x[1] == "cleared" for x in high), str(high))
        rep.check("high slot: HH is never pending", not [x for x in high if x[0] == "HH" and x[1] == "pending"], str(high))
        rep.check("missing data: pending, active, acknowledged, snoozed, cleared by a reading, in that order",
                  subsequence([("MISSING", "pending"), ("MISSING", "active"), ("MISSING", "acknowledged"),
                               ("MISSING", "snoozed"), ("MISSING", "cleared")], [(x[0], x[1]) for x in missing]), str(missing))
        updates = [l for l in sec.get("alert lines", []) if "kind=update" in l and "slot=missing" in l]
        rep.check("missing data: the notification names the cause once the scheduler has (no new readings, or the source cannot be reached)",
                  any("cause=no-new-readings" in l or "cause=source-unreachable" in l for l in updates)
                  or any("kind=notify" in l and "slot=missing" in l and re.search(r"cause=(no-new-readings|source-unreachable)", l)
                         for l in sec.get("alert lines", [])), "\n".join(sec.get("alert lines", []))[-600:])
        rep.check("every episode ends: no slot is left active at the end except by chance",
                  all(x["state"] in ("cleared", "normal") for x in [
                      {"state": s[-1][1]} for s in (low, high, missing) if s]), str((low[-1:], high[-1:], missing[-1:])))
        rep.check("an alert entry holds no glucose value and no name",
                  not any(k in x for x in ex for k in ("value", "glucose", "name", "mg_dl")), str(ex[:2]))

        lines = sec.get("alert lines", [])
        fmt = re.compile(r"^alert: kind=(notify|remind|update|withdraw|interruption) ")
        rep.check("the notifications were delivered", len([l for l in lines if "kind=notify" in l]) >= 4, "\n".join(lines))
        rep.check("LL was reminded while unacknowledged",
                  any("kind=remind" in l and "alert=LL" in l for l in lines), "\n".join(lines))
        allowed = re.compile(r"^alert: kind=\w+ (subject=\{[0-9a-f-]+\} slot=\w+ alert=\w+ state=\w+ cause=[\w-]* silent=[01]|from=\S+ to=\S+)$")
        rep.check("a notification carries only the Subject's UUID, the slot, the level word, the state and a cause",
                  all(allowed.match(l) for l in lines), "\n".join(l for l in lines if not allowed.match(l))[:400])
        rep.check("the notifications name the Subject by the UUID of the configuration",
                  all(subject_id.lower() in l for l in lines if "interruption" not in l), "\n".join(lines)[:300])

        # The restore after kill -9: the lines the second instance printed at its start.
        before = int((re.search(r"ERR_LINES_BEFORE_KILL (\d+)", out) or [0, 0])[1])
        # the second instance's lines are those after the line count the shell recorded
        err_file = os.path.join(env.work, "svc.err")
        tail = [l for l in open(err_file, encoding="utf-8", errors="replace").read().splitlines()[before:]
                if l.startswith("alert:")] if os.path.exists(err_file) else []
        rep.check("after kill -9 the active urgent high alert was notified again by the new instance",
                  any("kind=notify" in l and "alert=HH" in l and "state=active" in l for l in tail[:6]), "\n".join(tail[:8]))
        episodes = {}
        for x in ex:
            episodes.setdefault((x["slot"], x["episode"]), []).append(x["state"])
        rep.check("each episode starts once and ends once (the restore did not split or repeat one)",
                  all(v.count("cleared") <= 1 and v[0] in ("pending", "active") for v in episodes.values()), str(episodes))

        combined = out + err + "\n".join(lines)
        rep.check("no password, source reference or name in anything printed",
                  base.PASSWORD not in combined and ref not in combined and "Sim Subject" not in combined,
                  "found")
    finally:
        if os.environ.get("LM_KEEP"):
            env.stop_sim()
            print("kept", env.work)
        else:
            env.cleanup()

    print("%d checks, %d failed" % (rep.count, len(rep.failed)))
    return 1 if rep.failed else 0


if __name__ == "__main__":
    sys.exit(main())
