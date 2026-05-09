--
-- M5.0b-1: GUC admission test.
--
--	Two new GUCs registered in _PG_init:
--	  jsonb_toaster_lite.enable_subtree_storage  (bool, default off)
--	  jsonb_toaster_lite.subtree_spill_threshold (int, default 4096)
--
--	Behaviour: zero — these GUCs are reserved here and consulted by
--	M5.0b-3 production spill.  Existing behaviour must remain
--	unchanged.
--

\set ON_ERROR_STOP on

CREATE EXTENSION IF NOT EXISTS toastapi;
CREATE EXTENSION IF NOT EXISTS jsonb_toaster_lite;

-- Ensure the lite shared library is loaded in this backend so that
-- _PG_init fires and registers the new GUCs.  Earlier tests in the
-- same suite have already done this, but the standalone path needs
-- it explicitly.
LOAD 'jsonb_toaster_lite';

-- =====================================================================
-- Pin 1: enable_subtree_storage default off
-- =====================================================================
SELECT 'pin_guc_default_off' AS what,
       current_setting('jsonb_toaster_lite.enable_subtree_storage') = 'off'
       AS pin;

-- =====================================================================
-- Pin 2: subtree_spill_threshold default 4096 bytes (shown as "4kB"
-- by GUC_UNIT_BYTE pretty-printing)
-- =====================================================================
SELECT 'pin_guc_threshold_default_4kb' AS what,
       current_setting('jsonb_toaster_lite.subtree_spill_threshold') = '4kB'
       AS pin;

-- =====================================================================
-- Pin 3: SET works at session level (USERSET context)
-- =====================================================================
SET jsonb_toaster_lite.enable_subtree_storage = on;
SELECT 'pin_guc_set_session_bool' AS what,
       current_setting('jsonb_toaster_lite.enable_subtree_storage') = 'on'
       AS pin;
RESET jsonb_toaster_lite.enable_subtree_storage;

SET jsonb_toaster_lite.subtree_spill_threshold = 8192;
SELECT 'pin_guc_set_session_int' AS what,
       current_setting('jsonb_toaster_lite.subtree_spill_threshold') = '8kB'
       AS pin;
RESET jsonb_toaster_lite.subtree_spill_threshold;

-- =====================================================================
-- Pin 4: SHOW works (presence check via plain command form)
-- =====================================================================
SHOW jsonb_toaster_lite.enable_subtree_storage;
SHOW jsonb_toaster_lite.subtree_spill_threshold;

-- =====================================================================
-- Pin 5: with the GUC ON (M5.0b-3 production spill is now active),
-- a large body produces a SUBTREE column that is much smaller than
-- the same body with the GUC OFF (which goes to the existing
-- chunked-write JBTL_POINTER path).
-- =====================================================================
SET jsonb_sort_field_values = off;

-- Baseline: GUCs at default (off, 4kB) — JBTL_POINTER path
CREATE TABLE m50b1_base (id int PRIMARY KEY, jb jsonb STORAGE EXTERNAL);
SELECT pgpro_toast.set_toaster('jsonb_toaster_lite', 'm50b1_base', 'jb') > 0
       AS attached_base;
INSERT INTO m50b1_base SELECT 1, jsonb_build_object(
  'key1', 100, 'key3', 100,
  'key2', (SELECT jsonb_agg(md5((100*1000 + s)::text))
             FROM generate_series(1, 1000) s));

-- Probe: GUCs ON — M5.0b-3 spill writes child separately.
SET jsonb_toaster_lite.enable_subtree_storage = on;
SET jsonb_toaster_lite.subtree_spill_threshold = 4096;
CREATE TABLE m50b1_probe (id int PRIMARY KEY, jb jsonb STORAGE EXTERNAL);
SELECT pgpro_toast.set_toaster('jsonb_toaster_lite', 'm50b1_probe', 'jb') > 0
       AS attached_probe;
INSERT INTO m50b1_probe SELECT 1, jsonb_build_object(
  'key1', 100, 'key3', 100,
  'key2', (SELECT jsonb_agg(md5((100*1000 + s)::text))
             FROM generate_series(1, 1000) s));

-- Both columns must read back equal jsonb (semantic equivalence).
SELECT 'pin_baseline_probe_jsonb_equal' AS what,
       (SELECT jb FROM m50b1_base) = (SELECT jb FROM m50b1_probe) AS pin;

-- Probe column should be SMALLER than baseline column when spill fires
-- (parent body small, child in separate chain).
SELECT 'pin_spill_reduces_column_size' AS what,
       (SELECT pg_column_size(jb) FROM m50b1_probe)
       <
       (SELECT pg_column_size(jb) FROM m50b1_base) AS pin;

RESET jsonb_toaster_lite.enable_subtree_storage;
RESET jsonb_toaster_lite.subtree_spill_threshold;
DROP TABLE m50b1_base, m50b1_probe;
TRUNCATE jbtl_subtree_refs;
