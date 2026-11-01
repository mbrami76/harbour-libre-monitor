# Code map

This page says what every published file is for. It is meant for someone who wants to find their way around the source: which folder to open, which file does what, and which file to read first for a given behaviour. `architecture.md` explains the design; this page ties it to the files, and the last section follows five requests through the code.

How to read it. Each folder has a short introduction (what the folder is for and who calls it), then one line per file. A header and its source file share a line (`foo.h`, `foo.cpp`). Every test program has one line. Icons, images and translations are described as groups. File names are given relative to the folder named in the heading above them. A check runs before every release and fails when a file is missing from this page or when this page names a file that does not exist, so the page cannot silently go stale.

## The layers

| Folder | What it is | Called by |
|---|---|---|
| `core/` | The logic that does not depend on the screens or on the phone: records, the store and the journal, the guard, the scheduler, the alert logic, the service life cycle. Qt core and SQL only | everything else |
| `adapters/libre/` | The only code that knows the data source | the service, by starting the binary in adapter mode |
| `daemon/` | The process side of the service: the modes of the binary, the D-Bus interface, the wiring of the pipeline, the ports for the phone and for the PC | `app/src/main.cpp`, the host executables, the tests |
| `app/ui/` | The logic of the screens, without QML | `app/qml/`, `app/real/` |
| `app/real/` | The real data sources of the screens: the service over D-Bus, the store read-only, the account page's saving | `app/src/main.cpp` |
| `app/qml/`, `app/src/` | The pages, the cover, and the `main` function that picks the mode | the system |
| `cli/` | A command-line program that plays the core's side of the adapter protocol | developers, host builds only |
| `tests/` | The test programs, their fakes and fixtures, and the integration scripts | developers |
| `tools/` | The data source simulator and the packaging scripts | developers |
| `design/`, `rpm/`, `addon/` | Icons and drawings, the package description, the optional add-on | the build |

## Files

### `./`

The project files at the top of the tree.

- `harbour-libre-monitor.pro`: the top-level qmake project; lists the libraries, the application and, in host builds, the test programs, the command-line program and the host executables, with their build order.
- `common.pri`: build settings shared by all projects: the development flag `devsource` (implied by host builds) and the application version read from the package description.
- `LICENSE`: the GNU General Public License, version 3.
- `README.md`: what the application is, how to try it, and the list of documents.
- `CONTRIBUTING.md`: how to build and test, the code style, and how generated code is handled.
- `SECURITY.md`: how to report a vulnerability, what is in scope.
- `.gitignore`: build products and local captures that are not kept.
- `.github/FUNDING.yml`: the Liberapay link GitHub shows as the Sponsor button.

### `docs/`

The published documents.

- `requirements.md`: the numbered requirements (`REQ-...`) that the code and the tests refer to.
- `architecture.md`: how the application is built, with diagrams.
- `code-map.md`: this page.
- `install.md`: installing, upgrading and removing the package, and the optional add-on.
- `screenshots/*.png`: the screenshots used by the README, one per main page.

### `adapters/libre/`

The Libre adapter: everything that knows the data source (REQ-ARC-06). It is a static library that the application links into its one binary; the service starts that binary with `--adapter libre` and `libremain` runs the adapter loop on standard input and output. Nothing else in the code base knows the source's addresses, field names or timestamp format. Only this folder uses QtNetwork.

- `libre.pro`: the project of the static library.
- `libreadapter.h`, `libreadapter.cpp`: the adapter loop of the protocol (`hello`, `accept` or `reject`, `configure`, one command at a time, `shutdown`) and the commands that log in, list the people, fetch the current reading, and fetch a range of history and logbook; `runLibreAdapter`.
- `libreclient.h`, `libreclient.cpp`: the HTTP side: the headers the source expects, the login, the regional redirect, the session blob with its expiry, and the mapping of HTTP statuses to error classes.
- `libreerror.h`: the description of a failed command (error class and a detail that never holds a secret).
- `libremain.h`, `libremain.cpp`: runs the adapter on the real standard streams; shared by adapter mode of the application and by the development host executable.
- `libresource.h`, `libresource.cpp`: turns the source's readings and logbook entries into records, builds the source tag `libre.<feed>`, the trend and the sensor object, and decides when a run of unusable items means the source has changed.
- `libretimestamp.h`, `libretimestamp.cpp`: parses the source's timestamp format (`M/D/YYYY h:mm:ss AM|PM`) into UTC, strictly and without a locale.
- `libreunit.h`, `libreunit.cpp`: decides the unit of a reading (mg/dL or mmol/L) from the source's unit code and the relation between its two values; a reading that fits neither is not guessed.
- `httptransport.h`: the abstract transport (the only door to the network) so that tests run without sockets.
- `qthttptransport.h`, `qthttptransport.cpp`: the transport on `QNetworkAccessManager`: default TLS checks, no redirects followed, no cookies, a limit on the size of a reply.

### `adapters/libre/host/`

- `host.pro`, `main.cpp`: a development-only executable (`libre-adapter-host`) that runs the adapter loop on the real standard streams. Built only in host builds, never installed or packaged.

### `core/`

- `core.pro`: the project of the shared static library: lists every source file of the subfolders below. Qt core and SQL only.

Most of the logic lives in the subfolders. A pattern repeats through them: the rules that decide are written against small interfaces (a clock, a random source, a store, a link), so that tests can drive it with fakes and a clock moved by hand, and the service plugs in the real ones.

### `core/time/`

- `utctime.h`, `utctime.cpp`: `UtcTime`, a point in time in UTC with millisecond resolution; strict ISO 8601 text in and out, and the "not in the future" check with a clock-skew tolerance.

### `core/units/`

- `glucoseunits.h`, `glucoseunits.cpp`: the one definition of the conversion between mg/dL and mmol/L (18.0156 mg/dL per mmol/L) and of the rounding, which is half away from zero on the decimal text and independent of the locale.

### `core/record/`

- `record.h`, `record.cpp`: `Record` (a sample or an event with its unit, UTC time, source tag and origin), its uniqueness key, and `validateRecord`, which names the reason a record is refused and never corrects it.
- `registry.h`, `registry.cpp`: the metric registry: which metrics exist (glucose and the logbook), their accepted units and plausible ranges, the allowed fields of an event, and the thinning rule compaction applies.

### `core/jsonl/`

- `jsonlcodec.h`, `jsonlcodec.cpp`: the JSON Lines decoder and encoder used on the adapter link: one message per line, a limit on the length of a line, malformed lines reported without quoting their content.

### `core/protocol/`

- `envelope.h`, `envelope.cpp`: the message header of the adapter protocol (version, id, type, correlation id), the message types, and the generator of message ids.

### `core/store/`

The measurement store and the service journal, both SQLite, and the jobs that maintain them. Called by the scheduler's store adapter, the alert coordinator, the service lifecycle, the maintenance runner and, read-only, by the window.

- `store.h`, `store.cpp`: the measurement store: ingest with validation and duplicate check, quarantine, marks, sensors, the ingest log, queries; append-only, enforced by triggers in the database.
- `journal.h`, `journal.cpp`: the service journal in its own database file: start, stop and heartbeat entries, connection states, alert, threshold and silent entries, pruning of old heartbeats, and the one purge exception.
- `dbutil.h`, `dbutil.cpp`: opens an SQLite database for writing or read-only, sets the write-ahead log, refuses a schema newer than the code, and small helpers for time and JSON columns.
- `migration.h`, `migration.cpp`: the migration steps of the store's schema, one version at a time.
- `compactor.h`, `compactor.cpp`: compaction: the estimate (dry run), the space check, the thinning of old current-feed readings, the compaction record, and the rebuild of the file.
- `purge.h`, `purge.cpp`: the purge of one person's data older than a chosen age, in the store and in the journal, with its estimate.
- `storagecheck.h`, `storagecheck.cpp`: the weekly storage check: sizes, free space, the low-space verdict, the entry in the journal, and the pruning of the journal that precedes it.
- `progress.h`, `progress.cpp`: throttled progress reports for the long store operations.

### `core/guard/`

The guard (REQ-GRD-*): the policy side of the link to an adapter process. It decides what a violation weighs and what to do about it. Called by the adapter link (as its policy) and by the scheduler.

- `guard.h`, `guard.cpp`: the guard: token buckets, the violation score, the reaction ladder, the circuit breaker, the spawn allowance, the trip to `disabled-by-guard` and the per-version trip, and the manual re-enable.
- `guardsettings.h`: every tunable number of the guard in one place.
- `violationscore.h`, `violationscore.cpp`: the score with lazy exponential decay and the token bucket.
- `statekeeper.h`, `statekeeper.cpp`: the persistent state of each connection and adapter version (disabled, needs attention, rate-limit hold), folded from a log and kept in memory.
- `statelog.h`: the interface of that log, the names of the states and causes, and an in-memory log for tests.
- `journalstatelog.h`, `journalstatelog.cpp`: the state log on the service journal (entries of kind `connection-state`).
- `ingestlog.h`, `ingestlog.cpp`: a bounded queue in front of the ingest log, so a busy store never blocks the guard; losses are counted and written as an entry.
- `ports.h`: the small interfaces shared by the guard and the scheduler: the clock, the random source, and the ingest-log entry.

### `core/session/`

The session driver: the core's end of the link to one adapter process (REQ-ADP-*, REQ-ADM-*). It detects, counts and reports; it does not decide policy.

- `adapterlink.h`, `adapterlink.cpp`: `AdapterLink`, the driver: starts the process, handles the handshake, sends one command at a time, checks every line and every record, reports violations to the policy, and delivers valid records and quarantined ones to their sinks.
- `linkpolicy.h`: the boundary between the driver and the guard: the identity of a link, violation kinds and severities, the error classes, the actions the policy can answer with, and the types of a command's outcome and completion.
- `nullpolicy.cpp`: the policy of the command-line program: a hard violation ends the adapter, a soft one is only recorded.
- `linkchannel.h`: the driver's view of the other end of the link, a process in production and in-memory pipes in tests.
- `processchannel.h`, `processchannel.cpp`: the channel on a child process, with limits on memory, CPU time and open files.
- `processmemory.h`, `processmemory.cpp`: reads a process's highest memory use from `/proc`.
- `adapterregistry.h`, `adapterregistry.cpp`: the list of built-in adapters; nothing outside it can run.
- `scrubber.h`, `scrubber.cpp`: removes known secrets and names from text before it is shown or logged.
- `diagnosticreport.h`, `diagnosticreport.cpp`: the pure part of the diagnostic report: it builds the text from a held batch and the notes of a run, without any glucose value.

### `core/scheduler/`

The scheduler (REQ-SCH-*): decides when to run which command and what to do about an error. Polled, with no timer or clock of its own. Called by the service lifecycle at every wake-up.

- `scheduler.h`, `scheduler.cpp`: the scheduler: the state machine of each person on each connection, the dispatch of due feeds, error handling by class, rate-limit holds, the staleness status, and the acceptance of records.
- `connectionsession.h`, `connectionsession.cpp`: the session manager of one connection: reading the login from the secret store, starting the adapter, logging in, renewing the token, the one re-login, and the waits between attempts.
- `schedulemath.h`, `schedulemath.cpp`: the pure arithmetic: backoff with jitter, the effective polling interval, the next run of the current feed, and the staleness cause.
- `schedulersettings.h`: every tunable number of the scheduler and the session manager.
- `schedulerports.h`: the interfaces the scheduler uses: the link, the secret store, the connection settings, the reading store, the link factory.
- `adapterlinkport.h`: wraps the real `AdapterLink` as the scheduler's link port, and makes links for it.
- `storereadingstore.h`, `storereadingstore.cpp`: connects the scheduler to the store: newest reading, ingest, quarantine, sensors, and the sink for ingest-log entries.

### `core/alerts/`

The alert logic (REQ-ALR-*, REQ-THR-*): a pure engine fed with readings, thresholds and time. Called by the alert coordinator.

- `alertengine.h`, `alertengine.cpp`: the engine: three slots per person (low, high, missing data), their states and transitions, persistence, hysteresis, reminders, escalation, acknowledge, snooze, and the list of effects it answers with.
- `alertcoordinator.h`, `alertcoordinator.cpp`: connects the engine to the service: listens to the service's events, reads the newest reading, writes the journal lines, hands notifications to the notifier, and offers the user's actions.
- `alerttypes.h`, `alerttypes.cpp`: the vocabulary: alert types, slots, states, and their names as written in the journal.
- `alertsettings.h`: every constant of the alert engine, in one place.
- `alertjournal.h`, `alertjournal.cpp`: turns alert and threshold records into journal entries and back, and rebuilds the engine's state at start.
- `thresholds.h`, `thresholds.cpp`: the four thresholds in whole mg/dL and the conversion of an edit typed in mmol/L.
- `thresholdbook.h`, `thresholdbook.cpp`: each person's thresholds with their append-only history, the rules for an edit and the challenge for a looser limit.
- `band.h`, `band.cpp`: which of the five bands a stored reading is in; used by both the engine and the screens.
- `silentbook.h`, `silentbook.cpp`: the ledger of silent mode per person, its end times and "until tomorrow".

### `core/service/`

The service life cycle (REQ-SVC-*) and the pieces the service hangs together from. Mostly rules that are polled by the lifecycle; the real platform is reached only through the interfaces in `serviceports.h`.

- `servicelifecycle.h`, `servicelifecycle.cpp`: the life cycle: the states from start to stop, the heartbeat, the weekly storage check, the approval re-raise, the wake-up that drives the scheduler, and the orderly stop.
- `serviceports.h`: the interfaces the life cycle uses (clock, boot-time clock, journal, notifier, activator, version stamp, the check of who holds the D-Bus name) and the types of a start and a stop.
- `servicesettings.h`: every tunable number of the service life cycle.
- `startcause.h`, `startcause.cpp`: works out why this start happened (first start, after a restart, after a stop, after a missing stop) from the last journal entries.
- `versionwatch.h`, `versionwatch.cpp`: compares the stamp the package leaves on disk with the version of the running binary, with the guard against restarting itself in a loop.
- `wakepolicy.h`, `wakepolicy.cpp`: how soon the next wake-up has to be, from what the scheduler and the alert engine are waiting for.
- `schedulerhost.h`, `schedulerhost.cpp`: the life cycle's narrow view of the scheduler; it forwards the calls and adds the few things the scheduler does not offer itself.
- `serviceevents.h`, `serviceevents.cpp`: the events the service passes on to the alert logic and to the window; it forwards them and does not interpret them.
- `journallog.h`, `journallog.cpp`: the life cycle's view of the journal: writes the start, stop and heartbeat entries and finds the newest of each.
- `maintenancerunner.h`, `maintenancerunner.cpp`: runs a compaction or a purge as a job with a single flight, progress, and a result the window can ask for.
- `effectfacts.h`: the facts the effects of the alert engine need from the outside (names, texts, whether the phone is quiet).
- `alertnotification.h`, `alertnotification.cpp`: turns an alert effect into a notification (title, text, sound, vibration, actions).
- `notificationcatalog.h`, `notificationcatalog.cpp`: the catalogue of every kind of notification and which of them the user can silence.
- `notificationtexts.h`, `notificationtexts.cpp`: the texts of notifications, built from facts and never from stored free text.
- `residentnotice.h`, `residentnotice.cpp`: the notice that stays while the service is running, shown or cleared according to what the service needs from the user.
- `noticewatcher.h`, `noticewatcher.cpp`: posts and clears the notices that are not alerts (waiting for approval, a connection disabled by the guard, a proposed diagnostic report) from the service's events.
- `donotdisturb.h`, `donotdisturb.cpp`: the rules for the phone's do-not-disturb state and which alerts pass it.
- `silentcontrol.h`: the interface through which the window asks the service to start or end silent mode.
- `subjectlabel.h`, `subjectlabel.cpp`: how a person is named where a stranger may read it (notifications, the resident notice, the cover), including when names are switched off.
- `diagnosticcollector.h`, `diagnosticcollector.cpp`: gathers the facts of a diagnostic report from the service and the files.
- `reportproposal.h`, `reportproposal.cpp`: proposes a diagnostic report to the user after a run of trouble, once per episode.

### `daemon/`

The process side. This folder turns the core's pieces into running programs: the four modes of the binary, the D-Bus interface, and the real implementations of the ports (files, clock, secret store, notifications on the phone; fakes and files on the PC). It is a static library linked into the application and into the host executables.

- `daemon.pro`: the project of the library.
- `processmode.h`, `processmode.cpp`: reads the command line and says which of the four modes was asked for.
- `servicecore.h`, `servicecore.cpp`: the facade that builds the whole pipeline (store, journal, guard, scheduler, alerts, life cycle) from the ports and starts it.
- `serviceruntime.h`, `serviceruntime.cpp`: the wake-up loop on Qt's event loop: timers, signals, the stop sequence.
- `serviceprocess.h`, `serviceprocess.cpp`: the service mode as a whole: single instance, creating the pipeline, claiming the D-Bus name, exit codes.
- `applicationservice.h`, `applicationservice.cpp`: the `--service` mode as called from `main`.
- `serviceplatform.h`: the interface that gathers everything the service needs from its platform, with the phone's and the PC's implementations behind it.
- `serviceconfig.h`, `serviceconfig.cpp`: reads `config.json` (connections, people, the polling interval) and reports what is wrong with it.
- `serviceidentity.h`, `serviceidentity.cpp`: the names of the service: D-Bus name and path, file locations, collection name in the secret store.
- `commonports.h`, `commonports.cpp`: ports that are the same on the phone and on the PC: the system clock, the random source, the check of who holds the D-Bus name, the JSON settings file, the version stamp file, and the self-activation through D-Bus.
- `dbusservice.h`, `dbusservice.cpp`: the D-Bus interface: the abstract API, the object that serves it, the adaptor, and the client the window uses.
- `adaptermode.h`, `adaptermode.cpp`: the `--adapter <id>` mode: finds the adapter in the registry and runs it on standard streams.
- `provisionmode.h`, `provisionmode.cpp`: the `--provision` mode as called from `main`.
- `provisioning.h`, `provisioning.cpp`: the provisioning tool: writes a connection and its login to the secret store and `config.json` without the window.
- `applicationprovision.h`, `applicationprovision.cpp`: the pieces of provisioning that need the application's own settings.
- `terminalprompt.h`, `terminalprompt.cpp`: reads a password from the terminal without echo.
- `discovery.h`, `discovery.cpp`: asks the adapter for the people behind a login, for the account page and for provisioning.
- `maintenancejobs.h`, `maintenancejobs.cpp`: the store jobs behind the maintenance runner (estimate, compact, purge) on the real files.
- `uiguard.h`, `uiguard.cpp`: lets only one window process run at a time; a second tap on the icon does not open a second window.
- `unixsignals.h`, `unixsignals.cpp`: turns the termination signals into Qt events for an orderly stop.
- `phoneports.h`, `phoneports.cpp`: the phone's implementations of the ports: keep-alive, system notifications, Sailfish Secrets. Compiled for the device only.
- `hostports.h`, `hostports.cpp`: stand-ins for what only the phone has, so the service runs on a PC: a timer for the keep-alive, the console for notifications, and a secret store that keeps nothing on disk. Host builds and tests only.
- `devfeed.h`, `devfeed.cpp`: a feed of made-up readings for development builds. Compiled only with `devsource`.
- `devlog.h`: a small development log. Compiled only with `devsource`.

### `daemon/host/`

- `host.pro`, `main.cpp`: a development-only executable (`lm-service`) that runs the service on a PC with the host ports. Built only in host builds, never installed or packaged.

### `app/`

The application: the window, and the entry point of the one binary.

- `app.pro`: the project of the executable `harbour-libre-monitor`; links the libraries and installs the QML, the translations and the icons.
- `harbour-libre-monitor.desktop`: the launcher entry and the sandbox permissions (Internet, Secrets, Documents); `ExecDBus` is what lets the system start the service by name.

### `app/src/`

- `main.cpp`: the entry point. Reads the mode first (window, `--service`, `--adapter`, `--provision`), starts that mode, and for the window picks the real data source or, in development builds, the fake one.
- `plotitem.h`, `plotitem.cpp`: the painted item that draws the plot from ready-made geometry; it holds no logic of its own.

### `app/ui/`

The logic of the screens, with no QML. Each page has a view type that turns data into the values the page shows, and a controller that handles what the user does. Everything here is written against the interfaces `backend.h`, `alertsbackend.h`, `accountbackend.h` and `maintenancebackend.h`, so the same code runs on the real source (`app/real/`) and on the fakes.

- `ui.pro`: the project of the library.
- `backend.h`: the interface through which the screens get the people, the readings, the sensor and the status of monitoring.
- `alertsbackend.h`: the interface for alerts, thresholds and silent mode.
- `accountbackend.h`: the interface for the account page.
- `maintenancebackend.h`: the interface for storage, compaction and purge.
- `appsettings.h`: the interface and the names of the window's own settings (units, plot range, language).
- `uiconstants.h`: sizes and limits shared by the screens.
- `band.h`: the band names and colour keys the screens use for a reading.
- `glucoseformat.h`, `glucoseformat.cpp`: formats a value in the chosen unit with the right number of decimals.
- `direction.h`, `direction.cpp`: the trend as an arrow and its words.
- `subjectmodel.h`, `subjectmodel.cpp`: the list model of the people on the overview.
- `subjectstate.h`, `subjectstate.cpp`: the state of one person as the overview shows it (value, age, band, alert, staleness).
- `sensorview.h`, `sensorview.cpp`: the sensor block of a person's page.
- `monitoringinfo.h`, `monitoringinfo.cpp`: the text and flags for the monitoring strip: running, waiting for approval, stale, the cause.
- `coverrank.h`, `coverrank.cpp`: picks which person the cover shows.
- `plotview.h`, `plotview.cpp`: the plot's range, axes and the values to draw.
- `plotgeometry.h`, `plotgeometry.cpp`: turns readings into points in the plot's pixel space.
- `plotreduce.h`, `plotreduce.cpp`: thins the points to what the width of the screen can show.
- `plotcontroller.h`, `plotcontroller.cpp`: loads the readings for the plot and keeps it up to date.
- `alertmodel.h`, `alertmodel.cpp`: the list model of the alerts on a person's page.
- `alertview.h`, `alertview.cpp`: the values and texts of the alert section and of the alerts page.
- `thresholdview.h`, `thresholdview.cpp`: the values the thresholds page shows.
- `thresholdeditor.h`, `thresholdeditor.cpp`: edits the four thresholds, validates them and asks for the challenge when a limit gets looser.
- `accounterrors.h`, `accounterrors.cpp`: the words for each way the account setup can fail.
- `accountsetup.h`, `accountsetup.cpp`: the account page's steps: sign in, choose the people, save.
- `configstore.h`, `configstore.cpp`: reads and writes `config.json` for the window.
- `configsettings.h`, `configsettings.cpp`: the connection settings seen from the screens.
- `settingsstore.h`, `settingsstore.cpp`: reads and writes `ui-settings.json`.
- `storageview.h`, `storageview.cpp`: the values the storage page shows.
- `storagecontroller.h`, `storagecontroller.cpp`: starts compaction and follows its progress.
- `purgeview.h`, `purgeview.cpp`: the values of the purge page and its confirmation.
- `purgecontroller.h`, `purgecontroller.cpp`: starts a purge and follows its progress.
- `reportcontroller.h`, `reportcontroller.cpp`: shows the diagnostic report and hands it to the user.
- `fakebackend.h`, `fakebackend.cpp`: made-up people and readings for development builds. Compiled only with `devsource`.
- `fakealertsbackend.h`, `fakealertsbackend.cpp`: made-up alert state for development builds. Compiled only with `devsource`.
- `fakeaccountbackend.h`, `fakeaccountbackend.cpp`: a made-up account page for development builds. Compiled only with `devsource`.
- `fakemaintenancebackend.h`, `fakemaintenancebackend.cpp`: made-up storage and purge for development builds. Compiled only with `devsource`.

### `app/real/`

The real data source of the screens. The window reads the store and the journal read-only and talks to the service over D-Bus; it never writes the measurement data.

- `real.pro`: the project of the library.
- `realbackend.h`, `realbackend.cpp`: the backend for the overview and the person pages: builds a snapshot, refreshes it when the service says data changed.
- `snapshotbuilder.h`, `snapshotbuilder.cpp`: builds one consistent snapshot of people, readings, sensor and status.
- `storequeries.h`, `storequeries.cpp`: the read-only queries on the store and the journal.
- `storeworker.h`, `storeworker.cpp`: runs those queries on a worker thread so the window stays responsive.
- `servicelink.h`, `servicelink.cpp`: the window's connection to the service: calls, signals, and the state when the service is not there.
- `statusparser.h`, `statusparser.cpp`: reads the service's status text into the values the screens use.
- `realalertsbackend.h`, `realalertsbackend.cpp`: the backend for alerts, thresholds and silent mode, through the service.
- `realaccountbackend.h`, `realaccountbackend.cpp`: the backend for the account page: discovery, saving the login and the connection.
- `realmaintenancebackend.h`, `realmaintenancebackend.cpp`: the backend for storage, compaction and purge, through the service.

### `app/qml/`

- `harbour-libre-monitor.qml`: the application window and the page stack.
- `cover/CoverPage.qml`: the cover shown in the app switcher.
- `images/asclepius.png`: the symbol shown on the About page.

### `app/qml/pages/`

One file per page or dialog.

- `OverviewPage.qml`: the first page: one tile per person and the monitoring strip.
- `SubjectPage.qml`: one person: the reading, the plot, the sensor and the alerts.
- `AlertsPage.qml`: the alerts of one person and their actions (acknowledge, snooze).
- `ThresholdsPage.qml`: edits the four thresholds.
- `ThresholdChallengeDialog.qml`: the question asked before a limit is made looser.
- `SilentPage.qml`: chooses silent mode and its length.
- `SilentConfirmDialog.qml`: confirms silent mode.
- `StoragePage.qml`: the sizes of the store and the journal, and compaction.
- `PurgePage.qml`: deletes one person's old data.
- `AccountPage.qml`: sign in to the source and choose the people to follow.
- `FirstRunDialog.qml`: the notice and the choices shown the first time.
- `SettingsPage.qml`: units, plot range and the links to the other pages.
- `DiagnosticReportDialog.qml`: shows a diagnostic report before it is shared.
- `AboutPage.qml`: version, licence and the notice that this is not a medical device.

### `app/qml/components/`

Pieces used by several pages. The `.js` files hold the wording for a state so that pages stay free of text logic.

- `AlertLine.qml`, `AlertTimeBox.qml`: one line of an alert and its time.
- `BandBadge.qml`: the coloured mark for a band.
- `DndStrip.qml`: the strip that warns the phone is on do-not-disturb.
- `MonitoringStrip.qml`: the strip that says whether monitoring runs and why not.
- `ReadingBox.qml`: the value, unit, arrow and age.
- `SensorBlock.qml`: the sensor's state and time left.
- `SilentLine.qml`: the line that says silent mode is on.
- `SimulatedBar.qml`: the bar shown in development builds that use made-up data.
- `SubjectTile.qml`: one person's tile on the overview.
- `Style.js`: colours, sizes and fonts.
- `AgeText.js`: "5 minutes ago" and similar.
- `AlertText.js`: the words for alerts and their states.
- `PurgeText.js`: the words for the purge steps.
- `StateText.js`: the words for the monitoring state and the staleness cause.
- `StorageText.js`: the words for sizes and storage messages.
- `ThresholdText.js`: the words for thresholds and their errors.

### `app/translations/`

- `harbour-libre-monitor.ts`: the English source texts.
- `harbour-libre-monitor-it.ts`: the Italian translation.

### `cli/`

A command-line program for development (`lm-cli`, host builds only). It plays the core's side of the adapter protocol against a real adapter, with a login from a private file, and prints what comes back.

- `README.md`: how to use it.
- `cli.pro`, `cli.pri`: the project and the sources shared with its test.
- `main.cpp`: the entry point.
- `options.h`, `options.cpp`: reads the command line.
- `credentials.h`, `credentials.cpp`: reads the login from a private file (`username=` and `password=` lines) and checks its permissions.
- `runner.h`, `runner.cpp`: runs the adapter and the commands asked for.
- `output.h`, `output.cpp`: prints the results.

### `tests/`

Test programs built with Qt Test, run by `qmake && make check` in a host build. The rules that decide are tested against fakes and a clock moved by hand, so the tests do not wait and do not touch the network. Each test program sits in its own folder (`tst_name/`) with one `.cpp` and one `.pro`; the line below covers both.

- `tests.pro`: lists the test programs and builds them.

### `tests/support/`

Test helpers shared by several programs (headers only).

- `alerttest.h`: builds an alert engine with a manual clock and a short way to feed it readings.
- `modtest.h`: small helpers for tests of the store, such as temporary database files and records.
- `notifiertest.h`: a notifier that records what it was asked to post and remove.
- `schedtest.h`: a scheduler with a fake link, a fake secret store and a manual clock.
- `scriptedadapter.h`: an adapter whose replies are scripted line by line, for the driver and guard tests.
- `servicetest.h`: fakes for every port of the service life cycle.
- `silenttest.h`: helpers for the tests of silent mode.

### `tests/fixtures/`

- `store/v1/store-v1.sql`, `store/v2/store-v2.sql`: the first two versions of the store's schema, for the migration tests.
- `journal/v1/journal-v1.sql`: the first version of the journal's schema.

### `tests/`: core logic

- `tst_utctime/*`: the UTC time type: parsing, formatting, the future check.
- `tst_units/*`: the unit conversion and the rounding.
- `tst_record/*`: record validation and the registry.
- `tst_jsonl/*`: the JSON Lines codec.
- `tst_envelope/*`: the message header and the id generator.
- `tst_adapterlink/*`: the session driver: handshake, framing, limits, violations.
- `tst_guard/*`: the guard: scores, reactions, breaker, spawn allowance, per-version trip.
- `tst_scheduler/*`: the scheduler: states, errors by class, holds, staleness, the polling rhythm.
- `tst_schedulemath/*`: the scheduling arithmetic.
- `tst_tickrhythm/*`: that the rhythm of wake-ups does not change what the guard decides.
- `tst_storeadapters/*`: the two adapters between the scheduler and the store.
- `tst_store/*`: the measurement store: ingest, duplicates, quarantine, append-only triggers.
- `tst_storemigration/*`: the schema migrations on the fixtures.
- `tst_journal/*`: the journal.
- `tst_compaction/*`: compaction.
- `tst_purge/*`: the purge.
- `tst_storagecheck/*`: the weekly storage check.

### `tests/`: adapter and command line

- `tst_libreadapter/*`: the Libre adapter against a scripted transport: login, discovery, current, history, logbook, errors.
- `tst_libretime/*`: the source's timestamp format.
- `tst_libreunit/*`: the unit determination.
- `tst_cli/*`: the command-line program: arguments, secret files, exit codes, output.
- `tst_discovery/*`: discovery of the people behind a login.
- `tst_provision/*`: the provisioning mode: secret and configuration file.

### `tests/`: alerts and notifications

- `tst_alertengine/*`: the alert engine: bands, persistence, hysteresis, reminders, acknowledge, snooze.
- `tst_alertrhythm/*`: that the rhythm of the clock does not change what the engine decides.
- `tst_alertthresholds/*`: thresholds and their history.
- `tst_alertsilent/*`: the ledger of silent mode.
- `tst_alertjournal/*`: journal entries for alerts and the rebuild of state.
- `tst_alertcoordinator/*`: the coordinator between the service and the engine.
- `tst_alertnotification/*`: notifications built from alert effects.
- `tst_notificationcatalog/*`: the catalogue of notifications.
- `tst_notificationtexts/*`: the texts of notifications.
- `tst_notifierarbiter/*`: the arbiter of the long tone.
- `tst_notifiernotices/*`: the notices that stay.
- `tst_notifierreminders/*`: reminders re-posted by the notifier.
- `tst_notifiersilent/*`: silent mode in the notifier.
- `tst_donotdisturb/*`: the do-not-disturb rules.
- `tst_residentnotice/*`: the notice that stays while the service runs.

### `tests/`: service and daemon

- `tst_service/*`: the service life cycle as logic: start causes, wake-up policy, transitions.
- `tst_versionwatch/*`: the version stamp check and the self-restart guard.
- `tst_maintenance/*`: the maintenance runner.
- `tst_daemon/*`: the process side: mode parser, identity names, configuration reader, D-Bus interface, runtime.
- `tst_devfeed/*`: the development-only feed of made-up readings.
- `tst_diagnosticcollector/*`: collecting the facts of a diagnostic report.
- `tst_diagnosticreport/*`: building the report text.

### `tests/`: screens

- `tst_uilogic/*`: the direction table, number formatting, the state inventory and cover ranking.
- `tst_uimodel/*`: the list model, the plot controller and the scripted backend.
- `tst_uiplot/*`: plot geometry and thinning.
- `tst_uialerts/*`: the alert views and controllers.
- `tst_uithresholds/*`: the threshold editor and its challenge.
- `tst_uistorage/*`: the storage and purge views and controllers.
- `tst_uiaccount/*`: the account setup steps and their errors.
- `tst_uistore/*`: the configuration file and settings of the window, and that the service can read what the window writes.
- `tst_uireal/*`: the real data source of the screens: the service's status, the read-only queries, the snapshots.
- `tst_translations/*`: that every language file has every text of the English source and none is left untranslated.

### `tests/integration/`

Scripts that run the real binary. The `.py` files drive the data source simulator and the service in the SDK's build engine, or the emulator; the `.sh` files are the sessions they start. They are run by hand and need the Sailfish SDK.

- `alert_session.sh`, `run_alert_integration.py`: the service raising and clearing alerts against the simulator.
- `service_session.sh`, `run_service_integration.py`: the service life cycle against the simulator.
- `ui_alerts_session.sh`, `run_ui_alerts_integration.py`: the alert pages against a running service.
- `upgrade_session.sh`, `run_upgrade_test.py`: an upgrade while the service is running.
- `run_integration.py`: the first end-to-end run of the adapter against the simulator.
- `run_emulator_alerts.py`: alerts and notifications on the emulator.
- `run_emulator_ui_alerts.py`: the alert pages on the emulator.
- `run_emulator_ui_r3.py`, `run_emulator_ui_r4.py`, `run_emulator_ui_x1.py`, `run_emulator_ui_y1.py`: further runs of the pages on the emulator, with screenshots.
- `check_emulator_pictograms.py`: checks the notification icons on the emulator.
- `notify_monitor_summary.py`: summarises the notifications posted during a run.

### `tools/simulator/`

A stand-in for the data source, so the adapter and the service can be exercised without an account.

- `README.md`: how to start it and which scenarios exist.
- `libresim/__init__.py`, `libresim/__main__.py`: the package and its command line.
- `libresim/server.py`: the HTTP server that answers like the source.
- `libresim/scenario.py`: reads a scenario file.
- `libresim/gen.py`: generates the readings and the logbook for a scenario.
- `scenarios/basic.json`, `scenarios/bands.json`, `scenarios/gaps.json`, `scenarios/faults.json`, `scenarios/mmol.json`, `scenarios/multi.json`: a steady series, one that crosses every band, one with gaps, one with failures, one in mmol/L, and one with several people.
- `test_simulator.py`: tests of the simulator itself.

### `tools/packaging/`

- `build-release.sh`: builds the release package in the SDK's build engine.
- `emu-upgrade-test.sh`, `emu-self-replace-test.sh`, `emu-stamp-test.sh`: install, upgrade and version-stamp checks on the emulator.
- `emu-addon-test.sh`: install and removal of the add-on on the emulator.
- `emu-compaction-test.sh`, `emu-purge-test.sh`: compaction and purge on a store with a large history, on the emulator.

### `design/`

The sources and the exported images. The exports are binary or generated, so they are described as groups.

- `MANIFEST.md`: which drawing is exported to which file, at which size.
- `build.py`: exports the drawings in `src/` to the sizes in `export/`.
- `src/*.svg`: the drawings, one per image (notification icons, badges, sensor, states, the launcher icon).
- `export/launcher/*/harbour-libre-monitor.png`: the launcher icon at 86, 108, 128 and 172 pixels.
- `export/notify/*.png`: the notification icons, by band and by notice.
- `export/ui/*.svg`, `export/ui/*.png`: the band badges, the sensor and the state marks used by the pages.

### `rpm/`

- `harbour-libre-monitor.spec`: the package description of the application.
- `harbour-libre-monitor.changes.in`, `harbour-libre-monitor.changes.run.in`: templates of the change log of a package.

### `addon/`

The optional add-on package, a systemd user timer that starts the service after a reboot. It contains no code.

- `libre-monitor-autostart.service`, `libre-monitor-autostart.timer`: the unit that calls the service's D-Bus name 20 seconds after login and then every 60 seconds.
- `rpm/libre-monitor-autostart.spec`: the package description of the add-on.

## How a request travels through the code

Five examples, each followed from where it starts to where it ends.

**A new reading arrives.** The service wakes up (`core/service/servicelifecycle.cpp`, driven by the timers in `daemon/serviceruntime.cpp`) and ticks the scheduler (`core/scheduler/scheduler.cpp`). The scheduler sees that the current feed of a person is due (`schedulemath.cpp`), checks with the guard that the connection is allowed to run (`core/guard/guard.cpp`), and asks the session manager (`connectionsession.cpp`) for a logged-in adapter. The session manager reads the login from the secret store and starts the adapter process through `core/session/adapterlink.cpp`; the adapter (`adapters/libre/libreadapter.cpp`) calls the source through `libreclient.cpp` and `qthttptransport.cpp` and turns the reply into records with `libresource.cpp`. Each line coming back is checked by `adapterlink.cpp` (framing, limits, `core/record/record.cpp`). The scheduler keeps only readings newer than the last stored one and hands them to the store through `storereadingstore.cpp`, where `core/store/store.cpp` validates them again, writes them, or quarantines them. The store reports the change, `core/service/serviceevents.cpp` tells the alert coordinator (`core/alerts/alertcoordinator.cpp`) and the D-Bus object (`daemon/dbusservice.cpp`), which emits `DataChanged`. In the window `app/real/servicelink.cpp` receives the signal and `app/real/realbackend.cpp` rebuilds the snapshot with `storequeries.cpp`; `app/ui/subjectmodel.cpp` updates the rows that `OverviewPage.qml` shows.

**The reading is out of range.** Along the same path the alert coordinator reads the newest reading, gives it to the engine (`alertengine.cpp`), which uses `band.cpp` and the thresholds from `thresholdbook.cpp` and answers with effects. `alertcoordinator.cpp` writes the journal entry (`alertjournal.cpp`, `core/store/journal.cpp`) and hands the notification, built by `core/service/alertnotification.cpp` with the texts from `notificationtexts.cpp`, to the notifier, which on the phone is in `daemon/phoneports.cpp`.

**The user presses Acknowledge.** The button is on `AlertsPage.qml`; it calls the alert backend (`app/ui/alertsbackend.h`, implemented by `app/real/realalertsbackend.cpp`), which calls the service over D-Bus through `app/real/servicelink.cpp`. `daemon/dbusservice.cpp` receives the call and passes it to `core/alerts/alertcoordinator.cpp`, which gives it to the engine. The engine moves the slot to acknowledged and stops the reminders; the coordinator writes the journal entry and removes the notification. Deleting the notification from the phone's own list takes the same path from the notifier.

**The user starts a purge.** On `PurgePage.qml` the user picks a person and an age and confirms. `app/ui/purgecontroller.cpp` asks the maintenance backend (`app/real/realmaintenancebackend.cpp`), which calls the service over D-Bus. In the service `core/service/maintenancerunner.cpp` runs the job on its own thread with `daemon/maintenancejobs.cpp`, which calls `core/store/purge.cpp`. The purge deletes the person's old readings from the store and the person's alert, threshold and silent entries from the journal, in a way that the append-only triggers allow only during a purge, and leaves no record of it. Progress comes back through `core/store/progress.cpp` and the runner's signals, and `purgeview.cpp` shows it.

**The service starts, and the package is upgraded.** The system activates the D-Bus name; `app/src/main.cpp` sees `--service`, and `daemon/serviceprocess.cpp` builds the pipeline with `servicecore.cpp`. `core/service/startcause.cpp` works out why it started and `servicelifecycle.cpp` writes the start entry. At each heartbeat `core/service/versionwatch.cpp` compares the stamp left by the package with the version of the running binary; on a difference the service stops with the reason `upgraded` and the next D-Bus call starts the new binary.
