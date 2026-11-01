#!/bin/sh
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Michele Brami
#
# The part of the interface's alert integration run that happens INSIDE the
# Sailfish SDK build engine. Started by run_ui_alerts_integration.py through `sfdk build-shell`. It
# starts a private session bus whose activation file runs the HOST service (daemon/host/lm-service)
# against the simulator, as alert_session.sh does, then runs the interface's live test
# (tst_uireal liveHostServiceWithTheSimulator) with the service's name, and prints marked sections.
#
#   ui_alerts_session.sh BUILD_DIR WORK_DIR SIMULATOR_URL SUBJECT WAIT_S [FUNCTIONS]
#
# FUNCTIONS are the tst_uireal functions to run, separated by spaces; the default is the alert
# run of R2 (liveHostServiceWithTheSimulator); liveThresholdsAndSettingsWithTheHostService is the R3 run.
#
# WORK_DIR must hold data/config.json and cred (the simulator's fake account, mode 0600). Everything is
# synthetic; the real source is never contacted.

BUILD=$1
WORK=$2
URL=$3
SUBJ=$4
WAIT_S=$5
FUNCS=${6:-liveHostServiceWithTheSimulator}
SUFFIX=-ui
NAME=it.michelebrami.libre-monitor-host$SUFFIX
PATH_=/it/michelebrami/libre_monitor_host_ui
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
cat > "$WORK/run_service.sh" <<WRAP
#!/bin/sh
exec $SVC --data-dir $DATA --adapter-program $ADAPTER --base-url $URL --credentials-file $WORK/cred --wake-frequencies 1,2,5 --heartbeat-s 5 --name-suffix $SUFFIX 2>> $WORK/svc.err
WRAP
chmod +x "$WORK/run_service.sh"
cat > "$WORK/services/$NAME.service" <<SERVICE
[D-BUS Service]
Name=$NAME
Exec=$WORK/run_service.sh
SERVICE

dbus-daemon --config-file="$WORK/dbus.conf" --fork --print-pid < /dev/null > "$WORK/bus.pid" 2> "$WORK/bus.err"
export DBUS_SESSION_BUS_ADDRESS=unix:path=$WORK/bus.sock

echo "=== start"
dbus-send --session --print-reply=literal --dest=$NAME $PATH_ it.michelebrami.libre_monitor.Service.Start 2>&1 | head -2; echo

echo "=== live"
cd "$BUILD/tests/tst_uireal" || exit 1
# QtTest ends a test function after 5 minutes by default; this one waits for a real excursion.
export QTEST_FUNCTION_TIMEOUT=$(( (WAIT_S + 600) * 1000 ))
LM_UI_LIVE_NAME=$NAME LM_UI_LIVE_PATH=$PATH_ LM_UI_LIVE_SUBJECT=$SUBJ LM_UI_LIVE_WAIT_S=$WAIT_S LM_UI_LIVE_DATA=$DATA \
  ./tst_uireal $FUNCS 2>&1

echo "=== stop"
dbus-send --session --print-reply=literal --dest=$NAME $PATH_ it.michelebrami.libre_monitor.Service.Stop >/dev/null 2>&1
sleep 3
echo "=== service stderr (alert lines only counted)"
echo "ALERT_LINES $(grep -c '^alert:' "$WORK/svc.err")"
grep -v '^alert:' "$WORK/svc.err" | head -20
kill "$(cat "$WORK/bus.pid")" 2>/dev/null
echo "=== end"
