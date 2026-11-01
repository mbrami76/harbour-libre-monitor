#!/bin/sh
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Michele Brami
#
# The part of the upgrade test that happens INSIDE the Sailfish SDK
# build engine. run_upgrade_test.py calls it once per phase through `sfdk build-shell`; between the
# phases the Python side copies the data directory and compares the databases, which is why every
# phase starts and ends its own private D-Bus daemon instead of keeping one for the whole run.
#
#   upgrade_session.sh PHASE BUILD_DIR WORK_DIR DATA_DIR SIMULATOR_URL
#
# PHASE is one of
#   fill      run the service of BUILD_DIR until it has polled, then stop it in order (the old
#             binary writing the store, the journal and the settings the way the first release did)
#   upgrade   start the service of BUILD_DIR on a data directory it did not write, wait for the first
#             poll and for an alert raised by the readings, stop it, start it again (the second
#             restart), and print what the second start restored
#   attempt   start the service of BUILD_DIR, wait for a poll, stop it: used for the downgrade (the old
#             binary on the migrated data) and for the upgrade again afterwards
#
# The service is started by D-Bus activation, as on the phone. Every account is the simulator's fake
# one (WORK_DIR/cred); the real source is never contacted. The output is marked sections that the
# Python side reads; it carries states, times and counts, never a glucose value (the D-Bus answers
# hold none) and never a name.

PHASE=$1
BUILD=$2
WORK=$3
DATA=$4
URL=$5
SUFFIX=-up
NAME=it.michelebrami.libre-monitor-host$SUFFIX
PATH_=/it/michelebrami/libre_monitor_host_up
IFACE=it.michelebrami.libre_monitor.Service
SVC=$BUILD/daemon/host/lm-service
ADAPTER=$BUILD/adapters/libre/host/libre-adapter-host
BUS=$WORK/bus-$PHASE-$$

mkdir -p "$BUS/services"
cat > "$BUS/dbus.conf" <<CONF
<busconfig>
  <type>session</type>
  <listen>unix:path=$BUS/bus.sock</listen>
  <servicedir>$BUS/services</servicedir>
  <policy context="default">
    <allow send_destination="*" eavesdrop="true"/>
    <allow eavesdrop="true"/>
    <allow own="*"/>
  </policy>
</busconfig>
CONF
# A service started by D-Bus activation loses its standard error, so a wrapper appends it to a file
# of this phase; the Python side only counts its lines and looks for the words of a failure.
cat > "$BUS/run_service.sh" <<WRAP
#!/bin/sh
exec $SVC --data-dir $DATA --adapter-program $ADAPTER --base-url $URL --credentials-file $WORK/cred --wake-frequencies 1,2,5 --heartbeat-s 5 --name-suffix $SUFFIX 2>> $WORK/svc-$PHASE.err
WRAP
chmod +x "$BUS/run_service.sh"
cat > "$BUS/services/$NAME.service" <<SERVICE
[D-BUS Service]
Name=$NAME
Exec=$BUS/run_service.sh
SERVICE

dbus-daemon --config-file="$BUS/dbus.conf" --fork --print-pid < /dev/null > "$BUS/bus.pid" 2> "$BUS/bus.err"
export DBUS_SESSION_BUS_ADDRESS=unix:path=$BUS/bus.sock

call() { method=$1; shift; dbus-send --session --print-reply=literal --dest=$NAME $PATH_ $IFACE.$method "$@" 2>&1; echo; }
pid_of_service() {
  dbus-send --session --print-reply --dest=org.freedesktop.DBus /org/freedesktop/DBus \
    org.freedesktop.DBus.GetConnectionUnixProcessID string:$NAME 2>/dev/null | awk '/uint32/ {print $2}'
}
wait_for() { # wait_for SECONDS COMMAND...
  n=$1; shift
  while [ "$n" -gt 0 ]; do "$@" >/dev/null 2>&1 && return 0; sleep 1; n=$((n - 1)); done
  return 1
}
polled() { call Status | grep -q '"last_success_at":"2'; }
alert_active() { call Alerts | grep -q '"state":"active"'; }
gone() { [ -z "$(pid_of_service)" ]; }
running() { [ -n "$(pid_of_service)" ]; }

# Starts the service by activation and waits for its first successful poll; prints START and POLLED.
start_and_poll() {
  call Status > /dev/null
  if wait_for 30 running; then echo "START ok"; else echo "START failed"; fi
  if wait_for 90 polled; then echo "POLLED ok"; else echo "POLLED never"; fi
}
# The orderly stop the UI asks for (reason `requested`), then waits for the process to end.
stop_service() {
  call Stop > /dev/null
  if wait_for 30 gone; then echo "STOPPED ok"; else echo "STOPPED still running"; kill -TERM "$(pid_of_service)" 2>/dev/null; wait_for 10 gone; fi
}
journal_dump() { "$SVC" --data-dir "$DATA" --dump-journal 2>&1; }

case "$PHASE" in
fill)
  echo "=== fill"
  start_and_poll
  # The first poll fetches the current value; the graph and the logbook follow within seconds.
  sleep 25
  echo "STATUS $(call Status | tr -d '\n')"
  stop_service
  ;;
upgrade)
  echo "=== upgrade first start"
  start_and_poll
  echo "STATUS $(call Status | tr -d '\n')"
  if wait_for 240 alert_active; then echo "ALERT ok"; else echo "ALERT none"; fi
  echo "ALERTS $(call Alerts | tr -d '\n')"
  stop_service
  echo "=== upgrade second start"
  start_and_poll
  sleep 5
  echo "ALERTS $(call Alerts | tr -d '\n')"
  echo "STATUS $(call Status | tr -d '\n')"
  stop_service
  echo "=== journal"
  journal_dump
  ;;
attempt)
  echo "=== attempt"
  start_and_poll
  echo "STATUS $(call Status | tr -d '\n')"
  echo "ALERTS $(call Alerts | tr -d '\n')"
  stop_service
  echo "=== journal"
  journal_dump
  ;;
*)
  echo "unknown phase $PHASE"
  ;;
esac

kill "$(cat "$BUS/bus.pid")" 2>/dev/null
echo "=== end"
