--
-- K1: jsonb KVMap (value-size sorting) regression test
--
-- Tests the JB_FOBJECT_KVMAP physical layout extension: when GUC
-- jsonb_sort_field_values is on, jsonb objects with mixed-size
-- values are stored with a KVMap so small values can physically
-- precede large ones.  Logical jsonb semantics (lookups, equality,
-- text output) must remain unchanged.

-- ============================================================
-- 1. Default OFF
-- ============================================================
SHOW jsonb_sort_field_values;

-- A plain object with no GUC change does not get a KVMap.
SELECT jsonb_kvmap_debug('{"a":1, "b":"hello"}'::jsonb);


-- ============================================================
-- 2. GUC ON writes KVMap; layout facts
-- ============================================================
SET jsonb_sort_field_values = on;

-- Build a 4-pair object with values of clearly different sizes:
--   payload 5000 bytes, id 8 bytes (numeric), flag 0 bytes (bool),
--   name 3 bytes ("abc").  Sort by size ascending should give
--   logical order (id, flag, name, payload) -> physical (2, 0, 1, 3).
SELECT (jsonb_kvmap_debug(jsonb_build_object(
            'payload', repeat('x', 5000),
            'id', 42,
            'flag', true,
            'name', 'abc')) -> 'has_kvmap')::text         AS has_kvmap,
       (jsonb_kvmap_debug(jsonb_build_object(
            'payload', repeat('x', 5000),
            'id', 42,
            'flag', true,
            'name', 'abc')) -> 'npairs')::text             AS npairs,
       (jsonb_kvmap_debug(jsonb_build_object(
            'payload', repeat('x', 5000),
            'id', 42,
            'flag', true,
            'name', 'abc')) -> 'kvmap_entry_size')::text  AS kvmap_entry_size;


-- ============================================================
-- 3. Short values precede the large value (physical offsets)
-- ============================================================
WITH dbg AS (
    SELECT jsonb_kvmap_debug(jsonb_build_object(
                'payload', repeat('x', 5000),
                'id', 42,
                'flag', true,
                'name', 'abc')) AS d
)
SELECT
    (((d -> 'value_offsets' ->> 'id')::int)
        < ((d -> 'value_offsets' ->> 'payload')::int))   AS id_before_payload,
    (((d -> 'value_offsets' ->> 'flag')::int)
        < ((d -> 'value_offsets' ->> 'payload')::int))   AS flag_before_payload,
    (((d -> 'value_offsets' ->> 'name')::int)
        < ((d -> 'value_offsets' ->> 'payload')::int))   AS name_before_payload
FROM dbg;


-- ============================================================
-- 4. Lookup correctness (must hold under GUC ON)
-- ============================================================
WITH j AS (
    SELECT jsonb_build_object(
                'payload', repeat('x', 5000),
                'id', 42,
                'flag', true,
                'name', 'abc') AS v
)
SELECT
    v ->> 'id'                               AS lookup_id,
    v ->> 'flag'                             AS lookup_flag,
    v ->> 'name'                             AS lookup_name,
    length(v ->> 'payload')                  AS lookup_payload_len,
    (v -> 'missing') IS NULL                 AS missing_returns_null,
    (v ? 'missing')                          AS contains_missing
FROM j;


-- ============================================================
-- 5. Equality with the unsorted form (build same object both ways)
-- ============================================================
SET jsonb_sort_field_values = off;
SELECT jsonb_build_object('big', repeat('x', 200), 'small', 1) AS unsorted_jb \gset
SELECT (jsonb_kvmap_debug(:'unsorted_jb'::jsonb) ->> 'has_kvmap') AS unsorted_has_kvmap;

SET jsonb_sort_field_values = on;
SELECT jsonb_build_object('big', repeat('x', 200), 'small', 1) AS sorted_jb \gset
SELECT (jsonb_kvmap_debug(:'sorted_jb'::jsonb) ->> 'has_kvmap') AS sorted_has_kvmap;

-- jsonb equality: physical layout must not affect logical equality.
SELECT (:'sorted_jb'::jsonb) = (:'unsorted_jb'::jsonb)             AS jsonb_eq;
SELECT (:'sorted_jb'::jsonb)::text = (:'unsorted_jb'::jsonb)::text AS text_eq;


-- ============================================================
-- 6. Round-trip through table storage
-- ============================================================
SET jsonb_sort_field_values = on;
CREATE TEMP TABLE k1 (id int PRIMARY KEY, j jsonb);
INSERT INTO k1 VALUES (1, jsonb_build_object(
                            'payload', repeat('x', 5000),
                            'id', 42,
                            'flag', true,
                            'name', 'abc'));

SELECT
    (jsonb_kvmap_debug(j) ->> 'has_kvmap') AS persisted_has_kvmap,
    j ->> 'id'                              AS rt_id,
    j ->> 'flag'                            AS rt_flag,
    j ->> 'name'                            AS rt_name,
    length(j ->> 'payload')                 AS rt_payload_len
FROM k1 WHERE id = 1;


-- ============================================================
-- 7. GUC OFF after ON — new objects don't get KVMap; old ones still readable
-- ============================================================
SET jsonb_sort_field_values = off;
SELECT (jsonb_kvmap_debug(j) ->> 'has_kvmap')   AS persisted_value_still_has_kvmap,
       j ->> 'id'                                AS still_lookup_id,
       length(j ->> 'payload')                   AS still_lookup_payload_len
FROM k1 WHERE id = 1;

-- new insert under GUC OFF: no KVMap
INSERT INTO k1 VALUES (2, jsonb_build_object(
                            'payload', repeat('x', 5000),
                            'id', 42,
                            'flag', true,
                            'name', 'abc'));
SELECT (jsonb_kvmap_debug(j) ->> 'has_kvmap')   AS new_value_has_kvmap,
       j ->> 'id'                                AS new_lookup_id
FROM k1 WHERE id = 2;

-- The two stored values are logically equal regardless of physical layout.
SELECT (a.j = b.j) AS row1_eq_row2
FROM k1 a, k1 b WHERE a.id = 1 AND b.id = 2;


-- ============================================================
-- 8. Scalar / array inputs to debug helper
-- ============================================================
SELECT jsonb_kvmap_debug('42'::jsonb);
SELECT jsonb_kvmap_debug('[1,2,3]'::jsonb);


-- ============================================================
-- 9. KVMap entry-size threshold: 256 pairs -> entry_size = 2
-- ============================================================
SET jsonb_sort_field_values = on;
WITH big AS (
    SELECT jsonb_object_agg('k' || lpad(i::text, 4, '0'),
                            CASE WHEN i % 2 = 0 THEN to_jsonb(i)
                                 ELSE to_jsonb(repeat('y', i)) END) AS j
    FROM generate_series(1, 300) i
)
SELECT
    (jsonb_kvmap_debug(j) ->> 'has_kvmap')        AS big_has_kvmap,
    (jsonb_kvmap_debug(j) ->> 'kvmap_entry_size') AS big_kvmap_entry_size,
    (jsonb_kvmap_debug(j) ->> 'npairs')           AS big_npairs,
    j ->> 'k0001'                                  AS lookup_first,
    j ->> 'k0150'                                  AS lookup_middle,
    j ->> 'k0300'                                  AS lookup_last,
    j ? 'k0001'                                    AS contains_first,
    j ? 'kZZZZ'                                    AS contains_missing
FROM big;


-- ============================================================
-- 10. Empty / single-pair objects (KVMap not emitted)
-- ============================================================
SET jsonb_sort_field_values = on;
SELECT jsonb_kvmap_debug('{}'::jsonb)           AS empty_obj;
SELECT jsonb_kvmap_debug('{"only":1}'::jsonb)   AS single_pair_obj;


-- ============================================================
-- 11. Uniform sizes — sort doesn't reorder, KVMap not emitted
-- ============================================================
-- All values are integers of identical size: estimateJsonbValueSize
-- returns the same number for each, the qsort doesn't move anything,
-- so the writer falls back to the unsorted layout.
SET jsonb_sort_field_values = on;
SELECT (jsonb_kvmap_debug('{"a":1, "b":2, "c":3, "d":4}'::jsonb)
        ->> 'has_kvmap')                                       AS uniform_has_kvmap;


SET jsonb_sort_field_values = off;
