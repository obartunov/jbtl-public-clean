--
-- jsonb_toaster_lite L1.4 — canonical Bartunov-Glukhov bench
--
-- Reproduces the synthetic test from slide 30 of "One TOAST fits all"
-- (toast-nizhny-2022.pdf, also pgvision-2021).  Data shape:
--
--   { 'key1': i,
--     'key2': [0, 0, ..., 0]   -- 10..100k elems   (LONG)
--     'key3': i,
--     'key4': [0, 0, ..., 0]   --  1..10k  elems   (medium)  }
--
-- Sizes scale exponentially with i in 1..N: from ~130 B at i=1 to
-- multi-MB at high i.  Original used i=1..100 for ~13 MB peak; we
-- cap at i=50 (~5 MB peak) so the test fits the VM budget but still
-- shows the predicted asymptotic shape.
--
-- Query pattern (also from the slide): repeat 1000 evaluations of
-- the operator inside a single SELECT, isolating per-call cost:
--
--   SELECT jb -> 'keyN', jb -> 'keyN', ..., jb -> 'keyN'
--     FROM test_toast WHERE id = ?
--
-- Three contestants on the same data:
--   A  vanilla        — default toaster, jb -> 'keyN'
--   B  lite           — jsonb_toaster_lite, jb -> 'keyN' (full detoast)
--   B' lite + KVMap   — jbtl_object_field(jb, 'keyN')   (L1.4 fast path)
--
-- L1.4 prediction (the Bartunov claim):
--   key1, key3 (scalars) -> KVMap fast path -> flat per-call cost
--   key2, key4 (arrays)  -> KVMap falls back -> tracks vanilla
--

\set ON_ERROR_STOP on
\pset pager off
\timing off

CREATE EXTENSION IF NOT EXISTS toastapi;
CREATE EXTENSION IF NOT EXISTS jsonb_toaster_lite;
CREATE EXTENSION IF NOT EXISTS pg_buffercache;

SET jsonb_sort_field_values = on;

-- N controls the upper bound of i and therefore the largest jsonb.
-- Original paper used 100 (->13 MB).  We use 50 (->~5 MB).
\set N 50

-- ---------------------------------------------------------------- tables

DROP TABLE IF EXISTS test_a, test_b;
CREATE TABLE test_a (id int PRIMARY KEY, jb jsonb STORAGE EXTERNAL);      -- vanilla, no inline compression
CREATE TABLE test_b (id int PRIMARY KEY, jb jsonb STORAGE EXTERNAL);      -- lite
SELECT pgpro_toast.set_toaster('jsonb_toaster_lite', 'test_b', 'jb') > 0
       AS attached_b;

-- Build the exact dataset from slide 30, but capped at i=N.
-- Sample i at points to span size range from <2KB (inline) through
-- 8KB (TOAST threshold) to multi-MB (heavily TOASTed).
-- Use md5() values inside arrays so the content is incompressible —
-- otherwise pglz collapses arrays of zeros 100x and even i=50 stays
-- inline, which defeats the whole point of testing TOAST scaling.
CREATE TEMP TABLE docs AS
SELECT i, jsonb_build_object(
    'key1', i,
    'key2', (SELECT jsonb_agg(md5((i*1000 + s)::text))
               FROM generate_series(1, pow(10, 1 + 4.0 * i / 100.0)::int) s),
    'key3', i,
    'key4', (SELECT jsonb_agg(md5((i*9999 + s)::text))
               FROM generate_series(1, pow(10, 0 + 4.0 * i / 100.0)::int) s)
  ) AS jb
FROM (VALUES (1),(10),(20),(30),(40),(50),(60),(70)) v(i);

INSERT INTO test_a SELECT i, jb FROM docs;
INSERT INTO test_b SELECT i, jb FROM docs;

VACUUM ANALYZE test_a, test_b;

\echo
\echo === dataset (i, jsonb size, key2 elems, key4 elems) ===
\echo
SELECT id AS i,
       pg_size_pretty(pg_column_size(jb)::bigint)        AS jsonb_size,
       jsonb_array_length(jb -> 'key2')                  AS key2_elems,
       jsonb_array_length(jb -> 'key4')                  AS key4_elems
FROM test_a ORDER BY id;

\echo
\echo === total storage ===
\echo
SELECT 'vanilla' AS variant,
       pg_size_pretty(pg_total_relation_size('test_a')) AS total
UNION ALL
SELECT 'lite',
       pg_size_pretty(pg_total_relation_size('test_b'));

-- ---------------------------------------------------------------- helpers

CREATE OR REPLACE FUNCTION evict_three(rel regclass) RETURNS void
LANGUAGE plpgsql AS $body$
DECLARE
    base oid; toast_oid oid; pkey_oid oid;
BEGIN
    base := rel::oid;
    SELECT reltoastrelid INTO toast_oid FROM pg_class WHERE oid = base;
    SELECT i.indexrelid INTO pkey_oid
      FROM pg_index i WHERE i.indrelid = base AND i.indisprimary;
    PERFORM pg_buffercache_evict_relation(base);
    IF toast_oid <> 0 THEN
        PERFORM pg_buffercache_evict_relation(toast_oid);
    END IF;
    IF pkey_oid IS NOT NULL THEN
        PERFORM pg_buffercache_evict_relation(pkey_oid);
    END IF;
END;
$body$;

-- bench_one(table, id, key, fast_path, n_repeats):
-- builds a SQL string with `n_repeats` references to either
-- `jb -> 'keyN'` (vanilla) or `jbtl_object_field(jb, 'keyN')`
-- (KVMap fast path), runs it once, returns wall-time in microseconds.
-- The relation is evicted before each call so the result reflects
-- cold-cache cost (matches paper methodology).
--
-- 1000 repeats matches the paper exactly.

CREATE OR REPLACE FUNCTION bench_one(
    p_table text, p_id int, p_key text,
    p_fast_path boolean, p_repeats int DEFAULT 1000,
    OUT elapsed_us numeric
) RETURNS numeric
LANGUAGE plpgsql AS $body$
DECLARE
    sql text;
    expr text;
    t0 timestamptz; t1 timestamptz;
    parts text[] := '{}';
    i int;
    r record;
BEGIN
    IF p_fast_path THEN
        expr := format('jbtl_object_field(jb, %L)', p_key);
    ELSE
        expr := format('jb -> %L', p_key);
    END IF;

    FOR i IN 1..p_repeats LOOP
        parts := array_append(parts, expr);
    END LOOP;
    sql := 'SELECT ' || array_to_string(parts, ', ') ||
           format(' FROM %I WHERE id = %s', p_table, p_id);

    -- evict for cold-cache timing
    PERFORM evict_three(p_table::regclass);

    t0 := clock_timestamp();
    EXECUTE sql INTO r;
    t1 := clock_timestamp();

    elapsed_us := round(EXTRACT(epoch FROM (t1 - t0)) * 1000000.0, 1);
END;
$body$;

-- ---------------------------------------------------------------- run

CREATE TEMP TABLE results (
    i int, jsonb_size_bytes bigint, key text,
    variant text, elapsed_us numeric
);

DO $main$
DECLARE
    r record;
    k text;
    t numeric;
    n_repeats int := 1000;
BEGIN
    FOR r IN SELECT id AS i, pg_column_size(jb)::bigint AS sz
             FROM test_a ORDER BY id LOOP
        FOR k IN SELECT unnest(ARRAY['key1','key2','key3','key4']) LOOP
            -- A: vanilla
            SELECT elapsed_us INTO t FROM bench_one('test_a', r.i, k, false, n_repeats);
            INSERT INTO results VALUES (r.i, r.sz, k, 'A_vanilla', t);

            -- B: lite (j->>)
            SELECT elapsed_us INTO t FROM bench_one('test_b', r.i, k, false, n_repeats);
            INSERT INTO results VALUES (r.i, r.sz, k, 'B_lite', t);

            -- B': lite + KVMap fast path
            SELECT elapsed_us INTO t FROM bench_one('test_b', r.i, k, true, n_repeats);
            INSERT INTO results VALUES (r.i, r.sz, k, 'C_lite_kvmap', t);
        END LOOP;
    END LOOP;
END
$main$;

\echo
\echo === RESULTS: per-call cost (elapsed_us / 1000 ops) ===
\echo  Reading: A_vanilla = vanilla j->;
\echo           B_lite    = lite j-> (full detoast);
\echo           C_lite_kvmap = lite + L1.4 fast path
\echo

SELECT i,
       pg_size_pretty(jsonb_size_bytes) AS sz,
       key,
       round((SELECT elapsed_us FROM results r2
              WHERE r2.i = r.i AND r2.key = r.key
                AND r2.variant = 'A_vanilla') / 1000.0, 1) AS vanilla_us,
       round((SELECT elapsed_us FROM results r2
              WHERE r2.i = r.i AND r2.key = r.key
                AND r2.variant = 'B_lite') / 1000.0, 1)    AS lite_us,
       round((SELECT elapsed_us FROM results r2
              WHERE r2.i = r.i AND r2.key = r.key
                AND r2.variant = 'C_lite_kvmap') / 1000.0, 1) AS lite_kv_us
FROM (SELECT DISTINCT i, jsonb_size_bytes, key FROM results) r
ORDER BY i, key;

\echo
\echo === KEY CLAIM: KVMap fast path is FLAT for key1/key3 ===
\echo  (per-call us by jsonb size, key1 and key3 should stay constant)
\echo

SELECT key,
       i, pg_size_pretty(jsonb_size_bytes) AS sz,
       round(elapsed_us / 1000.0, 1) AS us_per_call
FROM results
WHERE variant = 'C_lite_kvmap' AND key IN ('key1', 'key3')
ORDER BY key, i;

\echo
\echo === COUNTER-PART: KVMap falls back for key2/key4 (containers) ===
\echo  (should track vanilla, growing with size)
\echo

SELECT key, i,
       pg_size_pretty(jsonb_size_bytes) AS sz,
       round((SELECT elapsed_us FROM results r2
              WHERE r2.i = r.i AND r2.key = r.key
                AND r2.variant = 'A_vanilla') / 1000.0, 1) AS vanilla_us,
       round((SELECT elapsed_us FROM results r2
              WHERE r2.i = r.i AND r2.key = r.key
                AND r2.variant = 'C_lite_kvmap') / 1000.0, 1) AS kvmap_us
FROM (SELECT DISTINCT i, jsonb_size_bytes, key FROM results
      WHERE key IN ('key2', 'key4')) r
ORDER BY key, i;

\echo
\echo === Speedup factor: KVMap vs vanilla, by key ===
\echo

SELECT key, i,
       pg_size_pretty(jsonb_size_bytes) AS sz,
       round(
         (SELECT elapsed_us FROM results r2
          WHERE r2.i = r.i AND r2.key = r.key AND r2.variant = 'A_vanilla')
         /
         NULLIF((SELECT elapsed_us FROM results r2
                 WHERE r2.i = r.i AND r2.key = r.key
                   AND r2.variant = 'C_lite_kvmap'), 0),
         2) || 'x' AS speedup_kvmap_vs_vanilla
FROM (SELECT DISTINCT i, jsonb_size_bytes, key FROM results) r
ORDER BY key, i;

DROP TABLE test_a, test_b;
DROP FUNCTION bench_one(text, int, text, boolean, int);
DROP FUNCTION evict_three(regclass);

\echo === done ===
