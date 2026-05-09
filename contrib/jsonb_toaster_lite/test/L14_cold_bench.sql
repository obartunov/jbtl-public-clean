--
-- jsonb_toaster_lite L1.4 cold-cache bench
--
-- Where L13b_realistic_bench measures hot-cache per-op cost
-- (toast chunks all fit in shared_buffers after warmup), this
-- bench evicts the toast relation from shared_buffers BEFORE
-- each timed call.  That puts us in the regime where
-- chunks_fetched actually drives wall time.
--
-- Methodology:
--   - one row, ~50 KB realistic-shape jsonb (same as L13b)
--   - per trial:
--       pg_buffercache_evict_relation(toast_rel)  -- cold
--       single SELECT under \timing
--       record elapsed
--   - median over N=50 trials
--
-- Eviction is per-relation, not per-cache-line, so we evict the
-- whole toast and base relation each time.  This includes index
-- pages too; hot j->> pays index re-warmup cost on every call.
-- That is the true cost the L1.4 fast path saves.
--

\set ON_ERROR_STOP on
\pset pager off

CREATE EXTENSION IF NOT EXISTS toastapi;
CREATE EXTENSION IF NOT EXISTS jsonb_toaster_lite;
CREATE EXTENSION IF NOT EXISTS pg_buffercache;

SET jsonb_sort_field_values = on;

CREATE TABLE bench_b (id int PRIMARY KEY, j jsonb STORAGE EXTERNAL);
SELECT pgpro_toast.set_toaster('jsonb_toaster_lite', 'bench_b', 'j') > 0
       AS attached_b;

INSERT INTO bench_b SELECT 1, jsonb_build_object(
    'id',       42,
    'status',   'STATUS_marker_active_x9',
    'kind',     'KIND_marker_invoice_x9',
    'tenant',   'TENANT_marker_acme_x9',
    'flags',    jsonb_build_array('flag_paid', 'flag_indexed', 'flag_archived'),
    'title',    'TITLE_marker_x9_'    || repeat('t', 200),
    'summary',  'SUMMARY_marker_x9_'  || repeat('s', 1000),
    'payload',  'PAYLOAD_marker_x9_'  ||
                (SELECT string_agg(md5(g::text), '')
                   FROM generate_series(1, 1500) g));

VACUUM ANALYZE bench_b;

-- Capture toast relation OID for eviction.
SELECT reltoastrelid AS toast_oid
FROM pg_class WHERE relname = 'bench_b' \gset

-- ---------------------------------------------------------------- helper

-- bench_cold(label, sql, trials=30)
-- For each trial:
--   evict bench_b's toast relation buffers,
--   evict bench_b's base relation buffers,
--   evict bench_b's PK index buffers,
--   run sql once, record clock_timestamp() delta in microseconds.
-- Reports median, min, max, n.

CREATE OR REPLACE FUNCTION bench_cold(
    p_label text, p_sql text,
    p_trials int DEFAULT 30,
    OUT op text, OUT median_us numeric,
    OUT min_us numeric, OUT max_us numeric,
    OUT trials int
) RETURNS record
LANGUAGE plpgsql AS $body$
DECLARE
    samples numeric[] := '{}';
    t0 timestamptz;
    t1 timestamptz;
    i int;
    r record;
    base_oid oid;
    toast_oid oid;
    pk_oid oid;
BEGIN
    SELECT 'bench_b'::regclass::oid INTO base_oid;
    SELECT reltoastrelid FROM pg_class WHERE oid = base_oid INTO toast_oid;
    SELECT 'bench_b_pkey'::regclass::oid INTO pk_oid;

    FOR i IN 1..p_trials LOOP
        PERFORM pg_buffercache_evict_relation(base_oid);
        PERFORM pg_buffercache_evict_relation(toast_oid);
        PERFORM pg_buffercache_evict_relation(pk_oid);

        t0 := clock_timestamp();
        EXECUTE p_sql INTO r;
        t1 := clock_timestamp();

        samples := array_append(samples,
            EXTRACT(epoch FROM (t1 - t0)) * 1000000.0);
    END LOOP;

    op := p_label;
    trials := p_trials;
    SELECT round(percentile_cont(0.5) WITHIN GROUP (ORDER BY s)::numeric, 1),
           round(min(s)::numeric, 1),
           round(max(s)::numeric, 1)
      INTO median_us, min_us, max_us
      FROM unnest(samples) s;
END;
$body$;

CREATE TEMP TABLE bench_results (
    op text, median_us numeric, min_us numeric,
    max_us numeric, trials int);

-- The contestants:
--   B_key_small    : j->>'status'                       (full-detoast path)
--   B_key_medium   : j->>'summary'
--   B_key_large    : j->>'payload'
--   B_kv_small     : jbtl_object_field_text(j,'status') (KVMap-aware fast path)
--   B_kv_medium    : jbtl_object_field_text(j,'summary')
--   B_kv_large     : jbtl_object_field_text(j,'payload') (fallback >50% threshold)
--   B_full         : SELECT j (control: full read for context)

INSERT INTO bench_results
SELECT * FROM bench_cold('B_full',
    $sql$SELECT j::text FROM bench_b WHERE id = 1$sql$);
INSERT INTO bench_results
SELECT * FROM bench_cold('B_key_small',
    $sql$SELECT j->>'status' FROM bench_b WHERE id = 1$sql$);
INSERT INTO bench_results
SELECT * FROM bench_cold('B_key_medium',
    $sql$SELECT j->>'summary' FROM bench_b WHERE id = 1$sql$);
INSERT INTO bench_results
SELECT * FROM bench_cold('B_key_large',
    $sql$SELECT j->>'payload' FROM bench_b WHERE id = 1$sql$);
INSERT INTO bench_results
SELECT * FROM bench_cold('B_kv_small',
    $sql$SELECT jbtl_object_field_text(j, 'status') FROM bench_b WHERE id = 1$sql$);
INSERT INTO bench_results
SELECT * FROM bench_cold('B_kv_medium',
    $sql$SELECT jbtl_object_field_text(j, 'summary') FROM bench_b WHERE id = 1$sql$);
INSERT INTO bench_results
SELECT * FROM bench_cold('B_kv_large',
    $sql$SELECT jbtl_object_field_text(j, 'payload') FROM bench_b WHERE id = 1$sql$);

\echo
\echo === COLD CACHE: median microseconds per op (n=30 trials) ===
\echo

SELECT op, median_us, min_us, max_us
FROM bench_results
ORDER BY op;

\echo
\echo === Speedup ratios (KVMap fast path vs j-arrow-arrow) ===
\echo

WITH p AS (
  SELECT
    (SELECT median_us FROM bench_results WHERE op = 'B_key_small')   AS old_small,
    (SELECT median_us FROM bench_results WHERE op = 'B_kv_small')    AS new_small,
    (SELECT median_us FROM bench_results WHERE op = 'B_key_medium')  AS old_medium,
    (SELECT median_us FROM bench_results WHERE op = 'B_kv_medium')   AS new_medium,
    (SELECT median_us FROM bench_results WHERE op = 'B_key_large')   AS old_large,
    (SELECT median_us FROM bench_results WHERE op = 'B_kv_large')    AS new_large
)
SELECT 'small  ' AS op,
       old_small || ' us' AS j_arrow,
       new_small || ' us' AS kvmap,
       round(old_small / new_small, 2) || 'x' AS speedup
FROM p
UNION ALL
SELECT 'medium ', old_medium || ' us', new_medium || ' us',
       round(old_medium / new_medium, 2) || 'x'
FROM p
UNION ALL
SELECT 'large  ', old_large || ' us', new_large || ' us',
       round(old_large / new_large, 2) || 'x  (fallback)'
FROM p;

DROP TABLE bench_b;
DROP FUNCTION bench_cold(text, text, int);

\echo === done ===
