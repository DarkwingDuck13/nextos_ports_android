#!/bin/bash
# Generate the Settings menu patch zip for Textures/Light.
set -e

GAMEDIR="${1:-.}"
case "${BULLY2_TEXTURE_MENU:-${BULLY_TEXTURE_MENU:-1}}" in
  0|off|false|no) exit 0;;
esac

data_zip="$GAMEDIR/assets/data_4.zip"
patch_zip="$GAMEDIR/assets/bully2_patch.zip"
patch_script="$GAMEDIR/tools/patch-bully-menu.py"

[ -f "$data_zip" ] || exit 0
[ -f "$patch_script" ] || exit 0

pybin=""
command -v python3 >/dev/null 2>&1 && pybin=python3
[ -z "$pybin" ] && command -v python >/dev/null 2>&1 && pybin=python
[ -n "$pybin" ] || exit 0

"$pybin" "$patch_script" "$data_zip" "$patch_zip"
echo "[menu] patch ready: $patch_zip"
