#!/bin/bash
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Michele Brami
#
# Add-on test (REQ-PKG-03 to 06) on the EMULATOR only (never the phone).
#
#   tools/packaging/emu-addon-test.sh MAIN.rpm ADDON.rpm       (MAIN: i486 main package)
#
#   1 install the main package (with the user's Allow) and the add-on, restart the emulator
#   2 after the restart: is the timer active, did it start the service, how long after the session?
#   3 kill -9 of the service: how many seconds until the timer starts it again?
#   4 the sandbox's view: does the application's sandbox see the add-on's timer file (QFile::exists)?
#   5 remove the add-on: does the timer stop, is a killed service still started again?
#
# Safety as in emu-upgrade-test.sh: audio output off and sinks muted (checked again after the
# restart), no configuration of the release identity (no connection, nothing contacts the source).
set -u
MAIN=${1:?usage: emu-addon-test.sh MAIN.rpm ADDON.rpm}
ADDON=${2:?usage: emu-addon-test.sh MAIN.rpm ADDON.rpm}
VM=SailfishOS-5.1.0.11
KEY=$HOME/SailfishOS/vmshare/ssh/private_keys/sdk
OPTS="-o ConnectTimeout=10 -o LogLevel=ERROR -o StrictHostKeyChecking=no -o UserKnownHostsFile=/dev/null -i $KEY"
R() { ssh $OPTS -p 2223 root@localhost "$1"; }
U() { ssh $OPTS -p 2223 defaultuser@localhost "$1"; }
TIMER=/usr/lib/systemd/user/libre-monitor-autostart.timer
SVC="pgrep -f '^/usr/bin/harbour-libre-monitor --service'"
fail=0
verdict() { if [ "$2" = ok ]; then echo "PASS  $1"; else echo "FAIL  $1"; fail=$((fail+1)); fi; }
safety() {
  VBoxManage controlvm $VM audioout off 2>/dev/null
  VBoxManage showvminfo $VM --machinereadable | grep -q '^audio_out="off"' || { echo "audio output not off, stop"; exit 2; }
  U 'for n in $(pactl list short sinks | cut -f1); do pactl set-sink-mute $n 1; done; echo "muted sinks: $(pactl list sinks | grep -c "Mute: yes") of $(pactl list short sinks | wc -l)"'
  U '[ ! -e ~/.config/it.michelebrami/libre-monitor/config.json ]' || { echo "release configuration present, stop"; exit 2; }
}
# Seconds until a service process appears (at most $1 s), "none" when it does not.
waitsvc() { for i in $(seq 1 $1); do [ -n "$(R "$SVC")" ] && { echo $i; return; }; sleep 1; done; echo none; }

echo "== safety"; safety
scp -q $OPTS -P 2223 "$MAIN" root@localhost:/tmp/lm-main.rpm && scp -q $OPTS -P 2223 "$ADDON" root@localhost:/tmp/lm-addon.rpm || exit 2

echo "== 1 install main and add-on, restart"
R "rpm -q harbour-libre-monitor >/dev/null || rpm -i /tmp/lm-main.rpm; rpm -q libre-monitor-autostart >/dev/null && rpm -e libre-monitor-autostart; rpm -i /tmp/lm-addon.rpm; rpm -q harbour-libre-monitor libre-monitor-autostart
   S='--system --print-reply --dest=org.sailfishos.sailjaild1 /org/sailfishos/sailjaild1 org.sailfishos.sailjaild1'
   dbus-send \$S.SetGrantedPermissions uint32:100000 string:harbour-libre-monitor array:string:Internet,Secrets,Documents >/dev/null
   dbus-send \$S.SetLaunchAllowed uint32:100000 string:harbour-libre-monitor int32:1 >/dev/null
   ls -l $TIMER /usr/lib/systemd/user/user-session.target.wants/libre-monitor-autostart.timer"
R 'sync; (sleep 1; reboot) >/dev/null 2>&1 &'
sleep 20
for i in $(seq 1 30); do R true 2>/dev/null && break; sleep 5; done
sleep 5; echo "== safety after the restart"; safety

echo "== 2 after the restart"
up=$(R "cut -d' ' -f1 /proc/uptime")
echo "uptime ${up} s; timer: $(U "systemctl --user is-active libre-monitor-autostart.timer") / $(U "systemctl --user is-enabled libre-monitor-autostart.timer 2>&1")"
s=$(waitsvc 90); echo "service process after: $s s more"
U "systemctl --user list-timers --all 2>/dev/null | grep libre-monitor"
U "journalctl --user -u libre-monitor-autostart.service -o short-monotonic --no-pager 2>/dev/null | tail -4"
[ "$s" != none ] && verdict "2 the timer starts the service after the session starts" ok || verdict "2 the timer starts the service after the session starts" no

echo "== 3 crash: kill -9"
R "kill -9 \$($SVC)"; sleep 1
s=$(waitsvc 75); echo "service again after: $s s"
[ "$s" != none ] && verdict "3 a killed service is started again within one period (60 s)" ok || verdict "3 a killed service is started again within one period (60 s)" no

echo "== 4 the sandbox's view of the add-on's file"
P=$(R "$SVC" | head -1)
R "ls -l /proc/$P/root$TIMER && echo visible-in-sandbox; grep -E ' /usr ' /proc/$P/mountinfo | head -1"
R "[ -e /proc/$P/root$TIMER ]" && verdict "4 the application's sandbox sees $TIMER" ok || verdict "4 the application's sandbox sees $TIMER" no

echo "== 5 remove the add-on"
R "rpm -e libre-monitor-autostart; rpm -q libre-monitor-autostart; ls $TIMER 2>&1"
sleep 2; echo "timer: $(U "systemctl --user is-active libre-monitor-autostart.timer")"
R "[ -e /proc/$P/root$TIMER ]" && echo "sandbox still sees the file" || echo "sandbox no longer sees the file"
R "kill -9 \$($SVC)"; sleep 1
s=$(waitsvc 75); echo "service again after: $s s"
U "journalctl --user -u libre-monitor-autostart.service -o short-monotonic --no-pager 2>/dev/null | tail -3"
[ "$s" = none ] && verdict "5 after removal the add-on no longer starts the service" ok || verdict "5 after removal the add-on no longer starts the service" no
U "systemctl --user daemon-reload; systemctl --user is-active libre-monitor-autostart.timer"
R "pkill -f '/usr/bin/harbour-libre-monitor --(service|adapter)'; rm -f /tmp/lm-main.rpm /tmp/lm-addon.rpm"
echo "FAILS=$fail"
echo TEST_DONE
