# Sliced jsonb read — subscripting scope decision

Decision document for whether jsonb subscripting (`jb['key']`,
`jb[0]`, `jb['a']['b']`) should ship in the Layer 1 review
package or as a follow-up patch.

Branch: `r2d2/layer1-sliced-read`.

## Verdict

**FOLLOW_UP_ACCEPTABLE.**

Subscripting integration is a small, mechanically clean
follow-up: ~30-50 lines in `jsonbsubs.c`, calling the existing
`getKeyJsonValueFromExternal` helper at the right boundary. It
does not refactor anything. But it adds its own correctness
surface (multi-step paths, array vs object roots, NULL
semantics), its own test coverage, and its own scope-creep
risks. Bundling it now expands the patch's review surface
without strengthening the Layer 1 acceptance evidence.

The smallest honest answer for the upstream reviewer is to land
Layer 1 for `->` and `->>` first, with the helper signature
already compatible with subscripting (Datum-level gate,
`out_handled` contract). Subscripting follows immediately as a
small named patch that the same reviewer reads next.

The opposite verdicts are not appropriate:

  - INCLUDE_SUBSCRIPTING_NOW would conflate two distinct patches
    and force the reviewer to absorb two correctness arguments
    at once;
  - NEEDS_JSONB_SUBSCRIPTING_REFACTOR overstates the cost —
    nothing about jsonbsubs.c needs to change shape;
  - BLOCKS_LAYER1_REVIEW is false — Layer 1 is internally
    complete; subscripting is a coverage extension, not a
    correctness prerequisite.

The remainder of this document is the substantive analysis
backing that verdict.

## 1. How subscripting reaches the read path today

The current execution chain for `jb['key']`:

  1. Parser/planner: `jsonb_subscript_transform`
     (`jsonbsubs.c:44`) builds a SubscriptingRef. Text/int
     coercion is fixed at plan time; the result type is always
     `jsonb`.
  2. Executor setup: `jsonb_exec_setup` allocates
     `JsonbSubWorkspace` (per-row scratch).
  3. Per-row, `jsonb_subscript_check_subscripts`
     (`jsonbsubs.c:176`) materialises each subscript Datum into
     `workspace->index[i]`, stringifying integers via
     `int4out` → `CStringGetTextDatum`. Sets
     `workspace->expectArray` if the first subscript is
     INT4OID.
  4. Per-row, `jsonb_subscript_fetch` (`jsonbsubs.c:236`):

         jsonbSource = DatumGetJsonbP(*op->resvalue);    /* full detoast */
         *op->resvalue = jsonb_get_element(jsonbSource,
                                           workspace->index,
                                           sbsrefstate->numupper,
                                           op->resnull,
                                           false);

  5. `jsonb_get_element` (`jsonfuncs.c:1640`): walks `path[]`,
     for object roots calls `getKeyJsonValueFromContainer`
     (line 1692), for array roots calls
     `getIthJsonbValueFromContainer` (line 1736). Multi-step
     paths descend container-to-container in memory.

The full detoast happens at `jsonbsubs.c:247` —
`DatumGetJsonbP(*op->resvalue)`. This is the exact pattern
Layer 1 already eliminated in `jsonb_object_field` and
`jsonb_object_field_text`.

Answer to Q1: **yes**, subscripting reaches
`getKeyJsonValueFromContainer` only after full detoast. There
is no existing fast path.

## 2. Where Layer 1 could integrate

Answer to Q2: **yes**, the Datum-level entry point exists.

The boundary is `jsonb_subscript_fetch` at line 247,
immediately before `DatumGetJsonbP`. The raw Datum is in
`*op->resvalue`. The signature shape would be the same as the
existing gate in `jsonb_object_field`:

  - check `VARATT_IS_EXTERNAL_ONDISK(DatumGetPointer(raw))`;
  - check single-step object case (see §3 below);
  - call `getKeyJsonValueFromExternal`;
  - on handled: convert JsonbValue → Jsonb Datum, set
    `*op->resvalue` and `*op->resnull` accordingly;
  - on not handled: fall through to the existing `DatumGetJsonbP`
    + `jsonb_get_element` path.

No new helper needed. The existing helper signature
(`Datum raw, const char *keyVal, int keyLen, JsonbValue *res,
bool *out_handled`) is exactly what subscripting needs.

## 3. Single-step object subscripting — semantic check

Answer to Q3: **yes for the single-step object case, with
caveats stated below.**

`jb['key']` with `npath == 1` and root being an object is
semantically equivalent to `jb -> 'key'`:

  - both return `jsonb` (not `text`); no `JsonbValueAsText`
    branch needed;
  - missing key returns SQL NULL via `*op->resnull = true`,
    same as `jsonb_object_field` returning `PG_RETURN_NULL`;
  - subscript Datum has already been coerced to text by the
    parser (line 128-133) or stringified at runtime by
    `check_subscripts` (line 219); so the Layer 1 helper's
    `(keyVal, keyLen)` arguments come from
    `VARDATA_ANY(workspace->index[0])` and
    `VARSIZE_ANY_EXHDR(workspace->index[0])`.

The conditions for invoking Layer 1 are:

  - `sbsrefstate->numupper == 1`
  - `workspace->expectArray == false`
  - `workspace->indexOid[0] != INT4OID` (defensive — text
    subscript)
  - `VARATT_IS_EXTERNAL_ONDISK(DatumGetPointer(*op->resvalue))`

Inside the helper, root-is-object is already enforced (the
existing code returns "not handled" on array root). So
`workspace->expectArray` is a fast pre-check that saves a
prefix fetch for the array case; the helper would still
do the right thing without it.

Caveat: subscript NULL handling. When
`sbsrefstate->upperindexnull[i]` is true, `check_subscripts`
already short-circuits with `*op->resnull = true` and returns
`false` (line 200-208), so `subscript_fetch` is not reached.
No additional NULL plumbing needed at the Layer 1 gate.

## 4. Multi-step paths

The interesting case is `jb['a']['b']` with `numupper == 2`.
`jsonb_get_element` walks step by step:

  - Step 0: returns JsonbValue, which becomes the next
    `container` if `jbvBinary`.
  - Step 1: looks up `'b'` inside the sub-container.

Layer 1's helper rejects container JEntries (spec §6 stage 7,
v0 scalar-only). So if step 0's result is a sub-container,
Layer 1 cannot serve it; we'd have to do full detoast anyway
to walk the path.

Two paths from here:

  (i) Layer 1 helps the **prefix step** only when the value at
      that step is a scalar AND it is the LAST step. For multi-
      step paths whose final value is a scalar, Layer 1 helps
      only if `numupper == 1`. For `numupper > 1` we always
      fall back.

  (ii) Extending Layer 1 to traverse sub-containers is out of
       scope of v0 (spec §6 stage 7 is explicit). The cost is
       not just additional code — it requires fetching enough
       bytes of the sub-container to walk its JEntries and key
       area, which for typical nested objects means fetching
       most of the body. Not worth the complexity, not
       proposed.

The simple rule: **Layer 1 engages for subscripting iff
`numupper == 1` and root is an object.** Everything else falls
through to the existing path. This is the same coverage as
`jb -> 'key'`.

## 5. Array subscripting

Answer to Q4: **out of scope for this patch.**

`jb[0]`, `jb[-1]`, `jb[5]` route through `jsonb_get_element`
into `getIthJsonbValueFromContainer`. Layer 1's helper does
not handle array roots; it returns "not handled" on
non-object root in stage 2.

Adding array sliced read would require:

  - a separate helper `getIthJsonValueFromExternal` (different
    binary-search shape — no key compare, just index walk);
  - new test coverage for positive, negative, out-of-bounds
    indices;
  - separate fallback decisions (array values are typically
    larger than object keys' values, half-body heuristic hits
    earlier).

This is a distinct design exercise. Not bundling it with the
Layer 1 review keeps the review focused on the object path
that the spec, the result doc, and the matrix evidence all
cover.

Array subscripting is a known follow-up; flagging it
alongside subscripting follow-up does not enlarge the v0
patch.

## 6. Patch-size analysis

Answer to Q5: **incremental, but not free.**

Estimating the subscripting integration as a diff against the
current Layer 1 patch:

  - `jsonbsubs.c`: +30..50 lines at the start of
    `jsonb_subscript_fetch`. Single gate, single helper call,
    JsonbValue → Datum conversion, one fall-through path.
  - `src/test/regress/sql/jsonb.sql` and
    `src/test/regress/sql/jsonb_subscript.sql` (if it exists,
    else add a few cases to jsonb.sql): +10..30 lines of new
    test cases covering the conditions enumerated in §3.
  - `docs/SLICED_JSONB_READ.md` §9.3 (subscripting integration):
    update from "scoped for future tasks" to "implemented;
    same gate, same helper."
  - `docs/SLICED_JSONB_READ_RESULT.md`: add a §6 or new section
    "Subscripting coverage" with the same cold-cache compression
    split repeated for `jb['key']`.

Estimated added review surface:

  | Surface | Layer 1 alone | + subscripting |
  |---|---:|---:|
  | Code lines (excl. test) | 466 | ~500 |
  | Test lines | 0 (regression-only) | +20 |
  | Doc lines | ~1500 | ~1700 |
  | Distinct integration sites | 2 (`->`, `->>`) | 3 |
  | Distinct correctness arguments | 1 (object-key fast path) | 2 (object-key fast path; subscripting wrapper) |

The marginal code is small. The marginal **reviewer cost** is
not. A vanilla reviewer reads the patch by:

  - reading the spec to understand the contract;
  - reading the helper for correctness;
  - reading each integration site to confirm the gate is
    correctly placed;
  - cross-checking against the test suite;
  - sanity-checking the empirical evidence.

Each additional integration site adds an item to that
checklist. Bundling subscripting means the reviewer has to
form an opinion on two distinct fast-paths simultaneously, one
of which is a wrapper of the other but introduces its own
semantic surface (multi-step, array roots, subscript NULLs).

If subscripting can land confidently within one week of Layer
1 acceptance, that is cheaper for the reviewer than asking
them to absorb both at once. If subscripting needs more design
iteration than expected, having Layer 1 already landed avoids
holding up the proven part.

## 7. Vanilla reviewer anticipation

Answer to Q6 (the substantive part): the reviewer's first
question — "why doesn't subscripting use this?" — has two
acceptable answers:

  A) "It does — see `jsonb_subscript_fetch` line 247."
     (INCLUDE_SUBSCRIPTING_NOW)

  B) "It will — see follow-up patch
     `SLICED_JSONB_SUBSCRIPTING_FOLLOWUP`. The helper signature
     was deliberately written compatible with that integration.
     The gate is one Datum-level check, the conditions are in
     `docs/SLICED_JSONB_SUBSCRIPTING_SCOPE.md` §3."
     (FOLLOW_UP_ACCEPTABLE)

(A) is shorter to state; (B) is shorter to review and
demonstrates that the deferral is intentional, scoped, and
non-speculative. (B) is the smallest honest answer once you
account for the reviewer's time, not just the patch author's.

What makes (B) credible as opposed to vapourware:

  - the helper signature in this patch is the one subscripting
    will call — no refactor needed;
  - this document exists and names the integration point
    precisely (`jsonbsubs.c:247`), the conditions (§3), the
    correctness guarantees (§3), and the scope boundary (§4,
    §5);
  - the existing Layer 1 result document already includes
    subscripting as OQ-B and the review notes flag it
    explicitly in §6.

If the reviewer rejects (B) — "no, do them together" — Layer
1's patch package is still complete on its own, and
subscripting can be added in the same revision cycle. The cost
of (B) being rejected is a single round-trip, not redesign.

The cost of (A) being rejected (e.g. reviewer wants Layer 1
done but is uneasy about subscripting's semantic surface) is
either holding Layer 1 hostage to subscripting redesign, or
splitting the patch under reviewer pressure — both worse
outcomes than starting with (B).

## 8. Plan for the follow-up patch

If verdict stands, the follow-up patch shape:

  1. New conditions block at the head of
     `jsonb_subscript_fetch` in `jsonbsubs.c`, mirroring the
     gate in `jsonb_object_field` (`jsonfuncs.c:917`):

         numupper == 1
         AND workspace->indexOid[0] != INT4OID
         AND VARATT_IS_EXTERNAL_ONDISK(*op->resvalue)

  2. Call `getKeyJsonValueFromExternal` with
     `VARDATA_ANY(workspace->index[0])` and
     `VARSIZE_ANY_EXHDR(workspace->index[0])`.

  3. On `handled == true`:
       - if `JsonbValue *result == NULL`: `*op->resnull = true`
         and return;
       - else: `*op->resvalue = JsonbPGetDatum(JsonbValueToJsonb
         (result))`; `*op->resnull = false`; return.

  4. On `handled == false`: fall through to existing
     `DatumGetJsonbP` + `jsonb_get_element` path. No change to
     that code.

  5. Regression tests added to `jsonb_kvmap.sql` (or
     dedicated):
       - `jb['key']` on external compressed body, key in
         prefix → result correct;
       - `jb['key']` on external compressed body, key past
         prefix → result correct (fallback);
       - `jb['key']` on external uncompressed body, both cases
         → result correct;
       - `jb['key']` on inline body → result correct (gate
         bypasses);
       - `jb['missing']` → SQL NULL;
       - `jb[0]` on external array → result correct (gate
         skips, existing path);
       - `jb['a']['b']` multi-step → result correct (gate
         skips on numupper > 1).

  6. Cold-cache compression split benchmark in the follow-up
     result doc, same shape as Layer 1's. Expected: matches
     `jb -> 'key'` numbers within counter noise (same code
     path, same body shapes).

  7. Doc updates:
       - `SLICED_JSONB_READ.md` §9.3: mark integration as
         done, reference follow-up commit;
       - `SLICED_JSONB_READ_RESULT.md`: add a brief subsection
         showing subscripting numbers parallel `->` numbers;
       - `LAYER1_REVIEW_NOTES.md` §6: remove OQ-B from the
         "NOT in this patch" list and mention the follow-up
         landed.

Estimated effort: half a day code + half a day docs and tests.
No design risk.

## Constraints honoured

  - no code in this document ✓ (design only);
  - no benchmark in this document ✓;
  - no Layer 2 relocation discussion ✓ (orthogonal);
  - no `jsonb_sort_field_values` default change ✓;
  - no modify/WAL work ✓ — note that subscripting
    *assignment* (`jb['key'] := value`) routes through
    `jsonb_subscript_assign` → `jsonb_set_element` which IS
    modify/WAL territory and is explicitly out of scope here.
    The follow-up patch handles `fetch` only.

## Stop here

The follow-up patch is fully specified. Awaiting yoda's review
of this verdict before either:

  - keeping Layer 1 as the standalone review package and
    queuing the subscripting follow-up; or
  - merging the subscripting work into Layer 1's package
    pre-submission (if yoda overrides FOLLOW_UP_ACCEPTABLE
    with INCLUDE_SUBSCRIPTING_NOW).

— @r2d2
