# Ferramentas GL / windowing

## sdl2-compat (libSDL2 sobre SDL3-mali do device)
Cross-build aarch64:
  git clone https://github.com/libsdl-org/sdl2-compat (ou tarball main)
  cmake -G Ninja -DCMAKE_C_COMPILER=aarch64-linux-gnu-gcc -DCMAKE_SYSTEM_NAME=Linux -DCMAKE_BUILD_TYPE=Release
  ninja   # ignora falha dos testes; gera libSDL2-2.0.so.0.3200.xx (só linka libc, dlopen SDL3)

## glprobe.c — sonda do GL entregue pelo stack
  aarch64-linux-gnu-gcc glprobe.c -o glprobe <path>/libSDL2-2.0.so.0.3200.xx
No device (frontend PARADO p/ liberar fb0):
  systemctl stop emustation; pkill -9 emulationstatio
  LD_LIBRARY_PATH=<libs>:/usr/lib SDL_VIDEODRIVER=mali ./glprobe
Resultado esperado: contexto GLES2 do Mali (sem ARB/EXT_framebuffer_object → DesktopGL rejeita;
por isso vamos de MonoGame GLES).
