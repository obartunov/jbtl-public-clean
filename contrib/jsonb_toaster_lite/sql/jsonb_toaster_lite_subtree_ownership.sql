--
-- M9.1 SUBTREE ownership regression.
--
-- Pins down the observable ownership behaviour of cleaned SUBTREE
-- storage (JBTL_POINTER_SUBTREE + jbtl_subtree_refs catalog) across
-- the five scenarios that must be understood before SUBTREE can be
-- used as substrate for any future M-B (recursive sub-object reuse)
-- port:
--
--	T1  repeated UPDATE on byte-identical sub-object
--	T2  DELETE + VACUUM
--	T3  aborted UPDATE
--	T4  VACUUM FULL (orphan-edge behaviour + gc)
--	T5  flip-flop UPDATE between two byte-distinct shapes
--
-- The tests do NOT exercise M-B-style reuse (which does not exist
-- in cleaned).  They pin the BASELINE behaviour of the current
-- SUBTREE write/update/delete path so that any future M-B port can
-- be compared against this baseline.
--
-- Observability surface:
--	jbtl_subtree_refs                       direct catalog read
--	jbtl_subtree_refs_check() -> int        dead-edge count (child
--	                                        toast relation or
--	                                        valueid no longer
--	                                        exists)
--	jbtl_subtree_refs_gc()    -> int        delete dead edges,
--	                                        return count removed
--
-- Each section starts with jbtl_subtree_refs_gc() to clear any
-- catalog noise left by previous tests in the same database
-- (the catalog persists across DROP TABLE / DROP EXTENSION, so
-- absolute edge counts are NOT stable between runs).  After this
-- gc the catalog count starts at 0 for the section's purposes.
--

\set ON_ERROR_STOP on

CREATE EXTENSION IF NOT EXISTS toastapi;
CREATE EXTENSION IF NOT EXISTS jsonb_toaster_lite;

SET jsonb_sort_field_values = off;
SET jsonb_toaster_lite.enable_subtree_storage = on;
SET jsonb_toaster_lite.subtree_spill_threshold = 4096;

-- ============================================================
-- T1  repeated UPDATE on a byte-identical sub-object
--
-- Scenario:
--   Row whose `jb` has a large sub-object (key2) that triggers
--   SUBTREE spill.  Run several UPDATEs that produce a
--   byte-identical jb (same shape, same content).
--
-- What is being pinned:
--   - md5(jb::text) correct after each UPDATE;
--   - the live edge count for this table stays bounded (== 1);
--   - whether the child_valueid stays the same across UPDATEs
--     (observable: it does NOT in cleaned -- each UPDATE goes
--     through full re-toast: tsr_delete on the old SUBTREE row
--     deletes the old edge, tsr_toast on the new value inserts
--     a new edge.  Cleaned does not implement M-B-style reuse.
--     This test pins that baseline.)
--   - jbtl_subtree_refs_check() reports 0 dead edges between
--     successive UPDATEs (each UPDATE cleans up its own old edge
--     in the same transaction).
-- ============================================================

SELECT jbtl_subtree_refs_gc() AS noise_cleared;
SELECT count(*) AS t1_initial_total, jbtl_subtree_refs_check() AS t1_initial_dead
  FROM jbtl_subtree_refs;

DROP TABLE IF EXISTS t1_st CASCADE;
CREATE TABLE t1_st (id int PRIMARY KEY, jb jsonb STORAGE EXTERNAL);
SELECT pgpro_toast.set_toaster('jsonb_toaster_lite', 't1_st', 'jb') > 0 AS attached;

INSERT INTO t1_st SELECT 1, jsonb_build_object(
    'key1', 100,
    'key2', (SELECT jsonb_agg(md5((100*1000 + s)::text))
               FROM generate_series(1, 1000) s));

-- Snapshot the initial child_valueid for this parent so we can
-- detect "stable vs replaced" across UPDATEs.  Take parent_valueid
-- from the catalog (there should be exactly one edge after INSERT).
CREATE TEMP TABLE t1_pre AS
SELECT md5(jb::text) AS md5_pre FROM t1_st WHERE id=1;
CREATE TEMP TABLE t1_seq (
    phase text,
    md5_eq bool,
    edges_live int,           -- edges for THIS table's toast rel
    dead int,                 -- jbtl_subtree_refs_check()
    parent_vid_changed bool,  -- vs previous phase
    child_vid_changed bool    -- vs previous phase
);

DO $do$
DECLARE
    cur_md5   text;
    cur_pvid  oid;
    cur_cvid  oid;
    prev_pvid oid;
    prev_cvid oid;
    cur_edges int;
    cur_dead  int;
    toastrel  oid;
    phase_lbl text;
    i         int;
BEGIN
    SELECT reltoastrelid INTO toastrel FROM pg_class WHERE relname='t1_st';

    -- Record baseline post-INSERT.
    SELECT md5(jb::text), parent_valueid, child_valueid
      INTO cur_md5, cur_pvid, cur_cvid
      FROM t1_st, jbtl_subtree_refs
     WHERE id=1 AND parent_toastrelid=toastrel
     LIMIT 1;
    SELECT count(*) INTO cur_edges
      FROM jbtl_subtree_refs WHERE parent_toastrelid=toastrel;
    SELECT jbtl_subtree_refs_check() INTO cur_dead;

    INSERT INTO t1_seq VALUES
        ('post_insert', cur_md5 = (SELECT md5_pre FROM t1_pre),
         cur_edges, cur_dead, NULL, NULL);

    prev_pvid := cur_pvid; prev_cvid := cur_cvid;

    -- Five UPDATEs that produce byte-identical jb.
    FOR i IN 1..5 LOOP
        UPDATE t1_st SET jb = jsonb_build_object(
            'key1', 100,
            'key2', (SELECT jsonb_agg(md5((100*1000 + s)::text))
                       FROM generate_series(1, 1000) s))
         WHERE id=1;

        SELECT md5(jb::text), parent_valueid, child_valueid
          INTO cur_md5, cur_pvid, cur_cvid
          FROM t1_st, jbtl_subtree_refs
         WHERE id=1 AND parent_toastrelid=toastrel
         LIMIT 1;
        SELECT count(*) INTO cur_edges
          FROM jbtl_subtree_refs WHERE parent_toastrelid=toastrel;
        SELECT jbtl_subtree_refs_check() INTO cur_dead;

        phase_lbl := 'after_update_' || i::text;
        INSERT INTO t1_seq VALUES
            (phase_lbl, cur_md5 = (SELECT md5_pre FROM t1_pre),
             cur_edges, cur_dead,
             cur_pvid <> prev_pvid, cur_cvid <> prev_cvid);
        prev_pvid := cur_pvid; prev_cvid := cur_cvid;
    END LOOP;
END $do$;

SELECT * FROM t1_seq ORDER BY ctid;

-- Aggregate assertions independent of the row-by-row dump.
SELECT
    bool_and(md5_eq) AS t1_md5_all_correct,
    bool_and(edges_live = 1) AS t1_edges_stable_at_1,
    bool_and(dead = 0) AS t1_no_dead_during_chain
  FROM t1_seq;

-- T1 cleanup.
DROP TABLE t1_st;
DROP TABLE t1_pre;
DROP TABLE t1_seq;

-- ============================================================
-- T2  DELETE + VACUUM
--
-- Scenario:
--   Insert a SUBTREE row, DELETE it, VACUUM.
--
-- What is being pinned:
--   - row physically gone after DELETE + VACUUM;
--   - edges for this parent_toastrelid gone (tsr_delete called
--     jbtl_subtree_refs_delete_one which removed the edge);
--   - jbtl_subtree_refs_check() returns 0 (no orphans created).
-- ============================================================

SELECT jbtl_subtree_refs_gc() AS noise_cleared;

DROP TABLE IF EXISTS t2_st CASCADE;
CREATE TABLE t2_st (id int PRIMARY KEY, jb jsonb STORAGE EXTERNAL);
SELECT pgpro_toast.set_toaster('jsonb_toaster_lite', 't2_st', 'jb') > 0 AS attached;

INSERT INTO t2_st SELECT 1, jsonb_build_object(
    'key1', 100,
    'key2', (SELECT jsonb_agg(md5((100*1000 + s)::text))
               FROM generate_series(1, 1000) s));

-- Post-INSERT: 1 live edge for this table.
SELECT 'post_insert' AS phase,
       count(*) AS edges_for_t2,
       jbtl_subtree_refs_check() AS dead
  FROM jbtl_subtree_refs r, pg_class c
 WHERE c.relname='t2_st' AND r.parent_toastrelid = c.reltoastrelid;

DELETE FROM t2_st WHERE id=1;
VACUUM t2_st;

-- After DELETE + VACUUM:
--   - logical row gone;
--   - edges for this parent_toastrelid should be 0 (tsr_delete
--     called delete_one);
--   - dead count should be 0 (delete_one is symmetric with insert,
--     and the child chunk chain is physically removed when refcount
--     hits zero).
SELECT 'post_delete_vacuum' AS phase,
       (SELECT count(*) FROM t2_st) AS row_count,
       (SELECT count(*) FROM jbtl_subtree_refs r, pg_class c
        WHERE c.relname='t2_st' AND r.parent_toastrelid = c.reltoastrelid)
           AS edges_for_t2,
       jbtl_subtree_refs_check() AS dead;

DROP TABLE t2_st;

-- ============================================================
-- T3  aborted UPDATE
--
-- Scenario:
--   Insert a SUBTREE row, begin a transaction, run an UPDATE
--   that touches the SUBTREE child, ROLLBACK, VACUUM.
--
-- What is being pinned:
--   - md5(jb::text) equals pre-BEGIN value;
--   - edge count is back to pre-BEGIN (the UPDATE inserted a new
--     edge and deleted the old one inside the transaction; both
--     are MVCC-reverted on ROLLBACK);
--   - dead-edge count is whatever it was before BEGIN (no
--     orphans introduced).
-- ============================================================

SELECT jbtl_subtree_refs_gc() AS noise_cleared;

DROP TABLE IF EXISTS t3_st CASCADE;
CREATE TABLE t3_st (id int PRIMARY KEY, jb jsonb STORAGE EXTERNAL);
SELECT pgpro_toast.set_toaster('jsonb_toaster_lite', 't3_st', 'jb') > 0 AS attached;

INSERT INTO t3_st SELECT 1, jsonb_build_object(
    'key1', 100,
    'key2', (SELECT jsonb_agg(md5((100*1000 + s)::text))
               FROM generate_series(1, 1000) s));

CREATE TEMP TABLE t3_pre AS
SELECT
    md5(jb::text) AS md5_pre,
    (SELECT count(*) FROM jbtl_subtree_refs r, pg_class c
     WHERE c.relname='t3_st' AND r.parent_toastrelid = c.reltoastrelid)
       AS edges_pre,
    jbtl_subtree_refs_check() AS dead_pre
  FROM t3_st WHERE id=1;

BEGIN;
UPDATE t3_st SET jb = jsonb_build_object(
    'key1', 200,
    'key2', (SELECT jsonb_agg(md5((200*1000 + s)::text))
               FROM generate_series(1, 1000) s))
 WHERE id=1;
ROLLBACK;

VACUUM t3_st;

SELECT 'after_rollback_vacuum' AS phase,
       md5(jb::text) = (SELECT md5_pre FROM t3_pre) AS md5_preserved,
       (SELECT count(*) FROM jbtl_subtree_refs r, pg_class c
        WHERE c.relname='t3_st' AND r.parent_toastrelid = c.reltoastrelid)
            = (SELECT edges_pre FROM t3_pre)
        AS edges_match_pre,
       jbtl_subtree_refs_check()
            = (SELECT dead_pre FROM t3_pre)
        AS dead_match_pre
  FROM t3_st WHERE id=1;

DROP TABLE t3_st;
DROP TABLE t3_pre;

-- ============================================================
-- T4  VACUUM FULL
--
-- Scenario:
--   Several rows with SUBTREE.  VACUUM FULL.
--
-- What is being pinned:
--   - md5(jb::text) preserved per row across VACUUM FULL;
--   - public `->` correctness after rewrite;
--   - the state of jbtl_subtree_refs after the rewrite is
--     explicitly recorded.  The rewrite goes through
--     detoast + retoast, which:
--       1. allocates a NEW toast relation OID for the rewritten
--          heap (so child_toastrelid of old edges no longer
--          matches any live table's reltoastrelid);
--       2. inserts fresh edges for each rewritten row;
--       3. leaves OLD edges in the catalog pointing at the dropped
--          toast relation (orphans).
--   - jbtl_subtree_refs_check() reports the orphan count;
--   - jbtl_subtree_refs_gc() cleans them up.
--   - logical value is still preserved after gc.
-- ============================================================

SELECT jbtl_subtree_refs_gc() AS noise_cleared;

DROP TABLE IF EXISTS t4_st CASCADE;
CREATE TABLE t4_st (id int PRIMARY KEY, jb jsonb STORAGE EXTERNAL);
SELECT pgpro_toast.set_toaster('jsonb_toaster_lite', 't4_st', 'jb') > 0 AS attached;

INSERT INTO t4_st SELECT g, jsonb_build_object(
    'id',   g,
    'key1', g * 10,
    'key2', (SELECT jsonb_agg(md5((g*1000 + s)::text))
               FROM generate_series(1, 1000) s))
  FROM generate_series(1, 3) g;

CREATE TEMP TABLE t4_pre AS
SELECT id, md5(jb::text) AS md5_pre,
       jb -> 'key2' ->> 0 AS arrow_pre
  FROM t4_st ORDER BY id;

-- Pre-rewrite catalog state.
SELECT 'pre_vacuum_full' AS phase,
       (SELECT count(*) FROM jbtl_subtree_refs r, pg_class c
        WHERE c.relname='t4_st' AND r.parent_toastrelid = c.reltoastrelid)
           AS edges_for_t4,
       (SELECT count(*) FROM jbtl_subtree_refs) AS edges_total,
       jbtl_subtree_refs_check() AS dead;

VACUUM FULL t4_st;

-- Post-rewrite: logical preservation.
SELECT id,
       md5(jb::text) = (SELECT md5_pre FROM t4_pre p WHERE p.id = c.id)
       AS t4_md5_preserved,
       (jb -> 'key2' ->> 0) = (SELECT arrow_pre FROM t4_pre p WHERE p.id = c.id)
       AS t4_arrow_ok
  FROM t4_st c
 ORDER BY id;

-- Post-rewrite: catalog state.  Record what is observable.
-- After VACUUM FULL, the old toast relation is gone and a new one
-- was allocated.  Live edges are for the NEW reltoastrelid; old
-- edges are orphans whose child_toastrelid is the dropped old
-- toast.
SELECT 'post_vacuum_full_before_gc' AS phase,
       (SELECT count(*) FROM jbtl_subtree_refs r, pg_class c
        WHERE c.relname='t4_st' AND r.parent_toastrelid = c.reltoastrelid)
           AS edges_live_for_t4,
       (SELECT count(*) FROM jbtl_subtree_refs) AS edges_total,
       jbtl_subtree_refs_check() AS dead;

-- Run gc.
SELECT jbtl_subtree_refs_gc() AS t4_gc_removed;

-- Post-gc.  Live edges for t4_st must remain unchanged; orphan
-- count must be 0.
SELECT 'post_vacuum_full_after_gc' AS phase,
       (SELECT count(*) FROM jbtl_subtree_refs r, pg_class c
        WHERE c.relname='t4_st' AND r.parent_toastrelid = c.reltoastrelid)
           AS edges_live_for_t4,
       (SELECT count(*) FROM jbtl_subtree_refs) AS edges_total,
       jbtl_subtree_refs_check() AS dead;

-- Logical value still preserved after gc.
SELECT id,
       md5(jb::text) = (SELECT md5_pre FROM t4_pre p WHERE p.id = c.id)
       AS t4_md5_preserved_after_gc
  FROM t4_st c
 ORDER BY id;

DROP TABLE t4_st;
DROP TABLE t4_pre;

-- ============================================================
-- T5  flip-flop UPDATE between two byte-distinct shapes
--
-- Scenario:
--   Repeatedly flip the SUBTREE sub-object between two distinct
--   shapes (different jsonb_agg seeds).
--
-- What is being pinned:
--   - md5(jb::text) follows the expected shape pattern;
--   - live edge count for this table stays bounded at 1;
--   - dead count stays at 0 (the in-transaction delete_one drops
--     the old edge, the in-transaction insert adds the new; no
--     edge ever survives past its own UPDATE as a corpse);
--   - jbtl_subtree_refs_gc() at the end returns 0 (nothing to
--     reclaim).
-- ============================================================

SELECT jbtl_subtree_refs_gc() AS noise_cleared;

DROP TABLE IF EXISTS t5_st CASCADE;
CREATE TABLE t5_st (id int PRIMARY KEY, jb jsonb STORAGE EXTERNAL);
SELECT pgpro_toast.set_toaster('jsonb_toaster_lite', 't5_st', 'jb') > 0 AS attached;

INSERT INTO t5_st SELECT 1, jsonb_build_object(
    'key1', 100,
    'key2', (SELECT jsonb_agg(md5((100*1000 + s)::text))
               FROM generate_series(1, 1000) s));

CREATE TEMP TABLE t5_md5 AS
SELECT
    md5(jsonb_build_object(
        'key1', 100,
        'key2', (SELECT jsonb_agg(md5((100*1000 + s)::text))
                   FROM generate_series(1, 1000) s))::text) AS shape_a,
    md5(jsonb_build_object(
        'key1', 200,
        'key2', (SELECT jsonb_agg(md5((200*1000 + s)::text))
                   FROM generate_series(1, 1000) s))::text) AS shape_b;

CREATE TEMP TABLE t5_seq (
    phase text,
    md5_matches_expected bool,
    edges_live int,
    dead int
);

DO $do$
DECLARE
    i int;
    cur_md5 text;
    cur_edges int;
    cur_dead int;
    expected_md5 text;
    toastrel oid;
BEGIN
    SELECT reltoastrelid INTO toastrel FROM pg_class WHERE relname='t5_st';

    -- 6 flips: A→B→A→B→A→B
    FOR i IN 1..6 LOOP
        IF (i % 2) = 1 THEN
            UPDATE t5_st SET jb = jsonb_build_object(
                'key1', 200,
                'key2', (SELECT jsonb_agg(md5((200*1000 + s)::text))
                           FROM generate_series(1, 1000) s))
             WHERE id=1;
            SELECT shape_b INTO expected_md5 FROM t5_md5;
        ELSE
            UPDATE t5_st SET jb = jsonb_build_object(
                'key1', 100,
                'key2', (SELECT jsonb_agg(md5((100*1000 + s)::text))
                           FROM generate_series(1, 1000) s))
             WHERE id=1;
            SELECT shape_a INTO expected_md5 FROM t5_md5;
        END IF;

        SELECT md5(jb::text) INTO cur_md5 FROM t5_st WHERE id=1;
        SELECT count(*) INTO cur_edges
          FROM jbtl_subtree_refs WHERE parent_toastrelid=toastrel;
        SELECT jbtl_subtree_refs_check() INTO cur_dead;

        INSERT INTO t5_seq VALUES (
            'flip_' || i::text,
            cur_md5 = expected_md5,
            cur_edges,
            cur_dead);
    END LOOP;
END $do$;

SELECT * FROM t5_seq ORDER BY ctid;

SELECT
    bool_and(md5_matches_expected) AS t5_md5_all_correct,
    bool_and(edges_live = 1) AS t5_edges_stable_at_1,
    bool_and(dead = 0) AS t5_no_dead_during_chain
  FROM t5_seq;

-- Final gc should reclaim nothing.
SELECT jbtl_subtree_refs_gc() AS t5_final_gc_removed;

DROP TABLE t5_st;
DROP TABLE t5_md5;
DROP TABLE t5_seq;

RESET jsonb_toaster_lite.enable_subtree_storage;
RESET jsonb_toaster_lite.subtree_spill_threshold;
