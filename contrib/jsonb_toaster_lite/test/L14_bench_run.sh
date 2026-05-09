#!/bin/bash
#
# L1.4 bench runner.  Runs both:
#   L14_cold_bench.sql      (per-op cold cache, eviction before each call)
#   L14_seqscan_bench.sql   (5000-row seq scan, natural cache pressure)
#
set -euo pipefail

PG_BIN=${PG_BIN:-/root/pg-install/bin}
PG_HOST=${PG_HOST:-/tmp}
PG_PORT=${PG_PORT:-5433}
PG_USER=${PG_USER:-ubuntu}
DB_NAME=${DB_NAME:-jbtl_bench_db}

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

OUT_FILE=${OUT_FILE:-/tmp/L14_bench_$(date +%Y%m%d_%H%M%S).out}

PSQL="$PG_BIN/psql -h $PG_HOST -p $PG_PORT -U $PG_USER -X"

$PSQL -d template1 -c "DROP DATABASE IF EXISTS $DB_NAME" >/dev/null
$PSQL -d template1 -c "CREATE DATABASE $DB_NAME"          >/dev/null

{
  echo "=========================================="
  echo "  L1.4 cold-per-op bench"
  echo "=========================================="
  $PSQL -d "$DB_NAME" -P pager=off -f "$SCRIPT_DIR/L14_cold_bench.sql"

  $PSQL -d template1 -c "DROP DATABASE IF EXISTS $DB_NAME" >/dev/null
  $PSQL -d template1 -c "CREATE DATABASE $DB_NAME"          >/dev/null

  echo
  echo "=========================================="
  echo "  L1.4 seq-scan bench"
  echo "=========================================="
  $PSQL -d "$DB_NAME" -P pager=off -f "$SCRIPT_DIR/L14_seqscan_bench.sql"
} > "$OUT_FILE" 2>&1

$PSQL -d template1 -c "DROP DATABASE $DB_NAME" >/dev/null

echo "result: $OUT_FILE"
echo
echo "summary tail:"
tail -100 "$OUT_FILE"
