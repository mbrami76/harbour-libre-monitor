<!-- SPDX-License-Identifier: GPL-3.0-or-later -->
<!-- Copyright (C) 2026 Michele Brami -->
# lm-cli

Command-line program (REQ-ARC-04): log in, discover the Subjects, print the current value. It plays the core's side of the adapter contract through the session driver (`core/session/`), with no store, no scheduling and no UI.

**Host builds only** (`CONFIG+=hostbuild`). It is installed in no package and is not part of the Harbour RPM. Supplementary monitor, not a medical device.

## Build

The PC has no Qt; the host build runs in the SDK i486 build engine (`CONTRIBUTING.md`, "Building and testing"). `make` in the build directory also builds `adapters/libre/host/libre-adapter-host`, the executable that `lm-cli` starts as the adapter process.

## Use

```text
lm-cli <command> [options]

commands
  discover                list the Subjects the account can follow
  current                 print the current value of each Subject
  range --from T --to T   print the readings between two UTC times (YYYY-MM-DDTHH:MM:SSZ)

options
  --credentials-file P    file with username= and password= lines (see below)
  --session-file P        keep the opaque session between runs
  --adapter P             adapter host executable (default: libre-adapter-host next to lm-cli)
  --subject N             only the Subject with this index (default: all)
  --json                  machine-readable output on standard output
  --show-ref              discover --json only: also print each Subject's source reference
  --verbose               adapter log and driver diagnostics on standard error
  --base-url URL          development builds only: use a simulated source
  --help
```

Subjects are addressed by the index that `discover` prints. The source's own reference is not shown unless you ask for it with `discover --json --show-ref` (it identifies a person: keep the output out of chats, commits and screenshots; the provisioning mode of the app needs this value). The program keeps no Subject mapping (that belongs to the configuration).

`current` and `range` run `discover` first and then fetch each chosen Subject, one command at a time. `range` returns what the source can answer: the graph covers about 12 hours and the logbook about 13 days, whatever the request; the `coverage` lines say what each feed covered (contract 5.5).

## Credentials: safe handling (REQ-ADP-11, REQ-SEC-06)

- Credentials are **never** arguments or environment variables. There are no such options: `--password` is rejected as an unknown option, and the error text names the option, never its value.
- **Credentials file:** two lines, `username=<text>` and `password=<text>` (the text is the rest of the line, taken as it is). Blank lines and lines starting with `#` are ignored. The program refuses the file unless it is a regular file (a symbolic link is refused), owned by the user running the program, and without any permission for group or others (`chmod 600`). The error text never quotes the file.
- **Prompt:** without `--credentials-file`, the program asks for the username and then the password with the terminal echo off. It needs a terminal on standard input and standard error; otherwise it ends with exit code 2.
- **Session file** (optional): its first line is the opaque session the adapter returned (about 180 days). Same mode rule for reading; written with mode 0600, never through a symbolic link. The session is a credential: keep the file private and delete it when you stop using it. The program does not rotate or expire it.
- Credentials, the session and the Subject names are scrubbed from every adapter diagnostic before it is shown. The program never prints the session, a token or the credentials.
- **Names** are personal data: `discover`, `current` and `range` print them on standard output, nothing else does, and the program writes no file except the session file.
- Keep the credentials file outside the repository. Delete it after a real-source run.

With `--base-url` the program prints `SIMULATED SOURCE` on standard error. The option exists only in builds with the flag `devsource` (implied by `hostbuild`); a release build has neither the option nor the code (REQ-SIM-06).

## Exit codes

| Code | Meaning |
|---|---|
| 0 | success |
| 1 | other failure: the adapter could not be started or ended unexpectedly |
| 2 | usage error: arguments, credentials or session file, no terminal for a prompt, Subject index out of range |
| 10 | `auth-failed` |
| 11 | `terms-must-be-accepted` |
| 12 | `rate-limited` (the retry hint, if any, is printed) |
| 13 | `transient` (also a missed hello or command deadline) |
| 14 | `protocol-changed` (also a burst of rejected records: at least 3 rejected and at least half of those examined, REQ-ADM-03; a feed of fewer records never gives 14 for that reason) |
| 15 | `invalid-command` |
| 20 | admission rejected (REQ-ADM-01); the reject reason is printed |
| 21 | hard contract violation by the adapter (contract section 7); the program ends the adapter at once (the default policy answers `kill`), so this also comes without waiting for a deadline |

Codes 1 and 21 are proposals. Errors go to standard error as `error: <class>: <detail>`; the detail is the adapter's text after scrubbing. If the adapter broke contract rules, a line `contract violations: N hard, M soft (kinds)` follows (kinds only, no content).

## `--json` shape

One compact JSON object per run on standard output, nothing else there. Times are UTC ISO 8601 with milliseconds. Keys are stable for scripts; additions are possible, removals are not.

`discover`:

```json
{"subjects":[{"index":0,"display_name":"…","unit":"mg/dL","unit_basis":"code"}]}
```

With `--show-ref` each entry also has `"subject_ref"`.

`unit` is `null` when undetermined; `unit_basis` is `code`, `relation`, `magnitude` or `none` (REQ-DAT-13).

`current`:

```json
{"subjects":[{"index":0,"display_name":"…",
  "current":{"value":112,"unit":"mg/dL","measured_at":"2026-01-01T12:00:00.000Z","age_minutes":5,"source":"libre.current"},
  "sensor":{"id":"<hash>","activated_at":"…","warmup_minutes":60}}]}
```

`current` is `null` when the Subject has no current value; `sensor` is `null` when the adapter sent none. `age_minutes` is whole minutes since the measurement, never negative.

`range`:

```json
{"subjects":[{"index":0,"display_name":"…",
  "records":[{"metric":"glucose","measured_at":"…","source":"libre.graph","unit":"mg/dL","value":112}],
  "coverage":[{"feed":"graph","from":"…","to":"…"}],
  "watermark":"…",
  "records_count":{"received":148,"valid":148,"rejected":0},
  "sensor":null}]}
```

`records` are the valid records in time order; events (`logbook_entry`) carry `payload` instead of `value`. `rejected` counts records the core refused (quarantine, REQ-ADM-02); the program has no store and keeps only the count.

## Tests

- `tests/tst_cli`: arguments, the file-mode rule, exit-code mapping, output shapes (QtTest, host build).
- `tests/integration/run_integration.py`: runs `lm-cli` through the build engine against the simulator (Python 3, standard library). See its header for how the simulator is bound to the build engine's network. Run: `python3 tests/integration/run_integration.py`.
- The real source is never used by tests.

## Run against the real source (the author's own run)

This is milestone M1. The program never contacts the real source on its own; the module has not done it. From the build directory, in a terminal:

```bash
cd build-host-i486 && sfdk -c target=SailfishOS-5.1.0.11-i486 build-shell make -j4
```

Create the credentials file **outside the repository**, with your own follower account, for example `<your build directory>/real-credentials` (inside the build directory, which is outside the repository; the build engine sees it under the same path):

```bash
install -m 600 /dev/null <your build directory>/real-credentials && nano <your build directory>/real-credentials
```

with the two lines `username=…` and `password=…`. Then, without `--base-url`:

```bash
cd build-host-i486 && sfdk -c target=SailfishOS-5.1.0.11-i486 build-shell ./cli/lm-cli current --verbose --adapter ./adapters/libre/host/libre-adapter-host --credentials-file real-credentials
```

What to look at:

1. Exit code 0 and, per Subject, a value with its unit, a measurement time in UTC and an age of a few minutes at most.
2. Source `libre.current`, and a `sensor:` line (the id is a hash; `warm-up` and `activated` come from the source).
3. No `SIMULATED SOURCE` banner, no `contract violations:` line. With `--verbose` the adapter's log lines appear on standard error (counts and notices, no values or names).
4. `discover` shows the unit and its basis (`code` is the verified one); compare the unit with the Abbott app.
5. Any other exit code: note the code and the `error:` line (they are the first real login and error replies). Do not paste raw replies into documents.

Afterwards delete the credentials file.
