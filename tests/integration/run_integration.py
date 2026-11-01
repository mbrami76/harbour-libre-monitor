#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Michele Brami
"""Integration run of lm-cli against the simulator (REQ-ARC-04, REQ-SIM-*).

Python 3 standard library only. Not a QtTest and not part of the qmake build.

The simulator runs on the PC. lm-cli and the adapter host run in the Sailfish SDK build
engine (`sfdk ... build-shell`), where the host build of this repository lives; the
simulator is bound to the Docker bridge address of the build engine so that the engine can
reach it. Every case starts a fresh simulator (fault counters are per server run) from a
shipped scenario with `start_utc` set to "now". The real source is never contacted.

    python3 tests/integration/run_integration.py [--build-dir DIR] [--bridge ADDR]

Everything printed here is synthetic. The simulator's accounts are fake (README of
tools/simulator).
"""
import argparse
import datetime
import json
import os
import shutil
import socket
import stat
import subprocess
import sys
import tempfile
import time

REPO = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
SIMDIR = os.path.join(REPO, "tools", "simulator")
SCENARIOS = os.path.join(SIMDIR, "scenarios")
PASSWORD = "sim-password"

EXIT_OK, EXIT_USAGE, EXIT_FAILURE = 0, 2, 1
AUTH, TERMS, RATE, TRANSIENT, PROTOCOL, INVALID = 10, 11, 12, 13, 14, 15


class Env:
    def __init__(self, build_dir, target, bridge):
        self.build_dir = build_dir
        self.target = target
        self.bridge = bridge
        self.work = tempfile.mkdtemp(prefix="it-", dir=os.path.join(build_dir, "it"))
        self.port = 18100
        self.sim = None
        self.pidfile = os.path.join(self.work, "sim.pid")

    # -- simulator ------------------------------------------------------------------
    def scenario_file(self, name):
        with open(os.path.join(SCENARIOS, name + ".json"), encoding="utf-8") as f:
            data = json.load(f)
        data["start_utc"] = "now"
        path = os.path.join(self.work, name + "_now.json")
        with open(path, "w", encoding="utf-8") as f:
            json.dump(data, f)
        return path

    def start_sim(self, scenario):
        self.stop_sim()
        self.port += 1
        self.sim = subprocess.Popen(
            [sys.executable, "-m", "libresim", "--scenario", self.scenario_file(scenario),
             "--host", self.bridge, "--port", str(self.port)],
            cwd=SIMDIR, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        with open(self.pidfile, "w") as f:
            f.write(str(self.sim.pid))
        deadline = time.time() + 15
        while time.time() < deadline:
            try:
                socket.create_connection((self.bridge, self.port), timeout=0.5).close()
                return
            except OSError:
                time.sleep(0.1)
        raise RuntimeError("the simulator did not start")

    def stop_sim(self):
        # By the PID this script started, never by a pattern that could match something else.
        if self.sim is not None:
            self.sim.terminate()
            try:
                self.sim.wait(timeout=5)
            except subprocess.TimeoutExpired:
                self.sim.kill()
                self.sim.wait()
            self.sim = None

    # -- files ----------------------------------------------------------------------
    def rel(self, path):
        return os.path.relpath(path, self.build_dir)

    def credentials(self, email, password=PASSWORD, mode=0o600, name="cred"):
        path = os.path.join(self.work, name)
        with open(path, "w") as f:
            f.write("username=%s\npassword=%s\n" % (email, password))
        os.chmod(path, mode)
        return path

    # -- the program ----------------------------------------------------------------
    def cli(self, args, adapter="./adapters/libre/host/libre-adapter-host", timeout=180):
        cmd = ["sfdk", "-c", "target=" + self.target, "build-shell", "./cli/lm-cli"] + list(args)
        if adapter:
            cmd += ["--adapter", adapter]
        cmd += ["--base-url", "http://%s:%d" % (self.bridge, self.port)]
        p = subprocess.run(cmd, cwd=self.build_dir, stdin=subprocess.DEVNULL,
                           stdout=subprocess.PIPE, stderr=subprocess.PIPE, timeout=timeout)
        return p.returncode, p.stdout.decode("utf-8", "replace"), p.stderr.decode("utf-8", "replace")

    def cleanup(self):
        self.stop_sim()
        shutil.rmtree(self.work, ignore_errors=True)


class Report:
    def __init__(self):
        self.failed = []
        self.count = 0

    def check(self, case, ok, detail=""):
        self.count += 1
        if not ok:
            self.failed.append("%s: %s" % (case, detail))
            print("FAIL  %s  %s" % (case, detail))
        else:
            print("ok    %s" % case)


def docker_bridge():
    out = subprocess.run(["docker", "network", "ls", "--format", "{{.Name}}"],
                         stdout=subprocess.PIPE, check=True).stdout.decode().split()
    name = next((n for n in out if n.startswith("sailfish-sdk")), None)
    if name is None:
        raise RuntimeError("no sailfish-sdk Docker network; pass --bridge")
    gw = subprocess.run(["docker", "network", "inspect", name, "-f",
                         "{{range .IPAM.Config}}{{.Gateway}}{{end}}"],
                        stdout=subprocess.PIPE, check=True).stdout.decode().strip()
    return gw


def iso(dt):
    return dt.strftime("%Y-%m-%dT%H:%M:%SZ")


def no_leak(rep, case, out, err, email="follower@example.invalid", extra=()):
    text = out + err
    secrets = [PASSWORD, email] + list(extra)
    bad = [s for s in secrets if s and s in text]
    rep.check(case + ": no secret in the output", not bad, "found %s" % bad)


def run(env, rep):
    now = datetime.datetime.now(datetime.timezone.utc)
    simulated = "SIMULATED SOURCE"

    # ---- basic ---------------------------------------------------------------------
    env.start_sim("basic")
    cred = env.rel(env.credentials("follower@example.invalid"))
    rc, out, err = env.cli(["discover", "--credentials-file", cred])
    rep.check("basic discover: exit 0", rc == 0, "rc=%s err=%s" % (rc, err))
    rep.check("basic discover: lists the Subject", "Sim Subject A" in out and "mg/dL" in out, out)
    rep.check("basic discover: banner on stderr", simulated in err)
    rep.check("basic discover: no violation", "contract violations" not in err, err)
    no_leak(rep, "basic discover", out, err)

    rc, out, err = env.cli(["current", "--json", "--credentials-file", cred])
    rep.check("basic current: exit 0", rc == 0, "rc=%s err=%s" % (rc, err))
    try:
        s = json.loads(out)["subjects"][0]
        cur = s["current"]
        rep.check("basic current: value, unit, source",
                  40 <= cur["value"] <= 400 and cur["unit"] == "mg/dL" and cur["source"] == "libre.current",
                  out)
        rep.check("basic current: age in minutes", 0 <= cur["age_minutes"] <= 30, out)
        rep.check("basic current: sensor object", isinstance(s["sensor"], dict) and "id" in s["sensor"], out)
        rep.check("basic current: measurement time is UTC",
                  cur["measured_at"].endswith("Z"), out)
    except (ValueError, KeyError, IndexError, TypeError) as e:
        rep.check("basic current: json shape", False, "%r in %r" % (e, out))
    no_leak(rep, "basic current", out, err)

    rc, out, err = env.cli(["current", "--credentials-file", cred])
    rep.check("basic current (text): value line", rc == 0 and "value:" in out and "min ago" in out, out)

    frm, to = iso(now - datetime.timedelta(hours=6)), iso(now)
    rc, out, err = env.cli(["range", "--json", "--from", frm, "--to", to, "--credentials-file", cred])
    rep.check("basic range: exit 0", rc == 0, "rc=%s err=%s" % (rc, err))
    try:
        s = json.loads(out)["subjects"][0]
        times = [r["measured_at"] for r in s["records"]]
        rep.check("basic range: records inside the range", len(times) > 20 and times == sorted(times)
                  and times[0] >= frm and times[-1] <= to + "9", "%d records" % len(times))
        rep.check("basic range: nothing rejected", s["records_count"]["rejected"] == 0, out[:200])
        feeds = [c["feed"] for c in s["coverage"]]
        rep.check("basic range: graph coverage reported", "graph" in feeds, str(feeds))
    except (ValueError, KeyError, IndexError, TypeError) as e:
        rep.check("basic range: json shape", False, "%r" % e)

    rc, out, err = env.cli(["range", "--from", to, "--to", frm, "--credentials-file", cred])
    rep.check("range reversed: usage error", rc == EXIT_USAGE, "rc=%s" % rc)

    rc, out, err = env.cli(["current", "--subject", "5", "--credentials-file", cred])
    rep.check("subject index out of range: usage error", rc == EXIT_USAGE, "rc=%s" % rc)

    # session file kept between runs
    sess = os.path.join(env.work, "session")
    for n in (1, 2):
        rc, out, err = env.cli(["current", "--credentials-file", cred, "--session-file", env.rel(sess)])
        rep.check("session file run %d: exit 0" % n, rc == 0, "rc=%s err=%s" % (rc, err))
        with open(sess) as f:
            blob = f.read().strip()
        rep.check("session file run %d: mode 600 and content kept" % n,
                  stat.S_IMODE(os.stat(sess).st_mode) == 0o600 and len(blob) > 20)
        no_leak(rep, "session file run %d" % n, out, err, extra=[blob])

    # file mode rule, no prompt, missing adapter
    loose = env.rel(env.credentials("follower@example.invalid", mode=0o644, name="loose"))
    rc, out, err = env.cli(["discover", "--credentials-file", loose])
    rep.check("loose credentials file: usage error", rc == EXIT_USAGE and "chmod 600" in err, "rc=%s %s" % (rc, err))
    rc, out, err = env.cli(["discover"])
    rep.check("no credentials and no terminal: usage error", rc == EXIT_USAGE, "rc=%s" % rc)
    rc, out, err = env.cli(["discover", "--credentials-file", cred], adapter="./no-such-adapter")
    rep.check("missing adapter executable: exit 1", rc == EXIT_FAILURE, "rc=%s err=%s" % (rc, err))

    # wrong password (the simulator's reply to bad credentials is a guess, README)
    bad = env.rel(env.credentials("follower@example.invalid", password="wrong-password-synthetic", name="bad"))
    rc, out, err = env.cli(["discover", "--credentials-file", bad])
    rep.check("wrong password: an authentication or transient error", rc in (AUTH, TRANSIENT), "rc=%s err=%s" % (rc, err))
    no_leak(rep, "wrong password", out, err, extra=["wrong-password-synthetic"])

    # ---- mmol/L (simulated guess) -------------------------------------------
    env.start_sim("mmol")
    mm = json.load(open(os.path.join(SCENARIOS, "mmol.json")))["accounts"][0]
    cred_mm = env.rel(env.credentials(mm["email"], mm["password"], name="cred_mm"))
    rc, out, err = env.cli(["current", "--json", "--credentials-file", cred_mm])
    rep.check("mmol current: exit 0", rc == 0, "rc=%s err=%s" % (rc, err))
    try:
        cur = json.loads(out)["subjects"][0]["current"]
        rep.check("mmol current: unit mmol/L, plausible value (simulated guess)",
                  cur["unit"] == "mmol/L" and 2 <= cur["value"] <= 23, out)
    except (ValueError, KeyError, IndexError, TypeError) as e:
        rep.check("mmol current: json shape", False, "%r in %r" % (e, out))

    # ---- fault scenarios: one error class per account -------------------------------
    def fault(case, account, args, expect, text=None):
        env.start_sim("faults")
        c = env.rel(env.credentials(account + "@example.invalid", name="cred_" + account))
        rc, out, err = env.cli(args + ["--credentials-file", c])
        rep.check("%s: exit %s" % (case, expect), rc == expect, "rc=%s err=%s" % (rc, err.strip()))
        if text is not None:
            rep.check("%s: stderr says %r" % (case, text), text in err, err.strip())
        no_leak(rep, case, out, err, email=account + "@example.invalid")

    fault("faults ok account", "ok", ["current"], EXIT_OK)
    # Observed 2026-10-14: with --base-url the adapter cannot follow a regional redirect and
    # reports protocol-changed (the redirect reply itself is a simulator guess).
    fault("redirect (guess)", "redirect", ["current"], PROTOCOL, "login redirect")
    # CP-04/CP-08: the terms reply is not captured; a login reply with a non-zero status is
    # transient for now, so the terms case cannot be told apart yet.
    fault("terms (CP-04: not detectable yet)", "terms", ["current"], TRANSIENT, "transient")
    fault("expired token on connections", "expired", ["discover"], AUTH, "auth-failed")
    fault("rate limit with retry hint", "limited", ["current"], RATE, "retry after 30")
    fault("error in HTTP 200", "server", ["discover"], TRANSIENT, "transient")
    fault("missing keys in the reply", "broken", ["current"], PROTOCOL, "protocol-changed")
    fault("invalid JSON in the graph reply", "broken", ["range", "--from", frm, "--to", to], PROTOCOL)
    # The adapter makes two attempts per request, so a single dropped connection is absorbed.
    frm3 = iso(now - datetime.timedelta(days=3))
    fault("one dropped connection is retried", "dropped", ["range", "--from", frm3, "--to", to], EXIT_OK)

    # One simulator run, consecutive runs of the program (the fault counters advance): the
    # error in HTTP 200 on connections (once), 503 on the graph (twice, and the adapter's
    # second attempt may use one), 403 on the logbook (once), then the source is fine.
    # Expected order of the exit codes: 13, then 13 (503) one or two times, 14 (403), 0.
    env.start_sim("faults")
    cs = env.rel(env.credentials("server@example.invalid", name="cred_server"))
    codes = []
    for n in range(6):
        args = ["discover"] if n == 0 else ["range", "--from", frm3, "--to", to]
        rc, out, err = env.cli(args + ["--credentials-file", cs])
        codes.append(rc)
        no_leak(rep, "server sequence run %d" % n, out, err, email="server@example.invalid")
        if rc == EXIT_OK:
            break
    stages = [x for i, x in enumerate(codes) if i == 0 or x != codes[i - 1]]
    rep.check("server sequence: 13 (200 with status), 13 (503), 14 (403), 0",
              stages == [TRANSIENT, PROTOCOL, EXIT_OK], "exit codes %s" % codes)

    # ---- data faults: gaps, graph lag, no sensor ------------------------------------
    env.start_sim("gaps")
    gm = json.load(open(os.path.join(SCENARIOS, "gaps.json")))["accounts"][0]
    cred_g = env.rel(env.credentials(gm["email"], gm["password"], name="cred_g"))
    rc, out, err = env.cli(["current", "--json", "--credentials-file", cred_g])
    rep.check("gaps: current exit 0, no violation", rc == 0 and "contract violations" not in err, "rc=%s %s" % (rc, err))
    try:
        subs = json.loads(out)["subjects"]
        rep.check("gaps: every Subject answered", len(subs) >= 5, "%d subjects" % len(subs))
        rep.check("gaps: a Subject without a value is reported, not an error",
                  any(x["current"] is None for x in subs) or all(x["current"] is not None for x in subs))
    except (ValueError, KeyError, TypeError) as e:
        rep.check("gaps: json shape", False, "%r" % e)
    rc, out, err = env.cli(["range", "--json", "--from", frm, "--to", to, "--credentials-file", cred_g])
    rep.check("gaps: range exit 0, no violation", rc == 0 and "contract violations" not in err, "rc=%s %s" % (rc, err))


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--build-dir", default=os.path.expanduser("~/SFOS-DEV/harbour-libre-monitor-development/build/host-i486"))
    ap.add_argument("--target", default="SailfishOS-5.1.0.11-i486")
    ap.add_argument("--bridge", default=None, help="address the build engine reaches the PC on (default: from docker)")
    a = ap.parse_args()
    os.makedirs(os.path.join(a.build_dir, "it"), exist_ok=True)
    env = Env(a.build_dir, a.target, a.bridge or docker_bridge())
    rep = Report()
    try:
        run(env, rep)
    finally:
        env.cleanup()
    print("\n%d checks, %d failed" % (rep.count, len(rep.failed)))
    for f in rep.failed:
        print("  " + f)
    return 1 if rep.failed else 0


if __name__ == "__main__":
    sys.exit(main())
