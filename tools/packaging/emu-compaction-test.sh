#!/bin/bash
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Michele Brami
#
# the service, on the EMULATOR only (never the phone): the store of an
# older package (schema 1) is migrated to schema 2 by the new service, and a compaction through the
# service's D-Bus method thins it, with SYNTHETIC data only.
#
#   tools/packaging/emu-compaction-test.sh OLD.rpm NEW.rpm
#     OLD: a package with store schema 1 and the version stamp check
#     NEW: a package of this round with ANOTHER version (the 0.1-4 probe), so the old service
#          replaces itself after `rpm -U` and the new one opens and migrates the store
#
#   1 install OLD, start, stop: the old code creates its schema-1 store
#   2 synthetic rows are added on the PC (python3 sqlite3, the copy only): 3000 one-minute samples
#     200 days old, 60 recent ones, one old compaction record and one reimport record as schema 1
#     wrote them; the store goes back to the emulator
#   3 start OLD, rpm -U to NEW: the old service replaces itself; the new one migrates the store
#   4 the store (a copy) is schema 2, every row is still there, the old records are byte-identical
#   5 StartMaintenance("compaction", {"window_months":3}) over D-Bus; the signals are recorded
#   6 the store (a copy) is thinned: old samples fewer, recent ones all there, a new compaction
#     record without archive, the old ones kept; the service still runs with the same process
# Safety as in emu-self-replace-test.sh: audio output off and every sink muted (checked, else stop),
# no release configuration (no connection: nothing contacts any source), no add-on. Synthetic data
# only; copies of the store on the PC live in a temporary directory that is deleted at the end.
set -u
OLD=${1:?usage: emu-compaction-test.sh OLD.rpm NEW.rpm}
NEW=${2:?usage: emu-compaction-test.sh OLD.rpm NEW.rpm}
VM=SailfishOS-5.1.0.11
KEY=$HOME/SailfishOS/vmshare/ssh/private_keys/sdk
OPTS="-o ConnectTimeout=15 -o LogLevel=ERROR -o StrictHostKeyChecking=no -o UserKnownHostsFile=/dev/null -i $KEY"
R() { ssh $OPTS -p 2223 root@localhost "$1"; }
U() { ssh $OPTS -p 2223 defaultuser@localhost "$1"; }
NAME=it.michelebrami.libre-monitor
DEST="--dest=$NAME /it/michelebrami/libre_monitor it.michelebrami.libre_monitor.Service"
DATA=.local/share/it.michelebrami/libre-monitor
SUBJ=d2f1c9a0-5b3e-4c7d-8a91-0e2b4c6d8f10
SVC="pgrep -f '^/usr/bin/harbour-libre-monitor --service' | head -1"
WORK=$(mktemp -d)
trap 'rm -rf "$WORK"' EXIT
fail=0
verdict() { if [ "$2" = ok ]; then echo "PASS  $1"; else echo "FAIL  $1"; fail=$((fail+1)); fi; }
same() { [ "$2" = "$3" ] && verdict "$1 ($2)" ok || { echo "      got '$2', want '$3'"; verdict "$1" no; }; }
call() { U "dbus-send --session --print-reply --reply-timeout=20000 $DEST.$1 > /tmp/lm-call.out 2>&1; echo rc=\$? bytes=\$(wc -c < /tmp/lm-call.out); rm -f /tmp/lm-call.out"; }
appver() { U "dbus-send --session --print-reply --reply-timeout=20000 $DEST.Status 2>/dev/null | grep -o '\"app_version\":\"[^\"]*\"' | cut -d'\"' -f4"; }
grant() {
  R "S='--system --print-reply --dest=org.sailfishos.sailjaild1 /org/sailfishos/sailjaild1 org.sailfishos.sailjaild1'
     dbus-send \$S.SetGrantedPermissions uint32:100000 string:harbour-libre-monitor array:string:Internet,Secrets,Documents >/dev/null
     dbus-send \$S.SetLaunchAllowed uint32:100000 string:harbour-libre-monitor int32:1 >/dev/null"
}
# Copies the store (and its write-ahead log, if any) of the emulator into $WORK/<name>/.
fetch() { rm -rf "$WORK/$1"; mkdir -p "$WORK/$1"; scp -q $OPTS -P 2223 "defaultuser@localhost:$DATA/store.db*" "$WORK/$1/"; }
# Facts of a copy, one per line, read with python3 -I (numbers and keys only, no value).
facts() {
  python3 -I - "$WORK/$1/store.db" "$SUBJ" <<'EOF'
import sqlite3, sys, time
db = sqlite3.connect(sys.argv[1]); subj = sys.argv[2]
now = int(time.time() * 1000); old = now - 90 * 86400000
q = lambda s, *a: db.execute(s, a).fetchone()[0]
print("schema", q("select value from meta where key='store_schema'"))
print("samples_old", q("select count(*) from record where subject=? and metric='glucose' and measured_at<?", subj, old))
print("samples_recent", q("select count(*) from record where subject=? and metric='glucose' and measured_at>=?", subj, old))
print("compaction_rows", q("select count(*) from compaction"))
print("compaction_archived", q("select count(*) from compaction where archive_id is not null and archive_id<>''"))
print("compaction_old", "|".join(map(str, db.execute("select * from compaction where id='3c4d5e6f-7a8b-4c9d-8e0f-2a3b4c5d6e7f'").fetchone() or ())))
print("reimport_old", "|".join(map(str, db.execute("select * from reimport where id='5e6f7a8b-9c0d-4e1f-8a2b-4c5d6e7f8a9b'").fetchone() or ())))
print("compaction_new_archive", q("select count(*) from compaction where id<>'3c4d5e6f-7a8b-4c9d-8e0f-2a3b4c5d6e7f' and archive_id is null and archive_sha256 is null"))
EOF
}
fact() { grep "^$2 " <<<"$1" | cut -d' ' -f2-; }

echo "== safety"
VBoxManage controlvm $VM audioout off 2>/dev/null
VBoxManage showvminfo $VM --machinereadable | grep -q '^audio_out="off"' || { echo "audio output not off, stop"; exit 2; }
U 'for n in $(pactl list short sinks | cut -f1); do pactl set-sink-mute $n 1; done; echo "muted sinks: $(pactl list sinks | grep -c "Mute: yes") of $(pactl list short sinks | wc -l)"'
U '[ ! -e ~/.config/it.michelebrami/libre-monitor/config.json ]' || { echo "the release identity has a configuration: stop"; exit 2; }
echo "no release configuration: no connection"
R 'rpm -q libre-monitor-autostart >/dev/null 2>&1 && rpm -e libre-monitor-autostart; echo "add-on installed: $(rpm -q libre-monitor-autostart >/dev/null 2>&1 && echo yes || echo no)"'
scp -q $OPTS -P 2223 "$OLD" root@localhost:/tmp/lm-old.rpm && scp -q $OPTS -P 2223 "$NEW" root@localhost:/tmp/lm-new.rpm || exit 2

echo "== 0 clean start (the release identity's synthetic data of earlier emulator tests is removed)"
R "rpm -q harbour-libre-monitor >/dev/null && rpm -e harbour-libre-monitor; pkill -f '/usr/bin/harbour-libre-monitor --(service|adapter)'; sleep 2; pkill -9 -f '/usr/bin/harbour-libre-monitor --(service|adapter)'; true"
U "rm -f $DATA/store.db* $DATA/journal.db* $DATA/settings.json"

echo "== 1 OLD creates its schema-1 store"
R "rpm -i /tmp/lm-old.rpm && rpm -q harbour-libre-monitor"; sleep 2; grant
echo "Start: $(call Start)"; sleep 4; echo "Stop: $(call Stop)"; sleep 4
fetch s1; F=$(facts s1); echo "$F"
same "1 the old package's store is schema 1" "$(fact "$F" schema)" "1"

echo "== 2 synthetic rows added on the PC"
python3 -I - "$WORK/s1/store.db" "$SUBJ" <<'EOF'
import sqlite3, sys, time
db = sqlite3.connect(sys.argv[1]); subj = sys.argv[2]
now = int(time.time() * 1000); day = 86400000
db.execute("insert or ignore into subject values(?)", (subj,))
rows = []
for k in range(3000):
    rows.append((subj, 'glucose', now - 200 * day + k * 60000, 'libre.current', '', 0, 100.0 + k % 50, None, None,
                 'mg/dL', now - 200 * day + k * 60000 + 30000, '1/1/2026 12:00:00 AM', 'adapter', None))
for k in range(60):
    rows.append((subj, 'glucose', now - day + k * 60000, 'libre.current', '', 0, 120.0, None, None,
                 'mg/dL', now - day + k * 60000 + 30000, '1/1/2026 12:00:00 AM', 'adapter', None))
db.executemany("insert into record values(?,?,?,?,?,?,?,?,?,?,?,?,?,?)", rows)
db.execute("insert into compaction values('3c4d5e6f-7a8b-4c9d-8e0f-2a3b4c5d6e7f', 1769817600000, 1762128000000, 1762214400000, '{\"record\":12}', '4d5e6f7a-8b9c-4d0e-9f1a-3b4c5d6e7f8a', 'abababababababababababababababababababababababababababababababab', '1')")
db.execute("insert into reimport values('5e6f7a8b-9c0d-4e1f-8a2b-4c5d6e7f8a9b', 1769860800000, '4d5e6f7a-8b9c-4d0e-9f1a-3b4c5d6e7f8a', 'abababababababababababababababababababababababababababababababab', '1.0', '{\"added\":{\"record\":12}}')")
db.commit(); db.execute("pragma wal_checkpoint(TRUNCATE)"); db.close()
EOF
F2=$(facts s1); echo "$F2"
U "rm -f $DATA/store.db-wal $DATA/store.db-shm"
scp -q $OPTS -P 2223 "$WORK/s1/store.db" "defaultuser@localhost:$DATA/store.db" || exit 2
echo "store back on the emulator: $(U "stat -c '%U %a %s' $DATA/store.db")"

echo "== 3 OLD runs, rpm -U to NEW: the old service replaces itself, the new one migrates"
echo "Start: $(call Start)"; sleep 4; P1=$(R "$SVC"); echo "old service pid=$P1 version=$(appver)"
R "rpm -U /tmp/lm-new.rpm && rpm -q harbour-libre-monitor"
P2=""; for i in $(seq 1 100); do p=$(R "$SVC"); [ -n "$p" ] && [ "$p" != "$P1" ] && { P2=$p; break; }; sleep 2; done
echo "new service pid=$P2"
[ -n "$P2" ] && verdict "3 the new service runs" ok || verdict "3 the new service runs" no
sleep 3; VN=$(appver); echo "new version: $VN"

echo "== 4 the store is schema 2 and keeps every row"
fetch s2; F3=$(facts s2); echo "$F3"
same "4 schema" "$(fact "$F3" schema)" "2"
same "4 old samples kept" "$(fact "$F3" samples_old)" "$(fact "$F2" samples_old)"
same "4 recent samples kept" "$(fact "$F3" samples_recent)" "$(fact "$F2" samples_recent)"
same "4 the old compaction record is byte-identical" "$(fact "$F3" compaction_old)" "$(fact "$F2" compaction_old)"
same "4 the old reimport record is byte-identical" "$(fact "$F3" reimport_old)" "$(fact "$F2" reimport_old)"

echo "== 5 compaction through the service"
U "nohup dbus-monitor --session \"type='signal',interface='it.michelebrami.libre_monitor.Service',member='MaintenanceProgress'\" \"type='signal',interface='it.michelebrami.libre_monitor.Service',member='MaintenanceFinished'\" > /tmp/lm-mon.log 2>&1 &"
sleep 1
echo "StartMaintenance answer: [$(U "dbus-send --session --print-reply --reply-timeout=20000 $DEST.StartMaintenance string:compaction string:'{\"window_months\":3}' | grep -o 'string \".*\"'")]"
for i in $(seq 1 120); do U 'grep -q "member=MaintenanceFinished" /tmp/lm-mon.log' && break; sleep 1; done
sleep 1; U 'pkill -f "^dbus-monitor --session"'; U 'cat /tmp/lm-mon.log; rm -f /tmp/lm-mon.log' > "$WORK/mon.txt"
echo "phases: $(awk '/member=MaintenanceProgress/ {p=1; n=0; next} p && /string/ {n++; if (n==2) {print $2; p=0}}' "$WORK/mon.txt" | tr -d '"' | uniq | tr '\n' ' ')"
FIN=$(awk '/member=MaintenanceFinished/ {p=1; next} p' "$WORK/mon.txt" | grep -E '^ +(string|boolean)' | sed -E 's/^ +//' | tr '\n' ' ')
echo "finished: $FIN"
grep -q 'boolean true' <<<"$FIN" && verdict "5 the compaction finished ok" ok || verdict "5 the compaction finished ok" no
grep -q '"space":"enough"' <<<"$FIN" && verdict "5 the result carries the estimate with the rebuild need" ok || verdict "5 the result carries the estimate with the rebuild need" no
same "5 the service still runs, same process" "$(R "$SVC")" "$P2"
echo "Status after: $(call Status)"

echo "== 6 the store is thinned"
fetch s3; F4=$(facts s3); echo "$F4"
same "6 schema" "$(fact "$F4" schema)" "2"
[ "$(fact "$F4" samples_old)" -lt "$(fact "$F3" samples_old)" ] && verdict "6 old samples thinned" ok || verdict "6 old samples thinned" no
same "6 recent samples all kept" "$(fact "$F4" samples_recent)" "$(fact "$F3" samples_recent)"
same "6 one new compaction record without archive" "$(fact "$F4" compaction_new_archive)" "1"
same "6 the old compaction record is still byte-identical" "$(fact "$F4" compaction_old)" "$(fact "$F2" compaction_old)"
same "6 the reimport record is still there" "$(fact "$F4" reimport_old)" "$(fact "$F2" reimport_old)"

echo "== 7 clean up (rpm -e; the service ends by itself)"
R "rpm -e harbour-libre-monitor; rm -f /tmp/lm-old.rpm /tmp/lm-new.rpm"
echo "FAILS=$fail"
echo TEST_DONE
