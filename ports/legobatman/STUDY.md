# LEGO Batman 3: Beyond Gotham — Estudo de Port (so-loader → Mali-450)

**APK:** `LEGO-Batman-Beyond-Gotham-v2.2.1.05-full-apkvision.apk` (~1.6 GB, repack apkvision)
**Pacote:** `com.wb.goog.lbbg`  ·  Activity: `com.wb.goog.lbbg.GameActivity`
**Engine nativa:** `libLEGO_Black_Mobile.so` — engine **"Fusion"** da WB Games (mesma de LEGO Batman 3 / Jurassic World / Marvel mobile)
**Device alvo:** `192.168.31.79` — EmuELEC NextOS, Mali-450 Amlogic, kernel 3.14.79, 852 MB RAM, GLES2/fbdev
**Status:** ESTUDO COMPLETO — scaffold criado, escrevendo `main.c` da sequência Fusion.

---

## 1. Veredito de viabilidade: VERDE 🟢

- **GLES2 puro.** NEEDED = `libGLESv2 libEGL libOpenSLES libandroid libc/m/dl liblog` (mesmo conjunto do LSW). **Sem libGLESv3.** Shaders são `precision mediump float` sem `#version` → ES2. Único uso de extensão é `GL_OES_EGL_image_external` (só para FMV/`nativeUpdateMovieInfo`, tratar depois).
- **Import surface trivial:** 237 undefined = libc + pthread + OpenSLES + `AAssetManager_*`. **100% coberto** pela infra do loader LSW (so_util/android_shim/opensles_shim/imports).
- **Sem DRM nativo.** `libstub.so` + `assets/apkvision.config` = só o wrapper de repack do apkvision (spoof de assinatura Java). Irrelevante para so-loader (pulamos a JVM inteira).
- **JNI resolvida por NOME** (símbolos `Java_*` exportados como `T`). Sem `JNI_OnLoad`/`RegisterNatives` → resolvemos por `so_find_addr`, igual ao LSW.

## 2. Arquitetura da engine — DIFERENTE do LSW

LSW (`libTTapp.so`) = engine TT/Nu, classe `com.tt.tech.TTActivity`.
Batman (`libLEGO_Black_Mobile.so`) = engine **Fusion**, duas classes JNI:

### `com.wbgames.LEGOgame.Fusion` (Activity / IO glue) — 22 natives
```
addAPKEntry                    nativeSetCommandLine
addAssetsDirs                  nativeSetSavePath
nativeInitializeAssetManager   nativeSetWritePath
nativeSetCachePath             nativeSetDeviceStrings
nativeSetConsumedItems         nativeSetGameCircleEnabled / Ready
nativeSetAlertDialogResponse   nativeSetAudioOutputBufferSize
nativeControllerSetData        nativeBackButtonPressed
nativeTouchEvent{Down,Up,Move,GestureStart,GestureEnd,CancelAll}
```

### `com.wbgames.LEGOgame.GameGLSurfaceView` (ciclo GL / render) — 10 natives
```
nativeColdBoot   nativeInit     nativeResize    nativeRender
nativeResume     nativePause    nativeStart     nativeStop
nativeDone       nativeWindowFocusChanged       nativeUpdateMovieInfo
```
Padrão clássico `GLSurfaceView.Renderer`:
- `onSurfaceCreated` → `nativeInit` / `nativeColdBoot`
- `onSurfaceChanged` → `nativeResize(w,h)`
- `onDrawFrame` → `nativeRender()` (chamado todo frame — É O LOOP)

## 3. Assinaturas confirmadas (via aarch64-linux-gnu-objdump)

- `nativeInitializeAssetManager(env, thiz, jobject assetMgr)` → `AAssetManager_fromJava(env, assetMgr)`, guarda global. → passamos fake jobject; `AAssetManager_fromJava_fake` devolve nosso fake mgr.
- `nativeSetAudioOutputBufferSize(env, thiz, int n)` → `fnaSound_SetAudioOutputBufferSize(n)`.
- (demais a desmontar uma a uma — ver TODO §6)

## 4. Dados do jogo
- `assets/data01` (788 MB) + `assets/data02` (799 MB) = arquivos packed da Fusion (lidos via `AAssetManager_open("data01"/"data02")` + `addAPKEntry`).
- `assets/loaded_screen.png`, `assets/obb_info.xml` (versão 22103), `assets/apkvision.config` (ignorar).
- `levels/levels/%s/%s_main.lvl` etc. = caminhos internos servidos pelo asset manager.
- Idioma: força `en_US` (string `languages_us`, `GetCurrentLanguageCountryCode`). **Regra: sempre inglês.**

## 5. Plano de port (reuso do framework LSW)

Reusados sem mudança: `so_util.{c,h}`, `egl_shim.*`, `opensles_shim.*`, `jni_shim.*`, `imports.*`, `android_shim.*`, `util/error`.
**Novo:** `main.c` dirigindo a sequência Fusion (substitui a TTActivity do LSW).

Sequência de boot prevista (a confirmar por disasm):
1. `so_load` + `so_relocate` + `so_resolve` + init_array (igual LSW).
2. SDL + `egl_shim_create_window` (1280x720, ES2).
3. `jni_shim_init`.
4. Fusion setup: `nativeInitializeAssetManager(fakeMgr)` → `addAPKEntry(apkPath)` → `addAssetsDirs(...)` → `nativeSetSavePath/WritePath/CachePath` → `nativeSetCommandLine` → `nativeSetDeviceStrings` → `nativeSetAudioOutputBufferSize`.
5. GL: `GameGLSurfaceView_nativeInit` / `nativeColdBoot` → `nativeResize(w,h)` → `nativeResume`/`nativeStart` → `nativeWindowFocusChanged(1)`.
6. Loop: a cada frame `nativeRender()` + bombear input (gamepad→`nativeControllerSetData`/keycodes) + `opensles_shim_pump_callbacks` + `SDL_GL_SwapWindow`.

## 6. TODO imediato
- [ ] Desmontar todas as 32 natives p/ contagem/tipo de args (helper `/tmp/dis.sh`).
- [ ] Descobrir como `addAPKEntry`/`addAssetsDirs` esperam os caminhos (apk inteiro? dir de assets? data01/02 soltos).
- [ ] `nativeSetDeviceStrings` / `nativeSetCommandLine` — quais strings (manufacturer/model/cmdline) a engine exige.
- [ ] Patches de stack-canary (a engine Fusion pode ter os mesmos falsos-positivos `__stack_chk_fail` do LSW → reusar `patch_all_stack_chk_branches`).
- [ ] Input: mapear gamepad p/ `nativeControllerSetData` (provável struct de estado) e/ou keycodes Android.

## 7. Build & deploy
- Toolchain: `~/NextOS-Elite-Edition/build.NextOS-Retro-Elite-Edition-Amlogic-old.aarch64-4/toolchain` (`aarch64-libreelec-linux-gnu-gcc`).
- Build: `ports/legobatman/build.sh` (modelo crazytaxi: `-lSDL2 -lGLESv2 -lEGL -ldl -lm -lpthread -lstdc++`).
- Deploy: `sshpass -p '' root@192.168.31.79`; launcher PortMaster em `ports_scripts/` E `ports/` (regra EmuELEC). Assets gigantes → `/storage/roms` (vfat).
- 🚨 Regras de device: matar+confirmar jogo anterior antes de lançar; `nohup bash Launcher.sh` (nunca setsid/sh); não forçar SDL_VIDEODRIVER/AUDIODRIVER; sempre inglês.
