--
-- jsonb_toaster_lite L1.3b-bench: realistic JSONB shape
--
-- Companion to L13_bench.sql (artificial extremes).  This bench
-- measures three storage layers on a realistic-shape JSONB document
-- with mixed-size top-level fields, on key-level lookup operations
-- that real applications actually run.
--
-- IMPORTANT (read first):
--
--   jsonb_toaster_lite does NOT currently expose a key-level sliced
--   read.  All operator paths (->, ->>, jsonb_path_query, ...) go
--   through core's detoast_attr, which calls our tsr_detoast with
--   offset=0 and length=-1 — i.e., a FULL detoast.  No KVMap-aware
--   fetch, no per-field offset short-circuit.
--
--   The only sliced primitive available is jbtl_slice_probe(j,
--   byte_offset, byte_length): the caller must know byte offsets a
--   priori.  This bench documents the cost of that gap by showing:
--     (a) full-detoast timings for every key access (B and C take
--         the full chunk fetch path);
--     (b) physical byte offsets of selected key values within the
--         JSONB body, computed by a marker-search helper; these are
--         the offsets a hypothetical KVMap-aware fetch would consult;
--     (c) the chunks each key value overlaps in the plain-chunks
--         layout, so the "what we COULD fetch with key-aware support"
--         number is concrete.
--
--   This bench's numbers are not a product story; they are a gap
--   readout.  See README.md.
--
-- K1 KVMap is enabled via SET jsonb_sort_field_values = on.  This
-- changes the on-disk layout so values sort by size, allowing future
-- KVMap-aware fetch logic to use the in-header offset table directly.
--

\set ON_ERROR_STOP on
\pset pager off

\echo
\echo === jsonb_toaster_lite L1.3b-bench (realistic shape) ===
\echo

CREATE EXTENSION IF NOT EXISTS toastapi;
CREATE EXTENSION IF NOT EXISTS jsonb_toaster_lite;

SET jsonb_sort_field_values = on;

-- ---------------------------------------------------------------- setup

CREATE TABLE bench_a (id int PRIMARY KEY, j jsonb);
CREATE TABLE bench_b (id int PRIMARY KEY, j jsonb STORAGE EXTERNAL);
CREATE TABLE bench_c (id int PRIMARY KEY, j jsonb STORAGE EXTERNAL);

SELECT pgpro_toast.set_toaster('jsonb_toaster_lite', 'bench_b', 'j') > 0
       AS attached_b;
SELECT pgpro_toast.set_toaster('jsonb_toaster_lite', 'bench_c', 'j') > 0
       AS attached_c;

-- A realistic-shape document with several small fields, two medium
-- fields, and one large heterogeneous payload.  Each value contains a
-- unique marker substring that lets us locate it in the on-disk body
-- bytes via simple substring search (since the byte content is
-- otherwise heterogeneous).
--
-- Structure:
--   id        small int
--   status    small text
--   kind      small text
--   tenant    small text
--   flags     small array
--   title     medium text (~200 chars)
--   summary   medium text (~1000 chars)
--   payload   large heterogeneous text (~50 KB md5-like, with marker)
--
-- jsonb_sort_field_values=on means physical order is determined by
-- value size (K1 KVMap convention), not key name.

CREATE TEMP TABLE doc AS
SELECT jsonb_build_object(
    'id',       42,
    'status',   'STATUS_marker_active_x9',
    'kind',     'KIND_marker_invoice_x9',
    'tenant',   'TENANT_marker_acme_x9',
    'flags',    jsonb_build_array('flag_paid', 'flag_indexed', 'flag_archived'),
    'title',    'TITLE_marker_x9_'    || repeat('t', 200),
    'summary',  'SUMMARY_marker_x9_'  || repeat('s', 1000),
    'payload',  'PAYLOAD_marker_x9_'  ||
                (SELECT string_agg(md5(g::text), '')
                   FROM generate_series(1, 1500) g))
       AS j;

INSERT INTO bench_a SELECT 1, j FROM doc;
INSERT INTO bench_b SELECT 1, j FROM doc;

SET jsonb_toaster_lite.compress_chunks = on;
INSERT INTO bench_c SELECT 1, j FROM doc;
SET jsonb_toaster_lite.compress_chunks = off;

VACUUM ANALYZE bench_a, bench_b, bench_c;

-- ---------------------------------------------------------------- helper

-- Bench helper: median ms over 5 trials of N executions of an
-- arbitrary single-row SQL.  EXECUTE INTO discards the result.
CREATE OR REPLACE FUNCTION bench_query(
    p_label text, p_sql text,
    p_iters int DEFAULT 200, p_trials int DEFAULT 5,
    OUT op text, OUT median_ms numeric, OUT min_ms numeric,
    OUT max_ms numeric, OUT iters int
) RETURNS record
LANGUAGE plpgsql AS $body$
DECLARE
    samples numeric[] := '{}';
    t0 timestamptz; t1 timestamptz;
    i int; k int;
    r record;
BEGIN
    EXECUTE p_sql INTO r;        -- warmup
    FOR i IN 1..p_trials LOOP
        t0 := clock_timestamp();
        FOR k IN 1..p_iters LOOP
            EXECUTE p_sql INTO r;
        END LOOP;
        t1 := clock_timestamp();
        samples := array_append(samples,
            EXTRACT(epoch FROM (t1 - t0)) * 1000.0);
    END LOOP;
    op := p_label;
    iters := p_iters;
    SELECT round(percentile_cont(0.5) WITHIN GROUP (ORDER BY s)::numeric, 3),
           round(min(s)::numeric, 3),
           round(max(s)::numeric, 3)
      INTO median_ms, min_ms, max_ms FROM unnest(samples) s;
END;
$body$;

-- --------------------------------------------------------- timing matrix

CREATE TEMP TABLE bench_results (
    op text, median_ms numeric, min_ms numeric,
    max_ms numeric, iters int);

-- Operations, per variant:
--   full         : SELECT j FROM ...                  (full read; ::text suppresses cache effects)
--   key_small    : SELECT j->>'status' FROM ...       (small text field)
--   key_medium   : SELECT j->>'summary' FROM ...      (medium text field, ~1 KB)
--   key_large    : SELECT j->>'payload' FROM ...      (large field, ~50 KB)
--   byte_slice   : SELECT jbtl_slice_probe(j, 0, 100) (byte-level prefix; B/C only meaningful)

INSERT INTO bench_results
SELECT * FROM bench_query('A_full',        $sql$SELECT j::text FROM bench_a WHERE id = 1$sql$);
INSERT INTO bench_results
SELECT * FROM bench_query('A_key_small',   $sql$SELECT j->>'status' FROM bench_a WHERE id = 1$sql$);
INSERT INTO bench_results
SELECT * FROM bench_query('A_key_medium',  $sql$SELECT j->>'summary' FROM bench_a WHERE id = 1$sql$);
INSERT INTO bench_results
SELECT * FROM bench_query('A_key_large',   $sql$SELECT j->>'payload' FROM bench_a WHERE id = 1$sql$);
INSERT INTO bench_results
SELECT * FROM bench_query('A_byte_slice',  $sql$SELECT (jbtl_slice_probe(j, 0, 100)).slice_bytes FROM bench_a WHERE id = 1$sql$);

INSERT INTO bench_results
SELECT * FROM bench_query('B_full',        $sql$SELECT j::text FROM bench_b WHERE id = 1$sql$);
INSERT INTO bench_results
SELECT * FROM bench_query('B_key_small',   $sql$SELECT j->>'status' FROM bench_b WHERE id = 1$sql$);
INSERT INTO bench_results
SELECT * FROM bench_query('B_key_medium',  $sql$SELECT j->>'summary' FROM bench_b WHERE id = 1$sql$);
INSERT INTO bench_results
SELECT * FROM bench_query('B_key_large',   $sql$SELECT j->>'payload' FROM bench_b WHERE id = 1$sql$);
INSERT INTO bench_results
SELECT * FROM bench_query('B_byte_slice',  $sql$SELECT (jbtl_slice_probe(j, 0, 100)).slice_bytes FROM bench_b WHERE id = 1$sql$);
-- L1.4: KVMap-aware key fast path (jbtl_object_field_text).
INSERT INTO bench_results
SELECT * FROM bench_query('B_kv_small',    $sql$SELECT jbtl_object_field_text(j, 'status') FROM bench_b WHERE id = 1$sql$);
INSERT INTO bench_results
SELECT * FROM bench_query('B_kv_medium',   $sql$SELECT jbtl_object_field_text(j, 'summary') FROM bench_b WHERE id = 1$sql$);
INSERT INTO bench_results
SELECT * FROM bench_query('B_kv_large',    $sql$SELECT jbtl_object_field_text(j, 'payload') FROM bench_b WHERE id = 1$sql$);

INSERT INTO bench_results
SELECT * FROM bench_query('C_full',        $sql$SELECT j::text FROM bench_c WHERE id = 1$sql$);
INSERT INTO bench_results
SELECT * FROM bench_query('C_key_small',   $sql$SELECT j->>'status' FROM bench_c WHERE id = 1$sql$);
INSERT INTO bench_results
SELECT * FROM bench_query('C_key_medium',  $sql$SELECT j->>'summary' FROM bench_c WHERE id = 1$sql$);
INSERT INTO bench_results
SELECT * FROM bench_query('C_key_large',   $sql$SELECT j->>'payload' FROM bench_c WHERE id = 1$sql$);
INSERT INTO bench_results
SELECT * FROM bench_query('C_byte_slice',  $sql$SELECT (jbtl_slice_probe(j, 0, 100)).slice_bytes FROM bench_c WHERE id = 1$sql$);
-- L1.4: KVMap-aware key fast path on compressed-chunks variant.
INSERT INTO bench_results
SELECT * FROM bench_query('C_kv_small',    $sql$SELECT jbtl_object_field_text(j, 'status') FROM bench_c WHERE id = 1$sql$);
INSERT INTO bench_results
SELECT * FROM bench_query('C_kv_medium',   $sql$SELECT jbtl_object_field_text(j, 'summary') FROM bench_c WHERE id = 1$sql$);
INSERT INTO bench_results
SELECT * FROM bench_query('C_kv_large',    $sql$SELECT jbtl_object_field_text(j, 'payload') FROM bench_c WHERE id = 1$sql$);

\echo
\echo === median (ms for 200 ops; min/max of 5 trials) ===
\echo

SELECT op, median_ms, min_ms, max_ms FROM bench_results ORDER BY op;

-- -------------------------------------------------------- physical metrics

CREATE TEMP TABLE phys_results (
    variant text, column_size int, toast_rel_bytes bigint,
    chunks_total int, chunks_compressed int);

INSERT INTO phys_results
SELECT 'A',
       pg_column_size(j),
       (SELECT pg_relation_size(reltoastrelid)
          FROM pg_class WHERE relname = 'bench_a'),
       NULL, NULL
FROM bench_a WHERE id = 1;

INSERT INTO phys_results
SELECT 'B',
       pg_column_size(b.j),
       (SELECT pg_relation_size(reltoastrelid)
          FROM pg_class WHERE relname = 'bench_b'),
       (SELECT count(*)::int FROM jbtl_chunk_inspect(b.j)),
       (SELECT coalesce(sum(is_compressed::int), 0)::int
          FROM jbtl_chunk_inspect(b.j))
FROM bench_b b WHERE id = 1;

INSERT INTO phys_results
SELECT 'C',
       pg_column_size(c.j),
       (SELECT pg_relation_size(reltoastrelid)
          FROM pg_class WHERE relname = 'bench_c'),
       (SELECT count(*)::int FROM jbtl_chunk_inspect(c.j)),
       (SELECT coalesce(sum(is_compressed::int), 0)::int
          FROM jbtl_chunk_inspect(c.j))
FROM bench_c c WHERE id = 1;

\echo
\echo === physical metrics ===
\echo

SELECT * FROM phys_results ORDER BY variant;

-- ----------------------------- key physical layout (debug helper output)

-- For each labeled key, compute:
--   marker_offset     byte offset where the key's value content starts
--                     in the on-disk uncompressed body (zero-based).
--                     Located by simple substring search on the unique
--                     marker each value contains.
--   value_size        octet_length of the key's value as text.
--   chunks_overlap    list of plain-chunk numbers (variant B) whose
--                     [first_byte..last_byte] range overlaps the
--                     value's [marker_offset..marker_offset+value_size).
--                     This is what a KVMap-aware fetcher COULD read
--                     instead of all chunks.
--
-- Computed against bench_b's on-disk layout (plain chunks).  bench_a
-- has no chunk attribution available (default toast); bench_c's
-- per-chunk-compressed layout has the same logical byte boundaries
-- as B (the writer uses the same input_cap), so attribution computed
-- on B applies to C as well.

CREATE OR REPLACE FUNCTION key_layout(p_marker text, p_key text)
RETURNS TABLE (key text, marker_offset int, value_size int,
               chunks_overlap int[])
LANGUAGE plpgsql AS $body$
DECLARE
    body bytea;
    pos int;
    sz  int;
    val_end int;
BEGIN
    body := (jbtl_slice_probe((SELECT j FROM bench_b WHERE id = 1),
                              0, 1000000)).slice_bytes;
    pos := position(p_marker::bytea in body) - 1;   /* 0-based */
    sz  := octet_length((SELECT j FROM bench_b WHERE id = 1)->>p_key);
    val_end := pos + sz - 1;

    key            := p_key;
    marker_offset  := pos;
    value_size     := sz;
    SELECT array_agg(chunk_no ORDER BY chunk_no)
      INTO chunks_overlap
      FROM jbtl_chunk_inspect((SELECT j FROM bench_b WHERE id = 1))
      WHERE raw_offset <= val_end
        AND raw_offset + raw_size - 1 >= pos;
    RETURN NEXT;
END;
$body$;

\echo
\echo === key physical layout (computed on plain-chunks variant B) ===
\echo

SELECT * FROM key_layout('STATUS_marker_active_x9',  'status');
SELECT * FROM key_layout('KIND_marker_invoice_x9',   'kind');
SELECT * FROM key_layout('TENANT_marker_acme_x9',    'tenant');
SELECT * FROM key_layout('TITLE_marker_x9_',         'title');
SELECT * FROM key_layout('SUMMARY_marker_x9_',       'summary');
SELECT * FROM key_layout('PAYLOAD_marker_x9_',       'payload');

-- ------------------------------------------ chunks_fetched on each op (B/C)

\echo
\echo === chunks_fetched per op (B/C only; A is fall-through detoast) ===
\echo

-- For full read on B/C:
SELECT 'B_full' AS op,
       (jbtl_slice_probe(j, 0, 1000000)).chunks_total,
       (jbtl_slice_probe(j, 0, 1000000)).chunks_fetched
FROM bench_b WHERE id = 1
UNION ALL
SELECT 'B_byte_slice [0,100)',
       (jbtl_slice_probe(j, 0, 100)).chunks_total,
       (jbtl_slice_probe(j, 0, 100)).chunks_fetched
FROM bench_b WHERE id = 1
UNION ALL
SELECT 'C_full',
       (jbtl_slice_probe(j, 0, 1000000)).chunks_total,
       (jbtl_slice_probe(j, 0, 1000000)).chunks_fetched
FROM bench_c WHERE id = 1
UNION ALL
SELECT 'C_byte_slice [0,100)',
       (jbtl_slice_probe(j, 0, 100)).chunks_total,
       (jbtl_slice_probe(j, 0, 100)).chunks_fetched
FROM bench_c WHERE id = 1;

\echo
\echo === L1.4 KVMap-aware key fetch counters (B variant) ===
\echo

-- Per-key chunks_fetched on the KVMap-aware path.  fallback=true rows
-- are the cases where the fast path declined (nested value, value
-- spans >50% of body) and the operator-side full-detoast number
-- applies instead.
SELECT key,
       (p).chunks_total,
       (p).chunks_fetched,
       (p).value_byte_offset,
       (p).value_byte_length,
       (p).fallback
FROM bench_b, (VALUES ('status'), ('kind'), ('tenant'), ('title'),
                      ('summary'), ('payload'), ('flags')) AS k(key),
     LATERAL (SELECT jbtl_object_field_probe(j, key) AS p) sub
WHERE id = 1
ORDER BY key;

\echo
\echo NOTE: L1.4 added jbtl_object_field_text() with a KVMap-aware
\echo sliced fetch.  Compare B_kv_small/medium/large vs B_key_*
\echo (j->>) above.  fast-path keys should show ~3 chunks_fetched
\echo for status/kind/etc.; payload (large value) and flags (nested
\echo value) take fallback paths.
\echo

-- --------------------------------------------------------------- cleanup

DROP TABLE bench_a, bench_b, bench_c;
DROP TABLE doc;
DROP FUNCTION bench_query(text, text, int, int);
DROP FUNCTION key_layout(text, text);

\echo === done ===
