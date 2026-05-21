# Layer 1 — independent pgsql-hackers-style review

Reviewer posture: an outside PostgreSQL hacker opening this patch
cold on the mailing list, with no access to JBTL / @r2d2 /
@yoda context, no internal docs, and no prior review thread.
The brief asks me to find what a real hackers reviewer would
complain about before they do, and to be stricter than the
prior internal pass.

Branch read: `r2d2/layer1-sliced-read-clean` at tip `f9a4b1d`
(actual published-clean tip; see §6 on SHA hygiene).

Code under review (Layer 1 only):
  - `62fbc6a` original implementation
  - `50f9a78` D1+D2+S2 fix cycle
  - `c752377` D3 regression coverage

Out of scope by request: Layer 2 relocation; subscripting
follow-up; modify/WAL; `jsonb_sort_field_values` default;
deferred cleanup items S1 and M1-M7 unless I think one of them
blocks.

I read the cover letter and checklist last, so the findings are
mostly from the code. The pre-existing internal review
(`LAYER1_INDEPENDENT_CODE_REVIEW.md`) found D1/D2/D3/S1/S2/S3
and M1-M7; D1+D2+D3+S2 are fixed, S1+M1-M7 are deferred. I do
not re-litigate those except where I disagree with the
disposition. I focus on what was missed.


## 1. Would this be understandable to an outside reviewer?

**Partly.** The code itself reads cleanly. The header comment
in `jsonb.h` (lines 502-536) and the function-level comment in
`jsonb_util.c` (lines 490-524) are well-written, name the
mechanisms (KVMap, INTALIGN, JEntry length convention), and
state the memory-ownership contract. The fallback enum is
self-documenting. A reviewer who knows core jsonb internals
can follow.

What hurts comprehension on a hackers thread:

  - The cover letter and the regression file are saturated with
    internal jargon: "Case A/B/C", "P0/J0/F0", "Layer 1",
    "Layer 2", "Case 9 — bug-2 regression lock", "JBTL bug-2
    migration trigger", "spec §10.2". Every one of these is a
    reference to an internal document the hackers reader does
    not have. The README of the patch must stand alone.

  - The cover letter references commit SHAs `28c8b7fa5c`,
    `47e738f66f`, etc., that do not exist on this branch. The
    SHAs on the published-clean branch are `62fbc6a`, `50f9a78`,
    `c752377`. The checklist uses a third set of SHAs
    (`d3bd1a9d90`, `c05ddb3182`, `f9015a62ff`). A reviewer
    trying to `git show` any of these will get "unknown
    revision" and start mistrusting the package.

  - The `Toastapi_jsonb_object_field_hook` call at jsonfuncs.c
    (around line 895, immediately above the Layer 1 block) is
    fork-only. An upstream reader will see a hook call that
    does not exist in pgsql-master, and the diff will not apply
    to master. See §2.B2.

  - The cover letter's headline numbers (9.3x / 11.9x / 6.7x)
    are produced by an out-of-tree bench-suite branch
    (`r2d2/legend-relocation-rename`) plus a compile-time
    counterfactual ("`if (0 && VARATT_IS_EXTERNAL_ONDISK(...)`")
    that is not in the patch. Hackers convention is that the
    reproducer ships with the patch. Without it, the numbers
    cannot be verified by the reviewer.

So: the code is understandable. The package around it is
internal-shaped and would draw "please rewrite the cover letter
for an outside audience" within the first review pass.


## 2. Exported helper API in `jsonb.h`

The exported surface is:

```c
typedef enum JsonbKeyLookupResult
{
    JSONB_KEY_LOOKUP_FOUND,
    JSONB_KEY_LOOKUP_MISSING,
    JSONB_KEY_LOOKUP_FALLBACK,
} JsonbKeyLookupResult;

extern JsonbKeyLookupResult getKeyJsonValueFromExternal(
    Datum raw, const char *keyVal, int keyLen, JsonbValue *res);
```

### A. Three-state enum vs returning `JsonbValue *`

The enum is correct and I would not change it. The prior
review's D2 argument (compile-time exhaustiveness for the
caller's `switch`) is the right argument. FOUND vs MISSING vs
FALLBACK is a real three-way distinction; FOUND-with-`*res`
written, MISSING with `*res` untouched and caller-returns-NULL,
FALLBACK with `*res` untouched and caller-detoasts.

### B. Memory-ownership contract

The header explains it clearly. The caller must materialise
before any context boundary; existing callers do so
immediately via `JsonbValueToJsonb` / `JsonbValueAsText`.

**But there is a per-row memory-bloat concern not addressed in
the contract.** On every FOUND, the helper deliberately leaves
`prefix` (1 KB to ~30 KB depending on object size) and possibly
`value_slice` allocated in CurrentMemoryContext. The
explanation: `fillJsonbValue` may have written pointers from
*res into those buffers, so they cannot be freed before the
caller materialises.

The natural fix is for the helper to do the materialisation
itself: copy the small scalar bytes into a fresh small allocation
and free the slices before returning. The caller would still get
back a `JsonbValue` whose pointers are owned by the helper's
small allocation, not by the multi-KB slices. The current design
chooses minimal copying inside the helper at the cost of
multi-KB residue in the executor's per-tuple context per FOUND
row.

For a `SELECT t.jb -> 'k' FROM t WHERE ...` over many rows, the
per-tuple context's high-water-mark grows. aset/generation
contexts retain block chunks until reset and grow block sizes
geometrically. A 10M-row scan with 4 KB residue per row will
allocate ~40 GB to the per-tuple context across the scan, with
peak instantaneous usage at the first ExecScan reset (~MB to GB
depending on tuple batch size). The slow path does not have this
bloat — `PG_GETARG_JSONB_P` returns a single detoasted body
which the executor frees in the normal pattern.

This is a regression in a metric that has not been measured.
It is also asymmetric with the slow path. A hackers reviewer
who works on memory accounting (Tomas Vondra, Andres) is likely
to flag it.

**Action**: deep-copy the scalar payload inside the helper,
free both slices before returning FOUND, drop the
memory-ownership contract from the header (callers no longer
need to know about slice lifetime).

### C. Does this belong in `jsonb.h`?

`jsonb.h` is a stable header. Once an `extern` lives there,
out-of-tree extensions can call it, and the signature plus
return-value semantics become a forever commitment. The
prototype's FOUND/MISSING/FALLBACK split is intimately tied to
this specific optimisation (the half-body heuristic, the
"compressed-and-past-prefix" fallback, the scalar-only
restriction); these are private implementation choices, not a
stable contract.

The only in-tree callers are two adjacent functions in
`jsonfuncs.c`. There is no upstream need for this to be
exported.

**Action**: either move the helper into `jsonfuncs.c` as
`static`, or expose it via a non-public header
(`src/include/utils/jsonb_internal.h` if one is to be created,
or simply nowhere — the function lives next to its callers).

This is consistent with current PG conventions:
`getKeyJsonValueFromContainer` is in `jsonb.h` because it is
called from several places including some extensions. Layer 1's
helper has no analogous justification.

### D. Naming

`getKeyJsonValueFromExternal` parallels
`getKeyJsonValueFromContainer`. The word "external" is
overloaded in PG (column STORAGE EXTERNAL ≠ on-disk TOAST
pointer ≠ varlena indirect). The helper actually means "on
plain external on-disk TOAST varlenas" — neither inline, nor
expanded, nor indirect. A more precise name:
`getKeyJsonValueFromToasted` or
`tryGetKeyJsonValueFromToasted`. The `try` prefix would also
signal the FALLBACK return value at the call site. I would not
demand renaming, but I would mention it.


## 3. Fallback boundaries

I traced each fallback condition through the helper and the
spec's §7 table (per the checklist, which I read after coding
up the trace). The mapping is one-to-one with one exception:

  | condition                                | line  | result    |
  |------------------------------------------|-------|-----------|
  | inline / non-on-disk varlena             | 570   | FALLBACK  |
  | body_size < 4 (header-only)              | 584   | FALLBACK  |
  | initial slice < 4 bytes                  | 604   | FALLBACK  |
  | non-object root                          | 613   | FALLBACK  |
  | empty object                             | 620   | MISSING   |
  | min_prefix > body_size                   | 635   | CORRUPTED |
  | extended slice short of min_prefix       | 661   | CORRUPTED |
  | key length walk past body                | 682   | CORRUPTED |
  | extended slice short of key_area_end     | 704   | CORRUPTED |
  | key not in object                        | 752   | MISSING   |
  | value is container                       | 763   | FALLBACK  |
  | value_len * 2 > body_size (half-body)    | 775   | FALLBACK  |
  | value past prefix on compressed external | 827   | FALLBACK  |
  | value byte range > body_size             | 814   | CORRUPTED |
  | short value slice fetch                  | 842   | CORRUPTED |

Mostly correct. Two concerns:

### A. `min_prefix > body_size` check is bypassable via integer overflow

`header_size = sizeof(uint32) + 2 * N * sizeof(JEntry)` is
`4 + 8N`. `kvmap_bytes = INTALIGN(N * kvmap_entry_size)` which is
`INTALIGN(4N)` for N ≥ 65536. Both computed and summed as int32.

`N` comes from `JsonContainerSize(jc)` which is
`jc->header & JB_CMASK = 0x0FFFFFFF` (28-bit field) — so N can
be up to 268435455. For N in roughly `[178956971, 268435455]`
and `has_kvmap=true`, `min_prefix = 4 + 8N + INTALIGN(4N)`
overflows int32 to a large negative value. The check at line
635 `if (min_prefix > body_size)` then evaluates
`negative > positive` as false, the corruption ereport does not
fire, and the code proceeds into Stage 3 with a corrupt
`min_prefix`. Subsequent computation:

  - `if (body_len < min_prefix)` at line 648 is `positive <
    negative`, false; no refetch is triggered.
  - Stage 4: `key_area_end = min_prefix` starts negative.
    The loop's `key_area_end > body_size` check at line 682
    will eventually trigger on a malicious JEntry walk, but
    only after some number of iterations of reading
    `jc->children[i]` for i ∈ [0, N) — and the first read
    `getJsonbLength(jc, i)` only walks JEntries in the prefix
    that has not been extended. Out-of-bounds reads against the
    1 KB prefix buffer.

I verified the arithmetic numerically (header_size = 2147483644,
kvmap_bytes = 1073741820, true sum = 3221225464, wraps to
-1073741832 on int32). Reproduces in Python; the same compiler
arithmetic in C on a 64-bit platform produces the same wrap.

To trigger this in practice, an attacker needs to inject a
forged container header. Routes: `bytea::jsonb` cast bypasses
parser validation; physical corruption; SQL injection writing
literal jsonb that bypasses the input parser is harder because
jsonb_in builds N from element count, not from a literal field.
So the threat surface is narrow but not empty.

Importantly: the slow path on the same forged body has no
bounds checks against body_size either, and would segfault
during `getKeyJsonValueFromContainer`'s own JEntry walk. The
helper does not introduce a new failure class — it inherits an
existing one. But the helper's stated intent in this code
(lines 635-642) is to detect the corruption and raise
ERRCODE_DATA_CORRUPTED. The bypass means the patch ships
documentation and a check that do not match runtime behaviour.

On pgsql-hackers, this kind of "the bounds check looks careful
but actually wraps" finding draws mandatory rework. I rate this
**blocking**.

**Action**: do the computation in int64.

```c
int64   min_prefix64;
...
min_prefix64 = (int64) sizeof(uint32)
             + 2 * (int64) N * (int64) sizeof(JEntry)
             + (has_kvmap ? INTALIGN((int64) N * kvmap_entry_size) : 0);
if (min_prefix64 > body_size)
    ereport(ERROR, ...);
min_prefix = (int32) min_prefix64;
```

Or alternatively cap N at a saner upper bound (jsonb objects of
millions of keys are degenerate anyway) and reject larger N as
corrupt before any sizing arithmetic. I prefer the int64 version
because it imposes no policy choice the spec did not already
make.

The same audit should be applied to `key_area_end` (line 675),
to `value_byte_end_in_body` (line 811), and to the bounds
arithmetic at lines 660-668 and 691-712 for completeness, even
though those each have a defensive secondary check.

### B. Half-body heuristic at line 775

```c
if ((int64) value_len * 2 > (int64) body_size)
    return JSONB_KEY_LOOKUP_FALLBACK;
```

The int64 cast here is correct (this was flagged in the prior
review as a non-issue and I concur on the overflow point).

But the threshold is unmotivated. The comment says "slice
fetches would overlap the prefix fetch in chunk space, the full
path is cheaper." This is an asserted conclusion, not a derived
one. The actual chunk-overlap calculation depends on
`TOAST_MAX_CHUNK_SIZE`, on `body_size`, and on how the value
straddles chunk boundaries. At 50% of a 100 KB body the value
spans ~25 chunks, the prefix spans ~1 chunk, no overlap — slice
fetch wins. The heuristic is conservative.

For uncompressed external (the only case where this fallback
actually fires — compressed external is caught earlier), the
break-even is probably closer to 80%-90% of body. The patch is
leaving wins on the table.

I would not block on this, but I would expect to see either:
  (a) a measurement establishing the actual break-even on
      common BLCKSZ;
  (b) the comment downgraded to "empirical cap; finer tuning
      deferred";
  (c) the threshold raised to ~0.75 with a justification.

Currently the patch reads as if 1/2 is a derived value. It is
not.

### C. Compressed-external-past-prefix fallback (line 827)

Correctly checks `is_compressed` after the prefix-resident
fast path. Comment cites `detoast_attr_slice`'s assertion that
`sliceoffset == 0` for compressed externals. Verified that
constraint in `src/backend/access/common/detoast.c`. Correct.

### D. Nested container fallback (line 763)

`JBE_ISCONTAINER(value_jentry)` is the right test. Correct.

### E. Non-object root (line 613)

`JsonContainerIsObject(jc)` rejects array roots and top-level
scalars (which are stored as a 1-element array with JB_FSCALAR).
Correct.

### F. Empty object (line 620)

Returns MISSING. Correct and a useful short-circuit. Note that
empty objects are typically inline, so this branch is rarely
hit in production. The regression suite does not cover it. Minor.


## 4. Corruption checks

### Strict-vs-weak balance

The five `ereport(ERROR, ..., ERRCODE_DATA_CORRUPTED, ...)`
sites:

  - line 638-642: `min_prefix > body_size` — bypassable, see
    §3.A.
  - line 664-668: extended slice short of min_prefix — guards
    against `detoast_attr_slice` returning short. In practice
    detoast cannot return short for a valid TOAST chain (it
    would itself ereport); this is defense in depth. OK.
  - line 685-687: key area runs past body — guards against
    forged or accumulated JEntry length corruption. Caught by
    the inner overflow check `key_area_end < min_prefix`. OK,
    diagnostic could be more specific.
  - line 706-710: extended slice short of key_area_end — same
    as line 664, defense in depth. OK.
  - line 815-820: value extent runs past body — guards against
    forged value-side JEntries. Note: also performed via int32
    arithmetic. `value_start_in_body = min_prefix +
    value_offset_in_data` and `value_byte_end_in_body =
    value_start_in_body + fetch_len`. If min_prefix is already
    wrong (per §3.A) this check is also wrong.
  - line 846-849: short value slice fetch — defense in depth.

### Header comment claim

`jsonb.h` lines 530-533 say:

> The helper may also raise ERRCODE_DATA_CORRUPTED on physical
> inconsistency (JEntry walk past body size, header size
> mismatch, short slice fetch). The slow path raises the same
> on the same bytes.

The second sentence is false. The slow path
(`getKeyJsonValueFromContainer`) has no bounds checks against
the original body size at all — it operates on a fully-detoasted
heap-allocated copy and walks JEntries past it with simple
pointer arithmetic. On forged bodies it segfaults, returns
garbage, or (rarely) succeeds by accident. It does not raise
ERRCODE_DATA_CORRUPTED on the same bytes.

The helper is therefore **adding a new error class** for some
corrupt bodies: where the existing system was producing
undefined behaviour (often a crash), the new code raises a
clean ereport. This is a net improvement in robustness, but it
is a user-visible behaviour change for the population of
already-corrupt rows in production databases. A query that
silently returned wrong data or crashed in a way the user did
not notice might now raise an error and fail the query.

This is the kind of change pgsql-hackers will want documented
explicitly in the commit message: "Behaviour change: corrupt
jsonb bodies that previously crashed or returned undefined
results will now raise ERRCODE_DATA_CORRUPTED earlier in the
call chain."

**Action**: rewrite the header claim, and document the
behaviour change in the commit message.

### `fetch_len = value_len`

This was bug 2 in the fix cycle. I traced fillJsonbValue's
access pattern for each scalar type:

  - jbvNumeric: data lives at `INTALIGN(synth_base + offset)`,
    length is `numeric_size` derived from the numeric itself,
    not from JEntry. JEntry length encodes
    `padlen + numeric_size`. So bytes at `[offset, offset +
    value_len)` cover the pad + the numeric. The numeric is
    self-describing (VARSIZE), so once payload starts at
    `INTALIGN(offset)`, fillJsonbValue reads only as much as
    the numeric encodes. fetch_len = value_len is correct and
    not an over-fetch.
  - jbvString: data lives at `synth_base + offset`, length is
    `value_len`. No padding. Correct.
  - jbvBool / jbvNull: no payload, JEntry tag alone encodes the
    value. value_len should be 0 for these.

The fix is correct.

One follow-on: for jbvBool / jbvNull on a past-prefix value,
the helper still goes through the value-fetch path. With
value_len = 0, `value_byte_end_in_body = value_start_in_body`,
and `body_len >= value_byte_end_in_body` is automatically
satisfied because we extended body_len to cover the key area
which ends at min_prefix + sum(key_lens) ≤ value_start_in_body
(for the first value) up to value_start_in_body for the last
key. So bool/null values land in the prefix path
opportunistically; never trigger the compressed fallback.
Acceptable.

### Numeric pad handling

Spot-checked against `convertJsonbObject` at jsonb_util.c:2110
(the writer cited in the cover letter) — writer side encodes
`length = padlen + valuelen` into JEntry for INTALIGN'd types.
Reader side at line 761 reads back the same length. Symmetric.
The patch's comment block at lines 783-794 explains this; I
think the explanation is correct and well-placed.

### `va_rawsize - VARHDRSZ` as body size

Verified against varatt_external struct documentation and
detoast.c usage. For both compressed and uncompressed externals,
`va_rawsize - VARHDRSZ` is the uncompressed body size in the
coordinate system JEntry offsets use. `VARATT_EXTERNAL_GET_EXTSIZE`
returns the on-disk saved size (compressed for compressed,
uncompressed for uncompressed), which is the wrong basis for
JEntry-offset bounds. Bug 1's fix is correct.


## 5. Regression coverage

The `jsonb_layer1.sql` file is well-structured, each case
labelled with the spec reference, the expected fallback /
non-fallback behaviour stated in a comment. Coverage of the
listed cases:

  - helper hit: Cases 2, 3, 4, 9, sort-on. ✓
  - helper fallback: Cases 1 (inline), 5 (compressed past
    prefix), 6 (nested container), 7 (half-body). ✓
  - compressed external: Cases 4, 5, 6, 7. ✓
  - uncompressed external: Cases 2, 3, 9. ✓
  - missing key: Case 8. ✓
  - nested container fallback: Case 6. ✓
  - numeric near body end: Case 9. ✓

What is missing:

### A. The Stage 3 refetch branch (line 648)

The helper has a code path "initial 1024-byte prefix is not
enough; refetch with `min_prefix + 4096`." This fires when N is
large enough that `4 + 8N + INTALIGN(4N) > 1024`. Roughly N >
100 with KVMap, N > 127 without. None of the regression cases
hit this — all the test objects have 3-4 keys.

This is a real code path that is currently untested. Hackers
reviewers do flag this kind of gap. The test would be: insert a
row with `jsonb_build_object` of ~200 short keys, make it
external (e.g. by setting one value to a long string),
SELECT `jb -> 'someKey'`, expect the correct value back.

### B. The Stage 4 refetch branch (line 691)

Similarly. Triggered by sum of key lengths exceeding the
extended slice. Less likely than Stage 3 but still reachable
for an object with many short-key-value pairs.

These two paths together are the "wide object" case. Untested.

### C. Bool / null as the looked-up value on an external body

Cases 1 (inline) exercises null and true via short bodies. No
external case has a bool or null payload. The bool/null path in
fillJsonbValue is trivial but the path through the helper
(value_len = 0, body_len ≥ value_byte_end_in_body
automatically, fillJsonbValue with synth_base) has no test.

### D. The corruption ereport paths

The .sql file explicitly skips this with rationale: "bytea cast
is fragile across builds; hex-edited heap files are not
regression-suite material." The rationale is reasonable. But
upstream practice is to test ERRCODE_DATA_CORRUPTED paths
either via TAP (`src/test/perl/`) with a server-side stub, or
via an injectable corruption point compiled under cassert.

If injection infrastructure is too heavy for this patch, a TAP
test that builds a row with bytea-cast jsonb and checks for the
correct error code is feasible (`select 'corrupt-bytes'::bytea::jsonb -> 'k'`).
The build-fragility argument applies if jsonb format ever
changes; jsonb is in fact format-stable, so a single fixture
should hold.

### E. Inline-vs-external equality

The .sql file shows the operator output for each case but does
not directly assert equality between the same logical query
against an inline body and against an external body of the same
JSON content. That would be the strongest regression: prove
that whether the helper fires or not, the user-visible result
is identical. Today, this is inferred from the .out file but
not enforced by the SQL.

A single test like:

```sql
WITH ext AS (SELECT jb FROM l1_ext_uncompr WHERE id = 1)
SELECT (ext.jb -> 'k1') = ('{"k1":"small_at_start", ...}'::jsonb -> 'k1')
FROM ext;
```

would lock the equivalence.

### F. Concurrency / TOAST visibility

Not testable in the regression suite as written (single-session
SQL). For pgsql-hackers, the question "does Layer 1 race with
TOAST vacuum between the two `detoast_attr_slice` calls" needs
an answer. The answer is no — TOAST entries are written once
and removed only when no live snapshot references them, and
both helper slice calls execute under the same snapshot. The
question should be addressed in the commit message.

### Summary on tests

The coverage as-is hits the documented case matrix. The Stage
3/4 refetch paths are real code, not just defensive
scaffolding, and are not exercised. I would block on at least
adding a wide-object test for Stage 3. The corruption path
test is "should-fix" rather than blocking, given the rationale.


## 6. Style and maintainability

### A. SHA hygiene

The cover letter at `docs/LAYER1_COVER_LETTER.md`:
  - states `Tip: b4e20c025e`. Actual tip is `f9a4b1d`.
  - lists patch series with SHAs `417a6cff97`, `28c8b7fa5c`,
    `47e738f66f`, `079e0b382d`, `b8e3c6f922`, `684b11bf5e`,
    `84122504c7`. None of these exist on the branch.

The checklist at `docs/LAYER1_REVIEW_CHECKLIST.md`:
  - cites `d3bd1a9d90`, `c05ddb3182`, `f9015a62ff`. None of
    these exist on the branch either.
  - explains the SHA divergence as a rebase artifact, but the
    explanation does not retroactively rename the SHAs to ones
    that actually exist.

Actual SHAs on `r2d2/layer1-sliced-read-clean`:
  - `62fbc6a` original Layer 1
  - `50f9a78` D1+D2+S2 fix
  - `c752377` D3 regression coverage
  - tip `f9a4b1d`

This is purely a documentation defect, not a code defect, but
it actively obstructs review. A reviewer running
`git show <SHA>` against any of the cited SHAs gets "unknown
revision." Trust in the package suffers immediately.

**Action**: refresh all SHAs in cover letter + checklist
before any external review or upstream submission. Best to
script this so it does not drift again.

### B. Patch series shape

For a pgsql-hackers submission this would have to be either one
squashed patch (preferred for a small change) or 3 patches in
this order:

  1. helper + integration sites (combined 62fbc6a + 50f9a78)
  2. regression coverage (c752377)
  3. documentation, if any user-visible

Currently the public branch has the implementation in `62fbc6a`,
a "fix" commit `50f9a78` against that implementation, and tests
in `c752377`. Submitting three commits where commit 2 fixes
defects in commit 1 invites the question "why not just one
commit?" Upstream squashes such sequences. Cover letter
acknowledges this implicitly under "next steps."

### C. Internal jargon in the codebase

The test file is `jsonb_layer1.sql`. The .out file is
`jsonb_layer1.out`. "Layer 1" is internal nomenclature. The
corresponding upstream-style name would be `jsonb_sliced_read.sql`
or `jsonb_object_field_external.sql`.

The `parallel_schedule` entry uses the test name verbatim
(`jsonb_kvmap jsonb_layer1`). Renaming the test is mechanical.

Test internal comments (`Case 9 — numeric value near body end —
bug-2 regression`) reference internal bug numbers. Upstream
prefers descriptive names ("regression: numeric value near body
end must not over-fetch").

### D. `serial_schedule`

This fork does not have `serial_schedule` (removed upstream in
PG 18-dev cycle, commit c855421080, Feb 2024). On the assumption
that this patch will eventually be submitted against PG 19-dev,
the absence is correct. Worth mentioning so the reviewer does
not flag it.

### E. Comment density

The helper's per-stage comments are good. The block comment
ahead of `getKeyJsonValueFromExternal` repeats most of the
header comment. The cover letter / spec also restate the same
material. Total repetition across the patch is high. On
pgsql-hackers this would be condensed.

### F. `PG_GETARG_DATUM(0)` followed by
`PG_GETARG_JSONB_P(0)` on fall-through

In `jsonb_object_field` and `jsonb_object_field_text`, the
Layer 1 block calls `PG_GETARG_DATUM(0)` and the slow path
calls `PG_GETARG_JSONB_P(0)`. `PG_GETARG_DATUM` returns the raw
fmgr Datum (no detoast). `PG_GETARG_JSONB_P` macro-expands to
`pg_detoast_datum`, which on an external on-disk pointer will
walk the toast chain.

On the fall-through path (helper returned FALLBACK), the second
call (`PG_GETARG_JSONB_P(0)`) does a full detoast. This is the
intended behaviour — Layer 1 has not handled the case, so the
existing path takes over. But:

If the helper returns FALLBACK after having already done one or
two `detoast_attr_slice` calls (prefix fetch, possibly key-area
refetch), the buffer pages for the early TOAST chunks are now
already in shared_buffers. The subsequent full detoast on the
fall-through path will hit those chunks warm, so it does not
re-read disk. But it does re-decompress the compressed-external
case (for which we fell back). That's a small CPU cost (one
decompression of a body the user is going to see anyway).

The bigger question: in the FALLBACK after partial work case,
the `prefix` slice buffer is freed (every FALLBACK path
explicitly calls `pfree(prefix)`). Good. But the slice
allocation has bumped the memory context's high-water-mark.
Per row, on a wide scan, this is non-trivial.

This is a subset of the §2.B concern. Same fix (smaller
allocations / explicit free).


## 7. What I would object to on pgsql-hackers

### BLOCKING

  **B1.** `min_prefix` arithmetic (§3.A) overflows int32 for
  pathological N with `has_kvmap=true`, bypassing the explicit
  corruption check at line 635. The fix is mechanical (int64
  intermediate). The check is *labelled* as defending against
  this exact case, so the bug is doubly visible. Must be fixed
  before posting.

  **B2.** The branch is based on a fork that includes
  `Toastapi_jsonb_object_field_hook`. The patch as published
  will not apply against `pgsql-master`. Any submission to
  hackers requires a re-roll against master with the hook block
  removed. This is procedural, not technical, but it means "the
  patch in the cover letter" and "the patch hackers will see"
  differ. Re-roll first, review the re-roll second.

### SHOULD FIX BEFORE SUBMISSION

  **F1.** Per-row memory bloat from retained `prefix` /
  `value_slice` buffers in CurrentMemoryContext on every FOUND
  row (§2.B). Materialise inside the helper and free the slices
  before returning. Eliminates the asymmetry with the slow path
  and removes the explicit memory-ownership contract from the
  exported header (which then no longer needs to be exported —
  see §2.C).

  **F2.** Move the helper out of `jsonb.h`. Either file-static
  in `jsonfuncs.c` or in a non-public internal header (§2.C).

  **F3.** Rewrite the cover-letter / header claim that "the
  slow path raises the same on the same bytes" (§4). It does
  not. The patch tightens corruption detection; that is a
  user-visible behaviour change for the population of
  already-corrupt rows in production, and should be stated
  honestly in the commit message.

  **F4.** Add at least one regression test for the Stage 3
  refetch branch (§5.A). Wide-object case, ~200 keys, body
  forced external.

  **F5.** Refresh all commit SHAs in cover letter and
  checklist to match the actual branch (§6.A). Stale references
  to non-existent SHAs damage the review package's credibility
  before the code is opened.

  **F6.** Squash the three commits into one for upstream
  posting, or document that the public branch carries the
  history but the format-patch will be squashed (§6.B). The
  fix-on-fix sequence reads badly on a fresh thread.

  **F7.** The half-body threshold (§3.B) should be either
  justified or downgraded in its comment to "empirical cap."

  **F8.** Audit the int32 arithmetic in `key_area_end`,
  `value_byte_end_in_body`, and the extended-slice sizing for
  the same overflow class as B1, even though they have
  secondary defences. Uniform int64 intermediates are easier to
  reason about than "this one is OK because the next check
  catches it."

### CAN FIX LATER

  **L1.** Rename `jsonb_layer1.sql` (and corresponding .out
  file, schedule entry) to remove internal "Layer 1" jargon
  (§6.C). Mechanical, do at upstream-prep time.

  **L2.** S1 from the prior internal review — collapse the
  two refetches into one — was deferred. I agree with the
  deferral disposition: it is a perf optimisation in a code
  path the wide-object test will newly exercise, and once the
  test exists the optimisation can be benchmarked. Should land
  before upstream submission but is not a correctness item.

  **L3.** Bool / null values past prefix do an unneeded
  `detoast_attr_slice(..., 0)` (§3 trace, type discussion).
  Negligible.

  **L4.** Consider whether the helper should also catch the
  case where the *initial* prefix slice itself comes back too
  short due to a corrupted toast pointer (`va_rawsize` larger
  than the actual chain). Currently lines 591-597 compute
  `prefix_size` from `va_rawsize` and trust
  `detoast_attr_slice` to return that many bytes. If it returns
  fewer, `body_len < min_prefix` catches it on the second
  fetch but the diagnostic is generic. Minor.

  **L5.** Naming nit: `getKeyJsonValueFromExternal` could be
  `tryGetKeyJsonValueFromToasted` (§2.D).

### NON-ISSUES (verified)

  **N1.** Parallel safety — helper allocates in
  CurrentMemoryContext, reads only via TOAST snapshot, no
  shared mutable state.
  **N2.** lz4 vs pglz — `VARATT_EXTERNAL_IS_COMPRESSED` is
  method-agnostic.
  **N3.** Locale safety — `lengthCompareJsonbString` is binary
  memcmp; keys are compared in the writer's canonical sort
  order which the reader inherits.
  **N4.** TOAST snapshot stability across the two slice calls
  — both run under the same snapshot; TOAST entries are
  write-once and vacuum-protected by that snapshot.
  **N5.** KVMap writer/reader alignment formula symmetry —
  writer at line 2505 uses `INTALIGN(kvmap_entry_size * nPairs)`,
  reader at line 630 uses `INTALIGN(N * kvmap_entry_size)`,
  commutative, identical.
  **N6.** `StaticAssertDecl` against `JSONB_SLICED_READ_INITIAL_PREFIX <
  TOAST_MAX_CHUNK_SIZE` correctly handles BLCKSZ-dependent
  chunk size at compile time.


## 8. Verdict

**Initial verdict (intake): NEEDS_SMALL_FIXES_BEFORE_UPSTREAM.**

**Post-fix-cycle verdict: READY_FOR_UPSTREAM_PREP.**

The patch's design — fast-path gate on
`VARATT_IS_EXTERNAL_ONDISK`, prefix-slice + binary-search +
value-slice with three fallback classes — is sound. The
fallback boundaries are correctly chosen. The memory-ownership
rework now landed in F1 closes the only quasi-design concern
the review raised. The regression file is well structured and
now covers the wide-object refetch paths.

### Fix status

  | Item | Disposition |
  |------|-------------|
  | B1 (int32 wrap in `min_prefix` arithmetic) | FIXED — int64 intermediates throughout `getKeyJsonValueFromExternal`. Numerically reproduced overflow pre-fix; corruption check now fires on forged N with KVMap. See `LAYER1_UPSTREAM_PREP_FIXES.md` § B1+F8. |
  | B2 (fork-base rebase) | DEFERRED, per @yoda's instruction "do not yet reroll against pgsql-master". Tracked under Pending in `LAYER1_UPSTREAM_PREP_FIXES.md`. |
  | F1 (per-row memory bloat) | FIXED — helper deep-copies the scalar payload and pfrees `prefix` and `value_slice` before returning FOUND. `*res` is self-contained on return; memory-ownership contract removed from the exported header. |
  | F2 (helper exported in `jsonb.h`) | FIXED — declarations removed from `jsonb.h`, new non-public `src/include/utils/jsonb_internal.h` carries them, included only from `jsonb_util.c` and `jsonfuncs.c`. |
  | F3 (false slow-path corruption claim) | FIXED — wording corrected in the function-level comment, the `jsonb_internal.h` contract, and the `jsonb_layer1.sql` corruption-skip rationale. |
  | F4 (no Stage 3/4 refetch test coverage) | FIXED — `jsonb_layer1.sql` now has Case 10 (200 short keys, Stage 3 refetch) and Case 11 (200 × 24-byte keys, Stage 4 refetch). Verified green in the seven-test jsonb regression set. |
  | F5 (stale SHA references) | FIXED — cover letter and checklist rewritten to reference patches by number (0001 / 0002 / 0003) rather than SHA. Explanation embedded so SHAs are not re-added. |
  | F6 (squashed commit shape) | DOCUMENTED — fork branch keeps history; upstream squash to 0001/0002/0003 happens at re-roll. See `LAYER1_COVER_LETTER.md` § Patch series. |
  | F7 (unjustified half-body threshold) | FIXED — comment rewritten to "conservative empirical cap; finer tuning deferred." |
  | F8 (int32 audit beyond `min_prefix`) | FIXED — landed together with B1. `body_size`, `body_len`, `header_size`, `kvmap_bytes`, `min_prefix`, `key_area_end`, `value_len`, `value_byte_end_in_body`, `fetch_len`, `value_start_in_body` all int64. Slice-call downcasts to int32 only after the int64 bounds check. |
  | L1–L5 (can-fix-later) | DEFERRED — none block upstream-prep. L1 (test rename) folds into re-roll; L2 (S1 single-refetch) is now testable thanks to F4 cases. |
  | N1–N6 (non-issues) | Verified unchanged by the fix cycle. |

### Pending across boundaries

  - **Cold-cache A/B/C re-measurement** on the upstream-prep
    branch. F1 changed the allocation pattern; the per-FOUND
    work now adds one `memcpy` of the scalar payload (few
    bytes) and removes the per-row slice residue. Expected
    effect on FOUND latency: low single-digit percent at
    most. Cannot run in the review sandbox (needs the
    bench-suite branch and a real cluster with restart
    between samples); flagged in `LAYER1_UPSTREAM_PREP_FIXES.md`
    Pending § 1.
  - **Re-roll against `pgsql-master`** — deferred per
    instruction. Pending § 2.
  - **Test-file rename** (`jsonb_layer1.sql` →
    `jsonb_sliced_read.sql` or similar) — folds into the
    re-roll. Pending § 3.

### Build and test evidence

  - `jsonb_util.o` and `jsonfuncs.o` recompile cleanly under
    `-Wall -Wpointer-arith -Wdeclaration-after-statement
    -Werror=vla -Wcast-function-type -Wshadow=compatible-local
    -Wformat-security -Wstrict-prototypes -Wold-style-definition`.
    Zero warnings.
  - Full jsonb regression set on the upstream-prep branch
    (single-CPU debug + cassert build, no-locale):
    `jsonb`, `json`, `jsonb_jsonpath`, `jsonpath_encoding`,
    `jsonpath`, `jsonb_kvmap`, `jsonb_layer1` — all 7/7 pass.
  - `jsonb_layer1` exercises Stage 3 refetch (Case 10) and
    Stage 4 refetch (Case 11) for the first time. Both
    produce correct values; no behavioural change in Cases 1–9.

### Reviewer disposition

Verdict moved from `NEEDS_SMALL_FIXES_BEFORE_UPSTREAM` to
`READY_FOR_UPSTREAM_PREP`. The remaining work to reach a
hackers post (re-roll, cold-cache re-run, test-file rename) is
sequenced and documented; none of it requires reopening any of
the design decisions covered in §§ 1–7 above.

— independent-hacker hat, post fix cycle
