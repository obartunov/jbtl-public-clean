--
-- M5.0b-2: jbtl_subtree_refs catalog and parent_valueid allocator.
--
--	Acceptance pins per @yoda directive:
--	  pin_refs_table_exists
--	  pin_refs_table_pk
--	  pin_refs_child_index
--	  pin_refs_check_empty
--	  pin_refs_gc_empty
--	  pin_parent_vid_unique           — many allocations, no dups
--	  pin_parent_vid_not_existing_chunk — id ≠ existing chunk_id
--	  pin_refs_insert_delete_roundtrip
--	  pin_rollback_refs               — in-progress edges visible
--	  pin_check_empty                 — clean after roundtrip
--

\set ON_ERROR_STOP on

CREATE EXTENSION IF NOT EXISTS toastapi;
CREATE EXTENSION IF NOT EXISTS jsonb_toaster_lite;
LOAD 'jsonb_toaster_lite';

SET jsonb_sort_field_values = off;

-- Isolate from any prior test's residue.
TRUNCATE jbtl_subtree_refs;

-- =====================================================================
-- Pin 1: catalog table exists in extension's namespace
-- =====================================================================
SELECT 'pin_refs_table_exists' AS what,
       EXISTS (
         SELECT 1 FROM pg_class c
         JOIN pg_namespace n ON n.oid = c.relnamespace
         WHERE c.relname = 'jbtl_subtree_refs'
           AND n.nspname = 'public'
           AND c.relkind = 'r'
       ) AS pin;

-- =====================================================================
-- Pin 2: PK is exactly the four-column composite
-- =====================================================================
SELECT 'pin_refs_table_pk' AS what,
       (SELECT array_agg(a.attname::text ORDER BY u.ord)
          FROM pg_constraint c
          JOIN pg_class r ON r.oid = c.conrelid
          JOIN unnest(c.conkey) WITH ORDINALITY u(attnum, ord) ON true
          JOIN pg_attribute a ON a.attrelid = r.oid AND a.attnum = u.attnum
          WHERE r.relname = 'jbtl_subtree_refs' AND c.contype = 'p')
       =
       ARRAY['parent_toastrelid', 'parent_valueid',
             'child_toastrelid',  'child_valueid'] AS pin;

-- =====================================================================
-- Pin 3: child inverse index exists
-- =====================================================================
SELECT 'pin_refs_child_index' AS what,
       EXISTS (
         SELECT 1 FROM pg_class
          WHERE relname = 'jbtl_subtree_refs_child_idx'
            AND relkind = 'i'
       ) AS pin;

-- =====================================================================
-- Pin 4: check() returns 0 on empty refs
-- =====================================================================
SELECT 'pin_refs_check_empty' AS what,
       jbtl_subtree_refs_check() = 0 AS pin;

-- =====================================================================
-- Pin 5: gc() returns 0 on empty refs
-- =====================================================================
SELECT 'pin_refs_gc_empty' AS what,
       jbtl_subtree_refs_gc() = 0 AS pin;

-- =====================================================================
-- Pin 6: parent_vid uniqueness — allocate many, no duplicates.
-- Use a temporary working table; the toast relation behind it has
-- no chunk rows yet so collisions can only come from the loop probe.
-- =====================================================================
DROP TABLE IF EXISTS m50b2_alloc;
CREATE TABLE m50b2_alloc (id int PRIMARY KEY, jb jsonb STORAGE EXTERNAL);
SELECT pgpro_toast.set_toaster('jsonb_toaster_lite', 'm50b2_alloc', 'jb') > 0
       AS attached_alloc;

-- Capture toastrelid (used by pin 7).
SELECT reltoastrelid AS m50b2_alloc_toastrelid
  FROM pg_class WHERE relname = 'm50b2_alloc' \gset

-- Allocate 256 parent_vids and check for duplicates AND make refs
-- aware of them so the next probe sees in-progress edges.
DO $$
DECLARE
  i int;
  vid oid;
  vids oid[] := ARRAY[]::oid[];
  toastrelid oid;
BEGIN
  SELECT reltoastrelid INTO toastrelid FROM pg_class WHERE relname='m50b2_alloc';
  FOR i IN 1..256 LOOP
    vid := jbtl_test_alloc_parent_valueid('m50b2_alloc'::regclass);
    -- Insert dummy edge so subsequent allocations see this vid as in-use.
    -- child_toastrelid/valueid are placeholder values (no real chain
    -- — pin 7 verifies allocator does not collide with REAL chunks).
    PERFORM jbtl_test_refs_insert(toastrelid, vid,
                                  toastrelid, (1000000 + i)::oid);
    vids := array_append(vids, vid);
  END LOOP;

  -- Uniqueness: array_length of distinct == array_length of all.
  IF array_length((SELECT array_agg(DISTINCT v) FROM unnest(vids) v), 1)
     <> array_length(vids, 1) THEN
    RAISE EXCEPTION 'pin_parent_vid_unique FAILED: duplicates produced';
  END IF;

  RAISE NOTICE 'pin_parent_vid_unique: t (256 allocations, no dups)';
END $$;

-- =====================================================================
-- Pin 7: parent_vid does not collide with EXISTING chunk_id.
-- Plant a real toasted row, then ensure the next allocator call
-- returns an OID different from that chunk_id.
-- =====================================================================
INSERT INTO m50b2_alloc SELECT 1, jsonb_build_object(
  'k', (SELECT jsonb_agg(md5(s::text)) FROM generate_series(1,1000) s));
VACUUM ANALYZE m50b2_alloc;

DO $$
DECLARE
  toastrelid oid;
  toast_name text;
  existing_chunk oid;
  pv oid;
  q text;
BEGIN
  SELECT reltoastrelid INTO toastrelid FROM pg_class WHERE relname='m50b2_alloc';
  SELECT relname INTO toast_name FROM pg_class WHERE oid=toastrelid;
  q := format('SELECT chunk_id FROM pg_toast.%I LIMIT 1', toast_name);
  EXECUTE q INTO existing_chunk;

  pv := jbtl_test_alloc_parent_valueid('m50b2_alloc'::regclass);

  IF pv = existing_chunk THEN
    RAISE EXCEPTION 'pin_parent_vid_not_existing_chunk FAILED: collision';
  END IF;

  RAISE NOTICE 'pin_parent_vid_not_existing_chunk: t';
END $$;

DROP TABLE m50b2_alloc;
TRUNCATE jbtl_subtree_refs;

-- =====================================================================
-- Pin 8: insert / delete roundtrip + child_orphan transitions
-- =====================================================================
DROP TABLE IF EXISTS m50b2_rt;
CREATE TABLE m50b2_rt (id int PRIMARY KEY, jb jsonb STORAGE EXTERNAL);
SELECT pgpro_toast.set_toaster('jsonb_toaster_lite', 'm50b2_rt', 'jb') > 0
       AS attached_rt;

SELECT reltoastrelid AS rt_toastrelid FROM pg_class WHERE relname='m50b2_rt' \gset

SELECT jbtl_test_refs_insert(:rt_toastrelid::oid,
                             (:rt_toastrelid + 1000000)::oid,	-- dummy parent_vid
                             :rt_toastrelid::oid,
                             (:rt_toastrelid + 2000000)::oid);	-- dummy child_vid

SELECT 'pin_refs_insert_visible_count' AS what,
       (SELECT count(*) FROM jbtl_subtree_refs) = 1 AS pin;

-- After insert: child is NOT orphan (1 edge exists)
SELECT 'pin_refs_insert_delete_roundtrip_step1_not_orphan' AS what,
       NOT jbtl_test_refs_child_orphan(
              :rt_toastrelid::oid,
              (:rt_toastrelid + 2000000)::oid) AS pin;

SELECT jbtl_test_refs_delete_one(:rt_toastrelid::oid,
                                 (:rt_toastrelid + 1000000)::oid,
                                 :rt_toastrelid::oid,
                                 (:rt_toastrelid + 2000000)::oid)
       AS remaining_after_delete;

-- After delete: child IS orphan
SELECT 'pin_refs_insert_delete_roundtrip_step2_orphan' AS what,
       jbtl_test_refs_child_orphan(
              :rt_toastrelid::oid,
              (:rt_toastrelid + 2000000)::oid) AS pin;

SELECT 'pin_refs_insert_delete_roundtrip_count_zero' AS what,
       (SELECT count(*) FROM jbtl_subtree_refs) = 0 AS pin;

DROP TABLE m50b2_rt;

-- =====================================================================
-- Pin 9: rollback — in-progress edges are visible (via SnapshotDirty
-- probe) AND removed cleanly on ROLLBACK.
-- =====================================================================
DROP TABLE IF EXISTS m50b2_rb;
CREATE TABLE m50b2_rb (id int PRIMARY KEY, jb jsonb STORAGE EXTERNAL);
SELECT pgpro_toast.set_toaster('jsonb_toaster_lite', 'm50b2_rb', 'jb') > 0
       AS attached_rb;

SELECT reltoastrelid AS rb_toastrelid FROM pg_class WHERE relname='m50b2_rb' \gset

BEGIN;
SELECT jbtl_test_refs_insert(:rb_toastrelid::oid,
                             (:rb_toastrelid + 1000000)::oid,
                             :rb_toastrelid::oid,
                             (:rb_toastrelid + 2000000)::oid);

-- In-progress check: parent_id_in_use should see this edge from
-- within the same transaction (regular MVCC visibility) AND from
-- the SnapshotDirty probe used by the allocator.
SELECT 'pin_rollback_refs_in_progress_visible' AS what,
       jbtl_test_refs_parent_id_in_use(
              :rb_toastrelid::oid,
              (:rb_toastrelid + 1000000)::oid) AS pin;

ROLLBACK;

-- After rollback: edge is gone from refs
SELECT 'pin_rollback_refs_after_rollback_invisible' AS what,
       NOT jbtl_test_refs_parent_id_in_use(
              :rb_toastrelid::oid,
              (:rb_toastrelid + 1000000)::oid) AS pin;

SELECT 'pin_rollback_refs_count_zero' AS what,
       (SELECT count(*) FROM jbtl_subtree_refs) = 0 AS pin;

DROP TABLE m50b2_rb;

-- =====================================================================
-- Pin 10: check() returns 0 after all the activity above.
-- (No dead edges should remain because each test cleaned up.)
-- =====================================================================
SELECT 'pin_check_empty_after_cleanup' AS what,
       jbtl_subtree_refs_check() = 0 AS pin;
