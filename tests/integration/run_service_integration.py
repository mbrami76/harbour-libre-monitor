#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Michele Brami
"""Integration run of the monitoring service against the simulator.

Python 3 standard library only. Not a QtTest and not part of the qmake build.

The simulator runs on the PC; the service (lm-service, the host executable of daemon/host), the
adapter host and a private D-Bus daemon run in the Sailfish SDK build engine, driven by
service_session.sh. What is checked
start by D-Bus activation; polling that stores readings (a successful poll shows in the
status); heartbeats and a start record in a REAL journal; a kill -9 and a restart with the
cause `after-missing-stop`; a second instance that exits without writing; the stop sequence on
SIGTERM (a stop record with the reason `term`). The real source is never contacted and every
account here is synthetic.

    python3 tests/integration/run_service_integration.py [--build-dir DIR] [--bridge ADDR]
"""
import argparse
import hashlib
import json
import os
import shutil
import subprocess
import sys
import tempfile
import uuid

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.dirname(os.path.dirname(HERE))
sys.path.insert(0, HERE)
sys.path.insert(0, os.path.join(REPO, "tools", "simulator"))

import run_integration as base  # noqa: E402  (the simulator helpers of the CLI run)


def patient_ref(seed, key):
    """The simulator's patient id for a Subject key (libresim.server._uuid, same recipe)."""
    from libresim import server
    return server._uuid(seed, "patient", key)


def sections(text):
    out, cur = {}, None
    for line in text.splitlines():
        if line.startswith("=== "):
            cur = line[4:]
            out[cur] = []
        elif cur is not None:
            out[cur].append(line)
    return out


def entries(lines):
    res = []
    for l in lines:
        l = l.strip()
        if l.startswith("{"):
            try:
                res.append(json.loads(l))
            except ValueError:
                pass
    return res


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--build-dir", default=os.path.expanduser("~/SFOS-DEV/harbour-libre-monitor-development/build/host-svc-i486"))
    ap.add_argument("--target", default="SailfishOS-5.1.0.11-i486")
    ap.add_argument("--bridge", default=None)
    a = ap.parse_args()

    os.makedirs(os.path.join(a.build_dir, "it"), exist_ok=True)
    rep = base.Report()
    env = base.Env(a.build_dir, a.target, a.bridge or base.docker_bridge())
    try:
        env.start_sim("basic")
        scen = json.load(open(os.path.join(base.SCENARIOS, "basic.json")))
        ref = patient_ref(scen["seed"], "a")
        data = os.path.join(env.work, "data")
        os.makedirs(data)
        subject_id = "{%s}" % uuid.uuid4()
        with open(os.path.join(data, "config.json"), "w") as f:
            json.dump({"format": 1, "connections": [{
                "id": "c1", "adapter": "libre", "subjects": [{"id": subject_id, "ref": ref}],
                "current_interval_s": 60}]}, f)
        env.credentials("follower@example.invalid", name="cred")
        env.credentials("follower@example.invalid", password="wrong-password-synthetic", name="bad")

        script = os.path.join(HERE, "service_session.sh")
        cmd = ["sfdk", "-c", "target=" + a.target, "build-shell", "sh", script, a.build_dir,
               env.work, "http://%s:%d" % (env.bridge, env.port)]
        try:
            p = subprocess.run(cmd, cwd=a.build_dir, stdin=subprocess.DEVNULL, stdout=subprocess.PIPE,
                               stderr=subprocess.PIPE, timeout=600)
            out = p.stdout.decode("utf-8", "replace")
            err = p.stderr.decode("utf-8", "replace")
        except subprocess.TimeoutExpired as e:
            out = (e.stdout or b"").decode("utf-8", "replace")
            err = (e.stderr or b"").decode("utf-8", "replace") + "\n(timed out)"
        sec = sections(out)

        rep.check("session script ran to its end", "end" in sec, out[-400:] + err[-400:])

        act = sec.get("activation", [])
        rep.check("activation: a call to the name started the service and wrote a start record",
                  "START_RECORD ok" in act, "\n".join(act))
        e = entries(act)
        starts = [x for x in e if x["kind"] == "start"]
        rep.check("activation: first start cause", bool(starts) and starts[0]["cause"] == "first-start", str(starts))
        rep.check("activation: the start record has both counters",
                  bool(starts) and starts[0]["boot_time_ms"] >= 0 and starts[0]["monotonic_ms"] >= 0, str(starts))

        poll = sec.get("polling", [])
        if os.environ.get("LM_DEBUG"): print("\n".join(sec.get("polling", []))[:900])
        rep.check("polling: a reading was fetched and stored through the simulator",
                  "POLLED ok" in poll, "\n".join(poll)[:600])
        status = []
        for l in poll:
            l = l.strip()
            if l.startswith("{"):
                try:
                    status.append(json.JSONDecoder().raw_decode(l)[0])
                except ValueError:
                    pass
        if status:
            s = status[-1]
            rep.check("status: service running with the start cause",
                      s["service"]["state"] == "running" and s["service"]["start_cause"] == "first-start", str(s["service"]))
            rep.check("status: the Subject appears by UUID only",
                      s["subjects"] and s["subjects"][0]["subject"] == subject_id, str(s["subjects"]))
            sub = s["subjects"][0] if s["subjects"] else {}
            rep.check("status: the Subject carries its adapter and version",
                      sub.get("adapter") == "libre" and bool(sub.get("adapter_version"))
                      and sub.get("report_proposed") is False, str(sub))
            rep.check("status: the trend arrow of the newest reading, from the real adapter and the simulator, with its time",
                      isinstance(sub.get("trend_code"), int) and sub.get("trend_for") == sub.get("last_success_at")
                      and bool(sub.get("trend_for")), str(sub))
            text = json.dumps(s)
            rep.check("status: no source reference, name or password in the status",
                      ref not in text and "Sim Subject" not in text and base.PASSWORD not in text, text[:300])
        else:
            rep.check("status: JSON present", False, "\n".join(poll)[:300])

        hb = entries(sec.get("heartbeats", []))
        rep.check("heartbeats: at least two in a few seconds (interval 2 s in this run)",
                  len([x for x in hb if x["kind"] == "heartbeat"]) >= 2, str(hb))

        # Discover: a good probe, a probe too soon, a wrong login, a known connection.
        def answer(name):
            for l in sec.get(name, []):
                l = l.strip()
                if l.startswith("{"):
                    try:
                        return json.loads(l)
                    except ValueError:
                        pass
            return None

        def file_state(name):
            for l in sec.get(name, []):
                if l.startswith("PROBE_FILE_AFTER"):
                    return l.split()[-1]
            return None

        good = answer("discover good probe")
        rep.check("discover: the probe secret was there before the call",
                  "PROBE_FILE_BEFORE present" in sec.get("discover good probe", []), "\n".join(sec.get("discover good probe", [])))
        rep.check("discover: a good probe answers the Subjects of the account",
                  bool(good) and good.get("ok") is True and ref in [x.get("subject_ref") for x in good.get("subjects", [])],
                  str(good)[:300])
        rep.check("discover: each Subject has a name, a unit and how the unit was determined",
                  bool(good) and all(x.get("display_name") and "unit" in x and x.get("unit_basis") for x in good.get("subjects", []))
                  and bool(good.get("subjects")), str(good)[:300])
        rep.check("discover: the probe secret is gone after the answer", file_state("discover good probe") == "gone",
                  "\n".join(sec.get("discover good probe", [])))
        soon = answer("discover too soon")
        rep.check("discover: a second probe within 30 s is rate-limited",
                  bool(soon) and soon.get("ok") is False and soon.get("error") == "rate-limited", str(soon))
        rep.check("discover: a refused probe takes its secret out too", file_state("discover too soon") == "gone",
                  "\n".join(sec.get("discover too soon", [])))
        wrong = answer("discover wrong login")
        rep.check("discover: a wrong login is an error class (auth-failed, or unreachable until the source's reply is captured)",
                  bool(wrong) and wrong.get("ok") is False and wrong.get("error") in ("auth-failed", "unreachable"), str(wrong))
        print("note: the wrong login was answered with %s" % (wrong.get("error") if wrong else None))
        rep.check("discover: the wrong login's secret is gone", file_state("discover wrong login") == "gone",
                  "\n".join(sec.get("discover wrong login", [])))
        known = answer("discover known connection")
        rep.check("discover: a known connection answers its Subjects without a probe secret",
                  bool(known) and known.get("ok") is True and ref in [x.get("subject_ref") for x in known.get("subjects", [])],
                  str(known)[:300])
        unknown = answer("discover unknown")
        rep.check("discover: an unknown connection is refused",
                  bool(unknown) and unknown.get("error") == "unknown-connection", str(unknown))
        rep.check("discover: no password in any answer",
                  base.PASSWORD not in "\n".join(sum((sec.get(k, []) for k in sec if k.startswith("discover")), []))
                  and "wrong-password-synthetic" not in "\n".join(sum((sec.get(k, []) for k in sec if k.startswith("discover")), [])),
                  "found")

        second = sec.get("second instance", [])
        rep.check("second instance: exits with 0", "SECOND_RC 0" in second, "\n".join(second))
        rep.check("second instance: no second start record", "SECOND_STARTS 1" in second, "\n".join(second))

        rep.check("kill: the service was gone after kill -9", "KILLED ok" in sec.get("kill", []), "\n".join(sec.get("kill", [])))
        rs = sec.get("restart after kill", [])
        rep.check("restart: the next call starts it again with after-missing-stop",
                  "RESTART_CAUSE ok" in rs, "\n".join(rs))
        re_ = entries(rs)
        starts2 = [x for x in re_ if x["kind"] == "start"]
        rep.check("restart: two start records, the second after-missing-stop with previous heartbeat",
                  len(starts2) == 2 and starts2[1]["cause"] == "after-missing-stop"
                  and starts2[1]["extra"].get("previous") == "heartbeat", str(starts2))
        rep.check("restart: no stop record was written by the kill", not [x for x in re_ if x["kind"] == "stop"], str(re_))

        term = sec.get("sigterm", [])
        rep.check("sigterm: the service exited", "TERM_EXIT ok" in term, "\n".join(term))
        te = entries(term)
        stops = [x for x in te if x["kind"] == "stop"]
        rep.check("sigterm: a stop record with reason term and both counters",
                  len(stops) == 1 and stops[0]["extra"].get("reason") == "term"
                  and stops[0]["boot_time_ms"] >= 0 and stops[0]["monotonic_ms"] >= 0, str(stops))

        combined = out + err
        rep.check("no password or token in anything printed",
                  base.PASSWORD not in combined, "found in output")
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
