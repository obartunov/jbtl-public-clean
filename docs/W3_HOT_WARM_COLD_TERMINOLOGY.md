# W3 v1: hot / warm / cold terminology (corrected)

Replaces the "warm prefix" wording. Three-temperature layout, with the physical
definition of warm and an explicit statement of what W3 v1 does and does not do.

## Definitions

- hot: the inline parent in the heap tuple. Cheapest access class. Measured:
  read_key3 on string_cold = ~15 buffers, flat, regardless of cold size.
- warm: the first one / few TOAST chunks of a value (about one TOAST page ~=
  3 chunks), readable as a bounded area WITHOUT materializing the cold tail.
  Warm is a PHYSICAL property (what lands in the first page), not "specially
  prepared structures". It is NOT automatically as cheap as inline: heap inline
  and TOAST chunks live in different relations and are not guaranteed physically
  adjacent.
- cold: the large payload tail, not read or rewritten during hot/warm metadata
  access. Measured: key2/key4 relocated string children.

## What W3 v1 is

W3 v1 = hot + cold. NOT hot/warm/cold.

  hot:  small inline parent carrying key0/key1/key3 + JENTRY_ISTOASTED
        descriptors for the relocated children;
  cold: relocated large top-level string children key2/key4.
  warm chunk layer: NOT implemented.

Current benchmark wording (do not change to claim warm chunks):
  - read_key3 on string_cold proves HOT-INLINE read avoidance.
  - MM1 WAL proves COLD-TAIL reuse on hot metadata update.
  - It does NOT prove that the first TOAST chunks are as cheap as inline.

## The warm layer needs value sorting — and why W3 v1 cannot rely on it

Without value sorting, the order of values inside a (possibly TOASTed) parent is
just stock jsonb serialization order (keys by name length then bytes; values in
that key order). So "the first chunks" are an arbitrary slice of serialization
order, NOT a reliable warm layer: a small frequently-read value may sit anywhere,
and what lands in the first page is incidental.

With value sorting + KVMap, the first TOAST page can become a real warm layer:
  - values sorted small-first, so small/hot values land in the first chunks;
  - large/cold values pushed later;
  - KVMap preserves key -> value addressing despite the reordering.
Only then does "first page / first few chunks = warm" mean something, and only
then can warm be separated from relocated by a single physical predicate
(position relative to the first page).

### Source note (verified, branch jsonb-warm-prefix-cold-tail-v1 @ 0141231)

The value-sorting GUC (jsonb_sort_field_values, "sort an object's values by size,
small first") and the KVMap writer/reader (K1 line) ARE present in this tree, but
default off. Crucially, the W3 split producer is de-KVMap / stock-layout BY
CONSTRUCTION, not merely because the GUC is off: while assembling the parent it
forces KVMap emission off for that JsonbValueToJsonb() call (PG_TRY-restored
internal override), and its comment states it "operates on stock jsonb layout
only -- no KVMap, no value sorting, no jsonb_sort_field_values dependency."

So the mechanism that could create a real warm layer (value-sort + KVMap) lives
in the tree as a separate legacy line, and W3 v1 deliberately does NOT use it.
W3 v1 therefore remains hot + cold.

### Commit history of the de-KVMap decision (why, not just what)

Two commits made W3 stock-layout, and the reason matters for the warm roadmap:

- 1432b64 "jsonb: de-KVMap the W2.x split/relocation line (stock layout only)":
  removed every ACTIVE W2.x dependency on KVMap / value sorting. The earlier W2.2
  producer gated split eligibility on JsonContainerHasKVMap — i.e. eligibility
  depended on the non-stock physical property of value sorting. That property is
  LOST across jsonb_set / || and similar rebuilds, so a split row silently
  degraded to whole-toast after such an UPDATE. The fix made eligibility "any
  top-level jsonb object", and moved the reuse finder / external-ref walker to
  stock slot layout (keys [0..n-1], values [n..2n-1]). The legacy K1 KVMap
  reader/writer was left in the tree, just unused by W2.x.
- 1afcde1 "jsonb: W2.x split producer builds stock-layout parent by
  construction": added the PG_TRY override that forces KVMap off during parent
  assembly, so the stock layout holds by construction, not by GUC state.

So "выкидывание KVMap" was real: the split line's dependency on KVMap was
removed. It was not deletion of KVMap from the tree — the legacy reader/writer
and the GUC remain; W3 just does not rely on them.

### Constraint this places on the future warm-chunk layer

The de-KVMap was a CORRECTNESS fix, not a style choice: binding the layer to
value sorting was unsafe because the sorted/KVMap property does not survive
jsonb_set / || rebuilds. Therefore a future value-sorted warm-chunk layer must
solve exactly this: keep the sorted ordering AND the KVMap key->value addressing
stable across UPDATE/rebuild paths, or it will silently degrade the same way W2.2
did. "First page = warm" is only durable if the sorted layout is preserved on
every rewrite, not just at first INSERT.

## Correcting the earlier prefix_friendly/hostile experiment

The earlier "large warm prefix" (prefix_hostile) was a misnomer. A 400-warm-key
parent of ~8 KB does not create a warm layer — it is a HOT parent that outgrew
the inline budget and was externalized whole by the stock TOAST path. Reading
key3 there cost ~226 buffers (vs ~15 inline) because the entire externalized
parent is read, not because a bounded warm area was accessed. That experiment
measures the HOT-inline budget boundary (parent stops fitting inline), not a
warm layer.

## Roadmap (future work, not implemented)

- value-sorted warm-chunk layer: turn on small-first value sorting so the first
  TOAST page holds hot/small values;
- KVMap-assisted warm access: preserve key->value addressing under sorting and
  cut in-prefix localization cost;
- first-page / first-N-chunks warm probe: separately measure whether reading the
  first 1-3 chunks is in the same cost class as inline, since heap inline and
  TOAST chunks are different relations with no adjacency guarantee. The earlier
  "1 vs 2 vs 7 chunks same class" probe measured externalized-parent reads, not a
  designed warm layer, so it does NOT settle this.
