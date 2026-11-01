# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Michele Brami
"""Tests of the simulator. Run: python3 -m unittest, in this folder."""

import glob
import hashlib
import http.client
import json
import os
import re
import unittest
from datetime import datetime, timezone

from libresim.scenario import SCENARIO_DIR, ScenarioError, load_scenario, parse_utc
from libresim.server import ManualClock, SimServer

HEADERS = {"product": "llu.android", "version": "4.16.0"}
TS_RE = re.compile(r"^(\d{1,2})/(\d{1,2})/(\d{4}) (\d{1,2}):(\d{2}):(\d{2}) (AM|PM)$")

CONNECTION_KEYS = ["id", "patientId", "country", "status", "firstName", "lastName", "targetLow", "targetHigh",
                   "uom", "sensor", "alarmRules", "glucoseMeasurement", "glucoseItem", "glucoseAlarm",
                   "patientDevice", "created"]
SENSOR_KEYS = ["deviceId", "sn", "a", "w", "pt", "s", "lj"]
DEVICE_KEYS = ["did", "dtid", "v", "l", "h", "alarms", "ll", "hl", "u", "fixedLowAlarmValues", "fixedLowThreshold"]
CURRENT_KEYS = ["FactoryTimestamp", "Timestamp", "type", "ValueInMgPerDl", "Value", "TrendArrow", "TrendMessage",
                "MeasurementColor", "GlucoseUnits", "isHigh", "isLow"]
GRAPH_KEYS = ["FactoryTimestamp", "Timestamp", "type", "Value", "ValueInMgPerDl", "MeasurementColor",
              "GlucoseUnits", "isHigh", "isLow"]
LOGBOOK_KEYS = ["FactoryTimestamp", "Timestamp", "type", "Value", "ValueInMgPerDl", "TrendArrow", "TrendMessage",
                "MeasurementColor", "GlucoseUnits", "isHigh", "isLow", "alarmType"]


def parse_ts(text):
    m = TS_RE.match(text)
    assert m, "timestamp does not match the documented format: %r" % text
    mo, d, y, h, mi, s, ap = m.groups()
    h = int(h) % 12 + (12 if ap == "PM" else 0)
    return int(datetime(int(y), int(mo), int(d), h, int(mi), int(s), tzinfo=timezone.utc).timestamp())


class Base(unittest.TestCase):
    scenario = "basic"
    start_override = None

    def setUp(self):
        self.scen = load_scenario(self.scenario)
        self.clock = ManualClock(self.start_override or self.scen.start_epoch)
        self.srv = SimServer(self.scen, self.clock).start()
        self.addCleanup(self.srv.stop)

    def request(self, method, path, headers=None, body=None, timeout=10):
        conn = http.client.HTTPConnection("127.0.0.1", self.srv.port, timeout=timeout)
        try:
            data = json.dumps(body) if body is not None else None
            conn.request(method, path, body=data, headers=dict(headers or {}))
            resp = conn.getresponse()
            raw = resp.read()
            return resp.status, dict((k.lower(), v) for k, v in resp.getheaders()), raw
        finally:
            conn.close()

    def login(self, email=None, password="sim-password"):
        email = email or self.scen.accounts[0].email
        status, _, raw = self.request("POST", "/llu/auth/login", HEADERS, {"email": email, "password": password})
        return status, json.loads(raw)

    def session(self, email=None):
        status, body = self.login(email)
        self.assertEqual(status, 200)
        uid = body["data"]["user"]["id"]
        return dict(HEADERS, authorization="Bearer " + body["data"]["authTicket"]["token"],
                    **{"account-id": hashlib.sha256(uid.encode()).hexdigest()}), body

    def get(self, path, headers):
        status, _, raw = self.request("GET", path, headers)
        return status, json.loads(raw)

    def connections(self, h):
        status, body = self.get("/llu/connections", h)
        self.assertEqual(status, 200)
        return body["data"]


class TestStructure(Base):
    def test_login_reply(self):
        h, body = self.session()
        self.assertEqual(body["status"], 0)
        self.assertEqual(list(body.keys()), ["status", "data"])           # no ticket key (5.3)
        for k in ("user", "messages", "notifications", "authTicket", "invitations", "trustedDeviceToken"):
            self.assertIn(k, body["data"])
        t = body["data"]["authTicket"]
        self.assertEqual(len(t["token"]), 460)
        self.assertEqual(t["duration"], 15552000000)
        self.assertEqual(t["expires"], int(self.clock.now()) + 15552000)
        self.assertEqual(body["data"]["user"]["uom"], "1")

    def test_connections(self):
        h, _ = self.session()
        status, body = self.get("/llu/connections", h)
        self.assertEqual((status, body["status"]), (200, 0))
        self.assertEqual(sorted(body["ticket"]), ["duration", "expires", "token"])
        self.assertEqual(body["ticket"]["token"], h["authorization"][7:])
        conn = body["data"][0]
        self.assertEqual(list(conn.keys()), CONNECTION_KEYS)
        self.assertEqual(list(conn["sensor"].keys()), SENSOR_KEYS)
        self.assertEqual(list(conn["patientDevice"].keys()), DEVICE_KEYS)
        self.assertEqual(list(conn["glucoseMeasurement"].keys()), CURRENT_KEYS)
        self.assertEqual(conn["glucoseMeasurement"], conn["glucoseItem"])
        self.assertEqual(conn["glucoseMeasurement"]["type"], 1)
        self.assertEqual(conn["patientDevice"]["fixedLowThreshold"], 0)
        self.assertIsNone(conn["glucoseAlarm"])
        for k in ("id", "patientId"):
            self.assertRegex(conn[k], r"^[0-9a-f]{8}-[0-9a-f]{4}-4[0-9a-f]{3}-[0-9a-f]{4}-[0-9a-f]{12}$")

    def test_graph(self):
        h, _ = self.session()
        conn = self.connections(h)[0]
        status, body = self.get("/llu/connections/%s/graph" % conn["patientId"], h)
        self.assertEqual((status, body["status"]), (200, 0))
        d = body["data"]
        self.assertEqual(list(d.keys()), ["connection", "activeSensors", "graphData"])
        self.assertEqual(d["connection"]["glucoseMeasurement"], conn["glucoseMeasurement"])
        self.assertEqual(list(d["activeSensors"][0].keys()), ["sensor", "device"])
        self.assertNotEqual(d["connection"]["patientDevice"]["fixedLowThreshold"], 0)
        pts = d["graphData"]
        self.assertEqual(len(pts), 141)
        for p in pts:
            self.assertEqual(list(p.keys()), GRAPH_KEYS)
            self.assertEqual(p["type"], 0)
            self.assertEqual(p["Value"], p["ValueInMgPerDl"])
            self.assertIs(p["isHigh"], False)
            self.assertIs(p["isLow"], False)
            self.assertIn(p["MeasurementColor"], (1, 3, 4))
        self.assertAlmostEqual((parse_ts(pts[-1]["FactoryTimestamp"]) - parse_ts(pts[0]["FactoryTimestamp"])) / 3600, 11.67, 2)

    def test_logbook(self):
        h, _ = self.session()
        conn = self.connections(h)[0]
        status, body = self.get("/llu/connections/%s/logbook" % conn["patientId"], h)
        self.assertEqual((status, body["status"]), (200, 0))
        entries = body["data"]
        self.assertGreater(len(entries), 10)
        times = []
        for e in entries:
            self.assertEqual(list(e.keys()), LOGBOOK_KEYS)
            self.assertEqual(e["type"], 3)
            self.assertIsInstance(e["Value"], int)
            self.assertIn(e["alarmType"], (1, 2))
            times.append(parse_ts(e["FactoryTimestamp"]))
        now = self.clock.now()
        self.assertTrue(all(now - 14 * 86400 <= t for t in times))               # rolling window
        gaps = [b - a for a, b in zip(times, times[1:])]
        self.assertGreaterEqual(min(gaps), 37 * 60)                              # not a sampled series
        self.assertGreater(max(gaps), 3 * 3600)
        self.assertGreater(len(set(t % 60 for t in times)), 3)                   # seconds off the grid
        self.assertTrue(all(43 <= t % 60 <= 53 for t in times))
        self.assertGreaterEqual(now - times[-1], 24 * 3600)                      # older than the graph

    def test_color_follows_target_range(self):
        h, _ = self.session()
        conn = self.connections(h)[0]
        _, body = self.get("/llu/connections/%s/graph" % conn["patientId"], h)
        lo, hi = conn["targetLow"], conn["targetHigh"]
        for p in body["data"]["graphData"]:
            v = p["ValueInMgPerDl"]
            self.assertEqual(p["MeasurementColor"], 4 if v < lo else 3 if v > hi else 1)


class TestTimestamps(Base):
    def test_format_and_offset_summer(self):
        h, _ = self.session()
        conn = self.connections(h)[0]
        _, body = self.get("/llu/connections/%s/graph" % conn["patientId"], h)
        for p in body["data"]["graphData"]:
            self.assertEqual(parse_ts(p["Timestamp"]) - parse_ts(p["FactoryTimestamp"]), 2 * 3600)  # CEST

    def test_format_is_unpadded_twelve_hour(self):
        h, _ = self.session()
        cur = self.connections(h)[0]["glucoseMeasurement"]
        self.assertTrue(cur["FactoryTimestamp"].startswith("7/15/2026 "))      # month first, no zero padding
        self.assertRegex(cur["FactoryTimestamp"], r"^7/15/2026 [0-9]{1,2}:[0-9]{2}:[0-9]{2} (AM|PM)$")

    def test_current_value_is_utc(self):
        h, _ = self.session()
        cur = self.connections(h)[0]["glucoseMeasurement"]
        age = self.clock.now() - parse_ts(cur["FactoryTimestamp"])
        self.assertTrue(0 <= age < 60)


class TestWinterAndSummerTime(Base):
    scenario = "multi"

    def test_offset_changes_during_graph_window(self):
        h, _ = self.session("follower1@example.invalid")
        conn = self.connections(h)[0]
        _, body = self.get("/llu/connections/%s/graph" % conn["patientId"], h)
        offs = {parse_ts(p["Timestamp"]) - parse_ts(p["FactoryTimestamp"]) for p in body["data"]["graphData"]}
        self.assertEqual(offs, {3600, 7200})        # CET to CEST on 2026-03-29

    def test_other_account_other_zone(self):
        h, _ = self.session("follower2@example.invalid")
        cur = self.connections(h)[0]["glucoseMeasurement"]
        self.assertEqual(parse_ts(cur["Timestamp"]) - parse_ts(cur["FactoryTimestamp"]), -4 * 3600)

    def test_two_accounts_several_subjects(self):
        h1, _ = self.session("follower1@example.invalid")
        h2, _ = self.session("follower2@example.invalid")
        c1, c2 = self.connections(h1), self.connections(h2)
        self.assertEqual(len(c1), 2)
        self.assertEqual(len(c2), 2)
        self.assertEqual(c1[1]["patientId"], c2[0]["patientId"])               # the same Subject, two followers
        # a Subject followed only by the other account is unknown here (captured: HTTP 200 with an error)
        other = c2[1]["patientId"]
        status, body = self.get("/llu/connections/%s/graph" % other, h1)
        self.assertEqual((status, body["status"]), (200, 4))


class TestMmol(Base):
    scenario = "mmol"

    def test_units(self):  # GUESS shape: no mmol/L reply was ever captured
        h, login = self.session()
        self.assertEqual(login["data"]["user"]["uom"], "2")
        conn = self.connections(h)[0]
        cur = conn["glucoseMeasurement"]
        self.assertEqual(cur["GlucoseUnits"], 2)
        self.assertIsInstance(cur["ValueInMgPerDl"], int)
        self.assertEqual(cur["Value"], round(cur["ValueInMgPerDl"] / 18.0156, 1))
        self.assertEqual(conn["uom"], 2)
        _, body = self.get("/llu/connections/%s/logbook" % conn["patientId"], h)
        self.assertTrue(all(e["GlucoseUnits"] == 2 and isinstance(e["Value"], float) for e in body["data"]))


class TestBands(Base):
    scenario = "bands"

    def test_all_limits_crossed_within_a_day(self):
        h, _ = self.session()
        conn = self.connections(h)[0]
        b = self.scen.bands
        seen = set()
        for step in range(24):
            self.clock.epoch = self.scen.start_epoch + step * 3600
            _, body = self.get("/llu/connections/%s/graph" % conn["patientId"], h)
            for p in body["data"]["graphData"]:
                v = p["ValueInMgPerDl"]
                seen.update(n for n, ok in (("ll", v < b["ll"]), ("l", v < b["l"]), ("h", v > b["h"]),
                                            ("hh", v > b["hh"])) if ok)
        self.assertEqual(seen, {"ll", "l", "h", "hh"})


class TestDeterminism(Base):
    def snapshot(self):
        h, _ = self.session()
        conn = self.connections(h)[0]
        out = [json.dumps(self.get("/llu/connections/%s/%s" % (conn["patientId"], ep), h)[1]["data"], sort_keys=True)
               for ep in ("graph", "logbook")]
        out.append(json.dumps(self.connections(h)[0]["glucoseMeasurement"]))
        return out

    def test_repeatable_in_one_run_and_across_runs(self):
        first = self.snapshot()
        self.assertEqual(first, self.snapshot())
        self.srv.stop()
        self.setUp()
        self.assertEqual(first, self.snapshot())                                 # same seed, new run

    def test_other_seed_differs(self):
        first = self.snapshot()
        self.srv.stop()
        self.scen = load_scenario("basic")
        self.scen.seed = 99
        self.srv = SimServer(self.scen, ManualClock(self.scen.start_epoch)).start()
        self.addCleanup(self.srv.stop)
        self.assertNotEqual(first[0], self.snapshot()[0])

    def test_later_request_repeats_earlier_points(self):
        h, _ = self.session()
        conn = self.connections(h)[0]
        path = "/llu/connections/%s/graph" % conn["patientId"]
        _, a = self.get(path, h)
        self.clock.epoch += 3 * 3600
        _, b = self.get(path, h)
        old = {p["FactoryTimestamp"]: p for p in a["data"]["graphData"]}
        new = {p["FactoryTimestamp"]: p for p in b["data"]["graphData"]}
        common = set(old) & set(new)
        self.assertGreater(len(common), 50)
        for k in common:
            self.assertEqual(old[k], new[k])

    def test_current_value_equals_graph_point_once_the_graph_catches_up(self):
        h, _ = self.session()
        conn = self.connections(h)[0]
        cur = conn["glucoseMeasurement"]
        self.clock.epoch += 30 * 60
        _, b = self.get("/llu/connections/%s/graph" % conn["patientId"], h)
        match = [p for p in b["data"]["graphData"] if p["FactoryTimestamp"] == cur["FactoryTimestamp"]]
        if match:     # only when the minute falls on the 5 minute grid
            self.assertEqual(match[0]["ValueInMgPerDl"], cur["ValueInMgPerDl"])


class TestCadence(Base):
    def cur(self, h):
        return self.connections(h)[0]["glucoseMeasurement"]["FactoryTimestamp"]

    def test_current_value_changes_once_a_minute(self):
        h, _ = self.session()
        stamps = []
        for i in range(0, 600, 15):                                              # 15 s polls, 10 minutes
            self.clock.epoch = self.scen.start_epoch + i
            stamps.append(parse_ts(self.cur(h)))
        distinct = sorted(set(stamps))
        self.assertTrue(11 <= len(distinct) <= 12)
        self.assertEqual({b - a for a, b in zip(distinct, distinct[1:])}, {60})

    def test_graph_five_minute_spacing_and_lag(self):
        h, _ = self.session()
        conn = self.connections(h)[0]
        for i in range(0, 1800, 97):
            self.clock.epoch = self.scen.start_epoch + i
            cur = parse_ts(self.cur(h))
            _, b = self.get("/llu/connections/%s/graph" % conn["patientId"], h)
            t = [parse_ts(p["FactoryTimestamp"]) for p in b["data"]["graphData"]]
            self.assertEqual({y - x for x, y in zip(t, t[1:])}, {300})
            lag_min = (cur - t[-1]) / 60.0
            self.assertTrue(17 <= lag_min < 22, lag_min)       # 17 to 19 observed twice; a 5 min grid gives up to 22

    def test_faster_clock_keeps_series_continuous(self):
        h, _ = self.session()
        conn = self.connections(h)[0]
        path = "/llu/connections/%s/graph" % conn["patientId"]
        values = {}
        for i in range(0, 6 * 3600, 600):                    # as if run at a high speed
            self.clock.epoch = self.scen.start_epoch + i
            for p in self.get(path, h)[1]["data"]["graphData"]:
                prev = values.setdefault(p["FactoryTimestamp"], p["ValueInMgPerDl"])
                self.assertEqual(prev, p["ValueInMgPerDl"])
        v = [values[k] for k in sorted(values, key=parse_ts)]
        self.assertLess(max(abs(b - a) for a, b in zip(v, v[1:])), 25)         # no jumps between neighbours


class TestRequestChecks(Base):
    def test_missing_and_malformed_token(self):
        h, _ = self.session()
        h2 = dict(h)
        del h2["authorization"]
        status, _, raw = self.request("GET", "/llu/connections", h2)
        self.assertEqual((status, json.loads(raw)), (400, {"message": "missing or malformed jwt"}))
        status, _, raw = self.request("GET", "/llu/connections", dict(h, authorization="garbage"))
        self.assertEqual(status, 400)

    def test_invalid_token(self):
        h, _ = self.session()
        status, _, raw = self.request("GET", "/llu/connections", dict(h, authorization="Bearer fake"))
        self.assertEqual((status, json.loads(raw)), (401, {"message": "invalid or expired jwt"}))

    def test_token_expires_with_the_simulated_clock(self):
        h, _ = self.session()
        self.clock.epoch += 15552000 + 1
        status, _, _ = self.request("GET", "/llu/connections", h)
        self.assertEqual(status, 401)

    def test_unknown_patient_is_http_200_with_error(self):  # captured
        h, _ = self.session()
        status, body = self.get("/llu/connections/00000000-0000-4000-8000-000000000000/graph", h)
        self.assertEqual((status, body), (200, {"status": 4, "error": {"message": "could not load patient"}}))
        status, body = self.get("/llu/connections/00000000-0000-4000-8000-000000000000/logbook", h)
        self.assertEqual((status, body), (200, {"status": 1, "error": {"message": "failed to retrieve measurements"}}))

    def test_headers(self):  # GUESS: 403 for wrong product, old version or wrong account-id
        h, _ = self.session()
        for bad in (dict(h, product="other"), dict(h, version="4.15.9"), dict(h, **{"account-id": "0" * 64})):
            status, _, _ = self.request("GET", "/llu/connections", bad)
            self.assertEqual(status, 403)
        no_version = {k: v for k, v in h.items() if k != "version"}
        self.assertEqual(self.request("GET", "/llu/connections", no_version)[0], 403)

    def test_login_failure_and_bad_request(self):
        status, body = self.login(password="wrong")
        self.assertEqual(status, 200)
        self.assertNotEqual(body["status"], 0)
        self.assertEqual(self.request("POST", "/llu/auth/login", HEADERS, None)[0], 400)

    def test_routes(self):
        self.assertEqual(self.request("GET", "/nope", HEADERS)[0], 404)
        self.assertEqual(self.request("GET", "/llu/auth/login", HEADERS)[0], 405)

    def test_token_is_synthetic_and_response_headers(self):
        _, body = self.login()
        self.assertTrue(body["data"]["authTicket"]["token"].startswith("SIMULATED."))
        _, hdrs, _ = self.request("POST", "/llu/auth/login", HEADERS, {"email": "follower@example.invalid", "password": "sim-password"})
        self.assertEqual(hdrs["cache-control"], "no-store")
        self.assertEqual(hdrs["x-simulated-data"], "1")


class TestFaults(Base):
    scenario = "faults"

    def sess(self, name):
        return self.session(name + "@example.invalid")[0]

    def test_terms_reply(self):
        status, body = self.login("terms@example.invalid")
        self.assertEqual(status, 200)
        self.assertNotEqual(body["status"], 0)
        self.assertNotIn("authTicket", body.get("data", {}))

    def test_redirect_once(self):
        _, body = self.login("redirect@example.invalid")
        self.assertEqual(body["data"], {"redirect": True, "region": "de"})
        _, body = self.login("redirect@example.invalid")
        self.assertIn("authTicket", body["data"])

    def test_auth_failures_on_data_endpoints(self):
        h = self.sess("expired")
        status, _, raw = self.request("GET", "/llu/connections", h)
        self.assertEqual((status, json.loads(raw)["message"]), (401, "invalid or expired jwt"))
        self.assertEqual(self.request("GET", "/llu/connections", h)[0], 200)      # count 1
        pid = self.connections(h)[0]["patientId"]
        self.assertEqual(self.request("GET", "/llu/connections/%s/graph" % pid, h)[0], 400)

    def test_rate_limit_with_retry_hint(self):
        h = self.sess("limited")
        self.assertEqual(self.request("GET", "/llu/connections", h)[0], 200)      # skip 1
        status, hdrs, _ = self.request("GET", "/llu/connections", h)
        self.assertEqual((status, hdrs["retry-after"]), (429, "30"))
        self.assertEqual(self.request("GET", "/llu/connections", h)[0], 429)
        self.assertEqual(self.request("GET", "/llu/connections", h)[0], 200)

    def test_5xx_403_and_error_in_200(self):
        h = self.sess("server")
        status, body = self.get("/llu/connections", h)
        self.assertEqual((status, body["status"]), (200, 4))
        pid = self.connections(h)[0]["patientId"]
        self.assertEqual(self.request("GET", "/llu/connections/%s/graph" % pid, h)[0], 503)
        self.assertEqual(self.request("GET", "/llu/connections/%s/graph" % pid, h)[0], 503)
        self.assertEqual(self.request("GET", "/llu/connections/%s/graph" % pid, h)[0], 200)
        self.assertEqual(self.request("GET", "/llu/connections/%s/logbook" % pid, h)[0], 403)

    def test_slow_reply(self):
        self.scen.faults[8]["delay_s"] = 0.4
        import time
        h = self.sess("slow")
        pid = self.connections(h)[0]["patientId"]
        t0 = time.time()
        self.assertEqual(self.request("GET", "/llu/connections/%s/graph" % pid, h)[0], 200)
        self.assertGreaterEqual(time.time() - t0, 0.4)

    def test_dropped_connection(self):
        h = self.sess("dropped")
        pid = self.connections(h)[0]["patientId"]
        with self.assertRaises((http.client.HTTPException, OSError)):
            self.request("GET", "/llu/connections/%s/logbook" % pid, h)
        self.assertEqual(self.request("GET", "/llu/connections/%s/logbook" % pid, h)[0], 200)

    def test_invalid_json_and_missing_keys(self):
        h = self.sess("broken")
        pid = self.connections(h)[0]["patientId"]
        # the first connections request passed (skip 1 is for the second one)
        status, _, raw = self.request("GET", "/llu/connections/%s/graph" % pid, h)
        self.assertEqual(status, 200)
        with self.assertRaises(ValueError):
            json.loads(raw)
        status, body = self.get("/llu/connections", h)
        self.assertNotIn("glucoseMeasurement", body["data"][0])

    def test_unaffected_account_is_clean(self):
        h = self.sess("ok")
        pid = self.connections(h)[0]["patientId"]
        self.assertEqual(self.get("/llu/connections/%s/graph" % pid, h)[1]["status"], 0)


class TestDataFaults(Base):
    scenario = "gaps"

    def graph(self, h, key):
        conns = {c["lastName"]: c for c in self.connections(h)}
        return conns, self.get("/llu/connections/%s/graph" % conns[key]["patientId"], h)[1]["data"]

    def test_signal_gap_leaves_holes_and_freezes_current_value(self):
        h, _ = self.session()
        conns, d = self.graph(h, "Signal gap")
        times = [parse_ts(p["FactoryTimestamp"]) for p in d["graphData"]]
        self.assertLess(len(times), 141)
        self.assertGreater(max(b - a for a, b in zip(times, times[1:])), 300)
        self.clock.epoch = parse_utc("2026-07-15T08:10:00Z")                     # inside the second gap
        cur = self.connections(h)[0]["glucoseMeasurement"]
        self.assertLessEqual(parse_ts(cur["FactoryTimestamp"]), parse_utc("2026-07-15T07:30:00Z"))

    def test_duplicates(self):
        h, _ = self.session()
        _, d = self.graph(h, "Duplicates")
        stamps = [p["FactoryTimestamp"] for p in d["graphData"]]
        self.assertGreater(len(stamps), len(set(stamps)))

    def test_delayed_graph(self):
        h, _ = self.session()
        conns, d = self.graph(h, "Delayed graph")
        cur = parse_ts(conns["Delayed graph"]["glucoseMeasurement"]["FactoryTimestamp"])
        self.assertGreaterEqual((cur - parse_ts(d["graphData"][-1]["FactoryTimestamp"])) / 60.0, 45)

    def test_out_of_order(self):
        h, _ = self.session()
        _, d = self.graph(h, "Out of order")
        t = [parse_ts(p["FactoryTimestamp"]) for p in d["graphData"]]
        self.assertNotEqual(t, sorted(t))
        self.assertEqual(len(t), 141)

    def test_warmup(self):
        h, _ = self.session()
        conns, d = self.graph(h, "Warm-up")           # GUESS: no readings during warm-up
        self.assertIsNone(conns["Warm-up"]["glucoseMeasurement"])
        self.assertEqual(d["graphData"], [])
        self.clock.epoch = parse_utc("2026-07-15T09:30:00Z")
        conns, d = self.graph(h, "Warm-up")
        self.assertIsNotNone(conns["Warm-up"]["glucoseMeasurement"])
        self.assertGreater(len(d["graphData"]), 0)
        self.assertGreaterEqual(parse_ts(d["graphData"][0]["FactoryTimestamp"]), parse_utc("2026-07-15T08:40:00Z"))

    def test_expiry(self):
        h, _ = self.session()
        conns, d = self.graph(h, "Expiring")
        self.assertEqual(len(d["activeSensors"]), 1)
        self.clock.epoch = parse_utc("2026-07-15T12:00:00Z")      # after the 14 days (GUESS)
        conns, d = self.graph(h, "Expiring")
        self.assertEqual(d["activeSensors"], [])
        expiry = conns["Expiring"]["sensor"]["a"] + 14 * 86400
        self.assertLessEqual(parse_ts(conns["Expiring"]["glucoseMeasurement"]["FactoryTimestamp"]), expiry)

    def test_subject_without_sensor(self):  # GUESS shape
        h, _ = self.session()
        conns, d = self.graph(h, "No sensor")
        c = conns["No sensor"]
        self.assertNotIn("sensor", c)
        self.assertIsNone(c["glucoseMeasurement"])
        self.assertEqual((d["graphData"], d["activeSensors"]), ([], []))
        pid = c["patientId"]
        self.assertEqual(self.get("/llu/connections/%s/logbook" % pid, h)[1]["data"], [])


class TestScenarios(unittest.TestCase):
    def test_every_shipped_scenario_serves_all_endpoints(self):
        names = [os.path.basename(p)[:-5] for p in glob.glob(os.path.join(SCENARIO_DIR, "*.json"))]
        self.assertEqual(set(names), {"basic", "bands", "multi", "mmol", "faults", "gaps"})
        for name in names:
            scen = load_scenario(name)
            srv = SimServer(scen, ManualClock(scen.start_epoch)).start()
            try:
                base = Base()
                base.srv, base.scen, base.clock = srv, scen, srv.sim.clock
                for acct in scen.accounts:
                    if name == "faults" and acct.email.split("@")[0] != "ok":
                        continue
                    h, _ = base.session(acct.email)
                    for conn in base.connections(h):
                        for ep in ("graph", "logbook"):
                            status, raw = base.request("GET", "/llu/connections/%s/%s" % (conn["patientId"], ep), h)[0:3:2]
                            self.assertEqual(status, 200, (name, ep))
            finally:
                srv.stop()

    def test_validation(self):
        for bad in ({"subjects": [], "accounts": []},
                    {"subjects": [{"key": "a"}], "accounts": [{"email": "x", "password": "y", "subjects": ["zz"]}]},
                    {"subjects": [{"key": "a"}], "accounts": [{"email": "x", "password": "y", "subjects": ["a"]}],
                     "faults": [{"kind": "nonsense"}]},
                    {"subjects": [{"key": "a"}], "accounts": [{"email": "x", "password": "y", "subjects": ["a"], "unit": "x"}]},
                    {"subjects": [{"key": "a"}], "accounts": [{"email": "x", "password": "y", "subjects": ["a"]}],
                     "bands": {"ll": 90}}):
            from libresim.scenario import Scenario
            with self.assertRaises(ScenarioError):
                Scenario(bad)

    def test_no_real_data_markers(self):
        for path in glob.glob(os.path.join(SCENARIO_DIR, "*.json")):
            with open(path) as fh:
                self.assertNotIn("libreview", fh.read().lower())


if __name__ == "__main__":
    unittest.main()
