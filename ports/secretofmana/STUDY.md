# Secret of Mana (remake 2018) — porte Mali-450 (so-loader)

APK base: `secret-of-mana-3.4.3-mod-menu-5play.apk` (arm64-v8a). Alvo: **.79** EMUELEC Amlogic Mali-450 fbdev.

## Engine
- **plandroid** (`jp.co.mcf.android.plandroid`, MCF/Marvelous). Java-driven: `PlAndroidActivity` + `GLSurfaceView`(`PlAndroidView`) + `Renderer` chamam `libplandroid.so` via JNI.
- **libplandroid.so** NÃO depende de `libRMS.so` (RMS = anti-tamper carregado só pelo Java; ignoramos, não rodamos stack Java).
- **GLES1.1 fixed-function** (glMatrixMode/glOrthof/glColorPointer/glEnableClientState/glVertexPointer/glTexCoordPointer + FBO OES). **Zero shader** → contexto EGL **ES1**, sem shim. Mali-450 Utgard suporta ES1.1 em HW.
- Assets via **AAssetManager** (`AAssetManager_open("res/xxx")`) → shim lê de `./assets/` (reusado do chrono).
- Sem DRM/pairip no nativo.

## Ciclo de vida (JNI `Java_..._PlAndroidLib_*`, todos native no dex)
`JNI_OnLoad(vm)` → `Construct(activity, String path, AssetManager)` → `OnStart` → `OnOpenWindow(w,h)` → `OnResumeWindow` → loop `OnFrame()->int` (≠0 = finish) + SwapWindow. Extras: `GetFrameRate`, `OnSuspend/CloseWindow`, `OnStop`, `OnSave/LoadState`, `Destruct`.
- `Renderer.onDrawFrame` (Java) faz: `updateSensorState()` (→ `sensor.updateInput()`) e então `OnFrame()`.

## Upcalls native→Java (`JNI_PlAndroidLib_*` → `PlAndroidLib.<name>`) — interceptar no jni_shim
- **Input**: `GetSensorStateFunc([I)` — engine aloca `int[37]`, Java preenche, engine lê. **É o único caminho de input.**
- **Idioma**: `GetLanguage`/`GetDeviceLanguage` → forçar inglês (enum, default 1; `SOM_LANG`). NUNCA japonês.
- **Path**: `GetObbMountedPath`→string. Versão: `GetAppVersionName/Code`.
- **Áudio (Java-side, tipo RE4)**: `SoundLibInit/SoundLoad(String)->int/SoundPlay(II)/SoundStop/SoundPause/SoundRelease/MusicLoad/MusicPlay/...` → hoje **stub silencioso**; implementar com SDL_mixer depois.
- **Fonte do sistema**: `FontWidthFunc/FontHeightFunc/FontDrawStringToImageFunc([I...String)` → métricas aproximadas + draw no-op; implementar com freetype depois.
- Diversos: `IsDeviceAndroidTV`→0, `isRetina`→0, `IsGLESBlendEquationOES`→1, memória KB → valores generosos.

## Modelo de INPUT (PlAndroidSensor) — layout do int[37] de `GetSensorStateFunc`
Ordem (índice → campo):
```
0 key_now_b   1 key_last_b  2 key_on_b   3 key_off_b
4 touch_ptr_max 5 touch_count 6 touch_last_ptr
7 touch_now_b 8 touch_last_b 9 touch_on_b 10 touch_off_b 11 touch_moving_b 12 touch_move_b
13 touch_max_x 14 touch_max_y
15..18 touch_start_x[4]  19..22 touch_start_y[4]
23..26 touch_move_x[4]   27..30 touch_move_y[4]
31..32 analog_stick_x[2] 33..34 analog_stick_y[2]
35..36 debug[2]
```
- Botões = **bitmask** em `key_now_b`. `updateInput()`: `key_on = ~key_last & key_now` (pressionado), `key_off = ~key_now & key_last` (solto). Reproduzido em `main.c:update_input`.
- `handleKey(KeyEvent)`: keycode→bit via switch; DOWN faz `key_now |= 1<<bit`, UP faz `&= ~(1<<bit)`. **Tabela keycode→bit ainda a confirmar empiricamente** (mapeamento provisório em `main.c` enum SB_*). Keycodes tratados: 4,19-31,41,42,45,47,49,51,52,54,82,96,97,99,100,102,103,107-109,188-193.
- Touch: `MAX_TOUCH_PTR=4`; motion → `touch_start/move_x/y[]`, `touch_now_b` bitmask por ponteiro.

## Estrutura do porte
so-loader em C (máquina reusada do chrono): `so_util` (carrega/reloca/resolve .so bionic), `jni_shim` (JNIEnv falso + upcalls plandroid + int[]), `imports` (libc/GL ES1/AAsset/zlib), `opensles_shim`, `main` (ciclo plandroid, contexto ES1).
- Imports extra vs chrono: 43 GL ES1/OES, zlib (`crc32/inflate*`), `setjmp/longjmp/strlcpy/__register_atfork/stderr`, `SL_IID_EFFECTSEND/PLAYBACKRATE`. `std::__ndk1` resolvem via `libc++_shared.so` (módulo auxiliar).
- Canary bionic (tpidr+0x28) tratado com pad TLS + guarda em torno de SDL_GL (igual SOTN/chrono).

## Deploy (.79)
`/storage/roms/ports/secretofmana/`: `secretofmana` (bin), `libplandroid.so`, `libc++_shared.so`, `assets/` (do APK), launcher em `ports_scripts/Secret of Mana.sh`. OBB (`com_square_enix_secret.zip` = FAT32) só se faltar dado.

## Status
- s1 (2026-07-03): estudo + scaffold + build OK. Em deploy/boot no .79. Input neutro no boot (imagem primeiro); controle/áudio/fonte = próximas fases.
