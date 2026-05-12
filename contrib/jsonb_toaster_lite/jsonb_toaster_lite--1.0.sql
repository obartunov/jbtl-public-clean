/* contrib/jsonb_toaster_lite/jsonb_toaster_lite--1.0.sql */

-- complain if script is sourced in psql, rather than via CREATE EXTENSION
\echo Use "CREATE EXTENSION jsonb_toaster_lite" to load this file. \quit

CREATE FUNCTION jsonb_toaster_lite_handler(internal)
RETURNS internal
AS 'MODULE_PATHNAME'
LANGUAGE C;

DO
$$
BEGIN
 IF to_regclass('pg_catalog.pg_toaster') IS NOT NULL
 THEN
   EXECUTE 'CREATE TOASTER jsonb_toaster_lite HANDLER jsonb_toaster_lite_handler';
   EXECUTE 'COMMENT ON TOASTER jsonb_toaster_lite IS ''jsonb_toaster_lite - TOAST/storage layer for jsonb''';
 ELSE
   PERFORM pgpro_toast.add_toaster('jsonb_toaster_lite', 'jsonb_toaster_lite_handler');
 END IF;
END
$$;

--
-- jbtl_slice_probe(jb, offset, length)
--
-- Test-only helper exposing the sliced-read counters
-- and the slice payload. Output columns:
--  slice_bytes bytea raw bytes of the requested slice
--  chunks_total int ceil(attrsize / TOAST_MAX_CHUNK_SIZE)
--  chunks_fetched int chunks actually decoded for this slice
--  toast_pages_touched int count(distinct blockno) of fetched
--  toast tuples; honest physical-I/O
--  metric — chunks_fetched can overstate
--  page savings because ~4 chunks live
--  on one 8KB page
--  chunks_decompressed int per-chunk pglz decompression count;
--  0 in plain mode, >0 only in
--  JBTL_POINTER_COMPRESSED_CHUNKS
--  bytes_decompressed int total post-decompression bytes
--  produced by per-chunk pglz; 0 in
--  plain mode
--
-- For inline JBTL_PLAIN_JSONB values, all five counters are 0
-- because no toast rows exist at all.
--
CREATE FUNCTION jbtl_slice_probe(jb jsonb, sliceoffset int, slicelength int,
                                 OUT slice_bytes bytea,
                                 OUT chunks_total int,
                                 OUT chunks_fetched int,
                                 OUT toast_pages_touched int,
                                 OUT chunks_decompressed int,
                                 OUT bytes_decompressed int)
RETURNS record
AS 'MODULE_PATHNAME', 'jbtl_slice_probe'
LANGUAGE C STRICT;

--
-- jbtl_chunk_inspect(jb): per-row dump of the toast relation backing
-- a jsonb_toaster_lite value. Storage-layout observability;
-- complements jbtl_slice_probe (which measures read behaviour).
--
-- toast_blockno is the heap-page blockno of the toast tuple
-- (= ItemPointerGetBlockNumber(t_self)). Used to audit page-level
-- locality: GROUP BY toast_blockno gives the page->chunks fan-out.
--
CREATE FUNCTION jbtl_chunk_inspect(jb jsonb,
                                   OUT chunk_no int,
                                   OUT chunk_seq int,
                                   OUT raw_offset int,
                                   OUT raw_size int,
                                   OUT stored_size int,
                                   OUT is_compressed bool,
                                   OUT compression_method int,
                                   OUT toast_blockno int)
RETURNS SETOF record
AS 'MODULE_PATHNAME', 'jbtl_chunk_inspect'
LANGUAGE C STRICT;

--
-- KVMap-aware top-level object field lookup with sliced TOAST.
--

--
-- jbtl_object_field(jb, key) -> jsonb
--  Equivalent to `jb->key`; uses sliced TOAST fetch for top-level
--  object lookups when jb is stored under jsonb_toaster_lite and
--  the value is a scalar. Falls back to core's jsonb_object_field
--  for nested values, non-object roots, inline JBTL_PLAIN_JSONB,
--  and default-toaster jsonb.
--
CREATE FUNCTION jbtl_object_field(jb jsonb, key text)
RETURNS jsonb
AS 'MODULE_PATHNAME', 'jbtl_object_field'
LANGUAGE C STRICT IMMUTABLE PARALLEL SAFE;

--
-- jbtl_object_field_text(jb, key) -> text
--  Equivalent to `jb->>key`. For scalar values via the fast path,
--  formats them to text without round-tripping through
--  JsonbValueToJsonb.
--
CREATE FUNCTION jbtl_object_field_text(jb jsonb, key text)
RETURNS text
AS 'MODULE_PATHNAME', 'jbtl_object_field_text'
LANGUAGE C STRICT IMMUTABLE PARALLEL SAFE;

--
-- jbtl_object_field_probe(jb, key)
--  Test/observability helper. Exposes:
--  chunks_total int -- total chunks of the value
--  chunks_fetched int -- chunks read for this lookup
--  (sum across prefix + value fetches)
--  value_byte_offset int -- offset of the value within the
--  jsonb body (no VARHDRSZ); 0 on
--  fallback/not-found
--  value_byte_length int -- value length in bytes; 0 on
--  fallback/not-found
--  fallback bool -- true if fast path declined
--  (caller should use ->>/-> instead)
--  toast_pages_touched int -- count(distinct blockno) of toast
--  tuples this fast-path read touched.
--  Honest physical-I/O metric.
--  0 on fallback (no toast scan done).
--  chunks_decompressed int -- per-chunk pglz decompression count
--  (0 in plain mode; reserved for the
--  compressed-mode plumbing that does
--  not yet thread it through )
--  bytes_decompressed int -- post-decompression byte count
--  (0 in plain mode)
--  value_text text -- value formatted to text, NULL on
--  fallback or key-not-found
--  Lets the bench attribute fetched chunks per op and observe the
--  "what would KVMap-aware fetch read" answer empirically.
--
CREATE FUNCTION jbtl_object_field_probe(jb jsonb, key text,
                                        OUT chunks_total int,
                                        OUT chunks_fetched int,
                                        OUT value_byte_offset int,
                                        OUT value_byte_length int,
                                        OUT fallback bool,
                                        OUT toast_pages_touched int,
                                        OUT chunks_decompressed int,
                                        OUT bytes_decompressed int,
                                        OUT value_text text)
RETURNS record
AS 'MODULE_PATHNAME', 'jbtl_object_field_probe'
LANGUAGE C STRICT IMMUTABLE PARALLEL SAFE;

-- debug helpers (debug-only, used by regression).
-- jbtl_update_calls() — counter of times tsr_update fired
-- jbtl_update_calls_reset() — resets the counter to 0
CREATE FUNCTION jbtl_update_calls() RETURNS int
AS 'MODULE_PATHNAME', 'jbtl_update_calls' LANGUAGE C;

CREATE FUNCTION jbtl_update_calls_reset() RETURNS void
AS 'MODULE_PATHNAME', 'jbtl_update_calls_reset' LANGUAGE C;

-- counter of DIFF pointers actually emitted by jbtl_update
-- (vs declined back to the standard delete+toast fallback path).
CREATE FUNCTION jbtl_update_diffs_emitted() RETURNS int
AS 'MODULE_PATHNAME', 'jbtl_update_diffs_emitted' LANGUAGE C;

-- M9.2: narrow SUBTREE sub-object reuse diagnostic counters.
--   attempts   = times jbtl_update entered the SUBTREE branch
--   successes  = times the branch returned a reused row
--   children   = total children whose toast chain was preserved
-- attempts - successes = declines that fell through to detoast+retoast.
CREATE FUNCTION jbtl_update_subtree_reuse_attempts() RETURNS int
AS 'MODULE_PATHNAME', 'jbtl_update_subtree_reuse_attempts_fn' LANGUAGE C;

CREATE FUNCTION jbtl_update_subtree_reuse_successes() RETURNS int
AS 'MODULE_PATHNAME', 'jbtl_update_subtree_reuse_successes_fn' LANGUAGE C;

CREATE FUNCTION jbtl_update_subtree_children_reused() RETURNS int
AS 'MODULE_PATHNAME', 'jbtl_update_subtree_children_reused_fn' LANGUAGE C;

-- test fixture for synthesising JBTL_POINTER_SUBTREE values
-- without a production writer. Takes (table, parent_jsonb, key) and
-- returns a custom-pointer that reads as the equivalent jsonb but
-- internally carries a JBTL_JENTRY_ISCONTAINER_PTR for that key.
CREATE FUNCTION jbtl_test_subtree_spill_key(regclass, jsonb, text, oid)
    RETURNS jsonb
    AS 'MODULE_PATHNAME', 'jbtl_test_subtree_spill_key' LANGUAGE C;

-- =====================================================================
-- subtree refs catalog
--
--  Edge between a parent SUBTREE custom-varlena and its child toast
--  chain. One row per (parent, child) pairing. Deleting a parent
--  row must call jbtl_subtree_refs_delete_one for each of its
--  children; child toast chain may then be deleted only if no
--  remaining edges reference it.
--
--  parent_toastrelid + parent_valueid identify the parent.
--  parent_valueid is the synthetic OID embedded in the v1
--  JbtlSubtreeHeader (see spec section 18.I-3).
--
--  child_toastrelid + child_valueid identify the child chain in
--  pg_toast.<toastrel>.
--
--  PK orders parent first to optimise the per-parent edge scan
--  used at delete time. Inverse index on (child_*) is used by
--  jbtl_subtree_refs_child_orphan() to count remaining edges to
--  a given child.
-- =====================================================================
CREATE TABLE jbtl_subtree_refs (
    parent_toastrelid  oid NOT NULL,
    parent_valueid     oid NOT NULL,
    child_toastrelid   oid NOT NULL,
    child_valueid      oid NOT NULL,
    PRIMARY KEY (parent_toastrelid, parent_valueid,
                 child_toastrelid,  child_valueid)
);

CREATE INDEX jbtl_subtree_refs_child_idx
    ON jbtl_subtree_refs (child_toastrelid, child_valueid);

-- Read-only access to PUBLIC for debugging / observability (e.g. running
-- jbtl_subtree_refs_check() from a non-superuser session). All MUTATIONS
-- happen exclusively through C-level CatalogTupleInsert / simple_heap_*
-- which bypass aclcheck on the relation. Granting INSERT/UPDATE/DELETE
-- to PUBLIC would be a privilege-escalation bug: any logged-in user
-- could corrupt refcount state and break delete dispatch.
GRANT SELECT ON jbtl_subtree_refs TO PUBLIC;

--
-- jbtl_subtree_refs_check()
--  Returns the count of edges whose child toast chain no longer
--  exists in pg_toast.<child_toastrelid>. A clean refs table
--  returns 0; non-zero indicates either incomplete delete logic
--  or external corruption. Manual / read-only.
--
CREATE FUNCTION jbtl_subtree_refs_check() RETURNS int
AS 'MODULE_PATHNAME', 'jbtl_subtree_refs_check' LANGUAGE C;

--
-- jbtl_subtree_refs_gc()
--  Removes dead edges (those with no live child chain). Returns
--  count of edges removed. Manual sweep until vacuum hook lands
--  in a later milestone. Per @yoda spec section 17.
--
CREATE FUNCTION jbtl_subtree_refs_gc() RETURNS int
AS 'MODULE_PATHNAME', 'jbtl_subtree_refs_gc' LANGUAGE C;

--
--  test helper: allocate one parent_valueid via the production
-- allocator path (Option 1: GetNewOidWithIndex + jbtl_subtree_refs
-- probe). Used by acceptance pins; not a production interface.
--
CREATE FUNCTION jbtl_test_alloc_parent_valueid(regclass) RETURNS oid
AS 'MODULE_PATHNAME', 'jbtl_test_alloc_parent_valueid' LANGUAGE C;

--
--  test helper: insert one edge directly. Used by acceptance
-- pins to exercise the C API without going through tsr_toast (which
-- doesn't yet emit edges; that lands).
--
CREATE FUNCTION jbtl_test_refs_insert(oid, oid, oid, oid) RETURNS void
AS 'MODULE_PATHNAME', 'jbtl_test_refs_insert' LANGUAGE C;

--
--  test helper: delete one edge directly; returns remaining
-- refcount for (child_toastrelid, child_valueid) after the delete.
--
CREATE FUNCTION jbtl_test_refs_delete_one(oid, oid, oid, oid) RETURNS int
AS 'MODULE_PATHNAME', 'jbtl_test_refs_delete_one' LANGUAGE C;

--
--  test helper: check parent_valueid uniqueness probe.
--
CREATE FUNCTION jbtl_test_refs_parent_id_in_use(oid, oid) RETURNS boolean
AS 'MODULE_PATHNAME', 'jbtl_test_refs_parent_id_in_use' LANGUAGE C;

-- Test helper: SQL wrapper around jbtl_subtree_refs_child_orphan.
CREATE FUNCTION jbtl_test_refs_child_orphan(oid, oid) RETURNS boolean
AS 'MODULE_PATHNAME', 'jbtl_test_refs_child_orphan' LANGUAGE C;
