--
-- β bridge proof: verify Toastapi_update_hook fires for
--   old = CUSTOM jbtl pointer + new = regular jsonb varlena
-- (the default jsonb_set output in master).
--
-- Without the β bridge in core toast_helper.c, this hook is
-- unreachable from any standard SQL update; jbtl_update_calls()
-- would stay 0 forever.  With the bridge, every UPDATE that
-- modifies the jsonb column increments the counter.
--
-- The hook itself currently returns Datum 0 (decline), so the
-- final stored value is produced by the standard fallback path
-- (delete-old + tsr_toast new).  That keeps existing behaviour
-- byte-identical and lets us also assert read-correctness here.
--

\set ON_ERROR_STOP on

CREATE EXTENSION IF NOT EXISTS toastapi;
CREATE EXTENSION IF NOT EXISTS jsonb_toaster_lite;

SET jsonb_sort_field_values = on;
SET jsonb_toaster_lite.compress_chunks = off;

CREATE TABLE beta_proof (id int PRIMARY KEY, jb jsonb STORAGE EXTERNAL);
SELECT pgpro_toast.set_toaster('jsonb_toaster_lite', 'beta_proof', 'jb') > 0
       AS attached;

-- Insert a payload large enough to be truly toasted.
INSERT INTO beta_proof SELECT 100, jsonb_build_object(
    'key1', 100, 'key3', 100,
    'key2', (SELECT jsonb_agg(md5((100*1000 + s)::text))
               FROM generate_series(1, 1000) s));

-- Reset the call counter; INSERTs do NOT call tsr_update,
-- but if any earlier session left counter incremented we want a
-- clean baseline.
SELECT jbtl_update_calls_reset();

-- Pin: counter is 0 at the start.
SELECT 'pin_zero_before' AS what,
       jbtl_update_calls() = 0 AS pin;

-- ============================================================
-- Scenario 1: jsonb_set produces regular jsonb new value;
--             β bridge MUST route the update via tsr_update.
-- ============================================================

UPDATE beta_proof SET jb = jsonb_set(jb, '{key3}', '99'::jsonb)
WHERE id = 100;

SELECT 'pin_called_once' AS what,
       jbtl_update_calls() = 1 AS pin;

-- Post-update read: result is correct (hook returned 0,
-- core fell back to delete+toast, value persisted via fallback).
SELECT 'pin_correctness' AS what,
       (jb->>'key3')::int = 99 AS pin
FROM beta_proof WHERE id = 100;

-- ============================================================
-- Scenario 2: another UPDATE; counter increments by exactly 1.
-- ============================================================

UPDATE beta_proof SET jb = jsonb_set(jb, '{key3}', '88'::jsonb)
WHERE id = 100;

SELECT 'pin_called_twice' AS what,
       jbtl_update_calls() = 2 AS pin;

-- ============================================================
-- Scenario 3: UPDATE that does NOT change the column does not
--   invoke tsr_update (it does not even reach toast_tuple_init's
--   per-attribute branch for this column in a useful way).
-- ============================================================

SELECT jbtl_update_calls_reset();

UPDATE beta_proof SET id = id WHERE id = 100;  -- no jb change

-- Counter stayed at 0.
SELECT 'pin_no_call_when_unchanged' AS what,
       jbtl_update_calls() = 0 AS pin;

-- ============================================================
-- Scenario 4: UPDATE with the SAME jb value bytes.  Core's
--   byte-identical case (case 2) should fire and skip tsr_update
--   entirely.
-- ============================================================

UPDATE beta_proof SET jb = jb WHERE id = 100;

SELECT 'pin_no_call_when_byte_identical' AS what,
       jbtl_update_calls() = 0 AS pin;

DROP TABLE beta_proof;
