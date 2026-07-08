#!/bin/bash
# ci/check-binary.sh — validate an aarch64 port binary before packaging
# Usage: ci/check-binary.sh <portname> <binary_path>
#
# Runs four quality gates in order:
#   1. ELF arch check     — must be aarch64 dynamically-linked ELF
#   2. glibc version check — must require no higher than GLIBC_2.17
#   3. Bundled-lib check  — must not DT_NEED libSDL2/libopenal/libmali/libmpg123
#   4. Cleanliness checks — no forbidden debug symbols; warn on unguarded GLSTATE/GLDRAW
#
# Exits 0 only when all hard checks pass.
# Warnings (GLSTATE/GLDRAW without guard) are printed but do not cause exit 1.

set -euo pipefail

PORTNAME="${1:?Usage: $0 <portname> <binary_path>}"
BINARY="${2:?Usage: $0 <portname> <binary_path>}"

PASS=0
FAIL=1

check_failed=0

_fail() {
  echo "[check-binary] FAIL: $*" >&2
  check_failed=1
}

_warn() {
  echo "[check-binary] WARNING: $*"
}

_ok() {
  echo "[check-binary] OK: $*"
}

if [ ! -f "$BINARY" ]; then
  echo "[check-binary] ERROR: binary not found: $BINARY" >&2
  exit 1
fi

echo "[check-binary] ============================================"
echo "[check-binary] Port:   $PORTNAME"
echo "[check-binary] Binary: $BINARY"
echo "[check-binary] ============================================"

# ── Check 1: ELF arch ────────────────────────────────────────────────────────
echo "[check-binary] --- Check 1: ELF architecture ---"
file_output=$(file "$BINARY")
echo "[check-binary] file output: $file_output"

if echo "$file_output" | grep -q "ELF 64-bit LSB" && \
   echo "$file_output" | grep -q "aarch64" && \
   echo "$file_output" | grep -q "dynamically linked"; then
  _ok "ELF 64-bit LSB aarch64 dynamically linked"
else
  _fail "Binary is not a valid aarch64 dynamically-linked ELF"
  echo "[check-binary]   Got: $file_output" >&2
  # Hard failure — remaining checks are meaningless
  exit 1
fi

# ── Check 2: glibc version ───────────────────────────────────────────────────
# Limit set to 2.35 — Knulli/Batocera Buildroot toolchain ships glibc 2.35
# on modern aarch64 targets, and Ubuntu 22.04 cross-compiler links against 2.35.
echo "[check-binary] --- Check 2: glibc version (max allowed: 2.35) ---"
MAX_MAJOR=0
MAX_MINOR=0
OFFENDING=""

# Parse all GLIBC_X.Y version strings from the dynamic section
while IFS= read -r ver; do
  major=$(echo "$ver" | cut -d. -f1)
  minor=$(echo "$ver" | cut -d. -f2)
  if [ "$major" -gt "$MAX_MAJOR" ] || \
     { [ "$major" -eq "$MAX_MAJOR" ] && [ "$minor" -gt "$MAX_MINOR" ]; }; then
    MAX_MAJOR=$major
    MAX_MINOR=$minor
  fi
done < <(objdump -p "$BINARY" 2>/dev/null | grep -oE 'GLIBC_[0-9]+\.[0-9]+' | \
         sed 's/GLIBC_//' | sort -t. -k1,1n -k2,2n -u)

if [ "$MAX_MAJOR" -eq 0 ] && [ "$MAX_MINOR" -eq 0 ]; then
  _ok "No versioned glibc symbols found (static or pre-2.35 only)"
elif [ "$MAX_MAJOR" -gt 2 ] || { [ "$MAX_MAJOR" -eq 2 ] && [ "$MAX_MINOR" -gt 35 ]; }; then
  OFFENDING=$(objdump -p "$BINARY" 2>/dev/null | grep -oE 'GLIBC_[0-9]+\.[0-9]+' | sort -u)
  _fail "Binary requires glibc $MAX_MAJOR.$MAX_MINOR (> 2.35 limit)"
  echo "[check-binary]   All versioned glibc requirements:" >&2
  echo "$OFFENDING" | sed 's/^/[check-binary]     /' >&2
else
  _ok "Highest glibc requirement: GLIBC_$MAX_MAJOR.$MAX_MINOR (<= 2.35)"
fi

# ── Check 3: No bundled runtime libraries ────────────────────────────────────
echo "[check-binary] --- Check 3: No forbidden bundled libraries ---"
FORBIDDEN_LIBS="libSDL2 libopenal libmali libmpg123"
dt_needed=$(readelf --dynamic "$BINARY" 2>/dev/null | grep 'DT_NEEDED' | \
            grep -oE '\(.*\)' | tr -d '()' || true)

found_forbidden=0
for lib in $FORBIDDEN_LIBS; do
  if echo "$dt_needed" | grep -q "^${lib}"; then
    _fail "Binary has forbidden DT_NEEDED entry: $(echo "$dt_needed" | grep "^${lib}")"
    found_forbidden=1
  fi
done

if [ $found_forbidden -eq 0 ]; then
  _ok "No forbidden runtime libraries in DT_NEEDED"
  if [ -n "$dt_needed" ]; then
    echo "[check-binary]   DT_NEEDED: $(echo "$dt_needed" | tr '\n' ' ')"
  fi
fi

# ── Check 4a: Cleanliness — forbidden symbols ─────────────────────────────────
echo "[check-binary] --- Check 4a: No forbidden debug symbols ---"
FORBIDDEN_SYMBOLS="watchdog_thread heartbeat_thread input_selftest dump_framebuffer log_frame_pixels"

found_symbols=0
for sym in $FORBIDDEN_SYMBOLS; do
  # nm type T = text (code), D = data — both are non-weak defined symbols
  if nm --defined-only "$BINARY" 2>/dev/null | grep -qE "^[0-9a-f]+ [TD] ${sym}$"; then
    _fail "Binary contains forbidden debug symbol: $sym"
    found_symbols=1
  fi
done

if [ $found_symbols -eq 0 ]; then
  _ok "No forbidden debug symbols found"
fi

# ── Check 4b: Cleanliness — GLSTATE/GLDRAW strings ───────────────────────────
echo "[check-binary] --- Check 4b: GLSTATE/GLDRAW string scan ---"
# Extract all printable strings, number them, then check for GLSTATE/GLDRAW
# For each match, look ±5 lines for a _VERBOSE guard string
strings_output=$(strings "$BINARY" 2>/dev/null)
string_count=$(echo "$strings_output" | wc -l)

found_unguarded=0
while IFS= read -r line_info; do
  lineno=$(echo "$line_info" | cut -d: -f1)
  matched=$(echo "$line_info" | cut -d: -f2-)

  # Get context: lines (lineno-5) to (lineno+5)
  start=$(( lineno - 5 ))
  end=$(( lineno + 5 ))
  [ $start -lt 1 ] && start=1
  [ $end -gt "$string_count" ] && end="$string_count"

  context=$(echo "$strings_output" | sed -n "${start},${end}p")

  if echo "$context" | grep -q "_VERBOSE"; then
    : # guarded — no warning
  else
    _warn "Found unguarded '$matched' in strings (line $lineno) — consider gating behind a _VERBOSE env var"
    found_unguarded=1
  fi
done < <(echo "$strings_output" | grep -n -E 'GLSTATE|GLDRAW' || true)

if [ $found_unguarded -eq 0 ]; then
  _ok "No unguarded GLSTATE/GLDRAW strings found"
fi

# ── Final result ─────────────────────────────────────────────────────────────
echo "[check-binary] ============================================"
if [ $check_failed -ne 0 ]; then
  echo "[check-binary] RESULT: FAILED — binary does not meet quality gates" >&2
  exit 1
fi
echo "[check-binary] RESULT: PASSED — all quality gates OK"
exit 0
