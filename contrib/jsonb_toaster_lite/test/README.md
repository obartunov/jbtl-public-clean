# jsonb_toaster_lite L1.3-bench / L1.3b-bench / L1.4

Three physics/perf snapshots, sharing the same harness pattern.

## L1.3 — artificial extremes (storage & slice path validation)

`L13_bench.sql` + `L13_bench_run.sh`.

Validates that the basic primitives work end-to-end:
sliced fetch, per-chunk pglz, raw fallback.  Payloads chosen to
isolate each behavior, not to look like real JSONB.

See "Reference numbers (artificial extremes)" further below.

## L1.3b — realistic JSONB shape (read-path gap readout)

`L13b_realistic_bench.sql` + `L13b_realistic_bench_run.sh`.

A document with realistic field-size mix: several small fields
(`id`, `status`, `kind`, `tenant`), a small array (`flags`), two
medium-size text fields (`title` ~200 chars, `summary` ~1000 chars),
and one large heterogeneous field (`payload` ~48 KB md5-like with a
prefix marker).  Stored under K1's `jsonb_sort_field_values = on` so
small fields land at the front of the binary body and large at the
end.

### What this bench is for

L1.3 showed that variant B (plain chunks) and variant C (per-chunk
pglz) save chunks on **byte-level** slice requests.  But real
applications use `j->'key'` and `j->>'key'`, not byte offsets.
L1.3b measures what those operators cost on each variant, and
documents the read-path gap they reveal.

### Honest gap statement (as of L1.3b; closed by L1.4 — see below)

Before L1.4, jsonb_toaster_lite did NOT expose a key-level sliced
read.

All operator paths (`->`, `->>`, `jsonb_path_query`, ...) went
through core's `detoast_attr`, which calls our `tsr_detoast` with
`offset=0` and `length=-1` — i.e., a full detoast.  For B that
meant `jbtl_toast_fetch_full_plain` reading every chunk; for C,
`jbtl_toast_fetch_compressed_chunks(slicelength=full)` reading
and decompressing every chunk.  Neither variant looked at the K1
KVMap offsets in the jsonb header to skip chunks the requested
key did not occupy.

**L1.4 added `jbtl_object_field` and `jbtl_object_field_text` SQL
functions that close this gap for the top-level-object-scalar-
value case.**  See the L1.4 section further down for benchmark
numbers showing 1-of-25 chunks fetched for small fields.  The
`->>` operator itself is not yet rewired; users opt in by calling
the new functions directly.  L1.3b's text below is preserved
because the byte-slice primitive remains the only sliced read for
non-key access paths (e.g., direct byte-offset reads).

The only sliced primitive available without L1.4 is
`jbtl_slice_probe(j, byte_offset, byte_length)`: the caller must
know byte offsets a priori.  `jbtl_chunk_inspect` can list the
on-disk chunk byte ranges, and the `key_layout` helper inside
`L13b_realistic_bench.sql` demonstrates how to compute the
chunks-overlap set for a given key manually.

### Reference numbers (debug build, BLCKSZ=8192, ASSERTS=on)

These are noisy on this build (single-machine, no isolation; ±20 %
seen between consecutive runs).  The ratios survive the noise; the
absolute numbers do not.

Median ms over 5×200 ops:

```
      op      | A (default) | B (plain) | C (compressed)
--------------+-------------+-----------+----------------
 full         |    ~120     |   ~115    |    ~120
 key_small    |     ~24     |    ~25    |     ~24
 key_medium   |     ~28     |    ~25    |     ~27
 key_large    |     ~31     |    ~30    |     ~37
 byte_slice   |     ~33     |    ~22    |     ~35
```

Storage:

```
 variant | column_size | toast_rel_bytes | chunks_total | chunks_compressed
---------+-------------+-----------------+--------------+-------------------
 A       |       45137 |           49152 |              |
 B       |          36 |           57344 |           25 |                 0
 C       |          36 |           57344 |           25 |                 1
```

Probe counters:

```
            op            | chunks_total | chunks_fetched
--------------------------+--------------+----------------
 B_full                   |           25 |             25
 B_byte_slice [0,100)     |           25 |              1   <- skip 24
 C_full                   |           25 |             25
 C_byte_slice [0,100)     |           25 |              1   <- skip 24
```

Physical layout of key markers (computed against B's on-disk layout):

```
   key   | marker_offset | value_size | chunks_overlap
---------+---------------+------------+-------------------
 status  |           171 |         23 | {0}
 kind    |           149 |         22 | {0}
 tenant  |           128 |         21 | {0}
 title   |           246 |        216 | {0}
 summary |           462 |       1018 | {0}
 payload |          1480 |      48018 | {0..24}      <- spans all chunks
```

### What the numbers say

1. **Full read is parity across A/B/C** (within 10 %).  No claim of
   "B/C wins on real JSONB" is supported by these numbers.

2. **Key access takes the full-detoast path on every variant.**
   B and C have chunk-skipping infrastructure that is not engaged.
   `j->>'status'` reads all 25 chunks even though the value lives
   in chunk 0 alone.

3. **C is *not* visibly more expensive than A on key access** for
   this document, because only 1 chunk out of 25 ended up
   compressed — the md5-like `payload` resisted per-chunk pglz.
   Decompression overhead is small.

4. **byte_slice shows the gap concretely.**  B byte_slice [0, 100)
   reads 1 chunk, takes ~22 ms.  B full read reads 25 chunks, takes
   ~115 ms.  ~5× speed-up on the chunk-skipping path that *does*
   work.

5. **Physical layout under `jsonb_sort_field_values=on` is favorable
   for KVMap-aware fetch on small/medium fields:** `status`,
   `kind`, `tenant`, `title`, `summary` all live in chunk 0.  A
   future KVMap-aware fetch could read just chunk 0 for those keys
   — same 25-to-1 chunk reduction the byte_slice currently exhibits.
   `payload` is the exception: it spans all 25 chunks because it's
   ~48 KB.  KVMap-aware fetch wouldn't help for `payload`-shaped
   fields.

### Decision support for L1.3 / next-step

Three candidate features were on the table:

  (a) DIFF / update (partial rewrite on UPDATE)
  (b) key-level sliced lookup using KVMap offsets
  (c) per-field toasting

This bench argues for (b) before (a):

  - Without (b), the chunk-skipping read path is unreachable from
    realistic SQL.  All `j->'key'` queries pay the full-detoast
    cost.
  - DIFF (a) is a write-side feature.  Without (b), reading the
    old chunks for partial-rewrite still goes through full detoast,
    which limits any DIFF gain.
  - Per-field toasting (c) is a more invasive storage-format
    change that subsumes (b)'s mechanics.  (b) is the smaller,
    safer, falsifiable step on the same path.

The recommendation is to add (b) next: a KVMap-aware read entry
point that takes a key name and uses the in-header offset table
to compute a `(byte_offset, byte_length)` for the value, then
delegates to the existing `jbtl_toast_fetch_*_chunks` slice
machinery.  After (b) lands and is bench'd, DIFF (a) becomes the
clear next.

## Common: how to run

```sh
contrib/jsonb_toaster_lite/test/L13_bench_run.sh
contrib/jsonb_toaster_lite/test/L13b_realistic_bench_run.sh
```

Each script recreates a clean DB (`jbtl_bench_db` by default), runs
the corresponding `.sql`, captures output to `/tmp/L13*_bench_<ts>.out`,
and drops the DB.

Environment overrides: `PG_BIN`, `PG_HOST`, `PG_PORT`, `PG_USER`,
`DB_NAME`, `OUT_FILE`.

## Acceptance criteria summary

L1.3 (artificial extremes) — see L1.3-bench commit.

L1.3b (realistic JSONB shape):

  - bench runs repeatably (qualitative ratios hold across runs;
    absolute numbers vary ±20 % on a debug build with no isolation,
    documented). ✓
  - measured the available slice primitives. ✓
  - explicitly documented that key lookup falls back to full
    detoast: see "Honest gap statement" above and the NOTE block at
    the end of `L13b_realistic_bench.sql`. ✓
  - exposed physical offsets of selected fields via `key_layout()`
    helper. ✓
  - decision support produced: (b) key-level sliced lookup is the
    natural next read-path feature; DIFF (a) should follow it. ✓

## L1.4 — KVMap-aware top-level object field lookup

The gap that L1.3b documented is now closed for top-level object
field reads.  `jbtl_object_field(jb, key) -> jsonb` and
`jbtl_object_field_text(jb, key) -> text` parse the on-disk
container header / 2N JEntries / KVMap / key area from a small
prefix slice, binary-search for the key (KVMap-redirected to the
physical value index), and fetch only the byte range the value
occupies.

Out of L1.4 scope (caller falls back to core's full-detoast +
`jsonb_object_field*`):
  - non-object roots (scalar, array)
  - nested-container value types (object/array)
  - JBTL_PLAIN_JSONB inline jsonb_toaster_lite values
  - default-toaster jsonb (non-CUSTOM varlena)
  - threshold guard: when the value occupies more than half the
    body, the prefix-fetch + value-fetch combined would touch more
    chunks than a single full detoast; we fall back instead.

### Headline result — first real read-path product proof

The product win lives in **key3**, not key1.

The canonical Bartunov-Glukhov shape (slide 30 of "One TOAST fits
all") is `{key1: small, key2: HUGE, key3: small, key4: medium}`.
Two of those four keys are the real test:

  - **key1** is degenerate.  In ordinary (non-KVMap) jsonb its
    JEntry is the first one and its scalar value is also the
    first physical byte after the JEntry / key area, i.e. it
    already lives in the first toast chunk regardless of
    layout.  L1.4 fast-pathing this key would be fast even
    without K1.  Use key1 only as a control.

  - **key3** is the clean case.  Logically it sits between two
    big values (key2 huge array, key4 medium array).  Without
    KVMap layout its scalar value is laid out in JEntry order:
    `int(key1) | HUGE_ARRAY(key2) | int(key3) | array(key4)`,
    so key3's bytes physically sit *after* the huge key2 and
    land in late toast chunks.  KVMap layout reorders the
    physical area so small values come first; key3's scalar
    moves into chunk 0.  Reading key3 from a single prefix
    slice is the proof that the K1 layout actually delivers
    on a slice path.

At the largest jsonb in the sweep (~396 KB):

```
key3 (clean KVMap test)
  vanilla j->'key3'                   562 us
  jsonb_toaster_lite + L1.4 fast      22 us
  speedup                             25.3x

key1 (degenerate control)
  vanilla j->'key1'                   572 us
  jsonb_toaster_lite + L1.4 fast      22 us
  speedup                             26.0x      <-- looks similar but
                                                     would be similar even
                                                     without K1 KVMap
key2, key4 (HUGE / medium array containers)
  L1.4 fast path falls back via JBE_ISCONTAINER and tracks vanilla.
  These are pure containers; partial detoast cannot return them.
  Use as fallback control only.
```

Both scalar keys are flat at ~22 us across the full size range
(512 B inline through 396 KB toasted, three orders of magnitude).
That flatness is the central claim of partial detoast.

### Decomposition (matrix bench, 2 clusters)

The matrix bench (`L14_yoda_run.sh` + `L14_yoda_one.sql` +
`L14_yoda_plot.py`) runs five cells side-by-side across two
PostgreSQL installs:

```
  vanilla cluster (port 5434)   /root/pg-install-vanilla
    PG 19devel master @ 8d829f5, no K1, no toastapi,
    no jsonb_toaster_lite

  patched cluster (port 5433)   /root/pg-install
    PG 19devel + K1 + toastapi + jsonb_toaster_lite
```

Both built with the same configure flags
(`--enable-cassert`, `-O0`, debug); both clusters use
`shared_buffers=128MB`, `work_mem=4MB`, `jit=off`.  Data shape and
generator seed are byte-identical across cells.  100 i values
(~512 B inline through ~396 KB toasted), 1000 op repeats per
timed SELECT, eviction (base + toast + PK index) before each
call.

```
  cell  cluster   storage                            sort_GUC
  ----  -------   --------                           --------
  A     vanilla   default toast, ordinary jsonb      n/a
  B     patched   default toast, ordinary jsonb      off
  C     patched   default toast, ordinary jsonb      on
  D     patched   jsonb_toaster_lite plain           on
  E     patched   jsonb_toaster_lite per-chunk pglz  on
  D_fast = D + jbtl_object_field()  (L1.4 fast path)
  E_fast = E + jbtl_object_field()
```

Per-call cost (us) at i=100, ~396 KB jsonb:

```
                        key1      key2       key3      key4
                       (deg)     (cont)    (clean)   (cont)
  ------------------   -----     ------    -------   ------
  A vanilla             571.9   1788.3      561.8    631.3
  B patched sort=off    574.8   1802.8      545.7    621.8
  C patched sort=on     564.7   1808.6      558.6    652.4
  D lite plain j->      563.0   1636.4      571.4    629.1
  E lite pglz  j->      758.7   2001.8      748.6    874.6
  D_fast (L1.4)          22.0   1544.9       22.2    636.2  <--
  E_fast (L1.4 + pglz)  297.1   2360.8      270.0   1035.8
```

Architectural decomposition on key3 (the clean case):

```
  effect                                       us / A      conclusion
  ------------------------------------------   ---------   -----------------
  no regression       (A vs B, sort=off)         0.971x    K1 patches do
                                                           not regress when
                                                           the GUC is off
  KVMap layout alone  (A vs C, sort=on,
                       no custom toaster)        0.994x    operator path
                                                           still full-
                                                           detoasts; layout
                                                           alone gives
                                                           nothing
  Custom toaster      (A vs D, lite plain
   alone               via j-> operator)         1.017x    storage layer
                                                           alone gives
                                                           nothing
  KVMap + L1.4        (A vs D_fast,
   combined            jbtl_object_field)        0.040x    25.3x speedup
                                                           ^^^ the win
  Compression cost    (D_fast vs E_fast)         12x       per-chunk pglz
                                                           kills flat-curve
                                                           because partial
                                                           prefix fetch
                                                           must sequentially
                                                           decompress
```

Architectural conclusion (the central claim of L1.4):

  KVMap alone does not help if the operator full-detoasts.
  jsonb_toaster_lite alone does not help if the operator
   full-detoasts.
  The win appears only when:
    K1 value-sorted layout
    +
    L1.4 key-level sliced lookup
  are used together.

### Earlier bench rounds (kept for reference)

**Hot single-row bench (L13b_realistic_bench.sql):** 5x200 EXECUTEs
on one row.  After warmup all 25 toast chunks live in shared_buffers,
and per-op cost is dominated by jsonb parse + result construction,
not chunk I/O.  Speedup observed: ~17 %.  Useful only as a
correctness/parity sanity check — not a performance story.  This
methodology is what made the first bench numbers misleading; it
does not exercise the slice-fetch mechanism that L1.4 is about.

**Cold per-op bench (L14_cold_bench.sql):** 30 trials, each preceded
by `pg_buffercache_evict_relation` for base, toast, and PK index.
Per-op median in microseconds:

```
   op             j->>          KVMap fast path     speedup
   -----------    -----------   ---------------     -------
   status         559 us        288 us              1.94x
   summary        567 us        276 us              2.06x
   payload        614 us        563 us              1.09x  (fallback)
```

Per-op constant overhead (eviction calls + per-call dispatch +
parse) is ~250 us; the variable part attributable to chunk count
shows the savings.  These keys are all "key1-like" in the
canonical-shape sense (small scalars next to other small
scalars), so they understate the headline number.

**Cold sequential-scan bench (L14_seqscan_bench.sql):** 5000-row
table (~250 MB total toast > 128 MB shared_buffers), single seq
scan, the natural OLAP shape:

```
                         j->>           kv             speedup    reads
   small  (status) cold  524 ms         117 ms         4.5x
   small  (status) warm  495 ms         85 ms          5.8x
   medium (summary) cold 552 ms         206 ms         2.7x
   large  (payload)      1499 ms        1566 ms        1.0x  (fallback)

   EXPLAIN (BUFFERS):
     j->>   :  shared hit=13750 read=31297    (45047 total)
     kv     :  shared hit=10000 read= 5047    (15047 total)
                                              6.2x fewer disk reads
```

Per-row chunk_fetched counts (KVMap-aware path, B variant):

```
   key      | chunks_total | chunks_fetched
   ---------+--------------+----------------
   status   |     25       |       1
   kind     |     25       |       1
   tenant   |     25       |       1
   title    |     25       |       1
   summary  |     25       |       2
   payload  |     25       |       1  (fallback)
   flags    |     25       |       1  (fallback, nested)
```

The 25-to-1 chunk reduction is the ground-truth I/O claim;
wall-time speedup depends on what fraction of the saved chunk
fetches actually hit cold storage.

### Acceptance against @yoda's L1.4 spec

  - `jbtl_object_field` fetches 1 chunk (was 25 — full detoast). ✓
  - small / medium scalars improve clearly. ✓
  - large value (>50% body) falls back; threshold guard verified. ✓
  - fallback shape pinned in regression
    (`jsonb_toaster_lite_object_field` test). ✓
  - byte-equal with `j->>` confirmed for every value type
    (string, number, bool, null, nested-via-fallback,
    missing-key, large-via-fallback). ✓
  - matrix bench across vanilla and patched clusters confirms
    A vs B no regression, KVMap+L1.4 combined gives 25.3× on
    key3 (clean KVMap test). ✓

The L1.4 path engages only on TOAST'd custom-varlena jsonb
(both plain-chunks and per-chunk-pglz storage modes); inline
JBTL_PLAIN_JSONB and default-toaster jsonb take the fallback
path silently.


## Reference numbers (artificial extremes; from L1.3-bench)

(Kept here for completeness; full text is in the L1.3-bench commit
message.)

```
Storage:
  A_compressible    column_size=274  (inline-pglz wins, no toast use)
  A_incompressible  column_size=59664, 65 KB toast
  B_compressible    column_size=36, 131 KB toast, 31/31 raw chunks
  B_incompressible  column_size=36, 131 KB toast, 33/33 raw chunks
  C_compressible    column_size=36,  74 KB toast, 31/31 PGLZ chunks
  C_incompressible  column_size=36,  74 KB toast, 33/33 raw fallback

Slice fetch counts (B and C):
  full op           chunks_fetched = chunks_total (31 or 33)
  slice ops         chunks_fetched = 1   (~31x reduction)
```

These are illustrative, not tuned (debug build, asserts on).  Real
workloads with assertion-free production builds will be faster across
the board, but the *ratios* between variants and operations are what
this snapshot is for.

