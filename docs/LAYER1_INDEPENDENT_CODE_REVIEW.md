# Independent code review — Layer 1 sliced jsonb read

Author of review: @r2d2, wearing the upstream-reviewer hat
                  (imagining a typical pgsql-hackers reviewer
                  reading this for the first time, with no
                  prior context from the design docs).

Code under review: commit 28c8b7fa5c, plus the JBTL audit in
                   079e0b382d.

**Status update (post fix-cycle):**
  - D1 — **fixed** in commit b4a8f043df (header-doc contract).
  - D2 — **fixed** in commit b4a8f043df (enum signature).
  - D3 — **fixed** in commit b088abdafd (regression suite).
  - S2 — **fixed** in commit b4a8f043df (StaticAssertDecl).
  - S3 — **partially fixed** in commit b4a8f043df (the void-cast
    idiom was removed as a fall-out of the D1+D2 cleanup; the
    memory-ownership note was relocated above the allocation
    site as recommended).
  - S1, M1-M7 — **deferred** to a follow-up cleanup commit.

Scope: code only. Documentation review is a separate exercise
       and the docs read well — that does not relax code
       standards.

Reviewer disposition: the patch achieves a real win on a real
                      problem (full-detoast on every key access
                      is wasteful), and the change is
                      mechanically small (+466 LOC, one new
                      exported helper, two integration sites).
                      Most of my comments below are not
                      objections to the design but tightening
                      pressure on the implementation. Three of
                      them are real defects.


## Defects worth fixing before merge

### D1. Memory ownership of `prefix` and `value_slice` is not documented in the exported contract

`getKeyJsonValueFromExternal` retains `prefix` and `value_slice`
(palloc'd by `detoast_attr_slice`) on the success path. The
comment at jsonb_util.c:858-872 explains why locally:

> fillJsonbValue may store a pointer into synth_base for
> jbvString (no copy) and into INTALIGN'd numerics. Both live in
> our slice memory.

This is true. But the caller has no way to know it from the
prototype in jsonb.h (line 501-504):

```
extern JsonbValue *getKeyJsonValueFromExternal(Datum raw,
                                                const char *keyVal,
                                                int keyLen,
                                                JsonbValue *res,
                                                bool *out_handled);
```

Anyone who reads only the header sees a function returning a
`JsonbValue *` and would reasonably assume `res` is
self-contained. It is not: `res->val.string.val` points into
heap-allocated memory the caller must keep alive until
`JsonbValueToJsonb` or `JsonbValueAsText` copies it out.

The existing callers do that immediately and the contract is
satisfied. But the contract is implicit. A subscripting integration
(which the follow-up plan promises) that stages the result
across an expression-evaluation boundary would break it
silently.

**Action**: extend the header documentation to state the
contract explicitly. Possible wording:

```
/*
 * On a found scalar (out_handled = true, return != NULL), the
 * returned JsonbValue's string / numeric pointer may reference
 * memory owned by an internal TOAST-slice buffer that lives in
 * CurrentMemoryContext until the next reset. The caller must
 * materialise (JsonbValueToJsonb / JsonbValueAsText) before
 * any operation that switches or resets the context.
 */
```

This is a documentation-only fix but it touches the exported
contract. Worth doing in the same patch.


### D2. Tri-state `(JsonbValue *result, bool out_handled)` invites caller bugs

The current contract has three states:

  | return value | out_handled | meaning |
  |--------------|-------------|---------|
  | non-NULL     | true        | scalar found, materialised in *res |
  | NULL         | true        | key proven missing, caller returns NULL |
  | NULL         | false       | not handled, caller falls back |

The callers correctly check `handled` first
(jsonfuncs.c:928, :1003). But a future caller (or a
post-rebase mis-merge) that writes:

```
if (res != NULL) { /* use it */ } else { /* fall back */ }
```

silently does the wrong thing for the "proven missing" case
— it falls back to the full detoast path, which itself
returns NULL, so the user-visible behaviour is correct but
the optimisation is silently disabled and the full body is
detoasted needlessly.

This is exactly the failure mode of bug 1 (silent self-disable
of the helper) replayed at the caller boundary.

**Action**: change the API to a single enum return value, e.g.

```
typedef enum {
    JSONB_KEY_LOOKUP_FOUND,        /* result in *res */
    JSONB_KEY_LOOKUP_MISSING,      /* definitively missing, no fallback */
    JSONB_KEY_LOOKUP_FALLBACK      /* helper cannot answer; fall back */
} JsonbKeyLookupResult;

extern JsonbKeyLookupResult getKeyJsonValueFromExternal(
    Datum raw, const char *keyVal, int keyLen, JsonbValue *res);
```

This eliminates the silent-disable failure mode at compile
time (caller must handle each enum value or compiler warns).
Touches three call sites and the helper signature. Mechanical
refactor; ~20 lines net.

Some reviewers may prefer the existing shape as "matches the
Toastapi hook pattern." The Toastapi hook pattern uses
`(bool *isnull, Datum *result) -> bool handled`, which has the
same fragility. Layer 1 is not bound to inherit it; the enum
is strictly safer and was already mentioned as an option in
spec §4 ("the helper signature can match that shape" — note
the *can*, not *must*).


### D3. No regression test specifically exercising the helper

The patch adds zero new SQL tests. Justification given:
existing jsonb regression suite already covers every code
path through the existing readers. This is true — but every
path through the **existing** readers, not through the new
helper.

Concretely, the existing suite does not include:

  - a row whose body is forced external compressed (most
    test fixtures are inline);
  - a numeric scalar value placed near the body end (the
    bug-2 reproduction case);
  - explicit verification that the helper's "not handled"
    fallback returns the same result as the slow path on
    nested-container values, missing keys, half-body
    heuristic triggers.

Bug 1 (silent disable) and bug 2 (cold-cache CORRUPTED) were
both caught by ad-hoc fixtures (`p1_iso` table), not by the
regression suite. They would have lived undetected through
the suite as it stands.

**Action**: add a `jsonb_layer1.sql` (or extend
`jsonb_kvmap.sql`) with the following cases, each verifying
result equality between the helper-active path and a
helper-disabled comparison (e.g. through
`pg_typeof(jb)::text || '::jsonb -> ...'` to force redetoast):

  - inline body, key present → result identical;
  - inline body, key absent → NULL;
  - external uncompressed, key in prefix → result identical;
  - external uncompressed, key past prefix → result identical;
  - external compressed, key in prefix → result identical;
  - external compressed, key past prefix → result identical
    (this is the case where Layer 1 falls back; ensures the
    fall-through path is correct);
  - external, value is nested container → returned as jsonb,
    identical to slow path;
  - external, value > half body → identical to slow path;
  - external, key is non-existent → NULL;
  - external, numeric value 42 at body end → 42 returned
    correctly (bug 2 regression coverage);
  - corrupted external (synthesised via raw bytea cast) → the
    error message matches what the slow path raises.

Minimum cost: ~30 lines of SQL, ~15 lines of expected output.
This is not a nice-to-have; it is the difference between "we
believe it works" and "we have automated proof it works in
the regression matrix."


## Significant concerns

### S1. Double-refetch when prefix < key_area_end

Stages 3 and 4 (jsonb_util.c:644-711) refetch the prefix slice
in two separate passes:

  - Stage 3 (line 647-668): if `body_len < min_prefix`, refetch
    `min_prefix + 4096`.
  - Stage 4 (line 690-711): if `body_len < key_area_end`,
    refetch `key_area_end`.

For an object with many keys (e.g. 500 keys of ~50 bytes each
→ key_area = ~25 KB), stage 3 refetches to `min_prefix + 4096`
(say ~10 KB), then stage 4 refetches to 25 KB. Two
`detoast_attr_slice` calls, two `pfree(prefix)` plus two
re-palloc's, two TOAST-chunk-index walks.

The 4096 margin in stage 3 is mostly wasted: for large objects
it does not avoid stage 4, and for small objects stage 4 would
not fire anyway.

**Action**: compute `min_prefix` and `key_area_end` together
upfront (we have N from the header, so JEntries are needed for
both; once stage 3 fetches enough for the JEntry array, we
can compute key_area_end without a second pass). Single
refetch covers both. Reduces worst case from two refetches to
one.

Net change: ~20 line restructure of stages 3-4. No behavior
change in terms of correctness; measurable buffer-page
reduction in the large-N case.


### S2. `JSONB_SLICED_READ_INITIAL_PREFIX = 1024` is unjustified by code

Defined inline at jsonb_util.c:528. Comment says "smaller than
one TOAST chunk." That is true today: `TOAST_MAX_CHUNK_SIZE`
is BLCKSZ-dependent and is ~2032 on default-build 8KB pages.

But:

  - if BLCKSZ is non-default (some shops build with 16K or 32K
    pages), TOAST_MAX_CHUNK_SIZE grows and 1024 becomes
    pessimistic;
  - no `StaticAssertStmt` guards against this drifting in the
    other direction (someone shrinks chunk size, 1024 becomes
    wasteful);
  - 1024 is also the JBTL prototype's value, but that lineage
    is not documented.

**Action**: either derive the constant:

```
#define JSONB_SLICED_READ_INITIAL_PREFIX \
    Min(1024, TOAST_MAX_CHUNK_SIZE - 64)
```

or assert that the chosen value is sane:

```
StaticAssertStmt(JSONB_SLICED_READ_INITIAL_PREFIX
                 < TOAST_MAX_CHUNK_SIZE,
                 "initial slice must fit in one TOAST chunk");
```

The second is cheaper. Either makes the constraint visible.


### S3. `(void) value_slice; (void) prefix;` casts (line 871-872) are an unusual idiom

The code intentionally retains the slice buffers so
fillJsonbValue's pointers into them stay valid. To silence
"unused variable" warnings on the kept pointers, the function
uses `(void) X;` casts. This is correct but rare in PG.

A reviewer skimming the code might misread these as "OK to
discard." The comment above them (lines 858-871) is correct
but a few lines away.

**Action**: either drop the casts entirely (compilers don't
warn — the variables ARE used: `value_slice` was assigned from
`detoast_attr_slice`, `prefix` was used in arithmetic just
above), or replace with an `Assert(value_slice == NULL ||
VARSIZE(value_slice) > 0)` that has a clearer purpose. The
cleanest fix: move the comment right above the relevant
allocations and drop the `(void) X;` lines.


## Smaller items

### M1. `int N` (line 543) vs `uint32` returned by `JsonContainerSize`

`JsonContainerSize` returns `uint32`. The variable holds it as
`int`. Implicit cast. For values up to `JB_CMASK` (28-bit) the
cast is benign; for theoretically adversarial bodies with N
between `INT_MAX` and `JB_CMASK_MAX` the comparison
`stopLow < stopHigh` could behave unexpectedly. Defense in
depth: declare `uint32 N` and adjust loop var types.

Same for `physical_value_idx`. Declared `int` (line 555); used
in arithmetic with `uint32` results from
`JSONB_KVMAP_ENTRY()`. Implicit cast OK on a 64-bit platform;
explicit type would be cleaner.

### M2. `physical_value_idx` may be uninitialized on the no-found-key path

Declared at line 555, written only inside the `if (difference
== 0)` branch (line 740), used only after `goto found` (line
759). The goto guarantees it is written before use; static
analysis tools should track this. Some compilers with
`-Wmaybe-uninitialized` still warn. PG builds at -Wall and
some include this flag.

**Action**: initialise `int physical_value_idx = -1;`
defensively. One token; eliminates the warning.

### M3. `PG_GETARG_TEXT_PP(1)` called twice if Layer 1 falls through

In `jsonb_object_field`:
- line 919: `hkey = PG_GETARG_TEXT_PP(1);` (inside Layer 1 block)
- line 939: `key = PG_GETARG_TEXT_PP(1);` (after fall-through)

`PG_GETARG_TEXT_PP` is `PG_DETOAST_DATUM_PACKED` which detoasts
the key text if needed. Keys are usually small inline, so the
second call returns the cached pointer without rework. Not a
performance defect, but lifting the variable out of the
block-scope would be cleaner:

```c
text *key = PG_GETARG_TEXT_PP(1);
Datum raw = PG_GETARG_DATUM(0);
/* ... gate using `raw`, helper using `key` ... */
```

This also moves the Datum declaration to function top, which
is closer to PG style.

### M4. `jsonb_object_field_text` lacks the Toastapi extension hook

Line 984-987 explicitly documents this:

> Note: this function does NOT currently have the
> Toastapi_jsonb_object_field_hook gate; that asymmetry
> pre-dates Layer 1 and is left untouched here.

This is honest disclosure. A reviewer will still ask: "why
not close the gap while you are here?" Answer in the comment
is defensible (separation of concerns, smaller diff) but
the next reviewer down the line will likely require closure.

**Action** (optional, recommended): add a parallel hook gate
for `->>` in this same patch. The Toastapi hook ABI for the
text variant might not exist yet; if so, defer with a clear
note. If it does exist, two lines closes the asymmetry.

### M5. The "AUDIT —" header in the JBTL comment (commit 079e0b382d)

The audit comment in
`contrib/jsonb_toaster_lite/jsonb_toaster_lite_object_field.c`
starts with `AUDIT — latent over-fetch on numerics, harmless
here.` PG convention is regular prose comments. The all-caps
tag is unusual.

Minor stylistic; the substance of the comment is valuable.
Reviewers might suggest rewording to standard prose without
losing the tracking-tag function.

### M6. No `Assert(JsonContainerIsObject(jc))` after stage 2

At jsonb_util.c:611 we check `if (!JsonContainerIsObject(jc))`
and return NULL. After that line, every other code path
operates on `jc` as an object container. Defensive `Assert` at
the start of stage 3 would document the invariant. Cheap.

### M7. `body_size` casts have small redundancy

Line 578: `body_size = (int32) toast_pointer.va_rawsize - (int32) VARHDRSZ;`

`va_rawsize` is `int32` already, `VARHDRSZ` is `4`. Both casts
are redundant. PG style would write:

```
body_size = toast_pointer.va_rawsize - VARHDRSZ;
```

Defensive: a `body_size > 0` check after the subtraction
guards against a corrupted toast pointer with garbage
va_rawsize. We already have `body_size < (int32) sizeof(uint32)`
at line 582 which implicitly covers this.


## Non-issues (verified during review)

  - **Locale / collation safety**: `lengthCompareJsonbString`
    is a binary memcmp; no locale state. Helper inherits this
    correctly.
  - **Parallel safety**: helper is read-only, no shared state,
    allocates in CurrentMemoryContext. Parallel-safe by
    inheritance from existing readers.
  - **lz4 vs pglz**: `VARATT_EXTERNAL_IS_COMPRESSED` covers
    both. Fallback rule applies uniformly. Good.
  - **KVMap alignment**: writer at jsonb_util.c:2505 uses
    `INTALIGN(kvmap_entry_size * nPairs)`; reader at
    jsonb_util.c:629 uses `INTALIGN(N * kvmap_entry_size)`.
    Identical (commutative). Good.
  - **Container scalar root rejection**: stage 2 rejects
    non-object roots cleanly (returns NULL with handled=false).
    Existing path then handles array / scalar roots correctly.
    Good.
  - **Half-body heuristic** at line 775: `(int64) value_len * 2
    > (int64) body_size`. int64 cast guards against overflow
    for body_size near INT_MAX. Defensive and correct.
  - **`detoast_attr_slice` error propagation**: any ereport
    from the slice fetch unwinds through the caller's per-tuple
    memory context. PG idiom. Correct.


## What I could not review

A self-review by the implementor cannot fully substitute for
an outside pair of eyes. Specifically, I could not:

  - judge whether the helper's algorithmic structure is
    idiomatic against the rest of jsonb_util.c (I wrote it;
    style biases match the file by construction);
  - assess the documentation as a first-time reader (I wrote
    those too);
  - catch issues that arise from reading the code without the
    spec in hand (I always read both together).

These limits are real. The defect list above is the strongest
I could push myself to produce; an outside reviewer will
likely find at least one item I missed.


## Recommendation

**Conditionally accept** with the following fixes required
before merge:

  - **D1**: document memory ownership in the exported header;
  - **D2**: replace tri-state contract with a clear enum;
  - **D3**: add regression tests for the new code paths.

Recommended but not blocking:

  - **S1**: collapse stages 3+4 to a single refetch;
  - **S2**: derive or assert the 1024 constant;
  - **S3**: clean up the `(void) X;` idiom.

Nits **M1-M7** can land in a single follow-up cleanup commit
or be folded into the fixes for D1-D3.

If D1-D3 are addressed, the patch is mergeable. Layer 1 solves
a real problem at low cost; the implementation is mechanically
correct; the design (three-case acceptance) is honest about
its boundaries.

— @r2d2 (independent-reviewer hat)
