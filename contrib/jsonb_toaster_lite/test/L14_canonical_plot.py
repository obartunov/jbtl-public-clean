#!/usr/bin/env python3
"""
L1.4 canonical bench plotter.  Reads /tmp/canon_sweep.csv produced
by L14_canonical_sweep.sql and emits paper-style figures.

Three figures, matching slides 31 and 44+ of "One TOAST fits all":

  fig1_vanilla_baseline.png
        Per-call cost vs jsonb size, vanilla -> for key1..key4.
        Reproduces slide 31: 'access time grows linearly with
        jsonb size, regardless of key size and position'.

  fig2_kvmap_effect.png
        Per-call cost vs jsonb size for key1 (scalar at front),
        three contestants: vanilla, lite, lite+KVMap.  Reproduces
        the 'TOASTER vs MASTER' shape from slide 44 -- KVMap line
        stays flat past the TOAST threshold.

  fig3_speedup.png
        Speedup factor (vanilla / lite+KVMap) vs jsonb size, all
        four keys.  Shows asymmetry: scalar keys grow with size,
        container keys stay near 1.
"""

import csv
import os
from collections import defaultdict

import matplotlib
matplotlib.use("Agg")  # no display
import matplotlib.pyplot as plt
import numpy as np

CSV_PATH = "/tmp/canon_sweep.csv"
OUT_DIR  = "/mnt/user-data/outputs"

# ---------------------------------------------------------------- load

rows = []
with open(CSV_PATH) as f:
    rdr = csv.DictReader(f)
    for r in rdr:
        rows.append({
            "i":       int(r["i"]),
            "size":    int(r["jsonb_size_bytes"]),
            "key":     r["key"],
            "variant": r["variant"],
            "us":      float(r["us_per_call"]),
        })

# Index: variant -> key -> sorted list of (size, us)
by = defaultdict(lambda: defaultdict(list))
for r in rows:
    by[r["variant"]][r["key"]].append((r["size"], r["us"]))
for v in by.values():
    for k in v:
        v[k].sort()

def xy(variant, key):
    pts = by[variant][key]
    return np.array([p[0] for p in pts]), np.array([p[1] for p in pts])

# Style: shared treatment across all figures
plt.rcParams.update({
    "font.size":        10,
    "axes.titlesize":   12,
    "axes.labelsize":   11,
    "legend.fontsize":  9,
    "lines.linewidth":  1.6,
    "lines.markersize": 4,
})

KEY_COLOR = {
    "key1": "#1f77b4",  # blue
    "key2": "#d62728",  # red
    "key3": "#2ca02c",  # green
    "key4": "#ff7f0e",  # orange
}
KEY_MARKER = {
    "key1": "o",
    "key2": "s",
    "key3": "^",
    "key4": "D",
}

# TOAST threshold at ~2 KB for STORAGE EXTERNAL.  Mark on each plot.
TOAST_THRESHOLD = 2048

# -------------------------------------------------------- figure 1
# Vanilla baseline: per-call cost vs jsonb size, all keys

fig, ax = plt.subplots(figsize=(8.0, 5.5))
for key in ["key1", "key2", "key3", "key4"]:
    x, y = xy("vanilla", key)
    ax.plot(x, y,
            color=KEY_COLOR[key], marker=KEY_MARKER[key],
            label=f"vanilla j-> '{key}'")

ax.axvline(TOAST_THRESHOLD, color="gray", linestyle=":", alpha=0.7,
           label="TOAST threshold (~2 KB)")
ax.set_xscale("log")
ax.set_yscale("log")
ax.set_xlabel("jsonb size (bytes)")
ax.set_ylabel("per-call cost (microseconds)")
ax.set_title(
    "Vanilla j-> baseline: access time grows linearly with jsonb size,\n"
    "regardless of key (reproduces slide 31)"
)
ax.grid(True, which="both", alpha=0.3)
ax.legend(loc="upper left")
fig.tight_layout()
fig.savefig(os.path.join(OUT_DIR, "fig1_vanilla_baseline.png"), dpi=140)
plt.close(fig)

# -------------------------------------------------------- figure 2
# KVMap effect on key1: vanilla vs lite vs lite+KVMap

fig, ax = plt.subplots(figsize=(8.0, 5.5))

x, y = xy("vanilla",    "key1")
ax.plot(x, y, color="#d62728", marker="s", label="vanilla j-> 'key1'")
x, y = xy("lite",       "key1")
ax.plot(x, y, color="#ff7f0e", marker="D",
        label="lite j-> 'key1' (full detoast)")
x, y = xy("lite_kvmap", "key1")
ax.plot(x, y, color="#1f77b4", marker="o",
        label="lite + KVMap fast path")

ax.axvline(TOAST_THRESHOLD, color="gray", linestyle=":", alpha=0.7,
           label="TOAST threshold (~2 KB)")
ax.set_xscale("log")
ax.set_yscale("log")
ax.set_xlabel("jsonb size (bytes)")
ax.set_ylabel("per-call cost (microseconds)")
ax.set_title(
    "L1.4 effect on key1 (scalar at front of object):\n"
    "KVMap fast path stays flat past TOAST threshold"
)
ax.grid(True, which="both", alpha=0.3)
ax.legend(loc="upper left")
fig.tight_layout()
fig.savefig(os.path.join(OUT_DIR, "fig2_kvmap_effect_key1.png"), dpi=140)
plt.close(fig)

# headline: direct vanilla vs lite+KVMap, no middle 'lite j->' to clutter
fig, ax = plt.subplots(figsize=(8.0, 5.5))
x, y = xy("vanilla",    "key1")
ax.plot(x, y, color="#d62728", marker="s", linewidth=2.0,
        label="vanilla jsonb: j-> 'key1'")
x, y = xy("lite_kvmap", "key1")
ax.plot(x, y, color="#1f77b4", marker="o", linewidth=2.0,
        label="jsonb_toaster_lite + L1.4 KVMap")

# annotate the speedup at the right end
last_v = xy("vanilla",    "key1")[1][-1]
last_k = xy("lite_kvmap", "key1")[1][-1]
last_x = xy("vanilla",    "key1")[0][-1]
ax.annotate(f"{last_v/last_k:.0f}x faster\n({last_v:.0f} us  ->  {last_k:.0f} us)",
            xy=(last_x, last_k), xytext=(last_x*0.4, last_k*0.35),
            fontsize=11, ha="center",
            arrowprops=dict(arrowstyle="->", color="#1f77b4", lw=1.4))

ax.axvline(TOAST_THRESHOLD, color="gray", linestyle=":", alpha=0.7,
           label="TOAST threshold (~2 KB)")
ax.set_xscale("log")
ax.set_yscale("log")
ax.set_xlabel("jsonb size (bytes)")
ax.set_ylabel("per-call cost (microseconds)")
ax.set_title(
    "Headline: vanilla jsonb vs jsonb_toaster_lite+L1.4 on 'key1'\n"
    "(scalar metadata at front; L1.4 holds flat across 1000x size range)"
)
ax.grid(True, which="both", alpha=0.3)
ax.legend(loc="upper left")
fig.tight_layout()
fig.savefig(os.path.join(OUT_DIR, "fig0_headline_vanilla_vs_kvmap.png"), dpi=140)
plt.close(fig)

# Same for key3 (also scalar)
fig, ax = plt.subplots(figsize=(8.0, 5.5))
x, y = xy("vanilla",    "key3")
ax.plot(x, y, color="#d62728", marker="s", label="vanilla j-> 'key3'")
x, y = xy("lite",       "key3")
ax.plot(x, y, color="#ff7f0e", marker="D", label="lite j-> 'key3' (full detoast)")
x, y = xy("lite_kvmap", "key3")
ax.plot(x, y, color="#1f77b4", marker="o", label="lite + KVMap fast path")
ax.axvline(TOAST_THRESHOLD, color="gray", linestyle=":", alpha=0.7,
           label="TOAST threshold (~2 KB)")
ax.set_xscale("log")
ax.set_yscale("log")
ax.set_xlabel("jsonb size (bytes)")
ax.set_ylabel("per-call cost (microseconds)")
ax.set_title("L1.4 effect on key3 (scalar at middle of object)")
ax.grid(True, which="both", alpha=0.3)
ax.legend(loc="upper left")
fig.tight_layout()
fig.savefig(os.path.join(OUT_DIR, "fig2_kvmap_effect_key3.png"), dpi=140)
plt.close(fig)

# -------------------------------------------------------- figure 3
# All four keys, vanilla vs lite+KVMap, in one panel

fig, ax = plt.subplots(figsize=(9.0, 6.0))

for key in ["key1", "key2", "key3", "key4"]:
    x, y_v = xy("vanilla",    key)
    _, y_k = xy("lite_kvmap", key)
    color = KEY_COLOR[key]
    ax.plot(x, y_v, color=color, linestyle="--", marker=KEY_MARKER[key],
            alpha=0.6, label=f"vanilla j-> '{key}'")
    ax.plot(x, y_k, color=color, linestyle="-",  marker=KEY_MARKER[key],
            label=f"lite+KVMap '{key}'")

ax.axvline(TOAST_THRESHOLD, color="gray", linestyle=":", alpha=0.7,
           label="TOAST threshold")
ax.set_xscale("log")
ax.set_yscale("log")
ax.set_xlabel("jsonb size (bytes)")
ax.set_ylabel("per-call cost (microseconds)")
ax.set_title(
    "All keys: vanilla (dashed) vs lite+KVMap (solid)\n"
    "key1, key3 (scalars) are FLAT under KVMap; "
    "key2, key4 (containers) fall back -> track vanilla"
)
ax.grid(True, which="both", alpha=0.3)
ax.legend(loc="upper left", ncol=2)
fig.tight_layout()
fig.savefig(os.path.join(OUT_DIR, "fig3_all_keys.png"), dpi=140)
plt.close(fig)

# -------------------------------------------------------- figure 4
# Speedup factor (vanilla / kvmap) vs size, per key

fig, ax = plt.subplots(figsize=(8.0, 5.5))

for key in ["key1", "key2", "key3", "key4"]:
    x_v, y_v = xy("vanilla",    key)
    x_k, y_k = xy("lite_kvmap", key)
    # x arrays are aligned by construction (same i)
    speedup = y_v / y_k
    ax.plot(x_v, speedup,
            color=KEY_COLOR[key], marker=KEY_MARKER[key],
            label=f"'{key}' ({'scalar' if key in ('key1','key3') else 'container -> fallback'})")

ax.axhline(1.0, color="gray", linestyle="-", alpha=0.5,
           label="parity (= 1x)")
ax.axvline(TOAST_THRESHOLD, color="gray", linestyle=":", alpha=0.7,
           label="TOAST threshold")
ax.set_xscale("log")
ax.set_yscale("log")
ax.set_xlabel("jsonb size (bytes)")
ax.set_ylabel("speedup factor: vanilla / (lite+KVMap)")
ax.set_title(
    "Speedup of L1.4 KVMap fast path over vanilla j->,\n"
    "per key, vs jsonb size (log-log)"
)
ax.grid(True, which="both", alpha=0.3)
ax.legend(loc="upper left")
fig.tight_layout()
fig.savefig(os.path.join(OUT_DIR, "fig4_speedup.png"), dpi=140)
plt.close(fig)

# -------------------------------------------------------- summary

print("written:")
for fn in sorted(os.listdir(OUT_DIR)):
    if fn.startswith("fig") and fn.endswith(".png"):
        path = os.path.join(OUT_DIR, fn)
        print(f"  {path}  ({os.path.getsize(path)} B)")

# print extreme-size summary
def at(variant, key, want_size_min):
    pts = by[variant][key]
    return [(s, u) for s, u in pts if s >= want_size_min]

print()
print("Per-call cost at largest jsonb (i=100, ~355 KB):")
for v in ("vanilla", "lite", "lite_kvmap"):
    for k in ("key1", "key2", "key3", "key4"):
        last = by[v][k][-1]
        print(f"  {v:12s} {k}: size={last[0]:>7d} B  per-call={last[1]:>8.2f} us")
