#!/bin/bash
# build do bully.compat -- MESMO codigo do bully, mas linkado contra GLIBC_2.17
# (via zig cc) p/ rodar em devices de glibc ANTIGA (ArkOS/dArkOS 2.27-2.31, etc).
# O bully (build.sh) precisa GLIBC_2.38 (NextOS/muOS/Knulli/ROCKNIX/X5M). Os dois
# juntos cobrem qualquer device. AMBOS tem o TLS pad anti-stack-smashing (main.c).
#
# Requisitos:
#   - zig (https://ziglang.org) no PATH ou em $ZIG  (testado 0.16.0)
#   - o sysroot do toolchain Amlogic-old (headers SDL2/EGL/GLES)
#   - ./bully ja compilado (build.sh) -- usado SO p/ extrair a lista de simbolos
#     SDL/EGL/GL que viram stubs (o lld do zig recusa o libSDL2.so do sysroot).
set -e
cd "$(dirname "$0")"
ZIG="${ZIG:-zig}"
command -v "$ZIG" >/dev/null || { echo "zig nao encontrado (defina \$ZIG)"; exit 1; }
TC=~/NextOS-Elite-Edition/build.NextOS-Retro-Elite-Edition-Amlogic-old.aarch64-4/toolchain
SR=$TC/aarch64-libreelec-linux-gnu/sysroot
[ -f ./bully ] || { echo "compile ./bully primeiro (bash build.sh) -- preciso dos simbolos"; exit 1; }

# 1) headers isolados (so SDL2/EGL/KHR/GLES*) -> a libc vem do zig (glibc 2.17,
#    sem o redirect __isoc23_* que os headers glibc 2.43 do sysroot injetam).
HDR=$(mktemp -d)
for d in SDL2 EGL KHR GLES2 GLES3 GLES; do [ -d "$SR/usr/include/$d" ] && ln -s "$SR/usr/include/$d" "$HDR/$d"; done

# 2) stubs .so (soname = o que o device tem) p/ satisfazer o linker; em runtime
#    o device usa os SDL2/EGL/GLESv2 REAIS. Simbolos extraidos do ./bully (gcc).
STUB=$(mktemp -d)
gen() { for s in $(nm -D --undefined-only ./bully | awk '{print $NF}' | grep -E "$1" | sort -u); do echo "void $s(void){}"; done; }
gen '^SDL_'    > "$STUB/sdl.c";   gen '^egl'     > "$STUB/egl.c";   gen '^gl[A-Z]' > "$STUB/gles.c"
$ZIG cc -target aarch64-linux-gnu.2.17 -shared -fPIC -nostdlib -Wl,-soname,libSDL2-2.0.so.0 "$STUB/sdl.c"  -o "$STUB/libSDL2.so"
$ZIG cc -target aarch64-linux-gnu.2.17 -shared -fPIC -nostdlib -Wl,-soname,libEGL.so        "$STUB/egl.c"  -o "$STUB/libEGL.so"
$ZIG cc -target aarch64-linux-gnu.2.17 -shared -fPIC -nostdlib -Wl,-soname,libGLESv2.so      "$STUB/gles.c" -o "$STUB/libGLESv2.so"

# 3) build do compat (PIE, glibc 2.17)
SRCS="src/main.c src/so_util.c src/jni_shim.c src/imports.c src/egl_shim.c \
      src/asset_archive.c src/zip_fs.c src/pthread_bridge.c src/util.c src/error.c"
$ZIG cc -target aarch64-linux-gnu.2.17 -fPIE -pie \
    -I src -I "$HDR" -O2 -fPIC \
    -Wno-unused-parameter -Wno-unused-function -Wno-unused-command-line-argument \
    -Wno-incompatible-pointer-types-discards-qualifiers \
    -o bully.compat $SRCS \
    -L "$STUB" -lSDL2 -lEGL -lGLESv2 -ldl -lm -lpthread

rm -rf "$HDR" "$STUB"
echo "BUILD OK -> bully.compat: GLIBC_$(readelf -V bully.compat | grep -oE 'GLIBC_[0-9.]+' | sed 's/GLIBC_//' | sort -uV | tail -1), $(file bully.compat | grep -o 'pie executable')"
