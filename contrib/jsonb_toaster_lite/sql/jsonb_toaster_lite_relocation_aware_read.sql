--
-- jsonb_toaster_lite_relocation_aware_read
--
-- Deterministic correctness contract for the relocation-aware fast
-- path that the jsonb_object_field hook takes on
-- JBTL_POINTER_SUBTREE wrappers. Locks in the dispatch and value
-- shapes against future regressions.
--
-- The benchmark suite (jsonb_p1_bench_suite_v0.9) exercises
-- performance shape only; this test exercises edge cases that the
-- benchmark does not reach.
--
-- Naming policy: the test file uses "relocation" terminology in its
-- comments and identifiers. The legacy "subtree" name appears only
-- when citing exact existing symbols (the GUC
-- jsonb_toaster_lite.enable_subtree_storage and the
-- jbtl_subtree_refs catalog).
--

CREATE EXTENSION IF NOT EXISTS toastapi;
CREATE EXTENSION IF NOT EXISTS jsonb_toaster_lite;

-- Test setup: every test creates its own table to keep state isolated.
SET jsonb_toaster_lite.enable_subtree_storage = on;
SET jsonb_sort_field_values = off;

--
-- Section 1: basic 4-key dataset (mimics the J01 benchmark cell).
-- Two large arrays (relocated) + two small scalars (inline).
--

CREATE TABLE r1_basic (id int PRIMARY KEY, jb jsonb);
SELECT pgpro_toast.set_toaster('jsonb_toaster_lite', 'public.r1_basic', 'jb');

INSERT INTO r1_basic VALUES (1, jsonb_build_object(
    'key1', 42,
    'key2', (SELECT jsonb_agg(g) FROM generate_series(1, 10000) g),
    'key3', 99,
    'key4', (SELECT jsonb_agg(g) FROM generate_series(1, 5000) g)
));

-- Confirm the wrapper engaged.
SELECT pg_column_size(jb) < 1000 AS wrapper_small,  -- inline parent body only
       (SELECT count(*) FROM jbtl_subtree_refs
        WHERE parent_toastrelid =
              (SELECT reltoastrelid FROM pg_class WHERE relname='r1_basic')) AS refs
FROM r1_basic WHERE id = 1;

-- Inline scalars: no fragment fetch required.
SELECT jb -> 'key1' AS key1_value FROM r1_basic WHERE id = 1;
SELECT jb -> 'key3' AS key3_value FROM r1_basic WHERE id = 1;

-- Out-of-line values: fetched as one fragment each.
SELECT jsonb_array_length(jb -> 'key2') AS key2_len FROM r1_basic WHERE id = 1;
SELECT jsonb_array_length(jb -> 'key4') AS key4_len FROM r1_basic WHERE id = 1;

-- Sample values inside the out-of-line arrays.
SELECT (jb -> 'key2') -> 0 AS first_of_key2 FROM r1_basic WHERE id = 1;
SELECT (jb -> 'key2') -> 9999 AS last_of_key2 FROM r1_basic WHERE id = 1;

-- Missing key: no fragment fetch, returns SQL NULL.
SELECT jb -> 'absent' AS absent_key FROM r1_basic WHERE id = 1;
SELECT (jb -> 'absent') IS NULL AS absent_is_null FROM r1_basic WHERE id = 1;

DROP TABLE r1_basic;

--
-- Section 2: heterogeneous inline value types alongside relocated values.
-- All inline JEntry kinds must produce the correct JsonbValue.
--

CREATE TABLE r1_types (id int PRIMARY KEY, jb jsonb);
SELECT pgpro_toast.set_toaster('jsonb_toaster_lite', 'public.r1_types', 'jb');

-- A single row that mixes:
--   inline scalars of all kinds (null, string, numeric, bool, container)
--   one large out-of-line value to force relocation engagement
INSERT INTO r1_types VALUES (1, jsonb_build_object(
    'a_null',     NULL::jsonb,
    'a_bool_t',   true,
    'a_bool_f',   false,
    'a_string',   'hello world',
    'a_emptystr', '',
    'a_int_pos',  12345,
    'a_int_neg',  -98765,
    'a_int_big',  1234567890123456789::numeric,
    'a_numeric',  3.14159::numeric,
    'a_inlobj',   jsonb_build_object('inner', 7, 'flag', true),
    'a_inlarr',   '[1,2,3,"x"]'::jsonb,
    'a_large',    (SELECT jsonb_agg(g) FROM generate_series(1, 10000) g)
));

-- Wrapper engaged?
SELECT pg_column_size(jb) < 2000 AS wrapper_small,
       (SELECT count(*) FROM jbtl_subtree_refs
        WHERE parent_toastrelid =
              (SELECT reltoastrelid FROM pg_class WHERE relname='r1_types')) AS refs
FROM r1_types WHERE id = 1;

-- Each inline type must round-trip correctly.
SELECT jb -> 'a_null'      AS v_null,
       (jb -> 'a_null') IS NOT DISTINCT FROM 'null'::jsonb AS is_json_null
FROM r1_types WHERE id = 1;
SELECT jb -> 'a_bool_t'    AS v_bool_t FROM r1_types WHERE id = 1;
SELECT jb -> 'a_bool_f'    AS v_bool_f FROM r1_types WHERE id = 1;
SELECT jb -> 'a_string'    AS v_string FROM r1_types WHERE id = 1;
SELECT jb -> 'a_emptystr'  AS v_emptystr FROM r1_types WHERE id = 1;
SELECT jb -> 'a_int_pos'   AS v_int_pos FROM r1_types WHERE id = 1;
SELECT jb -> 'a_int_neg'   AS v_int_neg FROM r1_types WHERE id = 1;
SELECT jb -> 'a_int_big'   AS v_int_big FROM r1_types WHERE id = 1;
SELECT jb -> 'a_numeric'   AS v_numeric FROM r1_types WHERE id = 1;
SELECT jb -> 'a_inlobj'    AS v_inlobj FROM r1_types WHERE id = 1;
SELECT jb -> 'a_inlarr'    AS v_inlarr FROM r1_types WHERE id = 1;

-- Out-of-line value (single key 'a_large' is the one relocated value):
SELECT jsonb_array_length(jb -> 'a_large') AS v_large_len FROM r1_types WHERE id = 1;

-- Missing keys at different alphabetical positions vs. the key set.
SELECT (jb -> '__before_all') IS NULL AS miss_before,
       (jb -> 'b_after_a')   IS NULL AS miss_middle,
       (jb -> 'zzz_after_all') IS NULL AS miss_after
FROM r1_types WHERE id = 1;

DROP TABLE r1_types;

--
-- Section 3: multi-byte UTF-8 key names.
-- The binary search uses length-then-memcmp; ensure UTF-8 bytes
-- compare correctly.
--

CREATE TABLE r1_utf8 (id int PRIMARY KEY, jb jsonb);
SELECT pgpro_toast.set_toaster('jsonb_toaster_lite', 'public.r1_utf8', 'jb');

INSERT INTO r1_utf8 VALUES (1, jsonb_build_object(
    'ключ',     'значение',
    '名前',     '太郎',
    'ключ_длинный',  77,
    'big_blob', (SELECT jsonb_agg(g) FROM generate_series(1, 10000) g)
));

SELECT jb -> 'ключ' AS cyrillic_key FROM r1_utf8 WHERE id = 1;
SELECT jb -> '名前' AS japanese_key FROM r1_utf8 WHERE id = 1;
SELECT jb -> 'ключ_длинный' AS cyrillic_long FROM r1_utf8 WHERE id = 1;
SELECT (jb -> 'absent_кл') IS NULL AS miss_cyrillic FROM r1_utf8 WHERE id = 1;
SELECT jsonb_array_length(jb -> 'big_blob') AS blob_len FROM r1_utf8 WHERE id = 1;

DROP TABLE r1_utf8;

--
-- Section 4: long keys (boundary of the key comparator).
--

CREATE TABLE r1_longkey (id int PRIMARY KEY, jb jsonb);
SELECT pgpro_toast.set_toaster('jsonb_toaster_lite', 'public.r1_longkey', 'jb');

INSERT INTO r1_longkey VALUES (1, jsonb_build_object(
    repeat('a', 250), 11,
    repeat('b', 250), 22,
    'short_key', 33,
    'large', (SELECT jsonb_agg(g) FROM generate_series(1, 10000) g)
));

SELECT jb -> repeat('a', 250) AS long_a FROM r1_longkey WHERE id = 1;
SELECT jb -> repeat('b', 250) AS long_b FROM r1_longkey WHERE id = 1;
SELECT jb -> 'short_key' AS short_v FROM r1_longkey WHERE id = 1;
SELECT (jb -> repeat('a', 249)) IS NULL AS miss_long FROM r1_longkey WHERE id = 1;

DROP TABLE r1_longkey;

--
-- Section 5: many-key object — exercise binary search at depth.
--

CREATE TABLE r1_manykeys (id int PRIMARY KEY, jb jsonb);
SELECT pgpro_toast.set_toaster('jsonb_toaster_lite', 'public.r1_manykeys', 'jb');

INSERT INTO r1_manykeys
SELECT 1, jsonb_object_agg(format('k_%04s', g), g) ||
          jsonb_build_object('LARGE', (SELECT jsonb_agg(s) FROM generate_series(1, 10000) s))
FROM generate_series(1, 100) g;

-- First key (alphabetically), middle, last; plus the large key.
SELECT jb -> 'k_0001'  AS k_first FROM r1_manykeys WHERE id = 1;
SELECT jb -> 'k_0050'  AS k_middle FROM r1_manykeys WHERE id = 1;
SELECT jb -> 'k_0100'  AS k_last FROM r1_manykeys WHERE id = 1;
SELECT jsonb_array_length(jb -> 'LARGE') AS k_large_len FROM r1_manykeys WHERE id = 1;

-- Missing at the binary-search boundaries.
SELECT (jb -> 'k_0000') IS NULL AS miss_before_first,
       (jb -> 'k_0101') IS NULL AS miss_after_last,
       (jb -> 'k_0049b') IS NULL AS miss_between
FROM r1_manykeys WHERE id = 1;

DROP TABLE r1_manykeys;

--
-- Section 6: parity with the inline-style fast path (no relocation).
-- Disable subtree storage and re-run a subset to confirm the
-- inline-style fast path still behaves correctly after the refactor.
--

SET jsonb_toaster_lite.enable_subtree_storage = off;

CREATE TABLE r1_inline (id int PRIMARY KEY, jb jsonb);
SELECT pgpro_toast.set_toaster('jsonb_toaster_lite', 'public.r1_inline', 'jb');

INSERT INTO r1_inline VALUES (1, jsonb_build_object(
    'a', 100,
    'b', 'text',
    'c', true,
    'd', (SELECT jsonb_agg(g) FROM generate_series(1, 2000) g)  -- big enough to toast
));

-- Wrapper should be inline-style, NOT relocation.
SELECT (SELECT count(*) FROM jbtl_subtree_refs
        WHERE parent_toastrelid =
              (SELECT reltoastrelid FROM pg_class WHERE relname='r1_inline')) AS refs_should_be_zero
FROM r1_inline WHERE id = 1;

SELECT jb -> 'a' AS a_value FROM r1_inline WHERE id = 1;
SELECT jb -> 'b' AS b_value FROM r1_inline WHERE id = 1;
SELECT jb -> 'c' AS c_value FROM r1_inline WHERE id = 1;
SELECT jsonb_array_length(jb -> 'd') AS d_len FROM r1_inline WHERE id = 1;
SELECT (jb -> 'missing') IS NULL AS d_miss FROM r1_inline WHERE id = 1;

DROP TABLE r1_inline;

-- Reset GUCs.
SET jsonb_toaster_lite.enable_subtree_storage = on;
SET jsonb_sort_field_values = off;
