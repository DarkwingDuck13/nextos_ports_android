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
  - PRÓXIMO: (1) **CONTROLES gamepad→multitouch** nos controles virtuais (dpad+pulo+tiro+arma; hold com touch ids distintos) — validar Mega Man anda/pula/atira; (2) **ÁUDIO real** (banks Cricket: fazer carregar via filesystem OU rotear output p/ SDL; hoje silencioso); (3) empacotar launcher ES + gptokeyb; (4) reusar base p/ MM2-6.

### BUILD / DEPLOY / RUN (a preencher conforme validar)
- Build (host): `cd ~/nextos_ports_android/ports/megaman1 && ./build.sh`.
- Deploy: `scp megaman1 root@192.168.31.114:/storage/roms/ports/megaman1/`; assets em `/storage/roms/ports/megaman1/assets/`.
- Run (matar+confirmar 0 instâncias ANTES): `HOME=$GAMEDIR LD_LIBRARY_PATH=/usr/lib32:$GAMEDIR ./megaman1` (SEM forçar SDL driver). Log em debug.log.
- Print sem TV: `cat /dev/fb0` OU glReadPixels (fb0 falha durante render Mali).
