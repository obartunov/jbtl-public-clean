#!/bin/bash
#
# yoda matrix bench runner.
#
#   A  vanilla cluster, ordinary jsonb
#   B  patched cluster, default toast, jsonb_sort_field_values=off
#   C  patched cluster, default toast, jsonb_sort_field_values=on
#   D  patched cluster, jsonb_toaster_lite plain, sort=on
#   E  patched cluster, jsonb_toaster_lite compressed, sort=on
#
# All five share: schema, data generator seed, shared_buffers,
# work_mem, jit=off, debug build.

set -euo pipefail

VANILLA_BIN=/root/pg-install-vanilla/bin
PATCHED_BIN=/root/pg-install/bin
VANILLA_PORT=5434
PATCHED_PORT=5433

CSV_DIR=/tmp/yoda_csv
mkdir -p "$CSV_DIR"
rm -f "$CSV_DIR"/*.csv

SQL=/home/claude/pg/contrib/jsonb_toaster_lite/test/L14_yoda_one.sql

run_variant() {
    local label=$1
    local bin=$2
    local port=$3
    local db=$4
    shift 4
    local extra_psql_vars=("$@")

    "$bin/psql" -h /tmp -p "$port" -U ubuntu -d template1 -X \
        -c "DROP DATABASE IF EXISTS $db" >/dev/null
    "$bin/psql" -h /tmp -p "$port" -U ubuntu -d template1 -X \
        -c "CREATE DATABASE $db" >/dev/null

    echo
    echo "=================================================="
    echo "  variant $label  (port=$port  db=$db)"
    echo "=================================================="

    "$bin/psql" -h /tmp -p "$port" -U ubuntu -d "$db" -X \
        -v "ON_ERROR_STOP=1" \
        -c "ALTER DATABASE $db SET jbtl.variant TO '$label'" \
        >/dev/null

    "$bin/psql" -h /tmp -p "$port" -U ubuntu -d "$db" -X \
        -v "ON_ERROR_STOP=1" \
        -v "variant=$label" \
        -v "csv_out=$CSV_DIR/${label}.csv" \
        "${extra_psql_vars[@]}" \
        -f "$SQL" 2>&1 | tail -8
}

# ---- A: true vanilla, no extensions, no GUC ------------------
run_variant A "$VANILLA_BIN" "$VANILLA_PORT" jbtl_yoda_a

# ---- B: patched, no toaster, GUC=off -------------------------
run_variant B "$PATCHED_BIN" "$PATCHED_PORT" jbtl_yoda_b \
    -v "use_extensions=t" \
    -v "set_sort_guc=t" -v "sort_guc=off"

# ---- C: patched, no toaster, GUC=on --------------------------
run_variant C "$PATCHED_BIN" "$PATCHED_PORT" jbtl_yoda_c \
    -v "use_extensions=t" \
    -v "set_sort_guc=t" -v "sort_guc=on"

# ---- D: patched, jsonb_toaster_lite plain, GUC=on -----------
run_variant D "$PATCHED_BIN" "$PATCHED_PORT" jbtl_yoda_d \
    -v "use_extensions=t" \
    -v "set_sort_guc=t" -v "sort_guc=on" \
    -v "use_lite=t"

# ---- E: patched, jsonb_toaster_lite compressed, GUC=on ------
run_variant E "$PATCHED_BIN" "$PATCHED_PORT" jbtl_yoda_e \
    -v "use_extensions=t" \
    -v "set_sort_guc=t" -v "sort_guc=on" \
    -v "use_lite=t" -v "use_compression=t"

# ---- merge all CSVs ------------------------------------------
echo
echo "=================================================="
echo "  merging CSVs"
echo "=================================================="
{
    head -1 "$CSV_DIR/A.csv"
    for v in A B C D E; do
        tail -n +2 "$CSV_DIR/$v.csv"
    done
} > "$CSV_DIR/all.csv"

wc -l "$CSV_DIR"/*.csv
echo
echo "merged: $CSV_DIR/all.csv"

# ---- cleanup databases ---------------------------------------
"$VANILLA_BIN/psql" -h /tmp -p $VANILLA_PORT -U ubuntu -d template1 -X \
    -c "DROP DATABASE IF EXISTS jbtl_yoda_a" >/dev/null
for db in jbtl_yoda_b jbtl_yoda_c jbtl_yoda_d jbtl_yoda_e; do
    "$PATCHED_BIN/psql" -h /tmp -p $PATCHED_PORT -U ubuntu -d template1 -X \
        -c "DROP DATABASE IF EXISTS $db" >/dev/null
done
