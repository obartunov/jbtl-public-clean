--
-- jsonb_toaster_lite L2.1a — dry-run update probe regression
--
-- Pins jbtl_update_probe behaviour against the L14 headline shape
-- (key1/key2 huge array/key3/key4 small array, i=100, ~386 KB) and
-- the four cases @yoda's L2.1a spec requires:
--
--   1. key3 same-length scalar update (100 -> 99): can_fast_update=t,
--      chunks_rewritten=1
--   2. length-changing update (numeric short string -> longer):
--      fallback
--   3. container key (key2 huge array): fallback
--   4. missing key (no_such_key): fallback
--
-- The probe is a pure inspector — no storage mutation occurs.
--

\set ON_ERROR_STOP on
\pset null '<NULL>'

CREATE EXTENSION IF NOT EXISTS toastapi;
CREATE EXTENSION IF NOT EXISTS jsonb_toaster_lite;

SET jsonb_sort_field_values = on;
SET jsonb_toaster_lite.compress_chunks = off;

-- L14 generator at i=100.
CREATE TABLE l21a_probe (id int PRIMARY KEY, jb jsonb STORAGE EXTERNAL);
SELECT pgpro_toast.set_toaster('jsonb_toaster_lite', 'l21a_probe', 'jb') > 0
       AS attached;

INSERT INTO l21a_probe SELECT 100, jsonb_build_object(
    'key1', 100,
    'key2', (SELECT jsonb_agg(md5((100*1000 + s)::text))
               FROM generate_series(1, pow(10, 1 + 3.0)::int) s),
    'key3', 100,
    'key4', (SELECT jsonb_agg(md5((100*9999 + s)::text))
               FROM generate_series(1, pow(10, 0 + 3.0)::int) s)
);
VACUUM ANALYZE l21a_probe;

-- ============================================================
-- Case 1: key3 100 -> 99 (same-length, same-type, in fast-path)
-- ============================================================

SELECT 'case_1_key3_same_length' AS case_label,
       (p).can_fast_update,
       (p).fallback_reason,
       (p).value_offset,
       (p).old_len,
       (p).new_len,
       (p).chunks_total,
       (p).chunks_rewritten,
       (p).chunks_untouched,
       round(((p).write_fraction * 1000)::numeric, 1) AS write_fraction_per_mille,
       (p).rewrite_amplification
FROM l21a_probe,
     LATERAL (
         SELECT jbtl_update_probe(jb, 'key3',
                                  jsonb_set(jb, '{key3}', '99'::jsonb)) AS p
     ) sub;

-- Pin the structural commitments separately for the canary row.
SELECT 'case_1_pins' AS what,
       (p).can_fast_update                          = true   AS pin_can_fast_update,
       (p).chunks_rewritten                         = 1      AS pin_one_chunk,
       (p).chunks_total                             = 199    AS pin_199_chunks_total,
       (p).old_len                                  = 8      AS pin_old_len_8,
       (p).new_len                                  = 8      AS pin_new_len_8,
       (p).old_len                                  = (p).new_len  AS pin_same_length,
       (p).chunks_untouched + (p).chunks_rewritten  = (p).chunks_total AS pin_partition,
       (p).rewrite_amplification                    = 1      AS pin_amp_1
FROM l21a_probe,
     LATERAL (
         SELECT jbtl_update_probe(jb, 'key3',
                                  jsonb_set(jb, '{key3}', '99'::jsonb)) AS p
     ) sub;

-- ============================================================
-- Case 2: length-changing update (numeric 100 -> 99999)
--
-- jsonb encodes numerics with NDIGITS-aware varlena, so 99999
-- (NDIGITS=2) is 2 bytes longer than 100 (NDIGITS=1).  The probe
-- must refuse with fallback_reason='length-changing update'.
-- ============================================================

SELECT 'case_2_length_changing' AS case_label,
       (p).can_fast_update,
       (p).fallback_reason,
       (p).old_len,
       (p).new_len
FROM l21a_probe,
     LATERAL (
         SELECT jbtl_update_probe(jb, 'key3',
                                  jsonb_set(jb, '{key3}', '99999'::jsonb)) AS p
     ) sub;

-- Pin: must refuse.
SELECT 'case_2_pins' AS what,
       (p).can_fast_update = false          AS pin_refused,
       (p).fallback_reason ~ 'length'       AS pin_reason_mentions_length
FROM l21a_probe,
     LATERAL (
         SELECT jbtl_update_probe(jb, 'key3',
                                  jsonb_set(jb, '{key3}', '99999'::jsonb)) AS p
     ) sub;

-- ============================================================
-- Case 3: container key (key2 huge array)
--
-- The L1.4 fast-path probe declines for key2 (>50% of body); the
-- update probe inherits that decline.
-- ============================================================

SELECT 'case_3_container_key2' AS case_label,
       (p).can_fast_update,
       (p).fallback_reason
FROM l21a_probe,
     LATERAL (
         -- Replace key2 with a tiny array (the probe will not even
         -- look at the new value because it bails before that).
         SELECT jbtl_update_probe(jb, 'key2',
                                  jsonb_set(jb, '{key2}', '[1,2,3]'::jsonb)) AS p
     ) sub;

SELECT 'case_3_pins' AS what,
       (p).can_fast_update                = false        AS pin_refused,
       (p).fallback_reason ~ 'fast-path'  OR
       (p).fallback_reason ~ 'container'  OR
       (p).fallback_reason ~ 'oversized'                 AS pin_reason_recognizable
FROM l21a_probe,
     LATERAL (
         SELECT jbtl_update_probe(jb, 'key2',
                                  jsonb_set(jb, '{key2}', '[1,2,3]'::jsonb)) AS p
     ) sub;

-- ============================================================
-- Case 4: missing key
-- ============================================================

SELECT 'case_4_missing_key' AS case_label,
       (p).can_fast_update,
       (p).fallback_reason
FROM l21a_probe,
     LATERAL (
         -- Pass any new_value document; the probe should refuse
         -- before consulting it because the key is absent in the
         -- old document.
         SELECT jbtl_update_probe(jb, 'no_such_key',
                                  '{"no_such_key": 42}'::jsonb) AS p
     ) sub;

SELECT 'case_4_pins' AS what,
       (p).can_fast_update                       = false  AS pin_refused,
       (p).fallback_reason ~ 'missing'                    AS pin_reason_missing
FROM l21a_probe,
     LATERAL (
         SELECT jbtl_update_probe(jb, 'no_such_key',
                                  '{"no_such_key": 42}'::jsonb) AS p
     ) sub;

-- ============================================================
-- Case 5: storage was not mutated (the dry-run contract)
--
-- Any of the four probes above must leave the document byte-
-- identical to what was inserted.  Read it back and compare.
-- ============================================================

SELECT 'case_5_no_mutation' AS what,
       (jb -> 'key1')::text = '100'                          AS pin_key1,
       jsonb_typeof(jb -> 'key2') = 'array'                  AS pin_key2_array,
       jsonb_array_length(jb -> 'key2') = 10000              AS pin_key2_len_10000,
       (jb -> 'key3')::text = '100'                          AS pin_key3_unchanged,
       jsonb_typeof(jb -> 'key4') = 'array'                  AS pin_key4_array,
       jsonb_array_length(jb -> 'key4') = 1000               AS pin_key4_len_1000
FROM l21a_probe;

DROP TABLE l21a_probe;
