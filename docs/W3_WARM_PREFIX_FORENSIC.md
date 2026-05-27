# W3 v1 forensic: inline parent vs warm-prefix model

Forensic/design clarification over the existing w3-relocation-matrix branch and
benchmark data. No new code, no KVMap, no container relocation. Question: is W3
v1 "inline warm parent + cold tail", or better described as "bounded warm prefix
+ cold tail" where the prefix happens to be inline for tested shapes?

## 1. Terminology (three distinct concepts)

- warm inline: the parent jsonb varlena physically fits in the heap tuple
  (no TOAST pointer for the parent itself).
- warm prefix: a bounded leading region of the value that can be read/updated
  without full detoast / cold-tail materialization. Inline-ness is one possible
  realization, not the definition.
- cold tail: large top-level children relocated out-of-line as ordinary TOAST
  values, referenced by JENTRY_ISTOASTED descriptors; must not be read or
  rewritten on a warm metadata access.

## 2. Source evidence

W3 split producer (jsonb_util.c jsonb_toast_split_datum):
- detoasts the value, walks top-level entries, and for each large STRING child
  (val.string.len >= JSONB_TOAST_SPLIT_VALUE_MIN = 256) calls toast_save_datum
  and replaces it with a JENTRY_ISTOASTED descriptor;
- returns parent = JsonbValueToJsonb(pstate.result): an ORDINARY jsonb varlena
  carrying inline warm metadata + cold descriptors. It is not a special
  bounded-prefix object.

heaptoast.c (after toast_or_split, ~line 612 onward):
- did_split is detected by pointer identity; the returned parent then re-enters
  the STOCK toast loop: `while (heap_compute_data_size(...) > maxDataLen)` picks
  the biggest attribute and externalizes it. The parent is NOT exempt.

Consequence from source: W3 guarantees the cold children are relocated, but the
parent lives under ordinary TOAST rules. If the warm metadata is large, the
parent itself can be externalized. So "inline parent" is not guaranteed by the
mechanism; it is an outcome for small-warm shapes.

## 3. Measurement (string_cold + key0, optimized, warm cache)

Storage / split (per-row toast chunks, before/after insert):

| id | raw key2 | U0 colsize | U0 chunks | W3 parent | W3 chunks | rk3 U0 | rk3 W3 | WAL ratio |
|----|----------|-----------|-----------|-----------|-----------|--------|--------|-----------|
| 1  | 352      | 508       | 0         | 508       | 0         | 9      | 15     | 0.45 |
| 10 | 1024     | 1244      | 0         | 1244      | 0         | 9      | 15     | 0.51 |
| 25 | 5696     | 6392      | 4         | 170       | 4         | 232    | 15     | 0.82 |
| 50 | 101184   | 111416    | 56        | 170       | 57        | 932    | 15     | 1.74 |
| 75 | 1799488  | 1979544   | 992       | 170       | 993       | 12682  | 15     | 360.81 |
| 100| 32000000 | 35200120  | 17636     | 170       | 17637     | 223082 | 15     | 790.31 |

Marked points:
- U0 externalization point: chunks 0 -> 4 between id=10 (1244 B) and id=25
  (6392 B). TOAST_TUPLE_THRESHOLD ~2 KB (block 8192, 4 tuples/page).
- W3 split point: parent 1244 -> 170 B in the SAME interval (id=10 -> 25).
- W3 parent size plateau: 170 B from id=25 onward (warm prefix is small and
  constant for this shape, regardless of cold size).
- read_key3 buffer plateau: W3 flat at ~15 buffers from id=1; U0 grows to
  223082. read_key0 identical (key0 sits in the same warm prefix, before key3).
- WAL break-even point: ratio crosses 1 between id=25 (0.82) and id=50 (1.74),
  i.e. LATER than externalization/split.

Forensic stress test (does the parent stay inline if warm metadata is large?):
- row A: 3 warm keys + 1 cold string -> parent 95 B (inline warm prefix).
- row B: 500 warm keys + same cold string -> parent 107538 B
  (parent ITSELF externalized; warm prefix is NOT inline).
This directly shows the prefix is inline only when warm metadata is small; the
mechanism does not bound the prefix.

## 4. Relation to key0 / KVMap

key0 is a small warm array, placed before key3 in canonical jsonb order
(key0,key1,key2,key3,key4). It lives in the warm prefix.

- without KVMap: read_key3 pays ordinary key-entry traversal cost inside the
  warm prefix (it scans entries up to key3, including key0). On the tested small
  prefixes this is ~15 buffers and flat, but the traversal cost is real and
  grows with the number of warm entries.
- with a future KVMap / warm access map: read_key3 should reduce the
  localization cost of finding key3 within the same warm prefix (directory
  lookup instead of linear entry scan).
- KVMap is orthogonal to cold-tail relocation: relocation decides what leaves
  the prefix; KVMap decides how cheaply a key is located within the prefix.
  Neither implemented here.

## 5. Corrected terminology / labels

- Do NOT call the WAL crossover a "TOAST boundary". Proven separate boundaries:
  stock TOAST externalization (chunk count, ~2-6 KB), W3 split point (same
  interval), observed WAL break-even (~6.4-111 KB, later), read_key3 buffer
  plateau (flat from id=1).
- Report wording corrected from "inline parent only" to:
  "warm prefix + cold tail, where the warm prefix is currently represented by a
  small inline parent in W3 v1 for the tested string_cold shapes."

## Conclusion

WARM_PREFIX_MODEL_CONFIRMED.

Source shows the parent re-enters the stock TOAST loop and is externalized when
large (not a bounded object); the stress test confirms a 500-warm-key parent is
itself TOASTed. Therefore "inline parent" is a shape-dependent outcome, not the
architecture. The accurate model is "bounded warm prefix + cold tail": W3 v1
relocates large string children to the cold tail and keeps warm metadata in a
prefix that, for small-warm shapes, is realized as a small inline parent. The
benefit (read_key3 plateau, WAL reuse) follows from warm/cold separation, not
from the parent being inline per se.

Note on "bounded": the prefix is bounded in PRINCIPLE (warm metadata only), but
W3 v1 does not enforce a hard bound — a large warm prefix is itself TOASTed by
the stock path. A real bounded-prefix guarantee (e.g. first-page/first-chunk
warm region) would be future work, distinct from both KVMap and container
relocation.

## Artifacts
results/w3_storage_boundary.csv (+ read_key3/key0 from summary_sc_k0.csv).
Plot: wal_ratio_U0_over_W3_black.png (two proven boundary markers).
