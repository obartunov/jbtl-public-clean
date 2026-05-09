--
-- jsonb_toaster_lite L1.4 sequential-scan bench
--
-- Many rows scenario: each row a ~50 KB realistic-shape jsonb.
-- shared_buffers=128 MB, so 5000 rows of 50 KB body = ~250 MB
-- of toast pressure; cache thrashes naturally during a seq scan.
-- This reproduces the OLAP-ish workload where every row is
-- visited once and chunk count drives wall time.
--
-- Operations compared:
--   j->>'status'                          (full detoast per row)
--   jbtl_object_field_text(j, 'status')   (KVMap-aware, 1 chunk)
--
-- Reported: total elapsed for the seq scan.  No per-op cycle.
--

\set ON_ERROR_STOP on
\pset pager off
\timing off

CREATE EXTENSION IF NOT EXISTS toastapi;
CREATE EXTENSION IF NOT EXISTS jsonb_toaster_lite;
CREATE EXTENSION IF NOT EXISTS pg_buffercache;

SET jsonb_sort_field_values = on;

DROP TABLE IF EXISTS bench_b;
CREATE TABLE bench_b (id int PRIMARY KEY, j jsonb STORAGE EXTERNAL);
SELECT pgpro_toast.set_toaster('jsonb_toaster_lite', 'bench_b', 'j') > 0
       AS attached;

INSERT INTO bench_b
SELECT g, jsonb_build_object(
    'id',       g,
    'status',   'STATUS_marker_active_x' || g,
    'kind',     'KIND_marker_invoice_x'  || g,
    'tenant',   'TENANT_marker_acme_x'   || g,
    'flags',    jsonb_build_array('flag_paid', 'flag_indexed'),
    'title',    'TITLE_marker_x_'        || repeat('t', 200),
    'summary',  'SUMMARY_marker_x_'      || repeat('s', 1000),
    'payload',  'PAYLOAD_marker_x_' ||
                (SELECT string_agg(md5((g + s)::text), '')
                   FROM generate_series(1, 1500) s))
FROM generate_series(1, 5000) g;

VACUUM ANALYZE bench_b;

\echo
\echo === storage ===
\echo
SELECT pg_size_pretty(pg_total_relation_size('bench_b'))    AS total_size,
       pg_size_pretty(pg_relation_size('bench_b'))          AS heap_size,
       pg_size_pretty(pg_total_relation_size('bench_b')
                      - pg_relation_size('bench_b'))        AS toast_plus_index_size;

\echo
\echo === bench: 5000-row sequential scan with key access ===
\echo (single run each; cache state is whatever previous query left)
\echo

-- Force fresh each time by restarting the cluster between trials
-- is impractical here.  Instead, we run TWO repeated cycles of
-- (j->>, kv) to wash out one-time effects: the first run of each
-- pair pays for cache warmup, the second is hot.  Both paths get
-- the same treatment, so ratio is meaningful.

\echo --- cycle 1 (cold; both relations evicted) ---
SELECT (pg_buffercache_evict_relation('bench_b'::regclass)).*;
SELECT (pg_buffercache_evict_relation(reltoastrelid)).*
FROM pg_class WHERE relname = 'bench_b';

\timing on
SELECT sum(length(j->>'status')) FROM bench_b;
\timing off

SELECT (pg_buffercache_evict_relation('bench_b'::regclass)).*;
SELECT (pg_buffercache_evict_relation(reltoastrelid)).*
FROM pg_class WHERE relname = 'bench_b';

\timing on
SELECT sum(length(jbtl_object_field_text(j, 'status'))) FROM bench_b;
\timing off

\echo --- cycle 2 (warm) ---

\timing on
SELECT sum(length(j->>'status')) FROM bench_b;
\timing off

\timing on
SELECT sum(length(jbtl_object_field_text(j, 'status'))) FROM bench_b;
\timing off

\echo --- cycle 3 (medium-size key, cold) ---
SELECT (pg_buffercache_evict_relation('bench_b'::regclass)).*;
SELECT (pg_buffercache_evict_relation(reltoastrelid)).*
FROM pg_class WHERE relname = 'bench_b';

\timing on
SELECT sum(length(j->>'summary')) FROM bench_b;
\timing off

SELECT (pg_buffercache_evict_relation('bench_b'::regclass)).*;
SELECT (pg_buffercache_evict_relation(reltoastrelid)).*
FROM pg_class WHERE relname = 'bench_b';

\timing on
SELECT sum(length(jbtl_object_field_text(j, 'summary'))) FROM bench_b;
\timing off

\echo --- cycle 4 (large; payload spans body, fast path falls back) ---
SELECT (pg_buffercache_evict_relation('bench_b'::regclass)).*;
SELECT (pg_buffercache_evict_relation(reltoastrelid)).*
FROM pg_class WHERE relname = 'bench_b';

\timing on
SELECT sum(length(j->>'payload')) FROM bench_b;
\timing off

SELECT (pg_buffercache_evict_relation('bench_b'::regclass)).*;
SELECT (pg_buffercache_evict_relation(reltoastrelid)).*
FROM pg_class WHERE relname = 'bench_b';

\timing on
SELECT sum(length(jbtl_object_field_text(j, 'payload'))) FROM bench_b;
\timing off

\echo
\echo --- EXPLAIN BUFFERS for one cold pair (small key) ---
\echo

SELECT (pg_buffercache_evict_relation('bench_b'::regclass)).*;
SELECT (pg_buffercache_evict_relation(reltoastrelid)).*
FROM pg_class WHERE relname = 'bench_b';

EXPLAIN (ANALYZE, BUFFERS, FORMAT text)
SELECT sum(length(j->>'status')) FROM bench_b;

SELECT (pg_buffercache_evict_relation('bench_b'::regclass)).*;
SELECT (pg_buffercache_evict_relation(reltoastrelid)).*
FROM pg_class WHERE relname = 'bench_b';

EXPLAIN (ANALYZE, BUFFERS, FORMAT text)
SELECT sum(length(jbtl_object_field_text(j, 'status'))) FROM bench_b;

DROP TABLE bench_b;
