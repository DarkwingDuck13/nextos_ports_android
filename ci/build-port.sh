#!/bin/bash
# ci/build-port.sh — invoke a port's build.sh in the correct environment
# Usage: CC=<path> SR=<path> ci/build-port.sh <portname> [repo_root]
#
# Requires:
#   CC  — absolute path to aarch64-linux-gnu-gcc-10 (or compatible cross-compiler)
#   SR  — absolute path to aarch64 sysroot directory
#
# The script cd's into ports/<portname>/ before invoking build.sh, exactly
# matching the working directory the local toolchain builds expect.

set -euo pipefail

PORTNAME="${1:?Usage: CC=<path> SR=<path> $0 <portname> [repo_root]}"
REPO_ROOT="${2:-$(dirname "$0")/..}"
REPO_ROOT="$(cd "$REPO_ROOT" && pwd)"

PORT_DIR="$REPO_ROOT/ports/$PORTNAME"
BUILD_SCRIPT="$PORT_DIR/build.sh"

# ── Precondition checks ───────────────────────────────────────────────────────
if [ -z "${CC:-}" ]; then
  echo "[build-port] ERROR: CC environment variable is not set" >&2
  exit 1
fi

if [ -z "${SR:-}" ]; then
  echo "[build-port] ERROR: SR environment variable is not set" >&2
  exit 1
fi

if [ ! -f "$BUILD_SCRIPT" ]; then
  echo "[build-port] ERROR: build.sh not found for port '$PORTNAME'" >&2
  echo "[build-port]   Expected: $BUILD_SCRIPT" >&2
  exit 1
fi

# ── Log build context (for reproducibility audit) ────────────────────────────
# Resolve CC to a full path so that [ -x "$CC" ] checks in port build.sh work.
# If CC is a bare command name (e.g. "aarch64-linux-gnu-gcc-10"), resolve it.
if [ -n "${CC:-}" ] && [ ! -x "$CC" ]; then
  CC_RESOLVED=$(command -v "$CC" 2>/dev/null || true)
  if [ -n "$CC_RESOLVED" ]; then
    CC="$CC_RESOLVED"
  else
    echo "[build-port] ERROR: CC='$CC' is not executable and not found in PATH" >&2
    exit 1
  fi
fi

echo "[build-port] ============================================"
echo "[build-port] Port:       $PORTNAME"
echo "[build-port] CC:         $CC"
echo "[build-port] SR:         $SR"
echo "[build-port] Git commit: $(git -C "$REPO_ROOT" rev-parse HEAD 2>/dev/null || echo 'unknown')"
echo "[build-port] Compiler:   $("$CC" --version 2>&1 | head -1 || echo 'version unknown')"
echo "[build-port] ============================================"

# ── Invoke the port's build.sh ────────────────────────────────────────────────
export CC SR

cd "$PORT_DIR"
echo "[build-port] Running: bash build.sh  (cwd: $PORT_DIR)"
bash build.sh
EXIT_CODE=$?

if [ $EXIT_CODE -ne 0 ]; then
  echo "[build-port] ERROR: build.sh exited with code $EXIT_CODE for port '$PORTNAME'" >&2
  exit $EXIT_CODE
fi

echo "[build-port] Build succeeded for port '$PORTNAME'"
