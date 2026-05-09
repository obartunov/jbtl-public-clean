--
-- M5.0b-3: production initial spill in tsr_toast.
--
--	Acceptance pins per @yoda directive:
--	  1. feature default OFF: no behaviour change
--	  2. feature ON + L14 insert: key2 spills
--	  3. parent column small
--	  4. child chain exists
--	  5. v1 header has parent_valueid and parent_toastrelid
--	  6. refs table has exactly one edge parent->child
--	  7. full detoast equals baseline
--	  8. jbtl_subtree_refs_check() clean
--	  9. previous tests still green (covered by full-suite run)
--

\set ON_ERROR_STOP on

CREATE EXTENSION IF NOT EXISTS toastapi;
CREATE EXTENSION IF NOT EXISTS jsonb_toaster_lite;
LOAD 'jsonb_toaster_lite';

-- Isolate from any prior test's residue.  m50b3 is the first test to
-- write refs edges via the production path; ensuring a clean slate
-- here means later tests in the same DB run see the post-cleanup
-- state (we DROP the spilled table at the end too).
TRUNCATE jbtl_subtree_refs;

SET jsonb_sort_field_values = off;
SET jsonb_toaster_lite.compress_chunks = off;

-- Vanilla baseline (no toaster).
CREATE TABLE m50b3_base (id int PRIMARY KEY, jb jsonb);

INSERT INTO m50b3_base SELECT 1, jsonb_build_object(
  'key1', 100, 'key3', 100,
  'key2', (SELECT jsonb_agg(md5((100*1000 + s)::text))
             FROM generate_series(1, 1000) s));

-- =====================================================================
-- Pin 1: feature default OFF — no behaviour change.
-- An insert against a lite-toasted column with the default GUC
-- produces the same column shape as without the GUC enabled
-- (i.e. JBTL_POINTER, not SUBTREE).
-- =====================================================================
CREATE TABLE m50b3_off (id int PRIMARY KEY, jb jsonb STORAGE EXTERNAL);
SELECT pgpro_toast.set_toaster('jsonb_toaster_lite', 'm50b3_off', 'jb') > 0
       AS attached_off;

-- GUC at default (off).
INSERT INTO m50b3_off SELECT 1, jsonb_build_object(
  'key1', 100, 'key3', 100,
  'key2', (SELECT jsonb_agg(md5((100*1000 + s)::text))
             FROM generate_series(1, 1000) s));

SELECT 'pin_default_off_no_subtree_edges' AS what,
       (SELECT count(*) FROM jbtl_subtree_refs) = 0 AS pin;

SELECT 'pin_default_off_read_equal' AS what,
       (SELECT jb FROM m50b3_off) = (SELECT jb FROM m50b3_base) AS pin;

DROP TABLE m50b3_off;

-- =====================================================================
-- Pin 2: feature ON + insert with large key2 → spill fires.
-- =====================================================================
SET jsonb_toaster_lite.enable_subtree_storage = on;
SET jsonb_toaster_lite.subtree_spill_threshold = 4096;

CREATE TABLE m50b3_on (id int PRIMARY KEY, jb jsonb STORAGE EXTERNAL);
SELECT pgpro_toast.set_toaster('jsonb_toaster_lite', 'm50b3_on', 'jb') > 0
       AS attached_on;

INSERT INTO m50b3_on SELECT 1, jsonb_build_object(
  'key1', 100, 'key3', 100,
  'key2', (SELECT jsonb_agg(md5((100*1000 + s)::text))
             FROM generate_series(1, 1000) s));

SELECT 'pin_on_inserted' AS what,
       (SELECT count(*) FROM m50b3_on) = 1 AS pin;

-- =====================================================================
-- Pin 3: parent column small.
-- v1 header (16 bytes) + parent inline body (small object header +
-- JEntries + keys + key1/key3 scalars + 22 byte child pointer for key2)
-- ≈ ~120 bytes total.
-- =====================================================================
SELECT 'pin_parent_column_small' AS what,
       (SELECT pg_column_size(jb) FROM m50b3_on) <= 200 AS pin;

-- =====================================================================
-- Pin 4: child chain exists in m50b3_on's toast relation.
-- Refs table records exactly one edge.
-- =====================================================================
SELECT 'pin_one_edge_recorded' AS what,
       (SELECT count(*) FROM jbtl_subtree_refs) = 1 AS pin;

-- Verify the refs edge points at a real child chunk.
DO $$
DECLARE
  toast_oid oid;
  toast_name text;
  edge_child_relid oid;
  edge_child_vid oid;
  child_chunk_count bigint;
  q text;
BEGIN
  SELECT reltoastrelid INTO toast_oid FROM pg_class WHERE relname='m50b3_on';
  SELECT relname INTO toast_name FROM pg_class WHERE oid=toast_oid;

  SELECT child_toastrelid, child_valueid
    INTO edge_child_relid, edge_child_vid
    FROM jbtl_subtree_refs LIMIT 1;

  q := format('SELECT count(*) FROM pg_toast.%I WHERE chunk_id = $1', toast_name);
  EXECUTE q INTO child_chunk_count USING edge_child_vid;

  IF child_chunk_count = 0 THEN
    RAISE EXCEPTION 'pin_child_chunks_present FAILED: no chunks for child_vid';
  END IF;

  -- Verify edge's parent_toastrelid matches m50b3_on's toast relation.
  IF edge_child_relid <> toast_oid THEN
    RAISE EXCEPTION 'pin_child_in_same_toastrel FAILED';
  END IF;

  RAISE NOTICE 'pin_child_chunks_present: t';
  RAISE NOTICE 'pin_child_in_same_toastrel: t';
END $$;

-- =====================================================================
-- Pin 5: v1 header presence (refs has parent_valueid which only the
-- v1 production writer allocates; v0 fixture path never inserts into
-- refs).  Cross-check: parent_toastrelid in refs equals m50b3_on's
-- reltoastrelid.
-- =====================================================================
DO $$
DECLARE
  toast_oid oid;
  edge_parent_relid oid;
  edge_parent_vid oid;
BEGIN
  SELECT reltoastrelid INTO toast_oid FROM pg_class WHERE relname='m50b3_on';
  SELECT parent_toastrelid, parent_valueid
    INTO edge_parent_relid, edge_parent_vid
    FROM jbtl_subtree_refs LIMIT 1;

  IF edge_parent_relid <> toast_oid THEN
    RAISE EXCEPTION 'pin_v1_parent_toastrelid FAILED';
  END IF;

  IF NOT (edge_parent_vid > 0) THEN
    RAISE EXCEPTION 'pin_v1_parent_valueid_allocated FAILED';
  END IF;

  RAISE NOTICE 'pin_v1_parent_toastrelid: t';
  RAISE NOTICE 'pin_v1_parent_valueid_allocated: t';
END $$;

-- =====================================================================
-- Pin 6: full detoast equals baseline.
-- =====================================================================
SELECT 'pin_read_full_jsonb_eq' AS what,
       (SELECT jb FROM m50b3_on) = (SELECT jb FROM m50b3_base) AS pin;

SELECT 'pin_read_full_text_eq' AS what,
       (SELECT jb::text FROM m50b3_on) = (SELECT jb::text FROM m50b3_base)
       AS pin;

SELECT 'pin_read_partial_key1' AS what,
       (SELECT jb->>'key1' FROM m50b3_on) =
       (SELECT jb->>'key1' FROM m50b3_base) AS pin;

SELECT 'pin_read_partial_key2_first' AS what,
       (SELECT jb->'key2'->>0 FROM m50b3_on) =
       (SELECT jb->'key2'->>0 FROM m50b3_base) AS pin;

SELECT 'pin_read_partial_key2_last' AS what,
       (SELECT jb->'key2'->>999 FROM m50b3_on) =
       (SELECT jb->'key2'->>999 FROM m50b3_base) AS pin;

-- =====================================================================
-- Pin 7: jbtl_subtree_refs_check() clean — no dead edges.
-- =====================================================================
SELECT 'pin_refs_check_clean' AS what,
       jbtl_subtree_refs_check() = 0 AS pin;

-- =====================================================================
-- Pin 8: small body (under threshold) does NOT spill.
-- =====================================================================
DROP TABLE IF EXISTS m50b3_small;
CREATE TABLE m50b3_small (id int PRIMARY KEY, jb jsonb STORAGE EXTERNAL);
SELECT pgpro_toast.set_toaster('jsonb_toaster_lite', 'm50b3_small', 'jb') > 0
       AS attached_small;

INSERT INTO m50b3_small SELECT 1, jsonb_build_object(
  'key1', 100, 'key3', 100,
  -- Small array, well under 4 KB threshold.
  'key2', (SELECT jsonb_agg(s) FROM generate_series(1, 10) s));

SELECT 'pin_under_threshold_no_spill' AS what,
       (SELECT count(*) FROM jbtl_subtree_refs
         WHERE parent_toastrelid =
               (SELECT reltoastrelid FROM pg_class WHERE relname='m50b3_small'))
       = 0 AS pin;

DROP TABLE m50b3_small;
DROP TABLE m50b3_on;
DROP TABLE m50b3_base;

RESET jsonb_toaster_lite.enable_subtree_storage;
RESET jsonb_toaster_lite.subtree_spill_threshold;
