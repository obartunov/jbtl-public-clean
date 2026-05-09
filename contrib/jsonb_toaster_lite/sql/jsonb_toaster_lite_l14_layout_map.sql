--
-- L14 layout-map regression
--
-- Pins the physical-locality classification of the 10 fixed L14
-- paths against the headline payload (i=100, ~386 KB, 199 chunks,
-- 50 pages).
--
-- This is a debug helper.  No runtime path is exercised here —
-- jbtl_l14_layout_map walks a fully-detoasted body and only uses
-- jbtl_count_pages_in_chunk_range as a probe-only secondary scan.
--

\set ON_ERROR_STOP on
\pset null '<NULL>'

CREATE EXTENSION IF NOT EXISTS toastapi;
CREATE EXTENSION IF NOT EXISTS jsonb_toaster_lite;

SET jsonb_sort_field_values = on;
SET jsonb_toaster_lite.compress_chunks = off;

CREATE TABLE l14_lm (id int PRIMARY KEY, jb jsonb STORAGE EXTERNAL);
SELECT pgpro_toast.set_toaster('jsonb_toaster_lite', 'l14_lm', 'jb') > 0
       AS attached;

INSERT INTO l14_lm SELECT 100, jsonb_build_object(
    'key1', 100,
    'key2', (SELECT jsonb_agg(md5((100*1000 + s)::text))
               FROM generate_series(1, pow(10, 1 + 3.0)::int) s),
    'key3', 100,
    'key4', (SELECT jsonb_agg(md5((100*9999 + s)::text))
               FROM generate_series(1, pow(10, 0 + 3.0)::int) s)
);
VACUUM ANALYZE l14_lm;

-- ============================================================
-- Layout map: 10 fixed paths
-- ============================================================

SELECT path, kind, byte_offset, byte_length,
       chunk_lo, chunk_hi,
       distinct_pages, parent_metadata_pages,
       round(fragmentation_ratio::numeric, 2) AS frag_ratio,
       class
FROM l14_lm, jbtl_l14_layout_map(jb)
ORDER BY byte_offset;

-- ============================================================
-- Per-class group counts
-- ============================================================

SELECT class, count(*) AS n
FROM l14_lm, jbtl_l14_layout_map(jb)
GROUP BY class
ORDER BY class;

-- ============================================================
-- Acceptance pins
-- ============================================================

-- Pin: top-level small scalars are inline.
SELECT 'pin_inline_top_scalars' AS what,
       count(*) FILTER (WHERE class = 'inline'
                        AND path IN ('key1', 'key3'))    = 2 AS pin_two_inline
FROM l14_lm, jbtl_l14_layout_map(jb);

-- Pin: key2 is a large extent (covers most of the document).
SELECT 'pin_key2_large_extent' AS what,
       (SELECT class FROM l14_lm, jbtl_l14_layout_map(jb)
        WHERE path = 'key2') = 'large_extent' AS pin_large_extent;

-- Pin: array elements at boundary indices are compact byte ranges
-- (each 32-byte md5 string fits in 1 chunk and 1 page) but require
-- multiple metadata pages -> needs_staged.
SELECT 'pin_array_elements_need_staged' AS what,
       count(*) FILTER (WHERE class = 'needs_staged'
                        AND path LIKE 'key2[%]')         = 5 AS pin_five_staged
FROM l14_lm, jbtl_l14_layout_map(jb);

-- Pin: JB_OFFSET_STRIDE doesn't change the page count —
-- key2[31] and key2[32] both resolve to one page each
-- (the offset cache lives entirely in the prefix metadata
-- which we count separately under parent_metadata_pages).
SELECT 'pin_offset_stride_uniform' AS what,
       (SELECT distinct_pages FROM l14_lm, jbtl_l14_layout_map(jb)
        WHERE path = 'key2[31]') = 1                        AS pin_31,
       (SELECT distinct_pages FROM l14_lm, jbtl_l14_layout_map(jb)
        WHERE path = 'key2[32]') = 1                        AS pin_32,
       (SELECT distinct_pages FROM l14_lm, jbtl_l14_layout_map(jb)
        WHERE path = 'key2[5000]') = 1                      AS pin_5000;

-- Pin: parent_metadata_pages for any key2 element is 10
-- (K1 sort places key2 at the front of the data area; its header +
-- 10000 JEntries occupy ~80 KB = ~10 pages).  This is the staging
-- cost — nested array element needs 10 metadata pages + 1 value page.
SELECT 'pin_array_staging_cost' AS what,
       array_agg(DISTINCT parent_metadata_pages
                 ORDER BY parent_metadata_pages) =
           ARRAY[10]                                         AS pin_uniform_10
FROM l14_lm, jbtl_l14_layout_map(jb)
WHERE path LIKE 'key2[%]';

DROP TABLE l14_lm;
