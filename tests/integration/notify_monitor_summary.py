#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Michele Brami
"""Summary of a dbus-monitor recording of the notification server.

Reads the text that `dbus-monitor --session "interface='org.freedesktop.Notifications'"` wrote on the
emulator during tests/integration/run_emulator_alerts.py and prints, in order, every Notify call of the
application ("Libre Monitor"): the id it replaces and the id it got, the icon file, the summary, the
body's lines, and the hints that matter (urgency, x-nemo-feedback, x-nemo-display-on, x-nemo-priority,
category, x-nemo-user-removable), then every CloseNotification and NotificationClosed with its reason.
The match rule records calls and signals but not the method returns (they carry no interface), so the
id a Notify got prints as None; the ids are in the service's own notify.log of the same run.
With alerts_show_value off the texts hold no reading (only limits, times and minutes); the run uses a
synthetic patient. Python 3 standard library only.

    python3 tests/integration/notify_monitor_summary.py monitor.txt
"""
import re
import sys

HEAD = re.compile(r"^(method call|method return|signal|error) time=([0-9.]+) .*?(?:serial=(\d+))?(?: reply_serial=(\d+))?.*?(?:member=(\w+))?\s*$")
KEEP = ("urgency", "x-nemo-feedback", "x-nemo-display-on", "x-nemo-priority", "category", "x-nemo-user-removable")


def blocks(text):
    cur = None
    for line in text.split("\n"):
        if re.match(r"^(method call|method return|signal|error) ", line):
            if cur:
                yield cur
            cur = [line]
        elif cur is not None:
            cur.append(line)
    if cur:
        yield cur


def strings(body):
    return re.findall(r'string "(.*?)"(?=\n|$)', body, re.S)


def main(path):
    text = open(path, encoding="utf-8", errors="replace").read()
    calls, by_serial = [], {}
    for b in blocks(text):
        head, body = b[0], "\n".join(b[1:])
        t = re.search(r"time=([0-9.]+)", head)
        t = float(t.group(1)) if t else 0.0
        serial = re.search(r" serial=(\d+)", head)
        reply = re.search(r"reply_serial=(\d+)", head)
        member = re.search(r"member=(\w+)", head)
        if head.startswith("method call") and member and member.group(1) == "Notify":
            s = strings(body)
            if not s or s[0] != "Libre Monitor":
                continue
            replaces = re.search(r"uint32 (\d+)", body)
            hints = {}
            for k, v in re.findall(r'dict entry\(\s*string "([^"]+)"\s*variant\s+\w+ ("?[^\n"]*"?)', body):
                if k in KEEP:
                    hints[k] = v.strip('"')
            c = {"t": t, "kind": "notify", "replaces": replaces.group(1) if replaces else "?",
                 "icon": s[1].rsplit("/", 1)[-1] if len(s) > 1 else "", "summary": s[2] if len(s) > 2 else "",
                 "body": s[3] if len(s) > 3 else "", "hints": hints, "id": None}
            calls.append(c)
            if serial:
                by_serial[(head.split("sender=")[1].split()[0] if "sender=" in head else "", serial.group(1))] = c
        elif head.startswith("method return") and reply:
            idm = re.search(r"uint32 (\d+)", body)
            dest = head.split("destination=")[1].split()[0] if "destination=" in head else ""
            c = by_serial.get((dest, reply.group(1)))
            if c is not None and idm:
                c["id"] = idm.group(1)
        elif head.startswith("method call") and member and member.group(1) == "CloseNotification":
            idm = re.search(r"uint32 (\d+)", body)
            calls.append({"t": t, "kind": "close", "id": idm.group(1) if idm else "?"})
        elif head.startswith("signal") and member and member.group(1) == "NotificationClosed":
            u = re.findall(r"uint32 (\d+)", body)
            calls.append({"t": t, "kind": "closed", "id": u[0] if u else "?", "reason": u[1] if len(u) > 1 else "?"})
    t0 = calls[0]["t"] if calls else 0
    for c in calls:
        at = "+%6.0fs" % (c["t"] - t0)
        if c["kind"] == "notify":
            lines = c["body"].split("\n")
            print("%s NOTIFY id=%s replaces=%s icon=%s | %s | %s | %s" % (
                at, c["id"], c["replaces"], c["icon"], c["summary"], " / ".join(lines),
                " ".join("%s=%s" % (k, c["hints"][k]) for k in KEEP if k in c["hints"])))
        elif c["kind"] == "close":
            print("%s CLOSE  id=%s (by the service)" % (at, c["id"]))
        else:
            print("%s CLOSED id=%s reason=%s" % (at, c["id"], c["reason"]))
    n = [c for c in calls if c["kind"] == "notify"]
    print("Notify calls: %d; distinct summaries: %s" % (len(n), sorted(set(c["summary"] for c in n))))


if __name__ == "__main__":
    main(sys.argv[1])
