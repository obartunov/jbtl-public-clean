# Layer 1 — review package notes

Cover document for the production Layer 1 sliced jsonb read
patch series. This is the file a reviewer should open first; it
points at the spec, the implementation, the empirical result,
and lists what is in / out of scope.

Branch: `r2d2/layer1-sliced-read`.
Verdict: **LAYER1_READY_FOR_CODE_REVIEW**.

## 1. Patch series in review order

```
417a6cff97  docs: sliced jsonb read — Layer 1 specification
28c8b7fa5c  jsonb: sliced read for jb -> 'key' on external bodies (Layer 1)
47e738f66f  docs: Layer 1 sliced read — implementation result
079e0b382d  jbtl: audit comment on latent fetch_len pad over-fetch
b8e3c6f922  docs: fix size-vs-id calibration in SLICED_JSONB_READ_RESULT §4
```

Plus the §10.2 acceptance refinement (this batch) — to be
folded into the spec and result commits or applied as a single
"§10.2 case split" commit, reviewer's choice.

Code change is one commit, `28c8b7fa5c`. The rest are docs and
one no-behavior-change JBTL comment. Total code diff: +466 lines
across three files:

  - `src/backend/utils/adt/jsonb_util.c`  +391
  - `src/backend/utils/adt/jsonfuncs.c`    +73
  - `src/include/utils/jsonb.h`             +4

No deletions of existing code paths. The helper is invoked
behind a `VARATT_IS_EXTERNAL_ONDISK` gate that falls through to
the existing path when "not handled."

## 2. Acceptance criteria — the three-case split

The spec's §10.2 was revised because its original wording
("P0 reproduces J0 within counter noise") was implicitly
assuming an API that the spec deferred in §7. The corrected
acceptance has three sub-cases the helper genuinely
distinguishes.

### Case A — prefix-resident scalar

Trigger: any external body, sort-on or sort-off, where the
value sits inside the structural-prefix slice the helper
already fetched.

Helper path: no second fetch; materialise from prefix.
Expected: reproduces J0 numbers, may be slightly better
(no extension dispatch overhead).

Evidence: at id=100 key3 with sort on, P10 = 4.37 buf/call vs
J10 = 7.90. Production is **better** than JBTL-A here because
the extension wrapper overhead is gone.

### Case B — uncompressed external, value past prefix

Trigger: `attstorage = external` (uncompressed) body, value's
byte offset past the structural-prefix slice.

Helper path: second `detoast_attr_slice` at the value's offset
fetches only the value's bytes.

Expected: reproduces J0 numbers within noise. Both reach the
value through the same mechanism — arbitrary-range slice into
uncompressed TOAST.

Evidence: cold-cache big_ext k3 (600 KB body): Layer 1 reads
6 disk pages in 0.172 ms vs baseline 80 disk pages in 1.150 ms.
6.7× latency win, 3.8× fewer reads.

### Case C — compressed external, value past prefix

Trigger: `attstorage = extended` (compressed) body, value past
the structural-prefix slice.

Helper path: §7 fallback. The helper sets `out_handled = false`
because `detoast_attr_slice` asserts `sliceoffset == 0` for
compressed externals — it cannot fetch from the middle of a
compressed body.

Expected: reproduces **F0**, not J0. Equality with F0 is the
correct acceptance for this case. JBTL-A reaches J0 here only
because its own storage wrapper has chunked compression with
per-chunk decompression boundaries (see §3 below).

Evidence: cold-cache real_cmp k3 (2 MB body, 23 KB on-disk
compressed): Layer 1 takes 1.437 ms vs baseline 1.543 ms.
No regression; no win. As specified.

## 3. Production Layer 1 vs JBTL-A

These two read paths look superficially similar but rest on
different storage contracts. Reviewers should not expect
identical numbers across them in every case.

JBTL-A owns chunked compression. The JBTL toaster writes its
own on-disk format where compressed bodies are broken into
independently-decompressable chunks, each with its own per-
chunk metadata. As a consequence the JBTL reader can slice an
arbitrary byte range out of a compressed body: it reads only
the chunks the range intersects, decompresses just those, and
returns the requested bytes. This is what gives J00 its flat
1053 buf/call regardless of where the requested value lives
in the body.

Production Layer 1 owns only what core TOAST provides. The
core TOAST API is `detoast_attr_slice(attr, offset, len)`.
For uncompressed external it supports arbitrary `offset`. For
compressed external it asserts `offset == 0` — the
implementation walks the streaming pglz/lz4 decoder from byte
zero on every call, so non-zero offsets cannot be served
without re-decoding everything before the offset anyway.

This is not a design oversight in production Layer 1. The
helper is correct under the API it has. The asymmetry between
Case B (uncompressed past-prefix → Layer 1 wins) and Case C
(compressed past-prefix → Layer 1 falls back) is a direct
consequence of the core TOAST API. Closing Case C requires
one of two things:

  (a) a core TOAST API change (e.g. an opt-in chunk-aligned
      compression mode, or a per-chunk decompression entry
      point) — explicitly out of scope of this task and
      flagged in spec §7 as "Optimisation deferred to a later
      task";

  (b) Layer 2 relocation, which sidesteps the problem by
      moving large values out of the parent body into their
      own TOAST chains. After relocation, the parent body is
      small enough that the structural prefix covers
      everything, so Case C does not arise — the value either
      sits in the prefix (Case A) or in a child TOAST chain
      that the relocation-aware reader fetches directly.

Note that production Layer 1 **beats** JBTL-A in Case A
(prefix-resident scalar, both can serve, but JBTL pays
dispatch overhead). The advantage in Case A is direct: Layer 1
lives in core and has no extension wrapper. Reviewers should
read this as "Layer 1 covers the cheap cases optimally; JBTL-A
extends coverage to compressed-past-prefix at the cost of an
extension wrapper."

## 4. What changed during implementation

Two bugs caught during corner-case testing before benchmark.
Both detailed in `docs/SLICED_JSONB_READ_RESULT.md` "Bugs found
during implementation." Brief summary for reviewer context:

  - **Bug 1**: helper used `VARATT_EXTERNAL_GET_EXTSIZE`
    (on-disk compressed size) where it needed
    `va_rawsize - VARHDRSZ` (uncompressed body size) for
    in-body bounds checks. Affects compressed external only;
    silently disabled the helper for that case until fixed.

  - **Bug 2**: `fetch_len = value_len + pad` over-fetched by
    0..3 bytes on numeric values because the writer's JEntry
    length already includes the pad. Affects cold-cache
    compressed-external numeric-scalar-near-body-end only;
    raised ERRCODE_DATA_CORRUPTED until fixed to
    `fetch_len = value_len`. Same line exists in JBTL but is
    harmless there because JBTL's slice dispatch has tolerant
    bounds. See §5 below.

Both bugs were caught by a 160 KB compressed-external corner-
case fixture (`p1_iso`, key3 = numeric 42 near body end). They
were **not** triggered by the warm matrix loop because that
loop amortises the first cold call over 30-200 reps; the bug
condition needs the first call against a stricter bounds
check.

This finding led to three follow-ups, all landed:

  - bench-suite cold-cache mode (`fb046f7` on bench-suite, a
    new `--mode cold` that does single-call EXPLAIN BUFFERS
    with server restart between samples);
  - JBTL audit comment (commit `079e0b382d`, see §5 below);
  - calibration fix in result doc §4 (commit `b8e3c6f922`,
    correcting a misleading sentence about id ranges).

A memory rule recording the general lesson:

> Lenient prototype tolerates over-fetch silently; strict
> production raises CORRUPTED on same bytes. Cold single-call
> near body end exposes what warm rep-loop amortises — harness
> without that path is blind.

## 5. JBTL bug-2 — audit only, no behavior change

JBTL contains the same `fetch_len = value_len + pad` line.
There it is harmless: JBTL's `jbtl_fetch_slice_dispatch` does
not bounds-check the returned slice strictly, and
`fillJsonbValue` reads only `value_len - padlen` bytes from
INTALIGN(offset), so the over-fetched tail bytes are never
observed.

Decision (commit `079e0b382d`): leave JBTL behavior as-is,
add an explicit AUDIT comment and a sanity Assert that fires
in dev builds only when the over-fetch would step past
attrsize. Rationale:

  - changing JBTL behavior would shift recorded J-cell
    numbers in every published matrix by 0..1 buffer pages
    per call (chunk-alignment-dependent). Invalidating
    historical baselines is not worth the cleanup;
  - the Assert puts a tripwire on any future tightening of
    `jbtl_fetch_slice_dispatch`. If that ever happens, the
    Assert fires reliably and the line must be migrated to
    `fetch_len = value_len`;
  - future maintainers have the audit trail (commit message
    + inline comment + Assert) and the production fix to
    cross-reference.

No JBTL behavior change is part of this review.

## 6. What is NOT in this patch

Explicit non-scope:

  - **OQ-B: subscripting / jsonpath integration.** The helper
    signature is compatible with future `jsonbsubs.c` routing
    (same Datum-level gate, same `out_handled` contract), but
    no subscripting code is in this patch. Marked as follow-up
    in spec §Open Questions. Anticipating the vanilla
    reviewer's first response ("why doesn't subscripting use
    this?"): the answer is "next patch," not "no plan."

  - Relocation (Layer 2). Separate design and patch series,
    work in progress in PRODUCTION_RELOCATION_SOURCE_MAPPING.md,
    RELOCATION_DELETE_OWNERSHIP.md, PREFIX_LOCALITY_VS_
    RELOCATION.md. Layer 1 is the read primitive Layer 2 also
    calls into; landing Layer 1 first sharpens Layer 2's
    acceptance criteria.

  - Modify/UPDATE/WAL behavior. Out of scope.

  - Changes to `jsonb_sort_field_values` default. Out of scope.
    Layer 1 works correctly with the GUC off (its current
    default) and benefits from it being on (Case A becomes
    more frequent). The default-flip decision is a separate
    compatibility task tracked in
    `docs/PREFIX_LOCALITY_VS_RELOCATION.md` §5b.

  - Any TOAST API change to enable Case C wins (compressed
    external arbitrary-range slice). Deferred in spec §7.

## 7. Review checklist

Suggested order for a reviewer:

  1. Read `docs/SLICED_JSONB_READ.md` (the spec) — start with
     §4 (proposed sliced-read path), §5 (prefix algorithm),
     §6 (value algorithm), §7 (compression / fallback table).
     §10 has the three-case acceptance.

  2. Read commit `28c8b7fa5c` jsonb_util.c addition. The
     helper function is ~270 lines including comments. Key
     review points:
       - body_size derivation (Bug 1 site);
       - prefix extension stages 1-4;
       - inline binary search via `lengthCompareJsonbString`
         and KVMap translation;
       - half-body heuristic at stage 8;
       - fetch_len = value_len (Bug 2 fix);
       - all bounds-checks against body_size, not attrsize;
       - "not handled" out-flag set in every fallback case.

  3. Read commit `28c8b7fa5c` jsonfuncs.c integration. Two
     parallel sites in `jsonb_object_field` (line 917) and
     `jsonb_object_field_text` (line 992). The text site
     previously had no Toastapi hook either — Layer 1 closes
     this asymmetry as a side effect.

  4. Read `docs/SLICED_JSONB_READ_RESULT.md` for empirical
     evidence: factorial matrix data and cold-cache baseline
     comparison.

  5. Cross-check against JBTL reference in
     `contrib/jsonb_toaster_lite/jsonb_toaster_lite_object_field.c`
     `jbtl_toast_fetch_object_field` (the algorithm mirrors
     this; production differs only in (a) using
     `detoast_attr_slice` directly, (b) strict bounds, (c)
     `body_size` not `attrsize`, (d) `fetch_len = value_len`
     not `+pad`).

## 8. Open questions for the reviewer

OQ-A. §10.2 wording revision applied in this batch — please
confirm the three-case split correctly captures spec intent.

OQ-B. Subscripting integration timing: same patch series
before vanilla submission, or follow-up after Layer 1 lands?
The vanilla reviewer will almost certainly ask.

OQ-C. JBTL bug-2 migration trigger condition. The Assert
remains as a tripwire. Confirm the decision to leave it
audit-only.

OQ-D. The mid-size buffer regression from
`PREFIX_LOCALITY_VS_RELOCATION.md` Test 2 was a JBTL-A
property (extension dispatch on small-but-external bodies),
not a sliced-read property in general. Production Layer 1
inherits F0's profile in that range (no extension dispatch).
This counterfactual strengthens the original argument; no
action needed, flagged for Layer 2 design.

## Verdict

**LAYER1_READY_FOR_CODE_REVIEW.**

Implementation is correct, complete relative to scope, with
empirical evidence covering all three Case A/B/C sub-cases.
Bug investigation closed, audit items applied, JBTL trail left
clean. No outstanding fix-it items inside the patch boundary.

Next step is whatever you (yoda) signal: continue to Layer 2
design, start the subscripting follow-up, or open the upstream
discussion thread on pgsql-hackers.

— @r2d2
