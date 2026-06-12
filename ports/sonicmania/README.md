# 🦔 Sonic Mania Plus — NextOS / Mali-450 (so-loader Android)

Port do **Sonic Mania Plus** (engine **RSDKv5 / Retro Engine**, build **Netflix** Android arm64)
rodando **nativo no Mali-450 (OpenGL ES 2.0, fbdev)** via **so-loader**. **INÉDITO MUNDIAL.**

> ⚠️ **Isto NÃO é um port PortMaster.** Não usa um build Linux/PC do jogo. É a **versão ANDROID**
> (o `libsonicmania.so` do APK) carregada por um **so-loader** (igual Bully/reVC deste repo) e
> rodando no Linux do Mali-450. O empacotamento usa o **framework do PortMaster só pra lançar**
> (control.txt + gptokeyb), mas o que roda por dentro é o binário Android. Ports PortMaster
> "de verdade" do Sonic Mania (builds Linux) são outra coisa e já existem à parte.

**Estado: 100% JOGÁVEL COM SOM.** Fluxo original completo: logos → título → menu (Mania Mode)
→ save select → escolher personagem → cutscene de abertura (Angel Island) → **Green Hill Zone
jogável**, música + efeitos, sai com **SELECT+START**.

## Game files (BYO — você fornece, do seu APK legítimo)
- `libsonicmania.so` (engine, de `lib/arm64-v8a/`) e `libc++_shared.so` → na pasta do port.
- `Data.rsdk` (assets, ~209 MB, extraído do APK) → na pasta do port.
- shaders `assets/*.frag/*.vert` adaptados GLES3→2 (#version 300→100 etc).

## Arquitetura
- **so-loader 2-módulos**: `libc++_shared.so` carregado 1º (snapshot de símbolos) + `libsonicmania.so`,
  tabela combinada (resolve o STL `__ndk1`). Entry = JNI estático (JNIEnv/JavaVM falsos).
- **Render GLES2**: o engine importa só `glUseProgram` (os shaders eram do lado Java!) →
  `gl_trace.c` cria um **programa de blit GLES2 próprio** e intercepta `glUseProgram`. vsync OFF.
- **startEngine** (16 args) + `step`=`RunGameLoop(60.0)` + `setGameRunning(1)`.

## Destraves (a parte difícil)
- **Título não avançava**: build Netflix usa `TitleSetup` custom; travava em
  `TitleSetup_State_WaitForConflictState` esperando um *cloud-save conflict* que nunca chega offline.
  **Fix**: forçar o estado da entity p/ `PressButton` + patch `UserStorage::GetCloudSaveConflictState`→`0`
  (senão o gate do PressButton bloqueia). Input chega em `controller[0]` via `OnKeyEvent`→`SetKeyEvent`
  + `onKeyboardConnected` no startup.
- **Crash STL no Menu**: `JniWrapper::GetCurrentNetflixProfileId` fazia `strlen` no retorno de
  `GetStringUTFChars`; jstrings falsos do JNI não são `char*` → devolver `""`.
- **Menu preto / spinner travado**: `MenuSetup` esperava save/options/replay/ta do storage offline.
  **Fix**: forçar `globals->saveLoaded/optionsLoaded/replayTableLoaded/taTableLoaded = STATUS_OK` +
  deixar `MenuSetup_PrerollChecks` **completar natural** (NOP nas ramificações dos gates de API mobile)
  → dispensa o spinner + ativa o menu (`selectionDisabled=false`).
- **Save de jogo novo travava** ("escolher Sonic"): build Netflix salva **só na cloud** via
  `JniWrapper::CloudSave` (async JNI que não completa offline). **Fix**: entregar os callbacks void
  pendentes via `Java_..._CallCallback` (drenados por `ExecuteCallbacks`) → o write "completa" → LoadScene
  → cutscene → fase.
- **Crash de fase (SIGILL) ao destruir badnik**: `Player_Action_Enemy`→`Stats::TryTrackStat`→
  `DummyStats::TrackStat`→`std::wstring_convert::from_bytes` crasha na libc. Stats são inúteis offline →
  patch `Stats::TryTrackStat`/`DummyStats::TrackStat`→`ret` (no-op).
- **SOM** (receita reutilizável p/ engines RSDKv5/Oboe): a callback de áudio do Oboe crasha em string STL.
  **Solução**: chamar o mixer puro `RSDK::Audio::MixToBuffer(float*,uint32)` **direto na thread de áudio
  do SDL** (`opensles_shim_set_mixfn`), convertendo float→int16 (44100 stereo) — bypassa o Oboe inteiro.
  Os canais tocavam mas saía silêncio porque o **volume do engine estava 0** (opções zeradas) →
  forçar `streamVolume`/`soundFXVolume` = 1.0.
- **Logo Netflix com cor errada** (N azul): o vídeo de abertura sobe BGRA como `GL_RGBA` na `imageTexture` →
  swap R↔B só nos uploads RGBA (`gl_trace.c`). A tela do jogo é RGB, não afetada.

## Lançar
- Launcher: `ports_scripts/Sonic Mania.sh` (usa o framework PortMaster **só pra lançar**: control.txt +
  get_controls + `$GPTOKEYB "sonicmania"` + `sonicmania.gptk` mapeando o controle → SELECT+START sai).
  Aparece em **Ports** no EmulationStation.
- Por SSH: `systemctl stop emustation; bash "/storage/roms/ports_scripts/Sonic Mania.sh"` (foreground).

## Flags de ambiente (debug)
- `SONIC_NOAUDIO` — desliga o som.
- `SONIC_ZONE=N` — carrega uma cena direto (ex: 14 = Green Hill Zone 1) + auto-RIGHT (demo).
- `SONIC_SKIPCUT` — pula a cutscene Angel Island → Green Hill.
- `SONIC_MENUFIX` — (experimental, não testado) força o realce do botão selecionado no menu OPTIONS
  (hook em `UIButton_Draw` forçando `buttonBounceOffset` pulsante).
- `SONIC_AUTONEW` / `SONIC_FORCEAPI` — auxiliares de debug do fluxo de menu.

## Crédito
Engine RSDKv5 open-source (decomp Rubberduckycooly) usado só como referência de RE.
Framework so-loader derivado dos ports de **mtojek** (Apache-2.0). **BYO game files.**
