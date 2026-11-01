#!/bin/bash
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Michele Brami
#
# (REQ-STO-17), the service, on the EMULATOR only (never the phone): the
# store (schema 2 to 3) and the journal (schema 1 to 2) of an older package are migrated by the new
# service, and a purge through the service's D-Bus method removes one of two synthetic Subjects' old
# data, with SYNTHETIC data only.
#
#   tools/packaging/emu-purge-test.sh OLD.rpm NEW.rpm [SQLITE3]
#     OLD: a package with store schema 2 and journal schema 1 and the version stamp check
#     NEW: a package of this round with ANOTHER version (a probe), so the old service replaces itself
#          after `rpm -U` and the new one opens and migrates both files
#     SQLITE3: optional directory with an i486 `sqlite3-i486` program and its `libreadline.so.8` (taken
#          from the i486 build target after `zypper install sqlite` there); with it, step 8 holds the
#          journal's write lock while a purge runs, so that its journal part fails and the repeat of
# is shown. Both go to /tmp on the emulator and are removed afterwards
#
#   1 a configuration with two Subjects (labels AA and BB, one connection each, NO credentials: nothing
#     can log in, nothing contacts a source); install OLD, start, stop: its store and journal exist
#   2 synthetic rows added on the PC (python3 -I, sqlite3 module, the copies only): per Subject 3000
#     one-minute samples 200 days old and 60 from a day back; per Subject an alert, a threshold and a
#     silent entry 200 days old and 2 days old; the files go back to the emulator
#   3 start OLD, rpm -U to NEW: the new service migrates: store schema 3, journal schema 2, every row kept
#   4 the gate: purges of A with wrong labels are refused at the start, nothing changes
#   5 purge-estimate of A (3 months)
#   6 purge of A with the right label; during the run the service answers Status; afterwards a new
#     heartbeat shows that monitoring went on
#   7 the files: A's old data gone in the store and the journal, A's recent data and ALL of B unchanged
#     (row hashes), the estimate equal to the result, no purge record (meta and tables as before)
#   8 (with SQLITE3) purge of B while the journal's write lock is held: the store part is done, the journal
#     part fails (io, store_part_done true); the same purge again finishes the journal part; a third finds
#     nothing; A unchanged by B's purge
# Safety: audio output off and every sink muted (checked, else stop); no credentials; no add-on; no value,
# name or label printed (counts, keys and hashes only). Copies on the PC live in a temporary directory.
set -u
OLD=${1:?usage: emu-purge-test.sh OLD.rpm NEW.rpm [SQLITE3]}
NEW=${2:?usage: emu-purge-test.sh OLD.rpm NEW.rpm [SQLITE3]}
SQL=${3:-}
VM=SailfishOS-5.1.0.11
KEY=$HOME/SailfishOS/vmshare/ssh/private_keys/sdk
OPTS="-o ConnectTimeout=15 -o LogLevel=ERROR -o StrictHostKeyChecking=no -o UserKnownHostsFile=/dev/null -i $KEY"
R() { ssh $OPTS -p 2223 root@localhost "$1"; }
U() { ssh $OPTS -p 2223 defaultuser@localhost "$1"; }
NAME=it.michelebrami.libre-monitor
DEST="--dest=$NAME /it/michelebrami/libre_monitor it.michelebrami.libre_monitor.Service"
DATA=.local/share/it.michelebrami/libre-monitor
CONF=.config/it.michelebrami/libre-monitor
A=1f2e3d4c-5b6a-4978-8a9b-0c1d2e3f4a5b
B=2a3b4c5d-6e7f-4a8b-9c0d-1e2f3a4b5c6d
SVC="pgrep -f '^/usr/bin/harbour-libre-monitor --service' | head -1"
WORK=$(mktemp -d)
trap 'rm -rf "$WORK"' EXIT
fail=0
verdict() { if [ "$2" = ok ]; then echo "PASS  $1"; else echo "FAIL  $1"; fail=$((fail+1)); fi; }
same() { [ "$2" = "$3" ] && verdict "$1 ($2)" ok || { echo "      got '$2', want '$3'"; verdict "$1" no; }; }
call() { U "dbus-send --session --print-reply --reply-timeout=20000 $DEST.$1 > /tmp/lm-call.out 2>&1; echo rc=\$? bytes=\$(wc -c < /tmp/lm-call.out); rm -f /tmp/lm-call.out"; }
grant() {
  R "S='--system --print-reply --dest=org.sailfishos.sailjaild1 /org/sailfishos/sailjaild1 org.sailfishos.sailjaild1'
     dbus-send \$S.SetGrantedPermissions uint32:100000 string:harbour-libre-monitor array:string:Internet,Secrets,Documents >/dev/null
     dbus-send \$S.SetLaunchAllowed uint32:100000 string:harbour-libre-monitor int32:1 >/dev/null"
}
fetch() { rm -rf "$WORK/$1"; mkdir -p "$WORK/$1"; scp -q $OPTS -P 2223 "defaultuser@localhost:$DATA/store.db*" "defaultuser@localhost:$DATA/journal.db*" "$WORK/$1/"; }
# StartMaintenance(kind, json): prints only the answer word ("" = started).
start() { U "dbus-send --session --print-reply --reply-timeout=20000 $DEST.StartMaintenance string:$1 string:'$2' | grep -o 'string \".*\"' | cut -d'\"' -f2"; }
monstart() { U "nohup dbus-monitor --session \"type='signal',interface='it.michelebrami.libre_monitor.Service',member='MaintenanceProgress'\" \"type='signal',interface='it.michelebrami.libre_monitor.Service',member='MaintenanceFinished'\" > /tmp/lm-mon.log 2>&1 &"; sleep 1; }
# Waits for MaintenanceFinished; leaves the monitor log in $WORK/$1.mon
monwait() { for i in $(seq 1 120); do U 'grep -q "member=MaintenanceFinished" /tmp/lm-mon.log' && break; sleep 1; done
            sleep 1; U 'pkill -f "^dbus-monitor --session"'; U 'cat /tmp/lm-mon.log; rm -f /tmp/lm-mon.log' > "$WORK/$1.mon"; }
phases() { awk '/member=MaintenanceProgress/ {p=1; n=0; next} p && /string/ {n++; if (n==2) {print $2; p=0}}' "$WORK/$1.mon" | tr -d '"' | uniq | tr '\n' ' '; }
# The six arguments of MaintenanceFinished as JSON: [kind, ok, class, message, cancel, details]
finished() {
  python3 -I - "$WORK/$1.mon" <<'EOF'
import json, re, sys
t = open(sys.argv[1]).read()
i = t.rfind("member=MaintenanceFinished")
args = []
for line in t[i:].splitlines()[1:]:
    line = line.strip()
    m = re.match(r'^string "(.*)"$', line)
    if m: args.append(m.group(1)); continue
    m = re.match(r'^boolean (true|false)$', line)
    if m: args.append(m.group(1) == "true")
args = args[:6]
if len(args) == 6:
    try: args[5] = json.loads(args[5])
    except Exception: pass
print(json.dumps(args, sort_keys=True))
EOF
}
j() { python3 -I -c "import json,sys; v=json.loads(sys.argv[1]); exec('for k in sys.argv[2].split(\".\"):\n  v = v[int(k)] if isinstance(v, list) else v.get(k)'); print(json.dumps(v, sort_keys=True) if isinstance(v,(dict,list)) else v)" "$1" "$2"; }
# Facts of a copy: schemas, counts per Subject old/recent, hashes of each Subject's rows, meta keys.
facts() {
  python3 -I - "$WORK/$1" "$A" "$B" <<'EOF'
import hashlib, sqlite3, sys, time
d, a, b = sys.argv[1:4]
s = sqlite3.connect(d + "/store.db"); jn = sqlite3.connect(d + "/journal.db")
now = int(time.time() * 1000); old = now - 90 * 86400000
q = lambda db, sql, *p: db.execute(sql, p).fetchone()[0]
h = lambda rows: hashlib.sha256("\n".join("|".join(map(str, r)) for r in rows).encode()).hexdigest()[:16]
print("store_schema", q(s, "select value from meta where key='store_schema'"))
print("journal_schema", q(jn, "select value from meta where key='store_schema'"))
print("store_meta", ",".join(r[0] for r in s.execute("select key from meta order by key")))
print("journal_meta", ",".join(r[0] for r in jn.execute("select key from meta order by key")))
print("tables", ",".join(r[0] for r in s.execute("select name from sqlite_master where type='table' order by name")))
print("compaction_rows", q(s, "select count(*) from compaction"))
for n, u in (("a", a), ("b", b)):
    print(n + "_old", q(s, "select count(*) from record where subject=? and measured_at<?", u, old))
    print(n + "_recent", q(s, "select count(*) from record where subject=? and measured_at>=?", u, old))
    print(n + "_hash", h(s.execute("select * from record where subject=? order by 1,2,3,4,5,6,7", (u,))))
    like = '%"subject":"' + u + '"%'
    for k in ("alert", "threshold", "silent"):
        print(n + "_j_" + k + "_old", q(jn, "select count(*) from journal where kind=? and extra like ? and wall_time<?", k, like, old))
    print(n + "_j_recent", q(jn, "select count(*) from journal where extra like ? and wall_time>=?", like, old))
    print(n + "_j_hash", h(jn.execute("select * from journal where extra like ? order by id", (like,))))
print("j_service", q(jn, "select count(*) from journal where kind in ('start','stop','heartbeat')"))
print("j_heartbeats", q(jn, "select count(*) from journal where kind='heartbeat'"))
EOF
}
fact() { grep "^$2 " <<<"$1" | cut -d' ' -f2-; }

echo "== safety"
VBoxManage controlvm $VM audioout off 2>/dev/null
VBoxManage showvminfo $VM --machinereadable | grep -q '^audio_out="off"' || { echo "audio output not off, stop"; exit 2; }
echo "audio_out=off (checked)"
U 'for n in $(pactl list short sinks | cut -f1); do pactl set-sink-mute $n 1; done; echo "muted sinks: $(pactl list sinks | grep -c "Mute: yes") of $(pactl list short sinks | wc -l)"'
R 'rpm -q libre-monitor-autostart >/dev/null 2>&1 && rpm -e libre-monitor-autostart; echo "add-on installed: $(rpm -q libre-monitor-autostart >/dev/null 2>&1 && echo yes || echo no)"'
scp -q $OPTS -P 2223 "$OLD" root@localhost:/tmp/lm-old.rpm && scp -q $OPTS -P 2223 "$NEW" root@localhost:/tmp/lm-new.rpm || exit 2

echo "== 0 clean start, configuration with two Subjects and no credentials"
R "rpm -q harbour-libre-monitor >/dev/null && rpm -e harbour-libre-monitor; pkill -f '/usr/bin/harbour-libre-monitor --(service|adapter)'; sleep 2; pkill -9 -f '/usr/bin/harbour-libre-monitor --(service|adapter)'; true"
U "rm -f $DATA/store.db* $DATA/journal.db* $DATA/settings.json; mkdir -p $CONF"
U "cat > $CONF/config.json" <<EOF
{"format":1,"connections":[
 {"id":"c1","adapter":"libre","subjects":[{"id":"{$A}","ref":"synthetic-ref-a","label":"AA"}]},
 {"id":"c2","adapter":"libre","subjects":[{"id":"{$B}","ref":"synthetic-ref-b","label":"BB"}]}]}
EOF
echo "configuration written ($(U "wc -c < $CONF/config.json") bytes)"

echo "== 1 OLD creates its store and journal"
R "rpm -i /tmp/lm-old.rpm && rpm -q harbour-libre-monitor"; sleep 2; grant
echo "Start: $(call Start)"; sleep 5; echo "Stop: $(call Stop)"; sleep 4
fetch s1; F=$(facts s1); echo "$F" | grep -E 'schema|tables'
same "1 store schema 2" "$(fact "$F" store_schema)" "2"
same "1 journal schema 1" "$(fact "$F" journal_schema)" "1"

echo "== 2 synthetic rows added on the PC"
python3 -I - "$WORK/s1" "$A" "$B" <<'EOF'
import json, sqlite3, sys, time, uuid
d, a, b = sys.argv[1:4]
s = sqlite3.connect(d + "/store.db"); jn = sqlite3.connect(d + "/journal.db")
now = int(time.time() * 1000); day = 86400000
seq = 0
for u in (a, b):
    s.execute("insert or ignore into subject values(?)", (u,))
    rows = []
    for k in range(3000):
        t = now - 200 * day + k * 60000
        rows.append((u, 'glucose', t, 'libre.current', '', 0, 100.0 + k % 50, None, None, 'mg/dL', t + 30000, '1/1/2026 12:00:00 AM', 'adapter', None))
    for k in range(60):
        t = now - day + k * 60000
        rows.append((u, 'glucose', t, 'libre.current', '', 0, 120.0, None, None, 'mg/dL', t + 30000, '1/1/2026 12:00:00 AM', 'adapter', None))
    s.executemany("insert into record values(?,?,?,?,?,?,?,?,?,?,?,?,?,?)", rows)
    for when in (now - 200 * day, now - 2 * day):
        seq += 1
        ep = str(uuid.uuid4())
        since = time.strftime("%Y-%m-%dT%H:%M:%S.000Z", time.gmtime(when / 1000))
        jn.execute("insert into journal values(?,?,?,?,?,?,?,?,?,?)", (str(uuid.uuid4()), "alert", when, None, None, None, None, None, None,
            json.dumps({"acknowledged_in_episode": False, "alert": "L", "episode": ep, "event": "pending", "from": "normal", "seq": seq,
                        "since": since, "slot": "low", "state": "pending", "subject": u}, sort_keys=True, separators=(",", ":"))))
        jn.execute("insert into journal values(?,?,?,?,?,?,?,?,?,?)", (str(uuid.uuid4()), "threshold", when + 1000, None, None, None, None, None, None,
            json.dumps({"boundary": "L", "previous": 70, "subject": u, "value": 75}, sort_keys=True, separators=(",", ":"))))
        jn.execute("insert into journal values(?,?,?,?,?,?,?,?,?,?)", (str(uuid.uuid4()), "silent", when + 2000, None, None, None, None, None, None,
            json.dumps({"event": "start", "seq": seq, "subject": u}, sort_keys=True, separators=(",", ":"))))
s.commit(); jn.commit()
s.execute("pragma wal_checkpoint(TRUNCATE)"); jn.execute("pragma wal_checkpoint(TRUNCATE)")
s.close(); jn.close()
EOF
F2=$(facts s1); echo "$F2" | grep -vE 'hash'
U "rm -f $DATA/store.db-wal $DATA/store.db-shm $DATA/journal.db-wal $DATA/journal.db-shm"
scp -q $OPTS -P 2223 "$WORK/s1/store.db" "$WORK/s1/journal.db" "defaultuser@localhost:$DATA/" || exit 2
echo "files back on the emulator: $(U "stat -c '%U %a %s' $DATA/store.db $DATA/journal.db | tr '\n' ' '")"

echo "== 3 OLD runs, rpm -U to NEW: the new service migrates store 2->3 and journal 1->2"
echo "Start: $(call Start)"; sleep 4; P1=$(R "$SVC"); echo "old service pid=$P1"
R "rpm -U /tmp/lm-new.rpm && rpm -q harbour-libre-monitor"
P2=""; for i in $(seq 1 120); do p=$(R "$SVC"); [ -n "$p" ] && [ "$p" != "$P1" ] && { P2=$p; break; }; sleep 2; done
echo "new service pid=$P2"
[ -n "$P2" ] && verdict "3 the new service runs" ok || verdict "3 the new service runs" no
sleep 5
fetch s2; F3=$(facts s2); echo "$F3" | grep -vE 'hash'
same "3 store schema" "$(fact "$F3" store_schema)" "3"
same "3 journal schema" "$(fact "$F3" journal_schema)" "2"
for k in a_old a_recent b_old b_recent a_hash b_hash; do same "3 store $k kept" "$(fact "$F3" $k)" "$(fact "$F2" $k)"; done
for k in a_j_alert_old a_j_threshold_old a_j_silent_old b_j_alert_old b_j_threshold_old b_j_silent_old; do same "3 journal $k kept" "$(fact "$F3" $k)" "$(fact "$F2" $k)"; done
same "3 no adapter process (no credentials, nothing contacts a source)" "$(R "pgrep -f '^/usr/bin/harbour-libre-monitor --adapter' | wc -l")" "0"

echo "== 4 the gate: wrong labels are refused at the start"
same "4 lower case" "$(start purge "{\"subject\":\"{$A}\",\"age_months\":3,\"typed_label\":\"aa\"}")" "label-mismatch"
same "4 the other Subject's label" "$(start purge "{\"subject\":\"{$A}\",\"age_months\":3,\"typed_label\":\"BB\"}")" "label-mismatch"
same "4 no label typed" "$(start purge "{\"subject\":\"{$A}\",\"age_months\":3}")" "label-mismatch"
same "4 age 5 months" "$(start purge "{\"subject\":\"{$A}\",\"age_months\":5,\"typed_label\":\"AA\"}")" "age-not-allowed"
same "4 unknown Subject" "$(start purge "{\"subject\":\"{$(cat /proc/sys/kernel/random/uuid)}\",\"age_months\":3,\"typed_label\":\"AA\"}")" "unknown-subject"
fetch s4; F4=$(facts s4)
same "4 store unchanged" "$(fact "$F4" a_hash) $(fact "$F4" b_hash)" "$(fact "$F3" a_hash) $(fact "$F3" b_hash)"
same "4 journal unchanged" "$(fact "$F4" a_j_hash) $(fact "$F4" b_j_hash)" "$(fact "$F3" a_j_hash) $(fact "$F3" b_j_hash)"

echo "== 5 estimate of A (3 months)"
monstart
same "5 the estimate starts" "$(start purge-estimate "{\"subject\":\"{$A}\",\"age_months\":3}")" ""
monwait est; EST=$(finished est); echo "estimate: $(j "$EST" 5 | python3 -I -c 'import json,sys; d=json.load(sys.stdin); print(json.dumps({k: d[k] for k in ("store_rows","journal_entries","total_rows","store_bytes","journal_file_bytes","needed_bytes","free_bytes","space","total_bytes_estimated")}, sort_keys=True))')"
same "5 estimate ok" "$(j "$EST" 1)" "True"
same "5 estimate: A's old records" "$(j "$EST" 5.store_rows.record)" "$(fact "$F3" a_old)"
fetch s5; F5=$(facts s5)
same "5 the estimate changed nothing" "$(fact "$F5" a_hash) $(fact "$F5" a_j_hash)" "$(fact "$F3" a_hash) $(fact "$F3" a_j_hash)"

echo "== 6 purge of A with the right label; the service answers meanwhile"
HB0=$(fact "$F5" j_heartbeats)
monstart
T0=$(date +%s.%N)
same "6 the purge starts" "$(start purge "{\"subject\":\"{$A}\",\"age_months\":3,\"typed_label\":\"AA\"}")" ""
echo "Status during the run: $(call Status)"
monwait run; T1=$(date +%s.%N); RUN=$(finished run)
echo "phases: $(phases run)"
echo "seconds from the start call to MaintenanceFinished seen (bus monitor, ssh round trips included): $(python3 -I -c "print(round($T1-$T0,1))")"
echo "result: $(j "$RUN" 5 | python3 -I -c 'import json,sys; d=json.load(sys.stdin); print(json.dumps({k: d.get(k) for k in ("removed","journal_removed","removed_total","bytes_freed","journal_bytes_freed","store_part_done")}, sort_keys=True))')"
same "6 the purge finished ok" "$(j "$RUN" 1)" "True"
same "6 removed per table equals the estimate" "$(j "$RUN" 5.removed)" "$(j "$EST" 5.store_rows)"
same "6 removed per journal kind equals the estimate" "$(j "$RUN" 5.journal_removed)" "$(j "$EST" 5.journal_entries)"
same "6 the label does not travel back" "$(grep -c '"AA"' <<<"$RUN")" "0"
same "6 the service still runs, same process" "$(R "$SVC")" "$P2"
echo "waiting 75 s for a heartbeat"; sleep 75

echo "== 7 the files after the purge of A"
fetch s7; F7=$(facts s7); echo "$F7" | grep -vE 'hash'
same "7 A's old records gone" "$(fact "$F7" a_old)" "0"
same "7 A's recent records kept" "$(fact "$F7" a_recent)" "$(fact "$F3" a_recent)"
same "7 A's old journal entries gone" "$(fact "$F7" a_j_alert_old) $(fact "$F7" a_j_threshold_old) $(fact "$F7" a_j_silent_old)" "0 0 0"
[ "$(fact "$F7" a_j_recent)" -ge "$(fact "$F3" a_j_recent)" ] && verdict "7 A's recent journal entries kept" ok || verdict "7 A's recent journal entries kept" no
same "7 B's store rows unchanged" "$(fact "$F7" b_hash)" "$(fact "$F3" b_hash)"
same "7 B's journal entries unchanged" "$(fact "$F7" b_j_hash)" "$(fact "$F3" b_j_hash)"
same "7 no purge record: store meta keys" "$(fact "$F7" store_meta)" "$(fact "$F3" store_meta)"
same "7 no purge record: journal meta keys" "$(fact "$F7" journal_meta)" "$(fact "$F3" journal_meta)"
same "7 no purge record: tables" "$(fact "$F7" tables)" "$(fact "$F3" tables)"
same "7 no purge record: compaction rows" "$(fact "$F7" compaction_rows)" "$(fact "$F3" compaction_rows)"
[ "$(fact "$F7" j_heartbeats)" -gt "$HB0" ] && verdict "7 monitoring went on: a new heartbeat ($HB0 -> $(fact "$F7" j_heartbeats))" ok || verdict "7 monitoring went on: a new heartbeat" no

if [ -n "$SQL" ]; then
  echo "== 8 purge of B with the journal's write lock held: the journal part fails, then the repeat"
  U "mkdir -p /tmp/lmlib"
  scp -q $OPTS -P 2223 "$SQL/sqlite3-i486" defaultuser@localhost:/tmp/lm-sqlite3 && scp -q $OPTS -P 2223 "$SQL/libreadline.so.8" defaultuser@localhost:/tmp/lmlib/ && U "chmod 755 /tmp/lm-sqlite3"
  U "nohup sh -c '(echo \"BEGIN EXCLUSIVE;\"; sleep 25; echo \"ROLLBACK;\") | LD_LIBRARY_PATH=/tmp/lmlib /tmp/lm-sqlite3 $DATA/journal.db' > /tmp/lm-lock.log 2>&1 &"
  sleep 2
  monstart
  T0=$(date +%s.%N)
  same "8 the purge of B starts" "$(start purge "{\"subject\":\"{$B}\",\"age_months\":3,\"typed_label\":\"BB\"}")" ""
  monwait f1; T1=$(date +%s.%N); F1R=$(finished f1)
  echo "seconds from the start call to the failed run's end seen: $(python3 -I -c "print(round($T1-$T0,1))")"
  echo "first: class=$(j "$F1R" 2) store_part_done=$(j "$F1R" 5.store_part_done) removed=$(j "$F1R" 5.removed) journal_removed=$(j "$F1R" 5.journal_removed)"
  same "8 the journal part failed with io" "$(j "$F1R" 1) $(j "$F1R" 2)" "False io"
  same "8 the store part is done" "$(j "$F1R" 5.store_part_done)" "True"
  sleep 25; echo "lock log: $(U 'cat /tmp/lm-lock.log; rm -f /tmp/lm-lock.log' | head -3)"
  same "8 the service survived the held journal lock, same process" "$(R "$SVC")" "$P2"
  HB1=$(fact "$(fetch s8h; facts s8h)" j_heartbeats); echo "waiting 70 s for a heartbeat after the lock"; sleep 70
  HB2=$(fact "$(fetch s8i; facts s8i)" j_heartbeats)
  [ "$HB2" -gt "$HB1" ] && verdict "8 heartbeats go on after the lock ($HB1 -> $HB2)" ok || verdict "8 heartbeats go on after the lock ($HB1 -> $HB2)" no
  fetch s8a; F8A=$(facts s8a)
  same "8 B's old records gone" "$(fact "$F8A" b_old)" "0"
  same "8 B's journal unchanged by the failed part" "$(fact "$F8A" b_j_hash)" "$(fact "$F3" b_j_hash)"
  monstart
  same "8 the repeat starts" "$(start purge "{\"subject\":\"{$B}\",\"age_months\":3,\"typed_label\":\"BB\"}")" ""
  monwait f2; F2R=$(finished f2)
  echo "repeat: ok=$(j "$F2R" 1) removed=$(j "$F2R" 5.removed) journal_removed=$(j "$F2R" 5.journal_removed)"
  same "8 the repeat finished ok" "$(j "$F2R" 1)" "True"
  same "8 the repeat removed nothing more from the store" "$(j "$F2R" 5.removed_total)" "$(j "$F2R" 5.journal_removed | python3 -I -c 'import json,sys; print(sum(json.load(sys.stdin).values()))')"
  monstart
  start purge "{\"subject\":\"{$B}\",\"age_months\":3,\"typed_label\":\"BB\"}" >/dev/null
  monwait f3; F3R=$(finished f3)
  same "8 a third run finds nothing" "$(j "$F3R" 1) $(j "$F3R" 5.removed_total)" "True 0"
  fetch s8; F8=$(facts s8)
  same "8 B's old journal entries gone" "$(fact "$F8" b_j_alert_old) $(fact "$F8" b_j_threshold_old) $(fact "$F8" b_j_silent_old)" "0 0 0"
  same "8 A unchanged by B's purge" "$(fact "$F8" a_hash) $(fact "$F8" a_old) $(fact "$F8" a_recent)" "$(fact "$F7" a_hash) 0 $(fact "$F7" a_recent)"
  U "rm -rf /tmp/lm-sqlite3 /tmp/lmlib"
fi

echo "== 9 clean up (rpm -e; the configuration and the synthetic data are removed)"
R "rpm -e harbour-libre-monitor; rm -f /tmp/lm-old.rpm /tmp/lm-new.rpm"
sleep 70
U "rm -f $DATA/store.db* $DATA/journal.db* $DATA/settings.json $CONF/config.json"
echo "FAILS=$fail"
echo TEST_DONE
