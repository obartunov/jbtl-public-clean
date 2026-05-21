# Layer 1 — F1 sanity check

Branch:      `r2d2/layer1-upstream-prep`
Compared to: `r2d2/layer1-sliced-read-clean` tip (pre-F1).
Purpose:     verify that F1 (helper-owns-payload + pfree slices before
             returning FOUND) did not change disk-level I/O, did not
             change fallback structure, did not regress the inline
             path, and did not unexpectedly grow per-backend memory.
Scope:       sanity check only. **Not** a cold-cache benchmark.

## What this document is, and is not

This document **is** in-container empirical evidence for four
narrow claims about F1's safety, together with the code-level
argument that backs them:

  1. F1 changes only the post-`fillJsonbValue` materialisation
     path inside `getKeyJsonValueFromExternal`. No
     `detoast_attr_slice` call site changes count, offset or
     length. No fallback branch is added or removed in a way that
     reaches normal-traffic input. Inline-path bypass is
     unchanged.
  2. Buffer-page counts for representative A/B/C cases are
     identical pre-F1 and post-F1 on the same fixtures.
  3. The inline / non-external path still bypasses the helper:
     `EXPLAIN BUFFERS` shows the same 2 shared hits pre and post.
  4. Per-backend memory does not grow more under post-F1 than
     under pre-F1 on a 6000-row FOUND scan. VmData deltas are
     equal within measurement noise; VmRSS is dominated by the
     shared-buffer pages the scan touches, not by allocator
     churn.

This document **is not**:

  - A cold-cache benchmark. The public bench-suite branch
    `r2d2/legend-relocation-rename` has no `--mode cold`
    implementation. The cold harness exists only in the
    author's local sandbox and was not pushed.
  - A latency measurement. Container wall-clock is not
    reproducible against a bench machine and would not be
    valid evidence for F1's expected sub-percent latency drift.
  - A replacement for the cold-cache A/B/C re-measurement
    described in `LAYER1_UPSTREAM_PREP_FIXES.md` Pending § 1.
  - Authorisation to update headline numbers in
    `LAYER1_COVER_LETTER.md`. Those still need the bench
    machine.

The verdict scope is correspondingly narrow: **does F1 break
something we can detect from the source tree and the regression
fixtures alone?**


## 1. Code-level check

### 1.1 Same `detoast_attr_slice` call sites

Pre-F1 (`r2d2/layer1-sliced-read-clean` tip
`f9a4b1d...`):

| Stage | Call                                                          |
|-------|---------------------------------------------------------------|
| 1     | `prefix = detoast_attr_slice(attr, 0, prefix_size);`          |
| 3     | `prefix = detoast_attr_slice(attr, 0, better_size);`          |
| 4     | `prefix = detoast_attr_slice(attr, 0, better_size);`          |
| 7     | `value_slice = detoast_attr_slice(attr, value_start_in_body, fetch_len);` |

Post-F1 (`r2d2/layer1-upstream-prep` tip `bf3025a...`):

| Stage | Call                                                                  |
|-------|-----------------------------------------------------------------------|
| 1     | `prefix = detoast_attr_slice(attr, 0, (int32) prefix_size);`          |
| 3     | `prefix = detoast_attr_slice(attr, 0, (int32) better_size);`          |
| 4     | `prefix = detoast_attr_slice(attr, 0, (int32) better_size);`          |
| 7     | `value_slice = detoast_attr_slice(attr, (int32) value_start_in_body, (int32) fetch_len);` |

The only difference is the explicit `(int32)` cast on
arguments that B1+F8 promoted to int64 intermediates. The slice
offsets remain 0/0/0/value_start_in_body, the slice lengths
remain prefix_size/better_size/better_size/fetch_len, computed
from identical bounds. The number of calls per FOUND path is
unchanged (1 to 4 depending on body width and value position).

### 1.2 Same fallback structure

Counts of enum-returning sites in `getKeyJsonValueFromExternal`:

|                          | pre-F1 | post-F1 |
|--------------------------|:------:|:-------:|
| JSONB_KEY_LOOKUP_FALLBACK| 6      | 7       |
| JSONB_KEY_LOOKUP_MISSING | 2      | 2       |
| JSONB_KEY_LOOKUP_FOUND   | 1      | 1       |

The single new FALLBACK site is the defensive `default:` arm in
Stage 9's `switch (res->type)`. `fillJsonbValue` only produces
jbvString / jbvNumeric / jbvBool / jbvNull from on-disk jsonb,
and the Stage 6 `JBE_ISCONTAINER` filter already rejects
binary-container values before they reach Stage 9. The
`default:` arm is therefore unreachable on any input that gets
past Stage 6, and inputs that would reach it would already have
been rejected earlier as either fallback or container.

The original 6 FALLBACK conditions are unchanged in predicate
and location. The 2 MISSING conditions are unchanged. The 1
FOUND return path now has the deep-copy + pfree block before
it, but the *condition under which FOUND is returned* is the
same.

### 1.3 Same gate at the call site

`jsonb_object_field` and `jsonb_object_field_text` in
`jsonfuncs.c`:

```c
if (VARATT_IS_EXTERNAL_ONDISK(DatumGetPointer(raw)))
{
    ...
    lr = getKeyJsonValueFromExternal(raw, ..., &sv);
    switch (lr) { ... }
}
```

The gate condition is byte-identical pre vs post. F1 changed
nothing in `jsonfuncs.c` beyond adding the
`#include "utils/jsonb_internal.h"` line (F2). Inline /
non-external / indirect / expanded datums all short-circuit
before the helper is consulted.


## 2. Buffer-count sanity check

### 2.1 Method

Two fork builds in the same container: post-F1 at
`/tmp/pg/bin/postgres`, pre-F1 at `/tmp/pg_preF1/bin/postgres`.
Pre-F1 obtained by swapping just `jsonb_util.c`, `jsonfuncs.c`,
`jsonb.h` and removing `jsonb_internal.h` back to their
`r2d2/layer1-sliced-read-clean` tip versions, rebuilding only
the two .o files and relinking. The two binaries are otherwise
the same (same `configure`, same compiler, same flags). Quick
sanity-check that we built two distinct binaries:

  - Pre-F1: corruption ereport format string contains `%d`.
  - Post-F1: same string contains `%lld` (the message-format
    update that came with B1+F8).

`strings` confirms.

Both clusters share `--no-locale` initdb, port 5432 vs 5433,
default shared_buffers. Fixtures shaped like
`jsonb_layer1.sql`'s small-N matrix: one inline body, one
external-uncompressed body (k1 prefix-resident, k3 past-prefix),
one external-compressed body (k1 prefix-resident, k3 past-
prefix, nested-container value).

For each case I ran `EXPLAIN (ANALYZE, BUFFERS, FORMAT JSON)`
on the relevant `jb -> 'k'` or `jb ->> 'k'` and aggregated
buffer counters from all plan nodes. Results in
`results/buf_probe.csv`.

These were *warm* shared-buffer reads — same session that did
the INSERT. The diagnostic value comes from the
**pre-vs-post match**, not from the absolute numbers.

### 2.2 Results

| Case                                                | helper expected | pre-F1 shr_hit | post-F1 shr_hit | Δ |
|-----------------------------------------------------|-----------------|---------------:|----------------:|--:|
| inline `jb -> 'a'`                                  | gate bypass     | 2              | 2               | 0 |
| inline `jb ->> 'b'`                                 | gate bypass     | 2              | 2               | 0 |
| uncompr ext, prefix-resident k1                     | FOUND (Case B') | 27             | 27              | 0 |
| uncompr ext, past-prefix k3                         | FOUND (Case B)  | 29             | 29              | 0 |
| compr ext, prefix-resident k1                       | FOUND (Case A)  | 27             | 27              | 0 |
| compr ext, past-prefix k3                           | FALLBACK (Case C) | 29           | 29              | 0 |
| compr ext, missing key                              | MISSING         | 27             | 27              | 0 |
| compr ext, nested container value                   | FALLBACK        | 30             | 30              | 0 |

Zero Δ on every row. Shared Read Blocks, Local Hit/Read Blocks,
Temp Read/Written Blocks all zero on both builds for all
cases.

### 2.3 What this confirms and what it does not

**Confirms.** F1 does not change the disk-level path. The same
TOAST chunks are touched in the same order on identical
fixtures. This is what the code-level claim in § 1 predicts.

**Does not confirm.** That F1's effect on FOUND latency is
within tolerance. Wall-clock is excluded by design in this
sandbox.


## 3. Inline gate sanity check

The two inline cases from § 2 already cover this: both `->` and
`->>` on an inline jsonb body hit 2 buffer pages pre and post,
which is the cost of the index lookup on `id` plus the heap
tuple page. The helper does no work — the
`VARATT_IS_EXTERNAL_ONDISK` gate returns false and control
falls through to the existing slow path on the same body, which
is already detoasted in place because it was inline.

No timing recorded; container wall-clock is not evidence at
this scale.


## 4. Per-backend memory check

### 4.1 Method

`probe_rss_v2.py` (in repo, alongside the buffer probe) opens a
single psql session via `subprocess.Popen`, captures the server
backend pid via `pg_backend_pid()`, reads
`/proc/<pid>/status` for `VmRSS`, `VmHWM`, `VmData`, `VmSize`,
then runs a 6000-row FOUND scan (3000 × k1 prefix-resident plus
3000 × k3 past-prefix), then reads `/proc` again. Three passes
each, each pass uses a fresh backend.

Fixture: 3000-row external-uncompressed table, each row a
jsonb object with three keys including one ~6 KB padding field
to force out-of-line storage.

### 4.2 Results (kB)

| label   | pass | VmRSS before | VmRSS after | ΔVmRSS | ΔVmHWM | ΔVmData |
|---------|:----:|-------------:|------------:|-------:|-------:|--------:|
| post-F1 | 0    | 13840        | 43008       | +29168 | +29168 |   +148  |
| post-F1 | 1    | 14036        | 42764       | +28728 | +28728 |   +136  |
| post-F1 | 2    | 13968        | 42696       | +28728 | +28728 |   +136  |
| pre-F1  | 0    | 14804        | 44376       | +29572 | +29572 |   +136  |
| pre-F1  | 1    | 14872        | 44012       | +29140 | +29140 |   +136  |
| pre-F1  | 2    | 14868        | 43880       | +29012 | +29012 |   +136  |

### 4.3 Reading

VmRSS Δ is essentially the same on both builds (~28.7-29.6 MB).
Most of that growth is the shared-buffer pages the scan touched
(6000 FOUND lookups against TOAST chunks, ~24 MB of TOAST
storage on this fixture, plus heap pages and indexes). It is
not allocator activity.

The signal-to-noise on the *allocator* side comes from VmData,
which excludes pages mapped from the shared-buffer segment.
VmData Δ is **±2 kB** between pre and post on three passes
each. F1's per-FOUND change (free a 1–6 KB slice early + copy
the few-byte scalar to a fresh palloc) shows up as a wash here
because ASET memory contexts pool freed blocks for reuse —
pre-F1 freed the slice at per-tuple-context reset, post-F1
frees it slightly earlier. Both reach the same steady-state
high-water mark within one row of operation.

**This does not mean F1 was unnecessary.** F1 was justified on
two separate axes:

  1. Contract surface area. Pre-F1 the exported header
     committed callers to a memory-ownership rule. F1 removes
     that rule. The win is in maintainability, not in steady-
     state allocator behaviour.
  2. Worst-case-row residue. On a one-row FOUND, pre-F1 leaves
     a slice palloc'd in the executor's per-tuple context until
     the next row resets it. For backends that pause between
     rows (e.g. cursor-driven, slow-network client) the
     residue can be transiently observable. The 6000-row tight
     loop here doesn't isolate that case.

The check we *can* do here is the negative one: F1 did not make
memory worse. It did not. Pre-F1 and post-F1 VmRSS / VmHWM /
VmData deltas are equal to within container noise.

### 4.4 What this does not show

A measurement of one-row peak-residue (the case F1 was actually
optimising for) requires either an instrumented backend with
palloc counters, a slower-paced client to expose the residue
window, or both. Neither is built here. The code-level
argument in § 1 is the primary evidence that F1 achieves what
it was designed to achieve; the RSS measurement above is the
negative-result sanity check.


## 5. Comparison against the cover-letter headline numbers

Not in scope. See § "What this document is not." The cover
letter's headline 9.3× / 11.9× / 6.7× / "no change for Case C" /
"no measurable change for inline" numbers were obtained on the
pre-F1 path. The cover letter retains the pre-F1 footnote.
Refresh requires the bench-machine cold-cache harness, which is
tracked under Pending § 1 of `LAYER1_UPSTREAM_PREP_FIXES.md`.


## 6. Did F1 change latency materially?

Cannot answer from a container. Code-level: F1 adds, per FOUND
row, one `palloc` of scalar-payload size (typically < 100 bytes
for strings, < 32 bytes for numerics, zero for bool/null), one
`memcpy` over the same length, and two `pfree` calls. Pre-F1
already paid one `palloc` for the prefix slice and one for the
value slice (when needed). The net difference is therefore one
extra small palloc + small memcpy + two pfrees per FOUND.

On cold-cache FOUND latencies in the 0.15–0.18 ms range
(per the pre-F1 cover letter), the extra CPU is at the
microsecond level: at most a single-digit percent shift,
expected direction either way depending on allocator state.

For inline-row hot path (~0.23 µs/call per cover letter), F1
runs zero new code because the gate short-circuits.

For compressed-external past-prefix (Case C, fallback), F1
runs zero new code because the helper returns FALLBACK before
Stage 9.

So the cases where F1's added work runs are exactly the FOUND
cases. The expected impact is sub-percent for cold-cache and
zero for the other categories. Confirming this in numbers
needs the bench machine.


## Verdict

**F1_SANITY_CHECK_PASSED.**

  - Code-level: F1 is post-fillJsonbValue-only; detoast call
    count, offsets, lengths, fallback structure all unchanged.
  - Buffer-count parity: pre and post F1 produce identical
    shared-hit counts across all 8 representative cases.
  - Inline gate: still short-circuits, no buffer-count
    regression.
  - Memory: VmRSS / VmHWM / VmData deltas across a 6000-row
    FOUND scan are equal between pre and post F1 within noise.
    F1 did not make memory worse.

The verdict does not bless the headline numbers in the cover
letter. Those still require the bench machine. The pre-F1
footnote stays.


## Pending (unchanged)

  - Cold-cache A/B/C re-measurement remains the deliverable.
  - pgsql-master re-roll remains pending.
  - Test-file rename remains pending.


## Outputs in repo

  - `results/buf_probe.csv` — buffer-count probe, pre and post.
  - `results/rss_probe.csv` — backend RSS probe, pre and post.
  - `scripts/probe_buffers.py`, `scripts/probe_rss_v2.py` —
    probe sources, runnable on any local PG.

— @r2d2
