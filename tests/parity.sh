#!/bin/sh
#
# parity.sh - the differential output-parity suite.
#
# Runs the same queries through sqlsh --compat and through sqlite3(1) and
# diffs the bytes. Parity is claimed only for the modes in MODES; the rest are
# best-effort and are reported, not enforced (see docs/notes/phase5-output-
# parity.md). A missing sqlite3 skips the suite rather than failing it, so the
# gate still works outside the dev shell.
#
# usage: tests/parity.sh [path-to-sqlsh]
set -eu

SQLSH=${1:-bin/sqlsh}
SQLITE=${SQLITE3:-sqlite3}
HERE=$(dirname "$0")

if ! command -v "$SQLITE" >/dev/null 2>&1; then
    echo "parity: sqlite3 not found, skipping"
    exit 0
fi
if [ ! -x "$SQLSH" ]; then
    echo "parity: $SQLSH not built" >&2
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
        "$SQLSH" --compat "-$mode" "$db" "$q" > "$tmp/got" 2>"$tmp/got.err" || true
        if ! cmp -s "$tmp/want" "$tmp/got"; then
            fail=$((fail + 1))
            echo "parity: MISMATCH mode=$mode query=$q"
            diff -u "$tmp/want" "$tmp/got" | sed -n '1,24p' | sed 's/^/    /'
        fi
    done < "$tmp/queries"
done

# The same, with the options that compose with every mode.
for opt in "--headers" "--noheader"; do
    :
done
for extra in "-header" "-noheader"; do
    for mode in box column list csv; do
        checks=$((checks + 1))
        "$SQLITE" "-$mode" "$extra" "$db" "SELECT * FROM nums;" > "$tmp/want" 2>/dev/null || true
        "$SQLSH" --compat "-$mode" "$extra" "$db" "SELECT * FROM nums;" > "$tmp/got" 2>/dev/null || true
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
