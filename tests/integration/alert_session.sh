#!/bin/sh
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Michele Brami
#
# The part of the alert integration run that happens INSIDE the Sailfish SDK
# build engine. Started by run_alert_integration.py through `sfdk build-shell`. It starts the host
# service by D-Bus activation against the simulator, lets the simulated Subject go through its
# excursions in real time, kills the service with kill -9 while an urgent high alert is active,
# lets D-Bus start it again, and prints marked sections that the Python side checks.
#
#   alert_session.sh BUILD_DIR WORK_DIR SIMULATOR_URL KILL_EPOCH END_EPOCH ALERT_SCALE SUBJECT SET_EPOCH ACT_EPOCH
#
# SUBJECT is the Subject's key in the configuration. At SET_EPOCH the script edits the Subject's L
# threshold through the D-Bus method (alerts in the service); at ACT_EPOCH, while the
# missing-data alert is active, it acknowledges and snoozes it. A dbus-monitor records the signal
# AlertChanged all along.
#
# WORK_DIR must hold data/config.json and cred (the simulator's fake account, mode 0600).
# Everything here is synthetic; the real source is never contacted.

BUILD=$1
WORK=$2
URL=$3
KILL_AT=$4
END_AT=$5
SCALE=$6
SUBJ=$7
SET_AT=$8
ACT_AT=$9
SUFFIX=-al
NAME=it.michelebrami.libre-monitor-host$SUFFIX
PATH_=/it/michelebrami/libre_monitor_host_al
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
# A service started by D-Bus activation does not keep its standard error, so the activation file runs
# a small wrapper that appends it to a file of our own.
cat > "$WORK/run_service.sh" <<WRAP
#!/bin/sh
exec $SVC --data-dir $DATA --adapter-program $ADAPTER --base-url $URL --credentials-file $WORK/cred --wake-frequencies 1,2,5 --heartbeat-s 5 --alert-scale $SCALE --name-suffix $SUFFIX 2>> $WORK/svc.err
WRAP
chmod +x "$WORK/run_service.sh"
cat > "$WORK/services/$NAME.service" <<SERVICE
[D-BUS Service]
Name=$NAME
Exec=$WORK/run_service.sh
SERVICE

dbus-daemon --config-file="$WORK/dbus.conf" --fork --print-pid < /dev/null > "$WORK/bus.pid" 2> "$WORK/bus.err"
export DBUS_SESSION_BUS_ADDRESS=unix:path=$WORK/bus.sock

call() { method=$1; shift; dbus-send --session --print-reply=literal --dest=$NAME $PATH_ $IFACE.$method "$@" 2>&1; echo; }
pid_of_service() {
  dbus-send --session --print-reply --dest=org.freedesktop.DBus /org/freedesktop/DBus \
    org.freedesktop.DBus.GetConnectionUnixProcessID string:$NAME 2>/dev/null | awk '/uint32/ {print $2}'
}
wait_for() { n=$1; shift; while [ "$n" -gt 0 ]; do "$@" >/dev/null 2>&1 && return 0; sleep 1; n=$((n - 1)); done; return 1; }
gone() { [ -z "$(pid_of_service)" ]; }
polled() { call Status | grep -q '"last_success_at":"2'; }
sleep_until() { while [ "$(date +%s)" -lt "$1" ]; do sleep 2; done; }
# One answer of the service on one line, labelled, for the Python side to read.
show() { label=$1; shift; echo "ANS $label $(call "$@" | tr -d '\n' | sed 's/^ *//')"; }
# The signal AlertChanged, as the bus carried it (the UI would hear the same).
if command -v dbus-monitor >/dev/null 2>&1; then
  dbus-monitor --session "type='signal',interface='$IFACE',member='AlertChanged'" > "$WORK/signals.txt" 2>/dev/null &
  MONITOR_PID=$!
  echo "$MONITOR_PID" > "$WORK/monitor.pid"
fi

echo "=== start"
call Status > "$WORK/status0.json"
if wait_for 60 polled; then echo "POLLED ok"; else echo "POLLED never"; fi
PID1=$(pid_of_service)
echo "PID1 ${PID1:-none}"

# The caregiver edits L before the excursions: a value above the standard 70 needs no challenge; a
# value below it is refused for want of the value typed again, and the answer says what was needed.
sleep_until "$SET_AT"
echo "=== thresholds"
show set_l_71 SetThresholds string:"$SUBJ" int32:71 int32:180 int32:-1 int32:-1
show set_l_65_refused SetThresholds string:"$SUBJ" int32:65 int32:180 int32:-1 int32:-1
show set_unknown_subject SetThresholds string:"{00000000-0000-4000-8000-000000000009}" int32:71 int32:180 int32:-1 int32:-1
show alerts_after_edit Alerts

sleep_until "$KILL_AT"
echo "=== kill"
echo "ERR_LINES_BEFORE_KILL $(wc -l < "$WORK/svc.err")"
"$SVC" --data-dir "$DATA" --dump-alerts > "$WORK/alerts_before_kill.txt"
echo "ALERT_ENTRIES_BEFORE_KILL $(wc -l < "$WORK/alerts_before_kill.txt")"
kill -9 "$PID1"
if wait_for 10 gone; then echo "KILLED ok"; else echo "KILLED still running"; fi
echo "KILLED_AT $(date +%s)"
call Status > "$WORK/status1.json"
if wait_for 60 polled; then echo "RESTARTED ok"; else echo "RESTARTED never"; fi
PID2=$(pid_of_service)
echo "PID2 ${PID2:-none}"
echo "ERR_LINES_AFTER_RESTART $(wc -l < "$WORK/svc.err")"
echo "=== restored"
show alerts_after_restart Alerts

# While the missing-data alert is active: acknowledge it, then snooze it, as the notification's
# button and the UI would. Waits (up to 3 minutes) for the alert to be listed.
sleep_until "$ACT_AT"
echo "=== actions"
n=180
while [ "$n" -gt 0 ]; do call Alerts | grep -q '"slot":"missing"' && break; sleep 1; n=$((n - 1)); done
show alerts_missing_active Alerts
show ack_missing AcknowledgeAlert string:"$SUBJ" string:missing
show ack_missing_again AcknowledgeAlert string:"$SUBJ" string:missing
show ack_bad_slot AcknowledgeAlert string:"$SUBJ" string:sideways
show alerts_missing_acknowledged Alerts
show snooze_bad_duration SnoozeAlert string:"$SUBJ" string:missing int32:7
show snooze_missing SnoozeAlert string:"$SUBJ" string:missing int32:15
show alerts_missing_snoozed Alerts

sleep_until "$END_AT"
echo "=== term"
kill -TERM "$PID2"
if wait_for 20 gone; then echo "TERM_EXIT ok"; else echo "TERM_EXIT still running"; fi

[ -f "$WORK/monitor.pid" ] && kill "$(cat "$WORK/monitor.pid")" 2>/dev/null
echo "=== signals"
[ -f "$WORK/signals.txt" ] && cat "$WORK/signals.txt" || echo "NO_MONITOR"
echo "=== alerts"
"$SVC" --data-dir "$DATA" --dump-alerts
echo "=== alert lines"
grep '^alert:' "$WORK/svc.err"
echo "=== other stderr"
grep -v '^alert:' "$WORK/svc.err" | head -40
kill "$(cat "$WORK/bus.pid")" 2>/dev/null
echo "=== end"
