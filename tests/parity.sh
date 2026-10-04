#!/bin/sh
#
# parity.sh - the differential output-parity suite.
#
# Runs the same queries through redstone --compat and through sqlite3(1) and
# diffs the bytes. Parity is claimed only for the modes in MODES; the rest are
# best-effort and are reported, not enforced (see docs/notes/phase5-output-
# parity.md). A missing sqlite3 skips the suite rather than failing it, so the
# gate still works outside the dev shell.
#
# usage: tests/parity.sh [path-to-redstone]
set -eu

REDSTONE=${1:-bin/redstone}
SQLITE=${SQLITE3:-sqlite3}
HERE=$(dirname "$0")

if ! command -v "$SQLITE" >/dev/null 2>&1; then
    echo "parity: sqlite3 not found, skipping"
    exit 0
fi
if [ ! -x "$REDSTONE" ]; then
    echo "parity: $REDSTONE not built" >&2
    exit 1
fi

MODES="ascii box column csv html insert json line list markdown quote table tabs"

tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT INT TERM

db=$tmp/parity.db
"$SQLITE" "$db" < "$HERE/parity.sql"

# Each query is one line; the suite runs every mode against every one.
cat > "$tmp/queries" <<'SQL'
SELECT * FROM t;
SELECT * FROM t WHERE id IN (1,2,3);
SELECT * FROM empty;
SELECT * FROM nums;
SELECT label FROM t ORDER BY id;
SELECT raw FROM t WHERE raw IS NOT NULL;
SELECT 1 AS one, 'two' AS two, 3.5 AS three, NULL AS four;
SELECT count(*) AS n, max(num) AS m FROM t;
SQL

fail=0
checks=0
for mode in $MODES; do
    while IFS= read -r q; do
        [ -n "$q" ] || continue
        checks=$((checks + 1))
        "$SQLITE" "-$mode" "$db" "$q" > "$tmp/want" 2>"$tmp/want.err" || true
        "$REDSTONE" --compat "-$mode" "$db" "$q" > "$tmp/got" 2>"$tmp/got.err" || true
        if ! cmp -s "$tmp/want" "$tmp/got"; then
            fail=$((fail + 1))
            echo "parity: MISMATCH mode=$mode query=$q"
            diff -u "$tmp/want" "$tmp/got" | sed -n '1,24p' | sed 's/^/    /'
        fi
    done < "$tmp/queries"
done

# The dot commands whose output is either fed back into SQLite (.dump,
# .fullschema, .clone) or diffed by hand (.schema, .dbinfo, .lint). Each is
# compared byte for byte: these have no "pretty" variant to fall back on, so a
# difference here is a bug rather than a design choice.
cat > "$tmp/dots" <<'DOT'
.tables
.tables t%
.indexes
.indexes t
.databases
.schema
.schema --indent
.schema --nosys
.schema t
.schema v
.schema sqlite_schema
.fullschema
.dump
.dump --data-only
.dump --nosys
.dump t
.show
.dbinfo
.limit
.limit attached
.dbconfig
.dbconfig defensive
.lint fkey-indexes
.lint fkey-indexes -verbose
DOT

while IFS= read -r dot; do
    [ -n "$dot" ] || continue
    checks=$((checks + 1))
    "$SQLITE" "$db" "$dot" > "$tmp/want" 2>&1 || true
    "$REDSTONE" --compat "$db" "$dot" > "$tmp/got" 2>&1 || true
    if ! cmp -s "$tmp/want" "$tmp/got"; then
        fail=$((fail + 1))
        echo "parity: MISMATCH $dot"
        diff -u "$tmp/want" "$tmp/got" | sed -n '1,24p' | sed 's/^/    /'
    fi
done < "$tmp/dots"

# A .dump has to restore, and restore to the same thing sqlite3(1)'s own dump
# does. Both are replayed into a fresh database and the results compared, so
# the check covers quoting and statement order without asserting that a
# restored schema lists its objects in the original order -- it does not, for
# either shell, because .dump emits tables before indexes.
checks=$((checks + 1))
"$REDSTONE" --compat "$db" ".dump" > "$tmp/got.sql" 2>/dev/null || true
"$SQLITE" "$db" ".dump" > "$tmp/want.sql" 2>/dev/null || true
rm -f "$tmp/got.db" "$tmp/want.db"
"$SQLITE" "$tmp/got.db" < "$tmp/got.sql" > /dev/null 2>&1 || true
"$SQLITE" "$tmp/want.db" < "$tmp/want.sql" > /dev/null 2>&1 || true
"$SQLITE" "$tmp/want.db" ".dump" > "$tmp/want" 2>/dev/null || true
"$SQLITE" "$tmp/got.db" ".dump" > "$tmp/got" 2>/dev/null || true
if ! cmp -s "$tmp/want" "$tmp/got"; then
    fail=$((fail + 1))
    echo "parity: MISMATCH .dump does not round-trip"
    diff -u "$tmp/want" "$tmp/got" | sed -n '1,24p' | sed 's/^/    /'
fi

# .import has to read back what .mode csv wrote, including the rows with a
# comma, a doubled quote and an embedded newline in them -- so the CSV is
# produced by sqlite3(1) and both shells import it into a fresh table, whose
# contents are then compared. An importer that mishandles RFC 4180 quoting
# passes every formatting check above and still loses data here.
checks=$((checks + 1))
"$SQLITE" -csv -header "$db" "SELECT * FROM t;" > "$tmp/round.csv" 2>/dev/null || true
for shell in want got; do
    rm -f "$tmp/imp.$shell.db"
done
"$SQLITE" "$tmp/imp.want.db" ".import --csv $tmp/round.csv t" ".dump" > "$tmp/want" 2>&1 || true
"$REDSTONE" --compat "$tmp/imp.got.db" ".import --csv $tmp/round.csv t" ".dump" > "$tmp/got" 2>&1 || true
if ! cmp -s "$tmp/want" "$tmp/got"; then
    fail=$((fail + 1))
    echo "parity: MISMATCH .import csv round-trip"
    diff -u "$tmp/want" "$tmp/got" | sed -n '1,24p' | sed 's/^/    /'
fi

# The same, with the options that compose with every mode.
for extra in "-header" "-noheader"; do
    for mode in box column list csv; do
        checks=$((checks + 1))
        "$SQLITE" "-$mode" "$extra" "$db" "SELECT * FROM nums;" > "$tmp/want" 2>/dev/null || true
        "$REDSTONE" --compat "-$mode" "$extra" "$db" "SELECT * FROM nums;" > "$tmp/got" 2>/dev/null || true
        if ! cmp -s "$tmp/want" "$tmp/got"; then
            fail=$((fail + 1))
            echo "parity: MISMATCH mode=$mode $extra"
            diff -u "$tmp/want" "$tmp/got" | sed -n '1,24p' | sed 's/^/    /'
        fi
    done
done

if [ "$fail" -ne 0 ]; then
    echo "parity: $fail of $checks checks FAILED"
    exit 1
fi
echo "parity: $checks checks passed"
