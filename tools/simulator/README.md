<!-- SPDX-License-Identifier: GPL-3.0-or-later -->
<!-- Copyright (C) 2026 Michele Brami -->
# Simulated data source

A development tool (REQ-SIM-01 to 09): a small HTTP server that answers like the LibreLinkUp follower interface, so the adapter, the core and the CLI can be developed and tested without a real account. It **generates** synthetic data from scenario files; it never replays recordings (REQ-SIM-02). Python 3.9 or newer, standard library only; not part of the qmake build and of no package (REQ-SIM-05).

**All data is synthetic.** A pass against the simulator is no evidence about the real source. The structure follows; everything the capture does not show is marked *guess* below.

## Run

```bash
cd tools/simulator
python3 -m libresim --scenario basic --port 8080
python3 -m libresim --scenario multi --port 8080 --speed 60 --seed 7
python3 -m unittest
```

Options: `--scenario` (path or shipped name), `--port`, `--host` (default `127.0.0.1`; plain HTTP, do not expose it), `--speed` (simulated seconds per real second), `--seed` (overrides the scenario seed), `--verbose` (request lines only; no headers, bodies or credentials are logged). The start prints the accounts and their passwords, which are fake.

Point the adapter at it with the development-only base-URL override (REQ-SIM-06; flag `devsource`, not yet implemented outside the simulator). The `x-simulated-data: 1` header marks every reply.

## Shipped scenarios

| Scenario | Content |
|---|---|
| `basic` | One account (mg/dL, `Europe/Rome`, summer time), one Subject. |
| `bands` | One Subject with daily excursions crossing very low, low, high and very high (default limits 54, 70, 180, 250 mg/dL). |
| `multi` | Two accounts, three Subjects (one followed by both), start just after the EU switch to summer time so the graph holds both offsets; the second account uses `America/New_York`. |
| `mmol` | One account in mmol/L (**guess**). |
| `faults` | Nine accounts; each carries faults for one error class (see "Faults"). The account `ok` is clean. |
| `gaps` | Seven Subjects: signal gap, duplicate points, delayed graph, out-of-order points, warm-up, sensor about to expire, no sensor. |

## Scenario format (JSON)

```text
name, seed (int or string), start_utc ("2026-07-15T08:00:00Z" or "now"),
timezone (IANA name, summer and winter time) | utc_offset_minutes,
min_version ("4.16.0"), strict_headers (true), bands {ll, l, h, hh} (mg/dL),
logbook_min_age_h (24), logbook_window_days (14)
accounts[]: email, password, unit ("mg/dL" | "mmol/L"), country, subjects [keys],
            timezone | utc_offset_minutes (optional, per account)
subjects[]: key, first_name, last_name, sensor | null, profile, ...
  sensor:  age_days (at start) | activated_utc, warmup_min (60), life_days (14)
  profile: baseline, daily_amp, daily_peak_hour, meals [{hour, rise}], meal_jitter_min,
           meal_peak_min, night_dip {hour, depth, width_h}, noise, drift_amp,
           drift_period_days, excursions [{daily_local "HH:MM" | at_utc, duration_min, peak}]
faults[]:  kind, plus the parameters below
```

Unknown keys are ignored; unknown fault kinds and inconsistent bands are rejected. The **band limits are scenario defaults, not requirements**.

### Data generation

- Every series is a pure function of seed, Subject and simulated time. A later request repeats earlier points identically; a faster clock (`--speed`) only moves the time at which the function is evaluated, so the series stays continuous.
- The current value changes once a minute. Sample times carry 0 to 2 s (fixed per Subject). Graph points lie on a 5 minute grid, 141 points over 11.67 h; the newest is the newest grid point at least 17 min older than the current value. A 5 minute grid makes the age 17 to 22 min; the capture saw 17 and 19.
- Profile: baseline, daily rhythm, meals (a peak 45 min after a jittered meal time), a night dip, slow drift, noise, and excursions blended towards a given peak. The rhythm follows the zone's *standard* time, so it does not jump when summer time starts. Values are clamped to 40 to 400 mg/dL; **HI and LO are not simulated** (delivery unknown).
- Logbook rule (**guess**): event times are a fixed sequence from the seed, gaps log-normal with median 300 min clamped to 38 min to 52.8 h, seconds of the minute 44 to 52; the window is 14 days and ends `logbook_min_age_h` (24) before the newest graph point, as observed. `Value` is the Subject's generated value at that moment. `MeasurementColor` follows the target range (captured). `TrendArrow` is binned from the slope over 15 min and `alarmType` is 2 above the high limit and 1 otherwise; both are labelled guesses and imply no meaning.

### Faults

Request faults apply to requests that match `endpoint` (`login`, `connections`, `graph`, `logbook`, `*`), optionally `account` (e-mail). `skip` lets the first N matching requests through, `count` limits how many are affected (default: all). Counting is per fault and per server run, so runs are repeatable.

| `kind` | Parameters | Effect |
|---|---|---|
| `auth_missing` | | HTTP 400 `{"message":"missing or malformed jwt"}` |
| `auth_invalid` | | HTTP 401 `{"message":"invalid or expired jwt"}` |
| `error_in_200` | `status` (4), `message` | HTTP 200 with a non-zero `status` and an `error` object |
| `login_failed` | | HTTP 200, `status` 2, `error.message` `notAuthenticated` (guess) |
| `terms` | | HTTP 200, `status` 4, `data.step.type` `tou` (guess) |
| `redirect` | `region` ("de") | Login reply `data.redirect` true with `data.region` (guess) |
| `rate_limit` | `retry_after_s` (30) | HTTP 429 with a `Retry-After` header (guess) |
| `http_error` | `status` (503), `message` | Any HTTP status, e.g. 403 or 5xx (guess) |
| `slow` | `delay_s` (2) | Real-time delay, then the normal reply |
| `drop` | | Connection closed without a reply |
| `invalid_json` | | Body cut in half |
| `missing_keys` | `paths` (dotted, lists are walked) | Keys removed from the reply, e.g. `data.glucoseMeasurement` |

Data faults act on a Subject (`subject` key): `gap` (`start_utc`, `end_utc`: no points; the current value stays at the last one before the gap), `duplicates` (`every`), `unordered`, `graph_lag` (`minutes`). A Subject can have `"sensor": null`, a sensor still in warm-up, or one that expires.

## Limits (REQ-SIM-07)

"Captured" cites; "guess" is a choice of the simulator, not an observation. The simulator invents nothing else.

| Behaviour | Status | Source |
|---|---|---|
| Endpoints, methods, envelope `{status, data, ticket}`, ticket key in data replies, none in the login reply | captured | 2, 5.3 |
| Key names, order and types of the connection, sensor, device, reading, graph and logbook objects | captured | 4.1 to 4.4 |
| Timestamp format `M/D/YYYY h:mm:ss AM\|PM`; `FactoryTimestamp` UTC, `Timestamp` local with the zone's summer and winter offset | captured (one zone, one season) | 3 |
| Token of 460 characters, `expires` in Unix seconds, `duration` 15552000000, same token in all replies | captured | 2, 5.3 |
| Current value updates once a minute; graph spacing 5 min; 141 points over 11.7 h; graph lags the current value by 17 to 19 min | captured | 4.2, 5, 5.1 |
| `isHigh` and `isLow` always false; `Value` equals `ValueInMgPerDl` in mg/dL; `MeasurementColor` 4/3/1 by target range | captured | 5, 5.1 |
| Logbook: type 3 only, irregular, seconds 44 to 52, rolling 14 day window, nothing newer than the graph window | captured | 4.3, 5.1 |
| Error replies: 400 no token, 401 fake token, HTTP 200 with `status` 4 (graph) or 1 (logbook) for an unknown patient | captured | 5.2 |
| `fixedLowThreshold` 0 in connections and non-zero in the graph | captured | 4.1 |
| Reply headers `cache-control: no-store`, `lvapi` (value `v1.0.0-sim`); no gzip, no CDN headers | partly captured | 2 |
| Values of `status`, `pt`, `s`, `lj`, `dtid`, `alarmRules`, `fixedLowAlarmValues` | synthetic placeholders; only the keys are captured | 4.1 |
| Names, identifiers, serial numbers, tokens, glucose values | synthetic, never real | 1 |
| 403 for a wrong `product`, an old `version` or a wrong `account-id`; missing `version` | guess (403 only suspected) | (c) |
| Bad login credentials, malformed login body, 404, 405 | guess | |
| Terms reply, regional redirect reply, 429 with `Retry-After`, 5xx, other 4xx | guess (never provoked or observed) | 5.3, 6 |
| Token lifetime tied to the simulated clock; older tokens stay valid after a new login | guess | 5.3 |
| mmol/L accounts: `GlucoseUnits` 2, `Value` with one decimal, `uom` 2, `targetLow/High` converted | guess | |
| Sorting: graph ascending, logbook ascending, connections in account order | guess | |
| Logbook entry times, `TrendArrow` bins, `alarmType` rule, minimum age of 24 h | guess (rule above) | 5.1 |
| Sensor: warm-up with no readings, expiry after `life_days` (14) with no readings and no active sensor, Subject without sensor (`sensor` key absent, readings null) | guess | 6 |
| Signal gaps keep the last current value; duplicate, delayed and out-of-order points | guess | 6 |
| HI and LO readings, logbook types other than 3, non-null `glucoseAlarm`, diary details | not simulated | 6 |
| Rate limits of the real source, update rate at other times | unknown, not simulated | |
