#!/usr/bin/env python3
"""
yoda matrix plotter: 5-variant decomposition.

Reads /tmp/yoda_csv/A.csv .. E.csv (E.csv contains both E and E_fast),
produces:

  yoda_fig0_headline_key3.png    HEADLINE - A vs C vs D_fast on key3
                                 (the clean KVMap test, per @yoda)
  yoda_fig0b_key1_vs_key3.png    key1 (degenerate) vs key3 (clean)
                                 overlay
  yoda_fig0c_control_key1.png    A vs C vs D_fast on key1
                                 (degenerate control, kept for
                                 comparison)
  yoda_fig1a_decomp_key1.png     all 7 lines for key1 (degenerate)
  yoda_fig1b_decomp_key3.png     all 7 lines for key3 (clean)
  yoda_fig1c_decomp_key2.png     all 7 lines for key2 (HUGE container,
                                 fallback control)
  yoda_fig1d_decomp_key4.png     all 7 lines for key4 (medium
                                 container, fallback control)
  yoda_fig3_no_regression.png    A vs B alone — proves K1 patches with
                                 GUC=off do not regress
  yoda_fig4_speedup_vs_A_key3.png   speedup over A per variant, key3
  yoda_fig5_speedup_vs_C_key3.png   speedup over C per variant, key3
                                    (isolates storage+L1.4 effect from
                                    KVMap)

Sizes for D and E come from `pg_column_size` which reports the
toast pointer, not the logical jsonb.  We use variant A's
size as the canonical logical jsonb size for the X axis.

Per @yoda's review: the headline is key3, NOT key1, because
key3 lives logically after the HUGE key2 array and would
physically be in late toast chunks WITHOUT the K1 KVMap layout.
The fact that L1.4 fast path keeps key3 access flat at ~22 us
across the full size range is the proof that K1+L1.4 combined
work as designed.
"""

import csv
import os
import sys
from collections import defaultdict

import matplotlib
matplotlib.use("Agg")
import matplotlib.pyplot as plt
import numpy as np

CSV_DIR = "/tmp/yoda_csv"
OUT_DIR = "/mnt/user-data/outputs"

# ----- load --------------------------------------------------

rows = []
for fn in ("A", "B", "C", "D", "E"):
    path = os.path.join(CSV_DIR, f"{fn}.csv")
    with open(path) as f:
        for r in csv.DictReader(f):
            v = r["variant"]
            # normalize: 'e' -> 'E', 'e_fast' -> 'E_fast' (case lost
            # via unquoted ALTER DATABASE in retry of variant E)
            if v.endswith("_fast"):
                v = v[:-5].upper() + "_fast"
            else:
                v = v.upper()
            rows.append({
                "variant": v,
                "i":       int(r["i"]),
                "size":    int(r["jsonb_size_bytes"]),
                "key":     r["key"],
                "path":    r["path"],
                "us":      float(r["us_per_call"]),
            })

# Canonical logical size = size from variant A (where pg_column_size
# is the actual jsonb size, not a toast pointer).
canonical_size = {}
for r in rows:
    if r["variant"] == "A":
        canonical_size[(r["i"], r["key"])] = r["size"]

# By (variant, key) -> sorted list of (logical_size, us)
by = defaultdict(lambda: defaultdict(list))
for r in rows:
    sz = canonical_size.get((r["i"], r["key"]), r["size"])
    by[r["variant"]][r["key"]].append((r["i"], sz, r["us"]))
for v in by.values():
    for k in v:
        v[k].sort(key=lambda t: t[0])  # sort by i

def xy(variant, key):
    pts = by[variant][key]
    return np.array([p[1] for p in pts]), np.array([p[2] for p in pts])

print("loaded variants:", sorted(by.keys()))

# ----- styling ----------------------------------------------

plt.rcParams.update({
    "font.size":        10,
    "axes.titlesize":   12,
    "axes.labelsize":   11,
    "legend.fontsize":  9,
    "lines.linewidth":  1.6,
    "lines.markersize": 4,
})

VARIANT_STYLE = {
    "A":      dict(color="#000000", marker="o", linestyle="-",
                   label="A: vanilla master, j->"),
    "B":      dict(color="#7f7f7f", marker="s", linestyle="--",
                   label="B: patched, sort=off, j->"),
    "C":      dict(color="#1f77b4", marker="^", linestyle="-",
                   label="C: patched, sort=on, j->"),
    "D":      dict(color="#2ca02c", marker="D", linestyle="-",
                   label="D: lite plain, j->"),
    "E":      dict(color="#bcbd22", marker="v", linestyle="-",
                   label="E: lite pglz, j->"),
    "D_fast": dict(color="#d62728", marker="o", linestyle="-",
                   label="D_fast: lite + L1.4 (jbtl_object_field)"),
    "E_fast": dict(color="#ff7f0e", marker="P", linestyle="--",
                   label="E_fast: lite_pglz + L1.4"),
}

TOAST_THRESHOLD = 2048

def base_axes(ax, title, ylabel="per-call cost (us)"):
    ax.axvline(TOAST_THRESHOLD, color="gray", linestyle=":", alpha=0.7,
               label="TOAST threshold")
    ax.set_xscale("log"); ax.set_yscale("log")
    ax.set_xlabel("jsonb logical size (bytes)")
    ax.set_ylabel(ylabel)
    ax.set_title(title)
    ax.grid(True, which="both", alpha=0.3)

def plot_per_key(key, fname_suffix, title_extra=""):
    """One figure per key showing all 7 variants."""
    fig, ax = plt.subplots(figsize=(9.5, 6.5))
    for v in ("A", "B", "C", "D", "E", "D_fast", "E_fast"):
        if v not in by:
            continue
        x, y = xy(v, key)
        ax.plot(x, y, **VARIANT_STYLE[v])
    base_axes(ax, f"All variants for '{key}'{title_extra}")
    ax.legend(loc="upper left", ncol=1, fontsize=8.5)
    fig.tight_layout()
    fig.savefig(os.path.join(OUT_DIR, f"yoda_{fname_suffix}.png"), dpi=140)
    plt.close(fig)

# All four keys, full decomposition each.
plot_per_key("key1", "fig1a_decomp_key1",
    "\n(scalar at front; logical=physical even without KVMap → DEGENERATE control)")
plot_per_key("key3", "fig1b_decomp_key3",
    "\n(scalar at logical-pos 3; without KVMap its value lives AFTER HUGE key2 — CLEAN test)")
plot_per_key("key2", "fig1c_decomp_key2",
    "\n(HUGE array container; KVMap fast path falls back via JBE_ISCONTAINER — CONTAINER control)")
plot_per_key("key4", "fig1d_decomp_key4",
    "\n(medium array container; same fallback path as key2 — CONTAINER control)")

# HEADLINE figure: key3 isolated three-way (the real KVMap test, per @yoda).
fig, ax = plt.subplots(figsize=(9.5, 6.0))
for v in ("A", "C", "D_fast"):
    if v not in by:
        continue
    x, y = xy(v, "key3")
    ax.plot(x, y, **VARIANT_STYLE[v])
xa, ya = xy("A", "key3")
_, yd = xy("D_fast", "key3")
ax.annotate(
    f"A: {ya[-1]:.0f} us\nD_fast: {yd[-1]:.0f} us\nratio: {ya[-1]/yd[-1]:.1f}x",
    xy=(xa[-1], yd[-1]),
    xytext=(xa[-1]*0.18, yd[-1]*0.45),
    fontsize=10, ha="left",
    arrowprops=dict(arrowstyle="->", color="#d62728", lw=1.4))
base_axes(ax,
    "HEADLINE: key3 (logical pos 3, small value, between two big values).\n"
    "Without KVMap, key3's value lives in late chunks (after HUGE key2);\n"
    "with KVMap, its value is in chunk 0 → flat curve on D_fast.\n"
    "This is the first read-path product proof of K1+L1.4 combined.")
ax.legend(loc="upper left")
fig.tight_layout()
fig.savefig(os.path.join(OUT_DIR, "yoda_fig0_headline_key3.png"), dpi=140)
plt.close(fig)

# Companion: degenerate key1 isolated three-way for comparison
fig, ax = plt.subplots(figsize=(9.5, 6.0))
for v in ("A", "C", "D_fast"):
    if v not in by:
        continue
    x, y = xy(v, "key1")
    ax.plot(x, y, **VARIANT_STYLE[v])
base_axes(ax,
    "Control: key1 (logical pos 1, scalar at FRONT).\n"
    "Looks similar to key3 result, but here logical=physical even WITHOUT KVMap;\n"
    "this shape would emerge for any front-of-object scalar.\n"
    "Use this as DEGENERATE control, not as the headline.")
ax.legend(loc="upper left")
fig.tight_layout()
fig.savefig(os.path.join(OUT_DIR, "yoda_fig0c_control_key1.png"), dpi=140)
plt.close(fig)

# Both scalars overlay: key1 (degenerate control) and key3 (headline)
fig, ax = plt.subplots(figsize=(9.5, 6.5))
ax.plot(*xy("A", "key1"),      color="#1f77b4", marker="o", linestyle="--",
        label="A (vanilla) j-> 'key1'  [DEGENERATE: logical=physical]")
ax.plot(*xy("A", "key3"),      color="#d62728", marker="s", linestyle="--",
        label="A (vanilla) j-> 'key3'  [CLEAN: physically late, after HUGE key2]")
ax.plot(*xy("D_fast", "key1"), color="#1f77b4", marker="o", linestyle="-",
        label="D_fast (lite + L1.4) 'key1'")
ax.plot(*xy("D_fast", "key3"), color="#d62728", marker="s", linestyle="-",
        label="D_fast (lite + L1.4) 'key3'  ← THIS is the clean KVMap proof")
base_axes(ax,
    "key1 vs key3: vanilla (dashed) and L1.4 fast path (solid).\n"
    "Both flat under L1.4 BECAUSE KVMap puts small values in first chunk\n"
    "regardless of their logical position.  Without KVMap, key3 would NOT be flat.")
ax.legend(loc="upper left", fontsize=9)
fig.tight_layout()
fig.savefig(os.path.join(OUT_DIR, "yoda_fig0b_key1_vs_key3.png"), dpi=140)
plt.close(fig)

# ----- figure 2: isolate A vs C vs D_fast --------------------

fig, ax = plt.subplots(figsize=(9.0, 6.0))
for v in ("A", "C", "D_fast"):
    if v not in by:
        continue
    x, y = xy(v, "key1")
    ax.plot(x, y, **VARIANT_STYLE[v])

# Annotate ratio at largest point
xa, ya = xy("A", "key1")
_, yd = xy("D_fast", "key1")
ax.annotate(
    f"A: {ya[-1]:.0f} us\nD_fast: {yd[-1]:.0f} us\nratio: {ya[-1]/yd[-1]:.1f}x",
    xy=(xa[-1], yd[-1]),
    xytext=(xa[-1]*0.18, yd[-1]*0.45),
    fontsize=10, ha="left",
    arrowprops=dict(arrowstyle="->", color="#d62728", lw=1.4))

base_axes(ax,
    "Isolation: A (true vanilla) vs C (KVMap only) vs D_fast (KVMap + L1.4)\n"
    "key1 scalar at front")
ax.legend(loc="upper left")
fig.tight_layout()
fig.savefig(os.path.join(OUT_DIR, "yoda_fig2_isolate.png"), dpi=140)
plt.close(fig)

# ----- figure 3: no-regression check (A vs B) ---------------

fig, ax = plt.subplots(figsize=(9.0, 5.5))
for v in ("A", "B"):
    if v not in by:
        continue
    x, y = xy(v, "key1")
    ax.plot(x, y, **VARIANT_STYLE[v])
base_axes(ax,
    "No-regression check: A (vanilla master) vs B (patched, sort=off)\n"
    "K1 patches must not slow down legacy workload when GUC is off")
ax.legend(loc="upper left")
fig.tight_layout()
fig.savefig(os.path.join(OUT_DIR, "yoda_fig3_no_regression.png"), dpi=140)
plt.close(fig)

# Print A vs B numerical diff per i
print("\nA vs B diff (key1):")
print(f"  {'size':>10s}  {'A_us':>8s}  {'B_us':>8s}  {'B/A':>6s}")
xa, ya = xy("A", "key1")
xb, yb = xy("B", "key1")
for sz, a_us, b_us in zip(xa, ya, yb):
    if sz > 1000:
        print(f"  {sz:>10d}  {a_us:>8.2f}  {b_us:>8.2f}  {b_us/a_us:>5.2f}x")

# ----- figure 4: speedup over A (true vanilla) — key3 (CLEAN test)

fig, ax = plt.subplots(figsize=(9.5, 6.0))
xa, ya = xy("A", "key3")
for v in ("B", "C", "D", "E", "D_fast", "E_fast"):
    if v not in by:
        continue
    xv, yv = xy(v, "key3")
    speedup = ya / yv  # A_time / V_time
    ax.plot(xv, speedup, **VARIANT_STYLE[v])
ax.axhline(1.0, color="gray", linestyle="-", alpha=0.5, label="parity vs A")
ax.axvline(TOAST_THRESHOLD, color="gray", linestyle=":", alpha=0.7,
           label="TOAST threshold")
ax.set_xscale("log"); ax.set_yscale("log")
ax.set_xlabel("jsonb logical size (bytes)")
ax.set_ylabel("speedup factor: A_time / variant_time")
ax.set_title("Speedup vs A (true vanilla) per variant — key3 (clean KVMap test)")
ax.grid(True, which="both", alpha=0.3)
ax.legend(loc="upper left", ncol=1, fontsize=9)
fig.tight_layout()
fig.savefig(os.path.join(OUT_DIR, "yoda_fig4_speedup_vs_A_key3.png"), dpi=140)
plt.close(fig)

# ----- figure 5: speedup over C (isolates storage+L1.4) — key3

fig, ax = plt.subplots(figsize=(9.5, 6.0))
xc, yc = xy("C", "key3")
for v in ("D", "E", "D_fast", "E_fast"):
    if v not in by:
        continue
    xv, yv = xy(v, "key3")
    speedup = yc / yv
    ax.plot(xv, speedup, **VARIANT_STYLE[v])
ax.axhline(1.0, color="gray", linestyle="-", alpha=0.5, label="parity vs C")
ax.axvline(TOAST_THRESHOLD, color="gray", linestyle=":", alpha=0.7,
           label="TOAST threshold")
ax.set_xscale("log"); ax.set_yscale("log")
ax.set_xlabel("jsonb logical size (bytes)")
ax.set_ylabel("speedup: C_time / variant_time")
ax.set_title(
    "Speedup vs C (KVMap layout only, default toast) — isolates storage+L1.4\n"
    "key3 (clean KVMap test)")
ax.grid(True, which="both", alpha=0.3)
ax.legend(loc="upper left")
fig.tight_layout()
fig.savefig(os.path.join(OUT_DIR, "yoda_fig5_speedup_vs_C_key3.png"), dpi=140)
plt.close(fig)

# ----- print numerical summary ------------------------------

for k in ("key1", "key2", "key3", "key4"):
    print(f"\nSummary table ({k}) at i=100 (~396 KB jsonb):")
    print(f"  {'variant':<10s}  {'us':>10s}  {'us / A':>8s}  {'us / C':>8s}")
    if "A" not in by or k not in by["A"]:
        print("  no data"); continue
    ya = xy("A", k)[1][-1]
    yc = xy("C", k)[1][-1] if "C" in by and k in by["C"] else None
    for v in ("A", "B", "C", "D", "E", "D_fast", "E_fast"):
        if v not in by or k not in by[v]:
            continue
        last = xy(v, k)[1][-1]
        cratio = f"{last/yc:>7.3f}x" if yc else "    -   "
        print(f"  {v:<10s}  {last:>10.2f}  {last/ya:>7.3f}x  {cratio}")

print("\nWritten figures:")
for fn in sorted(os.listdir(OUT_DIR)):
    if fn.startswith("yoda_") and fn.endswith(".png"):
        path = os.path.join(OUT_DIR, fn)
        print(f"  {path}  ({os.path.getsize(path)} B)")
