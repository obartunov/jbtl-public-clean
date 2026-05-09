--
-- L2.1b: DIFF emission for top-level same-length scalar.
--
-- Three scenarios are tested:
--   A: key3 100 -> 99 (same-byte-length top-level scalar)
--      → DIFF emitted; base toast rows unchanged; correctness vs baseline.
--   B: key3 100 -> 99999 (length-changing scalar)
--      → tsr_update declines (return 0); fallback path runs full retoast.
--   C: key2[0] -> "DEADBEEF...DEADBEEF" (same-length array element)
--      → tsr_update declines (diff lands inside a JBE_ISCONTAINER value).
--      Critically: NOT a silent-data-loss bug like postgrespro's historical
--      jsonb_toaster has on the same scenario.
--

\set ON_ERROR_STOP on

CREATE EXTENSION IF NOT EXISTS toastapi;
CREATE EXTENSION IF NOT EXISTS jsonb_toaster_lite;

SET jsonb_sort_field_values = on;
SET jsonb_toaster_lite.compress_chunks = off;

CREATE TABLE l21b_a (id int PRIMARY KEY, jb jsonb STORAGE EXTERNAL);
CREATE TABLE l21b_b (id int PRIMARY KEY, jb jsonb STORAGE EXTERNAL);
CREATE TABLE l21b_c (id int PRIMARY KEY, jb jsonb STORAGE EXTERNAL);
CREATE TABLE l21b_baseline_a (id int PRIMARY KEY, jb jsonb);
CREATE TABLE l21b_baseline_b (id int PRIMARY KEY, jb jsonb);
CREATE TABLE l21b_baseline_c (id int PRIMARY KEY, jb jsonb);

SELECT pgpro_toast.set_toaster('jsonb_toaster_lite', 'l21b_a', 'jb') > 0
       AS attached_a;
SELECT pgpro_toast.set_toaster('jsonb_toaster_lite', 'l21b_b', 'jb') > 0
       AS attached_b;
SELECT pgpro_toast.set_toaster('jsonb_toaster_lite', 'l21b_c', 'jb') > 0
       AS attached_c;

-- Common payload: object with key1 (small int), key2 (big array), key3 (small int)
WITH payload AS (
  SELECT jsonb_build_object(
           'key1', 100, 'key3', 100,
           'key2', (SELECT jsonb_agg(md5((100*1000 + s)::text))
                      FROM generate_series(1, 1000) s)) AS jb)
INSERT INTO l21b_a SELECT 1, jb FROM payload;

WITH payload AS (
  SELECT jsonb_build_object(
           'key1', 100, 'key3', 100,
           'key2', (SELECT jsonb_agg(md5((100*1000 + s)::text))
                      FROM generate_series(1, 1000) s)) AS jb)
INSERT INTO l21b_b SELECT 1, jb FROM payload;

WITH payload AS (
  SELECT jsonb_build_object(
           'key1', 100, 'key3', 100,
           'key2', (SELECT jsonb_agg(md5((100*1000 + s)::text))
                      FROM generate_series(1, 1000) s)) AS jb)
INSERT INTO l21b_c SELECT 1, jb FROM payload;

INSERT INTO l21b_baseline_a SELECT 1, jb FROM l21b_a;
INSERT INTO l21b_baseline_b SELECT 1, jb FROM l21b_b;
INSERT INTO l21b_baseline_c SELECT 1, jb FROM l21b_c;

VACUUM ANALYZE l21b_a, l21b_b, l21b_c,
               l21b_baseline_a, l21b_baseline_b, l21b_baseline_c;
CHECKPOINT;

-- ============================================================
-- Scenario A: same-length top-level scalar (key3 100 -> 99)
-- ============================================================
SELECT jbtl_update_calls_reset();

UPDATE l21b_a          SET jb = jsonb_set(jb, '{key3}', '99'::jsonb) WHERE id=1;
UPDATE l21b_baseline_a SET jb = jsonb_set(jb, '{key3}', '99'::jsonb) WHERE id=1;

-- Hook fired exactly once and emitted a DIFF.
SELECT 'pin_a_called_once'    AS what, jbtl_update_calls() = 1 AS pin;
SELECT 'pin_a_diff_emitted'   AS what, jbtl_update_diffs_emitted() = 1 AS pin;

-- Read result equals jsonb_set baseline byte-for-byte (text round-trip).
SELECT 'pin_a_correctness'    AS what,
       (SELECT jb::text FROM l21b_a) =
       (SELECT jb::text FROM l21b_baseline_a) AS pin;

SELECT 'pin_a_key1_unchanged' AS what,
       (SELECT jb->'key1' FROM l21b_a) =
       (SELECT jb->'key1' FROM l21b_baseline_a) AS pin;
SELECT 'pin_a_key2_unchanged' AS what,
       (SELECT jb->'key2' FROM l21b_a) =
       (SELECT jb->'key2' FROM l21b_baseline_a) AS pin;
SELECT 'pin_a_key3_changed'   AS what,
       (SELECT (jb->>'key3')::int FROM l21b_a) = 99 AS pin;

-- ============================================================
-- Scenario B: length-changing scalar (key3 100 -> 99999)
-- ============================================================
SELECT jbtl_update_calls_reset();

UPDATE l21b_b          SET jb = jsonb_set(jb, '{key3}', '99999'::jsonb) WHERE id=1;
UPDATE l21b_baseline_b SET jb = jsonb_set(jb, '{key3}', '99999'::jsonb) WHERE id=1;

-- Hook fired but declined — no DIFF.
SELECT 'pin_b_called_once'    AS what, jbtl_update_calls() = 1 AS pin;
SELECT 'pin_b_no_diff'        AS what, jbtl_update_diffs_emitted() = 0 AS pin;

-- Result still correct (via fallback retoast).
SELECT 'pin_b_correctness'    AS what,
       (SELECT jb::text FROM l21b_b) =
       (SELECT jb::text FROM l21b_baseline_b) AS pin;
SELECT 'pin_b_key3_99999'     AS what,
       (SELECT (jb->>'key3')::int FROM l21b_b) = 99999 AS pin;

-- ============================================================
-- Scenario C: same-length array element (key2[0])
-- ============================================================
SELECT jbtl_update_calls_reset();

UPDATE l21b_c          SET jb = jsonb_set(jb, '{key2,0}',
                                          to_jsonb(repeat('DEADBEEF', 4))) WHERE id=1;
UPDATE l21b_baseline_c SET jb = jsonb_set(jb, '{key2,0}',
                                          to_jsonb(repeat('DEADBEEF', 4))) WHERE id=1;

-- Hook fired but declined — diff lands inside a JBE_ISCONTAINER value.
SELECT 'pin_c_called_once'    AS what, jbtl_update_calls() = 1 AS pin;
SELECT 'pin_c_no_diff'        AS what, jbtl_update_diffs_emitted() = 0 AS pin;

-- Result still correct (via fallback retoast).  This is the critical
-- pin: postgrespro's historical jsonb_toaster has a silent data-loss
-- bug on the same scenario; we MUST not reproduce that.
SELECT 'pin_c_correctness'    AS what,
       (SELECT jb::text FROM l21b_c) =
       (SELECT jb::text FROM l21b_baseline_c) AS pin;
SELECT 'pin_c_key2_0_new'     AS what,
       (SELECT jb->'key2'->>0 FROM l21b_c) = repeat('DEADBEEF', 4) AS pin;

-- ============================================================
-- Single-shot rebase: a second UPDATE on a row that already has
-- a DIFF should NOT stack — the second call must decline so core
-- rebases via fallback.
-- ============================================================
SELECT jbtl_update_calls_reset();

UPDATE l21b_a          SET jb = jsonb_set(jb, '{key3}', '88'::jsonb) WHERE id=1;
UPDATE l21b_baseline_a SET jb = jsonb_set(jb, '{key3}', '88'::jsonb) WHERE id=1;

SELECT 'pin_single_shot_called'  AS what, jbtl_update_calls() = 1 AS pin;
SELECT 'pin_single_shot_no_diff' AS what, jbtl_update_diffs_emitted() = 0 AS pin;

SELECT 'pin_single_shot_correct' AS what,
       (SELECT jb::text FROM l21b_a) =
       (SELECT jb::text FROM l21b_baseline_a) AS pin;
SELECT 'pin_single_shot_key3_88' AS what,
       (SELECT (jb->>'key3')::int FROM l21b_a) = 88 AS pin;

DROP TABLE l21b_a, l21b_b, l21b_c,
           l21b_baseline_a, l21b_baseline_b, l21b_baseline_c;
