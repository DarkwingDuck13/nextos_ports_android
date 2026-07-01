# Sonic 4 EP2 — porte AARCH64 nativo (missão grande, em andamento)

Objetivo: compilar o loader em **aarch64** usando o `libfox.so` **arm64-v8a** da versão
**3.0.0**, pra rodar nativo em devices arm64-only (TrimUI Brick/Smart Pro e afins) e
matar a firula multiarch de áudio. Autorizado 2026-07-01.

## Por que é possível (confirmado)

- O APK v2.0.0 tem **só armeabi-v7a** (32-bit). Roda em celular arm64 porque celular
  Android executa 32-bit (AArch32) — não porque o jogo seja 64-bit.
- **TrimUI Brick muOS = aarch64 PURO**: sem `/usr/lib32`, sem `ld-linux-armhf`, sem
  box64/box86. Binário 32-bit **não inicia**. (Confirmado na imagem `MustardOS_TUI-BRICK`.)
- O APK/bundle **v3.0.0** (`com.sega.sonic4episode2_3.0.0-109_2arch_...apkm`, "2arch")
  tem `split_config.arm64_v8a.apk` com **`lib/arm64-v8a/libfox.so` = ELF 64-bit aarch64**
  (NDK r21d, Android 21, stripped, 12MB). Salvo em `device_libs_3.0.0_arm64/libfox.so`.

## Estado atual

- ✅ **MARCO 1 — compila.** `build_arm64_host.sh` (toolchain `aarch64-linux-gnu-gcc` do
  host, sem Docker) gera `sonic4.arm64` (ELF64 aarch64, PIE, GLIBC≤2.27).
  - **Loader ELF64 pronto**: `so_util_arm64.c` (relocs R_AARCH64_ABS64/RELATIVE/GLOB_DAT/
    JUMP_SLOT, `hook_arm64` com pool de trampolim B±128MB, init_array, resolve por nome+dlsym).
  - **Fix de build (gcc novo):** NUNCA passar `-I/usr/include` (headers x86_64 do host
    definem `uintptr_t`=32-bit → "cast pointer→int of different size" → "initializer element
    is not constant" em TODA tabela `(uintptr_t)func`). Solução: copiar só os headers de LIB
    (SDL2/GLES/EGL/mpg123/vorbis/ogg — arch-neutros) num dir limpo e deixar `<stdint.h>` vir
    do sysroot do próprio cross-gcc (uintptr_t=64-bit). SEM `softfp_shim` (arm64 não tem
    split softfp/hardfp — floats em regs FP nativamente).

- ✅ **MARCO 2 — loader PROVADO na lib arm64 real** (smoke-test `qemu-aarch64 -L /tmp/tuiroot`
  com os libs aarch64 reais do TrimUI: SDL2 2.30/EGL/GLES/libmali). O `sonic4.arm64` roda,
  aloca o heap de 256MB e o `so_util_arm64` faz **tudo**: `so_load` (ELF64, 8 PT, PT_LOAD R-X
  @0x0 + RW- @0xae3de0), **29153 símbolos**, **0 relocs ignoradas** (todas R_AARCH64 tratadas,
  sem TLS pendente), e **os 46 construtores do `.init_array` RODARAM (concluído 46)** sem crash.
  - **Muro atual:** crash **durante os 46 construtores** — o `fprintf` de `main.c:695` (logo
    após `so_execute_init_array()`) NÃO chega a imprimir, e o backtrace tem endereços FORA da
    lib (offset ~0x1d6xxxxx num heap de 256MB). Ou seja: um construtor C++ estático spawna uma
    **thread** que crasha assíncrona (a main imprime "concluído (46)" e a thread morre). O
    registro PC=0/regs=0 é artefato do qemu-user (não popula o ucontext do guest). Isso é
    **limitação do qemu-user** com threading do jogo, NÃO bug provado do loader. → **Precisa do
    device arm64 REAL** (gdb) pra passar disso; qemu não reproduz fielmente threads/JNI/
    self-modifying-code. Runtime de teste: `/tmp/sonic_arm64_rt` (sonic4 + lib/arm64-v8a/libfox.so).
    Comando: `qemu-aarch64 -L /tmp/tuiroot -E LD_LIBRARY_PATH=... -E SONIC_DEBUG=1 ./sonic4`.

## Análise da lib arm64 v3 (o que muda vs armv7 v2)

- **Imports (374 total, 77 não-libc):** 65 GLES `gl*`, 12 Android-NDK (`AAsset*`,
  `__android_log_*` — **todos já cobertos** por imports.c/jni/android/opensles), **0 EGL,
  0 SDL, 0 OpenSLES, 0 símbolo C++ runtime** (libc++ ESTÁTICO no NDK r21). Cobertura boa.
- **Modelo de integração DIFERENTE (importante):** v3 é **JNI-driven** (`JNI_OnLoad` @0x6ea578
  existe; `android_main`/`ANativeActivity_onCreate` AUSENTES). Sem EGL direto → o contexto GL
  vem do "GLSurfaceView"/Java. O loader precisa **criar+tornar-corrente um contexto EGL** e
  **dirigir o loop de frames chamando os métodos JNI** que o jogo registra (classe FF9/Unity-JNI),
  não o modelo NativeActivity/android_native_app_glue do armv7.
- **Símbolos do main.c:** dos 64 mangled que o main.c referencia, **42 existem no v3, 22 faltam**
  (v3.0.0 refatorou): `gs::user::CUtil`/`gs::backup` (save), `SsDrawObjectShadow`/`amPostEF*`
  (shadows/post-FX), `CStartDemo` dtor (attract-demo/DEMOGUARD), `F2FExtension`. → Os hooks que
  dependem deles precisam re-RE ou virar no-op gracioso (a maioria usa `so_find_addr_safe`→0).

## Roadmap (próximas sessões — precisa do DEVICE arm64 p/ iterar)

1. **Data v3.0.0:** confirmar se o OBB/assets do v2 servem no v3 (usuário disse "data é igual").
   O bundle `.apkm` tem `split_packs.apk`/`base.apk` — checar assets embutidos vs OBB.
2. **main.c arm64:** gate/ajustar os 22 hooks de símbolo ausente (não pode `fatal_error`);
   re-RE dos 29 endereços hex hardcoded (são do v2 armv7 — todos mudam no v3 arm64) via
   `aarch64-linux-gnu-objdump -d`/capstone; começar TODOS gated OFF e ligar incremental.
3. **Bring-up do contexto:** como v3 não chama EGL, o loader cria o contexto (egl_shim) e
   faz current ANTES de chamar o método JNI de render; dirigir 1 frame → ver fb.
4. **Rodar no TrimUI/arm64 real** (não temos device físico ainda; a imagem montada
   `/tmp/tuiroot` tem os libs aarch64 reais — SDL2 2.28/EGL/GLES/libmali — p/ referência de
   runtime e p/ smoke-test sob `qemu-aarch64 -L`).

## Arquivos

- `build_arm64_host.sh` (host, rápido) e `build_arm64.sh` (Docker buster, gcc8, original).
- `src/so_util_arm64.c` = loader ELF64 (trocado no lugar de `so_util.c`).
- `device_libs_3.0.0_arm64/libfox.so` = a lib nativa arm64 (v3.0.0).
- APK/bundle v3: `~/Downloads/com.sega.sonic4episode2_3.0.0-109_2arch_..._apkmirror.com.apkm`.
