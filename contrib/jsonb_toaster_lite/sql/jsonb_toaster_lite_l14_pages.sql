--
-- jsonb_toaster_lite_l14_pages
--
-- L14 page-level metric pass: pin the page-locality story for the
-- L14 headline payload (key1/key2 huge array/key3/key4 small array,
-- i=100, ~386 KB).
--
-- Why this regression exists:
--   chunks_fetched can overstate physical I/O savings because TOAST
--   chunks are ~2 KB and PostgreSQL pages are 8 KB, so up to 4
--   chunk rows live on one heap page.  The honest physical-I/O
--   metric is `count(distinct toast_blockno)`.  This file pins
--   that metric for the headline shape so any future writer
--   change (chunk stride, fillfactor variant, KVMap layout drift)
--   that breaks page-locality will surface here, not in the
--   L14 evidence package on the next bench run.
--
-- The exact byte counts and chunks_total derive from the L14
-- generator in @yoda's regression series, which is deterministic.
--

\set ON_ERROR_STOP on
\pset null '<NULL>'

CREATE EXTENSION IF NOT EXISTS toastapi;
CREATE EXTENSION IF NOT EXISTS jsonb_toaster_lite;

SET jsonb_sort_field_values = on;
SET jsonb_toaster_lite.compress_chunks = off;

CREATE TABLE l14p (id int PRIMARY KEY, jb jsonb STORAGE EXTERNAL);
SELECT pgpro_toast.set_toaster('jsonb_toaster_lite', 'l14p', 'jb') > 0
       AS attached;

INSERT INTO l14p SELECT 100, jsonb_build_object(
    'key1', 100,
    'key2', (SELECT jsonb_agg(md5((100*1000 + s)::text))
               FROM generate_series(1, pow(10, 1 + 3.0)::int) s),
    'key3', 100,
    'key4', (SELECT jsonb_agg(md5((100*9999 + s)::text))
               FROM generate_series(1, pow(10, 0 + 3.0)::int) s)
);
VACUUM ANALYZE l14p;

-- ============================================================
-- Q1: page fan-out from jbtl_chunk_inspect
--     Pin: 4 chunks per page is the writer's current behaviour
--     (TOAST_MAX_CHUNK_SIZE is 1996, header is small, 4 fit in 8KB).
--     Any future change to per-page packing fires here.
-- ============================================================

SELECT 'q1_page_fanout' AS what,
       (SELECT count(*) FROM (
            SELECT toast_blockno, count(*) AS chunks_per_page
            FROM l14p, jbtl_chunk_inspect(jb)
            GROUP BY toast_blockno
        ) t WHERE chunks_per_page = 4)         AS pages_with_4_chunks_min,
       (SELECT max(chunks_per_page) FROM (
            SELECT count(*) AS chunks_per_page
            FROM l14p, jbtl_chunk_inspect(jb)
            GROUP BY toast_blockno
        ) t)                                   AS max_chunks_on_any_page;

-- ============================================================
-- Q2: total counts
--     Pin: 199 chunks across 50 distinct pages.
-- ============================================================

SELECT 'q2_totals' AS what,
       count(DISTINCT toast_blockno) = 50 AS pin_50_pages,
       count(*)                       = 199 AS pin_199_chunks
FROM l14p, jbtl_chunk_inspect(jb);

-- ============================================================
-- Q3: full read via slice_probe touches all chunks AND all pages
-- ============================================================

WITH p AS (
    SELECT (jbtl_slice_probe(jb, 0, length(jb::text))).*
    FROM l14p
)
SELECT 'q3_full_read' AS what,
       chunks_total          = 199 AS pin_chunks_total,
       chunks_fetched        = 199 AS pin_chunks_fetched,
       toast_pages_touched   = 50  AS pin_50_pages,
       chunks_decompressed   = 0   AS pin_no_decompress,
       bytes_decompressed    = 0   AS pin_no_bytes
FROM p;

-- ============================================================
-- Q4: fast read (key3) touches one chunk and one page
--     This is the honest restatement of the headline:
--     "fast path touches one TOAST page instead of many."
-- ============================================================

WITH p AS (
    SELECT (jbtl_object_field_probe(jb, 'key3')).*
    FROM l14p
)
SELECT 'q4_fast_read_key3' AS what,
       chunks_total          = 199 AS pin_chunks_total,
       chunks_fetched        = 1   AS pin_one_chunk,
       value_byte_offset     = 64  AS pin_offset_64,
       value_byte_length     = 8   AS pin_length_8,
       toast_pages_touched   = 1   AS pin_one_page,
       chunks_decompressed   = 0   AS pin_no_decompress,
       bytes_decompressed    = 0   AS pin_no_bytes,
       value_text            = '100' AS pin_value_100,
       NOT fallback                AS pin_no_fallback
FROM p;

-- ============================================================
-- Q5: ratios (chunks vs pages)
--     Both should show big savings, but the page ratio is the
--     honest one.  We do not pin exact ratios because the absolute
--     numbers (50 pages) can shift slightly under different page
--     fill behaviour; we pin "fast read uses strictly less than
--     full read" on both axes.
-- ============================================================

WITH full_read AS (
    SELECT (jbtl_slice_probe(jb, 0, length(jb::text))).* FROM l14p
),
fast_read AS (
    SELECT (jbtl_object_field_probe(jb, 'key3')).* FROM l14p
)
SELECT 'q5_ratios' AS what,
       fast_read.chunks_fetched      < full_read.chunks_fetched      AS pin_fast_fewer_chunks,
       fast_read.toast_pages_touched < full_read.toast_pages_touched AS pin_fast_fewer_pages,
       full_read.chunks_fetched      = 199                            AS pin_full_chunks_199,
       full_read.toast_pages_touched = 50                             AS pin_full_pages_50,
       fast_read.chunks_fetched      = 1                              AS pin_fast_chunks_1,
       fast_read.toast_pages_touched = 1                              AS pin_fast_pages_1
FROM full_read, fast_read;

-- ============================================================
-- Q6: per-key probe sanity, check for key1/key2/key4 too.
--     key1, key3 are scalars in the fast-path universe.
--     key2, key4 are big arrays -> fallback=true.
-- ============================================================

SELECT 'q6_per_key' AS what,
       key,
       (p).chunks_fetched,
       (p).toast_pages_touched,
       (p).fallback
FROM l14p,
     (VALUES ('key1'), ('key2'), ('key3'), ('key4')) AS k(key),
     LATERAL (SELECT jbtl_object_field_probe(jb, key) AS p) sub
ORDER BY key;

DROP TABLE l14p;
