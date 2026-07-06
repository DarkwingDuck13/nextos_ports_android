#!/bin/bash
set -e

TC=~/NextOS-Elite-Edition/build.NextOS-Retro-Elite-Edition-Amlogic-old.aarch64-4/toolchain
CC=$TC/bin/aarch64-libreelec-linux-gnu-gcc
SR=$TC/aarch64-libreelec-linux-gnu/sysroot

cd "$(dirname "$0")"

[ -x "$CC" ] || { echo "toolchain nao encontrado: $CC"; exit 1; }

SRCS="src/android_shim.c src/egl_shim.c src/error.c src/imports.c src/jni_shim.c \
      src/main.c src/opensles_shim.c src/so_util.c src/util.c"

STUB=$(mktemp -d)
for s in SDL_CreateWindow SDL_DestroyWindow SDL_GetDesktopDisplayMode SDL_GetError \
         SDL_GL_CreateContext SDL_GL_DeleteContext SDL_GL_GetProcAddress \
         SDL_GL_MakeCurrent SDL_GL_SetAttribute SDL_GL_SetSwapInterval \
         SDL_GL_SwapWindow SDL_Init SDL_Quit SDL_NumJoysticks SDL_IsGameController \
         SDL_GameControllerOpen SDL_GameControllerName SDL_GameControllerClose \
         SDL_GameControllerGetAxis SDL_PollEvent SDL_Delay SDL_OpenAudioDevice \
         SDL_CloseAudioDevice SDL_LockAudioDevice SDL_UnlockAudioDevice \
         SDL_PauseAudioDevice SDL_strlcpy; do echo "void $s(void){}"; done > "$STUB/sdl.c"
$CC -shared -fPIC -nostdlib -Wl,-soname,libSDL2-2.0.so.0 "$STUB/sdl.c" -o "$STUB/libSDL2.so"

$CC --sysroot="$SR" -D_GNU_SOURCE -DPORT_WINDOW_TITLE=\"LIMBO\" \
    -I src -I "$SR/usr/include" -I "$SR/usr/include/SDL2" \
    -O2 -fPIC -fno-omit-frame-pointer -rdynamic \
    -Wno-int-conversion -Wno-incompatible-pointer-types \
    -Wno-unused-function -Wno-unused-parameter \
    -o limbo $SRCS \
    -L "$STUB" -lSDL2 -lGLESv2 -lEGL -ldl -lm -lpthread -lstdc++ -lgcc_s

rm -rf "$STUB"

echo "BUILD OK -> $(file limbo | cut -d, -f1-3)"
