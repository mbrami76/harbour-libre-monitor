# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Michele Brami
"""HTTP server imitating the follower interface (REQ-SIM-01, REQ-SIM-05).

Plain HTTP on localhost, standard library only. Reply structure follows
; every other behaviour is a labelled
guess (README, "Limits"). All identifiers, names and tokens are synthetic.
"""

import base64
import hashlib
import json
import random
import re
import socket
import threading
import time
import uuid
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

from .gen import MG_PER_MMOL, SubjectModel, make_reading
from .scenario import REQUEST_FAULTS

TOKEN_LIFETIME_S = 15552000          # 180 days
TOKEN_LENGTH = 460
PRODUCT = "llu.android"
ROUTE_RE = re.compile(r"^/llu/connections/([^/]+)/(graph|logbook)$")


class Clock:
    """Simulated clock: starts at `start_epoch`, runs `speed` times faster than real time."""

    def __init__(self, start_epoch, speed=1.0, wall=time.time):
        self._start = start_epoch
        self._speed = float(speed)
        self._wall = wall
        self._wall0 = wall()

    def now(self):
        return self._start + (self._wall() - self._wall0) * self._speed


class ManualClock:
    """For tests: time moves only when told to."""

    def __init__(self, epoch):
        self.epoch = epoch

    def now(self):
        return self.epoch


class Reply:
    def __init__(self, status=200, body=None, headers=None, raw=None, drop=False):
        self.status = status
        self.body = body
        self.headers = headers or {}
        self.raw = raw
        self.drop = drop


def _uuid(*parts):
    digest = hashlib.sha256("|".join(str(p) for p in parts).encode()).digest()
    return str(uuid.UUID(bytes=digest[:16], version=4))


def _version_tuple(text):
    try:
        return tuple(int(p) for p in str(text).split("."))
    except ValueError:
        return None


def _remove_path(obj, parts):
    if not parts:
        return
    if isinstance(obj, list):
        for item in obj:
            _remove_path(item, parts)
    elif isinstance(obj, dict):
        if len(parts) == 1:
            obj.pop(parts[0], None)
        elif parts[0] in obj:
            _remove_path(obj[parts[0]], parts[1:])


class Simulator:
    """Request logic, independent of the HTTP plumbing."""

    def __init__(self, scenario, clock):
        self.scen = scenario
        self.clock = clock
        self.models = {k: SubjectModel(scenario, k) for k in scenario.subjects}
        self._tokens = {}                 # token -> (account, expires)
        self._logins = {}                 # email -> count
        self._seen = {}                   # fault index -> matching requests so far
        self._lock = threading.Lock()

    # ---- identifiers ------------------------------------------------------
    def user_id(self, acct):
        return _uuid(self.scen.seed, "user", acct.email)

    def account_hash(self, acct):
        return hashlib.sha256(self.user_id(acct).encode()).hexdigest()

    def patient_id(self, key):
        return _uuid(self.scen.seed, "patient", key)

    def _issue_token(self, acct, now):
        with self._lock:
            n = self._logins.get(acct.email, 0) + 1
            self._logins[acct.email] = n
        raw = b""
        block = ("%s|%s|%d" % (self.scen.seed, acct.email, n)).encode()
        while len(raw) < TOKEN_LENGTH:
            block = hashlib.sha256(block).digest()
            raw += block
        token = ("SIMULATED." + base64.urlsafe_b64encode(raw).decode().rstrip("="))[:TOKEN_LENGTH]
        expires = int(now) + TOKEN_LIFETIME_S
        self._tokens[token] = (acct, expires)
        return token, expires

    # ---- request handling -------------------------------------------------
    def handle(self, method, path, headers, body):
        """headers: dict with lower-case names. Returns a Reply."""
        now = self.clock.now()
        path = path.split("?", 1)[0]
        endpoint, patient = None, None
        if path == "/llu/auth/login":
            endpoint = "login"
        elif path == "/llu/connections":
            endpoint = "connections"
        else:
            m = ROUTE_RE.match(path)
            if m:
                endpoint, patient = m.group(2), m.group(1)
        if endpoint is None:
            return Reply(404, {"message": "not found"})                      # GUESS
        if (method == "POST") != (endpoint == "login"):
            return Reply(405, {"message": "method not allowed"})             # GUESS

        acct, ticket = None, None
        reply = self._header_check(headers)
        if reply is None and endpoint == "login":
            reply, acct = self._login(headers, body, now)
        elif reply is None:
            reply, acct, ticket = self._authenticate(headers, now)
            if reply is None:
                reply = self._data_reply(endpoint, patient, acct, ticket, now)
        return self._apply_faults(endpoint, acct, reply)

    def _header_check(self, headers):
        """GUESS: wrong or missing product/version -> 403 (: the 403 is only suspected)."""
        if not self.scen.strict_headers:
            return None
        if headers.get("product") != PRODUCT:
            return Reply(403, {"message": "product not allowed"})
        have, need = _version_tuple(headers.get("version")), _version_tuple(self.scen.min_version)
        if have is None or have < need:
            return Reply(403, {"message": "version not supported"})
        return None

    def _login(self, headers, body, now):
        try:
            req = json.loads(body.decode("utf-8"))
            email, password = req["email"], req["password"]
        except (ValueError, KeyError, TypeError, UnicodeDecodeError):
            return Reply(400, {"message": "malformed request"}), None         # GUESS
        acct = self.scen.account_by_email(email)
        if acct is None or acct.password != password:
            return self._login_failed(), None
        token, expires = self._issue_token(acct, now)
        data = {
            "user": {"id": self.user_id(acct), "firstName": "Sim", "lastName": "Follower",
                     "email": acct.email, "country": acct.country,
                     "uom": "2" if acct.unit == "mmol/L" else "1",
                     "accountType": "pat", "dateFormat": "2", "timeFormat": "2"},
            "messages": {"unread": 0},
            "notifications": {"unresolved": 0},
            "authTicket": {"token": token, "expires": expires, "duration": TOKEN_LIFETIME_S * 1000},
            "invitations": None,
            "trustedDeviceToken": "",
        }
        # captured (5.3): the login reply has status and data, but no ticket key
        return Reply(200, {"status": 0, "data": data}), acct

    @staticmethod
    def _login_failed():
        return Reply(200, {"status": 2, "error": {"message": "notAuthenticated"}})   # GUESS

    def _authenticate(self, headers, now):
        auth = headers.get("authorization", "")
        parts = auth.split(" ", 1)
        if len(parts) != 2 or parts[0].lower() != "bearer" or not parts[1].strip():
            return Reply(400, {"message": "missing or malformed jwt"}), None, None   # captured
        token = parts[1].strip()
        entry = self._tokens.get(token)
        if entry is None or entry[1] < now:
            return Reply(401, {"message": "invalid or expired jwt"}), None, None     # captured
        acct = entry[0]
        if headers.get("account-id") != self.account_hash(acct):
            return Reply(403, {"message": "account mismatch"}), None, None           # GUESS
        return None, acct, {"token": token, "expires": entry[1], "duration": TOKEN_LIFETIME_S * 1000}

    # ---- data replies -----------------------------------------------------
    def _data_reply(self, endpoint, patient, acct, ticket, now):
        if endpoint == "connections":
            data = [self._connection(acct, k, now, graph=False) for k in acct.subjects]
        else:
            key = next((k for k in acct.subjects if self.patient_id(k) == patient), None)
            if key is None:                                                      # captured (5.2)
                if endpoint == "graph":
                    return Reply(200, {"status": 4, "error": {"message": "could not load patient"}})
                return Reply(200, {"status": 1, "error": {"message": "failed to retrieve measurements"}})
            model = self.models[key]
            off = lambda t: acct.zone.offset_minutes(t)
            if endpoint == "graph":
                points = [make_reading(model, acct.unit, t, off(t), "graph") for t in model.graph_times(now)]
                points = self._shape_points(model, points)
                data = {"connection": self._connection(acct, key, now, graph=True),
                        "activeSensors": self._active_sensors(model, now),
                        "graphData": points}
            else:
                data = [make_reading(model, acct.unit, t, off(t), "logbook") for t in model.logbook_times(now)]
        return Reply(200, {"status": 0, "data": data, "ticket": ticket})

    def _shape_points(self, model, points):
        for f in model.faults:
            if f["kind"] == "duplicates":
                every = max(1, int(f.get("every", 10)))
                points = [p for i, q in enumerate(points) for p in ([q, dict(q)] if i % every == 0 else [q])]
            elif f["kind"] == "unordered":
                random.Random("%s|%s|unordered" % (self.scen.seed, model.key)).shuffle(points)
        return points

    def _sensor_obj(self, model):
        return {"deviceId": "", "sn": "SIM%s" % hashlib.sha256(model.key.encode()).hexdigest()[:7].upper(),
                "a": model.act, "w": model.warm_s // 60, "pt": 0, "s": False, "lj": False}

    def _device_obj(self, model, acct, graph):
        b = self.scen.bands
        return {"did": _uuid(self.scen.seed, "device", model.key), "dtid": 0, "v": "0.0.0",
                "l": True, "h": True, "alarms": True, "ll": b["l"], "hl": b["h"], "u": model.act,
                "fixedLowAlarmValues": {"mgdl": 60, "mmoll": round(60 / MG_PER_MMOL, 1)},
                "fixedLowThreshold": 60 if graph else 0}   # captured: 0 here, non-zero in graph

    def _active_sensors(self, model, now):
        if model.sensor_state(now) in ("none", "expired"):                      # GUESS
            return []
        return [{"sensor": self._sensor_obj(model), "device": self._device_obj(model, None, True)}]

    def _connection(self, acct, key, now, graph):
        model, b, mmol = self.models[key], self.scen.bands, acct.unit == "mmol/L"
        cfg = model.cfg
        cur_t = model.current_sample(now)
        current = None
        if cur_t is not None:
            current = make_reading(model, acct.unit, cur_t, acct.zone.offset_minutes(cur_t), "current")
        conv = lambda mg: round(mg / MG_PER_MMOL, 1) if mmol else mg
        rules = {"c": True,
                 "h": {"on": True, "th": b["h"], "thmm": round(b["h"] / MG_PER_MMOL, 1), "d": 0, "f": 1},
                 "f": {"th": 60, "thmm": round(60 / MG_PER_MMOL, 1), "d": 0, "tl": 0, "tlmm": 0},
                 "l": {"on": True, "th": b["l"], "thmm": round(b["l"] / MG_PER_MMOL, 1), "d": 0, "tl": 0, "tlmm": 0},
                 "nd": {"on": False, "i": 0, "r": 0, "l": 0}, "p": 0, "r": 0}
        out = {"id": _uuid(self.scen.seed, "connection", acct.email, key),
               "patientId": self.patient_id(key),
               "country": acct.country, "status": 2,
               "firstName": cfg.get("first_name", "Sim"), "lastName": cfg.get("last_name", "Subject " + key),
               "targetLow": conv(b["l"]), "targetHigh": conv(b["h"]),
               "uom": 2 if mmol else 1}
        if model.act is not None:
            out["sensor"] = self._sensor_obj(model)
        out["alarmRules"] = rules
        out["glucoseMeasurement"] = current
        out["glucoseItem"] = dict(current) if current else None
        out["glucoseAlarm"] = None
        out["patientDevice"] = self._device_obj(model, acct, graph) if model.act is not None else None
        out["created"] = (model.act or int(now)) - 86400
        return out

    # ---- faults (REQ-SIM-04) ----------------------------------------------
    def _matching_faults(self, endpoint, acct):
        out = []
        with self._lock:
            for i, f in enumerate(self.scen.faults):
                kind = f["kind"]
                if kind not in REQUEST_FAULTS:
                    continue
                if f.get("endpoint", REQUEST_FAULTS[kind]) not in ("*", endpoint):
                    continue
                if f.get("account") and (acct is None or f["account"].lower() != acct.email.lower()):
                    continue
                n = self._seen.get(i, 0)
                self._seen[i] = n + 1
                skip, count = f.get("skip", 0), f.get("count")
                if n >= skip and (count is None or n < skip + count):
                    out.append(f)
        return out

    def _apply_faults(self, endpoint, acct, reply):
        for f in self._matching_faults(endpoint, acct):
            kind = f["kind"]
            if kind == "slow":
                time.sleep(float(f.get("delay_s", 2)))
            elif kind == "drop":
                return Reply(drop=True)
            elif kind == "auth_missing":
                reply = Reply(400, {"message": "missing or malformed jwt"})
            elif kind == "auth_invalid":
                reply = Reply(401, {"message": "invalid or expired jwt"})
            elif kind == "error_in_200":
                reply = Reply(200, {"status": f.get("status", 4),
                                    "error": {"message": f.get("message", "could not load patient")}})
            elif kind == "login_failed":
                reply = self._login_failed()
            elif kind == "terms":
                reply = Reply(200, {"status": 4, "data": {"step": {"type": "tou", "componentName": "AcceptDocument",
                                                                   "props": {"reAcceptanceRequired": True}}}})
            elif kind == "redirect":
                reply = Reply(200, {"status": 0, "data": {"redirect": True, "region": f.get("region", "de")}})
            elif kind == "rate_limit":
                reply = Reply(429, {"message": "rate limited"},
                              headers={"Retry-After": str(int(f.get("retry_after_s", 30)))})
            elif kind == "http_error":
                reply = Reply(int(f.get("status", 503)), {"message": f.get("message", "simulated error")})
            elif kind == "invalid_json":
                text = json.dumps(reply.body if reply.body is not None else {}, separators=(",", ":"))
                reply = Reply(reply.status, raw=text[:max(1, len(text) // 2)].encode())
            elif kind == "missing_keys" and isinstance(reply.body, dict):
                for p in f.get("paths", ["data"]):
                    _remove_path(reply.body, p.split("."))
        return reply


class _Handler(BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.1"
    sim = None
    verbose = False

    def log_message(self, fmt, *args):
        if self.verbose:          # request line only: no headers, no body, no credentials
            super().log_message(fmt, *args)

    def _serve(self):
        length = int(self.headers.get("Content-Length") or 0)
        body = self.rfile.read(min(length, 65536)) if length else b""
        headers = {k.lower(): v for k, v in self.headers.items()}
        reply = self.sim.handle(self.command, self.path, headers, body)
        if reply.drop:
            self.close_connection = True
            try:
                self.connection.shutdown(socket.SHUT_RDWR)
            except OSError:
                pass
            return
        raw = reply.raw if reply.raw is not None else json.dumps(reply.body, separators=(",", ":")).encode()
        self.send_response(reply.status)
        self.send_header("Content-Type", "application/json; charset=utf-8")
        self.send_header("Content-Length", str(len(raw)))
        self.send_header("Cache-Control", "no-store")
        self.send_header("lvapi", "v1.0.0-sim")
        self.send_header("X-Simulated-Data", "1")
        for k, v in reply.headers.items():
            self.send_header(k, v)
        self.end_headers()
        self.wfile.write(raw)

    do_GET = do_POST = do_PUT = do_DELETE = _serve


class SimServer:
    def __init__(self, scenario, clock=None, host="127.0.0.1", port=0, verbose=False):
        self.sim = Simulator(scenario, clock or Clock(scenario.start_epoch))
        handler = type("Handler", (_Handler,), {"sim": self.sim, "verbose": verbose})
        self.httpd = ThreadingHTTPServer((host, port), handler)
        self.httpd.daemon_threads = True
        self._thread = None

    @property
    def port(self):
        return self.httpd.server_address[1]

    @property
    def url(self):
        return "http://%s:%d" % (self.httpd.server_address[0], self.port)

    def start(self):
        self._thread = threading.Thread(target=self.httpd.serve_forever, kwargs={"poll_interval": 0.05}, daemon=True)
        self._thread.start()
        return self

    def serve_forever(self):
        self.httpd.serve_forever()

    def stop(self):
        self.httpd.shutdown()
        self.httpd.server_close()
