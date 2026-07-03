# Mega Man 1 Mobile → Mali-450 so-loader — HANDOFF / diário vivo

> ÂNCORA DE MEMÓRIA. Atualizar a CADA iteração. Se o contexto resetar, reler isto e continuar exatamente daqui.

## 🚨 REGRAS DO NEXTOS (este port)
1. **Resolver TUDO, sem atalhos.** Manter o máximo do original. Atalho só se NÃO tiver outro jeito.
2. **Entregar IMAGEM + CONTROLES + ÁUDIO + INGLÊS funcionando.** Começar e **NÃO PARAR** até a imagem.
3. Globais: só master, sem co-autor/menção a Agente. Matar+confirmar 0 instâncias do jogo (por /proc/*/exe) ANTES de lançar. Nunca forçar SDL_VIDEODRIVER/SDL_AUDIODRIVER. Jamais japonês. glibc ≤2.30 no release (Docker buster).

## ALVO / ACESSO
- Device: **192.168.31.114** = **NextOS-Retro-Elite-Edition 4.8.2** (EmuELEC), Amlogic **kernel 3.14.79 aarch64**, **Mali-450 Utgard** (`/dev/mali`, `d00c0000.mali`). 832MB RAM (~444 livre), **swap FIXO 512MB** (/storage/.cache/swapfile — NÃO tocar, regra #9).
- SSH: `ssh root@192.168.31.114` (chave; host key mudou 2026-07-03, foi limpa). Deploy em `/storage/roms/ports/megaman1`, launcher em `/storage/roms/ports_scripts/`.
- APK: `~/Downloads/MEGAMAN-1.apk` (40MB), extraído em `~/Downloads/mm1_extract/`.

## ENGINE / RECON (2026-07-03, estudo de viabilidade — VIÁVEL, rota conhecida)
- **Cocos2d-x 3.9** (mesma família do Chrono Trigger 3.14.1 — port VERDE de referência).
- Lib única: **`lib/armeabi/libcocos2dcpp.so`** (8.4MB). **ELF32 ARM EABI5**, CPU_arch v7, FP VFPv3, **softfp calling convention** (args float em registradores core → precisa softfp shim, igual Shantae/RE4).
- **GLES2 puro** (shaders `#ifdef GL_ES`, `precision mediump float`) → casa 100% com Utgard/Mali-450.
- **STL = GNU STL (gnustl)**: NEEDED `libstdc++.so`; símbolos `std::` no namespace NORMAL (0× `__ndk1`, 0× `__cxx11`) → resolvem contra o **libstdc++ do host** (NÃO precisa módulo libc++_shared separado, ao contrário do chrono). 334 símbolos UND (libc/GL/AAsset/std).
- **DRM: NENHUM** (sem pairip/Denuvo/LVL). Strings "license" = Cricket Audio middleware, não proteção.
- **Áudio = Cricket Audio** (`assets/sound/*.cks` = 40MB, o grosso; entry `AppActivity_initCricket`). Provável saída via OpenSL ES (NEEDED libOpenSLES.so).

### Entry points JNI (família Cocos2d-x clássica, = chrono):
- `JNI_OnLoad` (registra natives via RegisterNatives)
- `cocos_android_app_init(JNIEnv*)` — registra AppDelegate
- `Java_org_cocos2dx_lib_Cocos2dxRenderer_nativeInit(env,thiz,w,h)` — init (GLView + app_init + Application::run)
- `Java_org_cocos2dx_lib_Cocos2dxRenderer_nativeRender(env,thiz)` — 1 frame (Director::mainLoop)
- `nativeTouchesBegin/End/Move/Cancel`, **`nativeKeyEvent(env,thiz,keyCode,pressed)`** ← input principal
- `Cocos2dxHelper_nativeSetContext`, `nativeSetApkPath`
- `Cocos2dxBitmap_nativeInitBitmapDC` — texto (igual chrono; ver text_render se UI branca)
- `Cocos2dxActivity_getGLContextAttrs`
- `AppActivity_initCricket` — áudio Cricket
- (NÃO exporta funções de Controller nativo — input é via nativeKeyEvent + VirtualPad touch)

### Controles (SIM, no binário):
- `APPLET::GetMaskCode(cocos2d::EventKeyboard::KeyCode)` — mapeia TECLA → bitmask de input (smoking gun).
- `fine_lib::Lib_Input`/`Lib_MultiInput` — lib de input própria processando `EventKeyboard`.
- `cocos2d::EventListenerController` — suporte a gamepad nativo (plano B).
- `VirtualPad::updateGamePad(int)` — pad virtual on-screen (touch).
- `ui::Widget::enableDpadNavigation` — nav de menu por D-pad.
- ⇒ **Estratégia: gptokeyb mapeia controle → teclado → nativeKeyEvent.** Descobrir o mapa exato de KeyCode via GetMaskCode quando rodar.

### Assets (41MB, tudo em assets/, sem OBB):
- `sound/*.cks` (40MB, Cricket Audio), `pad/` (rockman_ui.ssbp SpriteStudio + tex rockman_1.png), `gz/` (720K, chips/menu png+dat), `capcom_logo.png`, `title.png`. Servir CRUS via AAsset shim → `./assets/`.

## ARQUITETURA DO LOADER (base montada)
- **Toolchain armhf**: `~/NextOS-Elite-Edition/build.NextOS-Retro-Elite-Edition-Amlogic-old.aarch64-4/toolchain/bin/armv8a-emuelec-linux-gnueabihf-gcc` (sysroot `.../armv8a-emuelec-linux-gnueabihf/sysroot`). MESMO toolchain de Shantae/RE4/Terraria/DuckTales.
- Device tem **32-bit completo**: `/usr/lib32/ld-linux-armhf.so.3` + `/usr/lib32/libMali.{gondul,dvalin}.so` (symlinks libEGL/libGLESv2→libMali), SDL2 32-bit (checar libSDL2-2.0.so.0).
- **Base de código**: shims armhf do **Shantae** (`so_util.c` ELF32, `softfp_shim.c`, `egl_shim.c`, `android_shim.c`, `stdio_shim.c`, `pthread_bridge.c`, `imports.c`, `jni_shim.c`, `opensles_shim.c`) + **`main_megaman.c` NOVO** com a lógica de entry Cocos portada do chrono `main.c` (JNI_OnLoad → nativeSetApkPath/Context → nativeInit(w,h) → loop nativeRender+SwapWindow; input SDL→nativeKeyEvent).
- Diferenças vs chrono: 32-bit (não arm64 → sem truque TLS tpidr, sem hook_arm64; usar patch_thumb/hook arm32 do shantae); módulo ÚNICO (std:: resolve no host libstdc++, sem libc++_shared); input via nativeKeyEvent (não GameControllerAdapter).

## STATUS / LOG
- **s0 (2026-07-03)**: recon+viabilidade COMPLETOS. Scaffold criado + main_megaman.c (entry Cocos armhf) + build.sh.
- **s1 (2026-07-03) 🏆 MENU PRINCIPAL RENDERIZANDO EM INGLÊS (IMAGEM ✅)**: "MEGAMAN MOBILE" logo + menu GAME/OPTION + D-pad virtual + copyright Capcom, 0 crash, vivo indefinido. Device .114 (IP muda via DHCP — varrer subnet). Fixes (todos em src/, sem atalho):
  1. **jni_shim do chrono** (cocos-aware) no lugar do shantae (dispatch de getCurrentLanguage/writablePath/package). +jni_shim_make_array p/ linkar android_shim.
  2. **GetJavaVM (vtable JNIEnv idx 219)**: Cricket faz `env->GetJavaVM(&vm)` no init; sem isso vm=NULL → SystemAndroid chama vm->GetEnv(this=NULL) → deref nulo. RAIZ do 1º crash (achado via gdb: `ldr r1,[r0]` r0=0 em `_JavaVM::GetEnv+0x13`).
  3. **Cricket debug logging** (Logger/TextWriter::writef) crasha em glibc vsnprintf → neutralizado (patch_thumb_ret). NÃO stubar DebugWriter::fail (é noreturn, callers têm `udf` depois → SIGILL).
  4. **Audio::getNativeSampleRate**: lê sample rate global=0 (JNI não provê AudioTrack) → fail+udf. PATCH: retorna 44100 (movw r0,#44100;bx lr).
  5. **GraphOutputJavaAndroid::renderBuffer** (saída via Java AudioTrack): AudioTrackProxy::write()→JNI→0≠esperado → assert/udf. Neutralizado (stub) → SILENCIOSO mas sem crash. **TODO ÁUDIO: rotear p/ OpenSL/SDL.**
  - Método decisivo: **gdb no device** (`handle SIGILL/SIGSEGV stop nopass; run; bt`) dá o PC real — o crash-handler stack-scan é NÃO-confiável (rotula dados do heap como código; PC reportado 0x7c720=raise interno é enganoso).
- **s2 (2026-07-03) 🏆 GAMEPLAY ALCANÇADO (Mega Man na fase do CUTMAN)**: fluxo completo por TOUCH: menu→modo(NORMAL)→save(START)→**stage select (CUTMAN/GUTSMAN/ELECMAN/ICEMAN/FIREMAN/BOMBMAN)**→gameplay. Fase NES renderiza perfeita, INGLÊS, 0 crash.
  - **INPUT — descobertas**: (1) o MENU é TOUCH (nativeTouchesBegin), NÃO teclado. (2) `nativeTouchesBegin/End(int,float,float)` recebem FLOAT → precisam ABI **softfp** (`pcs("aapcs")`) senão coords viram lixo (FIX aplicado). (3) Teclado (Lib_Input/EventKeyboard) NÃO dirige o menu. Decodifiquei `APPLET::GetMaskCode` (tabela em vaddr 0x7d455c): cocos KeyCode LEFT=26/RIGHT=27/UP=28/DOWN=29 (mask 0x1000/2000/4000/8000), SPACE=59(0x10000), ação=124-130. mm_send_cocos_key() despacha EventKeyboard direto (dispatcher=*(Director+0x98)) — mas menu ignora; USAR TOUCH.
  - **Layout touch (1280x720)**: dpad virtual centro~(95,500) [L(35,505) R(155,505) U(95,445) D(95,560)]; confirmar✓/pulo~(1175,620); tiro~(1085,475); arma~(1175,310); voltar~(1175,455).
  - **SFX bloqueava gameplay**: confirmar toca SFX→`Lib_SoundCkManager::playSe`→`Cki::Sound::newBankSound(bank=NULL)`→crash. Banks Cricket NÃO carregam (asset via JNI Java AssetManager, nosso fake não provê; 0 chamadas ao nosso AAssetManager_open). Stub playSe/se_play → desbloqueia (silencioso).
- **s3 (2026-07-03) CONTROLE gameplay — INVESTIGAÇÃO (menu OK, gameplay GATED)**:
  - ✅ **Menu por touch 100%**: gamepad→multitouch nos controles virtuais navega menu→modo→save→stage-select→gameplay. Implementado (main_megaman.c): dpad(id0, hold/move), pulo A=(1175,620), tiro X=(1085,475), arma Y=(1175,310), voltar B=(1175,455), pause Start=(1210,55). Keyboard tb (setas/z/x/c).
  - ⚠️ **GAMEPLAY não responde a input** (Mega Man não anda/pula/atira). Verificado: (a) touch CHEGA — botão de pulo ACENDE (Lib_TouchButton detecta); (b) keyboard dispatch VÁLIDO (director=ok, dispatcher=*(Director+0x98)=ok); (c) `Lib_Input::onKeyPressed` só grava 1 keyCode em [this+0x54]+flag[+0x58] (input secundário single-key); (d) `VirtualPad::updateGamePad` posiciona `Lib_TouchButton` circles. MAS `APPLET::GameMainCtrl`/`GT_MANAGER::{Caller,HitCheck,ActChange}` não traduz em ação.
  - 🔑 **HIPÓTESE FORTE: gate ligado ao ÁUDIO stubado** — a transição de estado (teleporte-in / stage start) pode esperar callback de som que neutralizei (playSe/renderBuffer). Conecta áudio↔controle. **Testar: consertar banks Cricket primeiro.**
  - LEADS controle: `APPLET::SetCtrlActKC`/`KeyConfigEnd` (config de teclas data-driven, pode estar não-inicializada); injeção DIRETA na máscara de input do GameMainCtrl/player (achar o global lido por GT_MANAGER::ActChange); GetMaskCode masks LEFT=0x1000 RIGHT=0x2000 UP=0x4000 DOWN=0x8000 SPACE=0x10000.
- **s4 (2026-07-03) ÁUDIO banks RESOLVIDO + controle gameplay LOCALIZADO**:
  - ✅ **Cricket banks carregam via CkCustomFile handler** (setFileHandler): Cricket pede 'sound/se.ckb'→servimos assets/sound/se.ckb via fopen. Sem crash. playSe des-stubado. (FALTA output real: `renderBuffer` ainda stubado=silencioso; rotear p/ SDL.)
  - ❌ HIPÓTESE áudio-gate ERRADA: banks carregam mas controle gameplay ainda não responde. NÃO é áudio.
  - 🔬 **CONTROLE GAMEPLAY — diagnóstico preciso (É INTERATIVO!)**: o botão PAUSE/weapon (topo-dir ~1210,55) FUNCIONA em gameplay (abre menu de armas M.BUSTER — diff 77M). MAS dpad/pulo/tiro (`Lib_TouchButton::isTouch(Lib_MultiInput*)`) NÃO registram meus toques injetados (nem posição certa medida: jump(1173,620) shoot(1085,465) dpad-centro(90,500); testei tap/hold/move-contínuo/ids diferentes). 
  - 🔑 **RAIZ provável**: os botões de gameplay usam `fine_lib::Lib_MultiInput` (multitouch) alimentado por `Lib_Input::onTouchBegan`+`initSingleTouch`; esse listener não recebe nossos `nativeTouchesBegin` (enquanto os botões de UI/menu recebem). Investigar: (a) Lib_Input::onTouchBegan/initSingleTouch — como registra o listener e se está ativo no gameplay; (b) `updateGamePad` só roda em Loading/KeyConfig, não gameplay; (c) talvez precise dispatchar o EventTouch por outro caminho (all-at-once vs one-by-one) ou popular Lib_MultiInput direto; (d) alternativa: achar a máscara de input do player (GT_MANAGER) e escrever direto.
- **✅ LAUNCHER ES criado** (`payload/Mega Man 1.sh`, deployado em ports_scripts + ports; LD_LIBRARY_PATH=/usr/lib32, gamepad→touch no loader, gptokeyb p/ sair).
- **s5 (2026-07-03) 🏆🏆 CONTROLES + ÁUDIO FUNCIONANDO (usuário confirmou na TV)**:
  - **🎮 CONTROLES 100%**: descoberto o fluxo de input do gameplay: `VirtualPad::update()` (chamada por APPLET::update) escreve o bitmask em **`GlobalDataManager+4/+8`**, o Mega Man lê dali. **Hook arm32** (`hook_thumb_call`: prefixo com bl relocado via `ldr ip;blx ip` p/ alcance ∞, trampolim Thumb) em VirtualPad::update: chama a original + OR nosso `g_pad_mask`. **Bits (=GetMaskCode): LEFT=0x1000 RIGHT=0x2000 UP=0x4000 DOWN=0x8000 JUMP=0x800**. Dpad por injeção direta de bits (touch errava right/down — descoberto que o touch do dpad só pega offsets negativos); botões (pulo/tiro/arma/pause) por touch. Método p/ achar os bits: ler `[GDM+4]` enquanto injeta cada direção.
  - **🔊 ÁUDIO Cricket→SDL**: (1) file handler (banks do filesystem); (2) jni_shim short[] (New/Get/ReleaseShortArrayElements); (3) hook `AudioTrackProxy::write` → PCM p/ ring buffer → callback SDL (44100 S16 stereo); (4) **hook `getPlaybackHeadPosition` → frames tocados pelo SDL** (RAIZ: senão o `updateLoop` do Cricket via head=0 e nunca renderizava). renderBuffer des-stubado, write retorna `count`. Medido: 44100 frames/s, som normal.
  - **⏱️ SPEED (aceleração)**: jogo é FRAME-BASED (velocidade escala com render fps). **Sync vídeo→áudio**: paceia nativeRender pelo clock do áudio (g_frames_played), 1 frame a cada `MM_SPF` samples. default 1470 (~30fps) — **AFINAR o SPF com o usuário** (30fps ficou "um pouco rápido"; testar 1600+).
  - **hooks arm32 reusáveis** (hook_thumb_call/patch_thumb_jump) — chave p/ MM2-6.
- **ESTADO**: boot→gameplay JOGÁVEL (controle+som+inglês, 0 crash). **FALTA**: (1) afinar SPF (velocidade); (2) validar gameplay completo; (3) empacotar/limpar; (4) reusar base p/ MM2-6.

### BUILD / DEPLOY / RUN (a preencher conforme validar)
- Build (host): `cd ~/nextos_ports_android/ports/megaman1 && ./build.sh`.
- Deploy: `scp megaman1 root@192.168.31.114:/storage/roms/ports/megaman1/`; assets em `/storage/roms/ports/megaman1/assets/`.
- Run (matar+confirmar 0 instâncias ANTES): `HOME=$GAMEDIR LD_LIBRARY_PATH=/usr/lib32:$GAMEDIR ./megaman1` (SEM forçar SDL driver). Log em debug.log.
- Print sem TV: `cat /dev/fb0` OU glReadPixels (fb0 falha durante render Mali).
