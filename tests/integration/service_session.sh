#!/bin/sh
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Michele Brami
#
# The part of the service integration run that happens INSIDE the Sailfish
# SDK build engine, where the host build and a D-Bus daemon live. Started by
# run_service_integration.py through `sfdk build-shell`; it prints marked sections that the
# Python side checks. It removes nothing: the work directory belongs to the caller.
#
#   service_session.sh BUILD_DIR WORK_DIR SIMULATOR_URL
#
# WORK_DIR must hold: data/config.json (the connections), cred (the simulator's fake account,
# mode 0600) and bad (the same account with a wrong password, mode 0600). Everything here is
# synthetic; the real source is never contacted.

BUILD=$1
WORK=$2
URL=$3
SUFFIX=-it
NAME=it.michelebrami.libre-monitor-host$SUFFIX
PATH_=/it/michelebrami/libre_monitor_host_it
IFACE=it.michelebrami.libre_monitor.Service
SVC=$BUILD/daemon/host/lm-service
ADAPTER=$BUILD/adapters/libre/host/libre-adapter-host
DATA=$WORK/data

mkdir -p "$WORK/services"
cat > "$WORK/dbus.conf" <<CONF
<busconfig>
  <type>session</type>
  <listen>unix:path=$WORK/bus.sock</listen>
  <servicedir>$WORK/services</servicedir>
  <policy context="default">
    <allow send_destination="*" eavesdrop="true"/>
    <allow eavesdrop="true"/>
    <allow own="*"/>
  </policy>
</busconfig>
CONF

# D-Bus activation file: what the bus runs when someone calls the name (REQ-ARC-07).
cat > "$WORK/services/$NAME.service" <<SERVICE
[D-BUS Service]
Name=$NAME
Exec=$SVC --data-dir $DATA --adapter-program $ADAPTER --base-url $URL --credentials-file $WORK/cred --probe-credentials-file $WORK/probe_cred --wake-frequencies 1,2,5 --heartbeat-s 2 --name-suffix $SUFFIX
SERVICE

# stdin, stdout and stderr of the forked daemon (and of every service it activates) go to
# files: otherwise the caller waits for pipes that never close.
dbus-daemon --config-file="$WORK/dbus.conf" --fork --print-pid < /dev/null > "$WORK/bus.pid" 2> "$WORK/bus.err"
export DBUS_SESSION_BUS_ADDRESS=unix:path=$WORK/bus.sock

call() { method=$1; shift; dbus-send --session --print-reply=literal --dest=$NAME $PATH_ $IFACE.$method "$@" 2>&1; echo; }
pid_of_service() {
  dbus-send --session --print-reply --dest=org.freedesktop.DBus /org/freedesktop/DBus \
    org.freedesktop.DBus.GetConnectionUnixProcessID string:$NAME 2>/dev/null | awk '/uint32/ {print $2}'
}
dump() { "$SVC" --data-dir "$DATA" --dump-journal; }
count() { dump | wc -l; }
wait_for() { # wait_for SECONDS COMMAND...
  n=$1; shift
  while [ "$n" -gt 0 ]; do "$@" >/dev/null 2>&1 && return 0; sleep 1; n=$((n - 1)); done
  return 1
}
journal_has() { dump | grep -q "$1"; }
polled() { call Status | grep -q '"last_success_at":"2'; }
gone() { [ -z "$(pid_of_service)" ]; }

echo "=== activation"
call Status > "$WORK/status0.json"       # any call to the name starts the service
if wait_for 20 journal_has '"kind":"start"'; then echo "START_RECORD ok"; else echo "START_RECORD missing"; fi
dump
PID1=$(pid_of_service)
echo "PID1 ${PID1:-none}"

echo "=== polling"
if wait_for 60 polled; then echo "POLLED ok"; else echo "POLLED never"; fi
call Status
echo "=== heartbeats"
sleep 6
dump

# Discover. The file $WORK/probe_cred stands in for the secret that the set-up page stores
# under the name `probe`; the service deletes it when it takes the secret out. A discovery is a
# delayed reply, so dbus-send waits for it (up to 70 s).
discover() { dbus-send --session --print-reply=literal --reply-timeout=70000 --dest=$NAME $PATH_ $IFACE.Discover string:"$1" 2>&1; echo; }
probe_file() { [ -e "$WORK/probe_cred" ] && echo present || echo gone; }

echo "=== discover good probe"
cp "$WORK/cred" "$WORK/probe_cred"; chmod 600 "$WORK/probe_cred"
echo "PROBE_FILE_BEFORE $(probe_file)"
discover probe
echo "PROBE_FILE_AFTER $(probe_file)"

echo "=== discover too soon"
cp "$WORK/bad" "$WORK/probe_cred"; chmod 600 "$WORK/probe_cred"
discover probe
echo "PROBE_FILE_AFTER $(probe_file)"

echo "=== discover wrong login"
sleep 31
cp "$WORK/bad" "$WORK/probe_cred"; chmod 600 "$WORK/probe_cred"
discover probe
echo "PROBE_FILE_AFTER $(probe_file)"

echo "=== discover known connection"
discover c1

echo "=== discover unknown"
discover nobody

echo "=== second instance"
BEFORE=$(count)
"$SVC" --data-dir "$DATA" --adapter-program "$ADAPTER" --base-url "$URL" --credentials-file "$WORK/cred" --name-suffix $SUFFIX 2> "$WORK/second.err"
echo "SECOND_RC $?"
AFTER=$(count)
echo "SECOND_ENTRIES before=$BEFORE after_ge=$([ "$AFTER" -ge "$BEFORE" ] && echo yes || echo no)"
# The second instance wrote nothing of its own: no second start record appeared.
echo "SECOND_STARTS $(dump | grep -c '"kind":"start"')"

echo "=== kill"
kill -9 "$PID1"
if wait_for 10 gone; then echo "KILLED ok"; else echo "KILLED still running"; fi
echo "=== restart after kill"
call Status > "$WORK/status1.json"
if wait_for 20 journal_has 'after-missing-stop'; then echo "RESTART_CAUSE ok"; else echo "RESTART_CAUSE missing"; fi
dump
PID2=$(pid_of_service)
echo "PID2 ${PID2:-none}"

echo "=== sigterm"
kill -TERM "$PID2"
if wait_for 20 gone; then echo "TERM_EXIT ok"; else echo "TERM_EXIT still running"; fi
dump

kill "$(cat "$WORK/bus.pid")" 2>/dev/null
echo "=== end"
