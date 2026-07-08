#!/bin/bash
# ci/assemble-zip.sh — assemble a PortMaster-compatible zip for a port
# Usage: ci/assemble-zip.sh <portname> <binary_path> <output_dir> [repo_root]
#
# Produces:  <output_dir>/<port.json:.name>   (e.g. dist/chrono.zip)
#
# Zip layout (PortMaster standard):
#   Chrono Trigger.sh          ← top-level launch script (Title Case, from repo)
#   chrono/
#     chrono.aarch64           ← compiled binary, arch-suffixed
#     port.json                ← patched: arch=["aarch64"], items updated
#     README.md
#     screenshot.png
#     cover.png                (if present)
#     gameinfo.xml             (if present)
#     chrono.gptk              (if present)
#     alsoft.conf              (if present)
#     licenses/                (if present)
#
# The .items array in port.json is rebuilt to exactly match the zip manifest,
# so harbourmaster can track the port correctly.

set -euo pipefail

PORTNAME="${1:?Usage: $0 <portname> <binary_path> <output_dir> [repo_root]}"
BINARY_PATH="${2:?Usage: $0 <portname> <binary_path> <output_dir> [repo_root]}"
OUTPUT_DIR="${3:?Usage: $0 <portname> <binary_path> <output_dir> [repo_root]}"
REPO_ROOT="${4:-$(dirname "$0")/..}"
REPO_ROOT="$(cd "$REPO_ROOT" && pwd)"

PORT_DIR="$REPO_ROOT/ports/$PORTNAME"
PORT_JSON="$PORT_DIR/port.json"

# ── Preconditions ─────────────────────────────────────────────────────────────
if [ ! -f "$PORT_JSON" ]; then
  echo "[assemble-zip] ERROR: port.json not found: $PORT_JSON" >&2
  exit 1
fi

if [ ! -f "$BINARY_PATH" ]; then
  echo "[assemble-zip] ERROR: binary not found: $BINARY_PATH" >&2
  exit 1
fi

if ! command -v jq >/dev/null 2>&1; then
  echo "[assemble-zip] ERROR: jq is required but not installed" >&2
  exit 1
fi

if ! command -v zip >/dev/null 2>&1; then
  echo "[assemble-zip] ERROR: zip is required but not installed" >&2
  exit 1
fi

# ── Read zip name from port.json ──────────────────────────────────────────────
ZIP_NAME=$(jq -r '.name' "$PORT_JSON")
if [ -z "$ZIP_NAME" ] || [ "$ZIP_NAME" = "null" ]; then
  echo "[assemble-zip] ERROR: .name field missing or null in $PORT_JSON" >&2
  exit 1
fi

BINARY_DEST_NAME="${PORTNAME}.aarch64"

echo "[assemble-zip] ============================================"
echo "[assemble-zip] Port:    $PORTNAME"
echo "[assemble-zip] Binary:  $BINARY_PATH -> $PORTNAME/$BINARY_DEST_NAME"
echo "[assemble-zip] Zip:     $OUTPUT_DIR/$ZIP_NAME"
echo "[assemble-zip] ============================================"

# ── Set up staging area ───────────────────────────────────────────────────────
STAGING="$OUTPUT_DIR/staging"
STAGING_PORT="$STAGING/$PORTNAME"
rm -rf "$STAGING"
mkdir -p "$STAGING_PORT"

# ── Copy mandatory files ──────────────────────────────────────────────────────
echo "[assemble-zip] Copying mandatory files..."

# Launch script(s): go at top level of the zip (not inside portname/)
# We only want the PortMaster launcher (Title Case name like "Chrono Trigger.sh"),
# NOT build.sh or other utility scripts. Heuristic: exclude build*.sh, and any
# lowercase-starting script that isn't the launcher.
shcount=0
for sh_file in "$PORT_DIR"/*.sh; do
  [ -f "$sh_file" ] || continue
  bname="$(basename "$sh_file")"
  # Skip build*.sh and other lowercase utility scripts
  case "$bname" in
    build*.sh|build_*.sh) continue ;;
  esac
  cp "$sh_file" "$STAGING/"
  echo "[assemble-zip]   + $bname (launch script)"
  shcount=$((shcount + 1))
done
if [ $shcount -eq 0 ]; then
  echo "[assemble-zip] ERROR: No .sh launch script found in $PORT_DIR" >&2
  exit 1
fi

# port.json, README.md, screenshot.png
for mandatory in port.json README.md screenshot.png; do
  src="$PORT_DIR/$mandatory"
  if [ ! -f "$src" ]; then
    echo "[assemble-zip] ERROR: Mandatory file missing: $src" >&2
    exit 1
  fi
  cp "$src" "$STAGING_PORT/$mandatory"
  echo "[assemble-zip]   + $PORTNAME/$mandatory"
done

# Binary: renamed to <portname>.aarch64
cp "$BINARY_PATH" "$STAGING_PORT/$BINARY_DEST_NAME"
echo "[assemble-zip]   + $PORTNAME/$BINARY_DEST_NAME (binary)"

# ── Copy optional files ───────────────────────────────────────────────────────
echo "[assemble-zip] Copying optional files..."

for optional in cover.png gameinfo.xml alsoft.conf; do
  src="$PORT_DIR/$optional"
  if [ -f "$src" ]; then
    cp "$src" "$STAGING_PORT/$optional"
    echo "[assemble-zip]   + $PORTNAME/$optional"
  fi
done

# .gptk files
for gptk in "$PORT_DIR"/*.gptk; do
  [ -f "$gptk" ] || continue
  cp "$gptk" "$STAGING_PORT/$(basename "$gptk")"
  echo "[assemble-zip]   + $PORTNAME/$(basename "$gptk")"
done

# settings.ini (some ports ship one)
if [ -f "$PORT_DIR/settings.ini" ]; then
  cp "$PORT_DIR/settings.ini" "$STAGING_PORT/settings.ini"
  echo "[assemble-zip]   + $PORTNAME/settings.ini"
fi

# licenses/ directory
if [ -d "$PORT_DIR/licenses" ]; then
  cp -r "$PORT_DIR/licenses" "$STAGING_PORT/licenses"
  echo "[assemble-zip]   + $PORTNAME/licenses/"
fi

# ── Patch port.json ───────────────────────────────────────────────────────────
echo "[assemble-zip] Patching port.json..."

STAGED_JSON="$STAGING_PORT/port.json"

# Compute what items will actually be in the zip, in sorted order
# Top-level items (launch scripts)
toplevel_items=()
for sh_file in "$STAGING"/*.sh; do
  [ -f "$sh_file" ] || continue
  toplevel_items+=("$(basename "$sh_file")")
done

# Port-subfolder items (everything inside portname/)
subfolder_items=()
while IFS= read -r rel; do
  subfolder_items+=("$PORTNAME/$rel")
done < <(find "$STAGING_PORT" -type f | sed "s|$STAGING_PORT/||" | sort)

# Build the combined items array as JSON
all_items_json=$(
  printf '%s\n' "${toplevel_items[@]}" "${subfolder_items[@]}" | \
  jq -R -s 'split("\n") | map(select(length > 0)) | sort'
)

# Patch the staged port.json:
#   - set .attr.arch = ["aarch64"]
#   - replace .items with our computed manifest
jq \
  --argjson items "$all_items_json" \
  '.items = $items | .attr.arch = ["aarch64"]' \
  "$STAGED_JSON" > "${STAGED_JSON}.tmp"
mv "${STAGED_JSON}.tmp" "$STAGED_JSON"

echo "[assemble-zip] port.json patched: arch=[\"aarch64\"], items updated"

# ── Validate manifest vs port.json items ──────────────────────────────────────
echo "[assemble-zip] Validating manifest..."

# Recompute actual file list from staging
actual_manifest=$(
  find "$STAGING" -type f | sed "s|$STAGING/||" | sort
)

# Read items from the patched port.json (sorted)
json_items=$(jq -r '.items[]' "$STAGED_JSON" | sort)

# Compare
if diff_output=$(diff <(echo "$json_items") <(echo "$actual_manifest") 2>&1); then
  echo "[assemble-zip] Manifest validation passed"
else
  echo "[assemble-zip] ERROR: port.json items do not match actual zip contents" >&2
  echo "[assemble-zip]   diff (expected from port.json vs actual files):" >&2
  echo "$diff_output" | sed 's/^/[assemble-zip]   /' >&2
  rm -rf "$STAGING"
  exit 1
fi

# ── Assemble zip ──────────────────────────────────────────────────────────────
mkdir -p "$OUTPUT_DIR"
ZIP_PATH="$OUTPUT_DIR/$ZIP_NAME"

echo "[assemble-zip] Creating $ZIP_PATH ..."
(cd "$STAGING" && zip -r "$ZIP_PATH" .)

echo "[assemble-zip] Zip size: $(du -sh "$ZIP_PATH" | cut -f1)"

# ── Cleanup staging ───────────────────────────────────────────────────────────
rm -rf "$STAGING"

echo "[assemble-zip] Done: $ZIP_PATH"
