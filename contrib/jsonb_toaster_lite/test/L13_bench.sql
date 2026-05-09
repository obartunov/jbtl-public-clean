--
-- jsonb_toaster_lite L1.3-bench
--
-- Small physics/perf snapshot before adding update logic.  Compares
-- three storage layers on two payloads and four read operations.
--
-- Variants:
--   A. default PostgreSQL toaster (jsonb default storage = EXTENDED).
--      Core's inline-pglz step compresses the whole jsonb into the
--      heap row when small enough, otherwise spills to TOAST as plain
--      chunks.
--   B. jsonb_toaster_lite plain chunks
--      (STORAGE EXTERNAL forces toast; compress_chunks=off at INSERT
--      time so JBTL_POINTER mode is emitted)
--   C. jsonb_toaster_lite per-chunk pglz compressed
--      (STORAGE EXTERNAL; compress_chunks=on at INSERT time so
--      JBTL_POINTER_COMPRESSED_CHUNKS is emitted)
--
-- Payloads:
--   id=1 compressible   : repeat('A', 60000), pglz-friendly
--   id=2 incompressible : md5(g) for g in 1..2000 string-aggregated,
--                         ~64 KB pglz-hostile
--
-- Operations (per variant × payload):
--   full         : jbtl_slice_probe(j, 0, 1_000_000)        -- whole body
--   slice_head   : jbtl_slice_probe(j, 0, 100)              -- first 100 bytes
--   slice_middle : jbtl_slice_probe(j, 30000, 100)          -- mid-payload 100 bytes
--   slice_tail   : jbtl_slice_probe(j, 59000, 100)          -- last 100 bytes
--
-- jbtl_slice_probe falls through to detoast_attr+memory-slice for
-- variant A (no custom-pointer wrap), so its timings approximate what
-- core spends to satisfy a slice request through the default toaster
-- (no chunk-level skipping).  For B/C the probe walks our custom-
-- pointer fetcher and reads only the chunks overlapping the slice.
--
-- Metrics:
--   median_ms over 5 trials of 200 probe calls each (with 1 warmup)
--   chunks_total, chunks_fetched (reported by probe; meaningful for
--                                  B/C; A reports 0 because the
--                                  fall-through path bypasses our
--                                  toast scan)
--   pg_column_size                  : column footprint in heap row
--   toast relation size             : pg_relation_size(reltoastrelid)
--   chunks_compressed (B/C)         : per-row VARATT_IS_COMPRESSED count
--                                     from jbtl_chunk_inspect
--

\set ON_ERROR_STOP on
\pset pager off

\echo
\echo === jsonb_toaster_lite L1.3-bench ===
\echo

CREATE EXTENSION IF NOT EXISTS toastapi;
CREATE EXTENSION IF NOT EXISTS jsonb_toaster_lite;

-- ---------------------------------------------------------------- setup

CREATE TABLE bench_a (id int PRIMARY KEY, j jsonb);
CREATE TABLE bench_b (id int PRIMARY KEY, j jsonb STORAGE EXTERNAL);
CREATE TABLE bench_c (id int PRIMARY KEY, j jsonb STORAGE EXTERNAL);

SELECT pgpro_toast.set_toaster('jsonb_toaster_lite', 'bench_b', 'j') > 0
       AS attached_b;
SELECT pgpro_toast.set_toaster('jsonb_toaster_lite', 'bench_c', 'j') > 0
       AS attached_c;

-- A: default toaster (no custom toaster set)
SET jsonb_toaster_lite.compress_chunks = off;
INSERT INTO bench_a VALUES (1,
    jsonb_build_object('payload', repeat('A', 60000)));
INSERT INTO bench_a SELECT 2, jsonb_build_object('payload',
    (SELECT string_agg(md5(g::text), '')
       FROM generate_series(1, 2000) g));

-- B: jsonb_toaster_lite plain
INSERT INTO bench_b VALUES (1,
    jsonb_build_object('payload', repeat('A', 60000)));
INSERT INTO bench_b SELECT 2, jsonb_build_object('payload',
    (SELECT string_agg(md5(g::text), '')
       FROM generate_series(1, 2000) g));

-- C: jsonb_toaster_lite compressed
SET jsonb_toaster_lite.compress_chunks = on;
INSERT INTO bench_c VALUES (1,
    jsonb_build_object('payload', repeat('A', 60000)));
INSERT INTO bench_c SELECT 2, jsonb_build_object('payload',
    (SELECT string_agg(md5(g::text), '')
       FROM generate_series(1, 2000) g));
SET jsonb_toaster_lite.compress_chunks = off;

VACUUM ANALYZE bench_a, bench_b, bench_c;

-- ---------------------------------------------------------------- helper

CREATE OR REPLACE FUNCTION bench_probe(
    p_label  text,
    p_tbl    regclass,
    p_id     int,
    p_off    int,
    p_len    int,
    p_iters  int DEFAULT 200,
    p_trials int DEFAULT 5,
    OUT op   text,
    OUT median_ms numeric,
    OUT min_ms    numeric,
    OUT max_ms    numeric,
    OUT iters     int
) RETURNS record
LANGUAGE plpgsql AS $body$
DECLARE
    samples numeric[] := '{}';
    t0      timestamptz;
    t1      timestamptz;
    i       int;
    k       int;
    r       record;
    qry     text;
BEGIN
    qry := format(
        'SELECT jbtl_slice_probe(j, %s, %s) FROM %s WHERE id = %s',
        p_off, p_len, p_tbl::text, p_id);

    /* warmup */
    EXECUTE qry INTO r;

    FOR i IN 1..p_trials LOOP
        t0 := clock_timestamp();
        FOR k IN 1..p_iters LOOP
            EXECUTE qry INTO r;
        END LOOP;
        t1 := clock_timestamp();
        samples := array_append(samples,
            EXTRACT(epoch FROM (t1 - t0)) * 1000.0);
    END LOOP;

    op    := p_label;
    iters := p_iters;
    SELECT round(percentile_cont(0.5) WITHIN GROUP (ORDER BY s)::numeric, 3),
           round(min(s)::numeric, 3),
           round(max(s)::numeric, 3)
      INTO median_ms, min_ms, max_ms
      FROM unnest(samples) s;
END;
$body$;

-- --------------------------------------------------------- timing matrix

CREATE TEMP TABLE bench_results (
    op        text,
    median_ms numeric,
    min_ms    numeric,
    max_ms    numeric,
    iters     int);

INSERT INTO bench_results SELECT * FROM bench_probe('A_compr_full',          'bench_a', 1, 0,     1000000);
INSERT INTO bench_results SELECT * FROM bench_probe('A_compr_slice_head',    'bench_a', 1, 0,     100);
INSERT INTO bench_results SELECT * FROM bench_probe('A_compr_slice_middle',  'bench_a', 1, 30000, 100);
INSERT INTO bench_results SELECT * FROM bench_probe('A_compr_slice_tail',    'bench_a', 1, 59000, 100);

INSERT INTO bench_results SELECT * FROM bench_probe('A_incompr_full',        'bench_a', 2, 0,     1000000);
INSERT INTO bench_results SELECT * FROM bench_probe('A_incompr_slice_head',  'bench_a', 2, 0,     100);
INSERT INTO bench_results SELECT * FROM bench_probe('A_incompr_slice_middle','bench_a', 2, 30000, 100);
INSERT INTO bench_results SELECT * FROM bench_probe('A_incompr_slice_tail',  'bench_a', 2, 59000, 100);

INSERT INTO bench_results SELECT * FROM bench_probe('B_compr_full',          'bench_b', 1, 0,     1000000);
INSERT INTO bench_results SELECT * FROM bench_probe('B_compr_slice_head',    'bench_b', 1, 0,     100);
INSERT INTO bench_results SELECT * FROM bench_probe('B_compr_slice_middle',  'bench_b', 1, 30000, 100);
INSERT INTO bench_results SELECT * FROM bench_probe('B_compr_slice_tail',    'bench_b', 1, 59000, 100);

INSERT INTO bench_results SELECT * FROM bench_probe('B_incompr_full',        'bench_b', 2, 0,     1000000);
INSERT INTO bench_results SELECT * FROM bench_probe('B_incompr_slice_head',  'bench_b', 2, 0,     100);
INSERT INTO bench_results SELECT * FROM bench_probe('B_incompr_slice_middle','bench_b', 2, 30000, 100);
INSERT INTO bench_results SELECT * FROM bench_probe('B_incompr_slice_tail',  'bench_b', 2, 59000, 100);

INSERT INTO bench_results SELECT * FROM bench_probe('C_compr_full',          'bench_c', 1, 0,     1000000);
INSERT INTO bench_results SELECT * FROM bench_probe('C_compr_slice_head',    'bench_c', 1, 0,     100);
INSERT INTO bench_results SELECT * FROM bench_probe('C_compr_slice_middle',  'bench_c', 1, 30000, 100);
INSERT INTO bench_results SELECT * FROM bench_probe('C_compr_slice_tail',    'bench_c', 1, 59000, 100);

INSERT INTO bench_results SELECT * FROM bench_probe('C_incompr_full',        'bench_c', 2, 0,     1000000);
INSERT INTO bench_results SELECT * FROM bench_probe('C_incompr_slice_head',  'bench_c', 2, 0,     100);
INSERT INTO bench_results SELECT * FROM bench_probe('C_incompr_slice_middle','bench_c', 2, 30000, 100);
INSERT INTO bench_results SELECT * FROM bench_probe('C_incompr_slice_tail',  'bench_c', 2, 59000, 100);

\echo
\echo === median (ms for 200 probe calls; min/max of 5 trials) ===
\echo

SELECT op, median_ms, min_ms, max_ms FROM bench_results ORDER BY op;

-- -------------------------------------------------------- physical metrics

CREATE TEMP TABLE phys_results (
    variant            text,
    payload            text,
    column_size        int,
    toast_rel_bytes    bigint,
    chunks_total       int,
    chunks_compressed  int,
    total_rel_pretty   text);

-- A: default toaster
INSERT INTO phys_results
SELECT 'A', 'compressible',
       pg_column_size(j),
       (SELECT pg_relation_size(reltoastrelid)
          FROM pg_class WHERE relname = 'bench_a'),
       NULL, NULL,
       pg_size_pretty(pg_total_relation_size('bench_a'::regclass))
FROM bench_a WHERE id = 1;

INSERT INTO phys_results
SELECT 'A', 'incompressible',
       pg_column_size(j),
       (SELECT pg_relation_size(reltoastrelid)
          FROM pg_class WHERE relname = 'bench_a'),
       NULL, NULL,
       pg_size_pretty(pg_total_relation_size('bench_a'::regclass))
FROM bench_a WHERE id = 2;

-- B: plain chunks
INSERT INTO phys_results
SELECT 'B', 'compressible',
       pg_column_size(b.j),
       (SELECT pg_relation_size(reltoastrelid)
          FROM pg_class WHERE relname = 'bench_b'),
       (SELECT count(*)::int
          FROM jbtl_chunk_inspect(b.j)),
       (SELECT coalesce(sum(is_compressed::int), 0)::int
          FROM jbtl_chunk_inspect(b.j)),
       pg_size_pretty(pg_total_relation_size('bench_b'::regclass))
FROM bench_b b WHERE id = 1;

INSERT INTO phys_results
SELECT 'B', 'incompressible',
       pg_column_size(b.j),
       (SELECT pg_relation_size(reltoastrelid)
          FROM pg_class WHERE relname = 'bench_b'),
       (SELECT count(*)::int
          FROM jbtl_chunk_inspect(b.j)),
       (SELECT coalesce(sum(is_compressed::int), 0)::int
          FROM jbtl_chunk_inspect(b.j)),
       pg_size_pretty(pg_total_relation_size('bench_b'::regclass))
FROM bench_b b WHERE id = 2;

-- C: per-chunk pglz
INSERT INTO phys_results
SELECT 'C', 'compressible',
       pg_column_size(c.j),
       (SELECT pg_relation_size(reltoastrelid)
          FROM pg_class WHERE relname = 'bench_c'),
       (SELECT count(*)::int
          FROM jbtl_chunk_inspect(c.j)),
       (SELECT coalesce(sum(is_compressed::int), 0)::int
          FROM jbtl_chunk_inspect(c.j)),
       pg_size_pretty(pg_total_relation_size('bench_c'::regclass))
FROM bench_c c WHERE id = 1;

INSERT INTO phys_results
SELECT 'C', 'incompressible',
       pg_column_size(c.j),
       (SELECT pg_relation_size(reltoastrelid)
          FROM pg_class WHERE relname = 'bench_c'),
       (SELECT count(*)::int
          FROM jbtl_chunk_inspect(c.j)),
       (SELECT coalesce(sum(is_compressed::int), 0)::int
          FROM jbtl_chunk_inspect(c.j)),
       pg_size_pretty(pg_total_relation_size('bench_c'::regclass))
FROM bench_c c WHERE id = 2;

\echo
\echo === physical metrics ===
\echo

SELECT * FROM phys_results ORDER BY variant, payload;

-- ---------------------------------------------- per-op slice probe counters

CREATE TEMP TABLE counter_results (
    op             text,
    chunks_total   int,
    chunks_fetched int,
    slice_size     int);

-- We call jbtl_slice_probe once per op; for variant A the probe falls
-- through to detoast_attr (chunks_total = chunks_fetched = 0).

-- A: compressible
INSERT INTO counter_results
SELECT 'A_compr_full',         (jbtl_slice_probe(j, 0, 1000000)).chunks_total,
                               (jbtl_slice_probe(j, 0, 1000000)).chunks_fetched,
                               octet_length((jbtl_slice_probe(j, 0, 1000000)).slice_bytes)
FROM bench_a WHERE id = 1
UNION ALL
SELECT 'A_compr_slice_head',   (jbtl_slice_probe(j, 0, 100)).chunks_total,
                               (jbtl_slice_probe(j, 0, 100)).chunks_fetched,
                               octet_length((jbtl_slice_probe(j, 0, 100)).slice_bytes)
FROM bench_a WHERE id = 1
UNION ALL
SELECT 'A_compr_slice_middle', (jbtl_slice_probe(j, 30000, 100)).chunks_total,
                               (jbtl_slice_probe(j, 30000, 100)).chunks_fetched,
                               octet_length((jbtl_slice_probe(j, 30000, 100)).slice_bytes)
FROM bench_a WHERE id = 1
UNION ALL
SELECT 'A_compr_slice_tail',   (jbtl_slice_probe(j, 59000, 100)).chunks_total,
                               (jbtl_slice_probe(j, 59000, 100)).chunks_fetched,
                               octet_length((jbtl_slice_probe(j, 59000, 100)).slice_bytes)
FROM bench_a WHERE id = 1;

-- A: incompressible
INSERT INTO counter_results
SELECT 'A_incompr_full',         (jbtl_slice_probe(j, 0, 1000000)).chunks_total,
                                 (jbtl_slice_probe(j, 0, 1000000)).chunks_fetched,
                                 octet_length((jbtl_slice_probe(j, 0, 1000000)).slice_bytes)
FROM bench_a WHERE id = 2
UNION ALL
SELECT 'A_incompr_slice_head',   (jbtl_slice_probe(j, 0, 100)).chunks_total,
                                 (jbtl_slice_probe(j, 0, 100)).chunks_fetched,
                                 octet_length((jbtl_slice_probe(j, 0, 100)).slice_bytes)
FROM bench_a WHERE id = 2
UNION ALL
SELECT 'A_incompr_slice_middle', (jbtl_slice_probe(j, 30000, 100)).chunks_total,
                                 (jbtl_slice_probe(j, 30000, 100)).chunks_fetched,
                                 octet_length((jbtl_slice_probe(j, 30000, 100)).slice_bytes)
FROM bench_a WHERE id = 2
UNION ALL
SELECT 'A_incompr_slice_tail',   (jbtl_slice_probe(j, 59000, 100)).chunks_total,
                                 (jbtl_slice_probe(j, 59000, 100)).chunks_fetched,
                                 octet_length((jbtl_slice_probe(j, 59000, 100)).slice_bytes)
FROM bench_a WHERE id = 2;

-- B
INSERT INTO counter_results
SELECT 'B_compr_full',         (jbtl_slice_probe(j, 0, 1000000)).chunks_total,
                               (jbtl_slice_probe(j, 0, 1000000)).chunks_fetched,
                               octet_length((jbtl_slice_probe(j, 0, 1000000)).slice_bytes)
FROM bench_b WHERE id = 1
UNION ALL
SELECT 'B_compr_slice_head',   (jbtl_slice_probe(j, 0, 100)).chunks_total,
                               (jbtl_slice_probe(j, 0, 100)).chunks_fetched,
                               octet_length((jbtl_slice_probe(j, 0, 100)).slice_bytes)
FROM bench_b WHERE id = 1
UNION ALL
SELECT 'B_compr_slice_middle', (jbtl_slice_probe(j, 30000, 100)).chunks_total,
                               (jbtl_slice_probe(j, 30000, 100)).chunks_fetched,
                               octet_length((jbtl_slice_probe(j, 30000, 100)).slice_bytes)
FROM bench_b WHERE id = 1
UNION ALL
SELECT 'B_compr_slice_tail',   (jbtl_slice_probe(j, 59000, 100)).chunks_total,
                               (jbtl_slice_probe(j, 59000, 100)).chunks_fetched,
                               octet_length((jbtl_slice_probe(j, 59000, 100)).slice_bytes)
FROM bench_b WHERE id = 1;

INSERT INTO counter_results
SELECT 'B_incompr_full',         (jbtl_slice_probe(j, 0, 1000000)).chunks_total,
                                 (jbtl_slice_probe(j, 0, 1000000)).chunks_fetched,
                                 octet_length((jbtl_slice_probe(j, 0, 1000000)).slice_bytes)
FROM bench_b WHERE id = 2
UNION ALL
SELECT 'B_incompr_slice_head',   (jbtl_slice_probe(j, 0, 100)).chunks_total,
                                 (jbtl_slice_probe(j, 0, 100)).chunks_fetched,
                                 octet_length((jbtl_slice_probe(j, 0, 100)).slice_bytes)
FROM bench_b WHERE id = 2
UNION ALL
SELECT 'B_incompr_slice_middle', (jbtl_slice_probe(j, 30000, 100)).chunks_total,
                                 (jbtl_slice_probe(j, 30000, 100)).chunks_fetched,
                                 octet_length((jbtl_slice_probe(j, 30000, 100)).slice_bytes)
FROM bench_b WHERE id = 2
UNION ALL
SELECT 'B_incompr_slice_tail',   (jbtl_slice_probe(j, 59000, 100)).chunks_total,
                                 (jbtl_slice_probe(j, 59000, 100)).chunks_fetched,
                                 octet_length((jbtl_slice_probe(j, 59000, 100)).slice_bytes)
FROM bench_b WHERE id = 2;

-- C
INSERT INTO counter_results
SELECT 'C_compr_full',         (jbtl_slice_probe(j, 0, 1000000)).chunks_total,
                               (jbtl_slice_probe(j, 0, 1000000)).chunks_fetched,
                               octet_length((jbtl_slice_probe(j, 0, 1000000)).slice_bytes)
FROM bench_c WHERE id = 1
UNION ALL
SELECT 'C_compr_slice_head',   (jbtl_slice_probe(j, 0, 100)).chunks_total,
                               (jbtl_slice_probe(j, 0, 100)).chunks_fetched,
                               octet_length((jbtl_slice_probe(j, 0, 100)).slice_bytes)
FROM bench_c WHERE id = 1
UNION ALL
SELECT 'C_compr_slice_middle', (jbtl_slice_probe(j, 30000, 100)).chunks_total,
                               (jbtl_slice_probe(j, 30000, 100)).chunks_fetched,
                               octet_length((jbtl_slice_probe(j, 30000, 100)).slice_bytes)
FROM bench_c WHERE id = 1
UNION ALL
SELECT 'C_compr_slice_tail',   (jbtl_slice_probe(j, 59000, 100)).chunks_total,
                               (jbtl_slice_probe(j, 59000, 100)).chunks_fetched,
                               octet_length((jbtl_slice_probe(j, 59000, 100)).slice_bytes)
FROM bench_c WHERE id = 1;

INSERT INTO counter_results
SELECT 'C_incompr_full',         (jbtl_slice_probe(j, 0, 1000000)).chunks_total,
                                 (jbtl_slice_probe(j, 0, 1000000)).chunks_fetched,
                                 octet_length((jbtl_slice_probe(j, 0, 1000000)).slice_bytes)
FROM bench_c WHERE id = 2
UNION ALL
SELECT 'C_incompr_slice_head',   (jbtl_slice_probe(j, 0, 100)).chunks_total,
                                 (jbtl_slice_probe(j, 0, 100)).chunks_fetched,
                                 octet_length((jbtl_slice_probe(j, 0, 100)).slice_bytes)
FROM bench_c WHERE id = 2
UNION ALL
SELECT 'C_incompr_slice_middle', (jbtl_slice_probe(j, 30000, 100)).chunks_total,
                                 (jbtl_slice_probe(j, 30000, 100)).chunks_fetched,
                                 octet_length((jbtl_slice_probe(j, 30000, 100)).slice_bytes)
FROM bench_c WHERE id = 2
UNION ALL
SELECT 'C_incompr_slice_tail',   (jbtl_slice_probe(j, 59000, 100)).chunks_total,
                                 (jbtl_slice_probe(j, 59000, 100)).chunks_fetched,
                                 octet_length((jbtl_slice_probe(j, 59000, 100)).slice_bytes)
FROM bench_c WHERE id = 2;

\echo
\echo === absolute numbers (illustration only) ===
\echo

SELECT * FROM counter_results ORDER BY op;

-- -------------------------------------------------------------- cleanup

DROP TABLE bench_a, bench_b, bench_c;
DROP FUNCTION bench_probe(text, regclass, int, int, int, int, int);

\echo
\echo === done ===
