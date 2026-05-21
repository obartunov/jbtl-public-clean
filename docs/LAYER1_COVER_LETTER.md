# Layer 1 — cover letter for code review

Branch: `r2d2/layer1-upstream-prep`
Base:   `origin/r1-relocation-aware-read`
        (in `github.com/obartunov/jbtl-public-clean`)
Status: LAYER1_UPSTREAM_PREP — fix cycle complete; awaiting
        re-roll against `pgsql-master` for thread submission.
Scope:  `jsonb -> 'key'` and `jsonb ->> 'key'`
        — subscripting deferred to a named follow-up.

Specific commit SHAs are not pinned in this document; they drift
on every rebase. See `LAYER1_REVIEW_CHECKLIST.md` for the patch
layout (0001 / 0002 / 0003).

## Branch convention

This branch is the Layer-1-only review surface, in upstream-prep
shape after the B1/F1/F2/F3/F4/F5/F7/F8 fix cycle (details in
`LAYER1_UPSTREAM_PREP_FIXES.md`). The parent branch
`origin/r1-relocation-aware-read` carries the relocation /
Layer 2 design backlog including:

  - `PRODUCTION_RELOCATION_SOURCE_MAPPING.md`
  - `RELOCATION_DELETE_OWNERSHIP.md`
  - `PREFIX_LOCALITY_VS_RELOCATION.md`
  - other earlier prefix-locality and sort-default analyses

Inline references to those files inside Layer 1 docs (e.g. spec
§ pointers, result-doc tests) are intentional cross-context
pointers, not in-branch links. They resolve in the parent
branch. The Layer 1 reviewer does not need to follow them to
evaluate Layer 1.

## What this patch does

Adds a TOAST-aware sliced read path for the two jsonb key-fetch
operators when the source body is stored out of line. For an
object with a single small key access, the helper avoids
detoasting the whole body. The existing path is kept intact as
a fall-through; the helper engages only when it can be honest
about reducing work.

The helper `getKeyJsonValueFromExternal` lives in
`src/backend/utils/adt/jsonb_util.c`, with its prototype in the
non-public internal header `src/include/utils/jsonb_internal.h`
(deliberately not in the stable `jsonb.h`). It is called from
two sites in `src/backend/utils/adt/jsonfuncs.c`:
`jsonb_object_field` and `jsonb_object_field_text`.

## Headline performance — cold cache, single call

> Note: the numbers below were obtained on the pre-fix-cycle
> code path, where the helper left the prefix and value_slice
> palloc'd in CurrentMemoryContext for the caller's
> JsonbValueToJsonb to copy out of. After fix F1 (see
> `LAYER1_UPSTREAM_PREP_FIXES.md`) the helper deep-copies the
> scalar payload itself and pfrees both slices before
> returning FOUND. The new path adds one `memcpy` of the
> scalar (a few bytes for strings/numerics, zero for
> bool/null) and removes the per-row slice residue in the
> caller's memory context. The expected effect on the
> microsecond-scale FOUND latency below is at most low
> single-digit percent; the disk-read counts cannot change.
> Cold-cache re-measurement on the upstream-prep branch is
> pending and tracked as an open item in
> `LAYER1_UPSTREAM_PREP_FIXES.md`.

Baseline obtained by compile-time disabling the same code on
the same data (`if (0 && VARATT_IS_EXTERNAL_ONDISK(...))`), so
the comparison is strictly the gated helper vs no helper, not
a different build.

  Compressed external, prefix-resident scalar:
      1.521 ms -> 0.164 ms   (9.3x latency)
  Uncompressed external, prefix-resident scalar:
      1.873 ms -> 0.157 ms   (11.9x latency, 81 -> 5 disk reads)
  Uncompressed external, value past prefix:
      1.150 ms -> 0.172 ms   (6.7x latency, 80 -> 6 disk reads)
  Compressed external, value past prefix:
      1.543 ms -> 1.437 ms   (no change — deliberate fallback)
  Inline body:
      1.376 ms -> 1.245 ms   (no measurable change)

No regression on any case. Layer 1 is a strict win or a no-op
by construction.

Inline-row microbenchmark: 100 000 operator calls in a tight
loop, 5 runs of ~23.5 ms each (~0.23 us/call). The
`VARATT_IS_EXTERNAL_ONDISK` gate is a single inline bit-check
on the varlena tag; it cannot show up against per-call work of
~230 ns.

## Motivation

The existing read path for `jb -> 'key'` is:

  Toastapi_jsonb_object_field_hook   (extension fast path)
  jb = PG_GETARG_JSONB_P(0)           ← full detoast here
  if (!JB_ROOT_IS_OBJECT(jb)) RETURN NULL
  v = getKeyJsonValueFromContainer(...)
  RETURN JsonbValueToJsonb(v)

`PG_GETARG_JSONB_P` calls `pg_detoast_datum`, which reads every
chunk of a TOAST chain and decompresses everything. For a
247 KB body the existing path reads about 5100 buffer pages
and takes ~1.5 ms even when the requested key is the integer
literal at the start of the value area.

The fork already has the writer-side mechanism for prefix
locality (`jsonb_sort_field_values` and the KVMap on-disk
layout), but no reader actually uses it to skip work. Layer 1
is that reader. It is fully compatible with the existing
storage format — no on-disk byte changes, no new JEntry tags,
no catalog work.

## Algorithm sketch

  1. Fetch initial 1024-byte structural prefix via
     `detoast_attr_slice`.
  2. Parse container header; reject non-object roots ("not
     handled" — caller falls through).
  3. Compute `min_prefix = header + 2N JEntries +
     INTALIGN(KVMap)` using the same formula as the writer
     (`convertJsonbObject`, jsonb_util.c:2110-2114).
  4. Extend slice if it does not cover JEntries+KVMap+key area.
  5. Inline binary search the prefix bytes for the requested
     key, KVMap-aware (same translation as
     `getKeyJsonValueFromContainer`).
  6. On hit: reject container JEntries (scalar-only in v0);
     reject `value_len * 2 > body_size` (half-body heuristic
     — past that, slice fetches would overlap the prefix
     fetch in chunk space, the full path is cheaper).
  7. Fetch value bytes — from prefix if resident, else
     `detoast_attr_slice` at the value's offset. Compressed
     external + value past prefix sets "not handled" so the
     caller falls back (core's `detoast_attr_slice` asserts
     `sliceoffset == 0` for compressed externals).
  8. Materialise via existing `fillJsonbValue`.

## Acceptance criteria — three-case split

The spec's §10.2 originally said "P0 reproduces J0 within
counter noise," assuming an API the spec deferred. The
corrected acceptance has three sub-cases the helper genuinely
distinguishes:

  Case A. Prefix-resident scalar (sort on, or short value, or
          short body).
          Expected: reproduces J0, may beat it by saving
          extension dispatch overhead.
          Measured: P10 = 4.37 buf/call vs J10 = 7.90 at
          id=100 key3 sort on. Better than J0. ✓

  Case B. Uncompressed external, value past prefix.
          Expected: reproduces J0 — same mechanism (arbitrary-
          range slice into uncompressed TOAST).
          Measured: 6 reads / 0.172 ms vs baseline 80 reads
          / 1.150 ms. Comparable to JBTL. ✓

  Case C. Compressed external, value past prefix.
          Expected: reproduces F0, NOT J0. Helper falls back
          per spec §7 (core's `detoast_attr_slice` asserts
          `sliceoffset == 0` for compressed externals).
          Measured: 6 reads / 1.437 ms vs baseline 6 reads
          / 1.543 ms. No regression. ✓

JBTL-A reaches J0 in Case C only because its own storage
wrapper has chunked compression with per-chunk decompression
boundaries. Production Layer 1 has no comparable mechanism.
Closing Case C is deferred to either:

  (a) a future core TOAST API change (out of scope here,
      explicitly deferred in spec §7);
  (b) Layer 2 relocation, which sidesteps the problem by
      moving large values out of the parent body into their
      own TOAST chains (separate work).

## Patch series

For upstream-prep submission the branch is squashed/shaped into:

  0001  sliced jsonb read helper + integration
        - new helper getKeyJsonValueFromExternal in
          src/backend/utils/adt/jsonb_util.c
        - non-public declaration in src/include/utils/jsonb_internal.h
        - gate + caller switch in jsonfuncs.c at jsonb_object_field
          and jsonb_object_field_text
        - no change to src/include/utils/jsonb.h beyond a one-line
          pointer to jsonb_internal.h

  0002  regression coverage
        - src/test/regress/sql/jsonb_layer1.sql
        - src/test/regress/expected/jsonb_layer1.out
        - parallel_schedule entry

  0003  docs (cover letter, checklist, fix log, spec, result)
        - retained on the fork branch; trimmed for upstream
          posting (cover letter only)

The internal branch keeps a longer history (the original Layer 1
implementation commit, the D1+D2+S2 fix-cycle commit, the D3
tests commit, then the upstream-prep fix-cycle commits). Upstream
sees the squashed three-patch form.

## Documents

Read in this order for review:

  1. `docs/LAYER1_REVIEW_CHECKLIST.md` — patch layout, build /
     regression / cold-cache instructions, fix-cycle history;
  2. `docs/LAYER1_UPSTREAM_PREP_FIXES.md` — what changed in the
     upstream-prep cycle and why;
  3. `docs/LAYER1_POSTGRES_HACKER_REVIEW.md` — the strict
     independent-reviewer pass and its disposition;
  4. `docs/SLICED_JSONB_READ.md` — spec; §4-§7 are the
     algorithmic core, §10 the acceptance criteria;
  5. `docs/SLICED_JSONB_READ_RESULT.md` — empirical result,
     factorial matrix data, cold-cache baseline comparison
     (numbers from the pre-F1 path; see headline note above);
  6. `docs/SLICED_JSONB_SUBSCRIPTING_SCOPE.md` — why
     subscripting is deferred;
  7. the code itself, via `git log -p` over patch 0001.

## Explicit non-scope

This patch optimises `->` and `->>`.

Subscripting (`jb['key']`, `jb[0]`, `jb['a']['b']`) is
intentionally deferred to the next patch
(`SLICED_JSONB_SUBSCRIPTING_FOLLOWUP`). The helper signature
in `jsonb_internal.h` and the scope document in
`SLICED_JSONB_SUBSCRIPTING_SCOPE.md` make the follow-up
mechanical: single-step object-key fetch only, no array
subscripting, no multi-step traversal, no assignment path.

Also out of scope:

  - Layer 2 relocation (separate design and patch series);
  - Modify / UPDATE / WAL behavior;
  - Changes to the `jsonb_sort_field_values` default (Layer 1
    works correctly with the GUC off, its current default,
    and benefits when it is on — the default-flip decision is
    a separate compatibility task);
  - Any core TOAST API change to enable Case C wins (deferred
    in spec §7).

## What we found during implementation

Two bugs caught by a corner-case battery before benchmark.
Both detailed in `docs/SLICED_JSONB_READ_RESULT.md` "Bugs
found during implementation." Brief summary for context:

  - **Bug 1**: helper used `VARATT_EXTERNAL_GET_EXTSIZE`
    (on-disk compressed size) where it needed `va_rawsize -
    VARHDRSZ` (uncompressed body size) for in-body bounds.
    Affects compressed external only; silently disabled the
    helper for that case until fixed.

  - **Bug 2**: `fetch_len = value_len + pad` over-fetched by
    0..3 bytes on numeric values because the writer's JEntry
    length already includes the pad. Affects cold-cache
    compressed-external numeric-scalar-near-body-end only;
    raised ERRCODE_DATA_CORRUPTED until fixed to
    `fetch_len = value_len`. The same line exists in JBTL but
    is harmless there because JBTL's slice dispatch has
    tolerant bounds (see `079e0b382d` for the audit comment).

Neither bug was triggered by the warm matrix loop, because
that loop amortises the first cold call over 30-200 reps and
the bug condition needs the first call against strict bounds.
This led to a follow-up in the bench-suite: a new `--mode cold`
in `scripts/run_matrix.py` that does single-call EXPLAIN
BUFFERS with server restart between samples. Landed on the
bench-suite branch separately.

The general lesson: when porting a prototype into a stricter
production path, every bounds-check needs to be audited against
both spec contracts; places where the prototype is lenient and
production is strict are where latent bugs live.

## Regression suite

All 6 jsonb-relevant tests pass:

  ok jsonb             280 ms
  ok json               81 ms
  ok jsonb_jsonpath     59 ms
  ok jsonpath_encoding   7 ms
  ok jsonpath           14 ms
  ok jsonb_kvmap        15 ms

## Open questions for the reviewer

OQ-A. §10.2 wording revision: please confirm the three-case
split correctly captures spec intent.

~~OQ-B. Subscripting integration timing.~~ Resolved by yoda —
deferred to follow-up. See `SLICED_JSONB_SUBSCRIPTING_SCOPE.md`
and §6 of `LAYER1_REVIEW_NOTES.md` for the explicit reviewer-
facing wording.

OQ-C. JBTL bug-2 migration trigger condition. The Assert
remains in place as a tripwire on any future tightening of
`jbtl_fetch_slice_dispatch`. Confirm the decision to leave it
audit-only.

OQ-D. The mid-size buffer regression that
`PREFIX_LOCALITY_VS_RELOCATION.md` Test 2 attributed to
JBTL-A's chunked-compression dispatch overhead now has a clean
counterfactual: production Layer 1 inherits F0's profile in
that range. This strengthens the original Layer 2 design
argument; no action needed here, flagged for Layer 2.

## Next steps after acceptance

  1. Land Layer 1 patch series upstream / into the fork master.
  2. Apply `SLICED_JSONB_SUBSCRIPTING_FOLLOWUP` — small, scope
     fully specified in `SLICED_JSONB_SUBSCRIPTING_SCOPE.md`
     §8, estimated half-day code plus half-day docs/tests.
  3. Resume Layer 2 relocation design (separate branch, design
     work in `PRODUCTION_RELOCATION_SOURCE_MAPPING.md`,
     `RELOCATION_DELETE_OWNERSHIP.md`,
     `PREFIX_LOCALITY_VS_RELOCATION.md`).

Awaiting reviewer feedback on OQ-A, OQ-C, OQ-D.

— @r2d2
