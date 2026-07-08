#!/bin/bash
# ci/tests/test_assemble_zip.sh — shell unit tests for ci/assemble-zip.sh
set -euo pipefail
SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
ASSEMBLE="$SCRIPT_DIR/../assemble-zip.sh"
TMPDIR_TEST="$(mktemp -d)"
trap 'rm -rf "$TMPDIR_TEST"' EXIT

fail() { echo "FAIL: $*" >&2; exit 1; }

# Build a minimal valid aarch64 ELF for testing using dd (just needs to be a file)
# For real checks we use a dummy binary file; check-binary.sh is tested separately
make_dummy_binary() { echo "ELF_STUB" > "$1"; }

make_fixture_port() {
    local base="$1" portname="$2"; shift 2
    local port_dir="$base/ports/$portname"
    mkdir -p "$port_dir/src"
    # Mandatory files
    cp /dev/null "$port_dir/screenshot.png"
    printf '# %s\nTest port.\n' "$portname" > "$port_dir/README.md"
    cat > "$port_dir/${portname^}.sh" <<EOF
#!/bin/bash
GAMEDIR="/roms/ports/$portname"
cd "\$GAMEDIR"
./$portname.aarch64
EOF
    # port.json with optional arch override via extra arg
    local arch="${1:-aarch64}"
    cat > "$port_dir/port.json" <<EOF
{
  "version": 2,
  "name": "${portname}.zip",
  "items": ["${portname^}.sh", "$portname"],
  "items_opt": [],
  "attr": {
    "title": "$portname",
    "porter": ["test"],
    "desc": "test",
    "inst": "test",
    "genres": ["other"],
    "image": {},
    "rtr": false,
    "runtime": [],
    "reqs": [],
    "arch": ["$arch"]
  }
}
EOF
    make_dummy_binary "$port_dir/$portname"
    echo "$port_dir"
}

pass_count=0

# ── Test 1: all optional files present → all in zip ─────────────────────────
T1="$TMPDIR_TEST/t1"
port_dir=$(make_fixture_port "$T1" "testgame")
touch "$port_dir/cover.png" "$port_dir/gameinfo.xml" \
      "$port_dir/testgame.gptk" "$port_dir/alsoft.conf"
mkdir -p "$port_dir/licenses"
echo "MIT" > "$port_dir/licenses/LICENSE.md"

mkdir -p "$T1/dist"
bash "$ASSEMBLE" "testgame" "$port_dir/testgame" "$T1/dist" "$T1" >/dev/null 2>&1
[ -f "$T1/dist/testgame.zip" ] || fail "Test 1: zip not created"
contents=$(unzip -l "$T1/dist/testgame.zip" | awk '{print $4}' | grep -v '/$' | grep -v '^$' | grep -v 'Name\|----' | sort)
echo "$contents" | grep -q "testgame/cover.png"      || fail "Test 1: cover.png missing"
echo "$contents" | grep -q "testgame/gameinfo.xml"   || fail "Test 1: gameinfo.xml missing"
echo "$contents" | grep -q "testgame/testgame.gptk"  || fail "Test 1: .gptk missing"
echo "$contents" | grep -q "testgame/alsoft.conf"    || fail "Test 1: alsoft.conf missing"
echo "$contents" | grep -q "testgame/licenses/"      || fail "Test 1: licenses/ missing"
echo "$contents" | grep -q "testgame/testgame.aarch64" || fail "Test 1: binary missing"
pass_count=$((pass_count+1))
echo "  PASS test1: all optional files present in zip"

# ── Test 2: no optional files → only mandatory files ─────────────────────────
T2="$TMPDIR_TEST/t2"
port_dir=$(make_fixture_port "$T2" "minimal")
mkdir -p "$T2/dist"
bash "$ASSEMBLE" "minimal" "$port_dir/minimal" "$T2/dist" "$T2" >/dev/null 2>&1
[ -f "$T2/dist/minimal.zip" ] || fail "Test 2: zip not created"
contents=$(unzip -l "$T2/dist/minimal.zip" | awk '{print $4}' | grep -v '/$' | grep -v '^$' | grep -v 'Name\|----')
echo "$contents" | grep -q "cover.png"   && fail "Test 2: cover.png should not be present"
echo "$contents" | grep -q "alsoft.conf" && fail "Test 2: alsoft.conf should not be present"
echo "$contents" | grep -q "minimal/minimal.aarch64" || fail "Test 2: binary missing"
pass_count=$((pass_count+1))
echo "  PASS test2: only mandatory files in zip when no optional files"

# ── Test 3: .c source file in port dir → NOT included in zip ─────────────────
T3="$TMPDIR_TEST/t3"
port_dir=$(make_fixture_port "$T3" "srctest")
echo "int main(){}" > "$port_dir/src/main.c"
mkdir -p "$T3/dist"
bash "$ASSEMBLE" "srctest" "$port_dir/srctest" "$T3/dist" "$T3" >/dev/null 2>&1
contents=$(unzip -l "$T3/dist/srctest.zip" | awk '{print $4}')
echo "$contents" | grep -q "\.c" && fail "Test 3: .c file must not appear in zip"
pass_count=$((pass_count+1))
echo "  PASS test3: .c source files not included in zip"

# ── Test 4: port.json with arch=["armhf"] → assembled has arch=["aarch64"] ──
T4="$TMPDIR_TEST/t4"
port_dir=$(make_fixture_port "$T4" "archtest" "armhf")
mkdir -p "$T4/dist"
bash "$ASSEMBLE" "archtest" "$port_dir/archtest" "$T4/dist" "$T4" >/dev/null 2>&1
arch=$(unzip -p "$T4/dist/archtest.zip" "archtest/port.json" | jq -r '.attr.arch[0]')
[ "$arch" = "aarch64" ] || fail "Test 4: arch should be aarch64, got $arch"
pass_count=$((pass_count+1))
echo "  PASS test4: arch field patched to aarch64 regardless of input"

# ── Test 5: items array updated to use arch-suffixed binary name ──────────────
T5="$TMPDIR_TEST/t5"
port_dir=$(make_fixture_port "$T5" "itemtest")
mkdir -p "$T5/dist"
bash "$ASSEMBLE" "itemtest" "$port_dir/itemtest" "$T5/dist" "$T5" >/dev/null 2>&1
items=$(unzip -p "$T5/dist/itemtest.zip" "itemtest/port.json" | jq -r '.items[]')
echo "$items" | grep -q "itemtest/itemtest.aarch64" || fail "Test 5: items must contain itemtest/itemtest.aarch64"
echo "$items" | grep -q "^itemtest$" && fail "Test 5: bare binary name must not remain in items"
pass_count=$((pass_count+1))
echo "  PASS test5: items array uses arch-suffixed binary name"

echo ""
echo "All $pass_count assemble-zip tests passed"
