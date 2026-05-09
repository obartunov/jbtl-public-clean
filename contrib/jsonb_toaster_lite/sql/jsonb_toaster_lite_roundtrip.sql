--
-- jsonb_toaster_lite L1.2b regression: plain-chunk full round-trip
--
-- Acceptance scope (L1.2b):
--   1. Small jsonb stored as inline JBTL_PLAIN_JSONB; equality preserved.
--   2. Large incompressible jsonb forced through toast chunks (plain,
--      no per-chunk compression); equality preserved through full
--      detoast.
--   3. DELETE removes toast rows from the toast relation.
--   4. After DELETE of the toasted row, the small inline row remains
--      readable.
--
-- Out of scope here (L1.2c+):
--   - per-chunk compression
--   - sliced detoast
--   - DIFF / DIRECT_TIDS modes
--   - chunked arrays
--

-- IF NOT EXISTS so the test can run in any order relative to
-- jsonb_toaster_lite.sql, which also creates these extensions.
CREATE EXTENSION IF NOT EXISTS toastapi;
CREATE EXTENSION IF NOT EXISTS jsonb_toaster_lite;

CREATE TABLE rt (id int PRIMARY KEY, j jsonb);
SELECT pgpro_toast.set_toaster('jsonb_toaster_lite', 'rt', 'j') > 0
       AS attached_to_jsonb_toaster_lite;

-- Helper: count rows in rt's toast relation by name lookup
CREATE FUNCTION pg_temp.rt_toast_count() RETURNS bigint LANGUAGE plpgsql AS $$
DECLARE
    n bigint;
BEGIN
    EXECUTE format('SELECT count(*) FROM pg_toast.%I',
                   (SELECT t.relname
                    FROM pg_class c JOIN pg_class t ON c.reltoastrelid = t.oid
                    WHERE c.relname = 'rt'))
    INTO n;
    RETURN n;
END
$$;

--
-- 1. Small jsonb (inline JBTL_PLAIN_JSONB path)
--
INSERT INTO rt VALUES (1, '{"a": 1, "b": "hello"}'::jsonb);

SELECT 'small_jsonb_eq=' ||
       (j = '{"a": 1, "b": "hello"}'::jsonb)::text  AS small_jsonb_eq
FROM rt WHERE id = 1;

SELECT 'small_text_eq=' ||
       (j::text = '{"a": 1, "b": "hello"}'::jsonb::text)::text  AS small_text_eq
FROM rt WHERE id = 1;

--
-- 2. Large incompressible jsonb (toast-chunk JBTL_POINTER path)
--
-- An md5-aggregated payload is incompressible enough that core's
-- inline pglz step cannot fit it in the heap tuple, so toasting is
-- forced and our jsonb_toaster_lite tsr_toast actually runs.  The
-- payload size (2000 * 32 = 64000 bytes) is well above any single
-- toast chunk so multiple chunks must be written.
--
INSERT INTO rt
SELECT 2, jsonb_build_object(
              'payload',
              (SELECT string_agg(md5(g::text), '')
                 FROM generate_series(1, 2000) g),
              'id', 42);

-- Toast happened: at least one chunk row in the toast relation.
SELECT 'toast_rows_after_insert>0=' ||
       (pg_temp.rt_toast_count() > 0)::text          AS toast_rows_after_insert;

-- Payload integrity through full detoast.
SELECT 'large_id=' ||           (j->>'id')           AS large_id           FROM rt WHERE id = 2;
SELECT 'large_payload_len=' ||  length(j->>'payload') AS large_payload_len FROM rt WHERE id = 2;

-- Build the same value in a CTE and check exact jsonb equality.
WITH expect AS (
    SELECT jsonb_build_object(
              'payload',
              (SELECT string_agg(md5(g::text), '')
                 FROM generate_series(1, 2000) g),
              'id', 42) AS j
)
SELECT 'large_jsonb_eq=' ||
       ((SELECT j FROM rt WHERE id = 2) = e.j)::text  AS large_jsonb_eq
FROM expect e;

WITH expect AS (
    SELECT jsonb_build_object(
              'payload',
              (SELECT string_agg(md5(g::text), '')
                 FROM generate_series(1, 2000) g),
              'id', 42) AS j
)
SELECT 'large_text_eq=' ||
       ((SELECT j::text FROM rt WHERE id = 2) = e.j::text)::text  AS large_text_eq
FROM expect e;

--
-- 3. DELETE removes the toast rows for row id=2
--
DELETE FROM rt WHERE id = 2;

SELECT 'toast_rows_after_delete=' || pg_temp.rt_toast_count()
                                                    AS toast_rows_after_delete;

--
-- 4. The remaining inline-stored row is still readable
--
SELECT 'remaining_count=' || count(*)                AS remaining_count
FROM rt;

SELECT 'inline_row_still_eq=' ||
       (j = '{"a": 1, "b": "hello"}'::jsonb)::text   AS inline_row_still_eq
FROM rt WHERE id = 1;

--
-- 5. UPDATE coverage (cleanup-2 review item C2: old_value is ignored;
--    UPDATE always re-toasts in full).  We do not verify the toast-row
--    accounting (delete-then-insert is core's responsibility), only
--    that the new value reads back correctly after the rewrite.
--
INSERT INTO rt SELECT 3, jsonb_build_object(
              'payload',
              (SELECT string_agg(md5(g::text), '')
                 FROM generate_series(1, 1500) g));

SELECT 'pre_update_eq=' ||
       ((SELECT j FROM rt WHERE id = 3) = jsonb_build_object(
              'payload',
              (SELECT string_agg(md5(g::text), '')
                 FROM generate_series(1, 1500) g)))::text
       AS pre_update_eq;

UPDATE rt SET j = jsonb_build_object(
              'replaced',
              (SELECT string_agg(md5((g+10000)::text), '')
                 FROM generate_series(1, 1500) g))
       WHERE id = 3;

SELECT 'post_update_eq=' ||
       ((SELECT j FROM rt WHERE id = 3) = jsonb_build_object(
              'replaced',
              (SELECT string_agg(md5((g+10000)::text), '')
                 FROM generate_series(1, 1500) g)))::text
       AS post_update_eq;

DROP TABLE rt;
