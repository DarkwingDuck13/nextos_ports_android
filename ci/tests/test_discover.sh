#!/bin/bash
# ci/tests/test_discover.sh — shell unit tests for ci/discover-ports.sh
set -euo pipefail
SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
DISCOVER="$SCRIPT_DIR/../discover-ports.sh"
TMPDIR_TEST="$(mktemp -d)"
trap 'rm -rf "$TMPDIR_TEST"' EXIT

fail() { echo "FAIL: $*" >&2; exit 1; }
pass_count=0

make_readme() {
    local dir="$1"; shift
    local readme="$dir/README.md"
    cat > "$readme" <<'HDR'
# Test Repo
HDR
    echo "" >> "$readme"
    echo "### ✅ Concluídos — jogáveis" >> "$readme"
    echo "| Jogo | Engine | Estado | Pasta |" >> "$readme"
    echo "|---|---|---|---|" >> "$readme"
    for port in "$@"; do
        echo "| **$port** | so-loader | Jogável | [\`ports/$port\`](ports/$port/) |" >> "$readme"
    done
    echo "" >> "$readme"
    echo "### 🚧 Em andamento" >> "$readme"
    echo "| Jogo | Engine | Estado | Pasta |" >> "$readme"
    echo "|---|---|---|---|" >> "$readme"
}

# ── Test 1: 3 known ports, all with directories and build.sh ─────────────────
T1="$TMPDIR_TEST/t1"
mkdir -p "$T1/ports/alpha/src" "$T1/ports/beta/src" "$T1/ports/gamma/src"
touch "$T1/ports/alpha/build.sh" "$T1/ports/beta/build.sh" "$T1/ports/gamma/build.sh"
make_readme "$T1" alpha beta gamma

result=$(bash "$DISCOVER" "$T1/README.md" "$T1" 2>/dev/null)
expected='["alpha","beta","gamma"]'
actual=$(echo "$result" | jq -c 'sort')
[ "$actual" = "$expected" ] || fail "Test 1: expected $expected got $actual"
pass_count=$((pass_count+1))
echo "  PASS test1: 3 ports discovered correctly"

# ── Test 2: one port directory missing → excluded, warning on stderr, exit 0 ─
T2="$TMPDIR_TEST/t2"
mkdir -p "$T2/ports/present/src"
touch "$T2/ports/present/build.sh"
make_readme "$T2" present missing

stderr_out=$(bash "$DISCOVER" "$T2/README.md" "$T2" 2>&1 >/dev/null || true)
result=$(bash "$DISCOVER" "$T2/README.md" "$T2" 2>/dev/null)
actual=$(echo "$result" | jq -c 'sort')
[ "$actual" = '["present"]' ] || fail "Test 2: missing port should be excluded, got $actual"
echo "$stderr_out" | grep -q "missing" || fail "Test 2: expected WARNING about missing port on stderr"
pass_count=$((pass_count+1))
echo "  PASS test2: missing directory excluded with warning"

# ── Test 3: no Concluídos section → exit 1 ───────────────────────────────────
T3="$TMPDIR_TEST/t3"
mkdir -p "$T3"
echo "# Repo with no playable table" > "$T3/README.md"
bash "$DISCOVER" "$T3/README.md" "$T3" >/dev/null 2>&1 && fail "Test 3: should exit 1 on missing section" || true
pass_count=$((pass_count+1))
echo "  PASS test3: missing section causes exit 1"

# ── Test 4: port directory exists but build.sh missing → excluded with warning
T4="$TMPDIR_TEST/t4"
mkdir -p "$T4/ports/hasbuild" "$T4/ports/nobuild"
touch "$T4/ports/hasbuild/build.sh"
make_readme "$T4" hasbuild nobuild

stderr_out=$(bash "$DISCOVER" "$T4/README.md" "$T4" 2>&1 >/dev/null || true)
result=$(bash "$DISCOVER" "$T4/README.md" "$T4" 2>/dev/null)
actual=$(echo "$result" | jq -c 'sort')
[ "$actual" = '["hasbuild"]' ] || fail "Test 4: port without build.sh should be excluded, got $actual"
echo "$stderr_out" | grep -q "build.sh" || fail "Test 4: expected WARNING about missing build.sh"
pass_count=$((pass_count+1))
echo "  PASS test4: missing build.sh excluded with warning"

echo ""
echo "All $pass_count discover tests passed"
