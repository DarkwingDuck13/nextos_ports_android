#!/bin/bash
# build aarch64 do Chrono Trigger (Cocos2d-x 3.14.1 so-loader)
# toolchain NextOS Amlogic-old aarch64 -> Mali-450 fbdev (libMali.so provê GLESv2/EGL).
# CI: set CC, SR, and CI_BUILD=1 env vars before invoking to override local toolchain.
set -e
TC=${NEXTOS_TC:-~/NextOS-Elite-Edition/build.NextOS-Retro-Elite-Edition-Amlogic-old.aarch64-4/toolchain}
CC=${CC:-$TC/bin/aarch64-libreelec-linux-gnu-gcc}
SR=${SR:-$TC/aarch64-libreelec-linux-gnu/sysroot}
cd "$(dirname "$0")"
[ -x "$CC" ] || { echo "toolchain não encontrado: $CC"; exit 1; }
SRCS=$(ls src/*.c)

if [ "${CI_BUILD:-0}" = "1" ]; then
    # Ubuntu cross-compiler sysroot layout:
    #   $SR/include/                    — libc, kernel headers (aarch64 sysroot)
    #   /usr/include/aarch64-linux-gnu/ — arm64 arch-specific SDL2/freetype/EGL headers
    #                                     MUST come before /usr/include to avoid picking
    #                                     up the x86_64 SDL_cpuinfo.h (which pulls immintrin.h)
    #   /usr/include/                   — generic fallback headers

    # Create stub .so files for libraries that exist on-device but not in the CI sysroot.
    # The linker only needs these to satisfy -l flags; the device provides the real ones.
    STUBDIR="$(mktemp -d)"
    trap 'rm -rf "$STUBDIR"' EXIT
    for lib in SDL2 GLESv2 EGL freetype stdc++ gcc_s; do
        stub="$STUBDIR/lib${lib}.so"
        if [ ! -f "/usr/lib/aarch64-linux-gnu/lib${lib}.so" ] && \
           [ ! -f "/usr/aarch64-linux-gnu/lib/lib${lib}.so" ]; then
            aarch64-linux-gnu-gcc-10 --sysroot="$SR" \
                -shared -nostdlib -o "$stub" /dev/null 2>/dev/null || \
            printf 'GROUP ( )\n' > "$stub"
            echo "stub: lib${lib}.so"
        fi
    done

    $CC -D_GNU_SOURCE \
        -DSDL_DISABLE_IMMINTRIN_H \
        -I src \
        -I /usr/aarch64-linux-gnu/include \
        -I /usr/include/aarch64-linux-gnu \
        -I /usr/include/aarch64-linux-gnu/SDL2 \
        -I /usr/include/aarch64-linux-gnu/freetype2 \
        -I /usr/include \
        -I /usr/include/freetype2 \
        -O2 -fPIC -fno-omit-frame-pointer -rdynamic \
        -Wno-int-conversion -Wno-incompatible-pointer-types \
        -o chrono $SRCS \
        -lSDL2 -lGLESv2 -lEGL -lfreetype -ldl -lm -lpthread -lstdc++ -lgcc_s \
        -L/usr/lib/aarch64-linux-gnu \
        -L/usr/aarch64-linux-gnu/lib \
        -L"$STUBDIR"
else
    # Local NextOS toolchain build (original)
    $CC --sysroot="$SR" -D_GNU_SOURCE -I src -I "$SR/usr/include" -I "$SR/usr/include/SDL2" \
        -I "$SR/usr/include/freetype2" \
        -O2 -fPIC -fno-omit-frame-pointer -rdynamic \
        -Wno-int-conversion -Wno-incompatible-pointer-types \
        -o chrono $SRCS \
        -lSDL2 -lGLESv2 -lEGL -lfreetype -ldl -lm -lpthread -lstdc++ -lgcc_s
fi

echo "BUILD OK -> $(file chrono | cut -d, -f1-3)"
