# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Michele Brami
"""Deterministic data generation (REQ-SIM-02, REQ-SIM-03).

Every series is a pure function of (seed, Subject, simulated time). A later
request therefore repeats earlier points identically, and a faster simulated
clock only moves the time at which the function is evaluated: the series stays
continuous.
"""

import hashlib
import math
import random
import threading
from datetime import datetime, timedelta, timezone

from .scenario import parse_utc

MG_PER_MMOL = 18.0156
SENSOR_MIN = 40    # mg/dL; HI and LO are not simulated
SENSOR_MAX = 400
GRAPH_POINTS = 141          #: 141 points over about 11.7 h
GRAPH_STEP = 300            # seconds
CURRENT_STEP = 60           # seconds
GRAPH_LAG_MIN = 17          #: newest graph point 17 to 19 min older


def u01(seed, *key):
    """Deterministic uniform number in [0, 1) from a seed and a key."""
    text = "%s|%s" % (seed, "|".join(str(k) for k in key))
    return int.from_bytes(hashlib.sha256(text.encode("utf-8")).digest()[:8], "big") / 2.0 ** 64


def format_timestamp(epoch, offset_min):
    """`M/D/YYYY h:mm:ss AM|PM`: no padding, English marker."""
    dt = datetime(1970, 1, 1) + timedelta(seconds=int(epoch) + offset_min * 60)
    return "%d/%d/%d %d:%02d:%02d %s" % (dt.month, dt.day, dt.year, dt.hour % 12 or 12,
                                         dt.minute, dt.second, "AM" if dt.hour < 12 else "PM")


def trend_arrow(slope):
    """GUESS: 1..5 from the slope in mg/dL per minute. The meaning of the source's
    values is not verified; only that 1 to 5 occur."""
    if slope <= -2.0:
        return 1
    if slope <= -1.0:
        return 2
    if slope < 1.0:
        return 3
    if slope < 2.0:
        return 4
    return 5


class SubjectModel:
    def __init__(self, scen, key):
        cfg = scen.subjects[key]
        self.key = key
        self.cfg = cfg
        self.seed = scen.seed
        self.profile = cfg["profile"]
        self.bands = scen.bands
        self.phys_off = scen.zone.standard_offset_minutes()
        self.sec_off = int(u01(self.seed, key, "sec") * 3)   # graph points sit at 0 to 2 s
        self.min_age_s = scen.logbook_min_age_h * 3600
        self.window_s = scen.logbook_window_days * 86400
        self.faults = [f for f in scen.faults if f.get("subject") in (None, key)]

        sensor = cfg.get("sensor", {})
        self.act = None
        if sensor is not None:
            if "activated_utc" in sensor:
                self.act = parse_utc(sensor["activated_utc"])
            else:
                self.act = int(scen.start_epoch - float(sensor.get("age_days", 7.9)) * 86400)
            self.warm_s = int(sensor.get("warmup_min", 60)) * 60
            self.expiry = self.act + int(float(sensor.get("life_days", 14)) * 86400)  # GUESS
        self.gaps = [(parse_utc(f["start_utc"]), parse_utc(f["end_utc"]))
                     for f in self.faults if f["kind"] == "gap"]
        self._lb = []
        self._lb_base = scen.start_epoch - 90 * 86400
        self._lock = threading.Lock()

    # ---- physiology -------------------------------------------------------
    def mg(self, t):
        p = self.profile
        L = (t + self.phys_off * 60) / 60.0            # local minutes
        day = math.floor(L / 1440.0)
        h = (L - day * 1440.0) / 60.0
        v = p["baseline"] + p["daily_amp"] * math.cos(2 * math.pi * (h - p["daily_peak_hour"]) / 24.0)

        for d in (day - 1, day, day + 1):
            for i, meal in enumerate(p["meals"]):
                jit = (u01(self.seed, self.key, "meal", d, i) - 0.5) * 2 * p["meal_jitter_min"]
                x = (L - (d * 1440.0 + meal["hour"] * 60.0 + jit)) / p["meal_peak_min"]
                if 0 < x < 10:
                    v += meal["rise"] * x * math.exp(1 - x)

        nd = p["night_dip"]
        if nd:
            dh = (h - nd["hour"] + 12) % 24 - 12
            v -= nd["depth"] * math.exp(-(dh / nd["width_h"]) ** 2)

        v += p["drift_amp"] * math.sin(2 * math.pi * (t / 86400.0) / p["drift_period_days"])

        for ex in p["excursions"]:
            dur = float(ex["duration_min"])
            if "at_utc" in ex:
                starts = [parse_utc(ex["at_utc"]) / 60.0 + self.phys_off]
            else:
                hh, mm = ex["daily_local"].split(":")
                starts = [d * 1440.0 + int(hh) * 60 + int(mm) for d in (day - 1, day)]
            for s in starts:
                x = (L - s) / dur
                if 0 <= x <= 1:
                    w = math.sin(math.pi * x) ** 2
                    v += w * (ex["peak"] - v)

        k = math.floor(t / 300.0)
        f = t / 300.0 - k
        r0 = (u01(self.seed, self.key, "n", k) - 0.5) * 2
        r1 = (u01(self.seed, self.key, "n", k + 1) - 0.5) * 2
        v += p["noise"] * ((1 - f) * r0 + f * r1)
        v += 0.25 * p["noise"] * (u01(self.seed, self.key, "m", int(t // 60)) - 0.5) * 2
        return min(SENSOR_MAX, max(SENSOR_MIN, v))

    def mg_int(self, t):
        return int(round(self.mg(t)))

    def slope(self, t):
        return (self.mg(t) - self.mg(t - 900)) / 15.0

    # ---- sensor and availability -----------------------------------------
    def available(self, t):
        if self.act is None or t < self.act + self.warm_s or t > self.expiry:
            return False
        return not any(a <= t <= b for a, b in self.gaps)

    def sensor_state(self, now):
        if self.act is None:
            return "none"
        if now < self.act + self.warm_s:
            return "warmup"
        if now > self.expiry:
            return "expired"
        return "active"

    def _grid(self, t, step):
        return math.floor((t - self.sec_off) / step) * step + self.sec_off

    def current_sample(self, now):
        """Time of the newest sample of the current value (changes once a minute)."""
        if self.act is None or now < self.act + self.warm_s:
            return None
        t = self._grid(min(now, self.expiry), CURRENT_STEP)
        for i in range(1440):                       # signal loss: the last value stays
            if self.available(t - CURRENT_STEP * i):
                return t - CURRENT_STEP * i
        return None

    def graph_lag_min(self):
        for f in self.faults:
            if f["kind"] == "graph_lag":
                return float(f["minutes"])
        return GRAPH_LAG_MIN

    def graph_ref(self, now):
        """Upper limit for graph points: the current sample time minus the lag."""
        return self._grid(now, CURRENT_STEP) - self.graph_lag_min() * 60

    def graph_times(self, now):
        if self.act is None:
            return []
        newest = self._grid(self.graph_ref(now), GRAPH_STEP)
        times = [newest - GRAPH_STEP * i for i in range(GRAPH_POINTS - 1, -1, -1)]
        return [t for t in times if self.available(t)]

    # ---- logbook (GUESS: rule documented in the README) -------------------
    def _extend_events(self, limit):
        with self._lock:
            while not self._lb or self._lb[-1] <= limit:
                n = len(self._lb)
                rng = random.Random("%s|%s|lb|%d" % (self.seed, self.key, n))
                gap_min = min(3168.0, max(38.0, rng.lognormvariate(math.log(300.0), 1.0)))
                self._lb_base += gap_min * 60
                self._lb.append(int(self._lb_base // 60) * 60 + 44 + int(rng.random() * 9))

    def logbook_times(self, now):
        if self.act is None:
            return []
        top = self.graph_ref(now) - self.min_age_s
        self._extend_events(top)
        lo = now - self.window_s
        return [t for t in self._lb
                if lo <= t <= top and self.act + self.warm_s <= t <= self.expiry]


def make_reading(model, unit, t, offset_min, kind):
    """kind: 'current' (type 1), 'graph' (type 0) or 'logbook' (type 3)."""
    mg = model.mg_int(t)
    mmol = unit == "mmol/L"
    value = round(mg / MG_PER_MMOL, 1) if mmol else mg
    color = 4 if mg < model.bands["l"] else 3 if mg > model.bands["h"] else 1
    out = {"FactoryTimestamp": format_timestamp(t, 0), "Timestamp": format_timestamp(t, offset_min),
           "type": {"current": 1, "graph": 0, "logbook": 3}[kind]}
    if kind == "current":
        out["ValueInMgPerDl"] = mg
        out["Value"] = value
    else:
        out["Value"] = value
        out["ValueInMgPerDl"] = mg
    if kind != "graph":
        out["TrendArrow"] = trend_arrow(model.slope(t))
        out["TrendMessage"] = None
    out["MeasurementColor"] = color
    out["GlucoseUnits"] = 2 if mmol else 1
    out["isHigh"] = False      # captured: false in all 178 records
    out["isLow"] = False
    if kind == "logbook":
        out["alarmType"] = 2 if mg > model.bands["h"] else 1   # GUESS: no meaning implied
    return out
