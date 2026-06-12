# Dusklight (Zelda: Twilight Princess / engine Aurora) → Mali-450 so-loader

> Início: 2026-06-09. Port estilo Sonic Mania / reVC (so-loader do .so Android).
> APK: `Dusklight-v1.2.0-1-full-apkvision.apk` (1.5GB; data1=1.46GB; arm64).

## O QUE É
- **Twilight Princess** (GameCube) no **engine Aurora** (decomp zeldaret/tp). Strings: `daNpc_Zelda_c`,
  `e_hzelda_class`, "faron twilight / midna cs", `AuroraConfig`, `ARAM`, `AdapterPropertiesWGPU`.
- Mesmo engine do fork `felc18-blip/aurora` (mali450-gles2). Este APK é um **build Android pré-feito**.

## RECON (libmain.so, 45MB, arm64-v8a)
| Aspecto | Achado |
|---|---|
| **Render** | **GLES2** ✅ — ZERO imports GLES3 (sem VAO/UBO/instancing/TexStorage), 57 funcs GLES2, shaders `precision mediump float`. NEEDED libGLESv2.so + libGLESv1_CM.so. (strings "Vulkan" = só tabelas do Naga, dead code) |
| **Janela/SDL** | **SDL3 ESTÁTICO** dentro do libmain (1860 símbolos SDL_ definidos, 0 importados). `SDL_HasMainCallbacks`/`SDL_GetGamepadType` = SDL3. Backend = **ANDROID** (`Android_CreateWindow`, `Android_AddJoystick`, "Android activity unavailable") |
| **Entry** | `SDL_main` + `SDL_SetMainReady` (NÃO android_main) — fluxo SDL3 |
| **Controle** | SDL3 gamecontroller + gamecontrollerdb.txt completo ✅ |
| **Áudio** | libOpenSLES → opensles_shim |
| **Imports** | 525 UND: 277 auto (libc/libm/gles/pthread/zlib) + 248 "UNKNOWN" que são **quase todos libc/libm/zlib padrão** (acos/atoi/clock_gettime/compress2/dlopen...) → resolver via dlsym(RTLD_DEFAULT). Hard real ~40 (dl*, bionic-específicos, android) |

## 🔑 O NÓ CENTRAL (a batalha do projeto)
O **SDL3 é ESTÁTICO e é o backend ANDROID**. Diferente do reVC (que usa o SDL2 Linux do device),
aqui o engine chama o SDL3 Android interno (cria janela via **ANativeWindow**, input/áudio/lifecycle
via JNI/SDLActivity). No device (fbdev Linux) isso não existe.

**Caminho (igual RE4 fez "ANativeWindow shims"):** fazer o backend Android do SDL3 funcionar provendo:
1. **ANativeWindow** falso que embrulha o native window do **Mali fbdev** (a EGL do device aceita).
   O SDL3-Android faz `eglGetDisplay`/`eglCreateWindowSurface(ANativeWindow)` → cai na EGL Mali real.
2. **JNI falso** (igual Sonic): SDLActivity callbacks, ANativeActivity, AInputQueue/ALooper p/ eventos.
3. **AAssetManager** → ler `assets/data1` (1.46GB) do disco (redirect fopen).
- Alternativa: interpor no nível EGL (capturar eglCreateWindowSurface e dar a surface do device,
  insight do Bully cross-thread EGL — reusável aqui).

## ENTRY (adaptar do reVC, NÃO android_main)
```
SDL_SetMainReady();            // dlsym RTLD_DEFAULT OU o estático
so_find_addr("SDL_main")(argc,argv);
```
Antes: JNI_OnLoad + setar fake JNI + ANativeWindow/Android shims prontos.

## IMPORTS — estratégia
- 248 "UNKNOWN" do new-port.sh são conservadores: rotear libc/libm/zlib/socket/dl pro **glibc real**
  via fallback `dlsym(RTLD_DEFAULT)` no resolver (linkar -lm -lz -ldl). Sobra ~40 hard (dl_iterate_phdr,
  __ctype_*, android_set_abort_message, __android_log_write→stderr, bionic-específicos).

## PLANO (fases)
- **F0** esqueleto compilável: entry SDL_main + resolver c/ fallback RTLD_DEFAULT. ← COMEÇAR
- **F1** init: JNI_OnLoad roda, SDL_Init passa (prover Android shims até SDL_CreateWindow).
- **F2** janela/GL: ANativeWindow→Mali EGL, contexto GLES2 criado (insight Bully p/ cross-thread).
- **F3** assets: AAssetManager/fopen redirect p/ data1; engine carrega cena.
- **F4** render + controle + áudio.

## RESSALVAS
- 1.5GB num device de 1GB RAM → swap (já montado p/ RE4). TP mobile pode caber, conferir.
- APK do apkvision: tem `libAPKVISION.so` (wrapper/ads do repacker) — ignorar/limpar.
- Multi-sessão (engine grande). Device prioritário = Amlogic-old (Mali-450 fbdev).

## ARQUIVOS
- ports/dusklight/ (scaffold do new-port.sh): src/main.c (adaptar entry), imports.gen.c (resolver),
  jni_shim.c, egl_shim.c, android_shim.c, opensles_shim.c, so_util.c.
- Recon temp: /tmp/dl-recon/lib/arm64-v8a/libmain.so.

## ESTADO DO BUILD (2026-06-09, fim sessão — F0 ~90%)
Esqueleto compila: entry adaptado p/ **SDL_main** (não android_main), resolver com **fallback
dlsym(RTLD_DEFAULT)** (so_util.c) → libc/libm/zlib/socket resolvem no glibc real. imports.gen.c
ganhou bloco de includes + stubs android_log. SO_NAME=libmain.so. build.sh compila todos src/*.c.
**FALTA p/ compilar (próximo passo, mecânico):**
- `gl*` (57 funcs GLES2): incluir `<GLES2/gl2.h>`+link `-lGLESv2`, OU tirar do table e
  dlopen libGLESv2 RTLD_GLOBAL no startup (fallback RTLD_DEFAULT resolve) — preferir o 2º (igual reVC).
- `__cxa_atexit/__cxa_finalize/__cxa_thread_atexit_impl`: extern decl (existem no glibc).
- `__errno` (bionic): stub `int*__errno(void){return __errno_location();}`.
Depois: build → deploy libmain.so+data1 no device → 1ª run (ver até onde o init SDL3 vai antes
de precisar dos shims ANativeWindow/AInputQueue da F1/F2).

## F0 ✅ COMPILA + 1ª RUN (sessão 2, 2026-06-09)
**BUILD OK**: loader `dusklight` (ELF aarch64 169KB) compila. Fixes: resolver fallback dlsym(RTLD_DEFAULT)
em so_util.c; entry SDL_main; pthread_fake.c (gtavc) + 22 funcs gap (rwlock/once/key/sem-extras);
includes+stubs em imports.gen.c; dynlib_numfunctions; **NÃO linkar libs do device** (libz/libSDL2 têm
seção corrompida no toolchain ld → resolver em runtime via dlopen RTLD_GLOBAL + fallback); excluído
android_shim/egl_shim do build (Dusklight=SDL_main+SDL3 estático, não NativeActivity); sdl_audio_stub.c
(6 funcs SDL audio no-op + 5 SL_IID dummy, áudio=F4); dlopen libz/libGLESv2 RTLD_GLOBAL no main.
**1ª RUN**: loader carrega libmain, resolve ~478/525 imports (fallback), **CRASHA no init_array**
(construtores C++ estáticos) por causa dos **47 shims bionic/Android faltando** (UNRESOLVED-F1.txt).
**ROADMAP F1 (imediato)** = prover os 47:
- **bionic _chk** (FORTIFY): __strlen_chk→strlen, __strchr_chk→strchr, __strncpy_chk2→strncpy,
  __write_chk→write, __sendto_chk→sendto, __FD_SET_chk/__FD_ISSET_chk, __memcpy_chk etc. (mapear p/ glibc unchecked).
- **__sF** (bionic stdio FILE[3] = stdin/out/err) → array apontando p/ glibc stdin/stdout/stderr.
- **__system_property_get** → stub (return 0). **android_set_abort_message** → wirar stub no table (já existe).
- **ZSTD_trace_*** → stub no-op (tracing opcional).
- **F2** (depois): ANativeWindow_* (4) → janela Mali fbdev. **F3**: AAsset* (10) → fopen(data1).
  ASensor*/ALooper* (sensores/looper) → stub/ALooper mínimo.
Device: /storage/roms/dusklight-recon/ (libmain.so+dusklight). Run: SDL_VIDEODRIVER=mali
LD_LIBRARY_PATH=/usr/lib32:/usr/lib ./dusklight. Assets data1 (1.46GB) ainda NÃO deployados.

## F1✅→F2 (sessão 2 cont.): init+JNI+SDL_main RODANDO! muro=SDL3 Android backend (env cache)
3 FIXES GRANDES destravaram do init até SDL_main (commit 10ceb07+seguintes):
1. **so_load copiava só 2 PT_LOAD** mas libmain tem 9 PHs → `.init_array` num segmento NÃO-copiado
   (slots zerados → init_array TODO = base+0 → SIGILL chamando text_base+0). FIX: copiar TODOS PT_LOAD.
2. **RELR** (`.relr.dyn` ANDROID_RELR): clang-21 põe TODAS as relocs RELATIVE no RELR (0 RELATIVE em
   .rela.dyn!). so_relocate não tratava → vtables/func-ptrs não-relocados. FIX: add handling RELR
   (formato: entry par=endereço+aplica; ímpar=bitmap dos 63 words; *where+=base). so_util.c.
3. **jni_shim_get_vm retornava tabela ZERADA** → JNI_OnLoad crashava em vm->GetEnv (idx6=NULL).
   FIX: jni_shim_init (VM real: vm_GetEnv/Attach + JNIEnv completo). main.c chama JNI_OnLoad c/ ela.
+ bionic_shims.c (_chk→glibc, __sF buffer, __system_property_get, ZSTD no-op), android_ndk_shims.c
  (ASensor/ALooper/ANativeWindow/AAsset stubs), -rdynamic (fallback acha shims), setvbuf unbuffered.
**AGORA RODA**: init_array(171 ok) → JNI_OnLoad registra classes SDL (SDLActivity/SDLAudioManager/
SDLControllerManager/HIDDeviceManager) → JNI_OnLoad=0x10004 → SDL_SetMainReady → **SDL_main chamado**.
**MURO ATUAL (F2):** crash em `SDL_GetAndroidInternalStoragePath` (via dusk::data::initialize_data →
SDL_SYS_GetPrefPath). Faz `Android_JNI_GetEnv()` (env CACHEADO do JNI_OnLoad, NÃO re-chama GetEnv) →
`ldr x8,[x0]`(=*env)=**0x1** → `ldr x8,[x8,#152]`→ fault 0x99. Ou o env cacheado (TLS/global SDL) está
errado, OU jni_env_ptr (0x...d08, global loader) foi sobrescrito p/ 0x1 entre JNI_OnLoad e SDL_main
(SDL tem ptr p/ ele via env=&jni_env_ptr). PROX: (a) print jni_env_ptr antes/depois de SDL_main p/ ver
se corrompe; (b) como SDL3 Android_JNI_GetEnv cacheia o env (pthread TLS? checar pthread_*specific_fake);
(c) prover storage path sem JNI (stub SDL_GetAndroidInternalStoragePath via hook) — é o save dir.
Depois: F2 janela (ANativeWindow→Mali EGL real), F3 assets (AAsset→fopen data1 1.46GB). DUSK_NOSKIP=1
mostra crash real. Device /storage/roms/dusklight-recon/ (libmain.so+dusklight; data1 ainda não).
