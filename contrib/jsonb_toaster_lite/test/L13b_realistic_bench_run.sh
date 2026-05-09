#!/bin/bash
#
# jsonb_toaster_lite L1.3b-bench runner (realistic JSONB shape)
#
# Drops/creates a clean DB, runs L13b_realistic_bench.sql, captures
# output to a timestamped file under /tmp.  Repeatable.  Idempotent.
#
# Environment overrides (all optional):
#   PG_BIN, PG_HOST, PG_PORT, PG_USER, DB_NAME, OUT_FILE
#
set -euo pipefail

PG_BIN=${PG_BIN:-/root/pg-install/bin}
PG_HOST=${PG_HOST:-/tmp}
PG_PORT=${PG_PORT:-5433}
PG_USER=${PG_USER:-ubuntu}
DB_NAME=${DB_NAME:-jbtl_bench_db}

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
SQL_FILE="$SCRIPT_DIR/L13b_realistic_bench.sql"

OUT_FILE=${OUT_FILE:-/tmp/L13b_bench_$(date +%Y%m%d_%H%M%S).out}

PSQL="$PG_BIN/psql -h $PG_HOST -p $PG_PORT -U $PG_USER -X"

$PSQL -d template1 -c "DROP DATABASE IF EXISTS $DB_NAME" >/dev/null
$PSQL -d template1 -c "CREATE DATABASE $DB_NAME"          >/dev/null

$PSQL -d "$DB_NAME" -P pager=off -f "$SQL_FILE" \
      > "$OUT_FILE" 2>&1

$PSQL -d template1 -c "DROP DATABASE $DB_NAME" >/dev/null

echo "result: $OUT_FILE"
echo
echo "summary tail:"
tail -120 "$OUT_FILE"
