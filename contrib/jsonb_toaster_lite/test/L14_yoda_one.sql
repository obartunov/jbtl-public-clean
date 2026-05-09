--
-- jsonb_toaster_lite L1.4 — yoda matrix bench (per-variant setup)
--
-- Parameters injected via psql:
--   :variant    A | B | C | D | E
--   :sort_guc   on | off  (only for B/C/D/E; A doesn't have the GUC)
--   :use_lite   1 | 0     (1 only for D/E)
--   :compress   on | off  (only for E)
--   :csv_out    output filename
--
-- Five matrix cells:
--   A  vanilla cluster, ordinary jsonb (no extensions, no GUC)
--   B  patched cluster, plain jsonb, no set_toaster, GUC=off
--   C  patched cluster, plain jsonb, no set_toaster, GUC=on
--   D  patched cluster, jsonb_toaster_lite plain, GUC=on
--   E  patched cluster, jsonb_toaster_lite compressed, GUC=on
--

\set ON_ERROR_STOP on
\pset pager off
\timing off

-- Conditional extension install based on variant.
\if :{?use_extensions}
CREATE EXTENSION IF NOT EXISTS toastapi;
CREATE EXTENSION IF NOT EXISTS jsonb_toaster_lite;
\endif

CREATE EXTENSION IF NOT EXISTS pg_buffercache;

-- Conditional GUC (vanilla doesn't have it; gracefully skip).
\if :{?set_sort_guc}
SET jsonb_sort_field_values = :sort_guc;
\endif

DROP TABLE IF EXISTS test_v;
CREATE TABLE test_v (id int PRIMARY KEY, jb jsonb STORAGE EXTERNAL);

\if :{?use_lite}
SELECT pgpro_toast.set_toaster('jsonb_toaster_lite', 'test_v', 'jb') > 0
       AS attached;
\endif

\if :{?use_compression}
SET jsonb_toaster_lite.compress_chunks = on;
\endif

-- Deterministic data: identical across all five variants because
-- md5() is a pure function of its argument.  Seed is the i value
-- offset; this is the same generator as in L14_canonical_sweep.sql.
\echo Building 100 rows...
INSERT INTO test_v
SELECT i, jsonb_build_object(
    'key1', i,
    'key2', (SELECT jsonb_agg(md5((i*1000 + s)::text))
               FROM generate_series(1, pow(10, 1 + 3.0 * i / 100.0)::int) s),
    'key3', i,
    'key4', (SELECT jsonb_agg(md5((i*9999 + s)::text))
               FROM generate_series(1, pow(10, 0 + 3.0 * i / 100.0)::int) s)
  ) AS jb
FROM generate_series(1, 100) i;

\if :{?use_compression}
SET jsonb_toaster_lite.compress_chunks = off;
\endif

VACUUM ANALYZE test_v;

\echo Built; verifying baseline:
SELECT min(pg_column_size(jb)) AS min_b, max(pg_column_size(jb)) AS max_b,
       pg_size_pretty(pg_total_relation_size('test_v')) AS total
FROM test_v;

-- helpers ------------------------------------------------------

CREATE OR REPLACE FUNCTION evict_three(rel regclass) RETURNS void
LANGUAGE plpgsql AS $body$
DECLARE base oid; toast_oid oid; pkey_oid oid;
BEGIN
    base := rel::oid;
    SELECT reltoastrelid INTO toast_oid FROM pg_class WHERE oid = base;
    SELECT idx.indexrelid INTO pkey_oid
      FROM pg_index idx WHERE idx.indrelid = base AND idx.indisprimary;
    PERFORM pg_buffercache_evict_relation(base);
    IF toast_oid <> 0 THEN PERFORM pg_buffercache_evict_relation(toast_oid); END IF;
    IF pkey_oid IS NOT NULL THEN PERFORM pg_buffercache_evict_relation(pkey_oid); END IF;
END;
$body$;

CREATE OR REPLACE FUNCTION bench_one(
    p_id int, p_key text, p_path text DEFAULT 'arrow',
    p_repeats int DEFAULT 1000,
    OUT elapsed_us numeric
) RETURNS numeric LANGUAGE plpgsql AS $body$
DECLARE
    sql text; expr text;
    t0 timestamptz; t1 timestamptz;
    parts text[] := '{}';
    i int; r record;
BEGIN
    IF p_path = 'fast' THEN
        expr := format('jbtl_object_field(jb, %L)', p_key);
    ELSE
        expr := format('jb -> %L', p_key);
    END IF;
    FOR i IN 1..p_repeats LOOP
        parts := array_append(parts, expr);
    END LOOP;
    sql := 'SELECT ' || array_to_string(parts, ', ') ||
           format(' FROM test_v WHERE id = %s', p_id);
    PERFORM evict_three('test_v'::regclass);
    t0 := clock_timestamp();
    EXECUTE sql INTO r;
    t1 := clock_timestamp();
    elapsed_us := round(EXTRACT(epoch FROM (t1 - t0)) * 1000000.0, 1);
END;
$body$;

CREATE TEMP TABLE results (
    variant text, i int, jsonb_size_bytes int, key text,
    path text, elapsed_us numeric
);

\echo Running sweep...
DO $main$
DECLARE
    r record;
    k text;
    t numeric;
BEGIN
    FOR r IN SELECT id AS i, pg_column_size(jb)::int AS sz
             FROM test_v ORDER BY id LOOP
        FOR k IN SELECT unnest(ARRAY['key1','key2','key3','key4']) LOOP
            -- Always measure the operator path
            SELECT elapsed_us INTO t FROM bench_one(r.i, k, 'arrow');
            INSERT INTO results
              VALUES (current_setting('jbtl.variant'), r.i, r.sz, k, 'arrow', t);
        END LOOP;
        IF r.i % 25 = 0 THEN
            RAISE NOTICE '  variant=% i=% sz=%', current_setting('jbtl.variant'), r.i, r.sz;
        END IF;
    END LOOP;
END $main$;

-- For D/E variants also measure jbtl_object_field fast path.
\if :{?use_lite}
\echo Running fast-path sweep...
DO $main$
DECLARE
    r record;
    k text;
    t numeric;
BEGIN
    FOR r IN SELECT id AS i, pg_column_size(jb)::int AS sz
             FROM test_v ORDER BY id LOOP
        FOR k IN SELECT unnest(ARRAY['key1','key2','key3','key4']) LOOP
            SELECT elapsed_us INTO t FROM bench_one(r.i, k, 'fast');
            INSERT INTO results
              VALUES (current_setting('jbtl.variant') || '_fast',
                      r.i, r.sz, k, 'fast', t);
        END LOOP;
    END LOOP;
END $main$;
\endif

\echo Writing CSV: :csv_out

COPY (
    SELECT variant, i, jsonb_size_bytes, key, path,
           round(elapsed_us / 1000.0, 3) AS us_per_call
    FROM results
    ORDER BY variant, i, key
) TO :'csv_out' WITH CSV HEADER;

DROP TABLE test_v;
DROP FUNCTION bench_one(int, text, text, int);
DROP FUNCTION evict_three(regclass);

\echo done :variant
