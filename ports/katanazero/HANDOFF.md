# 🗡️ Katana ZERO — HANDOFF (Mali-450 so-loader)

## ========================= LEIA PRIMEIRO — PRÓXIMA SESSÃO (s5) =========================
### 🏆 s4 (2026-06-25): BINÁRIO ÚNICO UNIVERSAL (glibc 2.27) RODA NO R36S **E** NO MALI-450
`build_compat.sh` (docker debian:bullseye) compila o loader contra glibc antiga → **GLIBC max 2.27** →
binário ÚNICO p/ qualquer device. 🔑 stub **libz.so.1** com os 13 símbolos zlib que o `libyoyo` importa
(compress/crc32/deflate*/inflate*/zError — descomprime game.droid do APK); SEM ele = `UNRESOLVED import
zError` → crash. Device usa libSDL2/GLESv2/EGL/libz REAIS em runtime. Mesmo src/ (buttonMask+SELECT+START+
R1/L1). Binário 189KB = `katanazero` oficial nos 2 devices + local (nativo glibc2.38 = katanazero.native-glibc238).
**R36S/ArchR (169.254.170.2 root/archr)**: RODA E RENDERIZA (contexto GLES2 kmsdrm 640x480 OU wayland 1280x720;
tela disclaimer Netflix em INGLÊS+estática VHS; SDK READY; room_optionsprompt). 🧱 MURO = **CMA 96MB** (R33S,
MemTotal 480MB) → Mali OOM ao carregar (`mali gpu: OOM notifier: katanazero 70076kB`). Frontend essway+sway
usa CMA; parar libera só ~55MB (algo segura ~41MB). Cada run morto VAZA CMA → **reboot limpa** (NextOS: "sempre
reiniciar pra CMA limpo e suficiente"). Pra jogabilidade plena talvez `cma=192M` no /flash/extlinux.conf (NÃO
autorizado ainda; tira RAM, 480→384MB; /flash read-only→remount). Services R36S: essway(ES)+sway(wayland)
SEPARADOS; kmsdrm precisa parar OS DOIS (sway segura DRM). Launcher `Katana ZERO.r36s.sh` (/roms/ports).
swapfile 2GB /storage/swapfile_kz (runtime). Commits: 72c120a/219a29e/a1a8e87.
### 🔧 s4 controles extras (Mali-450): R1/L1 do controle do NextOS vêm como JOYBUTTON 5/4 que o SDL NÃO mapeia
p/ shoulders → feed NATIVO direto (joy5→R1 kc103, joy4→L1 kc102, katana_jni.c). +SELECT+START exit nativo
(g_exit_combo) + launch.sh/Katana ZERO.sh sem gptokeyb (gptokeyb quebraria o gamepad nativo).

### 🏆 s3 RESOLVIDO (2026-06-25): CONTROLE DO GAMEPLAY FUNCIONA! (NextOS confirmou "deu certoo")
**RAIZ:** `AndroidGamepadConnected(id,name,guid,vendor,product, p5, p6, axisCount, BUTTONMASK)` — o
ÚLTIMO arg NÃO é hatCount, é a **BITMASK de botões** (posições SDL_GameControllerButton). Disasm
(0x1275188): `buttonMask = ultimo_arg | 0x7800` (0x7800 = bits 11-14 = DPAD up/down/left/right sempre
add). Passávamos `1` → mask `0x7801` = **SÓ botão A (bit0) + dpad**. O engine FILTRA botões fora do
mask → o auto-mapping virava só `a:b0,dp...` → X/B/Y/ombros nunca registravam. Menu funcionava porque
só usa A + dpad (ambos no mask). Ataque [X] do gameplay = gp_face3 = SDL button 2 = bit2 → FORA do mask
→ nunca registrava. **FIX (katana_jni.c:324):** último arg `1` → `0x7FFF` (15 botões: A,B,X,Y,BACK,
GUIDE,START,LStick,RStick,LShoulder,RShoulder + dpad). Runtime confirma auto-mapping COMPLETO:
`a:b0,b:b1,x:b2,y:b3,back:b4,guide:b5,start:b6,leftstick:b7,...`. 🔑 LIÇÃO REUSÁVEL p/ QUALQUER port
GameMaker/YYC: o buttonMask do AndroidGamepadConnected é o ÚLTIMO arg e DEVE ser a bitmask completa SDL.
DESCOBERTA via log de runtime "GAMEPAD auto mapping - ...a:b0..." (não dá p/ ver só na fonte estática).
Hipóteses INVESTIGADAS e DESCARTADAS: save-fix (my_yyerror/BundleFileExists) NÃO toca o path do ataque
(0 refs); YYError engolido no gameplay = 0; analógico/aim era 2ª camada mas o press já destravou.
⚠️ inject_pad NÃO valida X: spoofa vendor/product Xbox360 → SDL aplica mapping real do Xbox (BTN codes
≠ os do inject_pad) → só A passa. Validação real = controle físico do NextOS.

### 🎯 PRÓXIMA TAREFA (s4): EMPACOTAR + COMMIT (controle JÁ resolvido)
- [ ] launcher PortMaster: `launch.sh` + `katanazero.gptk` → `ports_scripts/Katana ZERO.sh` E `ports/`
      (regra emuelec: deployar nos DOIS). SELECT+START sai.
- [ ] R2 (ports_aio). Commit master (SEM co-autor Agente).

### 🟢 ESTADO ANTERIOR (fim s2, 2026-06-25): JOGÁVEL até gameplay + ÁUDIO + 0 crash
Device = **192.168.31.89** (mudou de IP; auth por **chave SSH**, NÃO senha vazia). Mali-450 Amlogic.
Fluxo OK: boot → disclaimer(inglês) → título → New Game/Continue → cutscene/level_select → **room_factory_0
(gameplay)**. Controle navega MENUS, áudio toca (música+SFX pela HDMI), autosave não crasha. SÓ FALTA: o
**ataque in-game** (prompt [X] da intro) não dispara.

### 🎯 ÚNICA TAREFA DA s3: fazer o ataque [X] do gameplay funcionar
NextOS confirmou: o "[X]" é o **botão X do Xbox** (= gp_face3). Os prompts de tutorial usam um path de input
DIFERENTE dos menus. Testado e DESCARTADO: (a) feed duplo CONTROLLERBUTTON+JOYBUTTON não era o conflito —
deixei input LIMPO (só gamepad nativo, default) e ainda não passa; (b) gamepad nativo (todos botões), teclado,
mouse click, mouse swipe — nada avança; (c) IO_Update() por frame (add, não resolveu o gamepad-pressed).

**ESTUDAR O SONIC (sonicmania) — é a chave.** Sonic alimenta input DIFERENTE de nós:
- Sonic NÃO usa AndroidGamepad nativo. Usa `OnKeyEvent(env,thiz, androidKeycode, down)` (keycodes 96-109 p/
  botões, 19-22 dpad) + chama `AndroidInputDevice::Init()` e `Controller::Init()` no startup p/ inicializar
  o device de input. O engine RSDK processa os keycodes. (ver sonic_jni.c linhas ~237-260 e o loop ~300-310)
- Katana (GameMaker) NÓS usamos `AndroidGamepadConnected`+`AndroidGamepadOnButtonDown` (gamepad nativo). Menu
  funciona, gameplay não. **HIPÓTESE FORTE s3:** o gameplay lê via `get_input`→`controller_check`→buffer
  setado por `set_new_input` (gml_Script_set_new_input 0x32103b4), que talvez leia uma FONTE que não
  alimentamos (ou precisa do path estilo OnKeyEvent). OU falta inicializar o input device (estilo
  Controller::Init do Sonic) p/ o gameplay reconhecer o gamepad.

### 🔬 PLANO s3 (passos concretos):
1. **Ver o que `set_new_input` (0x32103b4 / GlobalScript) LÊ** — disassemblar e achar a fonte (keyboard_check_
   pressed? gamepad? io_inputs?). Essa é a fonte que o gameplay usa. Alimentar exatamente isso.
2. **Consertar o gp hook DIAG** (KZ_GPHOOK): a reimplementação retorna 0 (quebra menu). gp=0x55..(válido),
   bi certo, mas `GMGamePad::ButtonPressed(gp,bi)`=0 → o array pressed [gp+48] não tá populado quando o hook
   lê. Investigar: (a) talvez 2 GMGamePad objs (USB real + nosso dev0) e o update popula um, o check lê outro;
   (b) `GMGamePad::Update`(0x1270b14)/`GamepadUpdateM`(0x12758d0) popula [gp+40]down/[gp+48]pressed/[gp+56]
   released a partir do raw [obj+116] que AndroidGamepadOnButtonDown seta — confirmar se roda por frame e p/
   QUAL device. Se não roda p/ o nosso dev, CHAMAR por frame alinhado ao dev.
3. **Alinhar device:** confirmar que o gameplay checa o MESMO device (slot 0) que registramos. Logar o array
   de gamepads e o device que recebe os eventos.
4. **Plano B (estilo Sonic):** alimentar o ataque via o path do GameMaker que o gameplay realmente lê — pode
   ser feed via KeyEvent com o keycode certo SE set_new_input ler keyboard, OU achar a função de "input
   pressed" do gameplay e setá-la.

### 🧰 FERRAMENTAS/FLAGS DIAG (já prontas, gated por env — off no build normal):
- `tools/inject_pad.c` → `/tmp/inject_pad <delay_ms> CMD...` — gamepad uinput Xbox360 (SDL reconhece, navega
  menu+gameplay). CMDs: A B X Y LB RB SELECT START LS RS, HATU/D/L/R, AIM:x,y (right stick). Ex: navegar até
  gameplay = `A:3500 A:3500 A:4000`.
- `/tmp/inject_mouse` (click) e `/tmp/inject_swipe` (swipe direcional) — testar mouse/touch.
- `tools/inject.c` → `/tmp/inject` — teclado uinput.
- ENV flags: **KZ_GPHOOK** (loga gp_button pollado + reimpl ButtonPressed — quebra menu, consertar),
  **KZ_GPFORCE=<btn>** (força um gp_button=true; ex 32771=gp_face3/X), **KZ_KBHOOK** (keyboard_check held —
  NUNCA chamado pelo jogo), **KZ_KBDFEED** (liga feed teclado dos botões do controle), **KZ_JOYFEED** (liga
  JOYBUTTON), **KZ_KCDUMP**, **KZ_INLOG** (loga eventos input), **KZ_SNDLOG** (loga áudio), **KZ_MAXSECONDS**,
  **KZ_SHOTEVERY=N** (glReadPixels→/tmp/kz_shot.raw, 1280x720 BGRA flip-V → PNG).
- Símbolos-chave: get_input GlobalScript 0x1c07304, controller_check 0x185cbe8, set_new_input 0x32103b4,
  player_attack_check 0x3135da4, F_GamepadButtonCheckPressed 0x11224e4, GMGamePad::ButtonPressed 0x1270574
  (lê [gp+40]down/[gp+48]pressed/[gp+56]released), TranslateGamepadButtonM 0x1276420, array gamepad =
  *(*(text_base+0x47cf590))[device], count = *(*(text_base+0x47cf580)), AndroidGamepadOnButtonDown 0x1275760
  (seta bit em [dev+116]), IO_Update 0x1271b10. btn-map keycode→gp_idx: [0]=96(A)[1]=97(B)[2]=99(X)[3]=100(Y)
  [9]=102(L1)[10]=103(R1)[6]=108(START)[15]=109(SELECT)[17]=104(L2)[18]=105(R2). gp_face constants:
  gp_face1=0x8001 face2=0x8002 face3=0x8003(X) face4=0x8004.
- RODAR: kill antes (`pkill -9 katanazero`), `nohup env KZ_MAXSECONDS=N ./katanazero >/tmp/kz_play.log 2>&1 &`.
  Matar com kill -TERM (teardown EGL). Capturar áudio: `parec -d hdmi_real.monitor --raw --format=s16le ...`.
## ====================================================================================

# 🗡️ Katana ZERO — HANDOFF (Mali-450 .100 so-loader)

## ESTADO s1 (2026-06-25): 🏆 IMAGEM NA TELA. Boot completo → DISCLAIMER inglês → title. 0 crashes.
Engine GameMaker Studio 2 / YYC (`libyoyo.so` arm64), edição Netflix (`com.netflix.NGP.KatanaZero`).
Fork do port `sonicmania`. Prova de imagem: o disclaimer renderiza perfeito em inglês (FreeType).
Fluxo: room_logo (ASKIISOFT TV CRT) → room_optionsprompt → room_saveprompt → room_title.

## COMO RODAR
```
cd ~/nextos_ports_android/ports/katanazero
bash build.sh                       # toolchain Amlogic-old aarch64, -fuse-ld=bfd
rsync -a -e "sshpass -p '' ssh" katanazero root@192.168.31.89:/storage/roms/ports/katanazero/
# no device (matar antes, kill -TERM p/ teardown EGL!):
ssh root@.100 'systemctl stop emustation; cd /storage/roms/ports/katanazero; KZ_MAXSECONDS=60 ./katanazero > /tmp/kz.log 2>&1'
```
Arquivos no device em `/storage/roms/ports/katanazero/`: katanazero, libyoyo.so, game.droid,
audiogroup1-4.dat, song_*.ogg, options.ini, **katanazero.apk** (364MB, Startup lê game.droid via zip dele).
🚨 SEMPRE matar com `kill -TERM` (NÃO -9) → teardown EGL roda, senão TRAVA o Mali fbdev (device sai da rede,
precisa power-cycle). Captura: `KZ_SHOTEVERY=120` → glReadPixels → /tmp/kz_shot.raw (dd /dev/fb0 degrada por ssh).

## O QUE FOI RESOLVIDO (não mexer)
1. **so_load multi-segmento PT_LOAD** (so_util.c) — libyoyo tem 3 LOAD; o loader só carregava o último →
   `.data.rel.ro` (module-classes FreeType) zerado → crash. Loop copia todos.
2. **Startup(env,clazz, jstring APK_PATH, saveDir, ?, density, bool)** — arg2 = caminho do APK (zip_open lê
   game.droid de dentro). saveDir COM barra final (`/storage/roms/ports/katanazero/`).
3. **Bypass Netflix SDK** (katana_jni.c) — extensions checadas por tipo Java: NfxaInit→sentinela Double 1.0,
   GetPlayerID→sentinela String "katanaplayer1". **Pós-NfxaShowUI o jogo BLOQUEIA esperando async event** →
   postamos via CreateAsynEventWithDSMap(map, **70**=social) com keys: eventType="Nfx_onPlayerAccessChanged"
   (+aliases event_type/type/Current_Event_Type), **current="katanaplayer1"** (sem isso fica "undefined" e
   não concede), previous, playerID, id/handle=1 → "SDK READY".
4. **Áudio** — AAssetManager_open tenta sem prefixo "assets/" (audiogroups no root). 0 crashes.
5. shims: getprogname, FileExists hook (força 1 só .ini), SL_IID via dlsym, pthread bridge, raise-skip.
6. **Teardown EGL** (kz_teardown no SIGTERM/KZ_MAXSECONDS).

## ✅ INPUT ABI CONFIRMADO ESTATICAMENTE (s2, disasm libyoyo, alta confiança)
`Java_...KeyEvent(env,clazz, a,b,c,d,e)` (0x14f9054) faz tail-call `RegisterAndroidKeyEvent(a,b,c,d,e)`
(0x1277480). Decodificado: **w0=a=action (cmp w0,#1 → 1=down/0=up), w1=b=keycode** (`ldr w10,[x8,#12];
cmp w10,w1` casa keycode na fila; `stp w0,w1,[x8,#8]` action@+8/keycode@+12). p3(d)==-1 é caso especial
"limpar". Keycode raw Android é traduzido depois p/ vk GameMaker via tabela `g_AndroidKeyCode`.
→ Nosso `g_keyevent(env,thiz, down?1:0, android_kc, 0,0,0)` BATE EXATO. Keycodes Android padrão usados
(19-22 dpad, 66 enter, 111 esc, 54/52/31/50=Z/X/C/V, 62 space, 59 shift) são traduzíveis pela tabela.
**Input deve funcionar de 1ª no device.** Vars GML do jogo: g_VAR_keyboard_key_{confirm,attack,jump,
crouch,interact,item,back,pause,left,...} (config runtime). JNI alt: onGPKeyDown/Up (0x14fb958/0x14fb94c).

## 🏆 s2 (2026-06-25): JOGÁVEL! Menu→gameplay (room_factory_0), controle+áudio OK, 0 crash
Device mudou p/ **192.168.31.89** (DHCP; auth por CHAVE SSH, não senha vazia). É o mesmo Mali-450.
3 fixes desta sessão (TODOS reusáveis p/ GameMaker/YYC Netflix):
1. **🔴 CRASH no room_title (musica do titulo)** — `Audio_SoundPlay` chama `LoadSave::BundleFileExists`
   p/ os song_*.ogg streamed; o original checa APK assets (JNI `DynamicAssetExists`) / game.droid, NÃO
   acha os .ogg soltos no port dir → retorna 0 → `YYError` fatal → `__cxa_throw` → terminate → SIGSEGV.
   FIX = **hook nativo `_ZN8LoadSave16BundleFileExistsEPKc`** (`my_bundlefileexists`) checando o DISCO
   real (strip "assets/" + basename). (Tentar via JNI DynamicAssetExists falhou: varargs do
   CallStaticIntMethod entrega path lixo.)
2. **🎮 CONTROLE (gamepad nativo)** — o menu do titulo (obj_titlemenu) usa o sistema de gamepad NATIVO
   do GameMaker (gamepad_button_check), NÃO o teclado (KeyEvent não navega o menu, confirmado). FIX =
   registrar device via `AndroidGamepadConnected(id, name, guid, vendor, product, btnCount, 32, axisCount,
   hatCount)` (assinatura tirada do disasm de `onGPDeviceAdded`) + alimentar `AndroidGamepadOnButtonDown/
   Up(id, androidGpKeycode)` (faces 96-100, L1/R1 102/103, start108/select109, sticks106/107), dpad via
   `AndroidGamepadOnHat(id,0,x,y)`, eixos via `AndroidGamepadOnAxis(id,axis,val)`. SDL GameController =
   layout Xbox padrão → adapta a QUALQUER controle (gamecontrollerdb). NextOS confirmou: controle funciona.
3. **💾 CRASH no AUTOSAVE (cloud write)** — gameplay auto-salva via `NfxaCloudWrite` (Netflix SDK) → op
   ESPERA resultado async; nós não entregamos → TIMEOUT 10s → status -9902 → no WRITE chama `YYError` →
   mesmo crash (cxa_throw→terminate). FIX = **hook `_Z7YYErrorPKcz` (`my_yyerror`) NÃO-FATAL** (loga+retorna,
   não lança) — o save LOCAL já persiste (jogo mantém KatanaSave.zero), só o sync de cloud falha em silêncio.
   (Tentei completar a op via `CloudResultString`/drenar a lista em base+0x47cf1c0 — mas a lista só tem ops
   já completadas (state=7); as pendentes não aparecem lá durante o pending. Hook YYError é mais robusto.)
   ⚠️ Boot ainda demora ~20s pelos 2 cloud READ timeouts (graciosos, não crasham) — otimização futura.
- Áudio: opensles_shim → SDL_OpenAudioDevice (driver AUTO, regra #6) → pulse sink hdmi_real RUNNING,
  app conectado, 100% vol não-mutado. 1 player ativo. SOM SAI pela HDMI.
- Rooms: saveprompt→optionsprompt→logo→title→(New Game)cutscene→factory OU (Continue)level_select→fase.
- Tooling: `tools/inject.c` = injetor uinput de TECLADO; falta inject_pad p/ teste auto de gamepad.

## 🔊 s2 ÁUDIO RESOLVIDO (musica/SFX saem pela HDMI). 3 fixes combinados:
1. **hook `getJNIEnv` -> fake_env SEMPRE** (my_getjnienv): getJNIEnv retorna pthread_getspecific
   (env thread-local); a thread de streaming OGG (COggThread, musica .ogg) e criada internamente
   e NUNCA tem o TLS env -> retornava NULL -> "m_pJavaVM was null for OGG thread" -> falha ao abrir .ogg.
2. **`GetJavaVM` (idx 219/0x6D8) no fake_env** (j_GetJavaVM -> *vm=fake_vm): COggThread::StartThread faz
   env->GetJavaVM(&m_pJavaVM); sem isso o membro fica null mesmo com env ok.
3. **`opensles_shim_pump_callbacks()` no loop Process por frame**: a thread OGG enfileira UM buffer e
   ESPERA o callback de "buffer consumido" p/ enfileirar o proximo. Esse callback so era pumpado dentro
   de ALooper_pollAll (GameMaker NAO usa) -> musica enfileirava 1x e ficava muda (ring vazio). Pump
   por frame mantem a musica fluindo. Verif: parec do sink hdmi_real.monitor -> peak>0; mixpeak>0.
   Áudio = opensles_shim -> SDL_OpenAudioDevice (driver AUTO, regra #6) -> pulse hdmi_real.

## 🎮 CONTROLE GAMEPLAY — MURO (menus OK, ataque in-game [X] NAO) — investigado a fundo s2
Adicionado `IO_Update()` (0x1271b10) por frame DEPOIS do Process (computa pressed/released de teclado/
mouse; nao tinha caller no engine pq o runner Android chamava pelo loop Java). NAO destravou o ataque.
Hooks de DIAG (gated, off por padrao): KZ_GPHOOK (F_GamepadButtonCheckPressed -> loga gp_button pollado +
reimpl via TranslateGamepadButtonM+GMGamePad::ButtonPressed; array gamepad em *(*(text_base+0x47cf590))[dev]),
KZ_KBHOOK (YYGML_keyboard_check 0x1134a70 -> keyboard_check held NUNCA e chamado!), KZ_KCDUMP.
ACHADOS: (a) `keyboard_check` (held) nunca chamado -> o jogo nao usa keyboard held. (b) o menu polla TODOS
gp_face (0x8001-0x8010) via gamepad_button_check_PRESSED e FUNCIONA sem hook. (c) Minha reimpl de
ButtonPressed retorna 0 (gp valido 0x55..., bi certo, mas [gp+48]=pressed array nao populado quando MEU
hook le) -> quebra o menu, entao nao da p/ ver o que o PROMPT polla isolado. (d) IO_Update cuida de kb/mouse,
NAO do gamepad pressed (esse vem de GMGamePad::Update/GamepadUpdateM 0x12758d0, populando [gp+40]=down
[gp+48]=pressed [gp+56]=released a partir do raw [obj+116] que AndroidGamepadOnButtonDown seta).
HIPOTESES P/ PROXIMA: (1) talvez 2 GMGamePad objs (real USB + nosso dev0) e o update popula um, o check le
outro -> alinhar device. (2) talvez o prompt use TOUCH (device_mouse) com POSICAO especifica (mobile). (3)
confirmar se gamepad pressed e populado dentro do Process (entao timing do hook) ou nao (entao falta chamar
GMGamePad::Update/GamepadUpdateM por frame, alinhado ao dev que recebe os eventos Android).
⚠️ PRECISA: saber do NextOS (conhece o jogo) o que o prompt [X] da intro pede e o botao no Katana original.

## (orig) CONTROLE GAMEPLAY — detalhe
Na intro (room_factory_0, fantasma azul + prompt [X]) o input CHEGA no gameplay (o ghost responde ao
aim/botoes, muda de pose) MAS o "executar ataque" do prompt [X] NAO dispara — testado gamepad nativo
(gp_face1-4/L/R/triggers/start/select), teclado (X=52 e outros), E mouse (uinput click). O proprio
controle REAL do NextOS tambem nao passa. O jogo usa get_input -> controller_check (le buffer setado
por set_new_input) + gamepad_button_check/keyboard_check/mouse_check_button. obj_button_prompt checa um
input CONFIGURAVEL (func id vem de variavel setada pelo objeto-pai). Tabela btn-map do jogo (dump):
[0]=96(A/gp_face1) [1]=97(B) [2]=99(X/gp_face3) [3]=100(Y) [9]=102(L1) [10]=103(R1) [15]=109(SELECT)
[6]=108(START) [17]=104(L2) [18]=105(R2) — X(99)->gp_face3 OK. PROXIMO: dumpar g_AndroidKeyCode[52]
em runtime p/ confirmar traducao teclado X->vk88; OU achar o input exato que set_new_input le p/ "attack"
(gml_Script_player_attack_check 0x3135da4 / set_new_input 0x32103b4 / get_input GlobalScript 0x1c07304);
OU ver se a build Netflix espera TOUCH/swipe (mobile) no lugar do botao. Tools: tools/inject_pad.c
(gamepad uinput Xbox360, SDL reconhece, navega menus+gameplay), /tmp/inject_mouse (mouse uinput).
⚠️ USB Gamepad real gera CONTROLLERBUTTON **E** JOYBUTTON juntos -> path teclado recebe 96-109 duplicado
(talvez remover o handler SDL_JOYBUTTON, redundante com CONTROLLERBUTTON).

## TODO s2 (precisa do device)
- [ ] **Input/menu**: ABI confirmado (acima). VALIDAR no device que navega o menu do título e dá New Game.
      Se teclado não responder, testar path GAMEPAD: `onGPKeyDown`/`onGPDeviceAdded`/`registerGamepadConnected`
      (paddleboat) com keycodes de botão (A=96 etc.).
- [ ] Confirmar gameplay (entrar numa fase), áudio in-game, inglês.
- [ ] Empacotar launcher PortMaster: `launch.sh` + `katanazero.gptk` (prontos) → `ports_scripts/Katana ZERO.sh`
      E `ports/` (regra emuelec). gptokeyb mapeia controle→teclado. SELECT+START sai.
- [ ] Atualizar R2 (ports_aio). Commit master (SEM co-autor Agente).
- glErr=0x500 todo frame = BENIGNO (render funciona).

## Flags env (debug)
KZ_MAXSECONDS / KZ_MAXFRAMES (saída limpa) · KZ_SHOTEVERY=N (screenshot glReadPixels) · KZ_NONFX (não postar
evento Netflix) · KZ_NFXEVENT=<nome> · KZ_NOSKIP (não pular raise) · KZ_TESTCLEAR (clear vermelho) · KZ_W/KZ_H.
