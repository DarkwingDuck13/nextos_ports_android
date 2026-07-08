#!/bin/bash
# build_knulli.sh — Cross-compile the SOTN so-loader for Knulli (RK3566/aarch64)
# from a plain x86_64 Linux host. No Knulli-specific toolchain/sysroot needed.
#
# WHY THIS WORKS WITHOUT A REAL DEVICE TOOLCHAIN:
#  - We only need SDL2/EGL/GLES *headers* to compile against. These are plain
#    C declarations - identical content regardless of host architecture - so
#    any distro's dev packages work fine even though we're targeting aarch64.
#  - At LINK time we build tiny no-op "stub" .so files carrying the correct
#    SONAME (libSDL2-2.0.so.0 / libGLESv2.so / libGLESv1_CM.so) purely so the
#    linker has something to resolve symbols against. We never ship these.
#  - At RUNTIME on the actual device, ld.so re-resolves those SONAMEs against
#    Knulli's REAL SDL2 and REAL Mali GLESv2 libraries (via LD_LIBRARY_PATH,
#    set in "Castlevania SOTN.sh") instead of our stubs. Standard trick, same
#    one build.sh/build_compat.sh already use.
#  - Building inside debian:bullseye keeps glibc symbol versions old
#    (~2.31), so the single binary runs on Knulli's newer glibc without
#    needing to match versions exactly.
#
# CAVEAT: unlike build_compat.sh (which `nm`s an already-built native ./sotn
# to know exactly which gl*/SDL_* symbols need stubbing), this script derives
# that list by grepping the C source directly, since you don't have a native
# binary yet. It's a reasonable approximation but may pick up a stray token
# (e.g. a type name like SDL_GLContext) or, more importantly, MISS something
# if a symbol is referenced in a way grep doesn't catch. If the final link
# step fails with "undefined reference", paste the error back - it just means
# one symbol needs adding to the STUB lists by hand.
#
# Usage (run from ports/sotn/):
#   docker run --rm -v "$PWD":/repo debian:bullseye bash /repo/build_knulli.sh
#
set -e
CC=aarch64-linux-gnu-gcc
REPO=/repo
cd "$REPO"

if ! command -v "$CC" >/dev/null; then
  export DEBIAN_FRONTEND=noninteractive
  apt-get update -qq
  apt-get install -y -qq \
    gcc-aarch64-linux-gnu binutils-aarch64-linux-gnu \
    libsdl2-dev libgles2-mesa-dev libegl1-mesa-dev >/dev/null
fi
echo "CC: $($CC --version | head -1)"

[ -d src ] || { echo "run this from ports/sotn/ (needs ./src)"; exit 1; }

# 1) Headers, pulled straight from the host's dev packages we just installed.
HDR=$(mktemp -d)
for d in SDL2 EGL KHR GLES2 GLES3 GLES; do
  [ -d "/usr/include/$d" ] && cp -r "/usr/include/$d" "$HDR/$d"
done

# 2) Stub .so's — symbol lists derived from source (no native binary needed).
STUB=$(mktemp -d)
grep -rhoE '\bgl[A-Z][A-Za-z0-9_]*\b'  src/*.c src/*.h | sort -u \
  | while read -r s; do echo "void $s(void){}"; done > "$STUB/gles2.c"
grep -rhoE '\bSDL_[A-Za-z0-9_]*\b'     src/*.c src/*.h | sort -u \
  | while read -r s; do echo "void $s(void){}"; done > "$STUB/sdl.c"
sort -u -o "$STUB/gles2.c" "$STUB/gles2.c"
sort -u -o "$STUB/sdl.c"   "$STUB/sdl.c"

$CC -shared -fPIC -nostdlib -Wl,-soname,libGLESv2.so     "$STUB/gles2.c" -o "$STUB/libGLESv2.so"
$CC -shared -fPIC -nostdlib -Wl,-soname,libGLESv1_CM.so  "$STUB/gles2.c" -o "$STUB/libGLESv1_CM.so"
$CC -shared -fPIC -nostdlib -Wl,-soname,libSDL2-2.0.so.0 "$STUB/sdl.c"   -o "$STUB/libSDL2.so"

# 3) Build the loader itself.
SRCS=$(ls src/*.c)
$CC --sysroot=/ -D_GNU_SOURCE -I src -I "$HDR" \
    -O2 -fPIC -fno-omit-frame-pointer -rdynamic \
    -Wno-int-conversion -Wno-incompatible-pointer-types \
    -Wno-unused-parameter -Wno-unused-function \
    -o sotn $SRCS \
    -L "$STUB" -lSDL2 -lGLESv2 -lGLESv1_CM -ldl -lm -lpthread

rm -rf "$HDR" "$STUB"
echo "BUILD OK -> sotn"
echo "  GLIBC max: $(aarch64-linux-gnu-readelf -V sotn | grep -oE 'GLIBC_[0-9.]+' | sed 's/GLIBC_//' | sort -uV | tail -1)"
echo "  tamanho:   $(stat -c%s sotn) bytes"
echo "  tipo:      $(file sotn | cut -d, -f1-3)"
