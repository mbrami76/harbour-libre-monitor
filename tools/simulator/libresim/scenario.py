# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Michele Brami
"""Scenario files (REQ-SIM-03): loading, defaults and validation."""

import json
import os
import time
from datetime import datetime, timedelta, timezone

SCENARIO_DIR = os.path.join(os.path.dirname(os.path.dirname(os.path.abspath(__file__))), "scenarios")

# Scenario defaults, NOT requirements: the band limits are undecided.
DEFAULT_BANDS = {"ll": 54, "l": 70, "h": 180, "hh": 250}

DEFAULT_PROFILE = {
    "baseline": 110.0,
    "daily_amp": 8.0,
    "daily_peak_hour": 15.0,
    "meals": [{"hour": 7.5, "rise": 55.0}, {"hour": 13.0, "rise": 70.0}, {"hour": 19.5, "rise": 60.0}],
    "meal_jitter_min": 20.0,
    "meal_peak_min": 45.0,
    "night_dip": {"hour": 3.5, "depth": 25.0, "width_h": 1.5},
    "noise": 3.0,
    "drift_amp": 10.0,
    "drift_period_days": 5.0,
    "excursions": [],
}

REQUEST_FAULTS = {
    # kind: default endpoint
    "auth_missing": "*",      # captured: HTTP 400 {"message":"missing or malformed jwt"}
    "auth_invalid": "*",      # captured: HTTP 401 {"message":"invalid or expired jwt"}
    "error_in_200": "*",      # captured on graph and logbook: HTTP 200 with status != 0 and error
    "login_failed": "login",  # guess
    "terms": "login",         # guess
    "redirect": "login",      # guess
    "rate_limit": "*",        # guess
    "http_error": "*",        # guess (403, 5xx)
    "slow": "*",              # real-time delay
    "drop": "*",              # connection closed without a reply
    "invalid_json": "*",      # body cut in half
    "missing_keys": "*",      # listed paths removed from the reply
}
DATA_FAULTS = {"gap", "duplicates", "unordered", "graph_lag"}
ENDPOINTS = ("login", "connections", "graph", "logbook")


class ScenarioError(ValueError):
    pass


def parse_utc(text):
    try:
        dt = datetime.strptime(text, "%Y-%m-%dT%H:%M:%SZ")
    except (TypeError, ValueError):
        raise ScenarioError("not a UTC time like 2026-07-15T08:00:00Z: %r" % (text,))
    return int(dt.replace(tzinfo=timezone.utc).timestamp())


class Zone:
    """Local-time offset: an IANA zone name (summer and winter time) or a fixed offset."""

    def __init__(self, name=None, fixed_minutes=None):
        self.name = name
        self.fixed = fixed_minutes
        self._tz = None
        if name:
            try:
                from zoneinfo import ZoneInfo
                self._tz = ZoneInfo(name)
            except Exception as exc:  # missing tz database or unknown name
                raise ScenarioError("time zone %r not available: %s" % (name, exc))
        elif fixed_minutes is None:
            self.fixed = 0

    def offset_minutes(self, epoch):
        if self._tz is None:
            return self.fixed
        dt = datetime.fromtimestamp(epoch, timezone.utc).astimezone(self._tz)
        return int(dt.utcoffset().total_seconds() // 60)

    def standard_offset_minutes(self):
        """Offset without summer time; the simulated body rhythm follows it, so the
        series stays continuous when the local clock changes."""
        if self._tz is None:
            return self.fixed
        return self.offset_minutes(int(datetime(2026, 1, 15, 12, tzinfo=timezone.utc).timestamp()))


def _zone_of(raw):
    if raw.get("timezone"):
        return Zone(name=raw["timezone"])
    return Zone(fixed_minutes=int(raw.get("utc_offset_minutes", 0)))


class Account:
    def __init__(self, raw, default_zone):
        for key in ("email", "password", "subjects"):
            if key not in raw:
                raise ScenarioError("account lacks %r" % key)
        self.email = raw["email"]
        self.password = raw["password"]
        self.unit = raw.get("unit", "mg/dL")
        if self.unit not in ("mg/dL", "mmol/L"):
            raise ScenarioError("unit must be mg/dL or mmol/L, got %r" % (self.unit,))
        self.country = raw.get("country", "XX")
        self.subjects = list(raw["subjects"])
        self.zone = _zone_of(raw) if ("timezone" in raw or "utc_offset_minutes" in raw) else default_zone


class Scenario:
    def __init__(self, raw, source="<dict>"):
        self.source = source
        self.name = raw.get("name", "unnamed")
        self.seed = raw.get("seed", 1)
        start = raw.get("start_utc", "now")
        self.start_epoch = int(time.time()) if start == "now" else parse_utc(start)
        self.zone = _zone_of(raw)
        self.min_version = raw.get("min_version", "4.16.0")
        self.strict_headers = bool(raw.get("strict_headers", True))
        self.bands = dict(DEFAULT_BANDS)
        self.bands.update(raw.get("bands", {}))
        if not (self.bands["ll"] < self.bands["l"] < self.bands["h"] < self.bands["hh"]):
            raise ScenarioError("bands must satisfy ll < l < h < hh")
        self.logbook_min_age_h = float(raw.get("logbook_min_age_h", 24))
        self.logbook_window_days = float(raw.get("logbook_window_days", 14))

        subs = raw.get("subjects")
        if not isinstance(subs, list) or not subs:
            raise ScenarioError("scenario needs a non-empty 'subjects' list")
        self.subjects = {}
        for s in subs:
            key = s.get("key")
            if not key or key in self.subjects:
                raise ScenarioError("subject needs a unique 'key'")
            profile = dict(DEFAULT_PROFILE)
            profile.update(s.get("profile", {}))
            s = dict(s)
            s["profile"] = profile
            self.subjects[key] = s

        accts = raw.get("accounts")
        if not isinstance(accts, list) or not accts:
            raise ScenarioError("scenario needs a non-empty 'accounts' list")
        self.accounts = [Account(a, self.zone) for a in accts]
        emails = [a.email.lower() for a in self.accounts]
        if len(set(emails)) != len(emails):
            raise ScenarioError("duplicate account email")
        for a in self.accounts:
            for k in a.subjects:
                if k not in self.subjects:
                    raise ScenarioError("account %s follows unknown subject %r" % (a.email, k))

        self.faults = list(raw.get("faults", []))
        for f in self.faults:
            kind = f.get("kind")
            if kind not in REQUEST_FAULTS and kind not in DATA_FAULTS:
                raise ScenarioError("unknown fault kind %r" % (kind,))
            if "endpoint" in f and f["endpoint"] != "*" and f["endpoint"] not in ENDPOINTS:
                raise ScenarioError("unknown endpoint %r" % (f["endpoint"],))

    def account_by_email(self, email):
        for a in self.accounts:
            if a.email.lower() == str(email).lower():
                return a
        return None


def load_scenario(ref):
    """`ref` is a path or the name of a shipped scenario (without .json)."""
    path = ref
    if not os.path.exists(path):
        path = os.path.join(SCENARIO_DIR, ref + ".json")
    try:
        with open(path, "r", encoding="utf-8") as fh:
            raw = json.load(fh)
    except (OSError, ValueError) as exc:
        raise ScenarioError("cannot read scenario %s: %s" % (ref, exc))
    return Scenario(raw, source=path)
