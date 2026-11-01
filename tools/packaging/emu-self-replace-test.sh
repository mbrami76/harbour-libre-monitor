#!/bin/bash
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Michele Brami
#
# on the EMULATOR only (never the phone): the
# service replaces itself after an upgrade and ends itself after a removal, by comparing the version
# stamp /usr/share/harbour-libre-monitor/version with its own version at each heartbeat.
#
#   tools/packaging/emu-self-replace-test.sh A.rpm B.rpm     (i486, both WITH the check, A older than B)
#
#   1 install A, start the service: Status.app_version is A's
#   2 rpm -U to B, then NO D-Bus call of ours until a new service process exists: the old process stops
#     by itself and its own activation starts B. Measured: seconds from the end of rpm to the old
#     process gone, to B's process, to the first answer of Alerts. A bus monitor records who sent the
#     activation (the old process's own connection = the activation came from inside the sandbox).
#   3 a doubtful read: the stamp emptied by hand for two heartbeats: the service keeps running
#   4 the loop guard: rpm -U --oldpackage back to A less than 10 minutes after step 2's restart: no
#     restart while the guard holds (the binary 0.1-4 sees stamp 0.1-3), then exactly one restart once
#     the 10 minutes are over (measured), A runs
#   5 rpm -e: the service ends by itself (measured), nothing starts again, the user's data stays
#: a bus monitor also records the notification calls (Notify and
# CloseNotification to org.freedesktop.Notifications) and the resident notice's state is read from its
# icon (notice-1 active, notice-2 waiting or attention, notice-3 stopped; NotificationCatalog). Expected:
# no notice-3 from the old service at the upgrade stop (2), notice-3 at a normal Stop (4b), a
# CloseNotification and no notice-3 at the removal (5). This needs no sound and no screen.
# SKIP_GUARD=1 skips steps 3 and 4 (the doubtful read and the 10-minute loop guard).
# Each check prints PASS or FAIL. Timing comes from /proc/uptime on the emulator, so ssh delays do not
# count. Safety as in emu-upgrade-test.sh: audio output off and every sink muted (checked, else stop),
# the release identity has no configuration file (no connection, nothing contacts any source), the
# add-on is not installed (it would ping the name and hide what the service does by itself). Answers
# are not printed, only whether a method answered and the `app_version` of Status (no name, no value).
set -u
A=${1:?usage: emu-self-replace-test.sh A.rpm B.rpm}
B=${2:?usage: emu-self-replace-test.sh A.rpm B.rpm}
VM=SailfishOS-5.1.0.11
KEY=$HOME/SailfishOS/vmshare/ssh/private_keys/sdk
OPTS="-o ConnectTimeout=15 -o LogLevel=ERROR -o StrictHostKeyChecking=no -o UserKnownHostsFile=/dev/null -i $KEY"
R() { ssh $OPTS -p 2223 root@localhost "$1"; }
U() { ssh $OPTS -p 2223 defaultuser@localhost "$1"; }
NAME=it.michelebrami.libre-monitor
DEST="--dest=$NAME /it/michelebrami/libre_monitor it.michelebrami.libre_monitor.Service"
ST=/usr/share/harbour-libre-monitor/version
fail=0
verdict() { if [ "$2" = ok ]; then echo "PASS  $1"; else echo "FAIL  $1"; fail=$((fail+1)); fi; }
same() { [ "$2" = "$3" ] && verdict "$1 ($2)" ok || { echo "      got '$2', want '$3'"; verdict "$1" no; }; }
ver() { sfdk engine exec rpm -qp --qf '%{VERSION}-%{RELEASE}' "$1" 2>/dev/null; }
call() { U "dbus-send --session --print-reply --reply-timeout=20000 $DEST.$1 > /tmp/lm-call.out 2>&1; echo rc=\$? bytes=\$(wc -c < /tmp/lm-call.out); rm -f /tmp/lm-call.out"; }
appver() { U "dbus-send --session --print-reply --reply-timeout=20000 $DEST.Status 2>/dev/null | grep -o '\"app_version\":\"[^\"]*\"' | cut -d'\"' -f4"; }
startcause() { U "dbus-send --session --print-reply --reply-timeout=20000 $DEST.Status 2>/dev/null | grep -o '\"start_cause\":\"[^\"]*\"' | cut -d'\"' -f4"; }
# The bus monitor: activation calls and notification calls, on the session bus.
monstart() {
  U "nohup dbus-monitor --session \"type='method_call',member='Ping'\" \"type='method_call',member='StartServiceByName'\" \"type='method_call',interface='org.freedesktop.Notifications'\" > /tmp/lm-mon.log 2>&1 &"
  sleep 1
}
monstop() { sleep 2; U 'pkill -f "^dbus-monitor --session"'; sleep 1; U 'cat /tmp/lm-mon.log; rm -f /tmp/lm-mon.log' > /tmp/lm-mon.txt; }
# One line per call: "<sender> Notify notice-N" (the resident notice and its state) or "<sender> Close <id>".
notes() {
  awk '/^method call/ { m = ""; if ($0 ~ /member=Notify/) m = "Notify"; else if ($0 ~ /member=CloseNotification/) m = "Close";
                        match($0, /sender=[^ ]*/); snd = substr($0, RSTART + 7, RLENGTH - 7); next }
       m == "Notify" && /notice-[0-9]\.png/ { match($0, /notice-[0-9]/); print snd " Notify " substr($0, RSTART, RLENGTH); m = "" }
       m == "Close" && /uint32/ { print snd " Close " $2; m = "" }' /tmp/lm-mon.txt
}
holder() { U "dbus-send --session --print-reply --dest=org.freedesktop.DBus / org.freedesktop.DBus.GetNameOwner string:$NAME 2>/dev/null | grep -o '\":[0-9.]*\"' | tr -d '\"'"; }
grant() {
  R "S='--system --print-reply --dest=org.sailfishos.sailjaild1 /org/sailfishos/sailjaild1 org.sailfishos.sailjaild1'
     dbus-send \$S.SetGrantedPermissions uint32:100000 string:harbour-libre-monitor array:string:Internet,Secrets,Documents >/dev/null
     dbus-send \$S.SetLaunchAllowed uint32:100000 string:harbour-libre-monitor int32:1 >/dev/null"
}

# The watcher, run ON the emulator as root, so the times are not ssh round trips. Every command
# prints "<event> t=<seconds since the watcher started> ..." or "<event> timeout".
cat > /tmp/lm-watch.sh <<'EOF'
up() { cut -d' ' -f1 /proc/uptime; }
el() { awk -v a="$1" -v b="$(up)" 'BEGIN { printf "%.1f", b - a }'; }
svc() { pgrep -f '^/usr/bin/harbour-libre-monitor --service'; }
any() { pgrep -f '^/usr/bin/harbour-libre-monitor --(service|adapter)'; }
# A --service process started after tick $1 (field 22 of stat, USER_HZ 100).
newer() { for p in $(svc); do s=$(awk '{print $22}' /proc/$p/stat 2>/dev/null) || continue; [ -n "$s" ] && [ "$s" -gt "$1" ] && echo $p; done | head -1; }
# usage: watch.sh upgrade OLDPID TICK TIMEOUT | stays PID SECONDS | ends TIMEOUT | nothing SECONDS
t0=$(up); cmd=$1; shift
case $cmd in
upgrade)
  old=$1; tick=$2; lim=$3; gone=""; new=""
  while [ "$(el $t0 | cut -d. -f1)" -lt "$lim" ]; do
    [ -z "$gone" ] && [ ! -d /proc/$old ] && gone=$(el $t0) && echo "old-gone t=$gone"
    [ -z "$new" ] && n=$(newer $tick) && [ -n "$n" ] && new=$(el $t0) && echo "new-service t=$new pid=$n at=$(date -u +%H:%M:%S)"
    [ -n "$gone" ] && [ -n "$new" ] && exit 0
    sleep 0.25
  done
  [ -z "$gone" ] && echo "old-gone timeout"; [ -z "$new" ] && echo "new-service timeout"; exit 1 ;;
stays)
  pid=$1; lim=$2
  while [ "$(el $t0 | cut -d. -f1)" -lt "$lim" ]; do
    [ -d /proc/$pid ] || { echo "ended t=$(el $t0)"; exit 1; }
    n=$(svc | grep -v -x "$pid" | head -1); [ -n "$n" ] && { echo "other-service t=$(el $t0) pid=$n"; exit 1; }
    sleep 0.5
  done
  echo "stayed t=$(el $t0) pid=$pid"; exit 0 ;;
ends)
  lim=$1
  while [ "$(el $t0 | cut -d. -f1)" -lt "$lim" ]; do
    [ -z "$(any)" ] && { echo "no-process t=$(el $t0)"; exit 0; }
    sleep 0.25
  done
  echo "no-process timeout"; exit 1 ;;
nothing)
  lim=$1
  while [ "$(el $t0 | cut -d. -f1)" -lt "$lim" ]; do
    p=$(any | head -1); [ -n "$p" ] && { echo "process t=$(el $t0) pid=$p"; exit 1; }
    sleep 0.5
  done
  echo "nothing t=$(el $t0)"; exit 0 ;;
esac
EOF

echo "== safety"
VBoxManage controlvm $VM audioout off 2>/dev/null
VBoxManage showvminfo $VM --machinereadable | grep -q '^audio_out="off"' || { echo "audio output not off, stop"; exit 2; }
U 'for n in $(pactl list short sinks | cut -f1); do pactl set-sink-mute $n 1; done; echo "muted sinks: $(pactl list sinks | grep -c "Mute: yes") of $(pactl list short sinks | wc -l)"'
U '[ ! -e ~/.config/it.michelebrami/libre-monitor/config.json ]' || { echo "the release identity has a configuration: stop"; exit 2; }
echo "no release configuration: no connection"
R 'rpm -q libre-monitor-autostart >/dev/null 2>&1 && { rpm -e libre-monitor-autostart && echo "add-on removed for the test"; su defaultuser -c "XDG_RUNTIME_DIR=/run/user/100000 systemctl --user daemon-reload" 2>/dev/null; }; echo "add-on installed: $(rpm -q libre-monitor-autostart >/dev/null 2>&1 && echo yes || echo no)"'
VA=$(ver "$A"); VB=$(ver "$B"); echo "A=$(basename "$A") ($VA) B=$(basename "$B") ($VB)"
[ -n "$VA" ] && [ -n "$VB" ] && [ "$VA" != "$VB" ] || { echo "two different versions needed"; exit 2; }
scp -q $OPTS -P 2223 "$A" root@localhost:/tmp/lm-A.rpm && scp -q $OPTS -P 2223 "$B" root@localhost:/tmp/lm-B.rpm \
  && scp -q $OPTS -P 2223 /tmp/lm-watch.sh root@localhost:/tmp/lm-watch.sh || exit 2
rm -f /tmp/lm-watch.sh
W="sh /tmp/lm-watch.sh"
TICK='awk "{print int(\$1*100)}" /proc/uptime'
SVC="pgrep -f '^/usr/bin/harbour-libre-monitor --service' | head -1"

echo "== 0 clean start"
R "rpm -q harbour-libre-monitor >/dev/null && rpm -e harbour-libre-monitor; pkill -f '/usr/bin/harbour-libre-monitor --(service|adapter)'; sleep 2; pkill -9 -f '/usr/bin/harbour-libre-monitor --(service|adapter)'; true"
U 'rm -f ~/.local/share/it.michelebrami/libre-monitor/settings.json'   # no guard time of an earlier run
echo "processes: $(R "pgrep -f '^/usr/bin/harbour-libre-monitor --' | wc -l")"

echo "== 1 install A and start"
R "rpm -i /tmp/lm-A.rpm && rpm -q harbour-libre-monitor"; sleep 2; grant
echo "Start: $(call Start)"; sleep 4
P1=$(R "$SVC"); echo "service pid=$P1"
same "1 Status.app_version is A's" "$(appver)" "$VA"
echo "Alerts: $(call Alerts)"

echo "== 2 rpm -U to B; no call of ours until B's process exists"
HOLDER=$(holder)
echo "the old service's connection: $HOLDER"
monstart
T=$(R "$TICK")
OUT=$(R "rpm -U /tmp/lm-B.rpm && $W upgrade $P1 $T 180"); echo "$OUT"
grep -q '^new-service t=' <<<"$OUT" && verdict "2 B's service started without any call from outside" ok || verdict "2 B's service started without any call from outside" no
grep -q '^old-gone t=' <<<"$OUT" && verdict "2 the old process ended by itself" ok || verdict "2 the old process ended by itself" no
TA=$(U "up() { cut -d' ' -f1 /proc/uptime; }; s=\$(up); i=0; while [ \$i -lt 120 ]; do dbus-send --session --print-reply --reply-timeout=5000 $DEST.Alerts >/dev/null 2>&1 && break; sleep 0.25; i=\$((i+1)); done; awk -v a=\$s -v b=\$(up) 'BEGIN{printf \"%.1f\", b-a}'")
echo "Alerts answered $TA s after B's process was seen"
NEWHOLDER=$(holder); echo "the new service's connection: $NEWHOLDER"
monstop
echo "activation calls seen on the bus (sender -> destination member):"
grep -E '^method call' /tmp/lm-mon.txt | grep -E 'member=(Ping|StartServiceByName)' | sed -E 's/.*sender=([^ ]*).*destination=([^ ]*).*member=([^ ]*).*/  \1 -> \2 \3/' | tee /tmp/lm-mon-lines.txt
echo "notification calls (sender call notice):"; notes | sed 's/^/  /' | tee /tmp/lm-notes.txt
same "2: no stopped notice (notice-3) from the old service" "$(grep -c "^  $HOLDER Notify notice-3" /tmp/lm-notes.txt)" "0"
same "2: the old service closed nothing" "$(grep -c "^  $HOLDER Close" /tmp/lm-notes.txt)" "0"
[ "$(grep -c "^  $NEWHOLDER Notify notice-1" /tmp/lm-notes.txt)" -ge 1 ] && verdict "2 the new service shows its active notice (notice-1)" ok || verdict "2 the new service shows its active notice (notice-1)" no
rm -f /tmp/lm-notes.txt
grep -q "^  $HOLDER -> " /tmp/lm-mon-lines.txt && verdict "2 the activation came from the old service's own connection (inside the sandbox)" ok \
  || verdict "2 the activation came from the old service's own connection (inside the sandbox)" no
rm -f /tmp/lm-mon-lines.txt
P2=$(R "$SVC"); echo "service pid=$P2"
same "2 Status.app_version is B's" "$(appver)" "$VB"
same "2 B's start cause" "$(startcause)" "after-stop"
same "2 one service process" "$(R "pgrep -f '^/usr/bin/harbour-libre-monitor --service' | wc -l")" "1"
R "journalctl -b --no-pager 2>/dev/null | grep -o 'libre-monitor: restart after an upgrade.*' | tail -2"
R "command -v sqlite3 >/dev/null && sqlite3 /home/defaultuser/.local/share/it.michelebrami/libre-monitor/journal.db \"select kind, extra from journal where kind in ('stop','version-check') order by rowid desc limit 3\" 2>/dev/null || echo '(no sqlite3 on the emulator: stop record not read)'"
U "cat ~/.local/share/it.michelebrami/libre-monitor/settings.json 2>/dev/null | grep -o '\"version.self_restart_at\": *\"[^\"]*\"'"

if [ "${SKIP_GUARD:-0}" = 1 ]; then echo "== 3 and 4 skipped (SKIP_GUARD=1)"; P3=$P2; else
echo "== 3 a doubtful read: the stamp emptied for two heartbeats"
R "cp $ST /tmp/lm-stamp && : > $ST"
OUT=$(R "$W stays $P2 130"); echo "$OUT"
grep -q '^stayed' <<<"$OUT" && verdict "3 an empty stamp never stops the service" ok || verdict "3 an empty stamp never stops the service" no
R "cat /tmp/lm-stamp > $ST && rm -f /tmp/lm-stamp; cat $ST"

echo "== 4 the loop guard: back to A within 10 minutes of the restart of step 2"
R "rpm -U --oldpackage /tmp/lm-A.rpm && rpm -q harbour-libre-monitor"
OUT=$(R "$W stays $P2 130"); echo "$OUT"
grep -q '^stayed' <<<"$OUT" && verdict "4 no second restart within 10 minutes (the guard holds across the restart)" ok \
  || verdict "4 no second restart within 10 minutes (the guard holds across the restart)" no
same "4 B still runs with stamp A" "$(appver)" "$VB"
# The guard ends 600 s after the restart of step 2; wait for it here without any call to the service.
echo "guard time of step 2: $(U "grep -o '\"version.self_restart_at\": *\"[^\"]*\"' ~/.local/share/it.michelebrami/libre-monitor/settings.json")"
T=$(R "$TICK")
OUT=$(R "$W upgrade $P2 $T 600"); echo "$OUT"
grep -q '^new-service t=' <<<"$OUT" && verdict "4 after the guard one restart brings A" ok || verdict "4 after the guard one restart brings A" no
P3=$(R "$SVC")
same "4 Status.app_version is A's" "$(appver)" "$VA"
OUT=$(R "$W stays $P3 130"); echo "$OUT"
grep -q '^stayed' <<<"$OUT" && verdict "4 and only one" ok || verdict "4 and only one" no
fi

echo "== 4b a normal Stop keeps its notice, then Start"
O=$(holder); monstart
echo "Stop: $(call Stop)"
OUT=$(R "$W ends 30"); echo "$OUT"; monstop
notes | sed 's/^/  /' | tee /tmp/lm-notes.txt
[ "$(grep -c "^  $O Notify notice-3" /tmp/lm-notes.txt)" -ge 1 ] && verdict "4b a requested stop posts the stopped notice (notice-3)" ok || verdict "4b a requested stop posts the stopped notice (notice-3)" no
rm -f /tmp/lm-notes.txt
echo "Start: $(call Start)"; sleep 4; P3=$(R "$SVC"); echo "service pid=$P3"

echo "== 5 rpm -e"
O=$(holder); echo "the service's connection: $O"; monstart
OUT=$(R "rpm -e harbour-libre-monitor && $W ends 120"); echo "$OUT"
monstop; notes | sed 's/^/  /' | tee /tmp/lm-notes.txt
same "5: no stopped notice (notice-3) at the removal" "$(grep -c "^  $O Notify notice-3" /tmp/lm-notes.txt)" "0"
[ "$(grep -c "^  $O Close" /tmp/lm-notes.txt)" -ge 1 ] && verdict "5: the resident notice is closed" ok || verdict "5: the resident notice is closed" no
rm -f /tmp/lm-notes.txt
grep -q '^no-process t=' <<<"$OUT" && verdict "5 the service ended by itself after the removal" ok || verdict "5 the service ended by itself after the removal" no
OUT=$(R "$W nothing 90"); echo "$OUT"
grep -q '^nothing' <<<"$OUT" && verdict "5 nothing started again" ok || verdict "5 nothing started again" no
U '[ -s ~/.local/share/it.michelebrami/libre-monitor/journal.db ] && [ -s ~/.local/share/it.michelebrami/libre-monitor/store.db ]' \
  && verdict "5 the user's data stays" ok || verdict "5 the user's data stays" no
echo "activation file: $(R 'ls /run/user/100000/dbus-1/services/ 2>/dev/null | grep -c "^it.michelebrami.libre-monitor.service$"')"

R 'rm -f /tmp/lm-A.rpm /tmp/lm-B.rpm /tmp/lm-watch.sh'; rm -f /tmp/lm-mon.txt
echo "FAILS=$fail"
echo TEST_DONE
