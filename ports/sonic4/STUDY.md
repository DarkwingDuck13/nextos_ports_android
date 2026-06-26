# 🦔 Sonic the Hedgehog 4: Episode II → Mali-450 (so-loader) — ESTUDO

**Veredito: 🟢 VIÁVEL (verde).** Engine GLES2 aberta + dados completos em mãos. Sem muro
arquitetural. Padrão so-loader que já dominamos (Shantae/DuckTales/SOR4/Chrono/LEGO).
**Não existe PortMaster dele** (os Sonic do PortMaster = 1/2/CD via decompilação RSDK e SRB2;
a engine NN/fox não tem decompilação) → seria inédito.

## Arquivos (temos os 2 pedaços)
- **APK** `~/Downloads/sonic-the-hedgehog-4-episode-ii-2.0.0.apk` (20MB) — versão F2F/lite
  (`com.sega.sonic4ep2lite`), só engine + ads. Engine: `lib/armeabi-v7a/**libfox.so**` (8.8MB).
- **DADOS** `~/Downloads/cache-sonic-the-hedgehog-4-episode-ii-2.0.0.zip` (433MB) → contém
  **`com.sega.sonic4episode2/main.22.com.sega.sonic4episode2.obb`** (594MB, formato **LPK**) =
  o jogo COMPLETO (episode2, não lite). Magic `LPK\0`.

## Engine
- **`libfox.so`** = middleware **NN "Ninja" da Sega** (funções `nn*`: nnSetMaterialControl…,
  nn_AndVer…) + wrapper "fox" (mineloader). **ELF32 ARM (armeabi-v7a)**, stripped, BuildID
  2cbe8b35. **GLES2 PURO** (`nnSetMaterialControlSpecularGLES20`, shaders `precision mediump
  float`, sem ES3; só 1 `glUniformMatrix3fv`). NEEDED: libGLESv2, libEGL, libandroid, liblog,
  libz, libdl, libstdc++, libm, libc. (sem OpenSLES — áudio é via Java AudioHelper).
- **Modelo GLSurfaceView** (igual LEGO Batman/Chrono): a Activity Java dirige a engine por JNI.
  NÃO usamos NativeActivity; **replicamos o driver Java no nosso main.c**.

## 🔌 JNI — entry points da engine (`com.mineloader.fox.foxJniLib`) — NÓS CHAMAMOS
Sequência típica (a partir do disasm dos stubs em 0x4bd2xx):
- `init(...)` (0x4bd254) — init mestre.
- `SetGamePath(path)` (0x4bd41c) — diz onde estão os dados (LPK/OBB). Path esperado:
  **`/mnt/sdcard/Sonic4EP2DL/`** (no port: apontar pro nosso dir; redirecionar).
- `coreGetLPKFileInfo(...)` (0x4bd4b0) — registra/abre o LPK (índice do OBB).
- `SetLanguageId(id)` (0x4bd544) — **forçar INGLÊS** (NextOS odeia japonês/idioma errado).
- `DrawEGLCreated()` (0x4bd2a8) — chamar quando a surface/contexto EGL está pronto (GL init).
- **Loop por frame:** `FileProcess()` (load streaming) + `GameProcess()` (lógica) + `DrawFrame()`
  (render; a engine desenha — modelo GLSurfaceView, nós damos o present/swap como nos outros).
- Input: `SetPadData(...)` (0x4bd2e4, joypad), `SetTPData(...)` (0x4bd2d4, touch),
  `SetSensorData(...)`, `HasController()` (0x4bd3d8 → forçar 1).
- `nativePlaySe(...)` (0x4bd5a4) — SFX.
- ciclo de vida: `pauseEvent/resumeEvent/Release/quitAndWait/IsExitBlock`.
- flags: `GetBuildTarget/GetMarketTarget/SetContinueFlag/GetTweetClearRings`.
- 🔑 **`IsLiteVersion()` (0x4bd55c) → `GsTrialIsTrial()` (0x4be8d4)** e
  **`IsTegra3Version()` (0x4bd570) → `nn_AndVerIsTegra3()`** — ver DRM/GPU abaixo.

## 🪝 JNI — callbacks que a ENGINE chama (NÓS PROVEMOS no jni_shim)
Classes: `com/mineloader/fox/{AudioHelper, APKFileHelper(+APKFile), ADHelper, VibHelper,
OpenFeintHelper, WallPaper, FoxActivity_Core, bluetoothservice}` + `com/sega/f2fextension/
f2fextensionInterface` + `net/gogame/GoGameJniAdapter`.
- 🔊 **AudioHelper** (CRÍTICO p/ som): `DmSoundPlayBGM/PlaySE/SetVolumeBGM/SetVolumeSE`,
  `GmSoundPlayBGM/PlaySE/PlayBGMJingle`, `PlaySound`, `SetVolume` → **bridge SDL/pulse**
  (decodificar ogg/wav/mp3 do LPK e tocar; igual Chrono/RE4 audio shim).
- 📦 **APKFileHelper / APKFile** (CRÍTICO p/ dados): `getAPKFile(String)→APKFile`, `read(APKFile,
  int)→int`, etc. = a engine lê o LPK/arquivos por aqui. Implementar: abrir do nosso dir/OBB.
  (alternativa: usar AAssetManager nativo — `setassetmanager` (netlib::FileUtilsAndroid) existe).
- 🚫 **ADHelper** (`isShowAds`/`IsTrialShowAdBanner`→**false**), **f2fextension**
  (`isUserRemoveAds`→**true**, `isShowAds`→false, GDPR/consent/age → valores neutros,
  `callBackIntroVideo` etc → no-op), **GoGameJniAdapter** (stub), **VibHelper/OpenFeintHelper/
  WallPaper/bluetoothservice** (stub/no-op).

## 🗂️ Dados (LPK / OBB)
- `main.22.com.sega.sonic4episode2.obb` = archive **LPK** (header `4C 50 4B 00` + tabela de
  offsets). Conteúdo: stages (`stg0`, `ACT7`, `DEMO/STGSLCT`), personagens
  (`CHR_SONIC_BODY01_DIF.DDS`, `CHR_SONIC_SPIN`), modelos **NN** (`.NN/.UV/.NNTC`, 2023 `.AMB`),
  **texturas `.DDS`(1792) + `.PVR`(298)**, **áudio `.OGG`(286)/.WAV(281)/.MP3(45)**, vídeos
  `.MP4`(65), shaders `NNGLES20SHADER/*.pb`, `.ZIP`(772 sub-arquivos), `.TXB/.LNM/.LNO/.LNV`.
- Engine espera os dados em **`/mnt/sdcard/Sonic4EP2DL/`** (SetGamePath) — no port redirecionamos
  pro nosso GAMEDIR. O LPK é lido nativamente (não precisamos desempacotar; só servir o arquivo).

## 🔐 DRM / gates (todos solúveis)
- 🔑 **Trial/Lite → FULL:** `IsLiteVersion` retorna `GsTrialIsTrial()`. **Patch `GsTrialIsTrial`
  (0x4be8d4) p/ retornar 0** (`mov r0,#0; bx lr`) → jogo completo (usa `c_create_act_table`
  cheio, não `_trial`). Confirmar tб `GsTrialIsTrial_VerTwo` (0x4be8e4) e
  `AoEp1LicenseIsEnabled`. (Os dados completos JÁ estão no OBB episode2.)
- 🎮 **GPU/textura:** `IsTegra3Version → nn_AndVerIsTegra3` = **falso** no Mali (não spoofar
  Tegra3) → engine evita o set DXT (`DM_LTEGRA3T_MAIN`, `D_LOGO_TEGRA.AMB`). Mali-450 suporta
  **ETC1** → engine deve pegar o set ETC/genérico. ⚠️CONFIRMAR no 1º boot que escolhe ETC e não
  PVRTC (PVR não roda no Mali). Se pegar PVR/DXT, forçar a detecção pro set ETC.
- 🌐 Online (gogame/puyosega.com/SSL) e ads = stub (sem rede).

## 🧱 Plano de port (scaffold)
1. Base = **framework lswtcs-src** (`docs/reference/lswtcs-src/`): so_util/egl_shim/jni_shim/
   imports/android_shim REUSADOS. main.c novo p/ o driver fox (init→SetGamePath→SetLanguageId
   (inglês)→loop{FileProcess,GameProcess,DrawFrame} + input SetPadData/SetTPData).
2. Carregar `libfox.so` (ELF32-ARM; usar so_util armv7 do Shantae/DuckTales, não o arm64).
3. EGL/GL no Mali: surface 1280x720(?) → DrawEGLCreated → DrawFrame; present igual aos outros
   (a engine pode não dar eglSwapBuffers → damos nós; **lição LEGO Batman: cuidado com
   single-buffer/tearing → usar FBCOPY se precisar**).
4. JNI shim: prover AudioHelper(bridge SDL) + APKFileHelper(ler LPK) + stubs (ads/vib/etc).
5. Patch GsTrialIsTrial→0 (full). Forçar HasController→1, inglês.
6. Input: SetPadData (mapear SDL gamepad) + SELECT+START sair. Touch via SetTPData se preciso
   nos menus.
7. Áudio: decodificar ogg/wav/mp3 → SDL/pulse.

## ⚖️ Riscos (todos táticos, nenhum muro)
- Textura: garantir set ETC1 (não PVR/DXT) no Mali-450. [médio]
- Áudio bridge (AudioHelper) + formatos ogg/wav/mp3. [médio, já fizemos]
- Amplitude do jni_shim (muitos stubs F2F) — trivial mas chato. [baixo]
- Present/tearing no fbdev Mali (usar FBCOPY da lição LEGO Batman). [baixo]
- Confirmar que o LPK episode2 casa com a libfox da APK lite (mesma versão 2.0.0/main.22). [baixo]

## Régua
Mais fácil que LEGO Batman/Elderand: **GLES2 aberto, sem pairip, sem IL2CPP, sem job-system-GL
deadlock à vista, dados completos em mãos.** Bom candidato. Device alvo provável: Mali-450 .79/.164.
Bancada Android p/ comparar fluxo: instalar o APK lite + OBB num device arm (ex. Moto G100
192.168.31.49 via adb rede) e observar o fluxo correto.

---
## ⚡ PROGRESSO s1 (2026-06-26) — SCAFFOLD BOOTA, CARREGA LPK, RODA ENGINE ATÉ O INTRO

🟢 **so-loader funciona:** so_load libfox.so (ELF32-ARM, framework Shantae), resolve JNI,
roda init → GameProcess → loop. Binário `sonic4` (build.sh, toolchain armhf). Device .79,
em `/storage/roms/ports/sonic4/` (libfox em lib/armeabi-v7a/, OBB em data/).

**Fixes que destravaram (na ordem):**
1. patch GsTrialIsTrial/_VerTwo → 0 (full). ⚠️**ARM mode, não Thumb** (detectar pelo bit do símbolo;
   patch Thumb numa func ARM = SIGSEGV).
2. 🔑 **LPK/OBB:** `SetGamePath(env,thiz, id=255, jstring path_do_OBB)` → tsSetFileRootPath(255,..)
   → **tsInitFileRootLPK → fopen(OBB)** = abre+indexa o LPK. (id<254 só guarda o path, NÃO abre LPK!)
   +`SetGamePath(0, <dir>)` p/ root de arquivos soltos. ANTES do init (init lê font.nft do LPK).
   O OBB = LPK (magic `LPK\0`), lido via fread NATIVO (tsFRead tipo 1). "Read error" no log é
   ESPÚRIO (check de flag bionic [FILE*+12]&0x40 no FILE glibc) — fread retorna certo (32 bytes OK).
3. 🔑 **threading:** FileProcess = `amFS_proc` = loop da THREAD de file-system (cond_wait quando
   ocioso) → roda em pthread DEDICADA, não no loop. Main loop = GameProcess + DrawFrame.
4. 🔑 **JNI_OnLoad:** libfox exporta `JNI_OnLoad` (0x4fe988) — chamar com nosso fake VM (o Android
   chamaria ao carregar o .so). Seta o global JavaVM que `F2FExtension::GetJNIEnv` lê (NULL deref sem isso).
5. **f2fextension setup:** chamar `nativeSetContext`/`SetJavaObj`/`nativeSetApkPath` (JNI) antes do init.

🔴 **ONDE PAROU (próximos passos):**
- Engine roda GameProcess: getBundlePath, getRegionCode, **Android_playIntroVideo**, NeConInit
  (rede, completou). **Trava esperando o intro video** (SEGA logo mp4) — callBackIntroVideo não
  destravou (achar o callback/flag certo, ou skip nativo do estado intro).
- 🔴 **`ShaderProfile Num : -1`** — engine não detectou o profile de shader do GPU → provável
  bloqueio de RENDER (tela PRETA, frames rodam mas 0 pixels). Investigar a detecção (glGetString
  RENDERER/EXTENSIONS no Mali) — pode precisar spoof do profile/extensões.
- 🟡 present/Mali fbdev: quando renderizar, aplicar lição LEGO Batman (FBCOPY/pan; fb 1280x1440).
- 🟡 stubar f2fextension/ads direito (muitos Android_* callbacks); áudio (AudioHelper bridge).
- Flags: SONIC_DATADIR, SONIC_LPK, SONIC_IOLOG (log de fread).

### s1 update — GL PIPELINE CONFIRMADO (present OK), engine renderiza PRETO
- 🔑 **egl_shim_bind_main()**: modelo GLSurfaceView = a engine NÃO cria contexto EGL próprio,
  assume o contexto já current. O Shantae egl_shim soltava o contexto (gl_makecurrent NULL) no
  fim do create_window → DrawFrame rodava SEM contexto → preto. Fix = ligar o share-root na
  thread principal após create_window.
- ✅ **Teste glClear(vermelho) após DrawFrame (SONIC_TESTCLEAR=1) → TELA VERMELHA nas 2 metades**
  = contexto GL current OK + present OK + fbdev Mali OK (NÃO precisa FBCOPY/pan; SDL double-buffer
  mostra nas 2 metades). 
- 🔴 **PRÓXIMO = por que a engine renderiza PRETO** (DrawFrame não desenha o título):
  suspeito `ShaderProfile Num : -1` (shaders NN não carregam/registram → glDrawElements sem
  program → nada). Investigar: nnRegistStdShaderProfile / de onde carrega os NNGLES20SHADER
  (LPK? loose .pb?); confirmar se o game chega no DmTitleInit (title) ou trava antes (FS thread
  idle = não enfileira o load do título?). Achar o gate que impede o título de renderizar.
