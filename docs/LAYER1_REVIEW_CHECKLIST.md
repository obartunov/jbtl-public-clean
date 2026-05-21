# Layer 1 — internal code review checklist

## Branch and commits

  Branch: r2d2/layer1-upstream-prep
  Base:   origin/r1-relocation-aware-read
          (in github.com/obartunov/jbtl-public-clean)

  Commit range for review:
    git log --oneline origin/r1-relocation-aware-read..r2d2/layer1-upstream-prep

  Reviewer-facing layout (the upstream-prep shape, post fix cycle):

    0001  sliced jsonb read helper + integration (jsonb_util.c,
          jsonfuncs.c, jsonb.h, new jsonb_internal.h)
    0002  jsonb_layer1 regression coverage + parallel_schedule entry
    0003  cover letter, checklist, review notes, this fix log

  Specific commit SHAs are intentionally NOT pinned here. SHAs drift
  every time the branch is rebased onto a moving base; pinning them
  forces every reviewer-facing doc to be re-edited after each rebase
  and creates "unknown revision" failures when one of the documents
  has been refreshed but the other hasn't. Refer to commits by what
  they do (patch number above) and let the reviewer use `git log` to
  enumerate them.

  Fix cycle history (issue identifier → what changed):

    D1+D2+S2  enum signature, header memory-ownership contract,
              StaticAssertDecl on JSONB_SLICED_READ_INITIAL_PREFIX
    D3        jsonb_layer1.sql regression suite + parallel_schedule
              entry
    S3        partially fell out of D1+D2 (the void-cast idiom was
              dropped; memory-ownership note relocated)

  Upstream-prep fix cycle (issue identifier → what changed):

    B1+F8     all size arithmetic in getKeyJsonValueFromExternal
              widened to int64; corruption check on a forged-N
              container header can no longer be bypassed via int32
              wrap (verified numerically and via the regression run)
    F1        helper now deep-copies the scalar payload and frees
              the prefix / value_slice buffers before returning
              FOUND; *res is self-contained on return; per-row
              memory bloat in CurrentMemoryContext eliminated
    F2        helper un-exported from public jsonb.h; declared in
              new src/include/utils/jsonb_internal.h, included from
              jsonb_util.c and jsonfuncs.c only
    F3        false claim "the slow path raises the same on the
              same bytes" replaced in both the function-level
              comment and the test file's corruption-skip rationale
              with an accurate statement of the behaviour change
    F4        new regression cases (Case 10 wide object → Stage 3
              refetch; Case 11 long key names → Stage 4 refetch)
    F5        this section (SHA references dropped from
              reviewer-facing docs in favour of patch numbers)
    F7        half-body cap comment downgraded from "by chunk
              overlap analysis" to "conservative empirical cap;
              finer tuning deferred"
    S1, M1-M7 deferred from the prior internal cycle; still
              deferred — none of them block upstream-prep

  Cross-branch references: inline doc references to
  PRODUCTION_RELOCATION_SOURCE_MAPPING.md, RELOCATION_DELETE_OWNERSHIP.md,
  PREFIX_LOCALITY_VS_RELOCATION.md and similar Layer 2 / prefix-locality
  filenames resolve in the parent branch origin/r1-relocation-aware-read,
  not on this branch. They are intentional cross-context pointers;
  Layer 1 review does not require their content.

  Push delivery: branch could not be pushed from the dev sandbox
  (no GitHub credentials). Bundle delivered:
    layer1_push_bundle/layer1_clean.bundle
    layer1_push_bundle/layer1_clean_patches.mbox

  To push from a credentialed shell:
    cd ~/jbtl-public-clean
    git fetch /path/to/layer1_clean.bundle r2d2/layer1-sliced-read-clean
    git push origin FETCH_HEAD:refs/heads/r2d2/layer1-sliced-read-clean

## Review focus (from @yoda)

  1. Correctness of getKeyJsonValueFromExternal.
  2. Exact fallback boundaries.
  3. Compressed external handling.
  4. Scalar-only restriction.
  5. No regression for inline / non-external jsonb.
  6. Code/doc consistency with the three-case acceptance model.

## Read order

  1. docs/LAYER1_COVER_LETTER.md      (entry point; 270 lines)
  2. docs/LAYER1_REVIEW_NOTES.md      (review navigation)
  3. docs/SLICED_JSONB_READ.md        (spec; §4-§7 algorithm, §10 acceptance)
  4. docs/SLICED_JSONB_READ_RESULT.md (empirical result + factorial matrix)
  5. Commit 28c8b7fa5c — the code itself

## How to build

  cd ~/jbtl-public-clean
  ./configure --prefix=$HOME/pg --enable-debug --enable-cassert
  make -j$(nproc)
  make install
  cd contrib/jsonb_toaster_lite && make install && cd ../..
  cd contrib/toastapi          && make install && cd ../..

## How to run regression tests

  Method 1 — via running cluster (recommended when iterating):

    $PG/bin/pg_ctl -D $PGDATA -l /tmp/pg.log start
    psql -p $PGPORT -c "DROP DATABASE IF EXISTS regression;"
    psql -p $PGPORT -c "CREATE DATABASE regression;"
    cd src/test/regress
    PGHOST=/tmp PGPORT=$PGPORT PGUSER=postgres \
      ../../../src/test/regress/pg_regress \
        --inputdir=. \
        --bindir=$PG/bin \
        --dlpath=. \
        --use-existing \
        test_setup jsonb json jsonb_jsonpath jsonpath_encoding jsonpath jsonb_kvmap

  Expected: 6 of 7 pass. The single failure (test_setup) is a
  pre-existing tablespace-leftover artifact, not patch-induced.
  Verify by inspecting regression.diffs — first line of diff
  is "tablespace regress_tblspace already exists".

  Method 2 — full standard regression (requires fresh temp instance):

    cd src/test/regress && make installcheck-parallel

## How to run the cold-cache evidence cases

  Bench-suite branch r2d2/legend-relocation-rename adds
  `--mode cold` to scripts/run_matrix.py.

    cd ~/jsonb-p1-bench-suite
    export PATH=$PG/bin:$PATH
    export PGHOST=/tmp PGPORT=$PGPORT PGUSER=postgres PGDATABASE=postgres
    export BENCH_PG_RESTART_CMD="su postgres -c '$PG/bin/pg_ctl -D $PGDATA -m fast restart'"

    python3 scripts/run_matrix.py \
      --mode cold \
      --cells F0,P00,P10,J00,J10 \
      --ids 60,75,100 \
      --keys key1,key3 \
      --cold-runs 3 \
      --out results/raw_cold.csv

    python3 scripts/aggregate_results.py \
      results/raw_cold.csv \
      results/summary/cold_summary.csv

  Expected, per spec §10.2 three-case split:
    Case A (P10 at id=100 key1): P10 ≤ J10 on buffer count
    Case B (uncompressed external past prefix): 6 reads / ~0.17 ms
    Case C (compressed external past prefix at id=100 key3):
      P00 buffer profile matches F0 within noise — Layer 1 falls
      back per spec §7.

  Alternative: in-process direct measurement against running cluster.
  See docs/SLICED_JSONB_READ_RESULT.md §5 for the exact recipe
  used during implementation, including the compile-time gate
  disable used to obtain the baseline.

## What to check during review

Code review focus areas, mapped to source locations:

  Correctness of helper (review focus 1):
    src/backend/utils/adt/jsonb_util.c
      getKeyJsonValueFromExternal — ~270 lines including comments
    Key invariants:
      - body_size from va_rawsize - VARHDRSZ (NOT
        VARATT_EXTERNAL_GET_EXTSIZE) — see commit message of
        28c8b7fa5c, bug 1;
      - fetch_len = value_len (NOT value_len + pad) — bug 2;
      - all bounds-checks against body_size, never attrsize;
      - out_handled = false on every fallback condition;
      - ERRCODE_DATA_CORRUPTED on physical inconsistency only,
        not on policy fallback.

  Fallback boundaries (review focus 2):
    docs/SLICED_JSONB_READ.md §7 — fallback table
    docs/SLICED_JSONB_READ_RESULT.md "Code paths exercised"
    Verify each row in the spec's fallback table maps to a
    distinct branch in the helper code.

  Compressed external handling (review focus 3):
    docs/SLICED_JSONB_READ.md §7 — compression policy
    src/backend/access/common/detoast.c:605 — the
      `Assert(sliceoffset == 0 || !compressed)` we honour
    Verify: helper checks
      `body_len >= min_prefix + value_offset_in_data + fetch_len`
    before deciding whether to slice-fetch or fall back.

  Scalar-only restriction (review focus 4):
    src/backend/utils/adt/jsonb_util.c — stage 7
    Verify: JBE_ISCONTAINER(value_jentry) sets out_handled=false.
    No nested-container path in v0.

  Inline / non-external no-regression (review focus 5):
    src/backend/utils/adt/jsonfuncs.c:917, :992
    Verify: the gate is exactly
      `if (VARATT_IS_EXTERNAL_ONDISK(DatumGetPointer(raw)))`
    and that on the else branch the existing code is unchanged.
    Inline microbenchmark in docs/SLICED_JSONB_READ_RESULT.md §1
    confirms <5% per-call regression.

  Code/doc consistency with three-case acceptance (review focus 6):
    docs/SLICED_JSONB_READ.md §10.2 lists Cases A, B, C
    docs/SLICED_JSONB_READ_RESULT.md acceptance table maps each
      case to measured numbers
    docs/LAYER1_REVIEW_NOTES.md §2 ties cases to integration
      site behaviour
    Verify the same partition is used end-to-end; flag any
    discrepancy.

## Known non-scope items

  These are deliberate non-scope, not gaps:

  - Subscripting (jb['key'], jb[0], jb['a']['b']) — deferred
    to named follow-up SLICED_JSONB_SUBSCRIPTING_FOLLOWUP.
    Scope in docs/SLICED_JSONB_SUBSCRIPTING_SCOPE.md. The
    helper signature is already compatible.

  - Layer 2 relocation — separate patch series, design in
    PRODUCTION_RELOCATION_SOURCE_MAPPING.md,
    RELOCATION_DELETE_OWNERSHIP.md,
    PREFIX_LOCALITY_VS_RELOCATION.md. Out of scope here.

  - jsonb_sort_field_values default change — separate
    compatibility task. Layer 1 works correctly under both
    settings; the default-flip decision is independent.

  - Modify/UPDATE/WAL behaviour — out of scope.

  - Core TOAST API change to enable compressed past-prefix
    arbitrary-range slicing — deferred in spec §7.

  - JBTL bug-2 behaviour change — audit-only comment landed
    in commit 079e0b382d; no behavioural change to JBTL in
    this patch series.

  - Upstream submission (pgsql-hackers) — review decision
    only; thread will be opened only after internal review
    accepts.

## Open questions for the reviewer

  OQ-A. §10.2 wording revision — please confirm the three-case
        split correctly captures spec intent.

  OQ-C. JBTL bug-2 migration trigger — the Assert remains as a
        tripwire on any future tightening of
        jbtl_fetch_slice_dispatch. Confirm decision to leave it
        audit-only.

  OQ-D. PREFIX_LOCALITY_VS_RELOCATION Test 2 mid-size buffer
        regression: production Layer 1 inherits F0's profile in
        that range (no extension dispatch). This strengthens
        the original Layer 2 design argument; flagged here, no
        action in this patch.

OQ-B (subscripting timing) is already resolved by yoda's
acceptance of FOLLOW_UP_ACCEPTABLE.

## After review

  Accept   → mark r2d2/layer1-sliced-read for upstream-prep
             (rebase on pgsql-master, format-patch, prepare
             pgsql-hackers thread per separate task);
  Revise   → fix-it items collected into a single follow-up
             commit on the branch, re-run regression + cold-
             cache evidence, re-review;
  Reject   → reason will be recorded; no rollback needed since
             the branch is unmerged.

## Owner

  Author:    @r2d2 (implementor)
  Reviewer:  TBD (internal pass before yoda final review)
  Spec:      @yoda (architectural review already passed)
