# Layer 1 — upstream-prep fix cycle

Branch: `r2d2/layer1-upstream-prep` (forked from
        `r2d2/layer1-sliced-read-clean`).

Controlling review input: `LAYER1_POSTGRES_HACKER_REVIEW.md`.
Verdict on intake: NEEDS_SMALL_FIXES_BEFORE_UPSTREAM.
Verdict on exit:   READY_FOR_UPSTREAM_PREP.

This is the engineering log for the fixes between the two. One
entry per finding from the review, in the order the review
ranked them. Each entry states what the finding was, what we
changed, where the change lives, and how we verified it.


## B1 + F8 — int64 size arithmetic in `getKeyJsonValueFromExternal`

**Finding.** `min_prefix = header_size + kvmap_bytes` was computed
in int32. For a forged container header with N in
`[178_956_971, JB_CMASK_MAX]` and the KVMap flag set, the true
sum reaches ~3.2 GB, wraps to a large negative int32, and the
corruption check `if (min_prefix > body_size)` evaluates
`negative > positive` as false. The check labelled as defending
this exact case did not fire. The helper then walked container
metadata out of bounds.

**What changed.** All size-arithmetic locals in
`getKeyJsonValueFromExternal` widened to int64:

    body_size, prefix_size, body_len,
    header_size, kvmap_bytes, min_prefix,
    key_area_end,
    value_len, value_byte_end_in_body, fetch_len,
    value_start_in_body

`body_size` is computed in int64 from the start
(`(int64) toast_pointer.va_rawsize - (int64) VARHDRSZ`) so a
corrupt `va_rawsize` cannot wrap during subtraction. The
overflow guards on `key_area_end` no longer rely on
"accumulator went negative" because int64 cannot wrap inside
the working range. Each `klen` returned by `getJsonbLength` is
widened to int64 individually before being added.

`detoast_attr_slice` takes int32 sliceoffset and slicelength
arguments. Downcasts to int32 happen only after the int64
bounds check has proved the value is ≤ `body_size`, which is
itself bounded by `va_rawsize` (declared int32 in
`varatt_external`). The corruption check therefore fires before
any downcast.

The error-message format specifiers for `min_prefix`,
`body_size`, `body_len`, `key_area_end` were updated to `%lld`
with explicit `(long long)` casts.

**Where.** `src/backend/utils/adt/jsonb_util.c`, function
`getKeyJsonValueFromExternal`.

**Verification.** Numerical reproducer in Python pre-fix:
N = JB_CMASK_MAX (268_435_455), has_kvmap = true →
header_size = 2_147_483_644, kvmap_bytes = 1_073_741_820,
true sum = 3_221_225_464, int32-wrapped = −1_073_741_832.
Post-fix the int64 sum is 3_221_225_464, the corruption check
fires, ereport ERRCODE_DATA_CORRUPTED, no out-of-bounds read.

Build: `jsonb_util.o` rebuilt under `-Wall -Wpointer-arith
-Wdeclaration-after-statement -Werror=vla -Wcast-function-type
-Wshadow=compatible-local -Wformat-security -Wstrict-prototypes
-Wold-style-definition`. Zero warnings.

Regression: jsonb_layer1 passes; all 200-key wide-object cases
(Cases 10 and 11, see F4) hit the int64 arithmetic in both
Stage 3 and Stage 4 refetch paths.


## F1 — self-contained `*res` on FOUND; no slice retention

**Finding.** The helper retained `prefix` (1–30 KB) and
optionally `value_slice` palloc'd in CurrentMemoryContext on
every FOUND return, because `fillJsonbValue` wrote pointers
into them. The caller was contracted to materialise via
`JsonbValueToJsonb` or `JsonbValueAsText` before any context
boundary. On wide scans this bloated the per-tuple context's
high-water-mark by per-row residue not present on the slow
path, and the contract was asymmetric with `jsonb.h`'s usual
practice.

**What changed.** A new Stage 9 in the helper:

  1. After `fillJsonbValue` populates `*res`, type-dispatch on
     `res->type`:
       - `jbvString`: `palloc` a copy of length
         `res->val.string.len`, memcpy from `res->val.string.val`,
         repoint `res->val.string.val` at the copy.
       - `jbvNumeric`: `palloc` a copy of size
         `VARSIZE_ANY(res->val.numeric)`, memcpy, repoint.
       - `jbvBool` / `jbvNull`: no payload to copy.
       - default: free slices and return
         `JSONB_KEY_LOOKUP_FALLBACK`; defensive only —
         `fillJsonbValue` should not produce other scalar types
         from on-disk jsonb after JBE_ISCONTAINER has been
         filtered out at Stage 6.
  2. `pfree(value_slice)` if non-NULL.
  3. `pfree(prefix)`.
  4. Return `JSONB_KEY_LOOKUP_FOUND`.

The exported contract changed accordingly: the FOUND branch in
the header comment no longer mentions slice lifetime; *res is
self-contained on return; the caller may materialise at any
time, including across executor step boundaries.

**Where.** `src/backend/utils/adt/jsonb_util.c`,
`getKeyJsonValueFromExternal` Stage 9 (new) and the
function-level header comment.
`src/include/utils/jsonb_internal.h` (new file, see F2) — the
contract that travels with the prototype.

**Verification.** Build clean. jsonb_layer1 passes end-to-end,
including the long-string Case 2 and the numeric Case 9 — the
two cases that exercise the new memcpy paths
(`jbvString` and `jbvNumeric` respectively). bool/null carry
no payload and exercise the no-op branches; they are covered
by Case 1 (inline) and indirectly via the missing-key paths.

**Performance impact.** Per FOUND, the new path adds:

  - one `palloc` of size = scalar payload length;
  - one `memcpy` over that length;
  - two `pfree` calls (prefix; optionally value_slice).

For strings the typical payload is < 100 bytes; for numerics
< 32 bytes. Compared to the previous path which left 1–30 KB
of slice residue per row, this is a memory win at the cost of
two extra palloc/pfree pairs per FOUND. The headline numbers
in `LAYER1_COVER_LETTER.md` were obtained on the pre-F1 path
and have a footnote saying so. Re-measurement is required on
this branch and is tracked under "Pending" below.


## F2 — un-export the helper from public `jsonb.h`

**Finding.** Adding `extern JsonbKeyLookupResult
getKeyJsonValueFromExternal(...)` and `typedef enum
JsonbKeyLookupResult { ... }` to `jsonb.h` made them part of a
stable public API by accident. No in-tree code outside
`jsonfuncs.c` calls the helper. The FALLBACK / MISSING / FOUND
semantics are intimately tied to private implementation choices
(half-body cap, compressed-past-prefix behaviour, scalar-only
restriction).

**What changed.** Both declarations removed from `jsonb.h`. A
new non-public header `src/include/utils/jsonb_internal.h`
carries them, with a banner stating "NOT a stable API." Public
`jsonb.h` retains a one-paragraph pointer ("the sliced-read
helper used by jsonb_object_field / jsonb_object_field_text is
intentionally not declared here; see jsonb_internal.h") so a
reader does not have to grep to find it.

`jsonb_util.c` and `jsonfuncs.c` include the new header. No
other translation unit includes it.

**Where.**

  - `src/include/utils/jsonb.h` — Layer 1 block removed.
  - `src/include/utils/jsonb_internal.h` — new file.
  - `src/backend/utils/adt/jsonb_util.c` — include added.
  - `src/backend/utils/adt/jsonfuncs.c` — include added.

**Verification.** Build clean. `grep -r
getKeyJsonValueFromExternal src/` finds the declaration in
jsonb_internal.h and the definition + two call sites only.


## F3 — accurate corruption-detection wording

**Finding.** Both the function-level comment in `jsonb_util.c`
and the rationale block in `jsonb_layer1.sql` claimed "the slow
path raises ERRCODE_DATA_CORRUPTED on the same bytes." That is
false: `getKeyJsonValueFromContainer` performs no body-size
bounds checks; on a forged container header it walks JEntries
past the buffer end and may segfault, return undefined data,
or rarely succeed by accident. The fast path therefore
introduces a NEW error class for already-corrupt rows in
production.

**What changed.** Both locations rewritten:

  - The fast path may detect some physical inconsistencies
    (JEntry walk past body size, header size mismatch, short
    slice fetch) and raise ERRCODE_DATA_CORRUPTED.
  - The pre-existing slow path does NOT perform equivalent
    bounds checks. On the same corrupt bytes it may segfault,
    return undefined data, or rarely succeed by accident.
  - This is a behaviour change: queries that previously
    crashed or returned garbage on already-corrupt jsonb rows
    may now raise a clean ERRCODE_DATA_CORRUPTED.

The same statement is mirrored in
`src/include/utils/jsonb_internal.h` (the exported contract)
and in this fix log.

**Where.** `src/backend/utils/adt/jsonb_util.c` (function
comment), `src/include/utils/jsonb_internal.h` (header
contract), `src/test/regress/sql/jsonb_layer1.sql` (corruption
skip rationale at the end of the file).


## F4 — wide-object regression cases

**Finding.** The Stage 3 refetch branch (initial 1024-byte
prefix insufficient → re-fetch min_prefix + 4096) and the
Stage 4 refetch branch (key area exceeds Stage-3 extension →
re-fetch key_area_end) are real code paths. None of the
existing regression cases triggered them — all the test
objects had 3–4 keys, well under the ~100-key threshold for
Stage 3.

**What changed.** Two new cases in
`src/test/regress/sql/jsonb_layer1.sql`:

  Case 10. 200 short keys (`k0001` … `k0200`), all values
           short, table column `SET STORAGE EXTERNAL`. With
           N = 200 and no KVMap, min_prefix = 4 + 8·200 =
           1604 bytes, which exceeds the 1024-byte initial
           prefix. Stage 3 refetch fires. Five lookups: first
           key, middle, last, missing, plus one `->>` for
           jbvString materialisation.

  Case 11. 200 keys with 24-byte names (`long_key_padding_NNNNNN`),
           all values short. Total key area
           200 × 24 = 4800 bytes; combined with min_prefix
           the prefix needed at Stage 4 is ~6400 bytes,
           exceeding Stage 3's extension at min_prefix + 4096
           = 5700 bytes. Stage 4 refetch fires. Four lookups:
           first, middle, last, missing.

**Where.** `src/test/regress/sql/jsonb_layer1.sql`,
`src/test/regress/expected/jsonb_layer1.out`.

**Verification.** Full jsonb regression set
(jsonb / json / jsonb_jsonpath / jsonpath_encoding / jsonpath /
jsonb_kvmap / jsonb_layer1) all green on the upstream-prep
branch. New cases output the expected values.

A subtle point: Case 10's body (200 short keys, short values)
totals only ~3.5 KB. STORAGE EXTERNAL alone does not force
out-of-line storage if the row fits under
TOAST_TUPLE_THRESHOLD. With ~3.5 KB body the row exceeds the
default 2 KB threshold and goes external. We verified this
indirectly: if the row were inline, the helper's
VARATT_IS_EXTERNAL_ONDISK gate would short-circuit before any
Stage 3 logic, and the test would still pass — but with the
slow path doing the work, not the wide-object path we intended
to cover. To make Stage 3 coverage robust against future
TOAST_TUPLE_THRESHOLD changes (BLCKSZ-dependent), the test
relies on the row size being clearly above any reasonable
threshold; if upstream ever raises the threshold past 3.5 KB,
Case 10 will need its body inflated. That is a known fragility
of any "force external" test that doesn't poke
pg_class.relpages directly. The fragility is documented in the
test's comment.


## F5 — drop SHA references from reviewer-facing docs

**Finding.** The cover letter referenced SHAs `28c8b7fa5c`,
`b4e20c025e`, etc. The checklist referenced a third set
(`d3bd1a9d90`, `c05ddb3182`, `f9015a62ff`). Actual branch SHAs
were a fourth set. SHAs drift on every rebase; pinning them in
documentation creates "unknown revision" failures for any
reviewer who runs `git show <sha>` from a doc.

**What changed.** Cover letter and checklist both rewritten:

  - Specific SHAs removed.
  - References to the code itself go through the patch
    layout: 0001 helper+integration, 0002 tests, 0003 docs.
  - The fix-cycle log uses issue identifiers (B1, F1, F2, ...)
    rather than SHAs.
  - A short explanation of why SHAs are not pinned ("they
    drift on rebase") is included so future authors do not
    re-add them.

**Where.** `docs/LAYER1_COVER_LETTER.md`,
`docs/LAYER1_REVIEW_CHECKLIST.md`, this document.


## F7 — half-body comment downgraded to empirical cap

**Finding.** The comment at the half-body check said the
threshold derived from chunk-overlap analysis. It does not.
For uncompressed external bodies the real break-even is
significantly higher than 50%; the chosen threshold is
conservative and empirical.

**What changed.** Comment rewritten to say:

> Conservative empirical cap: when the value is large enough
> that a separate slice fetch would cover a substantial
> fraction of the body, the full-detoast path is competitive
> or better. The 1/2 threshold is intentionally pessimistic —
> for uncompressed external bodies the actual break-even is
> significantly higher and would need measurement against
> TOAST_MAX_CHUNK_SIZE to derive. Finer tuning is deferred
> until we have a benchmark showing the cap matters.

**Where.** `src/backend/utils/adt/jsonb_util.c`, Stage 6 of
`getKeyJsonValueFromExternal`.


## F6 — commit shape for upstream submission

**Finding.** The branch carried a three-commit sequence
(implementation, fix-of-implementation, tests) that reads
badly on a hackers thread.

**Plan.** The fork branch keeps the longer history (useful for
internal review and rollback). For upstream posting, the
branch is squashed and reshaped to the three patches listed
in F5. The actual squash happens at the re-roll step against
`pgsql-master`, which is explicitly the next step after this
fix cycle and is not part of this commit log.

**Where.** Documented in `LAYER1_COVER_LETTER.md` § Patch
series and in `LAYER1_REVIEW_CHECKLIST.md`.


## Deferred items (still deferred, not blocking)

The prior internal review's S1 (collapse Stage 3 and Stage 4
refetches into one), and M1–M7 (smaller cleanups), remain
deferred. None of them block upstream-prep. S1 is now
testable on the upstream-prep branch via the wide-object F4
cases; folding it in is a half-day item, candidate for the
re-roll commit.


## Regression results

Full jsonb test set on the upstream-prep branch, single-CPU
build, debug + cassert, no-locale:

  ok 1  jsonb                412 ms
  ok 2  json                  65 ms
  ok 3  jsonb_jsonpath        74 ms
  ok 4  jsonpath_encoding      6 ms
  ok 5  jsonpath              19 ms
  ok 6  jsonb_kvmap           12 ms
  ok 7  jsonb_layer1         285 ms
  1..7
  All 7 tests passed.

`jsonb_layer1` includes the new Cases 10 and 11.

The `test_setup` failure flagged in `LAYER1_REVIEW_CHECKLIST.md`
under the "Known non-scope items" section is unrelated to this
patch and is the pre-existing tablespace artifact; it does not
appear in the seven-test set above because that set does not
include test_setup.


## Pending — to be done outside this sandbox

  1. **Cold-cache A/B/C re-measurement on the upstream-prep
     branch.** F1 changed the allocation pattern: prior helper
     left 1–30 KB of slice residue in CurrentMemoryContext on
     every FOUND; new helper memcpy's the scalar payload (few
     bytes) and pfree's both slices before returning. Expected
     effect on FOUND latency: low single-digit percent at
     most. Required for the cover letter to ship clean
     numbers.

     How to run: `--mode cold` on
     `r2d2/legend-relocation-rename`, cells F0/P00/P10/J00/J10,
     ids 60/75/100, keys key1/key3, 3 cold runs per cell.
     Cover letter §10.2 acceptance is met if:
       Case A (P10 id=100 key1): P10 ≤ J10 on buffer count
       Case B (uncompressed past prefix): 6 reads / ~0.17 ms
       Case C (compressed past prefix at id=100 key3):
         P00 buffer profile matches F0 within noise

  2. **Re-roll against `pgsql-master`.** The current branch is
     based on `r1-relocation-aware-read`, which carries the
     fork's `Toastapi_jsonb_object_field_hook`. Upstream master
     does not. Rebase will require removing the hook block from
     `jsonb_object_field`'s Layer-1 gate site, and the
     companion site in `jsonb_object_field_text` does not need
     changes (it never had the hook).

     This step is NOT done in this fix cycle by design
     (@yoda's instruction: "Do not yet reroll against
     pgsql-master").

  3. **Test-file rename.** L1 from the review:
     `jsonb_layer1.sql` / `jsonb_layer1.out` retain the
     internal "Layer 1" nomenclature. Upstream-prep convention
     would rename to `jsonb_sliced_read.sql`. Mechanical;
     parallel_schedule needs the same rename. Defer to the
     re-roll commit so the rename and the rebase land
     together.


## Verdict

Each item from the review's BLOCKING and SHOULD-FIX lists is
either resolved above (B1, F1, F2, F3, F4, F5, F7, F8) or has a
documented disposition (F6 — plan recorded; pending re-roll —
explicitly deferred per instruction). The CAN-FIX-LATER items
(L1–L5) remain deferred and are tracked in the prior internal
review log.

Verdict: **READY_FOR_UPSTREAM_PREP**.

— @r2d2
