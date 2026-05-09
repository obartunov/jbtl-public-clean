--
-- jsonb_toaster_lite L1.4 — continuous-sweep bench for plotting
--
-- Same data shape and query pattern as L14_canonical_bench.sql,
-- but sweeps i continuously over 1..100 (matching paper's
-- methodology for the scatter-plot figures, slides 31 and 44+).
--
-- Output is a CSV file written via COPY to /tmp/canon_sweep.csv,
-- consumed by L14_canonical_plot.py to produce slide-style PNG
-- figures.
--
-- Build budget: formula caps jsonb at ~500 KB at i=100, total
-- ~30 MB over 100 rows.  Bench loop ~6-15 minutes depending on
-- VM I/O.
--

\set ON_ERROR_STOP on
\pset pager off
\timing off

CREATE EXTENSION IF NOT EXISTS toastapi;
CREATE EXTENSION IF NOT EXISTS jsonb_toaster_lite;
CREATE EXTENSION IF NOT EXISTS pg_buffercache;

SET jsonb_sort_field_values = on;

DROP TABLE IF EXISTS test_a, test_b;
CREATE TABLE test_a (id int PRIMARY KEY, jb jsonb STORAGE EXTERNAL);
CREATE TABLE test_b (id int PRIMARY KEY, jb jsonb STORAGE EXTERNAL);
SELECT pgpro_toast.set_toaster('jsonb_toaster_lite', 'test_b', 'jb') > 0;

-- Build i=1..100, peaks at ~500 KB jsonb (vs paper's 13 MB).
-- We use 3.0 instead of paper's 5.0 in the exponent to bound build
-- time and keep total dataset under 50 MB on this VM.
\echo Building 100-row dataset (this takes a moment)...

CREATE TEMP TABLE docs AS
SELECT i, jsonb_build_object(
    'key1', i,
    'key2', (SELECT jsonb_agg(md5((i*1000 + s)::text))
               FROM generate_series(1, pow(10, 1 + 3.0 * i / 100.0)::int) s),
    'key3', i,
    'key4', (SELECT jsonb_agg(md5((i*9999 + s)::text))
               FROM generate_series(1, pow(10, 0 + 3.0 * i / 100.0)::int) s)
  ) AS jb
FROM generate_series(1, 100) i;

INSERT INTO test_a SELECT i, jb FROM docs;
INSERT INTO test_b SELECT i, jb FROM docs;
DROP TABLE docs;
VACUUM ANALYZE test_a, test_b;

\echo Dataset size summary:

SELECT
  min(pg_column_size(jb))    AS min_size_b,
  max(pg_column_size(jb))    AS max_size_b,
  pg_size_pretty(pg_total_relation_size('test_a')) AS vanilla_total,
  pg_size_pretty(pg_total_relation_size('test_b')) AS lite_total
FROM test_a;

-- helpers ------------------------------------------------------

CREATE OR REPLACE FUNCTION evict_three(rel regclass) RETURNS void
LANGUAGE plpgsql AS $body$
DECLARE base oid; toast_oid oid; pkey_oid oid;
BEGIN
    base := rel::oid;
    SELECT reltoastrelid INTO toast_oid FROM pg_class WHERE oid = base;
    SELECT i.indexrelid INTO pkey_oid
      FROM pg_index i WHERE i.indrelid = base AND i.indisprimary;
    PERFORM pg_buffercache_evict_relation(base);
    IF toast_oid <> 0 THEN PERFORM pg_buffercache_evict_relation(toast_oid); END IF;
    IF pkey_oid IS NOT NULL THEN PERFORM pg_buffercache_evict_relation(pkey_oid); END IF;
END;
$body$;

CREATE OR REPLACE FUNCTION bench_one(
    p_table text, p_id int, p_key text,
    p_fast_path boolean, p_repeats int DEFAULT 1000,
    OUT elapsed_us numeric
) RETURNS numeric LANGUAGE plpgsql AS $body$
DECLARE
    sql text; expr text;
    t0 timestamptz; t1 timestamptz;
    parts text[] := '{}';
    i int; r record;
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
    PERFORM evict_three(p_table::regclass);
    t0 := clock_timestamp();
    EXECUTE sql INTO r;
    t1 := clock_timestamp();
    elapsed_us := round(EXTRACT(epoch FROM (t1 - t0)) * 1000000.0, 1);
END;
$body$;

CREATE TEMP TABLE results (
    i int, jsonb_size_bytes int, key text,
    variant text, elapsed_us numeric
);

\echo Running sweep (100 i x 4 keys x 3 variants x 1000 ops cold)...

DO $main$
DECLARE
    r record;
    k text;
    t numeric;
    n_repeats int := 1000;
    last_progress int := 0;
BEGIN
    FOR r IN SELECT id AS i, pg_column_size(jb)::int AS sz
             FROM test_a ORDER BY id LOOP
        FOR k IN SELECT unnest(ARRAY['key1','key2','key3','key4']) LOOP
            SELECT elapsed_us INTO t FROM bench_one('test_a', r.i, k, false, n_repeats);
            INSERT INTO results VALUES (r.i, r.sz, k, 'vanilla', t);

            SELECT elapsed_us INTO t FROM bench_one('test_b', r.i, k, false, n_repeats);
            INSERT INTO results VALUES (r.i, r.sz, k, 'lite', t);

            SELECT elapsed_us INTO t FROM bench_one('test_b', r.i, k, true, n_repeats);
            INSERT INTO results VALUES (r.i, r.sz, k, 'lite_kvmap', t);
        END LOOP;
        IF r.i % 10 = 0 THEN
            RAISE NOTICE 'progress: i=% (size=% B)', r.i, r.sz;
        END IF;
    END LOOP;
END
$main$;

\echo Sweep complete; writing CSV...

COPY (
    SELECT i, jsonb_size_bytes, key, variant,
           round(elapsed_us / 1000.0, 3) AS us_per_call
    FROM results
    ORDER BY i, key, variant
) TO '/tmp/canon_sweep.csv' WITH CSV HEADER;

DROP TABLE test_a, test_b;
DROP FUNCTION bench_one(text, int, text, boolean, int);
DROP FUNCTION evict_three(regclass);

\echo wrote /tmp/canon_sweep.csv
