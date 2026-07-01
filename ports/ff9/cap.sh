#!/bin/bash
# captura as DUAS metades do fb0 do device (top=0..719, bottom=720..1439) + yoffset atual.
# uso: ./cap.sh [nome]  -> /tmp/ff9cap_<nome>_top.png e _bot.png no desktop
set -e
DEV=${FF9_DEV:-192.168.31.90}
N=${1:-now}
OUT=${FF9_CAPDIR:-/tmp}
ssh "root@$DEV" 'dd if=/dev/fb0 of=/storage/roms/ff9/_t/cap.raw bs=1M count=8 2>/dev/null; cat /sys/class/graphics/fb0/free_scale 2>/dev/null; fbset 2>/dev/null | head -4' || true
scp -q "root@$DEV:/storage/roms/ff9/_t/cap.raw" "$OUT/ff9cap.raw"
python3 - "$OUT" "$N" <<'EOF'
import sys
from PIL import Image
out, name = sys.argv[1], sys.argv[2]
d = open(f"{out}/ff9cap.raw", "rb").read()
half = 1280*720*4
for tag, off in (("top", 0), ("bot", half)):
    if len(d) >= off + half:
        img = Image.frombytes("RGBA", (1280, 720), d[off:off+half], "raw", "BGRA").convert("RGB")
        img.save(f"{out}/ff9cap_{name}_{tag}.png")
        px = img.resize((1,1)).getpixel((0,0))
        print(f"{tag}: media RGB={px}")
EOF
echo "capturas em $OUT/ff9cap_${N}_top.png / _bot.png"
