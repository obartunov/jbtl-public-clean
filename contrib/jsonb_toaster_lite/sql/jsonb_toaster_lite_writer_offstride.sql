-- Regression for the writer offset-stride bug.
--
-- Background
-- ----------
-- jsonb stores JEntries with a length-or-cached-offset encoding:
-- every JB_OFFSET_STRIDE'th value JEntry carries JENTRY_HAS_OFF with
-- a cached offset instead of a direct length.  For an object with
-- N keys, value JEntries occupy children[N..2N-1]; for any value
-- index i in 0..N-1, HAS_OFF fires when (i + N) % JB_OFFSET_STRIDE == 0.
-- With JB_OFFSET_STRIDE == 32, the smallest N for which value index 0
-- carries HAS_OFF is N == 32.
--
-- The SUBTREE writer must reject HAS_OFF on any value JEntry whose
-- slot it intends to overwrite as ISCONTAINER_PTR, AND on every other
-- value JEntry, because the cached offsets in the post-spill body
-- would no longer match the shrunk data area.
--
-- Earlier the writer's check started at val_base + 1, missing the
-- val_base + 0 slot.  When N == 32 and the spillable big-container
-- happens to be the alphabetically-first key (so it lands at value
-- index 0 in the canonical key order), the writer silently produced
-- a SUBTREE row whose JEntry[val_base + 0] had its original HAS_OFF
-- bit cleared but its semantics torn from the surrounding entries.
-- Reading such a row crashed with "malloc(): corrupted top size".
--
-- This test pins the boundary: the row must either be readable
-- (writer correctly declines SUBTREE for this shape, falling back to
-- POINTER) or, if the writer were to accept it, the resulting SUBTREE
-- row must round-trip cleanly.  We verify by reading back via several
-- jsonb operators.

CREATE EXTENSION IF NOT EXISTS jsonb_toaster_lite;
SET jsonb_toaster_lite.enable_subtree_storage = on;

CREATE TABLE writer_offstride (id int4 PRIMARY KEY, jb jsonb, label text);
-- Discard set_toaster's OID return to keep this test stable across runs.
SELECT 'ok'::text AS attach FROM pgpro_toast.set_toaster('jsonb_toaster_lite', 'writer_offstride', 'jb');

-- Reproducer.
-- N == 32: 'big' alphabetically before the 31 's0xx' siblings, so big
-- lands at value index 0 in the canonical key order.  Stride hits
-- value index 0 (because (0 + 32) % 32 == 0).
INSERT INTO writer_offstride (id, jb, label)
SELECT 1,
  (SELECT jsonb_object_agg('s' || lpad(i::text, 3, '0'), i)
   FROM generate_series(1, 31) i)
  || jsonb_build_object('big',
       (SELECT jsonb_agg(jsonb_build_object('k', repeat('Z', 60), 'i', i))
        FROM generate_series(1, 500) i)),
  'N=32 big-first (reproducer)';

-- Control 1.
-- Same N == 32 but with 'zbig' sorting alphabetically last, so the
-- spillable container lands at value index 31, not 0.  Slot's JEntry
-- does NOT carry HAS_OFF; SUBTREE writer should still accept the row.
INSERT INTO writer_offstride (id, jb, label)
SELECT 2,
  (SELECT jsonb_object_agg('s' || lpad(i::text, 3, '0'), i)
   FROM generate_series(1, 31) i)
  || jsonb_build_object('zbig',
       (SELECT jsonb_agg(jsonb_build_object('k', repeat('Z', 60), 'i', i))
        FROM generate_series(1, 500) i)),
  'N=32 big-last (control: SUBTREE OK)';

-- Control 2.
-- N == 4: small object, no value JEntry carries HAS_OFF, SUBTREE
-- writer remains fully functional.
INSERT INTO writer_offstride (id, jb, label)
SELECT 3,
  jsonb_build_object('a', 1, 'b', 2, 'c', 3, 'big',
       (SELECT jsonb_agg(jsonb_build_object('k', repeat('Z', 60), 'i', i))
        FROM generate_series(1, 500) i)),
  'N=4 (control: SUBTREE OK)';

-- All three rows must read back without crashing and report
-- the same logical content.  pg_column_size differentiates
-- POINTER (~30 bytes) from SUBTREE (carries inline parent body).
SELECT id, label,
       (pg_column_size(jb) > 0)                AS readable,
       (length(jb::text) BETWEEN 40000 AND 41000) AS text_len_ok,
       CASE id WHEN 1 THEN jb ? 'big'
               WHEN 2 THEN jb ? 'zbig'
               WHEN 3 THEN jb ? 'big'
       END                                     AS has_big_key,
       CASE id WHEN 1 THEN jsonb_typeof(jb -> 'big')
               WHEN 2 THEN jsonb_typeof(jb -> 'zbig')
               WHEN 3 THEN jsonb_typeof(jb -> 'big')
       END                                     AS big_type,
       CASE id WHEN 1 THEN jsonb_array_length(jb -> 'big') = 500
               WHEN 2 THEN jsonb_array_length(jb -> 'zbig') = 500
               WHEN 3 THEN jsonb_array_length(jb -> 'big') = 500
       END                                     AS big_array_len_ok
FROM writer_offstride ORDER BY id;

-- Storage choice.  The reproducer (id=1) MUST fall back to POINTER
-- under the safe-refusal fix.  N=4 (id=3) MUST remain SUBTREE.
-- N=32 big-last (id=2) is classified informationally: depending on
-- the precision of the writer-side refusal it may land in either
-- POINTER (whole-container refusal) or SUBTREE (per-slot refusal).
-- We pin the corner cases that actually matter:
--   - id=1: must be POINTER  (no torn SUBTREE row);
--   - id=3: must be SUBTREE  (writer still functional for valid shapes).
SELECT id, label,
       CASE WHEN pg_column_size(jb) > 80 THEN 'SUBTREE'
            ELSE 'POINTER' END AS storage,
       CASE id WHEN 1 THEN (pg_column_size(jb) <= 80)
               WHEN 3 THEN (pg_column_size(jb) > 80)
               ELSE NULL END AS storage_assertion
FROM writer_offstride ORDER BY id;

-- Catalog must be self-consistent across the mix.
SELECT 'refs_check' AS k, jbtl_subtree_refs_check() AS violations;

DROP TABLE writer_offstride;
