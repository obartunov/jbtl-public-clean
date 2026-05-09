--
-- M5.0b-5: M5.0b lifecycle bundle + cross-feature regression.
--
--	No new runtime functionality.  This is the integrated lifecycle
--	test that ties together every M5.0b stage's contribution into
--	one end-to-end scenario, plus a cross-feature pin set proving
--	non-SUBTREE paths (L2.1b DIFF, plain JBTL_POINTER, inline
--	PLAIN_JSONB) still work alongside the SUBTREE machinery.
--
--	Lifecycle (per @yoda M5.0b-5 acceptance):
--	  1. feature default OFF
--	  2. INSERT with spill
--	  3. full read equality
--	  4. refs edge exists
--	  5. DELETE removes edge
--	  6. orphan child chain removed
--	  7. INSERT...SELECT creates independent copy
--	  8. DELETE source does not affect destination
--	  9. refs checker clean
--
--	Cross-feature (per @yoda M5.0b-5 acceptance):
--	  - L2.1b DIFF still works on non-subtree rows
--	  - existing POINTER/PLAIN/COMPRESSED paths still pass
--

\set ON_ERROR_STOP on

CREATE EXTENSION IF NOT EXISTS toastapi;
CREATE EXTENSION IF NOT EXISTS jsonb_toaster_lite;
LOAD 'jsonb_toaster_lite';

SET jsonb_sort_field_values = off;
SET jsonb_toaster_lite.compress_chunks = off;

TRUNCATE jbtl_subtree_refs;

-- =====================================================================
-- LIFECYCLE — feature OFF leg
-- =====================================================================

-- Pin 1: feature default OFF — no edges, no SUBTREE shape.
RESET jsonb_toaster_lite.enable_subtree_storage;
RESET jsonb_toaster_lite.subtree_spill_threshold;

CREATE TABLE m50b5_off (id int PRIMARY KEY, jb jsonb STORAGE EXTERNAL);
SELECT pgpro_toast.set_toaster('jsonb_toaster_lite', 'm50b5_off', 'jb') > 0
       AS attached_off;

INSERT INTO m50b5_off SELECT 1, jsonb_build_object(
  'key1', 100, 'key3', 100,
  'key2', (SELECT jsonb_agg(md5((100*1000 + s)::text))
             FROM generate_series(1, 1000) s));

SELECT 'pin_off_no_edges' AS what,
       (SELECT count(*) FROM jbtl_subtree_refs) = 0 AS pin;

DROP TABLE m50b5_off;

-- =====================================================================
-- LIFECYCLE — feature ON leg
-- =====================================================================

SET jsonb_toaster_lite.enable_subtree_storage = on;
SET jsonb_toaster_lite.subtree_spill_threshold = 4096;

-- Vanilla baseline for read equality.
CREATE TABLE m50b5_baseline (id int PRIMARY KEY, jb jsonb);
INSERT INTO m50b5_baseline SELECT 1, jsonb_build_object(
  'key1', 100, 'key3', 100,
  'key2', (SELECT jsonb_agg(md5((100*1000 + s)::text))
             FROM generate_series(1, 1000) s));

CREATE TABLE m50b5_main (id int PRIMARY KEY, jb jsonb STORAGE EXTERNAL);
SELECT pgpro_toast.set_toaster('jsonb_toaster_lite', 'm50b5_main', 'jb') > 0
       AS attached_main;

INSERT INTO m50b5_main SELECT 1, jsonb_build_object(
  'key1', 100, 'key3', 100,
  'key2', (SELECT jsonb_agg(md5((100*1000 + s)::text))
             FROM generate_series(1, 1000) s));

-- Pin 2: INSERT with spill produced exactly one edge.
SELECT 'pin_on_insert_one_edge' AS what,
       (SELECT count(*) FROM jbtl_subtree_refs) = 1 AS pin;

-- Pin 3: full read equality vs vanilla baseline.
SELECT 'pin_full_jsonb_eq' AS what,
       (SELECT jb FROM m50b5_main) = (SELECT jb FROM m50b5_baseline) AS pin;

SELECT 'pin_partial_key1' AS what,
       (SELECT jb->>'key1' FROM m50b5_main) =
       (SELECT jb->>'key1' FROM m50b5_baseline) AS pin;

SELECT 'pin_partial_key2_first' AS what,
       (SELECT jb->'key2'->>0 FROM m50b5_main) =
       (SELECT jb->'key2'->>0 FROM m50b5_baseline) AS pin;

SELECT 'pin_partial_key2_last' AS what,
       (SELECT jb->'key2'->>999 FROM m50b5_main) =
       (SELECT jb->'key2'->>999 FROM m50b5_baseline) AS pin;

-- Pin 4: refs has the edge, child chain is in pg_toast.
DO $$
DECLARE
  toast_oid oid; toast_name text; q text;
  edge_child_relid oid; edge_child_vid oid;
  child_chunks bigint;
BEGIN
  SELECT reltoastrelid INTO toast_oid FROM pg_class WHERE relname='m50b5_main';
  SELECT relname INTO toast_name FROM pg_class WHERE oid=toast_oid;

  SELECT child_toastrelid, child_valueid INTO edge_child_relid, edge_child_vid
    FROM jbtl_subtree_refs LIMIT 1;

  IF edge_child_relid <> toast_oid THEN
    RAISE EXCEPTION 'pin_edge_child_in_main_toastrel FAILED';
  END IF;

  q := format('SELECT count(*) FROM pg_toast.%I WHERE chunk_id = $1', toast_name);
  EXECUTE q INTO child_chunks USING edge_child_vid;
  IF child_chunks = 0 THEN
    RAISE EXCEPTION 'pin_child_chain_present FAILED';
  END IF;

  RAISE NOTICE 'pin_edge_child_in_main_toastrel: t';
  RAISE NOTICE 'pin_child_chain_present: t';
END $$;

-- =====================================================================
-- COPY leg — INSERT...SELECT creates independent refs entries.
-- =====================================================================

CREATE TABLE m50b5_copy (id int PRIMARY KEY, jb jsonb STORAGE EXTERNAL);
SELECT pgpro_toast.set_toaster('jsonb_toaster_lite', 'm50b5_copy', 'jb') > 0
       AS attached_copy;

INSERT INTO m50b5_copy SELECT id, jb FROM m50b5_main;

-- Pin 5: copy produced a fresh edge (now 2 total).
SELECT 'pin_copy_two_edges_total' AS what,
       (SELECT count(*) FROM jbtl_subtree_refs) = 2 AS pin;

-- Pin 6: parent_valueids and child_valueids are distinct between
-- source and copy (deep copy via detoast/retoast).
SELECT 'pin_copy_distinct_parent_vids' AS what,
       (SELECT count(DISTINCT parent_valueid) FROM jbtl_subtree_refs) = 2 AS pin;

SELECT 'pin_copy_distinct_child_vids' AS what,
       (SELECT count(DISTINCT child_valueid) FROM jbtl_subtree_refs) = 2 AS pin;

SELECT 'pin_copy_dst_eq_src' AS what,
       (SELECT jb FROM m50b5_copy) = (SELECT jb FROM m50b5_main) AS pin;

-- =====================================================================
-- DELETE leg — source delete does not affect destination.
-- =====================================================================

DELETE FROM m50b5_main WHERE id = 1;
VACUUM m50b5_main; CHECKPOINT;

-- Pin 7: edge gone for source; destination edge intact.
SELECT 'pin_after_src_delete_one_edge' AS what,
       (SELECT count(*) FROM jbtl_subtree_refs) = 1 AS pin;

-- Pin 8: source's child toast chain is gone.
DO $$
DECLARE
  toast_oid oid; toast_name text; q text; cnt bigint;
BEGIN
  SELECT reltoastrelid INTO toast_oid FROM pg_class WHERE relname='m50b5_main';
  SELECT relname INTO toast_name FROM pg_class WHERE oid=toast_oid;
  q := format('SELECT count(*) FROM pg_toast.%I', toast_name);
  EXECUTE q INTO cnt;
  IF cnt <> 0 THEN
    RAISE EXCEPTION 'pin_src_child_removed FAILED: chunks=%', cnt;
  END IF;
  RAISE NOTICE 'pin_src_child_removed: t';
END $$;

-- Pin 9: destination still readable.
SELECT 'pin_dst_still_readable' AS what,
       (SELECT jb FROM m50b5_copy) = (SELECT jb FROM m50b5_baseline) AS pin;

-- Pin 10: refs checker clean.
SELECT 'pin_refs_check_clean' AS what,
       jbtl_subtree_refs_check() = 0 AS pin;

-- Cleanup destination row to wipe refs for the next section.
DELETE FROM m50b5_copy WHERE id = 1;
VACUUM m50b5_copy; CHECKPOINT;

SELECT 'pin_all_clean_after_full_lifecycle' AS what,
       (SELECT count(*) FROM jbtl_subtree_refs) = 0 AS pin;

DROP TABLE m50b5_main, m50b5_copy, m50b5_baseline;

-- =====================================================================
-- CROSS-FEATURE — non-SUBTREE paths still work alongside SUBTREE.
-- =====================================================================

-- Pin 11: L2.1b DIFF still emits on a same-length scalar update of
-- a non-SUBTREE row.  Use a very-small-threshold-with-spill-OFF setup
-- so the row goes through the existing JBTL_POINTER path.
RESET jsonb_toaster_lite.enable_subtree_storage;

CREATE TABLE m50b5_diff (id int PRIMARY KEY, jb jsonb STORAGE EXTERNAL);
SELECT pgpro_toast.set_toaster('jsonb_toaster_lite', 'm50b5_diff', 'jb') > 0
       AS attached_diff;

INSERT INTO m50b5_diff SELECT 1, jsonb_build_object(
  'k1', 100, 'k3', 100,
  'k2', (SELECT jsonb_agg(md5((100*1000 + s)::text))
           FROM generate_series(1, 1000) s));
VACUUM ANALYZE m50b5_diff; CHECKPOINT;

SELECT jbtl_update_calls_reset();

UPDATE m50b5_diff SET jb = jsonb_set(jb, '{k3}', '99'::jsonb) WHERE id = 1;

-- L2.1b emits at least one DIFF when same-length-scalar update fits
-- the L2.1b conditions.
SELECT 'pin_l21b_diff_emitted' AS what,
       jbtl_update_diffs_emitted() >= 1 AS pin;

SELECT 'pin_diff_read_correct_k3' AS what,
       (SELECT jb->>'k3' FROM m50b5_diff) = '99' AS pin;

SELECT 'pin_diff_read_correct_k1' AS what,
       (SELECT jb->>'k1' FROM m50b5_diff) = '100' AS pin;

-- Pin 12: small body uses inline PLAIN_JSONB path (no edges, no
-- toast chain, no spill).
CREATE TABLE m50b5_inline (id int PRIMARY KEY, jb jsonb STORAGE EXTERNAL);
SELECT pgpro_toast.set_toaster('jsonb_toaster_lite', 'm50b5_inline', 'jb') > 0
       AS attached_inline;

INSERT INTO m50b5_inline SELECT 1, '{"a":1,"b":2}'::jsonb;

SELECT 'pin_inline_no_edges' AS what,
       (SELECT count(*) FROM jbtl_subtree_refs) = 0 AS pin;

SELECT 'pin_inline_read_back' AS what,
       (SELECT jb->>'a' FROM m50b5_inline) = '1' AS pin;

-- Pin 13: with spill ON but no candidate (all top-level scalars),
-- spill does not fire; row goes through JBTL_POINTER path.
SET jsonb_toaster_lite.enable_subtree_storage = on;

CREATE TABLE m50b5_no_candidate (id int PRIMARY KEY, jb jsonb STORAGE EXTERNAL);
SELECT pgpro_toast.set_toaster('jsonb_toaster_lite', 'm50b5_no_candidate', 'jb') > 0
       AS attached_nc;

-- 100 small scalar keys, no container values.
INSERT INTO m50b5_no_candidate SELECT 1,
  jsonb_object_agg('k' || s, repeat('x', 200))
  FROM generate_series(1, 100) s;

SELECT 'pin_no_container_no_spill' AS what,
       (SELECT count(*) FROM jbtl_subtree_refs
         WHERE parent_toastrelid =
               (SELECT reltoastrelid FROM pg_class WHERE relname='m50b5_no_candidate'))
       = 0 AS pin;

-- =====================================================================
-- Final cleanup
-- =====================================================================
DROP TABLE m50b5_diff, m50b5_inline, m50b5_no_candidate;
TRUNCATE jbtl_subtree_refs;

SELECT 'pin_final_refs_check_clean' AS what,
       jbtl_subtree_refs_check() = 0 AS pin;

RESET jsonb_toaster_lite.enable_subtree_storage;
RESET jsonb_toaster_lite.subtree_spill_threshold;
