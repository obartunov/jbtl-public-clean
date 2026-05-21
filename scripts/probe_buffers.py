#!/usr/bin/env python3
"""
Post-F1 buffer-path sanity probe.

Builds three fixtures shaped like LAYER1_COVER_LETTER Cases A, B, C
plus an inline body, then runs EXPLAIN (ANALYZE, BUFFERS, FORMAT JSON)
for `jb -> 'k'` and `jb ->> 'k'`.

Reports:
  - shared_buffers reads / hits / local reads / temp reads
  - whether the helper engaged or fell back (inferred from buffer
    counts: helper-engaged paths read 1-2 TOAST chunks, fallback
    reads the whole chain)

Does NOT report wall-clock latency. Container timing is not
reproducible against a bench machine and would be misleading
evidence for F1's low-single-digit-percent expected drift.

Usage:
  PGPORT=... PGUSER=... PGHOST=... python3 probe_buffers.py [--label LBL]
"""
from __future__ import annotations
import argparse, csv, json, os, subprocess, sys
from pathlib import Path

def psql(sql: str, capture=True):
    cmd = ["psql", "-X", "-q", "-v", "ON_ERROR_STOP=1",
           "-P", "pager=off", "-A", "-t", "-c", sql]
    res = subprocess.run(cmd, check=True, text=True,
                         stdout=subprocess.PIPE if capture else None,
                         stderr=subprocess.PIPE)
    return res.stdout.strip() if capture else ""

def explain_buffers(sql: str):
    """Run EXPLAIN (ANALYZE, BUFFERS, FORMAT JSON) and return plan dict."""
    out = psql(f"EXPLAIN (ANALYZE, BUFFERS, FORMAT JSON) {sql}")
    payload = "\n".join(line for line in out.splitlines() if line.strip())
    return json.loads(payload)[0]

def extract_buffers(plan):
    """Walk plan tree and accumulate buffer counters from all nodes."""
    keys = ("Shared Hit Blocks", "Shared Read Blocks",
            "Local Hit Blocks",  "Local Read Blocks",
            "Temp Read Blocks",  "Temp Written Blocks")
    acc = {k: 0 for k in keys}
    def walk(node):
        for k in keys:
            acc[k] += node.get(k, 0) or 0
        for child in node.get("Plans", []):
            walk(child)
    walk(plan["Plan"])
    return acc

def setup_fixtures():
    psql("""
DROP TABLE IF EXISTS probe_inline;
DROP TABLE IF EXISTS probe_ext_uncompr;
DROP TABLE IF EXISTS probe_ext_compr;

CREATE TABLE probe_inline (id int PRIMARY KEY, jb jsonb);
INSERT INTO probe_inline VALUES
    (1, '{"a": 1, "b": "x", "c": null, "d": true}'::jsonb);

CREATE TABLE probe_ext_uncompr (id int PRIMARY KEY, jb jsonb);
ALTER TABLE probe_ext_uncompr ALTER COLUMN jb SET STORAGE EXTERNAL;
INSERT INTO probe_ext_uncompr VALUES
    (1, jsonb_build_object(
        'k1', 'small_at_start',
        'k2', repeat('M', 60000),
        'k3', 'small_at_end'));

CREATE TABLE probe_ext_compr (id int PRIMARY KEY, jb jsonb);
INSERT INTO probe_ext_compr VALUES
    (1, jsonb_build_object(
        'k1', 'tiny',
        'k2', repeat('A', 500000),
        'k3', 'tail'));

CHECKPOINT;
""")

def run_probe(label):
    setup_fixtures()
    cases = [
        # (case_id, name, sql, helper_expected)
        ("INLINE_arrow",        "inline jb -> 'a'",
            "SELECT jb -> 'a' FROM probe_inline WHERE id = 1", "inline-no-engage"),
        ("INLINE_text",         "inline jb ->> 'b'",
            "SELECT jb ->> 'b' FROM probe_inline WHERE id = 1", "inline-no-engage"),
        ("B_uncompr_prefix",    "uncompr ext, prefix-resident k1 (Case B-ish)",
            "SELECT jb ->> 'k1' FROM probe_ext_uncompr WHERE id = 1", "FOUND"),
        ("B_uncompr_past",      "uncompr ext, past-prefix k3 (Case B)",
            "SELECT jb ->> 'k3' FROM probe_ext_uncompr WHERE id = 1", "FOUND"),
        ("A_compr_prefix",      "compr ext, prefix-resident k1 (Case A)",
            "SELECT jb ->> 'k1' FROM probe_ext_compr WHERE id = 1", "FOUND"),
        ("C_compr_past",        "compr ext, past-prefix k3 (Case C, FALLBACK)",
            "SELECT jb ->> 'k3' FROM probe_ext_compr WHERE id = 1", "FALLBACK"),
        ("MISS_compr",          "compr ext, missing key",
            "SELECT jb -> 'nope' FROM probe_ext_compr WHERE id = 1", "MISSING"),
        ("NESTED_fallback",     "compr ext, nested container value",
            "INSERT INTO probe_ext_compr VALUES (2, jsonb_build_object("
            "  'k1', 'a', 'k2', repeat('B',500000),"
            "  'n', '{\"deep\":\"value\"}'::jsonb));"
            "SELECT jb -> 'n' FROM probe_ext_compr WHERE id = 2", "FALLBACK"),
    ]
    rows = []
    for cid, name, sql, expected in cases:
        if cid == "NESTED_fallback":
            # The first statement is the insert; run it without explain.
            psql_stmts = sql.split(";SELECT")
            psql(psql_stmts[0])
            real_sql = "SELECT" + psql_stmts[1]
        else:
            real_sql = sql
        plan = explain_buffers(real_sql)
        bufs = extract_buffers(plan)
        row = dict(label=label, case=cid, name=name, expected=expected,
                   **bufs)
        rows.append(row)
    return rows

def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--label", default="post-F1")
    ap.add_argument("--out", default="/tmp/buf_probe.csv")
    args = ap.parse_args()
    rows = run_probe(args.label)
    # Append (not overwrite) so we can run twice (preF1 and postF1).
    write_header = not Path(args.out).exists()
    with open(args.out, "a", newline="") as f:
        keys = ["label","case","name","expected",
                "Shared Hit Blocks","Shared Read Blocks",
                "Local Hit Blocks","Local Read Blocks",
                "Temp Read Blocks","Temp Written Blocks"]
        w = csv.DictWriter(f, fieldnames=keys)
        if write_header:
            w.writeheader()
        w.writerows(rows)
    print(f"wrote {len(rows)} rows to {args.out} with label={args.label}")
    # Print a small summary too
    for r in rows:
        print(f"  {r['case']:24s} expected={r['expected']:18s} "
              f"shr_read={r['Shared Read Blocks']:5d} "
              f"shr_hit={r['Shared Hit Blocks']:5d}")

if __name__ == "__main__":
    main()
