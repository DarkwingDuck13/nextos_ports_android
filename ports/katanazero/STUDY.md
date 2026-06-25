# 🗡️ Katana ZERO — Estudo de Port (Mali-450 so-loader)

APK: `~/Downloads/Katana-ZERO-v1.1.10-unlocked-apkvision.apk` (348 MB)
Package: **`com.netflix.NGP.KatanaZero`** (edição **Netflix Games**)
Data do estudo: 2026-06-25

## TL;DR — VIÁVEL, VITÓRIA RÁPIDA 🟢
Engine = **GameMaker Studio 2 (YYC, YoYo Compiler)**, **GLES2 nativo**, **OpenAL→OpenSLES**, **arm64**.
Wrapper = **Netflix NGP** — o **MESMO** do nosso **Sonic Mania Netflix (JOGÁVEL)**.
➡️ **Base = fork do port `sonicmania`** (so-loader arm64 + JNI falso + egl_shim ES2 + opensles_shim +
gl_trace BGRA-swap do logo Netflix + launcher gptokeyb). Só troca o **driver de engine**
(`sonic_jni.c` RSDKv5 → `katana_jni.c` GameMaker) e os **arquivos de dados**.

## Engine (libyoyo.so)
- `lib/arm64-v8a/libyoyo.so` (76 MB) + `lib/armeabi-v7a/libyoyo.so` (80 MB). **Usar arm64** (igual FF9/Sonic).
- NDK r27b, target Android 26, stripped, 29.869 símbolos FUNC.
- **YYC**: `game.droid` NÃO tem chunk `CODE`/`VARI`/`FUNC` → toda a lógica GML está compilada em ARM
  nativo DENTRO da `.so` (por isso 76 MB). NÃO é VM/bytecode → **não dá pra usar runner genérico**,
  tem que so-loadar ESTA `libyoyo.so`.
- **Render = GLES2 PURO** ✅: shaders `gl_FragColor`/`texture2D`/`attribute`/`varying`/`precision mediump
  float` (GLSL ES 1.0). **ZERO marcador ES3** (`#version 300`, `in/out vec`, `layout()`). **Sem shim de
  shader** — diferente de Elderand/Bully. Importa `libGLESv2.so` dinâmico.
- **Áudio = OpenAL próprio do YoYo** (`yyalSourcePlay`/`OpenALBufferData`/`FreeALBuffer`) saindo por
  **`libOpenSLES.so`** ("Using OpenSL ES: %d") ou `android.media.AudioTrack`. → nosso **`opensles_shim.c`
  serve direto** (fake libOpenSLES, o mixer yyal alimenta a callback do SDL → pulse). Mesma receita
  RE4/Chrono.

## Entrypoint = JNI-driven (NÃO NativeActivity)
Bridge `com.yoyogames.runner.RunnerJNILib` — o Java (`RunnerActivity`) cria GLSurfaceView e chama via JNI:
- `Java_..._RunnerJNILib_Startup` (4572 B) — init grande
- `Java_..._RunnerJNILib_Process` (1128 B) — **loop por-frame** (chamar a cada vsync)
- `Java_..._RunnerJNILib_KeyEvent`, `_MouseButtonEvent`, `_onGPKeyDown`, `_onGPDeviceAdded`,
  `_registerGamepadConnected`, `_Pause`, `_BackKeyLongPressEvent` — input/lifecycle
- `_dsMapCreate/_dsMapAddInt/_jCreateDsMap/_CreateAsynEventWithDSMap` — sistema de eventos async
→ Modelo IDÊNTICO ao Sonic (`startEngine`/`step`/`OnKeyEvent`) e RE4/Terraria. Driver novo
  `katana_jni.c` resolve esses símbolos com `so_find_addr_safe` e dirige o loop.

## DRM / Netflix
- `com.netflix.NGP.KatanaZero` + Netflix SDK 1.3.0 (Broxcorp/Juju Adams, 2024-09-26) — **MESMA família**
  do Sonic Mania Netflix. O gate de login/cloud vive na **camada Java/dex** → no so-loader a gente
  **NÃO roda o dex**, dirige a `libyoyo` direto → bypass natural.
- APK "unlocked-apkvision" (tem `libstub.so` + `apkvision.config` injetados; ignoramos — carregamos
  `libyoyo` direto, não o stub).
- Possíveis gates nativos a stubar (reusar receita Sonic): cloud-save async que não completa offline,
  `GetCurrentNetflixProfileId` (devolver `""`), Netflix profile id, leaderboards/stats
  (`ms_iap_QueryGameLicense`, `g_LicensedTargets` — extensões GMS, no-op).

## Dados (assets/assets/, ~170 MB)
- `game.droid` (72 MB) — FORM chunks: GEN8/OPTN/SPRT/BGND/ROOM/OBJT/TPAG/TXTR(10 MB)/AUDO(54 MB)/
  STRG/FONT/SHDR... (sem CODE = YYC). **Renomear? NÃO** — o runner abre `game.droid` por nome.
- `audiogroup1..4.dat` (32 MB) — bancos de áudio extra.
- `song_*.ogg` (~80 MB) — músicas streamed (lidas direto do filesystem).
- `options.ini`, `buffer_sfx.ogg`, `splash.png`, `portrait_splash.png`, `*.jwe` (credenciais — stub).
- **Inglês**: GameMaker tem chunk `LANG` + `STRG`; garantir locale en (regra #5). Checar na 1ª run.

## PLANO (fork do sonicmania)
1. `cp -r ports/sonicmania ports/katanazero`; trocar nomes dos símbolos JNI
   `com.netflix.NGP.SonicMania.*` → `com.netflix.NGP.KatanaZero.*` e `RunnerJNILib_*`.
2. Reescrever `sonic_jni.c` → `katana_jni.c`: resolver `RunnerJNILib_Startup`/`Process`, montar
   `android_app`/`JNIEnv` falsos, loop = `Process()` por frame. Input via `KeyEvent`/`MouseButtonEvent`/
   gamepad JNI.
3. Dados: deployar `game.droid` + `audiogroup*.dat` + `song_*.ogg` + `options.ini` na pasta do port;
   apontar o working dir / AAssetManager fake pra eles (reusar `android_shim.c`).
4. Áudio: `opensles_shim.c` como está; conferir que `yyal*` resolve a fake libOpenSLES.
5. Netflix offline: stubar profile/cloud-save (reusar padrões do `sonic_jni.c`).
6. gl_trace BGRA-swap (logo Netflix) já está; conferir.
7. Launcher gptokeyb (`ports_scripts/Katana ZERO.sh`) clonado do Sonic. SELECT+START sai.

## Riscos / muros potenciais
- 🟢 Render: ES2 nativo → sem shim. Baixo risco.
- 🟡 Áudio: yyal→OpenSLES; se a fake libOpenSLES não casar, hookar `yyalBufferData`/mixer direto (igual
  receita `MixToBuffer` do Sonic). Médio.
- 🟡 Fluxo de boot Netflix: gates de cloud/profile podem travar título/menu (foi o muro do Sonic, já
  resolvido lá — replicar). Médio.
- 🟢 YYC arm64: roda lógica nativa, sem job-system Unity (sem o deadlock do RE4/Elderand). Baixo.
- 🟢 RAM: jogo pixel-art 2D, leve. Sem problema no Mali-450.

## Infra reusável do sonicmania (já existe, 5475 linhas)
`so_util.c`(ELF64 arm64) · `egl_shim.c`(ctx ES2 fbdev) · `opensles_shim.c`(áudio) · `jni_shim.c`+
`android_shim.c`+`jni_log.c`(JNIEnv/Android falsos) · `gl_trace.c`(BGRA swap Netflix) ·
`pthread_bridge.c` · `imports.gen.c` · `shims.c` · `main.c`. **Só `sonic_jni.c`→`katana_jni.c` é novo.**

## Régua
GameMaker 2D ES2 = engine ABERTA com caminho ES2 nativo → NUNCA muro de shader (igual régua do backlog).
Comparável a Sonic Mania (RSDKv5) em dificuldade, com o BÔNUS de já termos a base Netflix pronta.
