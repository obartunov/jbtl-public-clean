--
-- M9.2 narrow SUBTREE sub-object reuse (M-B v0).
--
-- Verifies that when an UPDATE touches only a top-level scalar/inline
-- sibling of a SUBTREE-managed child, the child's toast chain is
-- reused across the UPDATE instead of being re-toasted.
--
-- Pinned invariants:
--   - logical value after UPDATE equals ordinary jsonb result
--   - reused SUBTREE child's child_valueid is stable across UPDATE
--   - edge count stays bounded across repeated reuses
--   - jbtl_subtree_refs_check() stays clean
--   - DELETE + VACUUM cleans refs
--   - ROLLBACK preserves old refs state
--   - VACUUM FULL safety gate still refuses while live SUBTREE rows
--     exist; after recovery (text-cast UPDATE under GUC off), VACUUM
--     FULL succeeds.
--
-- Sections:
--   T1  baseline reuse: one UPDATE changing an unrelated scalar
--   T2  10 repeated scalar updates, same child stays reused
--   T3  changed-payload fallback: UPDATE replaces the large child
--   T4  rollback: BEGIN; UPDATE with reuse; ROLLBACK preserves state
--   T5  DELETE + VACUUM removes the row's refs
--   T6  VACUUM FULL gate still active; recovery + post-recovery
--       VACUUM FULL succeeds
--

\set ON_ERROR_STOP on

-- Deterministic starting state.
SET client_min_messages = warning;
CREATE EXTENSION IF NOT EXISTS toastapi;
CREATE EXTENSION IF NOT EXISTS jsonb_toaster_lite;
RESET client_min_messages;

SET jsonb_sort_field_values = off;
SET jsonb_toaster_lite.enable_subtree_storage = on;
SET jsonb_toaster_lite.subtree_spill_threshold = 4096;

SELECT jbtl_update_calls_reset();
SELECT jbtl_subtree_refs_gc() AS noise_cleared;

-- ============================================================
-- T1  baseline reuse
-- ============================================================

DROP TABLE IF EXISTS m92_t1 CASCADE;
CREATE TABLE m92_t1 (id int PRIMARY KEY, jb jsonb STORAGE EXTERNAL);
SELECT pgpro_toast.set_toaster('jsonb_toaster_lite', 'm92_t1', 'jb') > 0
       AS attached;

INSERT INTO m92_t1 VALUES (1, jsonb_build_object(
  'id', 1,
  'status', 'open',
  'key2', (SELECT jsonb_agg(md5(s::text)) FROM generate_series(1, 1000) s)));

-- Capture pre-update child identity.
CREATE TEMP TABLE t1_pre AS
SELECT child_valueid AS pre_child_v,
       parent_valueid AS pre_parent_v
  FROM jbtl_subtree_refs r, pg_class c
 WHERE c.relname = 'm92_t1' AND r.parent_toastrelid = c.reltoastrelid;

SELECT 't1_one_edge_before' AS pin,
       (SELECT count(*) FROM jbtl_subtree_refs r, pg_class c
         WHERE c.relname='m92_t1' AND r.parent_toastrelid=c.reltoastrelid) = 1
       AS ok;

-- Fingerprint pre-UPDATE.
CREATE TEMP TABLE t1_md5_pre AS SELECT md5(jb::text) AS h FROM m92_t1 WHERE id=1;

-- UPDATE only the 'status' scalar.
UPDATE m92_t1 SET jb = jsonb_set(jb, '{status}', '"done"') WHERE id = 1;

-- Logical value: status changed; key2 unchanged.
SELECT 't1_status_after' AS pin, jb->'status' = '"done"'::jsonb AS ok FROM m92_t1 WHERE id=1;
SELECT 't1_key2_len_after' AS pin, jsonb_array_length(jb->'key2') = 1000 AS ok FROM m92_t1 WHERE id=1;

-- Child id stable; edge count bounded (still exactly 1).
SELECT 't1_child_reused' AS pin,
       (SELECT child_valueid FROM jbtl_subtree_refs r, pg_class c
         WHERE c.relname='m92_t1' AND r.parent_toastrelid=c.reltoastrelid)
       = (SELECT pre_child_v FROM t1_pre)
       AS ok;
SELECT 't1_edge_count_bounded' AS pin,
       (SELECT count(*) FROM jbtl_subtree_refs r, pg_class c
         WHERE c.relname='m92_t1' AND r.parent_toastrelid=c.reltoastrelid) = 1
       AS ok;

-- Counters reflect one successful reuse.
SELECT 't1_attempts'  AS pin, jbtl_update_subtree_reuse_attempts() = 1 AS ok;
SELECT 't1_successes' AS pin, jbtl_update_subtree_reuse_successes() = 1 AS ok;
SELECT 't1_children'  AS pin, jbtl_update_subtree_children_reused() = 1 AS ok;
SELECT 't1_refs_check_clean' AS pin, jbtl_subtree_refs_check() = 0 AS ok;

DROP TABLE t1_pre, t1_md5_pre;
DROP TABLE m92_t1;
SELECT jbtl_subtree_refs_gc() AS t1_gc;

-- ============================================================
-- T2  10 repeated scalar updates — child stays reused, edges bounded
-- ============================================================

SELECT jbtl_update_calls_reset();

DROP TABLE IF EXISTS m92_t2 CASCADE;
CREATE TABLE m92_t2 (id int PRIMARY KEY, jb jsonb STORAGE EXTERNAL);
SELECT pgpro_toast.set_toaster('jsonb_toaster_lite', 'm92_t2', 'jb') > 0
       AS attached;

INSERT INTO m92_t2 VALUES (1, jsonb_build_object(
  'id', 1,
  'status', 'open',
  'key2', (SELECT jsonb_agg(md5(s::text)) FROM generate_series(1, 1000) s)));

CREATE TEMP TABLE t2_pre AS
SELECT child_valueid AS pre_child_v
  FROM jbtl_subtree_refs r, pg_class c
 WHERE c.relname='m92_t2' AND r.parent_toastrelid=c.reltoastrelid;

-- 10 updates, each touching only 'status'.
DO $$
DECLARE
  i int;
BEGIN
  FOR i IN 1..10 LOOP
    UPDATE m92_t2 SET jb = jsonb_set(jb, '{status}',
      to_jsonb('v' || i::text)) WHERE id = 1;
  END LOOP;
END $$;

-- Final status correct.
SELECT 't2_final_status' AS pin, jb->'status' = '"v10"'::jsonb AS ok FROM m92_t2 WHERE id=1;
SELECT 't2_final_key2_len' AS pin, jsonb_array_length(jb->'key2') = 1000 AS ok FROM m92_t2 WHERE id=1;

-- Child id stable across all 10 updates.
SELECT 't2_child_stable' AS pin,
       (SELECT child_valueid FROM jbtl_subtree_refs r, pg_class c
         WHERE c.relname='m92_t2' AND r.parent_toastrelid=c.reltoastrelid)
       = (SELECT pre_child_v FROM t2_pre)
       AS ok;
SELECT 't2_edge_count_bounded' AS pin,
       (SELECT count(*) FROM jbtl_subtree_refs r, pg_class c
         WHERE c.relname='m92_t2' AND r.parent_toastrelid=c.reltoastrelid) = 1
       AS ok;

-- Counters: 10 attempts, 10 successes, 10 children reused.
SELECT 't2_attempts'  AS pin, jbtl_update_subtree_reuse_attempts() = 10 AS ok;
SELECT 't2_successes' AS pin, jbtl_update_subtree_reuse_successes() = 10 AS ok;
SELECT 't2_children'  AS pin, jbtl_update_subtree_children_reused() = 10 AS ok;
SELECT 't2_refs_check_clean' AS pin, jbtl_subtree_refs_check() = 0 AS ok;

DROP TABLE t2_pre;
DROP TABLE m92_t2;
SELECT jbtl_subtree_refs_gc() AS t2_gc;

-- ============================================================
-- T3  changed-payload fallback — reuse path declines
-- ============================================================

SELECT jbtl_update_calls_reset();

DROP TABLE IF EXISTS m92_t3 CASCADE;
CREATE TABLE m92_t3 (id int PRIMARY KEY, jb jsonb STORAGE EXTERNAL);
SELECT pgpro_toast.set_toaster('jsonb_toaster_lite', 'm92_t3', 'jb') > 0
       AS attached;

INSERT INTO m92_t3 VALUES (1, jsonb_build_object(
  'id', 1,
  'status', 'open',
  'key2', (SELECT jsonb_agg(md5(s::text)) FROM generate_series(1, 1000) s)));

CREATE TEMP TABLE t3_pre AS
SELECT child_valueid AS pre_child_v
  FROM jbtl_subtree_refs r, pg_class c
 WHERE c.relname='m92_t3' AND r.parent_toastrelid=c.reltoastrelid;

-- UPDATE replaces the LARGE PAYLOAD (key2) with a scalar.
UPDATE m92_t3 SET jb = jsonb_set(jb, '{key2}', '"replaced"'::jsonb) WHERE id=1;

-- Logical value correct.
SELECT 't3_key2_replaced' AS pin, jb->'key2' = '"replaced"'::jsonb AS ok FROM m92_t3 WHERE id=1;
SELECT 't3_status_unchanged' AS pin, jb->'status' = '"open"'::jsonb AS ok FROM m92_t3 WHERE id=1;

-- Old child no longer reused: edge count for this table is 0 (the new
-- root has nothing large enough to spill, and we declined reuse so
-- the standard delete+retoast ran).
SELECT 't3_no_edges_for_m92_t3' AS pin,
       (SELECT count(*) FROM jbtl_subtree_refs r, pg_class c
         WHERE c.relname='m92_t3' AND r.parent_toastrelid=c.reltoastrelid) = 0
       AS ok;

-- Counters: 1 attempt, 0 successes, 0 children reused.
SELECT 't3_attempts'  AS pin, jbtl_update_subtree_reuse_attempts() = 1 AS ok;
SELECT 't3_successes' AS pin, jbtl_update_subtree_reuse_successes() = 0 AS ok;
SELECT 't3_children'  AS pin, jbtl_update_subtree_children_reused() = 0 AS ok;
SELECT 't3_refs_check_clean' AS pin, jbtl_subtree_refs_check() = 0 AS ok;

DROP TABLE t3_pre;
DROP TABLE m92_t3;
SELECT jbtl_subtree_refs_gc() AS t3_gc;

-- ============================================================
-- T4  rollback preserves old refs state
-- ============================================================

SELECT jbtl_update_calls_reset();

DROP TABLE IF EXISTS m92_t4 CASCADE;
CREATE TABLE m92_t4 (id int PRIMARY KEY, jb jsonb STORAGE EXTERNAL);
SELECT pgpro_toast.set_toaster('jsonb_toaster_lite', 'm92_t4', 'jb') > 0
       AS attached;

INSERT INTO m92_t4 VALUES (1, jsonb_build_object(
  'id', 1,
  'status', 'open',
  'key2', (SELECT jsonb_agg(md5(s::text)) FROM generate_series(1, 1000) s)));

CREATE TEMP TABLE t4_pre AS
SELECT md5(jb::text) AS pre_md5,
       (SELECT child_valueid FROM jbtl_subtree_refs r, pg_class c
         WHERE c.relname='m92_t4' AND r.parent_toastrelid=c.reltoastrelid)
       AS pre_child_v,
       (SELECT parent_valueid FROM jbtl_subtree_refs r, pg_class c
         WHERE c.relname='m92_t4' AND r.parent_toastrelid=c.reltoastrelid)
       AS pre_parent_v
  FROM m92_t4 WHERE id=1;

BEGIN;
UPDATE m92_t4 SET jb = jsonb_set(jb, '{status}', '"done"') WHERE id=1;
ROLLBACK;

-- After ROLLBACK: md5, child_v, parent_v all match pre-BEGIN.
SELECT 't4_md5_restored' AS pin,
       (SELECT md5(jb::text) FROM m92_t4 WHERE id=1) = (SELECT pre_md5 FROM t4_pre)
       AS ok;
SELECT 't4_child_restored' AS pin,
       (SELECT child_valueid FROM jbtl_subtree_refs r, pg_class c
         WHERE c.relname='m92_t4' AND r.parent_toastrelid=c.reltoastrelid)
       = (SELECT pre_child_v FROM t4_pre)
       AS ok;
SELECT 't4_parent_restored' AS pin,
       (SELECT parent_valueid FROM jbtl_subtree_refs r, pg_class c
         WHERE c.relname='m92_t4' AND r.parent_toastrelid=c.reltoastrelid)
       = (SELECT pre_parent_v FROM t4_pre)
       AS ok;
SELECT 't4_edge_count' AS pin,
       (SELECT count(*) FROM jbtl_subtree_refs r, pg_class c
         WHERE c.relname='m92_t4' AND r.parent_toastrelid=c.reltoastrelid) = 1
       AS ok;
SELECT 't4_refs_check_clean' AS pin, jbtl_subtree_refs_check() = 0 AS ok;

DROP TABLE t4_pre;
DROP TABLE m92_t4;
SELECT jbtl_subtree_refs_gc() AS t4_gc;

-- ============================================================
-- T5  DELETE + VACUUM removes the row's refs
-- ============================================================

SELECT jbtl_update_calls_reset();

DROP TABLE IF EXISTS m92_t5 CASCADE;
CREATE TABLE m92_t5 (id int PRIMARY KEY, jb jsonb STORAGE EXTERNAL);
SELECT pgpro_toast.set_toaster('jsonb_toaster_lite', 'm92_t5', 'jb') > 0
       AS attached;

INSERT INTO m92_t5 VALUES (1, jsonb_build_object(
  'id', 1, 'status', 'open',
  'key2', (SELECT jsonb_agg(md5(s::text)) FROM generate_series(1, 1000) s)));
INSERT INTO m92_t5 VALUES (2, jsonb_build_object(
  'id', 2, 'status', 'open',
  'key2', (SELECT jsonb_agg(md5((s+1000)::text)) FROM generate_series(1, 1000) s)));

-- Each row gets one reuse update so they both have new parent_vid.
UPDATE m92_t5 SET jb = jsonb_set(jb, '{status}', '"v1"') WHERE id=1;
UPDATE m92_t5 SET jb = jsonb_set(jb, '{status}', '"v1"') WHERE id=2;

SELECT 't5_edges_before_delete' AS pin,
       (SELECT count(*) FROM jbtl_subtree_refs r, pg_class c
         WHERE c.relname='m92_t5' AND r.parent_toastrelid=c.reltoastrelid) = 2
       AS ok;

DELETE FROM m92_t5 WHERE id = 1;

SELECT 't5_edges_after_delete' AS pin,
       (SELECT count(*) FROM jbtl_subtree_refs r, pg_class c
         WHERE c.relname='m92_t5' AND r.parent_toastrelid=c.reltoastrelid) = 1
       AS ok;

VACUUM m92_t5;

SELECT 't5_edges_after_vacuum' AS pin,
       (SELECT count(*) FROM jbtl_subtree_refs r, pg_class c
         WHERE c.relname='m92_t5' AND r.parent_toastrelid=c.reltoastrelid) = 1
       AS ok;

-- Remaining row reads correctly.
SELECT 't5_remaining_readable' AS pin,
       jb->'status' = '"v1"'::jsonb AND jsonb_array_length(jb->'key2') = 1000 AS ok
  FROM m92_t5 WHERE id=2;
SELECT 't5_refs_check_clean' AS pin, jbtl_subtree_refs_check() = 0 AS ok;

DROP TABLE m92_t5;
SELECT jbtl_subtree_refs_gc() AS t5_gc;

-- ============================================================
-- T6  VACUUM FULL gate still active; recovery succeeds
-- ============================================================

SELECT jbtl_update_calls_reset();

DROP TABLE IF EXISTS m92_t6 CASCADE;
CREATE TABLE m92_t6 (id int PRIMARY KEY, jb jsonb STORAGE EXTERNAL);
SELECT pgpro_toast.set_toaster('jsonb_toaster_lite', 'm92_t6', 'jb') > 0
       AS attached;

INSERT INTO m92_t6 VALUES (1, jsonb_build_object(
  'id', 1, 'status', 'open',
  'key2', (SELECT jsonb_agg(md5(s::text)) FROM generate_series(1, 1000) s)));
-- Force the reuse path to fire so we exercise an updated SUBTREE row.
UPDATE m92_t6 SET jb = jsonb_set(jb, '{status}', '"done"') WHERE id=1;

-- Gate refuses VACUUM FULL while live SUBTREE row exists.
DO $$
BEGIN
    VACUUM FULL m92_t6;
    RAISE NOTICE 't6_vfull_unexpectedly_succeeded';
EXCEPTION WHEN object_not_in_prerequisite_state THEN
    RAISE NOTICE 't6_vfull_refused_as_expected';
END;
$$;

-- Recover via text-cast UPDATE under GUC off.  The reuse helper
-- declines under GUC off, falling back to the standard
-- detoast+retoast which emits POINTER (no SUBTREE).
SET jsonb_toaster_lite.enable_subtree_storage = off;
UPDATE m92_t6 SET jb = jb::text::jsonb;

-- Now VACUUM FULL succeeds.
VACUUM FULL m92_t6;

SELECT 't6_value_after_recovery' AS pin,
       jb->'status' = '"done"'::jsonb AND jsonb_array_length(jb->'key2') = 1000 AS ok
  FROM m92_t6 WHERE id=1;
SELECT 't6_refs_check_clean' AS pin, jbtl_subtree_refs_check() = 0 AS ok;

-- Restore GUC for any subsequent tests in this DB.
SET jsonb_toaster_lite.enable_subtree_storage = on;

DROP TABLE m92_t6;
SELECT jbtl_subtree_refs_gc() AS t6_gc;

RESET jsonb_toaster_lite.enable_subtree_storage;
RESET jsonb_toaster_lite.subtree_spill_threshold;
