--
-- jsonb_toaster_lite L1.4: KVMap-aware top-level object field lookup
--
-- Pins the fast-path / fallback / not-found branches of
-- jbtl_object_field, jbtl_object_field_text, and jbtl_object_field_probe.
--

\set ON_ERROR_STOP on

CREATE EXTENSION IF NOT EXISTS toastapi;
CREATE EXTENSION IF NOT EXISTS jsonb_toaster_lite;

-- jsonb_sort_field_values=on enables K1 KVMap on writes.  Most of
-- the fast-path code is exercised under both layouts, but with
-- KVMap-bearing layout the binary search uses a non-identity
-- logical-to-physical mapping, which is the more interesting case.

-- ============================================================
-- Section 1: KVMap-bearing object, mixed-size scalars (status,
-- kind, etc., all in chunk 0) plus one large field that triggers
-- the >50%-of-body threshold fallback.
-- ============================================================

SET jsonb_sort_field_values = on;

CREATE TABLE t1 (id int PRIMARY KEY, j jsonb STORAGE EXTERNAL);
SELECT pgpro_toast.set_toaster('jsonb_toaster_lite', 't1', 'j') > 0
       AS attached;

INSERT INTO t1 SELECT 1, jsonb_build_object(
    'id',       42,
    'status',   'STATUS_active',
    'kind',     'KIND_invoice',
    'tenant',   'TENANT_acme',
    'flags',    jsonb_build_array('flag_paid', 'flag_indexed'),
    'title',    'TITLE_' || repeat('t', 200),
    'summary',  'SUMMARY_' || repeat('s', 1000),
    'payload',  'PAYLOAD_' ||
                (SELECT string_agg(md5(g::text), '')
                   FROM generate_series(1, 1500) g),
    'is_test',  true,
    'has_null', NULL::jsonb);

-- Each lookup must agree byte-for-byte with the core operator.
-- This is the primary correctness pin.

SELECT key, jbtl_object_field_text(j, key) IS NOT DISTINCT FROM j->>key AS eq
FROM t1, (VALUES
    ('id'), ('status'), ('kind'), ('tenant'),
    ('title'), ('summary'), ('payload'),
    ('flags'),       -- nested array  -> fallback path
    ('is_test'),     -- bool
    ('has_null'),    -- json null     -> SQL NULL via ->>
    ('no_such_key')  -- absent
) AS k(key)
WHERE id = 1
ORDER BY key;

-- jsonb-returning variant: same equivalence.
SELECT key, jbtl_object_field(j, key) IS NOT DISTINCT FROM j->key AS eq
FROM t1, (VALUES
    ('id'), ('status'), ('flags'), ('is_test'),
    ('has_null'), ('no_such_key')
) AS k(key)
WHERE id = 1
ORDER BY key;

-- Probe: which keys take the fast path?  Pin the boolean shape, not
-- the absolute counters (those depend on TOAST_MAX_CHUNK_SIZE which
-- is BLCKSZ-derived).
--
-- Expected:
--   fast path                        fallback=false, value present
--   nested (flags)                   fallback=true,  value NULL
--   payload (>50% of body)           fallback=true,  value NULL
--   absent key                       fallback=false, value NULL
SELECT key,
       (p).fallback                              AS fallback,
       (p).value_byte_length > 0                 AS has_offset_info,
       ((p).value_text IS NOT NULL)              AS produced_text
FROM t1, (VALUES
    ('status'), ('flags'), ('payload'), ('no_such_key')
) AS k(key),
LATERAL (SELECT jbtl_object_field_probe(j, key) AS p) sub
WHERE id = 1
ORDER BY key;

-- chunks_fetched < chunks_total means we actually skipped chunks on
-- the fast path.  Pin this strict inequality for at least one key.
SELECT key,
       (p).chunks_total > 0          AS has_chunks,
       (p).chunks_fetched < (p).chunks_total AS skipped_some
FROM t1, (VALUES ('status')) AS k(key),
LATERAL (SELECT jbtl_object_field_probe(j, key) AS p) sub
WHERE id = 1;

DROP TABLE t1;

-- ============================================================
-- Section 2: KVMap-less object (small N, GUC off so the writer
-- doesn't emit a KVMap).  Same correctness must hold via the
-- identity-mapping branch (kvmap_entry_size=0).
-- ============================================================

SET jsonb_sort_field_values = off;

CREATE TABLE t2 (id int PRIMARY KEY, j jsonb STORAGE EXTERNAL);
SELECT pgpro_toast.set_toaster('jsonb_toaster_lite', 't2', 'j') > 0
       AS attached;

INSERT INTO t2 SELECT 1, jsonb_build_object(
    'a', 'alpha',
    'b', 'beta',
    'c', 'gamma',
    'big', repeat('x', 60000));

-- Equivalence even without KVMap.
SELECT key, jbtl_object_field_text(j, key) IS NOT DISTINCT FROM j->>key AS eq
FROM t2, (VALUES ('a'), ('b'), ('c'), ('big'), ('z')) AS k(key)
WHERE id = 1
ORDER BY key;

DROP TABLE t2;

-- ============================================================
-- Section 3: Inline jsonb_toaster_lite (JBTL_PLAIN_JSONB) and
-- non-CUSTOM jsonb (default toaster).  Both must take the
-- fallback branch; correctness is the only requirement.
-- ============================================================

SET jsonb_sort_field_values = on;

CREATE TABLE t3_lite (id int PRIMARY KEY, j jsonb STORAGE EXTERNAL);
CREATE TABLE t3_default (id int PRIMARY KEY, j jsonb);
SELECT pgpro_toast.set_toaster('jsonb_toaster_lite', 't3_lite', 'j') > 0
       AS attached;

-- Tiny object -> stored inline in our toaster (JBTL_PLAIN_JSONB).
INSERT INTO t3_lite     VALUES (1, '{"x": 1, "y": "small"}'::jsonb);
INSERT INTO t3_default  VALUES (1, '{"x": 1, "y": "small"}'::jsonb);

-- Equivalence.
SELECT 'lite_inline' AS variant, key,
       jbtl_object_field_text(j, key) IS NOT DISTINCT FROM j->>key AS eq
FROM t3_lite, (VALUES ('x'), ('y'), ('z')) AS k(key)
WHERE id = 1
ORDER BY key;

SELECT 'default'  AS variant, key,
       jbtl_object_field_text(j, key) IS NOT DISTINCT FROM j->>key AS eq
FROM t3_default, (VALUES ('x'), ('y'), ('z')) AS k(key)
WHERE id = 1
ORDER BY key;

-- Probe should report fallback=true for both (no fast path applies).
SELECT 'lite_inline' AS variant,
       (p).fallback AS fallback
FROM t3_lite,
LATERAL (SELECT jbtl_object_field_probe(j, 'y') AS p) sub
WHERE id = 1;

SELECT 'default' AS variant,
       (p).fallback AS fallback
FROM t3_default,
LATERAL (SELECT jbtl_object_field_probe(j, 'y') AS p) sub
WHERE id = 1;

DROP TABLE t3_lite, t3_default;

-- ============================================================
-- Section 4: Non-object roots (scalar, array).  All take fallback.
-- ============================================================

SET jsonb_sort_field_values = on;

CREATE TABLE t4 (id int PRIMARY KEY, j jsonb STORAGE EXTERNAL);
SELECT pgpro_toast.set_toaster('jsonb_toaster_lite', 't4', 'j') > 0
       AS attached;

-- Scalar root.  We use a large repeat() to push it through the
-- TOAST path rather than getting it inlined.
INSERT INTO t4 VALUES (1, to_jsonb(repeat('s', 60000)));
-- Array root.
INSERT INTO t4 VALUES (2, to_jsonb(ARRAY['a', 'b', 'c']));

-- ->> on a scalar / array with a key returns NULL in core.  Our
-- function must do the same via fallback.
SELECT id, jbtl_object_field_text(j, 'any_key') IS NULL AS is_null
FROM t4 ORDER BY id;

DROP TABLE t4;

-- ============================================================
-- Section 5: per-chunk PGLZ storage (compress_chunks=on).  Same
-- correctness must hold; the fast path engages but tsr_detoast
-- decompresses the prefix on the way through.
-- ============================================================

SET jsonb_sort_field_values = on;
SET jsonb_toaster_lite.compress_chunks = on;

CREATE TABLE t5 (id int PRIMARY KEY, j jsonb STORAGE EXTERNAL);
SELECT pgpro_toast.set_toaster('jsonb_toaster_lite', 't5', 'j') > 0
       AS attached;

-- Same shape as Section 1, but stored under per-chunk pglz.
INSERT INTO t5 SELECT 1, jsonb_build_object(
    'id',       42,
    'status',   'STATUS_active',
    'kind',     'KIND_invoice',
    'tenant',   'TENANT_acme',
    'flags',    jsonb_build_array('flag_paid', 'flag_indexed'),
    'title',    'TITLE_' || repeat('t', 200),
    'summary',  'SUMMARY_' || repeat('s', 1000),
    'payload',  'PAYLOAD_' ||
                (SELECT string_agg(md5(g::text), '')
                   FROM generate_series(1, 1500) g),
    'is_test',  true,
    'has_null', NULL::jsonb);

SET jsonb_toaster_lite.compress_chunks = off;

-- Equivalence with the core operator under compressed storage.
SELECT key, jbtl_object_field_text(j, key) IS NOT DISTINCT FROM j->>key AS eq
FROM t5, (VALUES
    ('id'), ('status'), ('kind'), ('tenant'),
    ('title'), ('summary'), ('payload'),
    ('flags'), ('is_test'), ('has_null'),
    ('no_such_key')
) AS k(key)
WHERE id = 1
ORDER BY key;

-- Probe shape (boolean parity): fast path engages on small scalars,
-- falls back on container/oversized just like the plain-mode case.
SELECT key,
       (p).fallback                AS fallback,
       (p).value_text IS NOT NULL  AS produced_text
FROM t5, (VALUES
    ('status'), ('flags'), ('payload'), ('no_such_key')
) AS k(key),
LATERAL (SELECT jbtl_object_field_probe(j, key) AS p) sub
WHERE id = 1
ORDER BY key;

DROP TABLE t5;
