#!/usr/bin/env python3
"""
Backend-RSS probe v2 — uses a persistent psql session via Popen.

We open one psql, capture its server backend pid (the first SELECT
result), read /proc/<pid>/status, run the FOUND scan, read /proc
again, close.

We do this once per --pass to amortise startup jitter; the
reported numbers are the median across passes.
"""
from __future__ import annotations
import argparse, csv, re, subprocess, sys, time
from pathlib import Path

def read_status(pid: int):
    with open(f"/proc/{pid}/status") as f:
        text = f.read()
    keys = ("VmRSS","VmHWM","VmData","VmSize")
    out = {}
    for line in text.splitlines():
        m = re.match(r"^(\w+):\s+(\d+)\s+kB", line)
        if m and m.group(1) in keys:
            out[m.group(1)] = int(m.group(2))
    return out

def one_pass(host, port, db, rows):
    p = subprocess.Popen(
        ["psql","-X","-h",host,"-p",str(port),"-d",db,
         "-q","-A","-t","-v","ON_ERROR_STOP=1","-P","pager=off"],
        stdin=subprocess.PIPE, stdout=subprocess.PIPE,
        stderr=subprocess.PIPE, text=True, bufsize=1)
    def send(s):
        p.stdin.write(s + "\n")
        p.stdin.flush()
    def read_until(marker, timeout=30):
        start = time.time()
        buf = []
        while time.time() - start < timeout:
            line = p.stdout.readline()
            if not line: break
            buf.append(line)
            if marker in line:
                return "".join(buf)
        return "".join(buf)
    # Capture backend pid
    send("SELECT pg_backend_pid();")
    send("\\echo BACKEND_PID_DONE")
    out = read_until("BACKEND_PID_DONE")
    pid_line = [l for l in out.splitlines() if l.strip().isdigit()]
    pid = int(pid_line[0])
    before = read_status(pid)
    # Scan
    send(f"""
DO $$ DECLARE s text; cnt int := 0; BEGIN
  FOR s IN SELECT jb ->> 'k1' FROM rss_probe LOOP cnt := cnt+1; END LOOP;
  FOR s IN SELECT jb ->> 'k3' FROM rss_probe LOOP cnt := cnt+1; END LOOP;
END $$;
""")
    send("\\echo SCAN_DONE")
    read_until("SCAN_DONE")
    after = read_status(pid)
    send("\\q")
    try: p.wait(timeout=5)
    except subprocess.TimeoutExpired: p.kill()
    return pid, before, after

def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--label", required=True)
    ap.add_argument("--host", default="/tmp")
    ap.add_argument("--port", type=int, default=5432)
    ap.add_argument("--db", default="probe")
    ap.add_argument("--rows", type=int, default=3000)
    ap.add_argument("--passes", type=int, default=3)
    ap.add_argument("--out", default="/tmp/rss_probe.csv")
    args = ap.parse_args()

    # Setup fixture
    subprocess.run(
        ["psql","-X","-h",args.host,"-p",str(args.port),"-d",args.db,
         "-q","-c",f"""
DROP TABLE IF EXISTS rss_probe;
CREATE TABLE rss_probe (id int PRIMARY KEY, jb jsonb);
ALTER TABLE rss_probe ALTER COLUMN jb SET STORAGE EXTERNAL;
INSERT INTO rss_probe
SELECT g, jsonb_build_object(
    'k1', 'short_at_start_'||g,
    'k2', repeat('M', 6000),
    'k3', 'past_prefix_value_'||g)
FROM generate_series(1, {args.rows}) g;
CHECKPOINT;
"""], check=True, text=True, stdout=subprocess.PIPE)

    rows = []
    for i in range(args.passes):
        pid, before, after = one_pass(args.host, args.port, args.db, args.rows)
        rows.append(dict(
            label=args.label, pass_no=i, pid=pid, rows=args.rows,
            VmRSS_before=before.get("VmRSS"),  VmRSS_after=after.get("VmRSS"),
            VmHWM_before=before.get("VmHWM"),  VmHWM_after=after.get("VmHWM"),
            VmData_before=before.get("VmData"),VmData_after=after.get("VmData"),
        ))

    write_header = not Path(args.out).exists()
    with open(args.out, "a", newline="") as f:
        keys = ["label","pass_no","pid","rows",
                "VmRSS_before","VmRSS_after",
                "VmHWM_before","VmHWM_after",
                "VmData_before","VmData_after"]
        w = csv.DictWriter(f, fieldnames=keys)
        if write_header: w.writeheader()
        w.writerows(rows)
    print(f"wrote {len(rows)} rows to {args.out} label={args.label}")
    for r in rows:
        rss_d  = r['VmRSS_after']  - r['VmRSS_before']
        hwm_d  = r['VmHWM_after']  - r['VmHWM_before']
        data_d = r['VmData_after'] - r['VmData_before']
        print(f"  pass {r['pass_no']}: pid={r['pid']:6d}  "
              f"VmRSS  {r['VmRSS_before']:>8d} -> {r['VmRSS_after']:>8d}  Δ {rss_d:+6d}    "
              f"VmHWM Δ {hwm_d:+6d}    VmData Δ {data_d:+6d}    (kB)")

if __name__ == "__main__":
    main()
