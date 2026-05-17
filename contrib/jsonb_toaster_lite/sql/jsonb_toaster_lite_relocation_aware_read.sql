--
-- jsonb_toaster_lite_relocation_aware_read
--
-- Deterministic correctness contract for the relocation-aware fast
-- path that the jsonb_object_field hook takes on relocation-format
-- wrappers (current internal mode tag: JBTL_POINTER_SUBTREE). Locks
-- in the dispatch and value shapes against future regressions.
--
-- The benchmark suite (jsonb_p1_bench_suite_v0.9) exercises
-- performance shape only; this test exercises edge cases that the
-- benchmark does not reach.
--
-- Test policy (FORMAT_POLICY.md §13):
--   1. Each setup section runs inside its own explicit transaction
--      with SET LOCAL for compatibility GUCs, so test runs do not
--      leak GUC state to other regression files.
--   2. The compatibility threshold is pinned per section so the
--      stored layout is deterministic and does not depend on the
--      postgresql.conf default for jsonb_toaster_lite.subtree_spill_threshold.
--   3. Each section asserts the actual stored layout (refs count
--      against expected, wrapper size bucket) before interpreting
--      read behaviour. Reads run in a separate transaction without
--      any GUC set, which also exercises the reader's
--      GUC-independence (FORMAT_POLICY.md §5).
--
-- Per-key layout assertions ("expected large keys map to out-of-line
-- fragments; expected inline keys remain inline") would require a
-- new C-level probe helper, because jbtl_subtree_refs records only
-- (parent_oid, parent_valueid) -> (child_oid, child_valueid) and
-- does not store the originating key name. This test stays at the
-- row-level layout assertion (refs count, wrapper size bucket).
-- See report on per-key test support as a follow-up.
--
-- Naming policy: comments use "relocation" terminology. Legacy
-- "subtree" appears only when citing exact existing symbols (the GUC
-- jsonb_toaster_lite.enable_subtree_storage, the prototype catalog
-- jbtl_subtree_refs, and the prototype mode tag JBTL_POINTER_SUBTREE).
--

CREATE EXTENSION IF NOT EXISTS toastapi;
CREATE EXTENSION IF NOT EXISTS jsonb_toaster_lite;

--
-- Section 1: basic 4-key dataset (mimics the J01 benchmark cell).
-- Two large arrays (relocated) + two small scalars (inline).
--

BEGIN;
SET LOCAL jsonb_toaster_lite.enable_subtree_storage = on;
SET LOCAL jsonb_toaster_lite.subtree_spill_threshold = 4096;
SET LOCAL jsonb_sort_field_values = off;

CREATE TABLE r1_basic (id int PRIMARY KEY, jb jsonb);
SELECT pgpro_toast.set_toaster('jsonb_toaster_lite', 'public.r1_basic', 'jb') > 0 AS toaster_attached;

INSERT INTO r1_basic VALUES (1, jsonb_build_object(
    'key1', 42,
    'key2', (SELECT jsonb_agg(g) FROM generate_series(1, 10000) g),
    'key3', 99,
    'key4', (SELECT jsonb_agg(g) FROM generate_series(1, 5000) g)
));

-- Layout assertion: 2 relocated values (key2, key4); wrapper holds
-- inline part only (small bucket).
SELECT pg_column_size(jb) < 1000 AS layout_wrapper_small,
       (SELECT count(*) FROM jbtl_subtree_refs
        WHERE parent_toastrelid =
              (SELECT reltoastrelid FROM pg_class WHERE relname='r1_basic')) = 2
            AS layout_relocated_count_is_2
FROM r1_basic WHERE id = 1;

COMMIT;

-- Reads in a fresh transaction, no compatibility GUC set: exercises
-- the reader's GUC-independence (FORMAT_POLICY.md §5).

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
-- Section 2: heterogeneous inline value types alongside a relocated value.
-- All inline JEntry kinds must produce the correct JsonbValue.
--

BEGIN;
SET LOCAL jsonb_toaster_lite.enable_subtree_storage = on;
SET LOCAL jsonb_toaster_lite.subtree_spill_threshold = 4096;
SET LOCAL jsonb_sort_field_values = off;

CREATE TABLE r1_types (id int PRIMARY KEY, jb jsonb);
SELECT pgpro_toast.set_toaster('jsonb_toaster_lite', 'public.r1_types', 'jb') > 0 AS toaster_attached;

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

-- Layout assertion: 1 relocated value (a_large); other 11 keys inline.
SELECT pg_column_size(jb) < 2000 AS layout_wrapper_small,
       (SELECT count(*) FROM jbtl_subtree_refs
        WHERE parent_toastrelid =
              (SELECT reltoastrelid FROM pg_class WHERE relname='r1_types')) = 1
            AS layout_relocated_count_is_1
FROM r1_types WHERE id = 1;

COMMIT;

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

-- Out-of-line value:
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

BEGIN;
SET LOCAL jsonb_toaster_lite.enable_subtree_storage = on;
SET LOCAL jsonb_toaster_lite.subtree_spill_threshold = 4096;
SET LOCAL jsonb_sort_field_values = off;

CREATE TABLE r1_utf8 (id int PRIMARY KEY, jb jsonb);
SELECT pgpro_toast.set_toaster('jsonb_toaster_lite', 'public.r1_utf8', 'jb') > 0 AS toaster_attached;

INSERT INTO r1_utf8 VALUES (1, jsonb_build_object(
    'ключ',     'значение',
    '名前',     '太郎',
    'ключ_длинный',  77,
    'big_blob', (SELECT jsonb_agg(g) FROM generate_series(1, 10000) g)
));

-- Layout assertion: 1 relocated value (big_blob); 3 UTF-8 keys inline.
SELECT pg_column_size(jb) < 1000 AS layout_wrapper_small,
       (SELECT count(*) FROM jbtl_subtree_refs
        WHERE parent_toastrelid =
              (SELECT reltoastrelid FROM pg_class WHERE relname='r1_utf8')) = 1
            AS layout_relocated_count_is_1
FROM r1_utf8 WHERE id = 1;

COMMIT;

SELECT jb -> 'ключ' AS cyrillic_key FROM r1_utf8 WHERE id = 1;
SELECT jb -> '名前' AS japanese_key FROM r1_utf8 WHERE id = 1;
SELECT jb -> 'ключ_длинный' AS cyrillic_long FROM r1_utf8 WHERE id = 1;
SELECT (jb -> 'absent_кл') IS NULL AS miss_cyrillic FROM r1_utf8 WHERE id = 1;
SELECT jsonb_array_length(jb -> 'big_blob') AS blob_len FROM r1_utf8 WHERE id = 1;

DROP TABLE r1_utf8;

--
-- Section 4: long keys (boundary of the key comparator).
--

BEGIN;
SET LOCAL jsonb_toaster_lite.enable_subtree_storage = on;
SET LOCAL jsonb_toaster_lite.subtree_spill_threshold = 4096;
SET LOCAL jsonb_sort_field_values = off;

CREATE TABLE r1_longkey (id int PRIMARY KEY, jb jsonb);
SELECT pgpro_toast.set_toaster('jsonb_toaster_lite', 'public.r1_longkey', 'jb') > 0 AS toaster_attached;

INSERT INTO r1_longkey VALUES (1, jsonb_build_object(
    repeat('a', 250), 11,
    repeat('b', 250), 22,
    'short_key', 33,
    'large', (SELECT jsonb_agg(g) FROM generate_series(1, 10000) g)
));

-- Layout assertion: 1 relocated value (large); 3 keys inline. The
-- 250-byte key names live inside the inline part, so the wrapper
-- size budget is wider than the 4-key J01 baseline.
SELECT pg_column_size(jb) < 2000 AS layout_wrapper_small,
       (SELECT count(*) FROM jbtl_subtree_refs
        WHERE parent_toastrelid =
              (SELECT reltoastrelid FROM pg_class WHERE relname='r1_longkey')) = 1
            AS layout_relocated_count_is_1
FROM r1_longkey WHERE id = 1;

COMMIT;

SELECT jb -> repeat('a', 250) AS long_a FROM r1_longkey WHERE id = 1;
SELECT jb -> repeat('b', 250) AS long_b FROM r1_longkey WHERE id = 1;
SELECT jb -> 'short_key' AS short_v FROM r1_longkey WHERE id = 1;
SELECT (jb -> repeat('a', 249)) IS NULL AS miss_long FROM r1_longkey WHERE id = 1;

DROP TABLE r1_longkey;

--
-- Section 5: many-key object — exercise binary search at depth.
--
-- Note: the current relocation writer does not engage SUBTREE on a
-- many-key object with one large value (the cost model does not favour
-- it). To keep this section meaningful for the binary-search depth
-- coverage of the shared key-lookup helper, run with relocation OFF
-- and assert no refs — the binary search then exercises the
-- inline-style fast path on a 101-key object.
--

BEGIN;
SET LOCAL jsonb_toaster_lite.enable_subtree_storage = off;
SET LOCAL jsonb_toaster_lite.subtree_spill_threshold = 4096;
SET LOCAL jsonb_sort_field_values = off;

CREATE TABLE r1_manykeys (id int PRIMARY KEY, jb jsonb);
SELECT pgpro_toast.set_toaster('jsonb_toaster_lite', 'public.r1_manykeys', 'jb') > 0 AS toaster_attached;

-- Build a 101-key object: 100 sequential keys k_0001..k_0100, plus
-- one LARGE array. lpad gives a true zero-padded key; format('%04s')
-- pads with spaces and would give "k_   1", not "k_0001".
INSERT INTO r1_manykeys
SELECT 1, jsonb_object_agg('k_' || lpad(g::text, 4, '0'), g) ||
          jsonb_build_object('LARGE', (SELECT jsonb_agg(s) FROM generate_series(1, 10000) s))
FROM generate_series(1, 100) g;

-- Layout assertion: relocation OFF, refs must be zero. The binary
-- search below runs in the inline-style fast path (the refactored
-- jbtl_find_key_in_object helper).
SELECT (SELECT count(*) FROM jbtl_subtree_refs
        WHERE parent_toastrelid =
              (SELECT reltoastrelid FROM pg_class WHERE relname='r1_manykeys')) = 0
            AS layout_no_relocation
FROM r1_manykeys WHERE id = 1;

COMMIT;

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
-- Disable relocation storage and confirm the inline-style fast path
-- behaves correctly after the refactor.
--

BEGIN;
SET LOCAL jsonb_toaster_lite.enable_subtree_storage = off;
SET LOCAL jsonb_toaster_lite.subtree_spill_threshold = 4096;
SET LOCAL jsonb_sort_field_values = off;

CREATE TABLE r1_inline (id int PRIMARY KEY, jb jsonb);
SELECT pgpro_toast.set_toaster('jsonb_toaster_lite', 'public.r1_inline', 'jb') > 0 AS toaster_attached;

INSERT INTO r1_inline VALUES (1, jsonb_build_object(
    'a', 100,
    'b', 'text',
    'c', true,
    'd', (SELECT jsonb_agg(g) FROM generate_series(1, 2000) g)  -- big enough to toast
));

-- Layout assertion: relocation OFF, so refs must be zero. The 'd'
-- array is still TOAST-eligible, but the wrapper-format does not
-- use relocation when the compatibility GUC is off.
SELECT (SELECT count(*) FROM jbtl_subtree_refs
        WHERE parent_toastrelid =
              (SELECT reltoastrelid FROM pg_class WHERE relname='r1_inline')) = 0
            AS layout_no_relocation
FROM r1_inline WHERE id = 1;

COMMIT;

SELECT jb -> 'a' AS a_value FROM r1_inline WHERE id = 1;
SELECT jb -> 'b' AS b_value FROM r1_inline WHERE id = 1;
SELECT jb -> 'c' AS c_value FROM r1_inline WHERE id = 1;
SELECT jsonb_array_length(jb -> 'd') AS d_len FROM r1_inline WHERE id = 1;
SELECT (jb -> 'missing') IS NULL AS d_miss FROM r1_inline WHERE id = 1;

DROP TABLE r1_inline;
