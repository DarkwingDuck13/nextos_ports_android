# Chrono Trigger → Mali-450 so-loader — HANDOFF / diário vivo

> ÂNCORA DE MEMÓRIA. Atualizar a CADA iteração. Se o contexto resetar, reler isto e continuar exatamente daqui.

## 🚨 REGRAS DO NEXTOS (este port)
1. **Resolver TUDO, sem atalhos.** Manter o **máximo do original**. Atalho só se NÃO tiver outro jeito.
2. **Entregar IMAGEM + CONTROLES + ÁUDIO funcionando.** Port não é difícil. **Começar e NÃO PARAR POR NADA** até a imagem.
3. (globais) Só master, sem co-autor/menção a Agente. Matar+confirmar 0 instâncias do jogo no device ANTES de lançar (por /proc/*/exe).

## ALVOS / ACESSO
- Device Mali-450: **192.168.31.100** (deploy em `/storage/roms/ports/chrono`, launcher em `/storage/roms/ports_scripts/`).
- Bancada de referência: **Moto G 100 via ADB** (transport_id:3) — frida/dump/trace do APK real se precisar.
- APK: `~/Downloads/ChronoTrigger-v2.1.4-full-apkvision.apk` (569MB).

## ENGINE / RECON
- **Cocos2d-x 3.14.1** (C++, build do "riq"/Ricardo Quesada). **GLES2 NATIVO** → zero conversão de shader no Mali-450.
- Libs: `libchrono.so` (12.5MB) + `libc++_shared.so` + `libencrypt.so` (4KB, cifragem de assets).
- Entry points (JNI-render-driven, igual Crazy Taxi):
  - `cocos_android_app_init(JNIEnv*, jobject)` — registra AppDelegate
  - `Java_org_cocos2dx_lib_Cocos2dxRenderer_nativeInit(env,thiz,w,h)` — init (chama cocos_android_app_init + Application::run)
  - `Java_org_cocos2dx_lib_Cocos2dxRenderer_nativeRender(env,thiz)` — 1 frame (Director::mainLoop)
  - input: `Java_org_cocos2dx_lib_GameControllerAdapter_nativeControllerButtonEvent/AxisEvent`, `Cocos2dxRenderer_nativeKeyEvent`
  - assets/ctx: `Cocos2dxHelper_nativeSetContext`, `nativeSetApkPath`, `AppActivity_setAssetManager`
- Assets: `assets/001.dat`..`008.dat` + `007-en.dat` + `Game/...` (cifrados XXTEA — servir BYTES CRUS, engine decifra com chave embutida) + `assets/Shaders/*.fsh/.vsh` (GLSL ES2 já). arm64-only.
- DRM: Google Play Licensing (`libstub`? não veio no arm64; LVL é Java) — ignorável no so-loader.

## ESTRATÉGIA DO LOADER
- Base = `ports/crazytaxi/src` (aarch64 so-loader, SDL2+GLES2, AAsset shim → `./assets/`, opensles_shim, ~499 imports). 90% pronto.
- Adaptar `main.c`: SO_NAME=libchrono.so; resolver e chamar os entry points Cocos2d-x; loop nativeInit→nativeRender; input SDL→GameControllerAdapter.
- Estender `jni_shim.c`: callbacks Java do Cocos2d-x via CallStaticObjectMethod retornando jstring certo — `getCocos2dxWritablePath`, `getCurrentLanguage`, `getAssetsPath`/apk path, package name. (Texto via Cocos2dxBitmap=Java; TTF do cocos usa FreeType nativo → imagem primeiro, texto-Java depois se faltar.)
- Assets servidos crus do disco via AAsset shim (engine decifra). Deploy assets em `/storage/roms/ports/chrono/assets/`.

## STATUS / LOG
- s0 (2026-06-24): recon completo, scaffold criado, src copiado de crazytaxi, libs extraídas.
- s1 (2026-06-24): 🎉 **TELA DE TÍTULO RENDERIZANDO (IMAGEM ✅) + ÁUDIO ✅ (sead BGM)**, 0 crash, vivo indefinido. Device 192.168.31.100.
  FIXES (todos em ports/chrono/src; sem atalho, mantendo original):
  1. **so_util multi-módulo** (adotado do terraria) + **busca em módulo auxiliar**: libchrono importa libc++ do Android (`std::__ndk1`); carrego `libc++_shared.so` como 2º módulo (`so_set_aux_module`) e `so_resolve` resolve os `__ndk1`/`__cxa_*`/operator new-del contra ela (`so_aux_find_export`).
  2. **so_load layout-agnóstico**: libc++_shared tem PT_LOAD R/R+X/RW/RW (text NÃO no 1º seg nem vaddr 0); reescrevi p/ ancorar tudo em vaddr0=load_base e copiar todos PT_LOAD. Removi `so_finalize()` (re-protegia módulo todo RW→sem X); heap já é RWX.
  3. **stubs bionic-only** em imports.c: `__system_property_get`, `__android_log_assert`, `SL_IID_ANDROIDSIMPLEBUFFERQUEUE` (senão slot vira PLT0→crash).
  4. **JNI GetStringChars/ReleaseStringChars (UTF-16)**: cocos `StringUtils::getStringUTFCharsJNI` usa GetStringChars (não UTF8) → sem isso `std::u16string(NULL,len)`→memcpy NULL→crash. + métodos estáticos Java retornam strings certas (writable path/language/etc).
  5. **CANARY BIONIC**: pad `_Thread_local`[256] + guard SDL_GL (tpidr+0x28).
  6. **pthread_attr bionic(56B)≠glibc(64B)**: `DelegateManager::Initialize` (sead) aloca attr na pilha tam-bionic; glibc pthread_attr_init/setschedparam escreviam 64B→estouro→canário. Fix: no-ops (pthread_create_fake já ignora o attr). RAIZ do __stack_chk_fail.
  7. **stdio __sF bionic**: wrappers `sf_*`/`map_sf` (fwrite/fprintf/fputc/fputs/fflush/ferror/feof/fileno/fgetc/fgets/vfprintf) traduzem `&fake_sF[0/1/2]`→stream glibc real (senão "invalid stdio handle"→abort).
  - Env device: `SDL_VIDEODRIVER=mali SDL_AUDIODRIVER=pulse HOME=$GAMEDIR`. assets em /storage/roms/ports/chrono/assets/ (resources.bin=420MB é o arquivo PRINCIPAL, 001-008.dat tb). debug.log na pasta do jogo.
  - PRÓXIMO: **CONTROLES** (testar gamepad avançar do título→menu; loader tem path cocos Controller::Key e nativeKeyEvent/CHRONO_KEYBOARD). Depois empacotar launcher ES.
- s2 (2026-06-24): 🎉 **CONTROLES (TOQUE) ✅ → MENU PRINCIPAL ALCANÇADO**. Título→menu (2 slots New Game/Continue + cursor + ícones cloud/config/controle/privacy). **DESCOBERTA: jogo é 100% TOUCH** — nem cocos Controller::Key nem nativeKeyEvent avançam título/menu; SÓ `nativeTouchesBegin/End` funciona (testado: toque no centro avança, nz 964→1706). Loader agora resolve nativeTouchesBegin/End/Move + CHRONO_AUTOPRESS injeta toque.
  ⚠️ FALTA p/ HANDHELD (sem touchscreen): mapear **gamepad físico → toque**. Opções: (a) cursor virtual via analógico + A=tap (universal), (b) gptokeyb mouse-mode → SDL_MOUSE* → traduzir p/ touch no loader, (c) overlay por-UI (dpad/botões virtuais do CT no gameplay). Gameplay do CT mobile desenha dpad+botões virtuais na tela → mapear toques nas posições. RECOMENDADO: SDL_MOUSE→touch + gptokeyb mouse (reusa infra PortMaster).
  ✅ 3 ALVOS CORE DEMONSTRADOS: imagem (título+menu render lindo) + áudio (sead BGM) + input (toque avança o jogo). Resta o esquema de controle físico do handheld.
- s3 (2026-06-24) ESTUDO (NextOS: jogo TEM nav nativa por controle — a mãozinha NÃO é cursor de mouse, ela pula discreto p/ a opção ao lado, estilo Crazy Taxi; e faltam fontes nas opções):
  🎮 **CONTROLE NATIVO**: classes `GameController`(físico)+`VirtualController`(tela), via cocos `Controller`/`ControllerImpl::onConnected(name,id)` + camada própria `nsInput`/`nsKeySetting` + tela `nsMenu::MenuNodeConfig::setupKeySettingScreen` (config de teclas!). `GameController::isConnected()` = `(controllerId != -1)` (offset lido em isConnected@0x6daec0); `onConnectedController`@0x6da808 seta o id. `GameController::update(f)`/`UpdateKeyState`/`RepeatClear` = POLLED por frame. **TESTE: nativeControllerConnected(id0) repetido + cocos DPAD_RIGHT NÃO moveu a mãozinha** (continua na 1ª opção). Hipóteses p/ próximo: (a) jogo gateia em consulta Java de InputDevice (GameControllerHelper) que nosso fake-JNI zera; (b) precisa `ControllerImpl::onConnected` direto/timing; (c) há toggle de controle nas opções (sem fonte→invisível). 
  🔤 **FONTES FALTANDO**: libchrono EMBUTE FreeType (FT_Init/FontFreeType) → labels TTF renderizam; mas texto de UI usa método Java SE custom **`Cocos2dxBitmap.createTextBitmapShadowStroke`** (sombra+contorno) que o fake-JNI não atende → texto em branco. Callback nativo = `Java_org_cocos2dx_lib_Cocos2dxBitmap_nativeInitBitmapDC` sig **`(IIILjava/lang/String;[B)V`** e `(Ljava/lang/String;[B)V`. FIX: implementar createTextBitmapShadowStroke no fake-JNI → renderizar texto com FreeType num byte[] RGBA e chamar nativeInitBitmapDC.
  PLANO: (1) fontes primeiro (destrava ver opções/key-config), (2) destravar controle nativo com trace runtime de GameController::update/isConnected, (3) empacotar launcher ES + gptokeyb. NextOS quer controle bluetooth/qualquer via gptokeyb.
- s4 (2026-06-24) 🎉🎉 **INGLÊS + FONTES RESOLVIDOS — MENU 100% LEGÍVEL EM INGLÊS**: "New Game"/"Extras"+copyright completos, fonte Roboto, ZERO japonês. (print: scratchpad/final.png)
  🚫🇯🇵 **IDIOMA — RAIZ ERA A REGIÃO, NÃO A FONTE**: o jogo NÃO chama getCurrentLanguage; chama **`getLocationCode`** (região). Retornávamos 0=Japão → string table japonesa. FIX: `getLocationCode` retorna **1 = inglês** (default no código; env `CHRONO_LOC` ajusta). Aí o jogo carrega strings inglesas (007-en) e os labels viram "New Game" etc. (também hookei DeviceInfo::getCurrentLanguage→chrono_forced_lang p/ garantir, mas o lever real é getLocationCode). REGRA GLOBAL gravada: [[jamais-japones-em-jogos]].
  🔤 **TEXTO/FONTES (processo nativo ativado, fonte Roboto original do Android)**: o jogo renderiza UI via Java `Cocos2dxBitmap.createTextBitmapShadowStroke([BLjava/lang/String;IIIIIIIIZFFFFZIIIIFZI)Z` (fontName VAZIO = default do sistema Android = Roboto; o jogo NÃO empacota fonte p/ UI). Implementei o caminho nativo no fake-JNI:
    - `src/text_render.c`: FreeType (`-lfreetype` do sysroot) + **Roboto-Regular.ttf** (bundled em payload/, deploy em /storage/roms/ports/chrono/Roboto-Regular.ttf). `chrono_render_text(utf8,px,r,g,b,align,_,_)` → RGBA. **SEMPRE auto-dimensiona** (reqW/reqH do jogo são lixo/clipam — ignorados).
    - `src/jni_shim.c`: byte[] reais (NewByteArray/GetArrayLength/GetByteArrayElements/GetByteArrayRegion/SetByteArrayRegion idx176/171/184/200/208) + handlers CallStaticBoolean **A(119, jvalue[])**/V(118)/varargs(117). createTextBitmapShadowStroke→do_create_text_bitmap→render→`nativeInitBitmapDC`. ⚠️**ABI nativeInitBitmapDC**: o byte[] de pixels é lido do **3º arg (x4)**, não do último (desmontagem); passo `parr` em x4 E no último p/ garantir. nz subiu 1706→1938 = texto cheio.
  ✅✅ **STATUS: IMAGEM + ÁUDIO + TEXTO-INGLÊS + seleção verde funcionando, 0 crash.** Default sem env já = inglês.

═══════════════════════════════════════════════════════════════════
## 🧭 GUIA PRÓXIMA SEÇÃO (NextOS pediu: salvar tudo + como prints/vasculhar/Moto G/áudio/controles)
═══════════════════════════════════════════════════════════════════
### BUILD / DEPLOY / RUN
- Build (host): `cd ~/nextos_ports_android/ports/chrono && ./build.sh` (erros `ld bad subsection` são BENIGNOS — binário sai OK). Toolchain Amlogic-old aarch64 + freetype2 do sysroot.
- Device: **192.168.31.100** (EMUELEC, `sshpass -p emuelec ssh root@192.168.31.100`). Deploy: `scp chrono root@..:/storage/roms/ports/chrono/`. Assets já lá (resources.bin 420MB + 001-008.dat + Roboto-Regular.ttf).
- Rodar (regra: matar+confirmar 0 instâncias por /proc/*/exe ANTES): env = `HOME=$GAMEDIR LD_LIBRARY_PATH=/usr/lib:$GAMEDIR SDL_VIDEODRIVER=mali SDL_AUDIODRIVER=pulse` então `./chrono`. Flags debug: CHRONO_AUTOPRESS=1 (toca centro/avança título), CHRONO_TEXTLOG=1, CHRONO_JNILOG=1, CHRONO_LOC=N (região/idioma), CHRONO_FONT=path, CHRONO_KEYBOARD=1.
- Log do jogo: `/storage/roms/ports/chrono/debug.log` (debugPrintf). stdout buffer perde no crash → usar debug.log.

### COMO TIRAR PRINT (sem olhar a TV)
- No device: `cat /dev/fb0 > /tmp/fb.raw` (fb = 1280x720 visível, virtual 1280x1440 double-buffer, 32bpp BGRA). scp pro host.
- Converter (host, PIL): varrer frames de W*H*4=3686400 bytes, pegar o de mais pixels RGB!=0, `Image.frombytes('RGBA',(1280,720),chunk)`, split b,g,r,a → merge RGB → PNG. (script usado: scratchpad, vide final.png/en2.png).

### MOTO G (referência autoritativa — NextOS lembrou: root+adb+tudo)
- `adb devices` → Moto G 100 (transport_id pode mudar). É a BANCADA: tem o APK real do CT rodando no Android nativo.
- Usos: (a) `frida`/`adb logcat` p/ ver os args REAIS de createTextBitmapShadowStroke e a ABI de nativeInitBitmapDC; (b) confirmar como o controle nativo ativa (qual evento/registro o GameControllerHelper Java dispara → nativeControllerConnected); (c) comparar layout/posição de labels. ⚠️ spawn via frida pode ser bloqueado; attach costuma OK (como na bancada do Elderand).

### 🎮 CONTROLES (PRINCIPAL PENDENTE — NextOS: "só ativar meu controle"; a seleção verde já funciona via toque)
- Jogo é TOUCH (nativeTouchesBegin/End funcionam → avança título/menu). TEM controle nativo (classes GameController/VirtualController, EventListenerController, nsInput, tela setupKeySettingScreen) mas `nativeControllerConnected`+cocos Controller::Key NÃO moveram a seleção verde nos testes.
- `GameController::isConnected()`@0x6daec0 = (controllerId != -1); `onConnectedController`@0x6da808 seta o id. `update(f)`/`UpdateKeyState`/`RepeatClear` polled/frame.
- HIPÓTESES p/ ativar: (a) usar o Moto G p/ ver o que o GameControllerHelper Java chama (talvez precise `nativeControllerConnected` com vendor/id específico, ou um evento Android KeyEvent via `receiveExternalKeyEvent(IIZ)` que apareceu no JNILOG!); (b) **`receiveExternalKeyEvent`** parece o caminho de teclas externas — investigar (mandar key events por aí pode mover a seleção); (c) se nativo não ativar, mapear gamepad→toque OU gamepad→`receiveExternalKeyEvent`. NextOS quer: ativar suporte a controle (inclusive Bluetooth) e usar **gptokeyb** p/ qualquer controle.
- main.c já tem: SDL_GameController aberto, envia cocos Controller::Key (map_btn_cocos) e nativeKeyEvent; CHRONO_AUTOPRESS p/ teste. Falta o lever certo de ativação.

### 🔊 ÁUDIO (polir)
- Já abre (opensles_shim: SDL audio 44100/2ch + pulse) e sead toca BGM ("BGM bank destroy"). Confirmar SOM REAL saindo na TV (NextOS ouvir) e que efeitos/voz tocam no gameplay. Mali-450 = SDL_AUDIODRIVER=pulse (alsa falha set channels).

### 🎮 CONTROLES — ATUALIZAÇÃO s5 (2026-06-24)
- ⚠️ **DRIVERS SDL: NUNCA forçar SDL_VIDEODRIVER/SDL_AUDIODRIVER** (regra nova NextOS [[nao-forcar-sdl-driver]]). TESTADO sem eles: renderiza + áudio abrem AUTOMÁTICO (sistema escolhe). Remover do launcher/env.
- ❌ TESTADO e NÃO funcionou p/ mover a seleção verde New Game→Extras: nativeControllerButtonEvent (cocos DPAD_RIGHT) + nativeKeyEvent (Android DPAD) + forçar GameController::isConnected=1 (hook, CHRONO_FORCECONN). Menu NÃO navegou. => esse menu é navegado SÓ por toque do nosso lado; o caminho de controle nativo não dispara só com isso.
- ✅ **MOTO G PRONTO P/ RE**: instalei o CT real no Moto G (`com.square_enix.android_googleplay.chrono/org.cocos2dx.cpp.AppActivity`), frida-server em /data/local/tmp. Título idêntico; tap avança p/ cutscene de abertura (Toriyama). **PRÓXIMO TESTE AUTORITATIVO**: no Moto G, chegar no menu e mandar `adb shell input keyevent 22/21` (DPAD R/L) — se a seleção mover, o jogo responde a Android KeyEvent (source) e precisamos replicar o source/caminho certo; se NÃO, precisa InputDevice gamepad real → frida hook em nativeControllerButtonEvent/GameController::update p/ ver o que ativa. Também testar com controle BT pareado no Moto G (NextOS disse que tem suporte BT).
- main.c já tem hooks: chrono_forced_lang (idioma), chrono_force_connected (isConnected). cocos focus: existe `ui::Widget FocusNavigationController` — talvez a nav seja via focus-system (precisa setFocusEnabled/dpad nav). Investigar.
- PRÁTICO (fallback se native não ativar): mapear gamepad→toque nas posições dos itens, OU gptokeyb mouse→SDL_MOUSE→nativeTouches (NextOS quer gptokeyb/BT).

### 🎮 CONTROLES — s6 (2026-06-24) PROVA + BENCH FRIDA PRONTO
- ✅ **PROVADO: o jogo TEM controle nativo** — NextOS pareou um **PS5 (Bluetooth) no Moto G** e navega o menu (Extras↔New Game) PERFEITO. ⚠️ O PS5 foi SÓ pra PROVAR que existe suporte — **NÃO copiar config de PS5**; no handheld usamos OUTRO controle. A tarefa é **ATIVAR o caminho de controle nativo no nosso so-loader**.
- 🔬 **FRIDA BENCH MONTADO** (Moto G adb-rede 192.168.31.49:5555, frida-server v17.15.3 em /data/local/tmp/fs rodando como root setenforce0; host: venv `scratchpad/fridaenv` com frida 17.15.3; scripts `scratchpad/ctrl_hook.js` + `frida_spawn.py`/`frida_cap.py`). CT real instalado: `com.square_enix.android_googleplay.chrono`.
- 📊 **CAPTURADO (parcial)**: o jogo usa **cocos Controller::Key** nos eventos nativos (vi valores **1004=BUTTON_A**, **1001** num axis). controllerID real é GRANDE (~1290638904), NÃO 0. **NÃO capturei o CONNECTED** (dispara no startup, antes do meu hook que carrega ~9s após spawn) — ⚠️ próximo: hookar ANTES (spawn-gate no carregamento do módulo) OU re-parear o controle durante captura, p/ ver os args EXATOS de `nativeControllerConnected` (vendor, id).
- 🧠 **HIPÓTESE do porquê nosso loader não ativa**: nossa `nativeControllerConnected(id=0)` provavelmente não REGISTRA o controller no cocos do jeito certo (timing/args), então os `nativeControllerButtonEvent` são ignorados. Precisa: replicar a sequência/args reais do connect (capturar via frida) + garantir mesmo controllerID no connect e nos button events + talvez `Controller::startDiscoveryController`. Investigar tb o sistema de FOCUS do cocos ui (FocusNavigationController) e `receiveExternalKeyEvent`.
- 🔧 **PRÓXIMO PASSO LIMPO (nova seção)**: (1) frida: capturar CONNECTED args reais (hook cedo) + a sequência completa de uma navegação (connect→button dpad). (2) replicar EXATO no main.c (nativeControllerConnected com os args certos no momento certo + button events com mesmo id). (3) mapear o controle FÍSICO do handheld (evdev/SDL) → esses eventos nativos. (NÃO é sobre PS5; é o mecanismo de ativação.)
- arquivos host: ~/nextos_ports_android/ports/chrono/scratchpad? NÃO — bench em /tmp scratchpad (efêmero); recriar venv+frida-server se a sessão resetar (frida-server já no Moto G em /data/local/tmp/frida-server, copiar p/ fs + chmod755 + setenforce0 + run).

### EMPACOTAR (depois de controle+áudio OK)
- Launcher ES em `/storage/roms/ports_scripts/Chrono Trigger.sh` (com "s"), dados em ports/chrono. Setar env (SDL_VIDEODRIVER=mali, SDL_AUDIODRIVER=pulse, HOME, LD_LIBRARY_PATH) + gptokeyb + matar por /proc/*/exe. Ver modelo crazytaxi "Crazy Taxi Classic.sh". Rodar foreground puro (sem nohup/&/tee-vfat).
- ⚠️ commitar src/ no master (sem co-autor). Binário estável atual roda menu inglês.

- s7 (2026-06-24) 🎮🏆 **CONTROLE NATIVO RESOLVIDO — NAVEGAÇÃO 100% POR CONTROLE (padrão Xbox)**. NextOS confirmou na TV: A passa "Touch to Start"→menu, DPAD move a seleção verde New Game↔Extras, entra em gameplay perfeitamente.
  🔑 **RAIZ (bug de ABI)**: minha assinatura de `nativeControllerButtonEvent`/`nativeControllerAxisEvent` estava SEM o 1º arg `jstring vendorName`. ABI REAL = `(env, clazz, jstring vendor, int controllerId, int button, jboolean pressed, float value)`. Eu passava controllerId no slot do vendor → TODOS os args deslocavam 1 → o keycode 1013 (DPAD_RIGHT) caía no slot de controllerId e chegava como `0`/`1` no `GameController::onKeyDown` (provado via hook passthrough: antes key=0/1, depois key=1013=0x3f5). Fix: assinaturas com `void *vendor` 1º + `g_vendor=jni_make_string("Xbox Wireless Controller")` reusado em connect/button/axis.
  🔬 MÉTODO que destravou: (1) desmontei `GameController::onKeyDown@0x6da830` → mapa de teclas (DPAD_LEFT=0x3f4→bit9, DPAD_RIGHT=0x3f5→bit8, A=0x3ec→bit7… bitmask em [this+888]); (2) desmontei `nativeControllerButtonEvent@0x8d58fc`/`ControllerImpl::onButtonEvent@0x8d59d4` → acha controller por id no offset +112, e a chamada usa jstring2string(x2) ⇒ revelou o vendorName ANTES do id; (3) screenshot CONFIÁVEL via **glReadPixels** (`chrono_dump_shot`, fb0 falha durante render Mali); (4) hook passthrough `make_passthrough()` (copia 4 instrs não-PC-rel + salta addr+16) p/ logar onKeyDown sem perder a original.
  ⚙️ **SETUP ATUAL (default, sem env)**: connect id=0 no startup + `isConnected→1` por PADRÃO (hook `GameController::isConnected`, CHRONO_NOFORCECONN desativa) — garante que toda cena processe o controle mesmo perdendo o evento CONNECTED na troca de cena. SDL_GameController→send_button→ctrlButton (ABI corrigida). Régua NextOS: **controle padrão Xbox + gptokeyb por cima** (qualquer controle/BT vira Xbox→funciona).
  🧪 flags de teste (env-gated, fora do caminho normal): CHRONO_NAVTEST (fluxo controle-only: A passa título, DPAD nav), CHRONO_MAP (mapeia timeline+shots), CHRONO_HOOKKD (loga onKeyDown), CHRONO_T0. ⚠️ injeção de controle ANTES do menu existir CRASHA (SIGSEGV) — só injetar com cena ativa.
  🐞 menu só aparece após 1 tap/A no título (Square Enix splash→título→[A/touch]→menu). fb 1280x720 visível, glReadPixels é bottom-up (flip vertical ao converter).
  🔴 PENDENTE agora (NextOS testando ao vivo): **ÁUDIO gaguejado/bugado E baixo** — refill preso ao loop de render (opensles_shim_pump_callbacks 1×/frame); hitch de frame (resources.bin reaberto direto) esvazia o ring → stutter. pthread_create é REAL neste loader → solução = thread de áudio dedicada pumpando em taxa fixa (decouple do framerate), + investigar volume baixo. Depois: empacotar launcher ES (gptokeyb, sem forçar SDL drivers — regra #6).

- s8 (2026-06-24) 🔊🏆 **ÁUDIO LIMPO RESOLVIDO** (NextOS confirmou na TV "boaaaaa"). Era gagueira severa (PCM 97% silêncio, picos minúsculos intermitentes). 🔑**RAIZ = buffer raso**: `opensles_shim_pump_callbacks` calculava `refill_threshold = 2*last_enqueue_size` = ~3KB (o jogo enfileira pedaços de ~1.5KB), mas cada callback SDL consome 4096 frames@44100 = ~11.9KB de fonte@32000 → todo callback drenava 3/4 do buffer e underrunava o resto. FIX: `refill_threshold` FIXO em **32KB** (~250ms, cobre vários callbacks; CHRONO_ABUF=KB override) + `max_calls` 4→64 (enche o alvo após dreno). Resultado medido: readable 3072→~33KB, decdone=0, silêncio 97%→27% (só o splash inicial), sinal 5/120→82/120 janelas, pico 2663→8220.
  🩺 **MÉTODO de debug de áudio (reusável)**: (1) dump do PCM de saída do callback SDL via `CHRONO_PCMDUMP` (out.pcm) + análise RMS/silêncio no host com numpy → caracteriza starvation vs garbage; (2) `CHRONO_AUDIOSTAT` loga pumps/cbcalls/enq/decdone/readable 1×/s → isolou que readable ficava em 3072 (buffer raso, NÃO falta de produção — decdone=0). 
  🐞 **2º fix junto**: `AAssetManager_open_fake` fazia `debugPrintf` (=fopen+write+fclose de debug.log) a CADA abertura, e o jogo abre resources.bin centenas de vezes/frame → I/O stall no main loop. Gated por CHRONO_ASSETLOG (silencioso por padrão). + thread de áudio dedicada (s7) que pumpa em 2ms desacoplado do framerate + master_gain 0.30→0.65 (CHRONO_GAIN override).
  ✅ **STATUS: IMAGEM + CONTROLE NATIVO (Xbox) + ÁUDIO LIMPO + INGLÊS, 0 crash.** FALTA: empacotar launcher ES (ports_scripts/, gptokeyb, SEM forçar SDL driver — regra #6) + commitar src no master SEM co-autor.
