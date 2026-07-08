#!/bin/bash
# ci/tests/run-all.sh — run all ci/tests/test_*.sh shell unit tests
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
pass=0; fail=0; skip=0

for test_file in "$SCRIPT_DIR"/test_*.sh; do
    [ -f "$test_file" ] || continue
    name="$(basename "$test_file")"
    if [ ! -x "$test_file" ]; then
        echo "SKIP  $name (not executable)"
        skip=$((skip+1))
        continue
    fi
    if bash "$test_file" >/tmp/test_out_$$ 2>&1; then
        echo "PASS  $name"
        pass=$((pass+1))
    else
        echo "FAIL  $name"
        cat /tmp/test_out_$$
        fail=$((fail+1))
    fi
    rm -f /tmp/test_out_$$
done

echo ""
echo "Results: $pass passed, $fail failed, $skip skipped"
[ $fail -eq 0 ]
