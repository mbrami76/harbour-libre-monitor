#!/bin/bash
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Michele Brami
#
# Version stamp test on the EMULATOR only (never the phone).
#
#   tools/packaging/emu-stamp-test.sh A.rpm B.rpm      (i486, both with the stamp, A older than B)
#
#   1 the stamp is in both rpm files, with the package's Version-Release
#   2 install A: the stamp holds A's version, also as the running service's sandbox sees it
#   3 rpm -U to B: the stamp holds B's version, at once also inside the old service's sandbox
#   4 rpm -e: the stamp is gone, also inside the sandbox of the process left running
# Safety as in emu-upgrade-test.sh (audio off and muted, no release configuration).
set -u
A=${1:?usage: emu-stamp-test.sh A.rpm B.rpm}
B=${2:?usage: emu-stamp-test.sh A.rpm B.rpm}
VM=SailfishOS-5.1.0.11
KEY=$HOME/SailfishOS/vmshare/ssh/private_keys/sdk
OPTS="-o ConnectTimeout=15 -o LogLevel=ERROR -o StrictHostKeyChecking=no -o UserKnownHostsFile=/dev/null -i $KEY"
R() { ssh $OPTS -p 2223 root@localhost "$1"; }
U() { ssh $OPTS -p 2223 defaultuser@localhost "$1"; }
ST=/usr/share/harbour-libre-monitor/version
SVC="pgrep -f '^/usr/bin/harbour-libre-monitor --service'"
fail=0
check() { if [ "$2" = "$3" ]; then echo "PASS  $1 ($2)"; else echo "FAIL  $1 (got '$2', want '$3')"; fail=$((fail+1)); fi; }
ver() { sfdk engine exec rpm -qp --qf '%{VERSION}-%{RELEASE}' "$1" 2>/dev/null; }
inrpm() { sfdk engine exec rpm -qpl "$1" 2>/dev/null | grep -c -x "$ST"; }

echo "== safety"
VBoxManage controlvm $VM audioout off 2>/dev/null
VBoxManage showvminfo $VM --machinereadable | grep -q '^audio_out="off"' || { echo "audio output not off, stop"; exit 2; }
U 'for n in $(pactl list short sinks | cut -f1); do pactl set-sink-mute $n 1; done; echo "muted sinks: $(pactl list sinks | grep -c "Mute: yes") of $(pactl list short sinks | wc -l)"'
U '[ ! -e ~/.config/it.michelebrami/libre-monitor/config.json ]' || { echo "release configuration present, stop"; exit 2; }

echo "== 1 the stamp in the rpm files"
VA=$(ver "$A"); VB=$(ver "$B")
check "1 A lists $ST" "$(inrpm "$A")" 1
check "1 B lists $ST" "$(inrpm "$B")" 1
scp -q $OPTS -P 2223 "$A" root@localhost:/tmp/lm-A.rpm && scp -q $OPTS -P 2223 "$B" root@localhost:/tmp/lm-B.rpm || exit 2

echo "== 2 install A and start"
R "pkill -f '/usr/bin/harbour-libre-monitor --(service|adapter)'; sleep 2; rpm -q harbour-libre-monitor >/dev/null && rpm -e harbour-libre-monitor; rpm -i /tmp/lm-A.rpm
   S='--system --print-reply --dest=org.sailfishos.sailjaild1 /org/sailfishos/sailjaild1 org.sailfishos.sailjaild1'
   dbus-send \$S.SetGrantedPermissions uint32:100000 string:harbour-libre-monitor array:string:Internet,Secrets,Documents >/dev/null
   dbus-send \$S.SetLaunchAllowed uint32:100000 string:harbour-libre-monitor int32:1 >/dev/null"
U "dbus-send --session --print-reply --reply-timeout=20000 --dest=it.michelebrami.libre-monitor /it/michelebrami/libre_monitor it.michelebrami.libre_monitor.Service.Start >/dev/null; echo start rc=\$?"
sleep 5; P=$(R "$SVC" | head -1); echo "service pid=$P"
check "2 installed stamp" "$(R "cat $ST")" "$VA"
check "2 stamp mode and lines" "$(R "stat -c %a $ST; wc -l < $ST" | tr '\n' ' ')" "644 1 "
check "2 stamp seen by the service's sandbox" "$(R "cat /proc/$P/root$ST")" "$VA"

echo "== 3 rpm -U to B"
R "rpm -U /tmp/lm-B.rpm; rpm -q harbour-libre-monitor"
check "3 stamp replaced" "$(R "cat $ST")" "$VB"
check "3 the old service (pid $P, still running: $(R "[ -d /proc/$P ] && echo yes || echo no")) sees the new stamp" "$(R "cat /proc/$P/root$ST")" "$VB"

echo "== 4 rpm -e"
R "rpm -e harbour-libre-monitor; rpm -q harbour-libre-monitor"
check "4 stamp removed" "$(R "[ -e $ST ] && echo present || echo absent")" absent
check "4 the process left running sees no stamp" "$(R "[ -e /proc/$P/root$ST ] && echo present || echo absent")" absent

R "pkill -f '/usr/bin/harbour-libre-monitor --(service|adapter)'; rm -f /tmp/lm-A.rpm /tmp/lm-B.rpm"
echo "FAILS=$fail"
echo TEST_DONE
