#!/bin/bash
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Michele Brami
#
# test on the EMULATOR only (never the phone): what happens to a running monitoring
# service when the main package is upgraded, removed and installed again.
#
#   tools/packaging/emu-upgrade-test.sh A.rpm B.rpm      (i486 packages; A older than B)
#
#   1 install A, start the service (Service.Start), note its process
#   2 upgrade to B (rpm -U): is the running process B's binary, does Alerts answer?
#   3 Service.Stop, then Service.Start: does that give B's process (what a fix would do)?
#   4 remove B (rpm -e): is a process left, is the user's data still there?
#   5 install B again (rpm -i) and start: is the process B's, does Alerts answer?
# Each step prints the facts and a PASS or FAIL line for the rule. A process "is B's
# binary" when it was started after B was installed and its mapping of /usr/bin/harbour-libre-monitor
# is not marked "(deleted)" (rpm replaces the file, so an older process maps the removed one).
#
# Safety: the VirtualBox audio output must be off and every sink muted (checked, else the test stops);
# the release identity must have no configuration file (no connection, so nothing contacts the real
# source); the development package (harbour-libre-monitor-dev) is not touched. No answer text is
# printed, only whether a method answered and its length.
set -u
A=${1:?usage: emu-upgrade-test.sh A.rpm B.rpm}
B=${2:?usage: emu-upgrade-test.sh A.rpm B.rpm}
VM=SailfishOS-5.1.0.11
KEY=$HOME/SailfishOS/vmshare/ssh/private_keys/sdk
OPTS="-o ConnectTimeout=15 -o LogLevel=ERROR -o StrictHostKeyChecking=no -o UserKnownHostsFile=/dev/null -i $KEY"
R() { ssh $OPTS -p 2223 root@localhost "$1"; }
U() { ssh $OPTS -p 2223 defaultuser@localhost "$1"; }
DEST='--dest=it.michelebrami.libre-monitor /it/michelebrami/libre_monitor it.michelebrami.libre_monitor.Service'
PROCS="for p in \$(pgrep -f '^(/usr/bin/invoker .*)?/usr/bin/harbour-libre-monitor --(service|adapter)'); do
  [ -r /proc/\$p/stat ] || continue
  m=\$(grep -m1 ' /usr/bin/harbour-libre-monitor' /proc/\$p/maps 2>/dev/null | sed 's/.* \\//\\//')
  echo \"pid=\$p start=\$(awk '{print \$22}' /proc/\$p/stat) exe=\$(readlink /proc/\$p/exe) map=[\$m] args=\$(tr '\\0' ' ' < /proc/\$p/cmdline)\"
done"
SJ="awk '/^\\[harbour-libre-monitor\\]/{p=1;next} /^\\[/{p=0} p&&/^(Allowed|Granted)=/' /home/.system/var/lib/sailjail/settings/user-100000.settings | tr '\\n' ' '"
DATA='ls -l ~/.local/share/it.michelebrami/libre-monitor/ | awk "/\\.db\$/{print \$NF\"=\"\$5}" | tr "\n" " "'
NOW="awk '{print int(\$1*100)}' /proc/uptime"   # in clock ticks (USER_HZ 100), as field 22 of stat

fail=0
verdict() { if [ "$2" = ok ]; then echo "PASS  $1"; else echo "FAIL  $1"; fail=$((fail+1)); fi; }
call() { U "dbus-send --session --print-reply --reply-timeout=20000 $DEST.$1 > /tmp/lm-call.out 2>&1; echo rc=\$? bytes=\$(wc -c < /tmp/lm-call.out); rm -f /tmp/lm-call.out"; }
# A process of B: started after B was installed (tick) and mapping a binary that is not deleted.
newproc() { R "$PROCS" | awk -v t="$1" '/--service/ { split($2,a,"="); if (a[2] > t && $0 ~ /map=\[\// && $0 !~ /\(deleted\)/) n++ } END { print (n>0) ? "ok" : "no" }'; }
anyproc() { R "$PROCS" | grep -c 'pid='; }
# The user's "Allow" at the first start (the Sailjail permission dialog), given over sailjaild's system
# bus interface as root, because the headless emulator shows no dialog: the permissions of the desktop
# file are granted and the launch is allowed (Allowed=1, "always"). Printed before and after.
grant() {
  echo "sailjail before: $(R "$SJ")"
  R "S='--system --print-reply --dest=org.sailfishos.sailjaild1 /org/sailfishos/sailjaild1 org.sailfishos.sailjaild1'
     dbus-send \$S.SetGrantedPermissions uint32:100000 string:harbour-libre-monitor array:string:Internet,Secrets,Documents >/dev/null
     dbus-send \$S.SetLaunchAllowed uint32:100000 string:harbour-libre-monitor int32:1 >/dev/null"
  sleep 1; echo "sailjail after the user's Allow: $(R "$SJ")"
}

echo "== safety"
VBoxManage controlvm $VM audioout off 2>/dev/null
VBoxManage showvminfo $VM --machinereadable | grep -q '^audio_out="off"' || { echo "audio output not off, stop"; exit 2; }
U 'for n in $(pactl list short sinks | cut -f1); do pactl set-sink-mute $n 1; done; echo "muted sinks: $(pactl list sinks | grep -c "Mute: yes") of $(pactl list short sinks | wc -l)"'
U '[ ! -e ~/.config/it.michelebrami/libre-monitor/config.json ]' || { echo "the release identity has a configuration: stop (it could contact the source)"; exit 2; }
echo "no release configuration: no connection"
scp -q $OPTS -P 2223 "$A" root@localhost:/tmp/lm-A.rpm && scp -q $OPTS -P 2223 "$B" root@localhost:/tmp/lm-B.rpm || exit 2
echo "A=$(basename "$A") B=$(basename "$B")"

echo "== 0 clean start"
R "rpm -q harbour-libre-monitor >/dev/null && rpm -e harbour-libre-monitor; pkill -f '/usr/bin/harbour-libre-monitor --(service|adapter)'; sleep 2; pkill -9 -f '/usr/bin/harbour-libre-monitor --(service|adapter)'; true"
echo "processes left: $(anyproc)"

echo "== 1 install A and start"
R "rpm -i /tmp/lm-A.rpm && rpm -q harbour-libre-monitor"
sleep 3; grant
echo "Start: $(call Start)"; sleep 5
R "$PROCS"
echo "Alerts: $(call Alerts)"
verdict "1 A's service runs" "$(newproc 0)"

echo "== 2 upgrade A -> B"
T=$(R "$NOW"); R "rpm -U /tmp/lm-B.rpm && rpm -q harbour-libre-monitor"
sleep 5; echo "sailjail after the upgrade: $(R "$SJ")"
R "$PROCS"
echo "Alerts: $(call Alerts)"
verdict "2 after the upgrade the running service is B's binary" "$(newproc $T)"
[ "$(R "$PROCS" | grep -c '(deleted)')" = 0 ] && verdict "2 no process of the old version is left" ok || verdict "2 no process of the old version is left" no

echo "== 3 Service.Stop then Service.Start (what the package or the application would do)"
T=$(R "$NOW")
# If the upgrade took the permissions back, the user is asked again at the next start: give the Allow.
R "$SJ" | grep -q 'Allowed=1' || { echo "the upgrade reset the permissions"; grant; }
echo "Stop: $(call Stop)"; sleep 8
echo "processes after Stop: $(anyproc)"
echo "Start: $(call Start)"; sleep 5
R "$PROCS"
echo "Alerts: $(call Alerts)"
verdict "3 Stop and Start give B's process" "$(newproc $T)"

echo "== 4 remove B"
echo "data before: $(U "$DATA")"
R "rpm -e harbour-libre-monitor; rpm -q harbour-libre-monitor"
sleep 5; R "$PROCS"
echo "activation file: $(R 'ls /run/user/100000/dbus-1/services/ 2>/dev/null | grep -c "^it.michelebrami.libre-monitor.service$"')"
echo "data after: $(U "$DATA")"
echo "sailjail: $(R "$SJ")"
[ "$(anyproc)" = 0 ] && verdict "4 no process left after removal" ok || verdict "4 no process left after removal" no
U '[ -s ~/.local/share/it.michelebrami/libre-monitor/journal.db ]' && verdict "4 the user's data stays" ok || verdict "4 the user's data stays" no
# Clean the leftover by hand so that step 5 starts from a removed package without a process.
R "pkill -f '/usr/bin/harbour-libre-monitor --(service|adapter)'; sleep 3; pkill -9 -f '/usr/bin/harbour-libre-monitor --(service|adapter)'; true"

echo "== 5 install B again and start"
T=$(R "$NOW"); R "rpm -i /tmp/lm-B.rpm && rpm -q harbour-libre-monitor"
sleep 3; grant
echo "Start: $(call Start)"; sleep 5
R "$PROCS"
echo "Alerts: $(call Alerts)"
verdict "5 a fresh install starts B's process" "$(newproc $T)"

R 'rm -f /tmp/lm-A.rpm /tmp/lm-B.rpm'
echo "FAILS=$fail"
echo TEST_DONE
