--
-- M5.0b-4: refcount-aware SUBTREE delete + safe-copy guard.
--
--	Acceptance pins per @yoda directive:
--	  1. insert with spill creates one edge and one child
--	  2. delete row removes the edge
--	  3. if no refs remain, child chain is deleted
--	  4. jbtl_subtree_refs_check() clean after delete
--	  5. all previous tests green (covered by full-suite)
--
--	Plus copy-safety pin:
--	  - INSERT...SELECT from SUBTREE source produces independent
--	    refs entries in the destination (deep copy via detoast +
--	    retoast); deleting source after copy does NOT touch dest.
--

\set ON_ERROR_STOP on

CREATE EXTENSION IF NOT EXISTS toastapi;
CREATE EXTENSION IF NOT EXISTS jsonb_toaster_lite;
LOAD 'jsonb_toaster_lite';

SET jsonb_sort_field_values = off;
SET jsonb_toaster_lite.compress_chunks = off;
SET jsonb_toaster_lite.enable_subtree_storage = on;
SET jsonb_toaster_lite.subtree_spill_threshold = 4096;

-- Isolate from prior tests' refs residue.
TRUNCATE jbtl_subtree_refs;

-- =====================================================================
-- Pin 1: INSERT with spill creates one edge and one child chain.
-- =====================================================================
CREATE TABLE m50b4_main (id int PRIMARY KEY, jb jsonb STORAGE EXTERNAL);
SELECT pgpro_toast.set_toaster('jsonb_toaster_lite', 'm50b4_main', 'jb') > 0
       AS attached_main;

INSERT INTO m50b4_main SELECT 1, jsonb_build_object(
  'key1', 100, 'key3', 100,
  'key2', (SELECT jsonb_agg(md5((100*1000 + s)::text))
             FROM generate_series(1, 1000) s));

SELECT 'pin_after_insert_one_edge' AS what,
       (SELECT count(*) FROM jbtl_subtree_refs) = 1 AS pin;

DO $$
DECLARE
  toastrelid oid; toast_name text; q text; cnt bigint;
BEGIN
  SELECT reltoastrelid INTO toastrelid FROM pg_class WHERE relname='m50b4_main';
  SELECT relname INTO toast_name FROM pg_class WHERE oid=toastrelid;
  q := format('SELECT count(distinct chunk_id) FROM pg_toast.%I', toast_name);
  EXECUTE q INTO cnt;
  IF cnt <> 1 THEN
    RAISE EXCEPTION 'pin_after_insert_one_child FAILED: distinct chunk_ids=%', cnt;
  END IF;
  RAISE NOTICE 'pin_after_insert_one_child: t';
END $$;

-- =====================================================================
-- Pin 2: DELETE row removes the edge from refs.
-- =====================================================================
DELETE FROM m50b4_main WHERE id = 1;

SELECT 'pin_after_delete_zero_edges' AS what,
       (SELECT count(*) FROM jbtl_subtree_refs) = 0 AS pin;

-- =====================================================================
-- Pin 3: child toast chain is gone (refcount reached zero).
-- =====================================================================
VACUUM m50b4_main; CHECKPOINT;

DO $$
DECLARE
  toastrelid oid; toast_name text; q text; cnt bigint;
BEGIN
  SELECT reltoastrelid INTO toastrelid FROM pg_class WHERE relname='m50b4_main';
  SELECT relname INTO toast_name FROM pg_class WHERE oid=toastrelid;
  q := format('SELECT count(*) FROM pg_toast.%I', toast_name);
  EXECUTE q INTO cnt;
  IF cnt <> 0 THEN
    RAISE EXCEPTION 'pin_after_delete_no_chunks FAILED: chunks=%', cnt;
  END IF;
  RAISE NOTICE 'pin_after_delete_no_chunks: t';
END $$;

-- =====================================================================
-- Pin 4: refs_check() clean after delete.
-- =====================================================================
SELECT 'pin_refs_check_clean_after_delete' AS what,
       jbtl_subtree_refs_check() = 0 AS pin;

DROP TABLE m50b4_main;
TRUNCATE jbtl_subtree_refs;

-- =====================================================================
-- Pin 5 (extra): copy safety.  INSERT...SELECT from a SUBTREE-toasted
-- source into another SUBTREE-toasted destination must produce
-- independent refs (the copy hook returns Datum 0 → core detoasts +
-- retoasts → spill writes a fresh parent_valueid + new edges).
-- After copy, the destination has its own edge.  After deleting the
-- source row, the destination's edge AND child chain remain intact.
-- =====================================================================
CREATE TABLE m50b4_src (id int PRIMARY KEY, jb jsonb STORAGE EXTERNAL);
CREATE TABLE m50b4_dst (id int PRIMARY KEY, jb jsonb STORAGE EXTERNAL);
SELECT pgpro_toast.set_toaster('jsonb_toaster_lite', 'm50b4_src', 'jb') > 0
       AS attached_src;
SELECT pgpro_toast.set_toaster('jsonb_toaster_lite', 'm50b4_dst', 'jb') > 0
       AS attached_dst;

INSERT INTO m50b4_src SELECT 1, jsonb_build_object(
  'key1', 100,
  'key2', (SELECT jsonb_agg(md5((100*1000 + s)::text))
             FROM generate_series(1, 1000) s));

SELECT 'pin_copy_src_one_edge' AS what,
       (SELECT count(*) FROM jbtl_subtree_refs) = 1 AS pin;

-- The copy.
INSERT INTO m50b4_dst SELECT id, jb FROM m50b4_src;

-- Two independent edges now (one per heap row).
SELECT 'pin_copy_two_edges' AS what,
       (SELECT count(*) FROM jbtl_subtree_refs) = 2 AS pin;

-- Edges have DIFFERENT parent_valueids (independent identities).
SELECT 'pin_copy_distinct_parent_vids' AS what,
       (SELECT count(distinct parent_valueid) FROM jbtl_subtree_refs) = 2
       AS pin;

-- Edges have DIFFERENT child_valueids too (separate child chains).
SELECT 'pin_copy_distinct_child_vids' AS what,
       (SELECT count(distinct child_valueid) FROM jbtl_subtree_refs) = 2
       AS pin;

-- Both readable, equal content.
SELECT 'pin_copy_dst_eq_src' AS what,
       (SELECT jb FROM m50b4_src) = (SELECT jb FROM m50b4_dst) AS pin;

-- DELETE source: source edge gone, dest edge intact.
DELETE FROM m50b4_src WHERE id = 1;

SELECT 'pin_copy_after_src_delete_one_edge' AS what,
       (SELECT count(*) FROM jbtl_subtree_refs) = 1 AS pin;

-- Destination still readable.
SELECT 'pin_copy_dst_still_readable' AS what,
       jsonb_array_length((SELECT jb->'key2' FROM m50b4_dst)) = 1000 AS pin;

VACUUM m50b4_src, m50b4_dst; CHECKPOINT;
SELECT 'pin_copy_refs_check_clean' AS what,
       jbtl_subtree_refs_check() = 0 AS pin;

DROP TABLE m50b4_src, m50b4_dst;

RESET jsonb_toaster_lite.enable_subtree_storage;
RESET jsonb_toaster_lite.subtree_spill_threshold;
