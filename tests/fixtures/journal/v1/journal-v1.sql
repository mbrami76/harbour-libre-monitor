-- SPDX-License-Identifier: GPL-3.0-or-later
-- Copyright (C) 2026 Michele Brami
--
-- Fixture service journal of schema version 1 (REQ-STO-10): the schema exactly as the application wrote it
-- before that change (core/store/journal.cpp at commit 8b90c23, no delete guard), with synthetic entries of
-- two Subjects: alert, threshold (one with the comment of before that change) and silent, and service events.
-- tests/tst_journal opens it with the current Journal. One statement per line; never edit it to follow a later schema.
CREATE TABLE meta(key TEXT PRIMARY KEY, value TEXT NOT NULL) WITHOUT ROWID;
CREATE TABLE journal( id TEXT PRIMARY KEY, kind TEXT NOT NULL, wall_time INTEGER NOT NULL, boot_time_ms INTEGER, monotonic_ms INTEGER, cause TEXT, range_from INTEGER, range_to INTEGER, removed INTEGER, extra TEXT NOT NULL) WITHOUT ROWID;
CREATE INDEX journal_time ON journal(wall_time, id);
CREATE INDEX journal_kind_time ON journal(kind, wall_time);
CREATE TRIGGER journal_no_update BEFORE UPDATE ON journal BEGIN SELECT RAISE(ABORT, 'the journal is append-only'); END;
INSERT INTO meta VALUES('store_schema', '1');
INSERT INTO journal VALUES('10000000-0000-4000-8000-000000000001', 'start', 1769904000000, 1000, 200, 'after-stop', NULL, NULL, NULL, '{}');
INSERT INTO journal VALUES('10000000-0000-4000-8000-000000000002', 'heartbeat', 1769904060000, 61000, 60200, NULL, NULL, NULL, NULL, '{}');
INSERT INTO journal VALUES('20000000-0000-4000-8000-000000000001', 'alert', 1769907600000, NULL, NULL, NULL, NULL, NULL, NULL, '{"acknowledged_in_episode":false,"alert":"L","episode":"30000000-0000-4000-8000-000000000001","event":"pending","from":"normal","seq":1,"since":"2026-02-01T01:00:00.000Z","slot":"low","state":"pending","subject":"d2f1c9a0-5b3e-4c7d-8a91-0e2b4c6d8f10"}');
INSERT INTO journal VALUES('40000000-0000-4000-8000-000000000001', 'threshold', 1769907601000, NULL, NULL, NULL, NULL, NULL, NULL, '{"boundary":"L","comment":"written by an older version","previous":70,"subject":"d2f1c9a0-5b3e-4c7d-8a91-0e2b4c6d8f10","value":75}');
INSERT INTO journal VALUES('50000000-0000-4000-8000-000000000001', 'silent', 1769907602000, NULL, NULL, NULL, NULL, NULL, NULL, '{"event":"start","seq":1,"subject":"d2f1c9a0-5b3e-4c7d-8a91-0e2b4c6d8f10"}');
INSERT INTO journal VALUES('20000000-0000-4000-8000-000000000002', 'alert', 1769911200000, NULL, NULL, NULL, NULL, NULL, NULL, '{"acknowledged_in_episode":false,"alert":"L","episode":"30000000-0000-4000-8000-000000000002","event":"pending","from":"normal","seq":2,"since":"2026-02-01T02:00:00.000Z","slot":"low","state":"pending","subject":"0a1b2c3d-4e5f-4a6b-8c7d-9e0f1a2b3c4d"}');
INSERT INTO journal VALUES('40000000-0000-4000-8000-000000000002', 'threshold', 1769911201000, NULL, NULL, NULL, NULL, NULL, NULL, '{"boundary":"L","comment":"written by an older version","previous":70,"subject":"0a1b2c3d-4e5f-4a6b-8c7d-9e0f1a2b3c4d","value":75}');
INSERT INTO journal VALUES('50000000-0000-4000-8000-000000000002', 'silent', 1769911202000, NULL, NULL, NULL, NULL, NULL, NULL, '{"event":"start","seq":2,"subject":"0a1b2c3d-4e5f-4a6b-8c7d-9e0f1a2b3c4d"}');
INSERT INTO journal VALUES('10000000-0000-4000-8000-000000000003', 'stop', 1769911200000, 7201000, 7200200, NULL, NULL, NULL, NULL, '{}');
