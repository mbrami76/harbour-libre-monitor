#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Michele Brami
"""Upgrade test: the new service on data written by the first real release.

Python 3 standard library only. Not a QtTest and not part of the qmake build; rerun it whenever the
store, the journal or the configuration file changes.

Why it exists. The author installs the alpha (0.1-2) over the release of the first real run (0.1-1),
whose store already holds real readings. The new build knows journal kinds the old one did not
(`alert`, `threshold`, `silent`) and reads more configuration keys. This test proves, on synthetic
data only, that nothing is lost or broken by the upgrade, and finds out what a DOWNGRADE (the
rollback to 0.1-1) does with the upgraded data, because the author's rollback procedure depends on it
.

What it does:
  1. Builds the OLD tree as a host build if that build is not there yet. The old tree is taken from
     git with `git worktree add --detach` at the commit of the first real release (default 66728e4,
     the provisioning commit whose package ran the first real run),
     and the worktree is removed again after the build; nothing is copied into the repository.
  2. Writes the configuration file in the exact shape the old provisioning mode wrote it
     (daemon/provisioning.cpp `upsertConnection` at that commit: format 1, one connection `c1` of the
     adapter `libre` with one Subject {id, ref}, indented JSON, mode 0600). The old provisioning mode
     cannot run on the PC (it asks at a terminal and writes the phone's secret store), so the file is
     reproduced here; that is the one part not written by the old binary.
  3. FILL: the OLD service runs four times against the simulator (a start, the first polls, an
     orderly stop), so the store holds the graph of the last 11.7 hours with a two-hour signal gap
     (a simulator fault), the logbook of 14 days, the current readings of each run, a sensor and the
     ingest log; the journal holds the starts, stops, heartbeats and connection states of four runs.
     (Why not two days of readings: see FILL_RUNS below.)
  4. UPGRADE: the NEW service starts on a COPY of that data directory, against a simulator whose
     readings go very low now. Checked: every row of every table of the store and the journal is
     still there unchanged (row hashes; nothing is printed but counts), the schema versions, the
     configuration file is byte for byte the same, the Subject is followed, the thresholds are the
     standard ones and the per-Subject alert times are the defaults, an alert is raised from the
     readings, a second restart is clean and restores the same alert episode.
  5. DOWNGRADE: the OLD service on a copy of the upgraded data, with the configuration keys the
     alpha's interface writes added to the file. Recorded, not required: whether it starts, follows
     the Subject, polls, stops, and whether it keeps every row.
  6. UPGRADE AGAIN: the NEW service on the data the downgrade left, as after a second install.

Nothing here contacts the real source; every account is the simulator's fake one. The output holds
counts, states and check names, never a glucose value, a name or a reference.

    python3 tests/integration/run_upgrade_test.py [--old-commit C] [--old-build-dir D]
                                                   [--new-build-dir D] [--bridge ADDR] [--keep]
"""
import argparse
import collections
import hashlib
import json
import os
import re
import shutil
import socket
import sqlite3
import subprocess
import sys
import tempfile
import time
import uuid

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.dirname(os.path.dirname(HERE))
sys.path.insert(0, HERE)
sys.path.insert(0, os.path.join(REPO, "tools", "simulator"))

import run_integration as base  # noqa: E402  (Report, docker_bridge, credentials, PASSWORD)
import run_service_integration as svc  # noqa: E402  (patient_ref, sections)

HOUR = 3600.0
# Number of runs of the old service while filling. Each run is a start, the first polls and an orderly
# stop, as a phone that was restarted a few times. The simulator's clock is "now" each time: the
# scheduler asks for the history relative to the wall clock, so a simulated clock in the past gives
# no graph readings at all (tried first: four runs placed 40, 28, 16 and 4 hours back stored one
# current reading and no graph). The store therefore holds what the simulator can give at once: the
# graph of the last 11.7 hours with a two-hour signal gap, the logbook of 14 days, the current
# readings of each run and one sensor; about two days of readings,, would need a
# real day or two of running, which adds nothing to what the upgrade touches (the tables and the
# schema are the same whatever their size).
FILL_RUNS = 4
SEED = 21
# Columns that a later run is ALLOWED to change in a row it did not write: the sensor table keeps
# when the sensor was last seen (core/store/store.cpp), which every poll refreshes by design. Every
# other column of every row must stay as it was.
UPDATED_BY_DESIGN = {"sensor": {"last_seen"}}


def iso(epoch):
    return time.strftime("%Y-%m-%dT%H:%M:%SZ", time.gmtime(epoch))


def scenario(start_epoch, now_epoch, excursion_at=None):
    """The synthetic Subject. The sensor and the gap are absolute times, the same in every run, so the
    series stays one continuous function of the simulated time (tools/simulator/README.md)."""
    profile = {"noise": 2.0}
    if excursion_at is not None:
        # very low around excursion_at + 20 min; only the run of the new service sees it
        profile["excursions"] = [{"at_utc": iso(excursion_at), "duration_min": 40, "peak": 42}]
    return {
        "name": "upgrade", "seed": SEED, "start_utc": iso(start_epoch), "timezone": "Europe/Rome",
        "accounts": [{"email": "follower@example.invalid", "password": base.PASSWORD,
                      "unit": "mg/dL", "country": "IT", "subjects": ["a"]}],
        "subjects": [{"key": "a", "first_name": "Sim", "last_name": "Subject Upgrade",
                      "sensor": {"activated_utc": iso(now_epoch - 9 * 24 * HOUR)},
                      "profile": profile}],
        "faults": [{"kind": "gap", "subject": "a", "start_utc": iso(now_epoch - 7 * HOUR),
                    "end_utc": iso(now_epoch - 5 * HOUR)}],
    }


class Sim:
    """One simulator process on the Docker bridge, started from a scenario file of the work dir."""
    port = 18300

    def __init__(self, work, bridge, scen):
        Sim.port += 1
        self.port = Sim.port
        self.url = "http://%s:%d" % (bridge, self.port)
        path = os.path.join(work, "scenario-%d.json" % self.port)
        with open(path, "w", encoding="utf-8") as f:
            json.dump(scen, f)
        self.proc = subprocess.Popen(
            [sys.executable, "-m", "libresim", "--scenario", path, "--host", bridge, "--port", str(self.port)],
            cwd=base.SIMDIR, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        deadline = time.time() + 15
        while time.time() < deadline:
            try:
                socket.create_connection((bridge, self.port), timeout=0.5).close()
                return
            except OSError:
                time.sleep(0.1)
        raise RuntimeError("the simulator did not start")

    def stop(self):
        # By the process this script started, never by a pattern.
        self.proc.terminate()
        try:
            self.proc.wait(timeout=5)
        except subprocess.TimeoutExpired:
            self.proc.kill()
            self.proc.wait()


def sfdk(target, cwd, args, timeout):
    p = subprocess.run(["sfdk", "-c", "target=" + target] + args, cwd=cwd, stdin=subprocess.DEVNULL,
                       stdout=subprocess.PIPE, stderr=subprocess.PIPE, timeout=timeout)
    return p.returncode, p.stdout.decode("utf-8", "replace"), p.stderr.decode("utf-8", "replace")


def ensure_old_build(a):
    """Host build of the old tree, from a temporary git worktree that is removed afterwards."""
    exe = os.path.join(a.old_build_dir, "daemon", "host", "lm-service")
    adapter = os.path.join(a.old_build_dir, "adapters", "libre", "host", "libre-adapter-host")
    if os.path.exists(exe) and os.path.exists(adapter):
        return True
    wt = os.path.join(os.path.dirname(a.old_build_dir), "wt-upgrade-old-%s" % a.old_commit[:7])
    subprocess.run(["git", "-C", REPO, "worktree", "add", "--detach", wt, a.old_commit], check=True,
                   stdout=subprocess.DEVNULL)
    try:
        os.makedirs(a.old_build_dir, exist_ok=True)
        if not os.path.isdir(os.path.join(a.old_build_dir, ".sfdk")):
            sfdk(a.target, a.old_build_dir, ["build-init"], 300)
        rc, out, err = sfdk(a.target, a.old_build_dir, ["build-shell", "qmake",
                            os.path.join(wt, "harbour-libre-monitor.pro"), "CONFIG+=hostbuild"], 600)
        if rc != 0:
            print("old qmake failed:", err[-400:])
            return False
        # Only the two programs the test runs (and the libraries they need).
        rc, out, err = sfdk(a.target, a.old_build_dir, ["build-shell", "make", "-j6", "sub-daemon-host",
                                                       "sub-adapters-libre-host"], 3600)
        if rc != 0:
            print("old build failed:", "\n".join(l for l in (out + err).splitlines() if "error" in l)[-800:])
            return False
    finally:
        subprocess.run(["git", "-C", REPO, "worktree", "remove", "--force", wt], stdout=subprocess.DEVNULL)
    return os.path.exists(exe) and os.path.exists(adapter)


def write_old_config(path, subject_id, ref):
    """The configuration file as the old provisioning mode wrote it (see the module docstring)."""
    root = {"format": 1, "connections": [{"adapter": "libre", "id": "c1",
                                          "subjects": [{"id": subject_id, "ref": ref}]}]}
    with open(path, "w", encoding="utf-8") as f:
        f.write(json.dumps(root, indent=4, sort_keys=True) + "\n")
    os.chmod(path, 0o600)


def write_alpha_config(path):
    """Adds to the file the keys that the alpha's interface writes (top level, connection, Subject),
    keeping the connection and the Subject as they are. Synthetic values only."""
    with open(path, encoding="utf-8") as f:
        root = json.load(f)
    root["language"] = "it"
    root["alerts_show_value"] = False
    con = root["connections"][0]
    con["label"] = "Account"
    sub = con["subjects"][0]
    sub.update({"label": "AB", "name": "Sim Patient", "unit": "mg/dL", "alert_persistence_low_s": 0,
                "alert_persistence_high_s": 900, "missing_limit_s": 900})
    with open(path, "w", encoding="utf-8") as f:
        f.write(json.dumps(root, indent=4, sort_keys=True) + "\n")
    os.chmod(path, 0o600)


def fingerprint(db_path):
    """Per table: the multiset of row hashes, and the schema versions. Read from a private copy (with
    the write-ahead log, if any) so the original files are never touched. Values are only hashed."""
    tmp = tempfile.mkdtemp(prefix="fp-")
    try:
        for suffix in ("", "-wal"):
            if os.path.exists(db_path + suffix):
                shutil.copy2(db_path + suffix, os.path.join(tmp, "db" + suffix))
        db = sqlite3.connect(os.path.join(tmp, "db"))
        tables = [r[0] for r in db.execute("SELECT name FROM sqlite_master WHERE type='table' ORDER BY name")]
        rows = {}
        for t in tables:
            cols = [r[1] for r in db.execute('PRAGMA table_info("%s")' % t)
                    if r[1] not in UPDATED_BY_DESIGN.get(t, set())]
            c = collections.Counter()
            for r in db.execute('SELECT %s FROM "%s"' % (", ".join('"%s"' % x for x in cols), t)):
                c[hashlib.sha256(repr(r).encode("utf-8")).hexdigest()] += 1
            rows[t] = c
        meta = {}
        if "meta" in tables:
            meta = {k: v for k, v in db.execute("SELECT key, value FROM meta")}
        db.close()
        return rows, meta
    finally:
        shutil.rmtree(tmp, ignore_errors=True)


def fingerprints(data):
    return {name: fingerprint(os.path.join(data, name)) for name in ("store.db", "journal.db")}


def kept(rep, label, before, after):
    """Every row present before is present after, unchanged. Prints counts only."""
    for name in ("store.db", "journal.db"):
        rows_b, meta_b = before[name]
        rows_a, meta_a = after[name]
        lost = {t: sum((rows_b[t] - rows_a.get(t, collections.Counter())).values()) for t in rows_b}
        counts = ", ".join("%s %d->%d" % (t, sum(rows_b[t].values()), sum(rows_a.get(t, {}).values()))
                           for t in sorted(rows_b) if sum(rows_b[t].values()) or t in rows_a)
        print("      %s %s: %s" % (label, name, counts))
        rep.check("%s: %s keeps every row unchanged" % (label, name), not any(lost.values()),
                  "rows missing or changed per table: %s" % {t: n for t, n in lost.items() if n})
        rep.check("%s: %s schema version unchanged" % (label, name), meta_b == meta_a,
                  "before %s after %s" % (meta_b, meta_a))


def sha(path):
    with open(path, "rb") as f:
        return hashlib.sha256(f.read()).hexdigest()


def run_phase(a, phase, build, work, data, url):
    script = os.path.join(HERE, "upgrade_session.sh")
    try:
        rc, out, err = sfdk(a.target, build, ["build-shell", "sh", script, phase, build, work, data, url], 900)
    except subprocess.TimeoutExpired:
        return {"end": []}, "", "(timed out)"
    return svc.sections(out), out, err


def tagged(lines, tag):
    """The JSON after `TAG ` on the last line that has it, or None."""
    found = None
    for l in lines:
        if l.startswith(tag + " "):
            found = l[len(tag) + 1:].strip()
    if found is None:
        return None
    m = re.search(r"\{.*\}", found)
    try:
        return json.loads(m.group(0)) if m else None
    except ValueError:
        return None


def err_lines(work, phase):
    """Lines of the service's standard error that report a failure (counted, the words only)."""
    path = os.path.join(work, "svc-%s.err" % phase)
    if not os.path.exists(path):
        return []
    with open(path, encoding="utf-8", errors="replace") as f:
        return [l.strip() for l in f if re.search(r"fail|error|invalid|corrupt|cannot|refus", l, re.I)]


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--old-commit", default="66728e4")
    ap.add_argument("--old-build-dir", default=os.path.expanduser("~/SFOS-DEV/harbour-libre-monitor-development/build/host-upgrade-old-i486"))
    ap.add_argument("--new-build-dir", default=os.path.expanduser("~/SFOS-DEV/harbour-libre-monitor-development/build/host-alpha6-i486"))
    ap.add_argument("--target", default="SailfishOS-5.1.0.11-i486")
    ap.add_argument("--bridge", default=None)
    ap.add_argument("--keep", action="store_true", help="keep the work directory")
    a = ap.parse_args()

    rep = base.Report()
    rep.check("old tree %s built from a git worktree" % a.old_commit, ensure_old_build(a))
    if rep.failed:
        return 1
    bridge = a.bridge or base.docker_bridge()
    os.makedirs(os.path.join(a.new_build_dir, "it"), exist_ok=True)
    work = tempfile.mkdtemp(prefix="upg-", dir=os.path.join(a.new_build_dir, "it"))
    sims = []
    try:
        cred = os.path.join(work, "cred")
        with open(cred, "w") as f:
            f.write("username=follower@example.invalid\npassword=%s\n" % base.PASSWORD)
        os.chmod(cred, 0o600)
        now = time.time()
        data = os.path.join(work, "data-old")
        os.makedirs(data)
        subject_id = "{%s}" % uuid.uuid4()
        config = os.path.join(data, "config.json")
        write_old_config(config, subject_id, svc.patient_ref(SEED, "a"))

        # ---- 3. fill with the old binary ------------------------------------------------------------
        for n in range(FILL_RUNS):
            sim = Sim(work, bridge, scenario(time.time(), now))
            sims.append(sim)
            sec, out, err = run_phase(a, "fill", a.old_build_dir, work, data, sim.url)
            sim.stop()
            f = sec.get("fill", [])
            rep.check("fill run %d: the old service started, polled and stopped" % (n + 1),
                      "START ok" in f and "POLLED ok" in f and "STOPPED ok" in f, "\n".join(f)[:400] + err[-300:])
        before = fingerprints(data)
        config_sha = sha(config)
        rec = sum(before["store.db"][0].get("record", {}).values())
        rep.check("fill: the old store holds the graph, the logbook and current readings (%d record rows)" % rec,
                  rec >= 100)

        up = os.path.join(work, "data-up")
        shutil.copytree(data, up)

        # ---- 4. upgrade: the new binary on a copy --------------------------------------------------------
        t = time.time()
        sim = Sim(work, bridge, scenario(t, now, excursion_at=t - 12 * 60))
        sims.append(sim)
        sec, out, err = run_phase(a, "upgrade", a.new_build_dir, work, up, sim.url)
        first = sec.get("upgrade first start", [])
        second = sec.get("upgrade second start", [])
        rep.check("upgrade: the new service starts and polls on the old data",
                  "START ok" in first and "POLLED ok" in first, "\n".join(first)[:400] + err[-300:])
        st = tagged(first, "STATUS") or {}
        subs = st.get("subjects", [])
        rep.check("upgrade: the Subject of the old configuration is followed",
                  len(subs) == 1 and subs[0].get("subject") == subject_id
                  and subs[0].get("state") not in ("disabled-by-guard", "credentials-missing"), str(len(subs)))
        rep.check("upgrade: an alert is raised from the readings", "ALERT ok" in first)
        al1 = tagged(first, "ALERTS") or {}
        th = (al1.get("thresholds") or [{}])[0]
        rep.check("upgrade: the thresholds in force are the standard ones (54, 70, 180, 250 mg/dL)",
                  (th.get("ll"), th.get("l"), th.get("h"), th.get("hh")) == (54, 70, 180, 250), str(th))
        lim = al1.get("limits", {})
        rep.check("upgrade: the per-Subject alert times are the defaults",
                  bool(lim) and all(th.get(k) == lim.get(k, {}).get("default")
                                    for k in ("persistence_low_s", "persistence_high_s", "missing_limit_s")),
                  str({k: th.get(k) for k in ("persistence_low_s", "persistence_high_s", "missing_limit_s")}))
        rep.check("upgrade: the first run stopped in order", "STOPPED ok" in first)
        rep.check("upgrade: a second restart starts and polls", "START ok" in second and "POLLED ok" in second,
                  "\n".join(second)[:400])
        al2 = tagged(second, "ALERTS") or {}

        def episodes(al):
            return sorted((x.get("slot"), x.get("episode")) for x in al.get("alerts", []))
        rep.check("upgrade: the second start restores the same alert episode",
                  bool(episodes(al1)) and episodes(al1) == episodes(al2),
                  "%d alert(s) before, %d after" % (len(episodes(al1)), len(episodes(al2))))
        starts = [json.loads(l) for l in sec.get("journal", []) if l.startswith("{")]
        causes = [x.get("cause") for x in starts if x.get("kind") == "start"]
        rep.check("upgrade: the journal reads back with the old entries and the new starts (%d starts)" % len(causes),
                  len(causes) == FILL_RUNS + 2, str(causes))
        rep.check("upgrade: no start after a missing stop (every stop was orderly)",
                  "after-missing-stop" not in causes, str(causes))
        rep.check("upgrade: the configuration file is unchanged, byte for byte", sha(os.path.join(up, "config.json")) == config_sha)
        bad = err_lines(work, "upgrade")
        rep.check("upgrade: the service reported no failure on standard error", not bad, "%d line(s): %s" % (len(bad), bad[:3]))
        after = fingerprints(up)
        kept(rep, "upgrade", before, after)
        sim.stop()

        # ---- 5. downgrade: the old binary on the upgraded data ------------------------------------------
        down = os.path.join(work, "data-down")
        shutil.copytree(up, down)
        # The alpha's user interface writes keys the old release never knew: the
        # old binary must meet them too, because a rollback without restoring the backup leaves them.
        write_alpha_config(os.path.join(down, "config.json"))
        sim = Sim(work, bridge, scenario(time.time(), now))
        sims.append(sim)
        sec, out, err = run_phase(a, "attempt", a.old_build_dir, work, down, sim.url)
        os.replace(os.path.join(work, "svc-attempt.err"), os.path.join(work, "svc-downgrade.err")) \
            if os.path.exists(os.path.join(work, "svc-attempt.err")) else None
        att = sec.get("attempt", [])
        down_ok = "START ok" in att and "POLLED ok" in att and "STOPPED ok" in att
        print("      DOWNGRADE: the old service %s on the upgraded data (%s)" %
              ("WORKS" if down_ok else "DOES NOT WORK", ", ".join(l for l in att if l.split(" ")[0] in
                                                                  ("START", "POLLED", "STOPPED"))))
        dst = tagged(att, "STATUS") or {}
        print("      DOWNGRADE: the old service follows %d Subject(s), state %s" %
              (len(dst.get("subjects", [])), ",".join(s.get("state", "?") for s in dst.get("subjects", []))))
        bad = err_lines(work, "downgrade")
        print("      DOWNGRADE: %d failure line(s) on standard error%s" % (len(bad), (": %s" % bad[:3]) if bad else ""))
        after_down = fingerprints(down)
        kept(rep, "downgrade", after, after_down)

        # ---- 6. upgrade again ------------------------------------------------------------------------------
        sec, out, err = run_phase(a, "attempt", a.new_build_dir, work, down, sim.url)
        sim.stop()
        att = sec.get("attempt", [])
        rep.check("upgrade again: the new service starts, polls and stops after the downgrade",
                  "START ok" in att and "POLLED ok" in att and "STOPPED ok" in att, "\n".join(att)[:400])
        kept(rep, "upgrade again", after_down, fingerprints(down))
    finally:
        for s in sims:
            if s.proc.poll() is None:
                s.stop()
        if not a.keep:
            shutil.rmtree(work, ignore_errors=True)
        else:
            print("work directory kept:", work)

    print("%d checks, %d failed" % (rep.count, len(rep.failed)))
    return 1 if rep.failed else 0


if __name__ == "__main__":
    sys.exit(main())
