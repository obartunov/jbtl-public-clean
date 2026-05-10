--
-- jsonb_toaster_lite update-side v1 regression coverage.
--
-- Tests T1..T8 from the update-side v1 boundary note.  Asserts
-- semantic and storage-shape facts only, not WAL bytes.
--
--   T1  eligible same-length scalar replace at top-level key
--   T2  length-changing scalar replace
--   T3  key add / key remove
--   T4  nested / container value
--   T5  DIFF-on-DIFF
--   T6  rollback
--   T7  VACUUM after eligible UPDATE
--   T8  VACUUM FULL after eligible UPDATE
--
-- T9 (pg_dump round-trip) and T10 (cluster restart) are out of
-- regression scope per the milestone direction.
--

\set ON_ERROR_STOP on

CREATE EXTENSION IF NOT EXISTS toastapi;
CREATE EXTENSION IF NOT EXISTS jsonb_toaster_lite;

-- Fixture: a single 5-row table whose `jb` column is CUSTOM-toasted.
-- Body is large enough to be pushed out-of-line (>4 KB after the
-- jsonb encoding step), so storage mode is JBTL_POINTER, which is
-- the v1 happy path.

DROP TABLE IF EXISTS crit_u1 CASCADE;
CREATE TABLE crit_u1 (id int PRIMARY KEY, jb jsonb STORAGE EXTERNAL);
SELECT pgpro_toast.set_toaster('jsonb_toaster_lite', 'crit_u1', 'jb') > 0
       AS attached;

SET jsonb_sort_field_values = off;

INSERT INTO crit_u1 SELECT g, jsonb_build_object(
    'id',       42,
    'status',   'STATUS_active',
    'kind',     'KIND_invoice',
    'tenant',   'TENANT_acme',
    'title',    'TITLE_' || repeat('t', 200),
    'summary',  'SUMMARY_' || repeat('s', 1000),
    'payload',  repeat('P', 6000),
    'flags',    jsonb_build_array('flag_a', 'flag_b', 'flag_c')
) FROM generate_series(1, 5) g;
RESET jsonb_sort_field_values;

ANALYZE crit_u1;

-- Capture the pre-update fingerprint per row.
CREATE TEMP TABLE u1_pre AS
SELECT id, md5(jb::text) AS pre_md5, jb::text AS pre_text
  FROM crit_u1
 ORDER BY id;

-- Storage smoke: every row should be CUSTOM-toasted (pg_column_size
-- of the in-row CUSTOM varlena is small relative to the materialized
-- text, since chunks are out of line).
SELECT id,
       pg_column_size(jb) < length(jb::text) / 10 AS is_custom_toasted
  FROM crit_u1
 ORDER BY id;

-- Reset diagnostic counters.
SELECT jbtl_update_calls_reset();
SELECT jbtl_update_calls() AS calls_initial,
       jbtl_update_diffs_emitted() AS diffs_initial;

-- ============================================================
-- T1  eligible same-length scalar replace at top-level key
--     'STATUS_active' (13 chars) -> 'STATUS_closed' (13 chars)
-- ============================================================
UPDATE crit_u1
   SET jb = jsonb_set(jb, '{status}', '"STATUS_closed"'::jsonb)
 WHERE id = 1;

-- Counters: exactly one tsr_update call, exactly one DIFF emission.
SELECT jbtl_update_calls() AS calls_after_t1,
       jbtl_update_diffs_emitted() AS diffs_after_t1;

-- Logical value is correct and reads back through the read path.
SELECT id,
       jb -> 'status' AS new_status,
       jb -> 'kind'   AS unchanged_kind
  FROM crit_u1
 WHERE id = 1;

-- Other rows unchanged.
SELECT id,
       md5(jb::text) = (SELECT pre_md5 FROM u1_pre p WHERE p.id = c.id)
       AS unchanged
  FROM crit_u1 c
 WHERE id <> 1
 ORDER BY id;

-- ============================================================
-- T2  length-changing scalar replace
--     'KIND_invoice' (12 chars) -> 'KIND_paid' (9 chars)
--     Expect: no DIFF emitted; value still correct.
-- ============================================================
SELECT jbtl_update_calls() AS calls_before_t2,
       jbtl_update_diffs_emitted() AS diffs_before_t2;

UPDATE crit_u1
   SET jb = jsonb_set(jb, '{kind}', '"KIND_paid"'::jsonb)
 WHERE id = 2;

SELECT jbtl_update_calls() AS calls_after_t2,
       jbtl_update_diffs_emitted() AS diffs_after_t2;
-- Expect calls +1 (hook fired), diffs +0 (declined; core fallback).

SELECT id, jb -> 'kind' AS new_kind FROM crit_u1 WHERE id = 2;

-- ============================================================
-- T3  key add / key remove
--     Expect: no DIFF emitted; value correct.
-- ============================================================
SELECT jbtl_update_calls() AS calls_before_t3,
       jbtl_update_diffs_emitted() AS diffs_before_t3;

-- 3a: add a top-level key.
UPDATE crit_u1
   SET jb = jb || jsonb_build_object('extra_key', 'extra_value')
 WHERE id = 3;

-- 3b: remove a top-level key.
UPDATE crit_u1
   SET jb = jb - 'tenant'
 WHERE id = 4;

SELECT jbtl_update_calls() AS calls_after_t3,
       jbtl_update_diffs_emitted() AS diffs_after_t3;
-- Expect calls +2 (both fired), diffs +0 (both declined; key set changed).

SELECT id,
       jb ? 'extra_key'  AS has_extra_key,
       jb ? 'tenant'     AS has_tenant
  FROM crit_u1
 WHERE id IN (3, 4)
 ORDER BY id;

-- ============================================================
-- T4  nested / container value (target inside an array)
--     Expect: no DIFF emitted; value correct.
-- ============================================================
SELECT jbtl_update_calls() AS calls_before_t4,
       jbtl_update_diffs_emitted() AS diffs_before_t4;

-- Replace one array element with a same-length string.  The diff
-- range is byte-aligned to a value position INSIDE the 'flags'
-- array (a container).  jbtl_update's top-level scalar bound must
-- decline.
UPDATE crit_u1
   SET jb = jsonb_set(jb, '{flags,1}', '"flag_X"'::jsonb)
 WHERE id = 5;

SELECT jbtl_update_calls() AS calls_after_t4,
       jbtl_update_diffs_emitted() AS diffs_after_t4;
-- Expect calls +1, diffs +0.

SELECT id, jb -> 'flags' AS flags FROM crit_u1 WHERE id = 5;

-- ============================================================
-- T5  DIFF-on-DIFF
--     First eligible same-length update on row 1 already in DIFF
--     mode (after T1).  Second same-length update should DECLINE
--     (DIFF-on-DIFF refused), value still correct.
-- ============================================================
SELECT jbtl_update_calls() AS calls_before_t5,
       jbtl_update_diffs_emitted() AS diffs_before_t5;

-- Row 1 is currently in DIFF mode after T1.
-- Now apply another same-length scalar replace at the same key.
-- 'STATUS_closed' -> 'STATUS_purged' (13 chars -> 13 chars).
UPDATE crit_u1
   SET jb = jsonb_set(jb, '{status}', '"STATUS_purged"'::jsonb)
 WHERE id = 1;

SELECT jbtl_update_calls() AS calls_after_t5,
       jbtl_update_diffs_emitted() AS diffs_after_t5;
-- Expect calls +1 (hook fires), diffs +0 (DIFF-on-DIFF declined;
-- core falls back to delete-old + toast-new, leaving row 1 in
-- POINTER mode again).

SELECT id, jb -> 'status' AS final_status FROM crit_u1 WHERE id = 1;

-- ============================================================
-- T6  rollback
--     BEGIN; eligible UPDATE; ROLLBACK.
--     Value must return to pre-update state.
-- ============================================================

-- Reset row 1 fingerprint after the T1+T5 sequence as the new "pre".
DROP TABLE u1_pre;
CREATE TEMP TABLE u1_pre AS
SELECT id, md5(jb::text) AS pre_md5, jb::text AS pre_text
  FROM crit_u1
 ORDER BY id;

-- Reset counters before the transactional probe.
SELECT jbtl_update_calls_reset();

BEGIN;
  UPDATE crit_u1
     SET jb = jsonb_set(jb, '{title}', ('"TITLE_' || repeat('q', 200) || '"')::jsonb)
   WHERE id = 1;
  -- Inside the txn, the value reflects the update.
  SELECT id,
         (jb -> 'title')::text LIKE '%qqq%' AS title_changed_in_txn
    FROM crit_u1 WHERE id = 1;
ROLLBACK;

-- After rollback, value must equal pre-update fingerprint.
SELECT id,
       md5(jb::text) = (SELECT pre_md5 FROM u1_pre p WHERE p.id = c.id)
       AS rolled_back_to_pre
  FROM crit_u1 c
 WHERE id = 1;

-- ============================================================
-- T7  VACUUM after eligible UPDATE
--     Apply an eligible same-length update on row 2 (currently
--     POINTER after T2's fallback re-toast).  Then VACUUM.  Value
--     must be byte-identical to the pre-VACUUM read.
-- ============================================================

-- Eligible same-length update on row 2: 'KIND_paid' (9) -> 'KIND_done' (9).
UPDATE crit_u1
   SET jb = jsonb_set(jb, '{kind}', '"KIND_done"'::jsonb)
 WHERE id = 2;

-- Capture post-update fingerprint.
CREATE TEMP TABLE u1_post AS
SELECT id, md5(jb::text) AS post_md5
  FROM crit_u1
 ORDER BY id;

VACUUM crit_u1;

SELECT id,
       md5(jb::text) = (SELECT post_md5 FROM u1_post p WHERE p.id = c.id)
       AS preserved_through_vacuum
  FROM crit_u1 c
 ORDER BY id;

-- ============================================================
-- T8  VACUUM FULL after eligible UPDATE
--     VACUUM FULL rewrites both heap and toast; tsr_copy fires.
--     Storage mode may normalize (DIFF -> POINTER), but logical
--     value must be byte-identical.
-- ============================================================

VACUUM FULL crit_u1;

SELECT id,
       md5(jb::text) = (SELECT post_md5 FROM u1_post p WHERE p.id = c.id)
       AS preserved_through_vacuum_full
  FROM crit_u1 c
 ORDER BY id;

-- Cleanup.
DROP TABLE u1_post;
DROP TABLE u1_pre;
DROP TABLE crit_u1 CASCADE;
