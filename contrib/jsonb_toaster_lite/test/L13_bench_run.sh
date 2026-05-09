#!/bin/bash
#
# jsonb_toaster_lite L1.3-bench runner
#
# Drops/creates a clean DB, runs L13_bench.sql, captures output to a
# timestamped file under /tmp.  Repeatable.  Idempotent.
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
SQL_FILE="$SCRIPT_DIR/L13_bench.sql"

OUT_FILE=${OUT_FILE:-/tmp/L13_bench_$(date +%Y%m%d_%H%M%S).out}

PSQL="$PG_BIN/psql -h $PG_HOST -p $PG_PORT -U $PG_USER -X"

# Recreate the test database so each run starts from a clean state.
$PSQL -d template1 -c "DROP DATABASE IF EXISTS $DB_NAME" >/dev/null
$PSQL -d template1 -c "CREATE DATABASE $DB_NAME"          >/dev/null

# Run the bench.  -P pager=off so the script does not block on a pager
# when run interactively; -a echoes commands so the output captures the
# operation timing context.
$PSQL -d "$DB_NAME" -P pager=off -f "$SQL_FILE" \
      > "$OUT_FILE" 2>&1

# Drop the bench DB after the run; comment out for post-mortem
$PSQL -d template1 -c "DROP DATABASE $DB_NAME" >/dev/null

echo "result: $OUT_FILE"
echo
echo "summary tail:"
tail -80 "$OUT_FILE"
