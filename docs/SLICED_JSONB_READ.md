# Sliced jsonb read — production Layer 1

## Goal and non-goals

Specify the production read primitive that exploits the fork's
existing value-aware physical layout (KVMap, `jsonb_sort_field_values`)
to answer `jb -> 'key'` without detoasting the whole jsonb body.

This is **Layer 1** in the architecture set out in
`docs/PREFIX_LOCALITY_VS_RELOCATION.md`:

  Layer 0  KVMap + `jsonb_sort_field_values` + value-aware
           placement. Already in the fork.
  Layer 1  This document. TOAST-aware sliced read.
  Layer 2  Relocation (descriptor + child TOAST chains).
           Separate task.

This document is Layer 1 only. It is **not** a replacement for
relocation. It is the read primitive that both single-chain
jsonb and (later) relocation-aware jsonb call into.

In scope:

  - the signature and semantics of a sliced read helper inside
    production jsonb;
  - integration points in `jsonb_object_field`,
    `jsonb_object_field_text`, `getKeyJsonValueFromContainer`,
    and adjacent paths;
  - prefix-fetch and value-fetch algorithm against
    `detoast_attr_slice`;
  - compression-aware fallback rules;
  - interaction with `jsonb_sort_field_values` (including the
    explicit choice not to flip its default in this task);
  - acceptance evidence required before any patch is merged.

Out of scope:

  - relocation descriptor format and JEntry type tags
    (`docs/PRODUCTION_RELOCATION_SOURCE_MAPPING.md`);
  - delete-side ownership for relocation
    (`docs/RELOCATION_DELETE_OWNERSHIP.md`);
  - modify/UPDATE/WAL behaviour;
  - any change to the default of `jsonb_sort_field_values`;
  - nested-container value retrieval (the helper restricts itself
    to scalars in v0, matches the JBTL prototype's restriction).

## Conventions

Production terms: jsonb body, inline storage, out-of-line
storage, TOAST chain, structural prefix, key area, value area,
read path, sliced read. The word "spill" appears only inside
existing JBTL prototype identifiers when those identifiers are
referenced; the production design has no notion of "spill". The
word "emit" is avoided in prose.

`src/...` paths are relative to the fork repo root. `contrib/...`
paths point at JBTL prototype code used as a reference
implementation.

## 1. Existing fork jsonb layout facts

The fork's on-disk jsonb body for an object container, top-level:

```
[ container header (4 B)                                  ]
[ 2N JEntries (4 B each), keys first then values          ]
[ optional KVMap region (1/2/4 B per pair, INTALIGN'd)    ]
[ key area: N keys, concatenated                          ]
[ value area: N values, concatenated                      ]
```

Locations and definitions:

  - container header bits and `JB_FOBJECT_KVMAP = 0x80000000`:
    `src/include/utils/jsonb.h:226`.
  - `JsonContainerHasKVMap`: `src/include/utils/jsonb.h:233`.
  - `JsonbKVMap` runtime view and `JSONB_KVMAP_ENTRY_SIZE`:
    `src/include/utils/jsonb.h:245-258`.
  - writer that materialises this layout (and decides KVMap
    presence based on `jsonb_sort_field_values`):
    `src/backend/utils/adt/jsonb_util.c:2042` `convertJsonbObject`.
  - reader that consults KVMap during binary search:
    `src/backend/utils/adt/jsonb_util.c:416`
    `getKeyJsonValueFromContainer`.

The writer guarantees that, when KVMap is written, the value area
is sorted by ascending value size. The reader's
`getKeyJsonValueFromContainer` is already KVMap-aware: it does a
binary search over the key area (which stays in original logical
order) and then translates the matched index through KVMap to the
physical value index.

This means: **a sliced reader does not need to invent new
indexing logic.** It needs to fetch enough prefix bytes for the
existing in-memory key-search to run, then fetch the value bytes
the existing in-memory `fillJsonbValue` needs.

`fillJsonbValue` (`jsonb_util.c:531-572`) materialises a scalar
into a `JsonbValue` from a `(base_addr, offset)` pair. It only
needs the value's own bytes, not the surrounding body. This is
the natural cut point.

## 2. Current read path and why it full-detoasts

`jsonb_object_field` in `src/backend/utils/adt/jsonfuncs.c:866`:

```
Toastapi_jsonb_object_field_hook -> (extension fast path, if any)
jb = PG_GETARG_JSONB_P(0);   // <-- full detoast happens here
key = PG_GETARG_TEXT_PP(1);
if (!JB_ROOT_IS_OBJECT(jb)) RETURN NULL;
v = getKeyJsonValueFromContainer(&jb->root, ..., &vbuf);
if (v != NULL) RETURN JsonbValueToJsonb(v);
RETURN NULL;
```

`PG_GETARG_JSONB_P` (`src/include/utils/jsonb.h:487`) expands to
`DatumGetJsonbP` -> `PG_DETOAST_DATUM` -> `pg_detoast_datum`
(`src/include/fmgr.h:240`). `pg_detoast_datum` reads and
decompresses the entire varlena. The fork has the primitive
needed to avoid this (`detoast_attr_slice` in
`src/backend/access/common/detoast.c:371`,
`pg_detoast_datum_slice` in
`src/backend/utils/fmgr/fmgr.c:1823`) but nothing in
`src/backend/utils/adt/jsonb*.c` calls it.

`jsonb_object_field_text` (`jsonfuncs.c:940`) follows the same
shape and **does not** carry the Toastapi hook check — only
`jsonb_object_field` does. This is an existing asymmetry that
Layer 1 must close.

The other callers of `getKeyJsonValueFromContainer`:

  - `jsonb_util.c:401`: nested key lookup inside an in-memory
    iterator. Body is already in memory, no slicing needed.
  - `jsonb_util.c:1325`: deep-path traversal from
    `findJsonbValueFromContainer`. Body is in memory.
  - `jsonfuncs.c:1623`: deep path resolution. Body in memory.
  - `jsonfuncs.c:3548`: record column extraction. Body in memory.
  - `extended_stats_funcs.c:1137`: statistics-builder. Body in
    memory.

So the only sites where sliced read can change the work done are
**top-level external jsonb whose first key access happens via a
function-call boundary**. Those are `jsonb_object_field`,
`jsonb_object_field_text`, the subscript path
(`src/backend/utils/adt/jsonbsubs.c`), and the jsonpath single-key
case where the root is the external datum.

## 3. Why the cost is what it is

`PG_DETOAST_DATUM` on an external compressed body:

  - reads every chunk of the chain (B-tree lookups for the toast
    relation's chunk index, plus the chunk pages themselves);
  - decompresses every chunk into a contiguous buffer.

For a 247 kB jsonb body, the v1 dense matrix shows F0 reads ~5141
buffer pages and takes ~1.5 ms per call. JBTL's sliced reader
brings the same call to ~1053 buffer pages and ~0.009 ms (J00,
sort off; see Test 4 in `docs/PREFIX_LOCALITY_VS_RELOCATION.md`,
log-log slope of buffer-hits vs body-size is +0.000 — flat).

The cost reduction comes from **not** fetching most of the chunks.

## 4. Proposed sliced-read path

Add a single helper in `src/backend/utils/adt/jsonb_util.c`:

```
JsonbValue *
getKeyJsonValueFromExternal(Datum raw, const char *keyVal, int keyLen,
                            JsonbValue *res);
```

Contract (in prose, not C):

  - input `raw` is the un-detoasted column Datum (the caller has
    *not* yet called `PG_DETOAST_DATUM`);
  - if `raw` is not an external on-disk varlena, return NULL with
    a "not handled" out-flag set, so the caller falls through to
    the existing full-detoast path unchanged;
  - if `raw` is an external on-disk varlena, attempt sliced read
    against the toast chain;
  - on success return a `JsonbValue *` materialised into `res`
    (scalar only in v0; see §6);
  - on any fallback condition (compression layout that cannot be
    sliced, nested-container value, malformed prefix, value
    occupying more than ~half of body), return NULL with the
    "not handled" out-flag set so the caller does the
    existing-path detoast;
  - on hard data corruption (header_size disagrees with
    `sizeof(JsonbContainer)`-equivalent invariants, JEntry sums
    walk past `attrsize`) raise `ERRCODE_DATA_CORRUPTED`. No
    silent fallback for corruption — the body is on disk wrong.

The "not handled" out-flag is the same shape as the existing
`Toastapi_jsonb_object_field_hook` contract: `(bool *isnull,
Datum *result)` plus a return value meaning "I handled it"
vs "fall through". Concrete signature can match that shape so
the helper is callable from both the existing hook and a direct
in-core path.

## 5. Prefix fetch algorithm

Mirror the JBTL prototype's
`jbtl_toast_fetch_object_field` (`contrib/jsonb_toaster_lite/jsonb_toaster_lite_object_field.c:476-...`),
keeping the same stages but with no toaster-specific
wrapper unwrap. The toast pointer comes directly from the
on-disk `VARATT_EXTERNAL_ONDISK` datum.

Stage 1 — initial prefix fetch.

  Call `detoast_attr_slice(attr, 0, INITIAL_PREFIX)` with
  `INITIAL_PREFIX` = 1024 bytes (the JBTL constant
  `JBTL_OF_INITIAL_PREFIX_BYTES`, validated by the prototype's
  test suite). This covers the container header plus a few
  hundred JEntries plus a small key area for typical objects.

  Rationale for 1024: one TOAST chunk is 2032 bytes (default
  BLCKSZ); we want the prefix smaller than one chunk so we don't
  pay for two chunk fetches on the structural-only path. 1024
  fits comfortably in one chunk after compression accounting.

Stage 2 — parse container header.

  4 bytes. If `body_len < sizeof(uint32)`, raise CORRUPTED.

  Extract: container flags (`JB_FOBJECT`, `JB_FOBJECT_KVMAP`),
  count N. If not `JB_FOBJECT`, set "not handled" flag and return
  (the helper does not handle array or scalar roots in v0).

  If N == 0, return NULL (key not found). Fast-path completed
  without further fetches.

Stage 3 — compute minimum prefix bytes.

  Use the same formula as the writer at
  `jsonb_util.c:2110-2114`:

  ```
  min_prefix = sizeof(uint32) +              // header
               2 * N * sizeof(JEntry) +      // JEntries
               (has_kvmap ? INTALIGN(N * KVMAP_ENTRY_SIZE) : 0)
  ```

  This is the byte position where the data area begins. From the
  data area:

  ```
  key_area_end = min_prefix + sum_of_key_lengths
              = min_prefix + sum(getJsonbLength(jc, i) for i in 0..N)
  ```

  The reader needs `body_len >= key_area_end` to compare any key.

Stage 4 — extend prefix if needed.

  If `body_len < min_prefix`, refetch
  `detoast_attr_slice(attr, 0, min_prefix + 4096)`. The 4096
  margin is a heuristic for "and hopefully enough keys too".

  If after refetch `body_len < min_prefix`, raise CORRUPTED.

  If `body_len < key_area_end`, refetch
  `detoast_attr_slice(attr, 0, key_area_end)`. If still short,
  raise CORRUPTED.

Stage 5 — binary search for the key.

  The existing in-memory `getKeyJsonValueFromContainer` does this
  already (`jsonb_util.c:416`). Layer 1's helper can either:

  (a) inline the binary search loop into the helper (so the
      helper owns the prefix-extension decisions);
  (b) construct a synthetic `JsonbContainer*` pointing at the
      prefix and call the existing function.

  Option (a) is what JBTL does
  (`jbtl_find_key_in_object`). Option (b) is more refactor-heavy
  because the existing function assumes the body is complete in
  memory and may call `getJsonbLength` past the prefix.

  Recommended: option (a). Inline the binary-search loop in the
  helper; share with the existing function only the comparator
  (`lengthCompareJsonbString`, already non-static or made so).
  Keeps the prefix-bounded reads local to the helper.

  Result of stage 5: a `physical_value_idx` in [N, 2N) or
  "not found" (return NULL, fast-path complete).

## 6. Value fetch algorithm

Stage 6 — determine value extent.

  ```
  value_jentry        = jc->children[physical_value_idx];
  value_offset_in_data = getJsonbOffset(jc, physical_value_idx);
  value_len           = getJsonbLength(jc, physical_value_idx);
  ```

  These three calls all walk the JEntry array; the prefix already
  covers it.

Stage 7 — reject nested values.

  If `JBE_ISCONTAINER(value_jentry)`, set "not handled" flag and
  return. Layer 1 v0 returns scalars only. Nested-container
  retrieval is a future task; doing it via sliced read would
  require reassembling enough bytes to form a valid
  `JsonbContainer*`, including all of the child's JEntries, key
  area, value area — for typical nested objects that means
  fetching most of the body anyway. Not worth the complexity for
  v0.

Stage 8 — heuristic gate on value size.

  If `value_len * 2 > attrsize` (the value occupies more than half
  the body), set "not handled" flag and return. Sliced value
  fetch on a body that is mostly the requested value pays more
  than the full detoast (the value-fetch chunks overlap the
  prefix-fetch chunks; each overlapping chunk costs a duplicate
  B-tree walk).

  The 50% threshold matches the JBTL prototype
  (`jbtl_toast_fetch_object_field` step 7b). Tunable, default
  conservative.

Stage 9 — fetch the value bytes.

  Compute INTALIGN pad for numeric/container values:
  `pad = INTALIGN(value_offset_in_data) - value_offset_in_data`.

  Total fetch length: `fetch_len = value_len + pad`.
  Slice range: `[min_prefix + value_offset_in_data, ... + fetch_len)`.

  If `body_len >= min_prefix + value_offset_in_data + fetch_len`,
  the value is already in the prefix; use that.

  Otherwise call
  `detoast_attr_slice(attr, value_start_in_attr, fetch_len)`.

  **The slice offset is non-zero in general.** This is the
  compression interaction (§7).

Stage 10 — materialise the JsonbValue.

  Call the existing `fillJsonbValue` helper, passing
  `base_addr = (value_bytes - value_offset_in_data)` so its
  internal `offset` arithmetic still works. Or refactor
  `fillJsonbValue` to take an absolute pointer plus a JEntry; the
  function already knows how to dispatch by JEntry type. Pick
  the smaller refactor.

  Stage 10 produces the same `JsonbValue` that the existing
  full-detoast path would have produced. From here, the caller's
  existing code wraps it (e.g. `JsonbValueToJsonb` for
  `jsonb_object_field`, `JsonbValueAsText` for
  `jsonb_object_field_text`).

## 7. Compression and fallback rules

Core's `detoast_attr_slice` (`detoast.c:600-650`) supports two
shapes of slice:

  - **Uncompressed external**: arbitrary `[offset, len]` slices.
  - **Compressed external**: prefix slices only (`sliceoffset == 0`).
    There is an explicit
    `Assert(!VARATT_EXTERNAL_IS_COMPRESSED(toast_pointer) || 0 == sliceoffset);`
    at `detoast.c:605`. The asymmetry comes from the compression
    format: pglz and lz4 are streaming, so any prefix can be
    decoded from the start, but reading from byte 100k requires
    decoding everything up to byte 100k anyway.

This dictates the v0 policy:

| Body shape | Layer 1 behaviour |
|---|---|
| Inline (not external) | Set "not handled". The existing path runs unchanged; it never detoasts an inline datum anyway, so nothing is saved or lost. |
| External uncompressed (`attstorage = 'e'` or compressed-not-worthwhile) | Layer 1 fully active. Stage 4 prefix fetch, stage 9 arbitrary-range value fetch. |
| External compressed | Layer 1 active **only when** the value falls inside the structural prefix that was already fetched in stages 1-4. Concretely: if `min_prefix + value_offset_in_data + fetch_len <= body_len` after stage 4's extension, stage 9 does not call `detoast_attr_slice` again, and the value bytes come from the prefix. If the value lies past `body_len`, set "not handled" and fall through to full detoast. |

The compressed-external policy is the conservative form. It
delivers the layer-1 win on values that the writer has placed in
the structural prefix — which is exactly what the existing
`jsonb_sort_field_values` mechanism does (see §8). Values placed
later in the body (i.e. larger values, which by sort policy live
at the end) force a fallback to full detoast.

**Optimisation deferred to a later task:** sliced decompression of
the structural prefix only, followed by a separate sliced
decompression for the value extent. Both are prefix slices in
their own compressed sub-stream and could both be served by
`detoast_attr_slice` if a more granular core API existed. This
task does not propose such an API change.

Full fallback table:

| Condition | Behaviour |
|---|---|
| Datum not external | "not handled" — existing path |
| Root not an object | "not handled" — existing path |
| `N == 0` | return NULL (key not found) |
| Key not in object | return NULL (key not found) |
| Value is container | "not handled" — existing path |
| `value_len * 2 > attrsize` | "not handled" — existing path |
| External compressed, value past prefix | "not handled" — existing path |
| External uncompressed, value past prefix | active — slice fetch for value |
| Prefix walk parses inconsistent JEntry sums or out-of-range offsets | `ERRCODE_DATA_CORRUPTED` (no silent fallback) |
| Refetched prefix still cannot cover header / KVMap / key area | `ERRCODE_DATA_CORRUPTED` |

The corruption raises are matched by what the slow
detoast+`getKeyJsonValueFromContainer` would have produced — that
path raises `ERRCODE_DATA_CORRUPTED` in the same situations
(via its own bounds-checks on JEntry walking). Layer 1 does not
add new error classes; it raises the same ones earlier.

## 8. Interaction with `jsonb_sort_field_values`

The v1 dense matrix evidence
(`docs/PREFIX_LOCALITY_VS_RELOCATION.md` §5a, Tests 5 and 6,
synthesis table) reads as a factorial design over three binary
factors: sort × sliced × relocation. The Layer 1 cells are J00
(sliced, sort off) and J10 (sliced, sort on); both no relocation.

| | sort off | sort on |
|---|---|---|
| no slice (today) | F0: 5143 pages, 1.5 ms | F1: 5143 pages, 1.5 ms |
| sliced | J00: **1053 pages, 0.009 ms** | J10: **603 pages, 0.006 ms** |

Reading the deltas:

  - F1 - F0: sort alone does not help reads (Test 3). KVMap is
    written and unused by the existing reader.
  - J00 - F0: sliced read alone gives the **asymptotic** shape
    change (O(body) -> O(1) buffer reads).
  - J10 - J00: with sliced read active, sort improves the
    constant by ~1.75x on buffer reads, ~1.5x on latency.
    Mechanism: when KVMap is present, small values sit at the
    start of the value area, so on a compressed body more values
    fall inside the structural prefix and stage 9 does not need
    to refetch.

Implication for Layer 1:

  - Layer 1's **shape** (O(1) reads) does not depend on
    `jsonb_sort_field_values`. Layer 1 delivers the asymptotic
    win regardless of sort.
  - Layer 1's **constant** depends on sort. With sort off, Layer
    1 reads ~1.75x more buffer pages on the headline case than
    with sort on.

Two policy options exist, and **this task picks neither**:

  (i) Flip the default of `jsonb_sort_field_values` to `true`.
      Affects every newly written jsonb row with mixed value
      sizes. Compatibility/blast-radius decision; separate task.
  (ii) Make sort policy-driven inside the writer (e.g. always on
      for rows whose object pairs cross some size ratio). Smaller
      blast radius, still its own task because it changes
      on-disk bytes for some rows.

Layer 1 ships **independently of either decision**. It works
correctly with sort off (J00 numbers) and benefits from sort on
(J10 numbers). The choice of whether to flip or to add policy is
the next step after Layer 1, and is sensitive to whether Layer 2
ships at the same time (because relocation makes the sort
contribution disappear; see Section 5b of the prefix-vs-relocation
doc).

## 9. Integration points

Three primary integration sites in production code:

### 9.1 `jsonb_object_field` (`jsonfuncs.c:866`)

Already has a hook gate for CUSTOM varlenas
(`Toastapi_jsonb_object_field_hook`). For Layer 1 the parallel
gate is on `VARATT_IS_EXTERNAL_ONDISK`. Sketch of where the
helper goes:

```
jsonb_object_field(args):
  raw = PG_GETARG_DATUM(0);

  # existing hook for CUSTOM varlenas (toaster extensions)
  if Toastapi_jsonb_object_field_hook && VARATT_IS_CUSTOM(raw):
      ... existing dispatch ...

  # NEW: layer-1 sliced read for plain externals
  if VARATT_IS_EXTERNAL_ONDISK(raw):
      v = getKeyJsonValueFromExternal(raw, key_bytes, key_len, &vbuf);
      if handled:
          if v == NULL: PG_RETURN_NULL;
          PG_RETURN_JSONB_P(JsonbValueToJsonb(v));
      # else fall through to existing full-detoast path

  # existing full path
  jb = PG_GETARG_JSONB_P(0);
  ...
```

### 9.2 `jsonb_object_field_text` (`jsonfuncs.c:940`)

Symmetric. Currently lacks the Toastapi hook entirely — that is
an existing asymmetry independent of Layer 1, but the Layer 1
patch should fix it: every site that does `jb -> 'key'` or
`jb ->> 'key'` deserves the same fast path. After the helper
returns, the text variant wraps the result with
`JsonbValueAsText` instead of `JsonbValueToJsonb`.

### 9.3 `getKeyJsonValueFromContainer` callers that have an external datum

The other callers (deep-path traversal, jsonpath, record
extraction, statistics) operate on a `JsonbContainer *` that is
already in memory. They do not have an external datum at hand.
Layer 1 cannot help them directly. Two paths exist for future
tasks:

  - Top-level jsonpath key access where the path's first step is
    a simple key on the root datum: the planner / executor could
    route to the helper before detoasting. Out of scope here.
  - Subscripting (`jsonbsubs.c`): the subscript executor does
    detoast before subscripting today. Same story as jsonpath:
    needs its own task.

This task scopes Layer 1 to 9.1 and 9.2. The follow-ons are
mentioned for completeness; they are not the design boundary
here.

### 9.4 Helper placement

The helper lives in `src/backend/utils/adt/jsonb_util.c`,
alongside `getKeyJsonValueFromContainer`. It is exported in
`src/include/utils/jsonb.h` so `jsonfuncs.c` can call it.
`jsonfuncs.c` does not need to know about TOAST internals beyond
passing `raw` through.

The helper itself uses `detoast_attr_slice` directly. No new
core TOAST API is needed.

## 10. Acceptance measurements

Three measurements gate the patch.

### 10.1 Inline-row microbenchmark

Run a tight loop of `SELECT jb -> 'k' FROM t` on a table whose
rows are all inline jsonb (body well below
`TOAST_TUPLE_THRESHOLD = 2032 bytes`). Compare before/after Layer
1.

Acceptance: indistinguishable from baseline within run-to-run
noise. The helper's `VARATT_IS_EXTERNAL_ONDISK` gate runs on
every call; it must cost a single inline check and nothing more.
Concretely: median latency change <= 5%, p99 latency change
<= 10%, buffer reads identical.

This measurement matches risk R-HYB-1 from the prefix-vs-relocation
doc.

### 10.2 Sliced-read P0 cell against the v1 dense matrix

Build a "P0" cell (production Layer 1 patch applied, no toaster
extension bound, no relocation) and run the same workload as the
v1 dense matrix (15 ids, 4 keys, S1+MM3, 150 reps x 5 runs).

Acceptance is **case-dependent** because the spec's §7
compression policy routes different cases through different
paths. Splitting into the three cases the helper actually
distinguishes:

**Case A — prefix-resident scalar (sort on, or naturally short
value, or short body where the structural prefix already covers
the value).** Helper serves from prefix; no second fetch.

  Acceptance: P0 reproduces J0 numbers within counter noise.
  Both production Layer 1 and JBTL-A read the value directly
  from the structural prefix bytes; the dispatch overhead of
  JBTL-A's extension wrapper is the only systematic difference,
  so P0 may be slightly better than J0 (fewer buffer pages per
  call). Concretely, on the v1 matrix with sort=on at id=100
  key3, expect P10 ≤ J10 on buffer count.

**Case B — uncompressed external, value past prefix.** Helper
issues a second `detoast_attr_slice` at the value's byte offset
to fetch only the value's bytes.

  Acceptance: P0 reproduces J0 numbers within counter noise.
  Both reach the value through the same mechanism: an arbitrary-
  range slice into uncompressed TOAST. JBTL's chunked dispatch
  and production's vanilla `detoast_attr_slice` walk the same
  toast index; per-call overhead is comparable.

**Case C — compressed external, value past prefix.** Helper
gives up via the §7 fallback (compressed external supports
prefix slices only). Caller does the existing full detoast.

  Acceptance: P0 reproduces **F0** within counter noise, NOT J0.
  JBTL reaches J0 in this case via its own chunked-compression
  format (per-chunk decompression boundaries), which is a
  property of the JBTL storage wrapper, not a property of
  sliced reads in general. Production Layer 1 does not have a
  comparable mechanism; closing this gap requires a TOAST API
  change that the spec deferred (see §7 "Optimisation deferred
  to a later task"). Equality with F0 is the correct acceptance
  here; equality with J0 is not.

What this measurement validates:
  - asymptotic shape change at Case A and Case B (O(body) → O(1));
  - clean fallback at Case C (no regression vs baseline);
  - cold-payload reads (key2, key4) match F0 within noise — the
    helper's "not handled" gate must fire on container values
    and on values exceeding the half-body heuristic.

The split above replaces the earlier wording "P0 reproduces
J00's profile within counter noise" which was implicitly
assuming the chunked-compression API; that assumption did not
match the rest of the spec.

### 10.3 Mid-size buffer regression boundary

Specifically measure the col_size range 2.6 kB to 25 kB on
key3, both with sort off and sort on. Document the crossover
point: the size above which sliced read wins on buffers.

This is the empirical input for any future decision about a
size threshold below which Layer 1 should not engage (Section 7
mentions this as a deferred optimisation). The crossover seen
in the JBTL data:

  sort off: crossover at ~44 kB (J00 vs F0 buffer ratio crosses 1.0)
  sort on:  crossover earlier (J10 already 603 vs F0 493 at 2.6 kB)

Acceptance criterion: P0 reproduces the JBTL J00 crossover
within +/- 10% on buffer pages.

### 10.4 Compressed vs uncompressed external

Build two variants of the same large jsonb body: one with
default `attstorage = 'extended'` (compressed external), one
with `attstorage = 'external'` (uncompressed external). For each,
read `jb -> 'small_key_at_end'` (a key whose value sits past the
structural prefix).

Acceptance:

  - compressed variant: helper sets "not handled" and falls
    through. Performance equals F0. **No regression**.
  - uncompressed variant: helper actively slice-fetches the value.
    Performance shows the Layer 1 wins.

This validates the §7 compression policy.

## 11. What remains for relocation layer

After Layer 1 ships, the production read path can:

  - return any scalar key value from any non-compressed
    external jsonb body at O(1) buffer cost;
  - return scalars that sit in the structural prefix of a
    compressed external (with `jsonb_sort_field_values = on`,
    this is most short scalars on large bodies);
  - fall back cleanly to full detoast for everything else.

What Layer 1 still cannot do:

  - return scalars that sit late in a compressed external body
    cheaply;
  - shrink the writer-side memory footprint of jsonb
    materialisation (the writer still assembles the whole body
    before passing to TOAST);
  - avoid touching all N keys' worth of JEntries on a key lookup
    (the prefix has to cover the full JEntry array; that is N
    times sizeof(JEntry) bytes, ~4N);
  - serve nested-container values from a slice (Layer 1 v0 falls
    back).

Layer 2 (relocation) addresses three of these:

  - large cold values are moved out of the parent body into
    their own TOAST chains; the parent body shrinks to header +
    JEntries + descriptors. The compressed-external limitation
    from §7 stops mattering because the parent body is small
    enough to be fully decompressed without overhead.
  - the writer no longer assembles the whole body in one piece
    for relocation-eligible rows.
  - the JEntry array of the parent is the same size as before
    (Layer 2 does not reduce N), but the **bytes scanned during
    the prefix walk** drop because the data area shrinks.

Layer 1 does not preempt Layer 2. Layer 1 is the read primitive
that Layer 2 also uses for its parent body. Layer 2 simply
makes the parent body small.

The relocation descriptor format, JEntry tag reservation, and
delete-side walk are out of scope here, designed in
`docs/PRODUCTION_RELOCATION_SOURCE_MAPPING.md` and
`docs/RELOCATION_DELETE_OWNERSHIP.md`. Once Layer 1 lands, Layer
2 reduces to: "in the helper above, recognise a new JEntry type
tag in stage 5/6 and route to descriptor-driven child fetch
instead of inline value bytes."

## Verdict

**SLICED_READ_SPEC_READY.**

The pieces are all in place:

  - production has the TOAST primitive (`detoast_attr_slice`);
  - the integration point exists and is already used by one
    extension hook (`Toastapi_jsonb_object_field_hook`);
  - the algorithm is the JBTL prototype's, validated by the v1
    dense matrix (Tests 4, 5, 6);
  - the fallback rules are derivable from
    `detoast_attr_slice`'s own constraints (compressed external,
    prefix-only) plus a simple value-size heuristic;
  - the helper has a clean signature with one caller site per
    operator;
  - no new on-disk format;
  - no new JEntry type tag;
  - no delete-ownership work;
  - no change to `jsonb_sort_field_values` default.

The verdict is not `NEEDS_JSONB_API_REFACTOR` because the
refactor scope is local: one new function in `jsonb_util.c`,
its prototype in `jsonb.h`, two call-site changes in
`jsonfuncs.c`. The existing `getKeyJsonValueFromContainer` does
not need to change.

The verdict is not `NEEDS_TOAST_API_CHANGE` because
`detoast_attr_slice` is sufficient as-is. The deferred
optimisation in §7 (sliced decompression of a value past the
prefix on compressed externals) would need a TOAST API change,
but it is explicitly out of scope for Layer 1 v0.

The verdict is not `NOT_WORTH_LAYER1` because Test 4 proves the
asymptotic shape change happens at Layer 1, and the mid-size
regression in Test 2 is addressable by either gating the helper
on body size or by enabling sort (Section 8). Even at the
1053-page constant (sort off, large body), Layer 1 reduces work
by ~5x against F0 on the headline case.

## Open questions to settle when writing the patch

  - **OQ-1 — exact INITIAL_PREFIX value.** JBTL uses 1024 (one
    chunk minus margin). Confirm this against PG's default
    `TOAST_MAX_CHUNK_SIZE` derivation. The value should be
    "smaller than one chunk after compression accounting" — not
    a guess.
  - **OQ-2 — corruption raise vs fallback for malformed prefix.**
    The §7 table says raise CORRUPTED on JEntry-walk
    inconsistency. Confirm by reading what the slow path does on
    the same input. If the slow path silently returns NULL on
    some classes of malformed input, Layer 1 must match.
  - **OQ-3 — does subscripting (`jsonbsubs.c`) call through the
    helper or detoast first?** Out of scope for this design but
    affects whether subscript-heavy workloads benefit from
    Layer 1 without a separate task.
  - **OQ-4 — `fillJsonbValue` reuse vs duplication.** The helper
    needs the same scalar-construction logic. Sharing the
    existing function vs inlining is a code-organisation
    decision, not a design one.

None of these block the spec. They are scoped for the patch
author.

## Next step

The next task, after this design is reviewed, is one of:

  A) Apply the patch series:
       1. `jsonb: getKeyJsonValueFromExternal helper (no behavior change)`
       2. `jsonb: route jsonb_object_field through getKeyJsonValueFromExternal`
       3. `jsonb: route jsonb_object_field_text similarly`
       4. `jsonb: regression and microbenchmark`
     With acceptance evidence per Section 10.

  B) Specify the sort-default decision separately. This is the
     other half of the Layer 1 story (Section 8) and is
     independent of (A).

  C) Begin Layer 2 relocation spec work (descriptor v1 byte
     layout, writer-side policy). Layer 2 reuses Layer 1 as its
     parent-body reader, so doing (A) first sharpens (C)'s
     acceptance criteria.

Recommended order: A first.

No code, no benchmark run, no relocation design in this task.
