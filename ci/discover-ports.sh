#!/bin/bash
# ci/discover-ports.sh — parse README.md for playable ports
# Usage: ci/discover-ports.sh <readme_path> [repo_root]
#
# Outputs a JSON array of port names to stdout, e.g. ["bully","chrono",...]
# Warnings about missing directories or build.sh go to stderr.
# Exits 1 only if the resulting list is empty (malformed README).
#
# The authoritative source is the "### ✅ Concluídos — jogáveis" Markdown table.
# The "Pasta" column contains links like [`ports/bully`](ports/bully/) — we
# extract the link TARGET (the parenthesised part) and strip the "ports/" prefix.

set -euo pipefail

README="${1:?Usage: $0 <readme_path> [repo_root]}"
REPO_ROOT="${2:-$(dirname "$0")/..}"
REPO_ROOT="$(cd "$REPO_ROOT" && pwd)"

if [ ! -f "$README" ]; then
  echo "[discover-ports] ERROR: README not found: $README" >&2
  exit 1
fi

# ── Step 1: Extract the "Concluídos — jogáveis" section ─────────────────────
# Grab lines between the jogáveis header and the next ### header (or EOF).
section=$(awk '
  /###.*Conclu[ií]dos.*jog[áa]veis/ { in_section=1; next }
  in_section && /^###/ { exit }
  in_section { print }
' "$README")

if [ -z "$section" ]; then
  echo "[discover-ports] ERROR: Section '### ✅ Concluídos — jogáveis' not found in $README" >&2
  exit 1
fi

# ── Step 2: Extract port names from Pasta column link targets ────────────────
# Table rows look like:  | ... | [`ports/bully`](ports/bully/) |
# We want the path inside the last parenthesised link on each table row.
# Pattern: (ports/portname/) or (ports/portname)
raw_names=$(echo "$section" | \
  grep -oE '\(ports/[a-z0-9_-]+[/]?\)' | \
  sed 's|^(ports/||; s|[/)]||g' | \
  sort -u)

if [ -z "$raw_names" ]; then
  echo "[discover-ports] ERROR: No port entries found in playable table" >&2
  exit 1
fi

# ── Step 3: Validate each port against the repo ──────────────────────────────
valid_names=()
for name in $raw_names; do
  port_dir="$REPO_ROOT/ports/$name"
  if [ ! -d "$port_dir" ]; then
    echo "[discover-ports] WARNING: ports/$name/ directory not found — skipping" >&2
    continue
  fi
  if [ ! -f "$port_dir/build.sh" ]; then
    echo "[discover-ports] WARNING: ports/$name/build.sh not found — skipping" >&2
    continue
  fi
  valid_names+=("$name")
done

if [ ${#valid_names[@]} -eq 0 ]; then
  echo "[discover-ports] ERROR: No playable ports with both a directory and build.sh found" >&2
  exit 1
fi

# ── Step 4: Emit JSON array ───────────────────────────────────────────────────
printf '%s\n' "${valid_names[@]}" | \
  jq -R -s 'split("\n") | map(select(length > 0)) | sort'
