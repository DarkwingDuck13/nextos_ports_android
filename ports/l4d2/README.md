# L4D2 (com.jam.l4d2) — fan-game Left 4 Dead 2, Unity Mono ARM32 → Mali-450 (so-loader)

> **Status: 🟡 SCAFFOLD COMPILA (l4d2boot armhf). Base = infra do RE4 (Unity Mono ARM32).**
> Falta: extrair dados do APK p/ device + primeiro run + iterar bring-up do Mono.

## Recon do APK (`~/Downloads/L4D2FG.apk`, 184MB)
- **Engine:** Unity **2020.3.41f1**, backend **Mono** (`libmonobdwgc-2.0.so`), **armv7-only (32-bit)**.
- Libs: `libunity.so` (engine), `libmain.so` (JNI bootstrap), `libmonobdwgc-2.0.so` (Mono+Boehm GC), `libMonoPosixHelper.so`.
- **ES2 CONFIRMADO (não é muro!):** `data.unity3d` tem **16× `#version 100` (shaders ES2)** + 5× `#version 300 es`. Diferente do NieR (zero ES2) → roda no Mali-450.
- Dados: `assets/bin/Data/` (Managed/*.dll = Assembly-CSharp + BCL Mono; data.unity3d 71MB). **Sem OBB.** ~213MB descomprimido (cabe).
- Sem vídeos (.mp4/.usm/.bk2) — os "vídeos" eram problema do RE4, não deste.

## Por que RE4 de base (mesma família)
RE4 = Unity 2018 Mono **ARM32** tb → mesma toolchain **armhf** (`armv8a-emuelec-linux-gnueabihf-gcc`) + mesmo bring-up de Mono. Reusado (game-agnóstico): `so_util`, `jni_shim`, `opensles_shim` (áudio FMOD/OpenSLES), `android_shim`, `egl_shim`, `pthread_shim`, `softfp_shim`, `imports.gen`, `jni_idx_stubs`. O bring-up difícil do Mono (Boehm GC `GC_page_size`/sysconf, `jit_tls` attach, OOM) já vem resolvido na infra. NÃO herdamos os bugs do RE4 (deadlock job-system era do Unity 2018; câmera/vídeos eram do RE4) — L4D2 é Unity 2020.3, jogo novo.

## Adaptações feitas (main_l4d2.c, base = main_re4.c)
- package default `com.WS.RE4` → **`com.jam.l4d2`** (env PKG sobrepõe).
- mono lib `libmono.so` → **`libmonobdwgc-2.0.so`**.
- `SO_NAME` = `libunity.so` (entry, mantido). Hooks `RE4_*` ficam gated OFF (inócuos) — limpar depois.

## Build
```bash
bash build.sh   # -> l4d2boot (ELF 32-bit ARM armhf)
```
Toolchain = NextOS Amlogic-old (armhf cross). SDL2/GLESv2/EGL = runtime no device.

## PRÓXIMOS PASSOS
1. **Deploy dados:** extrair do APK p/ `/storage/roms/ports/l4d2/`: as 4 `.so` (armeabi-v7a) + `assets/` inteiro (Data/Managed + data.unity3d). Renomear/symlink mono se o loader esperar nome específico.
2. **Primeiro run** (device .164, armhf): `l4d2boot` → ver até onde o bring-up do Mono vai (deve reusar os fixes do RE4).
3. Iterar: render (ES2, deve achar shaders), input (gptokeyb/SDL GameController), áudio (opensles_shim).
4. Limpar os hooks `RE4_*` específicos do main_l4d2.c.

Device .164 = root/emuelec, Mali-450 fbdev, suporta exec ARM32 (RE4 rodou). RE4 de referência: `../re4`.
