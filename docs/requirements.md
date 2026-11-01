# Libre Monitor: requirements

These are the requirements of Libre Monitor, a Sailfish OS application for caregivers who follow the glucose of one or more people. Each requirement has a number (`REQ-<area>-<nn>`) that the source code and the other documents quote. The text states what the application does today. Items the application does not do yet are listed in section 13.

The column **Stage** says when a requirement applies: **V0** is the core library and the command-line program (a development tool, built only for the PC), **V1** is the application on the phone, **LATER** is not built.

---

## 1. Purpose and scope

### 1.1 Purpose
A community app for **caregivers** to monitor the glucose of one or more **Subjects** (the measured persons), using data obtained through the LibreLinkUp follower interface of Abbott's LibreView ecosystem.

### 1.2 Terms
| Term | Meaning |
|---|---|
| Caregiver | The person using the phone. Not the person measured. |
| Subject | The person whose data is recorded. Identified internally by an opaque ID. |
| Connection | A follower account used by an adapter to reach a source. One connection may see several Subjects. |
| Adapter | A separate process, started by the core, that talks to one external source and emits normalised records. In the Harbour-compliant packaging it is the application's own main binary started in adapter mode (REQ-ADP-15). |
| Core | The headless C++/Qt component: scheduler, ingest, store, alerts. |
| Record | A stored sample or event. |
| Guard | The core mechanism that limits and deactivates misbehaving adapters. |

### 1.3 In scope for V0 (core and command-line program)
Log in on behalf of a caregiver, discover Subjects, read the **current glucose value**, run as a headless core with a command-line entry point on Debian, and store records according to section 6.

### 1.4 In scope for V1 (first phone release)
The Sailfish service and user interface: the current value and the plot (12 h live, with zoom and scrolling back over what the store holds), monitoring in the background, alerts and thresholds, sensor tracking, the packages and the optional add-on. **LATER:** manual entries, other metrics (insulin, meals and carbohydrates, notes, weight, body fat, SpO2, heart rate), the full guard screen.


### 1.5 Out of scope
- Medical-device use. The app is a **supplementary monitor** and shall not be presented as safety-critical or as a basis for therapy decisions (REQ-SAF-01).
- Writing data back to any source.
- Cloud services operated by this project, telemetry, accounts on any server of ours.

---

## 2. Assumptions and constraints

**Assumptions.** The application depends on facts that its author cannot control. Where a fact was measured, the measurement is stated; everything else is an assumption and is tested wherever a test can detect its failure.

- There is no official public patient-data interface that the application could use. The follower interface it reads is **unofficial, undocumented and may change or be restricted without notice**. What is known of it comes from community documentation and from captures of the author's own account.
- The terms of use of the service may not permit third-party use of this interface. Anyone who installs the application must check this for themselves.
- A person must share their data with the caregiver's follower account from the official application. This is outside the control of this project.
- The sensor produces about one reading a minute. The history feed has a spacing of 5 minutes and lags the current value by about 19 minutes. The current value changes once a minute (measured over 10.5 minutes, 11 intervals of 56 to 65 s). The reading interval is therefore a configurable parameter (REQ-SCH-12), not a constant.
- The login may involve a regional redirect and a request to accept updated terms. The redirect is handled. The reply for the terms case has not been observed; until it is, the adapter reports it as `protocol-changed`.
- Timestamps come as `M/D/YYYY h:mm:ss AM/PM` without a zone. `FactoryTimestamp` is UTC and `Timestamp` is local time. The flags `isHigh` and `isLow` were false on every record, including values beyond the account's own alarm limits, and cannot be relied on. The token lasts about 180 days.
- Glucose values arrive in mg/dL or mmol/L depending on the person's country (REQ-DAT-13). Mg/dL was seen in every capture; mmol/L is expected from the source's unit code but not yet captured.
- Not known and to be measured: rate limits, the meaning of each trend-arrow value, error bodies, the reply when terms must be accepted, further logbook types, and whether the update rate of the current value holds at all times. A test against the simulator (REQ-SIM-07) is no evidence about any of these.
- The default limits of section 9.3 are quoted from the international consensus on time in range (Battelino et al., 2019). They should be checked against the source text before anyone relies on them.
- Platform behaviour verified on a device with Sailfish OS 5.2: a D-Bus call to the application name starts a headless, sandboxed instance without a window; background-activity wake-ups arrive while the phone is suspended and plain Qt timers do not; notifications persist after the process that posted them dies and after a phone restart, and can carry a D-Bus action; nothing restarts a dead instance, and nothing runs before the first PIN after a restart; after a restart the first access to the device-lock Secrets collection needs a one-time user approval. These may change with system updates.
- Jolla Harbour rules: only `/usr/bin/<name>` may be an executable binary; no other installed file may be executable except shared libraries; `.service` files and RPM scripts are not allowed. A user systemd timer can be installed only by a package, so it cannot be part of a Harbour-compliant package (REQ-PKG-03).

**Constraints**
- The core is C++/Qt, close to the platform. The user interface is QML/Silica.
- SI units only in documentation. Glucose is displayed in mmol/L or mg/dL by the user's choice (REQ-UI-02).
- Local-only operation. No data leaves the device except the calls to the data source made by adapters.

---

## 3. Architecture requirements

| ID | Requirement | Stage |
|---|---|---|
| REQ-ARC-01 | The system shall be layered: adapters, ingest core, store, scheduler/daemon, UI, alert engine. | V0 |
| REQ-ARC-02 | The UI and the daemon shall communicate only through the store plus a lightweight "new data" signal. | V1 |
| REQ-ARC-03 | Polling shall run in a process independent of the UI (REQ-ARC-07), so monitoring continues when the UI is closed. | V1 |
| REQ-ARC-04 | The first deliverable shall be a **headless core with a command-line entry point**, tested on Debian before the Sailfish service and UI exist. It logs in, discovers Subjects and prints the current value. The command-line program is built only in host builds (flag `hostbuild`) as a separate executable and is **not installed** in any package. The tests and the command-line program run in the SDK build engine on the Debian PC (Qt 5.6, the Qt of the target); a build with Debian's own Qt is not required. | V0 |
| REQ-ARC-05 | The core shall not depend on Python, Node or any other runtime on the phone. | V0 |
| REQ-ARC-06 | All protocol knowledge of the data source shall live in the adapter. Replacing the adapter (for example if Abbott offers an official route) shall not require changes to the core code, only to the adapter module and the adapter registry. This is the plan B. | V0 |
| REQ-ARC-07 | Monitoring shall run in a **headless service instance**: the application's own binary started in service mode by D-Bus activation (desktop key `ExecDBus`: bare binary name plus one argument word, as the validator requires). It runs sandboxed, has no window, and owns the D-Bus name `<OrganizationName>.<ApplicationName>`. Besides the UI instance it is the only process that opens the store and the secret store (adapter mode does not, REQ-ADP-15). | V1 |
| REQ-ARC-08 | The service shall take its time base from `BackgroundActivity` wake-ups (`libkeepalive`), never from plain Qt timers, which do not run reliably while the phone is suspended. The wake-up frequency is chosen from the supported set and shall be the lowest that meets REQ-SCH-02. Its battery cost is to be measured. | V1 |
| REQ-ARC-09 | At most one service instance shall run. An instance that cannot take the D-Bus name shall exit at once. | V1 |
| REQ-ARC-10 | On SIGTERM, SIGINT and SIGHUP the service shall write a stop record (REQ-SCH-10) and exit cleanly. | V1 |

### 3.1 Packaging
| ID | Requirement | Stage |
|---|---|---|
| REQ-PKG-01 | The main package shall comply with the Harbour rules (the `harbour` suite of `sfdk check` passes) and contain exactly one executable, `/usr/bin/<name>`. | V0 |
| REQ-PKG-02 | One source tree and one main binary shall serve all distributions. Variants differ only in packaging, not in code paths. The single exception is the development build (REQ-SIM-06), which is never distributed. | V0 |
| REQ-PKG-03 | An **optional add-on package**, distributed outside Harbour (for example on OpenRepos), shall contain no code and no scriptlets: a user systemd timer and a oneshot service that sends a D-Bus ping to the application name, installed in `/usr/lib/systemd/user/` and enabled by a symlink in `user-session.target.wants/`. It requires the main package. | V1 |
| REQ-PKG-04 | With the add-on the service starts about 20 s after the session starts (after the first PIN) and is revived after a crash within one timer period (60 s) when the phone is awake, up to about 4 min when asleep. These are measured values, not guarantees. | V1 |
| REQ-PKG-05 | The main package shall be fully usable without the add-on. Without it, monitoring starts when the user opens the application or taps the monitoring-state notice (REQ-ALR-06), and the gap records (REQ-SCH-10) make interruptions visible. | V1 |
| REQ-PKG-06 | The application shall detect whether the add-on is installed (presence of its timer unit file) and show this in settings with a short explanation. No other behaviour shall depend on it. | V1 |

### 3.2 Development tooling: simulated data source

| ID | Requirement | Stage |
|---|---|---|
| REQ-SIM-01 | A **simulated data source** shall exist for development: a program on the development PC that answers the adapter's requests in the format of the real source, so the CLI, the adapter and the core can be developed and tested without a real account. | V0 |
| REQ-SIM-02 | The simulator **generates** data, it does not replay recordings: trends, peaks and dips that cross the bands L, H, LL and HH, noise, gaps, duplicates, delayed or out-of-order readings, sensor warm-up and expiry, and every record kind found in the capture (the assumption in section 2 lists the logbook types seen). | V0 |
| REQ-SIM-03 | The simulator shall serve several accounts, each following several Subjects, each Subject with its own profile. Scenarios are files; a fixed seed makes a run repeatable; a simulated clock may run faster than real time. | V0 |
| REQ-SIM-04 | Fault scenarios shall cover each error class of REQ-ADP-10 and also slow replies, invalid JSON and a rate limit with a retry hint. | V0 |
| REQ-SIM-05 | The simulator is written in Python 3 using the standard library only, lives in `tools/simulator/`, is not part of the qmake build and is installed in no package. | V0 |
| REQ-SIM-06 | The override of the source's base URL (and plain HTTP) and the simulated-source mode exist **only in builds made with the development flag** `devsource` (qmake `CONFIG+=devsource`). Host builds (`hostbuild`: CLI, tests) imply it. A development package for the phone or the emulator is built with it. Release and Harbour packages are built without it: they contain neither the code nor a runtime switch. Credentials can be sent only to the default hosts unless the flag is set. | V0 |
| REQ-SIM-07 | The simulator reproduces only what has been captured and documented; unknown behaviour is not invented. Its limits are listed with the format description. | V0 |
| REQ-SIM-08 | Raw responses are captured once from the real source with the author's own client. They stay in a git-ignored folder and are never committed. What is committed is the written format description and synthetic data. | V0 |
| REQ-SIM-09 | A development package shall have its **own application identity** (package and desktop name, `ApplicationName`, data directory, secret collection), so it never touches the credentials or records of an installed release package. It shall show a permanent **SIMULATED DATA** indication on every screen, tag every simulated record with its own source tag, and keep simulated records in a store separate from real data. It is never distributed and never submitted to Harbour. | V0 |

---

## 4. Adapter requirements

### 4.1 Process model and transport
| ID | Requirement | Stage |
|---|---|---|
| REQ-ADP-01 | Each adapter shall run as a **separate operating-system process** started by the core. The adapter contract shall not depend on the implementation language of the adapter. | V0 |
| REQ-ADP-02 | Transport v0 shall be **standard input/output with JSON Lines** (one message per line). The protocol shall be defined independently of the transport so other bindings (for example D-Bus) can be added later. | V0 |
| REQ-ADP-03 | Adapters shall hold no durable state. A session token obtained by an adapter is returned to the core as an **opaque blob**, stored by the core in the secret store and passed back on the next `authenticate`. Restarting an adapter shall always be safe. | V0 |
| REQ-ADP-04 | The Libre adapter shall be written in C++/Qt. | V0 |
| REQ-ADP-05 | Only adapters that are compiled into the application and listed in a **built-in adapter registry** shall be accepted. Adapters are reviewed in-tree. Out-of-tree adapters are not supported (REQ-ADP-16). | V0 |
| REQ-ADP-15 | In the Harbour-compliant packaging, the adapter process shall be the application's **own main binary started in adapter mode** (a command-line mode that selects the adapter by identifier). No other executable shall be installed. In adapter mode the process shall not open the store or read the configuration file, and it shall receive credentials only inside the `authenticate` command (REQ-ADP-11). | V0 |
| REQ-ADP-16 | Adapters written in other languages, or shipped as separate executables or packages, are **deferred**. They are not supported in the Harbour-compliant package. Support in any other packaging is an open point. Contributors add adapters as source in the tree. | V0 |

### 4.2 Message model
- Every message carries: protocol version, message id, type, correlation id.
- Core to adapter: **commands**. Adapter to core: **replies**, **record batches**, **log lines**. Unsolicited messages are reserved for push-mode adapters (LATER).
- Unknown fields are ignored. Unknown message types are an error.

| ID | Requirement | Stage |
|---|---|---|
| REQ-ADP-06 | Session lifecycle: spawn, adapter `hello` (manifest, contract version, declared metrics and units), core admission checks, `accept` or `reject`, `configure` (non-secret settings only), commands, `shutdown`. | V0 |
| REQ-ADP-07 | Commands in contract v0: `authenticate`, `discover_subjects`, `fetch_current`, `fetch_range(from, to)`. | V0 |
| REQ-ADP-08 | Every command shall have a core-set **deadline**. On expiry the core kills the process and counts a transient error. | V0 |
| REQ-ADP-09 | Record batches shall carry unit, source timestamp as received, and source tag, and end with a **completion marker** with a watermark (newest timestamp covered). | V0 |
| REQ-ADP-10 | Errors shall carry exactly one class: `auth-failed`, `terms-must-be-accepted`, `rate-limited`, `transient`, `protocol-changed`, `invalid-command` (a defect of the core, not of the source); plus human-readable detail and an optional retry-after hint. The scheduler shall react only to the class. | V0 |
| REQ-ADP-11 | Credentials shall be passed to the adapter only inside the `authenticate` command, in memory. Never in command-line arguments or environment variables. | V0 |
| REQ-ADP-12 | Adapter stderr shall be captured for diagnostics only, never parsed as data, and scrubbed of secrets before logging. | V0 |
| REQ-ADP-13 | The default process policy shall be one long-lived process per connection while polling is active, ended after an idle timeout. The policy shall be changeable without changing adapters. | V0 |
| REQ-ADP-14 | Contract v0 is **frozen as is and labelled unstable**. It shall be revised after a working adapter and real captured data exist. | V0 |

### 4.3 Admission ("accepted as valid")
| ID | Requirement | Stage |
|---|---|---|
| REQ-ADM-01 | At load the core shall verify: built-in registry membership, manifest well-formedness, compatible contract version, declared metrics present in the registry, declared units accepted by the registry. | V0 |
| REQ-ADM-02 | At runtime the core shall validate every record: schema, unit, plausible range, timestamp not in the future beyond a clock-skew tolerance. Invalid records, and records whose metric, unit or feed the adapter did not declare, go to **quarantine**, unmodified, never silently coerced. Defaults: plausible glucose range 20 to 600 mg/dL (1.1 to 33.4 mmol/L), clock-skew tolerance 5 minutes; both are parameters, not alert limits. | V0 |
| REQ-ADM-03 | A burst of rejected records from one adapter shall escalate to the `protocol-changed` error class: when at least three records of one fetch were rejected and they are at least half of the records examined, the fetch completes with that class. For a feed with fewer records the rejections are counted and the scheduler escalates after consecutive polls with rejections (REQ-SCH-04). | V0 |
| REQ-ADM-04 | The adapter is untrusted input even when in-tree. | V0 |
| REQ-ADM-05 | A **conformance kit** (a fake core that drives an adapter through the lifecycle and checks replies against the contract) shall be provided. Passing it is the practical meaning of "valid". | V1 |

### 4.4 Guard: limits and deactivation
| ID | Requirement | Stage |
|---|---|---|
| REQ-GRD-01 | The core shall enforce: maximum line length (process killed if exceeded); messages and bytes per second (token bucket, allowing legitimate bursts such as a backfill); records per batch, per command and per rolling window; plausible record rate per metric per Subject (declared in the registry); stderr volume cap; spawn rate; OS limits on memory, CPU time and file descriptors where the platform allows. Default maximum line length 1 MiB (1048576 bytes, terminator excluded). | V0 |
| REQ-GRD-02 | The manifest may request budgets. The core applies its own ceilings and takes the lower value. A request above the ceiling is rejected at admission. | V0 |
| REQ-GRD-03 | Reaction ladder, never silent: backpressure; drop and count; quarantine; kill and restart with exponential backoff; circuit breaker. | V0 |
| REQ-GRD-04 | Violations shall be **soft** (budget overrun, implausible rate, stderr flood: low weight) or **hard** (line length, unknown message type, undeclared metric or unit, malformed framing: high weight). A **violation score with time decay** decides the trip. Weights and limits are documented, tunable per build, and documented. | V0 |
| REQ-GRD-05 | When the score crosses the limit, the connection shall enter the state **`disabled-by-guard`**. This state is distinct from `needs-attention` (which means the adapter behaved correctly but needs the caregiver, for example new credentials). | V0 |
| REQ-GRD-06 | `disabled-by-guard` shall be **persistent** across restarts of the core and the phone. The core shall never leave it automatically. | V0 |
| REQ-GRD-07 | The trip applies **per connection**. If the same adapter version trips on two or more connections, the whole adapter version shall be disabled. | V0 |
| REQ-GRD-08 | Re-enabling shall be **manual only**. In V1: a well visible button on the main page of the app, not hidden in a menu, next to the notice of the disabled connection; pressing it re-enables the connection; a comment may be added but is not required. Later: a screen showing which limits were hit, when and how often. In both cases the acknowledgement shall be written to the ingest log. | V1 (button), LATER (screen) |
| REQ-GRD-09 | A new adapter version shall go through admission again, but the disabled notice remains visible until a human confirms. | V1 |
| REQ-GRD-10 | Records ingested during an offending session shall remain in the store, tagged by session ID in the ingest log, and may later be marked as ignored (REQ-DAT-07). | V0 |
| REQ-GRD-11 | Every violation shall be written to the ingest log with adapter identity and version. | V0 |
| REQ-GRD-12 | Documentation shall state that the guard defends against faults and resource abuse, **not against hostile code** (a malicious adapter can exfiltrate credentials with a single request). In the Harbour-compliant packaging adapters are part of the application binary and run under the same user and sandbox as the core, so the process boundary limits faults, not privileges. | V0 |
| REQ-GRD-13 | When the guard ends the adapter process in the middle of a batch, **none of the records of that batch is delivered** to the store (the adapter is not trusted for that batch); records already quarantined stay in quarantine. The batch, exactly as received, is held in memory only, per connection, and never written to disk without the user's action. | V0 |
| REQ-GRD-14 | The notice of a connection stopped by a guard kill shall offer **Retry**, a well visible button on the main page of the app, not hidden in a menu, in the same place and manner as the re-enable button (REQ-GRD-08). A successful retry discards the held batch. | V1 |
| REQ-GRD-15 | After a **second consecutive guard kill** of the same connection, the app shall propose to the user to prepare a **diagnostic report for the developer**. Only with the user's consent it writes an inspectable plain-text file (JSON Lines) to the phone's documents or downloads folder and shows its path and the developer's e-mail address, read from the packaging metadata. The app sends nothing; the user reads the file and sends it. Content: the application version, the adapter id and version, the backend version, the violation kinds with their details, the command type, the counts, and the held batch **anonymized**: no names, no Subject or account identifiers, no source references, no sensor id, no session or token; absolute times replaced by offsets from the first time of the batch; glucose values and units kept, because the problem cannot be understood without them (the user decides whether to send). | V1 |

---

## 5. Source connection (Libre adapter)

| ID | Requirement | Stage |
|---|---|---|
| REQ-SRC-01 | Log in with the follower account's credentials and obtain a session. | V0 |
| REQ-SRC-02 | Handle a regional redirect: store the correct regional host per connection. | V0 |
| REQ-SRC-03 | If updated terms must be accepted, surface this to the caregiver. The app shall never accept terms automatically. | V0 |
| REQ-SRC-04 | Discover the Subjects visible to the follower account and let the caregiver select one or more. | V0 |
| REQ-SRC-05 | Read the current glucose value of a selected Subject, with its own sensor timestamp. | V0 |
| REQ-SRC-06 | Fetch the graph data for the last 12 h on first connection and after any gap; merge by (Subject, type, timestamp, source). | V1 |
| REQ-SRC-07 | Collect sensor information (REQ-SEN-*). | V1 |
| REQ-SRC-08 | Logbook entries shall be stored as events of the registry type `logbook_entry`, carrying the raw entry, and shall not be interpreted, plotted or alerted on until their meaning is established. | V0 |

---

## 6. Data requirements

### 6.1 Record model
Two record shapes only:
- **Sample**: a numeric value at a point in time (glucose, heart rate, SpO₂, weight, body fat).
- **Event**: a discrete occurrence with a small structured payload (insulin dose, meal with carbohydrates, note), optionally with a duration.

The metric and event registry declares per type: unit(s) accepted, allowed payload fields, plausible range, maximum sampling rate. The registry is designed in full now; **only glucose is implemented in V1**. Glucose defaults: plausible range 20 to 600 mg/dL (1.1 to 33.4 mmol/L), maximum 2 samples per minute per Subject and feed. Each kind also declares its compaction rule (REQ-STO-12).

### 6.2 Invariants
| ID | Requirement | Stage |
|---|---|---|
| REQ-DAT-01 | Every record shall carry its **unit**, its **timestamp** and a **source tag**. The unit is the one reported by the source, stored with the value, never inferred. Conversion happens only at display. | V0 |
| REQ-DAT-02 | Each record shall carry two times: measurement time (UTC) and ingest time. The original timestamp string as received shall also be kept. | V0 |
| REQ-DAT-03 | Each record shall have an **origin**: `adapter`, `manual` or `derived`. Derived records (for example a computed sensor change) are recomputable, carry the rule version, and are never confused with source data. | V0 |
| REQ-DAT-04 | Adapter data shall **never be modified or deleted** (append-only). The only exception is the compaction of section 6.5 (REQ-STO-05, REQ-STO-06), which runs only after the user's confirmation (REQ-STO-05), deletes permanently and is itself recorded. | V0 |
| REQ-DAT-05 | Adapter data shall **never overwrite manual data**, and vice versa. | V0 |
| REQ-DAT-06 | Idempotency: unique key on (Subject, type, timestamp, source). Re-fetching overlapping windows shall be harmless. Manual records additionally include a generated entry ID, since two manual notes can share a timestamp. | V0 |
| REQ-DAT-07 | A record may be marked **ignored** through an overlay `mark` record: references the target, mandatory comment, time, optional author. Un-ignoring is a new mark that supersedes the old one. All consumers (plot, statistics, alerts) read through a view that excludes ignored records. A raw view shall remain available. | V0 |
| REQ-DAT-08 | Invalid records go to **quarantine**, unmodified. This includes records whose metric, unit or feed was not declared by the adapter. | V0 |
| REQ-DAT-09 | Conflicting duplicates (same timestamp, different value, for example the `current` feed versus the `graph` feed): the feed is part of the source tag, so both coexist, both stored raw and kept in full. There is **no precedence rule** and no merged view: a view selects the feed by its source tag, the user chooses what the screen shows, and alerts and the latest value use the `current` feed only. Measured: the two feeds differ at the same timestamp. | V0 |
| REQ-DAT-10 | Manual entry of any data type shall be possible, including data an adapter also provides. Manual data shall be marked as such. Display rules when both exist are not defined yet. | LATER |
| REQ-DAT-11 | The adapter shall validate the format of every received timestamp against the documented format and convert it to UTC, taking the measurement time from `FactoryTimestamp`. The original string is kept as audit (REQ-DAT-02). A timestamp that does not match is never guessed: the record goes to quarantine (REQ-DAT-08) and a `protocol-changed` error is counted (REQ-ADP-10). | V0 |
| REQ-DAT-12 | Display conversion between mg/dL and mmol/L shall use the factor 18.0156 mg/dL per mmol/L (molar mass of glucose 180.156 g/mol). It shall always start from the stored value, never from an already rounded or converted one, so no chain of conversions accumulates error. The result shall be rounded to the native resolution of the target unit, declared in the registry (mg/dL: whole numbers; mmol/L: one decimal, unverified until an mmol/L capture). Conversion shall never be stored, and thresholds, bands and alerts shall never be compared on rounded values. A unit test shall check both directions across the plausible range. | V0 |
| REQ-DAT-13 | The value of a reading shall be stored exactly as the source reports it, with its unit in a separate field. The unit shall be determined, in this order: (1) from the source's unit code through a table of verified codes, provided it agrees with the relation of the source's two values; (2) if the code is unknown or provisional, from the relation between the source's value and its mg/dL value (equal means mg/dL; times 18.0156 equals means mmol/L, within rounding); (3) only if the mg/dL value is missing, from magnitude using the sensor's reading range of 40 to 400 mg/dL (2.2 to 22.2 mmol/L; Abbott support page on HI and LO): 40 or more is mg/dL, 22.3 or less is mmol/L. A reading whose unit stays undetermined (including the gap between 22.3 and 40) shall not be stored as a sample but quarantined. The unit depends on the Subject's country and not on a setting the user can choose, so the app shall accept either unit from any account. | V0 |

### 6.3 Subject identity
| ID | Requirement | Stage |
|---|---|---|
| REQ-SUB-01 | The store shall identify a Subject only by an **opaque ID** (UUID). The store shall never contain a name. | V0 |
| REQ-SUB-02 | A **configuration file** maps ID to name, and ID to the source's own patient identifier per connection. The ID is created once, never reused, never renamed. A rename changes only the configuration. | V0 |
| REQ-SUB-03 | The configuration file is the re-identification key. Its permissions shall be restricted. The application exports nothing. The store is pseudonymous, not anonymous. | V0 |
| REQ-SUB-04 | Deleting a Subject shall delete the store rows by ID **and** the configuration entry. Delete per Subject shall be supported (the application exports nothing; the purge of older data per Subject is REQ-STO-17). | V1 |
| REQ-SUB-05 | When a Subject is selected, the app shall show the unit the adapter determined for that Subject, with how it was determined (code, relation or magnitude), and ask the user to confirm or correct it. The confirmed unit is kept in the configuration, not in the store. Monitoring and alerts shall not wait for the confirmation. If later readings arrive in a different unit, they shall be stored with their own unit (REQ-DAT-13), nothing shall be converted silently, and the user shall be notified once and asked to confirm the new unit. | V1 |

### 6.4 Store contents
Collections: `subject` (ID only), `record`, `mark`, `sensor`, `quarantine`, ingest log. Names, credentials and thresholds live **outside** the store (thresholds as append-only history, see REQ-ALR-05).

### 6.5 Storage management and compaction

Phones are replaced after 3 to 5 years, so the store is bounded by compaction (REQ-STO-05 to REQ-STO-07), which the user confirms and which deletes permanently, not by silent deletion; the application neither exports nor imports data. Estimate, not measured: about 50 to 100 MB a year per Subject for samples alone.

| ID | Requirement | Stage |
|---|---|---|
| REQ-STO-01 | The store module shall check the free disk space **once a week** (not more often) while the service runs, and record the result in the service journal as a `storage-check` entry. | V0 |
| REQ-STO-02 | If the free space is **smaller than 3 times the current size of the store** (all collections, the ingest log and the journal), the app shall notify the user in the app and point to compaction (REQ-STO-13) with its estimate. The notice repeats at each weekly check while the condition holds. | V1 |
| REQ-STO-05 | **Compaction deletes permanently.** Before it runs, the app shall show the estimate (REQ-STO-13) and say, in plain words, that the removed samples cannot be restored by the application (the vendor's own service may still hold the readings, for as long as it keeps them) and ask the user to confirm. **Only after this confirmation** shall compaction run. | V0 |
| REQ-STO-06 | **Compaction** applies to samples older than the **full-resolution window (default 3 calendar months, a parameter)** and removes derived records and the quarantine and ingest-log entries older than the same window. **Marks, events and sensor records stay on the phone in full.** Samples are thinned by REQ-STO-07. Each run appends a compaction record (time range, counts removed, rule version) that is never removed. | V0 |
| REQ-STO-07 | **Sample thinning rule:** time is divided into 5-minute slots aligned to UTC (hh:00, hh:05 and so on). Only samples of the **`current` feed** (about one per minute) older than the full-resolution window are thinned: from the samples of a Subject, kind and unit in a slot, only the sample **nearest to the start of the slot** is kept (ties: the earlier one). A slot with a single sample keeps it. **The `graph` (history) feed is never thinned**, and neither are logbook entries, marks, events and sensor records. Kept samples are unchanged adapter records, not derived values. | V0 |
| REQ-STO-10 | **The store schema shall stay consistent across major versions:** every change to the data model that raises the major version shall ship a migration step (applied step by step, never in one jump) and a test that opens a fixture store of each earlier major version. | V0 |
| REQ-STO-13 | The user shall be able to change the full-resolution window in the app (default 3 calendar months). **Data newer than 14 days shall never be thinned:** the window cannot be set below 14 days. The app shall show an **estimate of the space that compaction would free** for the chosen window, computed by a dry run over the store (records that would be removed times their average size) and labelled as an estimate. The dry run changes nothing. The estimate is shown before the confirmation of REQ-STO-05 and in the notice of REQ-STO-02. | V0 |
| REQ-STO-12 | Measurements other than glucose (heart rate, SpO2, weight and so on) use the same mechanism. The registry declares per kind whether the thinning rule applies, which feeds it thins (glucose: the `current` feed only) and any different slot length; unknown kinds are never thinned. | V0 |
| REQ-STO-15 | **Store maintenance shall not interrupt monitoring.** While compaction runs (it can block writes to the store for tens of seconds, and longer with several Subjects), the service shall keep polling the sources and evaluating alerts. Readings fetched meanwhile shall be held in a bounded queue in memory and written when the store is free again; alerts shall not depend on the store being writable. The queue and the alert state shall not lose a reading silently: if the queue would overflow, the maintenance run stops cleanly (its rollback guarantees apply) and the user is told. The queue has a high-water mark (about 80%) at which the scheduler asks the service to stop the maintenance run, so that a batch is dropped only as a last resort and logged as `readings-lost`. The UI shall say that maintenance is running. | V1 |
| REQ-STO-16 | **Compaction shall check its space before deleting anything.** The database file is rebuilt after the deletion and the rebuild needs free space of about the size of what remains. Before it starts, compaction shall verify that this space is available and, if not, refuse without changing the store and tell the user what would help (a shorter full-resolution window frees more, a longer one frees less, or more free space). The dry-run estimate of REQ-STO-13 shall include this need. | V0 |
| REQ-STO-17 | **Purge.** The user can start, by hand and for **one Subject at a time**, an irreversible purge of **everything of that Subject older than a chosen age**: 3, 6, 12 or 24 months. It removes the Subject's readings of every feed, derived records, quarantine and ingest-log entries, marks, events, logbook entries, sensor records and alert records held in the store tables; it does not remove the Subject itself or its configuration, nor anything newer than the chosen age (so never anything inside the 14-day floor). It also removes, from the service journal, that Subject's **alert, threshold and silent-mode entries** older than the chosen age; every other journal entry (service events: heartbeat, start, stop, connection state, storage check) stays, and the purge **leaves no record**. Before it runs the app shows an **estimate of the space it can recover** (labelled as an estimate, computed by a dry run that changes nothing, with the rebuild need of REQ-STO-16) and asks for **confirmation**, naming the Subject by its label and the age, then for the **typed label** of the Subject and a final button (the phone's own security authentication is not used). The same space check as REQ-STO-16 applies before anything is deleted; monitoring is not interrupted (REQ-STO-15). | V1 |

---

## 7. Sensor requirements

| ID | Requirement | Stage |
|---|---|---|
| REQ-SEN-01 | A sensor entity per Subject: identifier as reported by the adapter (a hash of the serial number), activation time, model if available, first and last reading seen. | V1 |
| REQ-SEN-02 | A sensor replacement shall be detected when the reported sensor identifier changes between consecutive readings, and stored as a `derived` event carrying the rule version. The first sensor ever seen is not a replacement. | V1 |
| REQ-SEN-03 | A documented warm-up duration (nominally about 1 h, to be verified) shall apply after a replacement. Missing-data alerts shall be suppressed during it. | V1 |
| REQ-SEN-04 | Expected sensor lifetime is a documented constant of ours. An "expiring soon" notice may be added later. | LATER |

---

## 8. Scheduling and session requirements

| ID | Requirement | Stage |
|---|---|---|
| REQ-SCH-01 | One schedule and state per (connection, Subject): `idle → fetching → backoff → needs-attention`, plus `disabled-by-guard`. | V0 |
| REQ-SCH-02 | The next run of the `current` feed shall derive from the sensor timestamp of the last reading plus the nominal interval, not a fixed timer; the other feeds run at their own interval counted from the end of the previous poll. | V0 |
| REQ-SCH-03 | A reading of the `current` feed shall be stored only if its timestamp is newer than the last stored one; fetches of ranges rely on the store's key rule (same key and value: nothing is written). | V0 |
| REQ-SCH-04 | Transient errors: exponential backoff with jitter. Auth error: one re-login, then `needs-attention`. Rate limit: stop that connection for a long period. Protocol change: stop that connection and raise a visible warning; a run of consecutive polls in which records were rejected counts as one (the number is a parameter). | V0 |
| REQ-SCH-05 | Minimum intervals between `authenticate` and `fetch` commands per connection shall be enforced to avoid account lockout. | V0 |
| REQ-SCH-06 | A per-Subject **staleness clock**, independent of errors, shall track `last_success_at`, the measurement time of the newest stored `current` record (a successful `graph` or `logbook` fetch does not move it), and record the cause of staleness. | V0 |
| REQ-SCH-07 | Staleness causes: (1) pipeline cannot reach the source; (2) source has no new readings; (3) sensor in warm-up; (4) source disabled by guard; (5) monitoring service not running (gap, REQ-SCH-10); (6) credentials locked, waiting for the user's approval (REQ-SEC-05); (7) monitoring stopped and user action needed (credentials refused, terms, source changed, internal defect), with the detail. A rate limit is cause (1) with the detail `rate-limited`. | V0 |
| REQ-SCH-08 | The session manager shall hold, per connection: token, expiry, regional endpoint. Credentials live in the secret store (REQ-SEC-01). | V0 |
| REQ-SCH-09 | The service shall append a **heartbeat** to a service journal at least once per nominal sensor interval while it runs. A heartbeat carries wall time and the boot-time and monotonic counters. The journal is append-only and separate from the measurement store, except for the pruning of REQ-SCH-14. | V1 |
| REQ-SCH-10 | The service journal shall also hold a **start record** (with the cause it can determine: first start, after a stop record, after a missing stop record, after a phone restart) and a **stop record**. A **gap** is the interval from the last heartbeat or stop record to the next start record. Gaps are shown in the history as "monitoring not running" and count as missing data for REQ-ALR-02. | V1 |
| REQ-SCH-11 | The difference between the boot-time and monotonic counters shall be used to tell time spent in suspend (monitoring continues through keepalive wake-ups) from time the service was not running. | V1 |
| REQ-SCH-12 | The nominal reading interval used by REQ-SCH-02 shall be a **configurable parameter** per connection and feed, with a default taken from measured data: 60 s for the current value, 300 s for the history feed. | V0 |
| REQ-SCH-13 | The scheduler shall be able to **adapt its request rate to limits imposed by the source**: the minimum interval per connection is a parameter, raised by the rate-limit reaction of REQ-SCH-04 and by a retry hint from the adapter (REQ-ADP-10). The mechanism exists from V1; the actual limits are measured later. | V0 |
| REQ-SCH-14 | Heartbeats older than one calendar month shall be removed from the service journal at the weekly storage check (REQ-STO-01), **except the last heartbeat of each run** (a run lasts from a start record to the next start or stop record), so that every gap keeps its start (REQ-SCH-10). Start and stop records are never removed. Each pruning that removed something appends a `prune` entry (time range, number removed) to the journal. No export is needed first: what is lost is only the suspend detail of REQ-SCH-11 for periods older than a month, which is diagnostic. | V1 |

Detailed state machines are the subject of separate documents (section 13).

---

## 9. Alert requirements

### 9.1 Alert types
Hyperglycaemia, hypoglycaemia and **missing data**.

| ID | Requirement | Stage |
|---|---|---|
| REQ-ALR-01 | Each (Subject, alert type) shall have the states `normal → pending → active → acknowledged → cleared`, plus snooze. Thresholds support persistence time and hysteresis. | V1 |
| REQ-ALR-02 | The missing-data alert shall fire regardless of the guard state, so a deactivated source never hides the blind spot. The message shall state whether no data is being collected and the cause (REQ-SCH-07). | V1 |
| REQ-ALR-03 | Missing-data alerts shall be suppressed during sensor warm-up. | V1 |
| REQ-ALR-04 | Alerts shall be delivered as system notifications by the daemon even when the UI is closed. | V1 |
| REQ-ALR-05 | Threshold changes shall be **append-only** (time, boundary, new and previous value; no comment) so "what were the thresholds when this alert fired?" is always answerable. | V1 |
| REQ-ALR-06 | The service shall keep one **resident monitoring-state notification** ("monitoring active, last heartbeat HH:MM") updated at its wake-ups, with a D-Bus action that starts the service when tapped. The notification survives a dead service and a phone restart, so a stale time reveals a stop. It cannot raise an alarm by itself: nothing runs after the service dies. The notification also lists the active and the acknowledged alerts, in one line of its body, with the label and the level word only. | V1 |
| REQ-ALR-07 | At start, if a gap longer than a configurable limit is found, the service shall raise a notification "monitoring was interrupted from X to Y", styled as a non-glucose state (REQ-UI-06). | V1 |
| REQ-ALR-08 | If the credentials are locked (REQ-SEC-05) the service shall request the approval itself, keep a visible "waiting for approval" state and log the gap. | V1 |
| REQ-ALR-09 | **Silent mode.** Each Subject can be put in a silent mode for 1 hour, 4 hours, until tomorrow, or without limit; a timed mode ends by itself. In silent mode every alert of that Subject, **LL and HH included**, is still raised, shown and journaled, and only its sound and vibration are withheld. The mode is held by the service, is journaled (start, end, no Subject name) and survives a restart of the service and of the phone. The notices about the monitor itself (service stopped, no data, interruption, waiting for approval) are not covered by a Subject's mode. | V1 |
| REQ-ALR-10 | The text of every alert notification and notice, the banner summary and body and the page summary and body alike, is translated: the service loads the same translation files as the UI and uses the application's language (written by the UI into the configuration file, default the phone's language), English being the source (REQ-UI-13). The **banner summary is as short as the language allows** (a design guide of about 25 characters, not a tested limit), carries no label and no application name, and says by itself what it is about; the body (2 to 3 lines) starts with the Subject's label and names no application either. | V1 |

### 9.2 Glucose bands and indicators
Five bands: **LL, L, N, H, HH**, shown with colour **and** an icon. Four boundaries per Subject.

### 9.3 Threshold rules
Default boundaries from the international consensus on CGM time-in-range (Battelino et al., 2019). **The numbers in mg/dL are the thresholds; the mmol/L figures are displays**. To be verified against the source text.

| Boundary | Default | Editable by caregiver |
|---|---|---|
| LL (urgent low) | below 54 mg/dL (3.0 mmol/L) | **No, fixed** |
| L (low) | below 70 mg/dL (3.9 mmol/L) | Yes |
| H (high) | above 180 mg/dL (10.0 mmol/L) | Yes |
| HH (urgent high) | above 250 mg/dL (13.9 mmol/L) | **No, fixed** |

| ID | Requirement | Stage |
|---|---|---|
| REQ-THR-01 | Only L and H shall be user-editable, per Subject. LL and HH shall be fixed at the global values and shall **always raise an alert** that no setting can delay or snooze; the only thing that can withhold its sound and vibration is the visible silent mode of REQ-ALR-09. | V1 |
| REQ-THR-02 | If a user change makes L or H looser than the global value, the app shall warn at the time of the change and require a **challenge**: the caregiver types the new value again, the screen does not show the value just asked for, and it must match; the service enforces it. There is no comment field. Nothing is forbidden; the deviation is made explicit and recorded. | V1 |
| REQ-THR-03 | The global defaults are defaults, not universal truths: the consensus gives different targets for some groups (for example pregnancy, older or high-risk people). This shall be stated in the documentation. | V1 |
| REQ-THR-04 | The thresholds page shows every number with its unit (the Subject's own, with the unit's name next to the field and in the messages) and explains every refusal in plain words, naming what was violated and with which numbers: not confirmed, the value typed again does not match (which of L or H), the order broken with the allowed range, an impossible number, no change. | V1 |

---

## 10. UI requirements

| ID | Requirement | Stage |
|---|---|---|
| REQ-UI-01 | The user interface shows the current value, the trend and the age of the reading, and the 12 h plot drawn from the store, with horizontal zoom and scrolling back in time over what the store holds; the vertical scale is fixed; the plot turns grey when it is not at the current time, and a tap on the value reading box of the Subject page returns the plot to the current reading and resets the zoom. | V1 |
| REQ-UI-02 | The caregiver selects mmol/L or mg/dL at **first run**, and may change it later in settings. | V1 |
| REQ-UI-03 | **Colour is reserved exclusively for glucose levels.** Glucose state is also carried by the **band badge**, a round badge whose triangle glyph tells the band (three pointing down, one down, two facing each other, one up, three up), and by trend/status indicators. The application icon is the identity of the application and is outside this rule. | V1 |
| REQ-UI-04 | Every non-glucose state (including `disabled-by-guard`, `needs-attention`, staleness) shall be carried by **pictogram plus explicit text**, neutral foreground styling, never by colour alone. Its pictogram shall not be readable as LL, L, N, H or HH. | V1 |
| REQ-UI-05 | **Stale data shall lose its glucose colour.** When the staleness clock passes its limit, the value is shown neutral with its age and a staleness icon. Colour means "current reading in this band". | V1 |
| REQ-UI-06 | The guard state is shown in a separate status strip, not dismissible until acknowledged. It raises a system notification with its own sound and vibration pattern, distinct from glucose alerts. | V1 |
| REQ-UI-07 | The core stores only the semantic state. Presentation (colour, icon) is decided by the UI. | V0 |
| REQ-UI-08 | Contrast shall hold in light and dark Sailfish ambiences. | V1 |
| REQ-UI-09 | The UI shall never display an old value as if it were current. | V1 |
| REQ-UI-10 | A monitoring-state strip shall show whether the service is running, stopped, interrupted (from, to) or waiting for approval, by pictogram plus text (REQ-UI-04). | V1 |
| REQ-UI-11 | Opening the UI shall start the service if it is not running (D-Bus call) and show its state. | V1 |
| REQ-UI-12 | Settings shall show whether the optional add-on is installed and what restarts monitoring in each case (REQ-PKG-06). | V1 |
| REQ-UI-13 | The user interface is written in **English**, with an **Italian translation**. All user-visible text is a translatable string with a source key; adding a language needs only a new translation file, no change of code, and the procedure is documented and tested by adding a dummy language. The packages carry the compiled translations. | V1 |
| REQ-UI-14 | The application has a **cover page**: an image with the current glucose value and the trend indicator, shown on the application's card on the home screen. It follows REQ-UI-03 to 05 and 09: colour only for the glucose band, stale data loses its colour and shows its age, and a stopped monitoring or an old value is never shown as current. It exists only while the UI process runs and is never a substitute for the monitoring service. | V1 |
| REQ-UI-15 | Silent mode is shown permanently next to the reading wherever the Subject appears (the selection screen, the Subject page and the cover) by a struck-through speaker pictogram and text ("Silent until 14:30", "Silent"), never by colour alone; it is set from the Subject selection screen and from the Subject page. Setting it without limit asks one explicit confirmation that says that urgent alerts will be silent as well, and the resident notice says that a Subject is silent (both are safeguards). | V1 |
| REQ-UI-16 | No text the user sees names an alert level by its abbreviation: the levels are *very low*, *low*, *high* and *very high* (and *no data* for the missing-data alert) and their translations, and the monitored person is a *patient*, on every page, in every notification, on the thresholds page ("low limit", "high limit") and in the user documentation. The abbreviations and the words urgent low, urgent high and Subject stay in the code and in the specification. | V1 |

---

## 11. Security and privacy requirements

| ID | Requirement | Stage |
|---|---|---|
| REQ-SEC-01 | Credentials and session blobs shall be kept in Sailfish Secrets (on Debian, the platform's secret store), never in configuration files. Each adapter receives only its own credentials. | V0 |
| REQ-SEC-02 | The app shall be local-only with **no telemetry**. This is an explicit design rule. | V0 |
| REQ-SEC-03 | Encryption at rest of the store shall be evaluated in the design, and delete per Subject provided (REQ-SUB-04); the application exports nothing. | V1 |
| REQ-SEC-04 | Per-adapter network restrictions (declared hosts enforced by the sandbox) are not available while all adapters run inside the application's own sandbox (REQ-ADP-15). To be reconsidered if the platform later allows it. | LATER |
| REQ-SEC-05 | The credentials and session blob shall live only in a Sailfish Secrets collection (device lock, unlocked after first use), accessed by the service and the UI. After a phone restart the first access needs a one-time user approval (a system notification); the service shall request it itself. No plain copy shall be kept to avoid the prompt. | V1 |
| REQ-SEC-06 | Processes in adapter mode shall not access the secret store. Credentials reach an adapter only inside the `authenticate` command. | V0 |
| REQ-SAF-01 | The README and the application shall state that the app is a **supplementary monitor**, not a medical device, and not to be relied on for therapy decisions. | V0 |

---

## 12. Documentation requirements

| ID | Requirement | Stage |
|---|---|---|
| REQ-DOC-01 | Current implementation and assumptions shall be **thoroughly documented**: a "how it works today" document for the Libre adapter, a threat and privacy note, a plan-B note, the adapter contract. | V0 |
| REQ-DOC-02 | Each assumption that depends on external behaviour shall be documented with the claim, how it was learned, when it was last verified and the test that would detect its failure. | V0 |
| REQ-DOC-03 | **Fixtures** built from the written format description (synthetic, containing no real person's data, REQ-SIM-08) and contract tests shall make a protocol change show up as a failing test. | V0 |
| REQ-DOC-04 | The README shall state honestly the unofficial status of the interface. | V0 |
| REQ-DOC-05 | All documents shall be Markdown. | V0 |
| REQ-DOC-06 | **Code shall be thoroughly commented** to explain the patterns it uses and the assumptions it makes. Every file states its purpose; every class and every non-trivial function states what it is for, which pattern or design approach it follows and why, and the assumptions it depends on (about the source, the platform, the input or the caller). A comment is understandable on its own, in plain words, by a reader who has not read the requirements; a requirement number (`REQ-...`) is a pointer to the detail and never replaces the explanation. Comments explain **why**, not what the next line does. Tests state what they prove. | V0 |

---

---

## 13. Not built yet

Ideas for later versions, in no fixed order and without dates. They are not promises. The list is the same as the "Roadmap" in the README, with the requirement numbers added.

- Other types of sensor, through further data sources. This needs data and help from people who use those sensors.
- Testing and tuning with several real patients.
- Manual entry of data (notes, insulin, meals, weight and other measurements), shown with the glucose (REQ-DAT-10).
- Predictive alerts and a history of past alerts.
- An "expiring soon" notice for the sensor (REQ-SEN-04).
- A screen that shows which protection limits were reached, when and how often (REQ-GRD-08).
- Reading intervals as a setting, the add-on state in Settings, and a line on the overview that reports an interruption.
- Data sources written in other languages or shipped as separate packages (REQ-ADP-16).
- Per-adapter network restrictions, if the platform allows them (REQ-SEC-04).
