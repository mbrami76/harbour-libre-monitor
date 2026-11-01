-- SPDX-License-Identifier: GPL-3.0-or-later
-- Copyright (C) 2026 Michele Brami
--
-- Fixture store of schema version 2 (REQ-STO-10): the schema exactly as the application wrote it
-- and before that change (core/store/store.cpp at commit 7fdb895), with synthetic rows of every
-- table: those of the v1 fixture (the old compaction record keeps its archive), plus a compaction
-- record without an archive as schema 2 writes it. tests/tst_storemigration opens it with the current
-- Store. One statement per line; never edit this file to follow a later schema.
CREATE TABLE meta(key TEXT PRIMARY KEY, value TEXT NOT NULL) WITHOUT ROWID;
CREATE TABLE subject(id TEXT PRIMARY KEY) WITHOUT ROWID;
CREATE TABLE record( subject TEXT NOT NULL, metric TEXT NOT NULL, measured_at INTEGER NOT NULL, source TEXT NOT NULL, entry_id TEXT NOT NULL DEFAULT '', shape INTEGER NOT NULL, value REAL, payload TEXT, duration_ms INTEGER, unit TEXT, ingested_at INTEGER NOT NULL, original_timestamp TEXT NOT NULL, origin TEXT NOT NULL, rule_version TEXT, PRIMARY KEY(subject, metric, measured_at, source, entry_id)) WITHOUT ROWID;
CREATE TABLE mark( id TEXT PRIMARY KEY, time INTEGER NOT NULL, t_subject TEXT NOT NULL, t_metric TEXT NOT NULL, t_measured_at INTEGER NOT NULL, t_source TEXT NOT NULL, t_entry_id TEXT NOT NULL DEFAULT '', state TEXT NOT NULL, comment TEXT NOT NULL, author TEXT) WITHOUT ROWID;
CREATE INDEX mark_target ON mark(t_subject, t_metric, t_measured_at, t_source, t_entry_id);
CREATE TABLE sensor( subject TEXT NOT NULL, sensor_id TEXT NOT NULL, activated_at INTEGER, warmup_minutes INTEGER, model TEXT, first_seen INTEGER NOT NULL, last_seen INTEGER NOT NULL, PRIMARY KEY(subject, sensor_id)) WITHOUT ROWID;
CREATE TABLE quarantine( id TEXT PRIMARY KEY, received_at INTEGER NOT NULL, adapter TEXT NOT NULL, adapter_version TEXT NOT NULL, reason TEXT NOT NULL, detail TEXT NOT NULL, record TEXT NOT NULL) WITHOUT ROWID;
CREATE TABLE ingest_log( id TEXT PRIMARY KEY, time INTEGER NOT NULL, event TEXT NOT NULL, adapter TEXT NOT NULL, adapter_version TEXT NOT NULL, session_id TEXT NOT NULL, detail TEXT NOT NULL, extra TEXT NOT NULL) WITHOUT ROWID;
CREATE TABLE compaction( id TEXT PRIMARY KEY, time INTEGER NOT NULL, range_from INTEGER NOT NULL, range_to INTEGER NOT NULL, removed TEXT NOT NULL, archive_id TEXT, archive_sha256 TEXT, rule_version TEXT NOT NULL) WITHOUT ROWID;
CREATE TABLE reimport( id TEXT PRIMARY KEY, time INTEGER NOT NULL, archive_id TEXT NOT NULL, archive_sha256 TEXT NOT NULL, format_version TEXT NOT NULL, counts TEXT NOT NULL) WITHOUT ROWID;
CREATE INDEX mark_time ON mark(time, id);
CREATE INDEX quarantine_time ON quarantine(received_at, id);
CREATE INDEX ingest_log_time ON ingest_log(time, id);
CREATE TRIGGER record_no_update BEFORE UPDATE ON record BEGIN SELECT RAISE(ABORT, 'record is append-only'); END;
CREATE TRIGGER mark_no_update BEFORE UPDATE ON mark BEGIN SELECT RAISE(ABORT, 'mark is append-only'); END;
CREATE TRIGGER quarantine_no_update BEFORE UPDATE ON quarantine BEGIN SELECT RAISE(ABORT, 'quarantine is append-only'); END;
CREATE TRIGGER ingest_log_no_update BEFORE UPDATE ON ingest_log BEGIN SELECT RAISE(ABORT, 'ingest log is append-only'); END;
CREATE TRIGGER record_no_delete BEFORE DELETE ON record WHEN IFNULL((SELECT value FROM meta WHERE key='compacting'), '0') <> '1' BEGIN SELECT RAISE(ABORT, 'records are deleted only by compaction'); END;
CREATE TRIGGER quarantine_no_delete BEFORE DELETE ON quarantine WHEN IFNULL((SELECT value FROM meta WHERE key='compacting'), '0') <> '1' BEGIN SELECT RAISE(ABORT, 'quarantine is deleted only by compaction'); END;
CREATE TRIGGER ingest_log_no_delete BEFORE DELETE ON ingest_log WHEN IFNULL((SELECT value FROM meta WHERE key='compacting'), '0') <> '1' BEGIN SELECT RAISE(ABORT, 'the ingest log is deleted only by compaction'); END;
CREATE TRIGGER mark_no_delete BEFORE DELETE ON mark BEGIN SELECT RAISE(ABORT, 'marks are never deleted'); END;
CREATE TRIGGER sensor_no_delete BEFORE DELETE ON sensor BEGIN SELECT RAISE(ABORT, 'sensor records are never deleted'); END;
CREATE TRIGGER compaction_no_delete BEFORE DELETE ON compaction BEGIN SELECT RAISE(ABORT, 'compaction records are never deleted'); END;
CREATE TRIGGER reimport_no_delete BEFORE DELETE ON reimport BEGIN SELECT RAISE(ABORT, 'reimport records are never deleted'); END;
CREATE TRIGGER compaction_no_update BEFORE UPDATE ON compaction BEGIN SELECT RAISE(ABORT, 'compaction records are append-only'); END;
CREATE TRIGGER reimport_no_update BEFORE UPDATE ON reimport BEGIN SELECT RAISE(ABORT, 'reimport records are append-only'); END;
INSERT INTO meta VALUES('store_schema', '2');
INSERT INTO subject VALUES('d2f1c9a0-5b3e-4c7d-8a91-0e2b4c6d8f10');
INSERT INTO record VALUES('d2f1c9a0-5b3e-4c7d-8a91-0e2b4c6d8f10', 'glucose', 1769904000000, 'libre.current', '', 0, 100.0, NULL, NULL, 'mg/dL', 1769904600000, '2/1/2026 12:00:00 AM', 'adapter', NULL);
INSERT INTO record VALUES('d2f1c9a0-5b3e-4c7d-8a91-0e2b4c6d8f10', 'glucose', 1769904060000, 'libre.current', '', 0, 140.0, NULL, NULL, 'mg/dL', 1769904600000, '2/1/2026 12:01:00 AM', 'adapter', NULL);
INSERT INTO record VALUES('d2f1c9a0-5b3e-4c7d-8a91-0e2b4c6d8f10', 'glucose', 1769904120000, 'libre.current', '', 0, 90.0, NULL, NULL, 'mg/dL', 1769904600000, '2/1/2026 12:02:00 AM', 'adapter', NULL);
INSERT INTO record VALUES('d2f1c9a0-5b3e-4c7d-8a91-0e2b4c6d8f10', 'glucose', 1769904180000, 'libre.current', '', 0, 120.0, NULL, NULL, 'mg/dL', 1769904600000, '2/1/2026 12:03:00 AM', 'adapter', NULL);
INSERT INTO record VALUES('d2f1c9a0-5b3e-4c7d-8a91-0e2b4c6d8f10', 'glucose', 1769904240000, 'libre.current', '', 0, 110.0, NULL, NULL, 'mg/dL', 1769904600000, '2/1/2026 12:04:00 AM', 'adapter', NULL);
INSERT INTO record VALUES('d2f1c9a0-5b3e-4c7d-8a91-0e2b4c6d8f10', 'glucose', 1769904300000, 'libre.current', '', 0, 130.0, NULL, NULL, 'mg/dL', 1769904600000, '2/1/2026 12:05:00 AM', 'adapter', NULL);
INSERT INTO record VALUES('d2f1c9a0-5b3e-4c7d-8a91-0e2b4c6d8f10', 'logbook_entry', 1769904420000, 'libre.logbook', '', 1, NULL, '{"entry_type":3,"raw":{}}', NULL, NULL, 1769904600000, '2/1/2026 12:07:00 AM', 'adapter', NULL);
INSERT INTO mark VALUES('6f0c1a2b-3c4d-4e5f-8a6b-7c8d9e0f1a2b', 1769907600000, 'd2f1c9a0-5b3e-4c7d-8a91-0e2b4c6d8f10', 'glucose', 1769904180000, 'libre.current', '', 'ignored', 'synthetic comment', NULL);
INSERT INTO sensor VALUES('d2f1c9a0-5b3e-4c7d-8a91-0e2b4c6d8f10', 'f3a9c2', 1769817600000, 60, NULL, 1769904000000, 1769904300000);
INSERT INTO quarantine VALUES('1a2b3c4d-5e6f-4a7b-8c9d-0e1f2a3b4c5d', 1769904060000, 'libre', '1.0.0', 'UnknownMetric', 'metric', '{"a":1}');
INSERT INTO ingest_log VALUES('2b3c4d5e-6f7a-4b8c-9d0e-1f2a3b4c5d6e', 1769904120000, 'violation', 'libre', '1.0.0', 's-1', 'synthetic', '{}');
INSERT INTO compaction VALUES('3c4d5e6f-7a8b-4c9d-8e0f-2a3b4c5d6e7f', 1769817600000, 1762128000000, 1762214400000, '{"record":12}', '4d5e6f7a-8b9c-4d0e-9f1a-3b4c5d6e7f8a', 'abababababababababababababababababababababababababababababababab', '1');
INSERT INTO reimport VALUES('5e6f7a8b-9c0d-4e1f-8a2b-4c5d6e7f8a9b', 1769860800000, '4d5e6f7a-8b9c-4d0e-9f1a-3b4c5d6e7f8a', 'abababababababababababababababababababababababababababababababab', '1.0', '{"added":{"record":12}}');
INSERT INTO compaction(id, time, range_from, range_to, removed, rule_version) VALUES('6f7a8b9c-0d1e-4f2a-9b3c-5d6e7f8a9b0c', 1769990400000, 1762300800000, 1762387200000, '{"record":4}', '2');
