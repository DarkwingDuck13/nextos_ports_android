# Streets of Rage 4 — Port NextOS (MonoGame/.NET) — HANDOFF / diário de bordo

Objetivo: rodar **Streets of Rage 4** (APK Android v1.4.5) no device **<device-ip>**
(Mali-450). Trabalho autônomo, commitando cada conquista. Plano completo aprovado nas
notas internas do projeto.

> Atualizar este arquivo a cada descoberta/decisão (padrão NFS/Banjo). Convenção do projeto:
> registrar SEMPRE o estado git. Commits em PT, sem co-autor.

---

## 🏆🏆🏆🥊 GAMEPLAY RODANDO (2026-06-16 cont.11) — JOGÁVEL! Blaze em "The Streets" stage 1
Screenshot /tmp/sor4_streets.png: BLAZE em pé na fase 1 (rua), HUD completo (nome BLAZE + barra de
vida verde + score 000000 + estrelas), cenário urbano (prédios, cerca, lixeira, poster). O porter
JOGOU com o controle REAL dele do menu até a fase. CONTROLES = RESOLVIDOS (pad real funciona 100%
desde que entramos no menu). CADEIA DE FIXES desta sessão (todos confirmados com LOG/screenshot):

1. **MISTÉRIO DO "controle não passa do título" RESOLVIDO — NÃO era input.** Sonda Cecil (titleprobe,
   port/tools/titleprobe) em `TitleScreen.handle_input` provou: handle_input RODA todo frame; o press
   do pad gera borda limpa (`[HI] jbp=True cur=1 prev=0` = A com rising edge). O título JÁ setava
   `loading=true`. O muro era DEPOIS: `TitleScreen.handle_input` quando loading só chama `start_game()`
   se `platform.load_save_and_config_is_finished()`==true — e essa retorna
   `AndroidServices.DidFinishLoadingCloudFiles()` que está STUBADO (noopm) → false p/ SEMPRE → preso
   em loading eterno. **FIX: rettrue (port/tools/rettrue) força `load_save_and_config_is_finished`→true.**
2. **NRE `MainMenuScreen.MoreGamesNotificationUpdate`** (Firebase RemoteConfig + Android prefs, promo
   "more games", nada essencial). FIX: noopm.
3. **NRE `InterStageVideoScreen.update_gui→platform.video_get_time`** (vídeo de intro de fase). FIX
   TEMPORÁRIO/scaffold: `noopm platform.video_exists`→false → `transition_to_intro_video` retorna false
   → vai DIRETO pro gameplay (sem vídeo). A fase CARREGA (loading lento de decors/main_campaign/stage_1)
   e o gameplay aparece. ⚠️ SERÁ SUBSTITUÍDO por player de vídeo REAL (queremos vídeos de verdade).

PIPELINE SOR4.dll AGORA (port/tools/buildfix.sh, base=/tmp/SOR4.device.dll que já tem patchgam+noopm
+skipvideo+verstub+noopm EOSManager): + titleprobe(DEBUG-remover) + rettrue(load_save) +
noopm(MoreGamesNotificationUpdate) + [--skipvideo: noopm(video_exists)].

### INFRA DE INPUT AUTÔNOMO (criada nesta sessão, p/ testar SEM o porter)
- **vpad.py / vpadd.py** (port/tools): gamepad VIRTUAL via /dev/uinput clonando EXATO o " USB Gamepad "
  0810:0001 (mesmo bus/vendor/product/version/nome/caps → SDL calcula a MESMA GUID → mesmo mapping
  interno → jogo lê idêntico ao pad real). vpadd.py = DAEMON persistente lendo comandos de FIFO
  /tmp/vpadcmd (p/r/t <code>, hx/hy dpad, ax/ay stick, q). SDL detecta via hotplug udev.
- ⚠️ MENU só lê o controller **slot 0** (1º aberto = pad real do porter = js0/event2). Virtual entra
  em slot >0 e o MENU IGNORA (só o título aceita qualquer slot). P/ dirigir menu autônomo: unbind do
  pad real (`echo -n 1-1 > /sys/bus/usb/drivers/usb/unbind`; rebind com .../bind) p/ virtual virar slot0.
- Mapeamento evdev→jogo (pad 0810:0001): 288=Y, 290=B, 291=X; A≈289 (confirmar). dpad=ABS_HAT0X/Y(16/17).
- Screenshot autônomo: launcher faz glReadPixels→/tmp/sor4shot.raw + .dim no DEVICE; /tmp/getshot.sh
  puxa e converte p/ PNG (flip vertical). SOR4_SHOT=N (frame interval).

### 🎮 MAPEAMENTO DO CONTROLE CORRIGIDO (cont.11) — botões de ataque agora funcionam
Gameplay (relato): direcionais perfeitos, MAS X/B/Start agiam como START(pause), só Square atacava.
CAUSA: SDL usava o mapping BUILT-IN **"Twin USB PS2 Adapter"** (errado p/ esse pad 0810:0001) porque
o SDL_GAMECONTROLLERCONFIG do launcher usava GUID SEM CRC (`03000000...`) → ignorado. Runtime GUID
real = `0300605b100800000100000010010000` (CRC 605b, confirmado via /tmp/sdlprobe). FIX: run_diag.sh
agora seta SDL_GAMECONTROLLERCONFIG com a GUID REAL + mapping explícito
`a:b2,b:b1,x:b3,y:b0,back:b8,start:b9,leftshoulder:b4,rightshoulder:b5,lefttrigger:b6,righttrigger:b7,leftx:a0,lefty:a1,dpup:h0.1,dpdown:h0.4,dpleft:h0.8,dpright:h0.2`.
VERIFICADO ativo: sdlprobe gcName mudou de 'Twin USB PS2 Adapter' → 'USB Gamepad'. Agora face btns
evdev 288/289/290/291 = Y/B/A/X (b0/b1/b2/b3) e START vai p/ botão-base 297(b9). SoR4 default:
jump=A(290) attack=X(291) special=Y(288) pickup=B(289) backAttack=RT(295) starMove=RB(293). ⏳ o porter
confirma no playtest (se layout físico dele divergir, ajustar a string). Determinístico+gcName=forte.

### 🔊 SOM — DECISÃO: NATIVO (Wwise real do APK), NÃO reimplementação custom (decisão: "faça o nativo")
DESCOBERTAS desta sessão:
- wem 0x3040 do SoR4 = **Ogg Opus padrão no chunk "data"** (ffmpeg/libopus tocam direto, SEM vgmstream).
- Banks v135: Core.bnk(28MB,1743 wem DIDX), Generic.bnk(10MB,516 wem), Init.bnk(185 hirc), Music.bnk=32B
  (MÚSICA é STREAMED, fora dos banks — provavelmente nos arquivos obfuscados `\x00IAP` do gameassets).
- Device: PulseAudio rodando, libopenal 1.25.2, libopusfile, ffmpeg/libavcodec.62, SDL2_mixer. ALSA=AML-M8AUDIO.

CAMINHO CUSTOM (FEITO, mas NÃO desejado — fica de fallback/referência):
- port/tools/wwise_extract.py (parser HIRC+DIDX: event_id FNV-1 -> wem_ids, extrai .opus) +
  wwise_real.c (libWwise.so glibc: dlopen OpenAL+opusfile, post_event toca o .opus). FUNCIONOU end-to-end:
  ouvimos o SOM DE CONFIRMAÇÃO do menu. MAS parcial (sem música/mixagem/estados). Fallback deployado.

## ⏭️🔊 PRÓXIMA SESSÃO (cont.14) — MURO: Wwise renderiza SILÊNCIO (RAWpeak=0). Pipeline 100% OK.
**ONDE PAROU:** toda a cadeia de áudio nativo FUNCIONA (Wwise real carrega, init=1, bancos+613 wem
presentes, pump thread, OpenSLES->SDL->PulseAudio->HDMI sink RUNNING, ENQUEUE contínuo). MAS NÃO
SE ESCUTA. Diagnóstico definitivo: log `[opensles] ENQUEUE ... RAWpeak=0` = a Wwise enfileira buffers
de SILÊNCIO PURO (zeros), ANTES do meu volume. Logo a **própria Wwise renderiza silêncio** — não é
roteamento/volume do shim (masterg=0.30 ok, sem CORRUPT vol). active=1 (1 voz ativa) mas dados=zeros.

**HIPÓTESES p/ investigar (ordem de probabilidade):**
- (A) **MUTE por FOCO/BACKGROUND**: jogos Android mutam áudio quando a Activity perde foco. Nossa
  activity é FAKE (0x1000) -> o jogo pode achar que está em background e chamar AK::SoundEngine::Suspend
  ou setar volume master=0. INVESTIGAR: o jogo chama algum suspend? A resposta JNI de "hasWindowFocus"/
  "isFocused" precisa ser true? Ver AndroidGameActivity OnPause/OnResume no jogo.
- (B) **VOLUME RTPC/config=0**: o jogo seta volume master/música via RTPC ou config. Com saves no-opados,
  o default pode ser 0. INVESTIGAR: LOGAR native_wwise_set_rtpc_value (nome+valor) no wrapper -> ver se
  o jogo seta um RTPC de volume p/ 0. Ver Configuration/MetaGameConfig defaults de musicVolume/sfxVolume/
  masterVolume no IL (/tmp/sor4_il.txt). Talvez precise forçar esses RTPCs p/ valor audível.
- (C) **voz VIRTUAL**: se o volume master=0, a Wwise virtualiza a voz (silêncio). Consequência de A/B.
**PRÓXIMOS PASSOS:** 1) logar set_rtpc_value+set_state no wrapper (FWD atuais não logam). 2) grep IL por
musicVolume/get_master_volume/set_volume/Suspend/WakeupFromSuspend. 3) testar forçar volume.
**TODO:** áudio driver AUTOMÁTICO (não forçar SDL_AUDIODRIVER=pulse; deixar SDL escolher c/ fallback
pulse->alsa) — funciona com pulse hoje mas deveria ser auto p/ qualquer SDL.

**COMO CONTINUAR (infra pronta):**
- Device: **<device-ip>** root (ssh por chave, sem senha). Game dir: `/storage/roms/sor4-test`.
- ⚠️ SEMPRE `pkill -9 sor4host` antes de lançar. Lançar: `cd /storage/roms/sor4-test && SOR4_TEXSCALE=3
  SOR4_SHOT=300 nohup sh run_diag.sh >/dev/null 2>&1 &` (run_diag.sh já tem SDL_AUDIODRIVER=pulse +
  SDL_NO_SIGNAL_HANDLERS + mapping do controle).
- Wrapper: `~/nextos_ports_android/ports/sor4wwise/` -> `bash build.sh` gera libWwise.so (toolchain NextOS
  Amlogic-old) -> `scp libWwise.so root@<device-ip>:/storage/roms/sor4-test/host_pkg/libs/libWwise.so`.
  Fontes: src/wwise_native.c (wrapper/trampolins/AAsset/JNI/pump/NOPs), src/opensles_shim.c (SDL audio +
  logs PICO/ENQUEUE/RAWpeak). libWwise.real.so = a Wwise REAL do APK (já no device host_pkg/libs/).
- VERIFICAÇÃO SEM OUVIR: `ssh root@<device-ip> 'pactl list short sinks'` (RUNNING=tem stream) +
  `grep -E "RAWpeak|PICO|real init" /storage/roms/sor4-test/wwise.log` (RAWpeak>100 = Wwise produz som).
- ⚠️ LIMPAR no final: logs [opensles]/[wwise-native]/ENQUEUE/PICO/RAWpeak + os probes [PAD]/[HI]/[HB] do
  MonoGame + titleprobe do SOR4.dll. NOPs hardcoded são da v1.4.5.
- Estado git: COMMITADO (c552424 = áudio nativo toca/HDMI RUNNING; este handoff = a documentar).

### 🔊🏆🏆 NATIVO cont.13: ÁUDIO WWISE ORIGINAL na TUBULAÇÃO (HDMI sink RUNNING) — falta só o silêncio interno
A libWwise REAL do APK roda nativa via so-loader e PRODUZ SOM (motor Wwise original, música+SFX). Cadeia
de fixes finais (depois do "carrega mas init=0"):
1. **init=0 -> init=1**: native_wwise_init faz ~10 checks de subsistema em sequencia. Os 3 de PATH
   (AddBasePath@b.ne 0x12242c, 2o-check cbz 0x122438, SetBasePath@b.ne 0x122510) FALHAM no ambiente
   Android-fake (validam path asset-relativo) mas NAO sao necessarios (nosso AAssetManager_open le por
   path absoluto). FIX: NOPar esses 3 b.ne (default embutido no wrapper; achei via bisecao com WWISE_NOP).
   IO hook (CAkDefaultIOHookBlocking::Init) + InitAndroidIO (JNI) RODAM REAIS e sucedem.
2. **AAssetManager_open path duplicado**: a Wwise passa path absoluto (base+bank); meu shim prefixava
   g_bankbase de novo. FIX: se fn[0]=='/' usa direto.
3. **SDL audio nao inicializado**: opensles_shim chamava SDL_OpenAudioDevice mas o jogo (MonoGame) so
   inicia SDL de VIDEO. FIX: SDL_InitSubSystem(SDL_INIT_AUDIO) no ensure_audio_initialized + launcher
   seta SDL_AUDIODRIVER=pulse. -> SDL audio abre no PulseAudio (dev=2 OK).
4. **🔑 RETRY infinito do sink (slCreateEngine repetia, counter sempre=1)**: o callback do BufferQueue
   (avisa "buffer consumido, manda mais") so e disparado por opensles_shim_pump_callbacks(); no Android
   quem chama e a thread do OpenSLES, AQUI ninguem chamava -> Wwise mandava 1 buffer, esperava o cb que
   nunca vinha, timeout, resetava o sink (retry!). FIX: **thread de pump no wrapper** (pump_thread_fn,
   chama opensles_shim_pump_callbacks() @250Hz). -> sink ESTAVEL, ENQUEUE counter cresce continuo.
5. **🔑 SILENCIO (nao se ouvia)**: faltavam os **613 arquivos .wem** (musica+SFX STREAMED) que NAO
   foram extraidos do APK (so os bancos+xnb). A Wwise pedia ex 353312695.wem (5.4MB=tema do menu) ->
   AAsset FALHOU -> renderizava silencio. FIX: extrair `assets/*.wem` do APK (635MB) -> device gameassets/.
VERIFICACAO SEM OUVIR: `pactl list short sinks` = hdmi_real RUNNING (PulseAudio suspende
em silencio; RUNNING=som real) + ENQUEUE counter CRESCE (Wwise renderizando) + wem abre sem FALHOU.
ARQUIVOS: ports/sor4wwise/ (wrapper libWwise.so + src/wwise_native.c + opensles_shim.c modificado +
build.sh). libWwise.real.so (2.9MB do APK) no device host_pkg/libs/. Launcher run_diag.sh: SDL_AUDIODRIVER
=pulse. FALTA polir: as offsets de NOP sao da v1.4.5 (hardcoded); remover logs [opensles]/[wwise-native]/
ENQUEUE no final; confirmar SFX de gameplay no ouvido; load lento da fase (notado em teste).

### 🔊🟢 NATIVO — avanco cont.12: Wwise REAL CARREGA via so-loader (init=0 falta) [RESOLVIDO acima]
ports/sor4wwise/ = wrapper glibc libWwise.so (PLUGIN do .NET) que so-carrega a Wwise REAL do APK.
- Construtor: so_load(libWwise.real.so) + relocate + resolve(tabela combinada extra+gen) + init_array.
  **FUNCIONA**: "[wwise-native] libWwise REAL carregada OK" (so-loader OK p/ a Wwise C++ 2.9MB!).
- JNI: nosso host headless NÃO chama JNI_OnLoad. FIX: construtor chama `JNI_OnLoad(jni_shim fake_vm)`
  -> retorna 65542 (JNI 1.6 OK) + `native_android_preinit(fake activity 0x1000)`. AAssetManager_fromJava
  é chamado (cadeia JNI funciona). build: build.sh (toolchain NextOS Amlogic-old + -lSDL2, ld warning
  "bad subsection" do libSDL2 é inofensivo; .so sai OK c/ 21 exports native_wwise).
- ⚠️ MURO ATUAL: `native_wwise_init` real **retorna 0** (falha). Path: o jogo passa get_data_folder()
  VAZIO; substituí por gameassets/ no trampolim (SOR4_BANKDIR). MAS init=0 persiste e NENHUMA chamada
  AAsset_open acontece durante init -> falha é num SUBSISTEMA Wwise (memória/sound engine), NÃO no IO.
  RE: native_wwise_init@0x122350; em 0x122428 `cmp w0,#1;b.ne fail` apos `AddBasePath`(0x140704); em
  0x122438 `cbz x0,fail` apos `bl 0x256b24` que chama 0x1441fc(rpmalloc?) + lê global g_pAKPluginList+3520.
  PRÓXIMO: gdb arm64 no device (logar text_base no construtor p/ achar addr; breakpoint em text_base+offset)
  OU instrumentar mais shims (rpmalloc usa mmap/TLS bionic?) OU logar retorno de native_android_preinit +
  InitAndroidIO. Logs: opensles_shim slCreateEngine loga; wwise.log no device. Arquivos: ports/sor4wwise/
  src/wwise_native.c (wrapper+trampolins+AAsset+__sF+JNI), src/imports.gen.c (tabela; CORRIGIDO: faltava
  bloco includes + externs _fake + def dynlib_numfunctions + nomes SL_IID — new-port.sh tem esses BUGS).
- Wwise real em /tmp/apklib/lib/arm64-v8a/libWwise.so -> device host_pkg/libs/libWwise.real.so. Wrapper
  -> libWwise.so. Stub antigo backup: libWwise.so.stub.bak. Custom (referencia) backup nao guardado.

CAMINHO NATIVO (detalhes anteriores):
- libWwise.so REAL extraída do APK -> `/tmp/apklib/lib/arm64-v8a/libWwise.so` (2.9MB, exporta os 21
  native_wwise_*, NEEDED libOpenSLES/libandroid/liblog/libc/libm/libdl bionic, 134 imports UND).
- Scaffold gerado: `tools/new-port.sh libWwise.so sor4wwise` -> **ports/sor4wwise/** (98/134 auto-resolvidos:
  48 passthrough libc/libm + 34 pthread_fake + 5 opensles_shim + 8 android_shim + 3 cxx/log; 36 UNKNOWN =
  quase todos libc/libm padrão (acosf/feof/syscall/vfprintf...) que a glibc TEM via dlsym(RTLD_DEFAULT) +
  _chk/__system_property_get/android_set_abort_message que bionic_shims.c JÁ cobre + liblog stub + __sF).
- FALTA p/ o nativo: (1) converter ports/sor4wwise de EXE-loader -> PLUGIN .so (constructor so_load +
  trampolins native_wwise_* -> so_find_addr); (2) resolver os 36 UNKNOWN (fallback dlsym RTLD_DEFAULT +
  wire bionic_shims + __sF stride 0x54); (3) AAssetManager_*/AAsset_* (android_shim) LER OS BANKS de
  gameassets/ (é como a Wwise carrega Init/Core/Generic/Music.bnk); (4) OpenSLES (slCreateEngine_shim +
  SL_IID_* data) -> opensles_shim -> SDL/PulseAudio; (5) build .so c/ aarch64-linux-gnu-gcc (igual stub);
  (6) deploy + depurar no device (reloc C++/RTTI/exceptions, TLS/canary bionic, thread de áudio Wwise).
  Backup do stub: host_pkg/libs/libWwise.so.stub.bak. P/Invoke do jogo = DllImport "Wwise" -> libWwise.so.

### 🎮 CONTROLE — feedback do porter APÓS o fix de mapping (AINDA ABERTO)
- Mapping explícito ATIVO (gcName mudou). MAS: "A e B continuam virando START" + "analógico pra baixo
  pula 2 casas" (input duplicado). Logo: o assignment `a:b2,b:b1,...,start:b9` NÃO casa o layout FÍSICO
  do pad do porter (unidade não-padrão). Pedido: "se vire com logs/pad virtual, não precisa eu apertar".
- TEORIA: os botões físicos A/B dele emitem evdev 296/297 (que mapeei p/ back/start b8/b9) -> viram pause.
  Square(attack=X=b3=291) funciona -> ancora. DUPLO no dpad: mapeei dpad no hat0 + stick a0/a1; o pad dele
  manda movimento em a0/a1 (analógico) E hat -> 2x. FIX dpad: usar dpad DIGITAL só nos eixos
  (dpup:-a1,dpdown:+a1,dpleft:-a0,dpright:+a0) SEM leftx/lefty e SEM hat (igual o built-in que era "perfeito").
- P/ achar o layout SEM o porter: pad VIRTUAL vpadd clona evdev exato -> unbind pad real (slot0) -> apertar
  cada evdev 288-299 + screenshot/efeito no menu (confirma/cancela/move/pausa) -> deduzir evdev->ação.
  evcap.py (lê /dev/input/event2 cru) capturou VAZIO (ninguém apertou na janela). Game ACEITA TECLADO tb.

### 🖼️ CERCAS/OBJETOS BRANCOS no gameplay (task #3, ABERTO) — confirmado que persiste.
Provável: textura específica (cerca/chain-link/objetos) com alpha/blend/formato que o Mali renderiza branco
(alpha-test? PNG transparência decodificada como branco? material aditivo?). Investigar textura/material em
decors/main_campaign/stage_1. Resto do cenário ~100% correto.

### MISSÃO ATUAL: VÍDEOS REAIS + SOM NATIVO (Wwise real) + mapeamento controle + cercas brancas
- **SOM**: jogo usa **Wwise** (Init/Core/Generic/Music.bnk em gameassets + .wem). Hoje libWwise=STUB=mudo.
  Meta: áudio REAL. APK tem libWwise.so arm64. Device=PulseAudio (Mali-450). Investigar binding P/Invoke
  do `BeatThemAll.audio` (post_event/set_music_manager_state) → decidir: usar libWwise real (sink?) ou
  decodificar .wem (Vorbis/Opus) e tocar via OpenAL/SDL_mixer. Ver memória DYSMANTLE/NFS (áudio so-loader).
- **VÍDEOS**: .mp4 (H.264) em gameassets/videos/ (game_intro, stageN_intro...). Mali-450 sem decode HW
  fácil. Precisa player próprio (ffmpeg/libavcodec SW → texturas GL) plugado no MonoGame como o tipo
  custom `SuperVideoPlayer`/`platform.video_*`. Grande. Tentar "o mais original possível".
- **LOAD lento da fase**: stage_1 decodifica MUITOS XNB/ASTC no UI thread (~9 tex/s). Otimizar depois.

---

## Fatos confirmados do APK (FASE 0/1)
APK fonte: `/home/root/Downloads/Streets-of-Rage-4-v1.4.5-unlocked-apkvision(1).apk`
(1,9 GB, 27.234 arquivos).

- Engine = **MonoGame + .NET-for-Android (MonoVM)**. NÃO é Unity, NÃO é FNA.
- Libs nativas: `libmonosgen-2.0.so`, `libmonodroid.so`, `libxamarin-app.so`,
  `libassemblies.arm64-v8a.blob.so` (assembly store), `libSystem.*.Native.so`,
  `libopenal.so`, `libWwise.so`, `libfreetype.so`, `libharfbuzz.so`,
  `libEOSSDK.so`, `libpairipcore.so` (PAIRIP anti-tamper), `libstub.so`.
- Assembly store com **~99 PE (MZ) em claro** → assemblies extraíveis.
  Nomes vistos: `MonoGame.Framework.dll`, `System.Private.CoreLib.dll` (.NET moderno/8),
  `Mono.Android.Runtime.dll`, e o jogo provável: `StandaloneTypeModel.Android.Retail.dll`
  (+ DLLs ofuscadas curtas: `Qe.dll`, `Ts.dll`, etc.).
- Assets: `.xnb` (conteúdo MonoGame) em `assets/`, + banks Wwise.

## Estratégia (decidida)
**NÃO so-loader** (lógica é C# gerenciado, não nativa). Caminho =
**runtime .NET nativo + MonoGame DesktopGL/GLES**:
extrair assemblies → rodar em .NET 8 arm64 no device → host próprio (Program.Main sem
AndroidGameActivity) → MonoGame DesktopGL → gl4es p/ GLES2 no Mali-450 → stubar
Mono.Android/EOS/Helpshift/Billing/pairip.

## Device <device-ip> (recon FASE 0)
- Mali-450 Utgard, **GLES2-only**, **fbdev** (/dev/fb0, /dev/fb1, sem /dev/dri).
- Kernel 3.14.79 EMUELEC aarch64, **glibc 2.43** (moderno — bom p/ .NET 8), 4 cores.
- `/storage` = 996 MB livres (NÃO usar p/ dados). **`/storage/roms` (p3) = 21 GB livres** → usar.
- GL: `/usr/lib/libMali.m450.so` (=libMali.so=libEGL/libGLESv2). **gl4es: `libEGL_gl4es.so.1`**.
- SDL: `libSDL2-2.0.so.0.3200.69` (2.32, provável mali) + SDL3. `SDL_VIDEODRIVER=mali` p/ fbdev.
- gptokeyb em `/storage/roms/ports/PortMaster/`.
- **Sem runtime .NET/Mono no device** (só LÖVE em PortMaster/runtimes). → prover runtime nós mesmos.
- SSH: `root@<device-ip>` (sem senha via chave já configurada). Regra: nunca relançar sobre
  instância viva (matcher por /proc/PID/exe — ver `ports/cuphead/run.sh`).

## Convenções de launcher (do port Cuphead, reaproveitar)
- GAMEDIR = `/storage/roms/<jogo>`; launcher device-aware (fbdev→mali / kmsdrm→x5m).
- fbdev: `SDL_VIDEODRIVER=mali LD_LIBRARY_PATH=/usr/lib`, EGL real do Mali.
- Matar instância viva por inode antes de lançar; `nohup ./bin > run.out 2>&1 &`.

---

## GO/NO-GO gates
- [ ] **A** assemblies extraíveis em IL legível
- [ ] **B** contexto GL a partir de C# no device (gl4es)
- [ ] **C** jogo sobe sem dep Android fatal
- [ ] **D** primeira imagem (objetivo central)

## Riscos abertos
- Runtime .NET 8 arm64 em kernel 3.14 (glibc 2.43 ajuda; testar; fallback self-contained/Mono).
- Acoplamento Mono.Android / pairip dentro do código do jogo.
- Shaders `.xnb` (DX bytecode) → MojoShader/GL (pode exigir recompilar conteúdo de Effect).

---

## 🎯 CAUSA-RAIZ do "trava aos 100%" ACHADA NOS LOGS (2026-06-16 cont.5) — NÃO é OOM/hang
**Dado real** (device .127 `run.log` + `progress.log` após reboot): o preload chega a 100%,
o `ScreenManager` entra na **`StartGameVideoScreen`** (vídeo de abertura) e morre com **exceção
gerenciada** (NÃO hang, NÃO OOM — `progress.log`: avail=147MB, swapused=250MB, rss caindo p/ 91MB
quando morreu = morte por exceção, não falta de memória):
```
[host] EXC: System.TypeLoadException: Could not load type
  'Microsoft.Xna.Framework.Media.SuperVideoPlayer' from assembly 'MonoGame.Framework'
   at CommonLib.platform.video_draw()
   at BeatThemAll.MetaGame.StartGameVideoScreen.draw()
   at BeatThemAll.MetaGame.ScreenManager.draw() / program.draw / StandaloneGame.Draw / Game.Tick
```
**CAUSA**: `SuperVideoPlayer` é um tipo CUSTOM do fork MonoGame Android do SoR4 (player de vídeo
das cutscenes/logos). Meu MonoGame buildado do fonte 3.8.4 NÃO tem esse tipo → `TypeLoadException`
ao primeiro Draw da `StartGameVideoScreen` → game-loop morre.
**PLANO DE FIX** (a confirmar via IL de `platform.video_draw` + `StartGameVideoScreen`):
ou (1) adicionar tipo `SuperVideoPlayer` stub no meu MonoGame.Framework (namespace Media) que
reporta vídeo "terminado" na hora → a tela avança sozinha pro menu; ou (2) Cecil-patch
`platform.video_draw`→no-op + forçar a tela a avançar. Objetivo: pular a intro e cair no MENU.
NOTA: memória/TEXSCALE/swap NÃO eram o problema dos 100% — descartado com dado.

## 🎯 cont.9 — cadeia de fixes ATÉ o menu desenhar (cada muro = 1 NRE gerenciado limpo)
Depois do SDL fix + skipvideo + verstub, a sequencia de muros no caminho do menu (todos resolvidos):
3. **FileNotFoundException fonte** `storage/roms/sor4-test/host_pkg/NotoSans-Bold.ttf` (path CWD sem
   "/"): a GUI do menu DESENHA texto e pede a fonte por path absoluto-malformado. FIX: AssetBridge.Open
   (build/bridge/AssetBridge.cs — NAO a copia em port/tools/bridge!) ganhou fallbacks: path absoluto
   ("/"+name) e **basename em Root**. Recompilar build/bridge -> SOR4Bridge.dll -> host_pkg. Tb copiei
   *.ttf/*.otf de gameassets/ pro host_pkg/.
4. **render_text_to_texture NRE** (SharpFont): rasterizacao de glifo. SharpFont.Core estava STUBADO
   (19968B). FIX: usar o REAL `build/extract/asm/asm_025.dll` (29696B, SharpFont.Core v1.0.0.0,
   pinvokeimpl("freetype")) como host_pkg/SharpFont.Core.dll -> resolve /usr/lib/libfreetype.so.6 (existe!).
   Texto passa a rasterizar de verdade.
5. **EOSManager.PollMessage NRE**: update loop -> online.receive_state_changes -> EOSManager.PollMessage
   -> get_p2pHandle -> platform(null).GetP2PInterface(). EOS nao inicializado. FIX: noopm
   EOSManager.PollMessage -> return null (sem mensagem). Rede nao precisa pro menu single-player.
PIPELINE SOR4.dll AGORA: patchgam -> noopm(AndroidServices.*+save) -> skipvideo -> verstub ->
noopm EOSManager.PollMessage. Deploy extra: SharpFont.Core REAL + fontes .ttf/.otf + SOR4Bridge novo.
Provavel proximo: mais stubs EOS/online no update loop (mesmo padrao, noopm) + audio (Wwise stub=mudo)
+ input (gptokeyb). Avanco confirmado em teste real. Foco seguinte: SOM + CONTROLES.

## 🎮🟡 cont.10 — MISSÃO CONTROLES (objetivo da PRÓXIMA sessão): passar da tela de TAP + todos os botões
### OBJETIVO
1) Passar da tela de título ("PRESS ANY BUTTON" / "tap anywhere" — é build MOBILE).
2) Todos os botões do controle funcionais no menu/jogo.
👉 ESTUDAR como os ports **NFS** (`ports/nfs`, saga de touch/"tap anywhere" + nativeTouchScreenEvent +
   IsSameObject dispatch — ver memória project_nfs_most_wanted_soloader_mali450) e **DYSMANTLE**
   (`ports/dysmantle`, GameActivity so-loader input) resolveram input — foi DIFÍCIL nos dois.
   Também `ports/sonicmania`, `ports/bully` usam gptokeyb (gamepad→teclado) — padrão pronto.

### O QUE JÁ SABEMOS (com DADO, não chute)
- **O gamepad CHEGA perfeitamente no MonoGame.** Pad = USB genérico **0810:0001**, SDL GUID REAL
  `0300605b100800000100000010010000` (bytes [2-3]=605b = CRC16 do nome; meu mapeamento manual usava
  `03000000...` SEM CRC → era IGNORADO). SDL já reconhece via mapping embutido "Twin USB PS2 Adapter"
  (IsGameController=1). Sonda SDL: `/tmp/sdlprobe` (dlopen libSDL2).
- Logs `[PAD]` (instrumentei MonoGame: GamePad.SDL.cs, Joystick.SDL.cs, SDLGamePlatform.cs — REMOVER no
  final) provam in-game: `JoyDeviceAdded → Joystick.AddDevice isGameController=1 → GamePad.AddDevice
  slot=0 total=1`; e `GetState idx=0` retorna A/B/X/Y/Start/Back/ombros/**dpad** TODOS corretos quando
  se aperta. Ou seja MonoGame↔SDL↔pad = 100% OK.
- Cadeia do jogo: `input.update` → `platform.update_game_pad_state_array` → `platform_strict.
  get_game_pad_state(0)` → `GamePad.GetState(PlayerIndex.One)` (mapeia botões certos: A=0x1000 etc) →
  `GamePadState_s[]` (current/previous). `TitleScreen.handle_input`: `input.is_any_button_just_pressed
  (true)` && MetaGameConfig==null → `start_load_save_and_config`; e um path gated por flag →
  `start_game` quando `load_save_and_config_is_finished()`. `is_any_button_just_pressed` =
  `is_any_button_just_pressed_controller` (borda: current.is_pressed && !previous, botões enum 1,2,3,4,
  9,10) OU teclado (Enter=13/Space=32/Esc=27/Backspace=8 + currentPressedKeyList).
- **MAS o título NÃO avança** apertando o pad. `start_load_save_and_config` NUNCA logou.
- **TEORIA DO PORTER (forte, do Sonic Mania):** título mobile espera um **TAP NA TELA**; o controle só
  funciona DEPOIS do tap. SoR4 é o mesmo caso provável. NOSSO MonoGame **NÃO alimenta TouchPanel**
  (sem mouse→touch, sem SDL_FINGER) → toque ausente → título preso.

### PRÓXIMOS PASSOS SUGERIDOS (a próxima sessão decide)
A) **Confirmar a causa com 1 instrumentação Cecil decisiva**: logar o retorno de `is_any_button_just_
   pressed` + entrada de `start_load_save_and_config` + `load_save_and_config_is_finished`. Se
   is_any_button_just_pressed=FALSE com botão apertado → é a borda/ mapeamento do pad no jogo. Se TRUE
   mas start_game não vem → é o load de save/config (no-opamos saves: conferir start_load_save_and_
   config / load_save_and_config_is_finished não travam). Ferramenta: port/tools/probe (modelo de sonda).
B) **TAP sintético**: (b1) habilitar mouse→touch no MonoGame (TouchPanel.EnableMouseTouchPoint + feed
   no SDLGamePlatform a partir de Mouse/SDL_FINGER) e injetar um clique; (b2) ou Cecil-patch no
   `TitleScreen`/`input` pra tratar um botão do pad como o "tap" (advance). Ver `merge_touch_input_into
   _gamepad_state`, `is_touch_action_just_pressed`, `get_lastPressWasTouch`.
C) **gptokeyb→teclado** (caminho Sonic Mania, JÁ MEIO MONTADO): criei `/storage/roms/sor4-test/sor4.gptk`
   + `run_gptk.sh` (a=enter,b=esc,start=enter,dpad=setas,x=space). gptokeyb roda MAS não faz "grab" do
   pad → SDL ainda abre como controller (leitura dupla; A às vezes lê grudado=1). FALTA: confirmar se
   Enter via gptokeyb avança o título; se sim, talvez setar `SDL_GAMECONTROLLER_IGNORE_DEVICES=0x0810/
   0x0001` p/ MonoGame NÃO abrir o pad (só gptokeyb→teclado, sem leitura dupla). run_gptk.sh ESQUECEU
   SOR4_TEXCACHE (re-decodifica do zero; re-adicionar: `export SOR4_TEXCACHE=/storage/roms/sor4-test/texcache`).

### ESTADO/ARTEFATOS
- Pipeline SOR4.dll (reproduzir de /tmp/SOR4.bak = patchgam+noopm já aplicados): skipvideo → verstub →
  noopm EOSManager.PollMessage. Tools em port/tools/{skipvideo,verstub,probe,noopm} (dotnet build -c Release -o bin).
- Deploy: SharpFont.Core REAL (build/extract/asm/asm_025.dll) + fontes *.ttf/*.otf + SOR4Bridge (build/
  bridge, com fallback de path) + MonoGame.Framework com [PAD] logs (/tmp/mgout). Tudo em host_pkg.
- ⚠️ Logs [PAD] em MonoGame (3 arquivos) e o log de GetState em PlatformGetState = REMOVER antes do final.
- Launchers no device: run_diag.sh (minidump+EXITCODE+SHOT, SDL_NO_SIGNAL_HANDLERS+SDL_GAMECONTROLLER
  CONFIG) e run_gptk.sh (gptokeyb). SOR4_TEXSCALE=3.

## 🏆🏆🏆 MENU PRINCIPAL NA TELA (2026-06-16 cont.9) — MARCO
Screenshot /tmp/sor4menu.png: TELA DE TITULO do SoR4 renderizando COMPLETA (logo + arte do elenco +
TEXTO embaixo rasterizado via SharpFont/FreeType real). Estavel, VIVO, SHOT f=4020 sem crash.
Pipeline final SOR4.dll: patchgam -> noopm(AndroidServices.*+save_save_game+save_config) -> skipvideo
-> verstub(get_version_identifier="1.4.5") -> noopm(EOSManager.PollMessage). Deploy: SharpFont.Core
REAL (asm_025.dll) + fontes .ttf/.otf no host_pkg + SOR4Bridge com fallback de path + SDL_NO_SIGNAL_
HANDLERS=1 no launcher. SOR4_TEXSCALE=3. Launcher: run_diag.sh.
FALTA: CONTROLES (navegar menu) + SOM (Wwise stub=mudo) + polish/empacotar.

## 🎯 cont.8 — DEPOIS do SDL fix: 2 bugs Android-stub na transição pro menu (debug com sonda Cecil)
Sequência de muros DEPOIS de SDL_NO_SIGNAL_HANDLERS (cada um virou exceção GERENCIADA legivel):
1. **video_draw NRE** (videoPlayer null): a tela de video de abertura. FIX: `skipvideo` (Cecil tool,
   port/tools/skipvideo) patcha `program::reset_game` p/ criar `TitleScreen` no lugar de
   `StartGameVideoScreen` (intro nao toca no Mali mesmo; destino pos-intro e o menu). 1 site.
2. **TitleScreen.update_gui NRE**: parecia guiInstance null. Criei sonda Cecil (port/tools/probe)
   que loga no `GameScreen::update_gui` o `guiDataRef.yaml_serialize()` (path) + thread.
   RESULTADO: `[PROBE] path=gui/menus/gui_title_screen thr=main` → path VALIDO, GUI CARREGA OK
   (guiInstance NAO era null!). A stack real (com base expandida) revelou o verdadeiro NRE:
   `CommonLib.utils.get_version_identifier` → chama **APIs Android** (Application.Context.
   PackageManager.GetPackageInfo → VersionName/LongVersionCode) que sao STUB → null → NRE ao
   formatar a string de versao exibida no menu. FIX: `verstub` (Cecil tool, port/tools/verstub)
   substitui o corpo de `get_version_identifier` por `return "1.4.5"`.
NOTA: reset_game roda num ThreadPool worker (PreloadingScreen.BeginState via Task) -> excecoes la
sao UNHANDLED (abort 134); no game-loop (main thread) sao capturadas pelo Host.Main (EXC, exit 0).
Asset loading (incl. GuiNodeData do bigfile) FUNCIONA em qualquer thread (descartado problema de
thread/memoria/desserializacao com dado). Pipeline SOR4.dll agora: patchgam -> noopm -> skipvideo
-> verstub. LICAO: telas do menu chamam APIs Android (versao, etc) -> stubar/retornar constantes.
Ferramenta-chave de debug: sonda Cecil que loga campo+thread no ponto exato (sem dotnet-dump, que
nao faz cross-arch x64->arm64; gdb arm64 do device pro PC nativo).

## 🎯 cont.7 — CAUSA DO SIGSEGV ACHADA: handler de sinal do SDL3 × CoreCLR (NÃO era memória)
**Refutado memória com dado**: scale-3 + 3GB swap → crashou IGUAL (swapused=41MB de 3GB, avail=195MB).
Crash DETERMINÍSTICO, mesmo IP nas 2 runs. createdump rotula a thread errada (handler em wait4).
**gdb arm64 NO DEVICE no core (`coredump.5132`) reconstruiu os frames de sinal** e mostrou a verdade:
```
#13 0x7f61c10b2c           <- FALHA ORIGINAL (mem anonima = codigo JIT) -> SIGSEGV
#12 <signal handler called>
#11 libSDL3.so.0 ...       <- handler de SIGSEGV do SDL3 INTERCEPTOU
#10 raise()                <- e re-disparou
#8  <signal handler called> -> libcoreclr -> wait4 (createdump)
```
**CAUSA**: SDL3 instala handler de SIGSEGV no `SDL_Init` (dentro de game.Run(), DEPOIS do CoreCLR).
O CoreCLR usa SIGSEGV LEGITIMO (null-check, write-barrier GC, stack-probe) e trata internamente
continuando. O SDL rouba o handler e ao pegar esse SIGSEGV chama `raise()` -> mata um AV recuperavel.
Por isso era deterministico (um caminho especifico de codigo dispara um AV que o CoreCLR trataria).
**FIX**: `SDL_NO_SIGNAL_HANDLERS=1` no launcher (run_diag.sh, apos SDL_VIDEODRIVER). Deixa o CoreCLR
tratar. Se era null-check legitimo -> jogo segue; se null-ref real -> vira NullReferenceException
gerenciada limpa (stack legivel) em vez de crash nativo mudo.
LICAO GERAL (todo port .NET/CoreCLR sob SDL): SEMPRE setar SDL_NO_SIGNAL_HANDLERS=1.
Ferramentas: dotnet-dump do host x64 NAO analisa core arm64 (cross-arch nao suportado) -> usar
gdb arm64 do DEVICE no core (reconstrucao de signal frame revela o PC real da falha).

## 🧩 cont.6 — SuperVideoPlayer RESOLVIDO; novo muro = SIGSEGV na carga final (~97.6%) por MEMÓRIA
**Fix do vídeo aplicado e validado**: criei `MonoGame.Framework/Media/SuperVideoPlayer.cs` (stub que
reporta vídeo "terminado": State=Stopped, PlayPosition=Duration=1s → `platform.video_is_finished()`=true
no 1º frame → StartGameVideoScreen avança sozinho pro TitleScreen) + `Graphics/SpriteOESBatch.cs`
(SpriteBatch trivial p/ o `newobj` em video_start). Buildado (`/tmp/mgout`, 0 erros) e deployado.
RESULTADO: `TypeLoadException` SUMIU; jogo renderiza a tela de loading com arte real ("Loading 97.6%")
e o game-loop roda (SHOT f=120..840).

**NOVO MURO (dado real do crashreport)**: morre em ~97.6% com **EXITCODE=139 = SIGSEGV (signal 11)**,
SEM exceção gerenciada. createdump: `coredump.4469` + `.crashreport.json`. Stack da thread (managed):
`libc (img+0x83aa8) ← libcoreclr (img+0x60eb10..0x63b3d8)` — **NÃO tem driver Mali** → não é GL.
ExceptionType=0x20000000. No instante: swap subindo (298/511MB), avail=175MB, RSS 191→63MB (GC grande
antes de morrer). Assinatura de **exaustão de memória** (heap cresce com grafos protobuf dos assets de
gameplay; runtime tenta crescer/mmap sob thrash → memset/memcpy falha → SIGSEGV).
DESCOBERTA: o `swap2g` (1GB) NÃO estava ativo pós-reboot (só swapfile 511MB) → virtual era só ~1.34GB.

**EXPERIMENTO EM CURSO**: swapon swap2g + criei swap3 (1.5G) → swap total 3071MB + 832MB RAM ≈ 3.9GB.
Relancei com **SOR4_TEXSCALE=3** (pedido; reduz RAM RETIDA das texturas GL; cache scale-3 é novo
→ run mais lenta, re-decodifica ASTC do zero — NÃO matar). Medir se passa de 97.6%→100%→[SVP] Play→menu.
Launcher diagnóstico: `/storage/roms/sor4-test/run_diag.sh` (minidump on, EXITCODE no log, SHOT).
Se passar = memória confirmada. Se crashar igual = não é só RAM retida (subir p/ analisar managed stack
do minidump via dotnet-dump + DAC, ou stack-overflow no deserializer).

## 🏆🏆 IMAGENS REAIS + jogo boota completo (2026-06-16 cont.4)
**Confirmado: LOGO do SoR4 com TEXTURA REAL na tela** (ASTC decodificado). Estado:
- Boot 100%: render + assets + OnDeviceCreated + 1ºDraw + shaders + AfterFirstDraw + **bigfile
  protobuf desserializa** + program.initialize + carrega assets de gameplay (background thread).
- **ASTC real**: `libsor4astc.so` (astcenc 5.0.0 decompressor NEON arm64, sem LTO) + Texture2DReader
  decode (detecta bloco pelos dados). Imagens reais no Mali-450 (que não tem ASTC nativo!).

**Fixes-chave desta rodada** (host replica o que MainActivity.OnCreate fazia, que eu bypasso):
- `program.static_init()` (cria typeModel/StandaloneTypeModel protobuf).
- `reflection.delegate_serialize/deserialize/deep_clone` wired (engine de serialização protobuf).
- `AndroidServices.*` (38 métodos) NO-OP (Google Play/cloud/sign-in - crashavam em stubs null).
- `platform.save_save_game` + `save_config` NO-OP (escrita inicial de save/config; data_folder="").
- Ferramentas Cecil: `noopm` (no-op métodos/classe), `addfwd` (forward de tipo), `injlog` (log de
  entrada/arg). `patchgam` (get_AssetManager→bridge).

**Assets**: 25.292 arquivos (.xnb/png/fonts, sem .wem) em `/storage/roms/sor4-test/gameassets`
(SOR4_ASSETS aponta lá). .wem (áudio) vão por Wwise (stub) — não precisam em disco.

**Pendências conhecidas** (polish, não-bloqueantes):
- **Texto preto**: Effect custom `shader/text.xnb` (GLSL ES, 2 samplers `ps_s0/ps_s1` estilo
  MojoShader) — binding de sampler/uniform do Effect custom no meu MonoGame. Logo usa SpriteEffect
  padrão (OK). Investigar EffectReader/ConstantBuffer mapping.
- Áudio (Wwise stub - silêncio), input (gptokeyb), perf, empacotar.

**Pipeline de patch do SOR4.dll** (ordem): patchgam (get_AssetManager) → noopm (AndroidServices.* +
platform.save_save_game + save_config). Host: static_init + 3 delegates + set_as_main_thread.

## 🏆 GATE D ALCANÇADO: PRIMEIRA IMAGEM NA TELA + jogo inicializa fundo (2026-06-16 cont.3)
**CONFIRMADAS imagens na TV.** O jogo agora roda MUITO fundo na inicialização. Cadeia que
JÁ FUNCIONA (boot → render → init):
- .NET 9 CoreCLR + MonoGame GLES + SDL3-mali → contexto + GraphicsDevice ✅
- Asset loading via bridge (get_AssetManager patchado p/ AssetBridge.GetAssets) ✅
- Texturas .xnb carregam (ASTC → placeholder RGBA cinza) ✅
- OnDeviceCreated + PreloadingScreen.Initialize ✅
- Game loop RODANDO: Initialize → 1º Draw → shaders SpriteBatch (GLSL ES) compilam+linkam ✅
- `PreloadingScreen.AfterFirstDraw`: read_bigfile (DeflateStream OK) + program.initialize +
  audio.initialize + start_background_loading ✅
- bigfile deserializando (`reflection.bin_deserialize` de `MetaGameConfigData`) — **crash aqui**.

**Fixes desta rodada** (todos em port/):
- `System.Runtime` forward de `DefaultDllImportSearchPathsAttribute` (`port/tools/addfwd/`).
- Stubber agora **STRIP** de `DefaultDllImportSearchPaths`/`UnmanagedFunctionPointer` attrs
  (o retarget corelib→System.Runtime quebrava reflection de atributos no AfterFirstDraw).
- Texture2DReader: ASTC (fmt>=96) → placeholder RGBA (`port/monogame-gles-patches/Texture2DReader.SOR4.cs`).
- ALC resolver restrito (System.*/Microsoft.* → runtime).
- injlog ganhou `@metodo` (loga arg string) + `addfwd` tool.

**MURO ATUAL**: `bin_deserialize(MetaGameConfigData)` → `ldr x0,[x0+8]` com **x0 = ponteiro LIXO**
(não null — runtime converteria em NRE; é AV de verdade). Deserializador binário do jogo produz
referência corrompida. 
**HIPÓTESE PRINCIPAL**: jogo é **MonoVM** (Android, libmonosgen) e rodo em **CoreCLR**. O serializador
pode depender de comportamento do Mono (layout/ordem de campos/unsafe). → testar **.NET 9 com
`<UseMonoRuntime>true</UseMonoRuntime>`** (runtime Mono desktop) — pode casar o comportamento.
Alternativas: debugar o formato do bin_deserialize; ou rodar com DOTNET_gcServer/layout flags.
**Próximo passo recomendado**: publicar host com UseMonoRuntime e testar se passa a deserialização.

## GATE C → GATE D: CRASH NATIVO RESOLVIDO! Agora é só formato ASTC (2026-06-16 cont.2)
**MARCO**: o segfault nativo no asset loader FOI EMBORA. Causa-raiz: `asset_cache.get_AssetManager`
fazia `new AssetManagerWrapper(Game.Activity.Assets)` e o **`callvirt Context::get_Assets()`** no
AndroidGameActivity (Mono.Android stub uninitialized) crashava nativo no slot de vtable. 
**FIX (Cecil)**: `port/tools/patchgam/` reescreve `get_AssetManager` no SOR4.dll: troca
`Game.Activity.Assets` (call get_Activity + isinst MainActivity + callvirt get_Assets) por
**`call SOR4Bridge.AssetBridge.GetAssets()`** (nopa os 2 anteriores). Tudo downstream
(AssetManagerWrapper.Open/List → AssetManager.Open/List já bridgeados no stub) funciona.
Pinpoint via `port/tools/injlog/` (injeta log de entrada em métodos do jogo) →
sequência: get→try_get_asset_in_cache→load_asset→xna_load_asset→get_AssetManager→(crash).

**ESTADO ATUAL**: o jogo agora **CARREGA a textura via MonoGame**:
`[asset] gui/mobile/left_filler.xnb` → `ContentManager.ReadAsset` → `GetContentReaderFromXnb` →
**`NotSupportedException: SurfaceFormat '98' is not supported`** (gerenciado, capturado).
Reflection do .NET 9 OK (testado isolado, `build/reftest`). 

**NOVO MURO = ASTC**: formato 98 = família ASTC (enum tem `Astc4X4Rgba=96`; 98 = outra variante,
provável 6x6/8x8 — fork MonoGame do dev tem mais ASTC). **Mali-450 NÃO suporta ASTC** (extensões só
`GL_OES_compressed_ETC1_RGB8`, sem ASTC/ETC2/DXT/PVRTC). Mesmo problema do GunGodz.
**Opções p/ resolver (PRÓXIMO PASSO)**:
1. **Decode ASTC→RGBA8 em runtime** no MonoGame (Texture2D/GetGLFormat): detectar ASTC → decodificar
   p/ RGBA8 (Color) → glTexImage2D. Precisa decoder ASTC (C# puro embutido, ou astcenc nativo arm64
   P/Invoke). Limpo, escala, mas RGBA8 infla memória/VRAM.
2. **Converter assets offline** ASTC→RGBA8/4444 (receita GunGodz: astcenc-native + reescrever .xnb).
   Reprocessa 1.9GB, infla muito (RGBA8 ~4-8GB). 
3. **Placeholder (interim p/ 1ª imagem)**: textura dummy (cor) pro formato ASTC → ver o LAYOUT da
   tela de preload renderizar (prova pipeline end-to-end), depois implementar decode real.
Recomendado: (3) p/ a 1ª imagem rápida, depois (1) decode real (RGBA4444 p/ economizar).
Notar: Mali-450 só comprime ETC1 (RGB, sem alpha) → texturas com alpha = RGBA8/4444 sem compressão.

## GATE C — boot: crash isolado no LOADER (reflection) — 4 fixes aplicados (2026-06-16 cont.)
**Fixes novos aplicados (todos necessários, em port/)**:
1. **`utils.set_as_main_thread()`** chamado pelo host no boot (senão `is_main_thread()`=false →
   asset loader vai pro caminho de background thread). Host: `port/host/Program.cs`.
2. **Stub `libWwise.so`** (aarch64, 23 símbolos `native_wwise_*` no-op) — `port/tools/wwise_stub.c`.
   Vai em `libs/`. (P/Invokes "Wwise" do jogo resolvem; áudio real é FASE 5.)
3. **`AssetManager.List` bridgeado** (Exists do jogo usa List(dir)) + **`get_Assets` override** no
   `AndroidGameActivity` + Activity via `GetUninitializedObject` (ctors stub são no-op inválidos).
4. **GL no-op fallback**: `OpenGL.SDL.cs.LoadFunction` retorna `sor4_gl_noop` (em libWwise) p/
   funções GL ausentes no Mali GLES2 (loga `[GLMISS]`), em vez de null→`blr null`. 
   GLMISS no startup: glMakeCurrent, glClearDepth, glDepthRange, glPolygonMode, glDrawBuffers,
   glReadBuffer, glDrawBuffer, glBlitFramebuffer, glGen/Begin/EndQuery, glBlend*Separatei,
   glGetTexImage, glTexImage3D, glMapBuffer/Unmap, glDrawElementsInstanced, glVertexAttribDivisor.

**PONTE DE ASSETS 100% PROVADA**: teste isolado abre `gui/mobile/left_filler` (.xnb, len=181939)
e `title_screen` (len=803079) via Context slot. set_as_main_thread OK (thread "main").

**Crash que resta** (após TODOS os fixes): ainda segfault nativo (139) logo após `SB ctor COMPLETO`,
dentro do `OnDeviceCreated`→`PreloadingScreen.Initialize`→`asset_cache.get<TextureProxy>` →
`load_asset`→`xna_load_asset`. 
- gdb: `movk x2,#0x7f,lsl#32 ; ldr x2,[x2] ; blr x2` → **chamada via ponteiro de função carregado
  de um slot, que é lixo** (NÃO null→seria NRE). Stack corrompida (não unwind).
- NÃO é: GL ausente (no-op'd, nenhum GLMISS durante o load), Wwise (stub, e texture não chama),
  asset/Open (bridge provado), codegen (TieredComp/R2R/QuickJit=0 não mudam), exceção gerenciada
  (try/catch em OnDeviceCreated NÃO pega → puro nativo).
- `xna_load_asset` usa **reflection**: `genericGetMethodInfo.MakeGenericMethod(type).Invoke(...)`
  (`MethodBase.Invoke` no .NET 9 usa InvokeStub emit-based). Forte suspeita: a invoke-stub ou um
  `delegate*`/function-pointer no path de loading aponta p/ lixo neste runtime.
- Teste host de `asset_cache.get<TextureProxy>` SEM device → **NullReferenceException** gerenciada
  (provável Device null). Com device (no jogo) vai além → crash nativo. Falta testar COM device.

**PRÓXIMO PASSO**: 
- Reproduzir `asset_cache.get<TextureProxy>` COM GraphicsDevice presente (criar device no host ou
  hookar no 1º Update do MonoGame) p/ pegar o ponto exato.
- Investigar a reflection `MethodBase.Invoke`/`MakeGenericMethod` no .NET 9 self-contained arm64:
  testar programa mínimo de invoke genérico no device; tentar feature-switch p/ desabilitar
  emit-invoke (`System.Reflection` config) se for bug do InvokeStub.
- Verificar `asset_cache.genericGetMethodInfo` = `GetMethod("get")` — se "get" é ambíguo (vários
  overloads) retorna null/ambíguo → cadeia quebra. Conferir e, se preciso, patchar o jogo (Cecil).
- Device-side: instalar SOS no gdb do device (DAC em libmscordaccore.so no pacote) p/ clrstack.

## GATE C — boot: crash isolado no LOADER MULTI-THREAD do jogo (2026-06-16 madrugada)
**PONTE DE ASSETS PROVADA FUNCIONANDO** ✅: teste isolado no host —
`Game.Activity.Assets.Open("gui/mobile/title_screen")` abre o .xnb (**len=803079**), tanto via
tipo `AndroidGameActivity` quanto via slot `Context::get_Assets()` (cast). `AssetManager.Open/List`
bridgeados + `get_Assets` override no `AndroidGameActivity` (SOR4Compat.cs) + Activity via
`GetUninitializedObject` (ctors do stub são no-op inválidos). `AssetBridge.List` adicionado (Exists
do jogo usa `AssetManager.List(dir)`).

**Crash atual**: `asset_cache.get<T>` → `load_asset(AssetId)` → segfault NATIVO **ANTES** de chegar
no `get_AssetManager`/Open (override de Assets NÃO é chamado). `load_asset` é **multi-thread**:
`Monitor.Enter/Exit`, `utils.is_main_thread()`, `asset_cache.update_on_main_thread`, `Thread.Sleep/
Yield`, e uma **"asset loading thread"**. Hipótese forte: o loader cria a textura GL numa **thread
de background sem o contexto GL current** (ou a fila de `Threading.BlockOnUIThread` não é bombeada
pois o game loop ainda não roda durante OnDeviceCreated) → Mali segfalta. `is_main_thread()` usa
`Environment.CurrentManagedThreadId` vs id registrado — se o registro do main thread não casa, o
loader toma o caminho de background thread.

**PRÓXIMO PASSO (retomar aqui)**:
- Dump limpo de `CommonLib.utils.is_main_thread()` + onde o main thread id é registrado (provável
  em algum init que meu host não chama). Garantir que `is_main_thread()`==true na main thread.
- Entender a "asset loading thread": se ela faz GL, precisa do contexto compartilhado OU forçar
  loading síncrono na main thread (durante OnDeviceCreated estamos na main thread).
- Alternativa: bombear `Microsoft.Xna.Framework.Threading.Run()` ou rodar o loader após o game
  loop começar (mover a 1ª carga p/ depois do 1º Update). 
- MonoGame: `Threading.BlockOnUIThread` enfileira p/ `Threading.Run()` (chamado no loop) — durante
  OnDeviceCreated o loop não roda → deadlock/crash se asset thread espera por ele.
Fontes funcionais sincronizados em `port/` (bridge c/ List, injector c/ List, SOR4Compat c/ override).

## GATE C — boot: MUITO PERTO (2026-06-16 noite) — crash na 1ª carga de asset
**Estado atual exato** (cadeia de logs `[MG]` confirma): boot vai até dentro do
`OnDeviceCreated` → `PreloadingScreen.Initialize()`:
1. `new SpriteEffect` ✅  2. `Effect ctor(byte[])` ✅  3. `new SpriteBatcher` ✅  4. SpriteBatch ctor **COMPLETO** ✅
5. **CRASH** em `asset_cache.get<TextureProxy>("gui/mobile/left_filler")** — segfault NATIVO,
   ANTES de `Texture2D.PlatformConstruct` (sem log `[MG] Tex2D`) e ANTES do `AssetManager.Open`
   gerenciado (sem log `[asset]`).

**Causa provável**: `asset_cache.get` chama `CommonLib.AssetManagerWrapper.get_AssetManager()` +
`.Exists(string)` ANTES do `Context.Assets.Open()`. O `AssetManagerWrapper` provavelmente usa um
**handle de asset nativo (NDK AAssetManager)** derivado do AssetManager Java — que no meu stub é
um objeto uninitialized (`GetUninitializedObject`, Handle=0/lixo) → ponteiro inválido → segfault
nativo. (O managed `AssetManager.Open` JÁ está bridgeado p/ filesystem e funcionaria se alcançado.)

**PRÓXIMO PASSO CLARO (retomar aqui)**:
- Dumpar IL de `CommonLib.AssetManagerWrapper::Exists(string)` e `::get_AssetManager()` (achar o
  P/Invoke nativo / uso do Handle).
- Fix provável: Cecil-patchar no SOR4.dll `AssetManagerWrapper.Exists`→`return true` e forçar o
  caminho do `Context.Assets.Open()` (já bridgeado), OU bridgear o método nativo de asset.
  Infra de patch pronta: `port/tools/injector` (Cecil).
- Assets: já extraí `gui/preload`+`gui/mobile`+`shader` em `build/game_assets` (~11MB). O resto
  (1.9GB) vai p/ device/R2 quando a 1ª imagem sair.
- Depois: shaders custom `.xnb` (15, GLSL ES) devem compilar no meu MonoGame GLES; validar.

**Notas técnicas acumuladas (GATE C)**:
- `Threading.BlockOnUIThread` (Platform/Threading.cs): roda inline se `IsOnUIThread()` senão
  enfileira p/ `Threading.Run()` (game loop). Durante OnDeviceCreated o loop NÃO roda → se algo
  carregar fora da UI thread, trava. Texture2D.PlatformConstruct usa BlockOnUIThread.
- O jogo tem P/Invoke "Wwise" (áudio nativo) e `Wwise.load_bank` — tratar na FASE 5.
- ⚠️ MUITOS logs `[MG]` de debug no source do MonoGame (GraphicsContext/GraphicsDevice/Game.cs/
  SpriteBatch/Shader/Texture2D/Threading) + `[host]`/`[asset]` — GATEAR por env (ex: SOR4_TRACE)
  ou remover antes do build final. `SOR4_DUMPGLSL=1` dumpa GLSL dos shaders.
- Pacote de teste no device: `/storage/roms/sor4-test/host_pkg`. Rodar (parar ES):
  `LD_LIBRARY_PATH=libs:/usr/lib SDL_VIDEODRIVER=mali DOTNET_EnableWriteXorExecute=0
   SOR4_ASSETS=$PWD/assets ./sor4host`.

## GATE C — boot do jogo: QUASE (2026-06-16) — carrega tudo, crash no OnDeviceCreated do jogo
**Conquistado**: o host `sor4host` (`port/host/`) **carrega SOR4.dll + 15 stubs Android + MonoGame
GLES**, roda `CommonLib.xna.CreateGame()` (cria `CommonLib.xna+StandaloneGame` : Game), e
`game.Run()` inicializa o **contexto GLES + GraphicsDevice 100%** (PlatformInitialize, FBO,
states, ApplyRenderTargets — tudo OK). 
**Crash atual**: segfault nativo (139) dentro de **`GraphicsDeviceManager.OnDeviceCreated`** →
handler de DeviceCreated DO JOGO (em SOR4.dll). É crash NATIVO (não NullRef gerenciado, pois
funções GL ausentes retornam null=NullRef; logo é Mali driver/gl4es/Wwise ou GL call inválida).
Suspeitos: RenderTarget2D/`glDrawBuffers` (GLES3, nil no Mali GLES2), shader compile, ou Wwise.

**Infra de boot que funciona**:
- Stubs Android (15) via `port/tools/stubber/` (Cecil: zera corpos + retarget corelib→System.Runtime).
- MonoGame compat `SOR4Compat.cs`: `AndroidGameActivity:Android.App.Activity`(stub) + `Game.Activity`.
- Host: AssemblyLoadContext.Resolving resolve dlls do dir por nome simples (ignora versão/PKT) →
  SOR4 referencia MonoGame 3.8.3.1 strong-named mas carrega meu unsigned (Core é leniente). ✓
- Pacote em device `/storage/roms/sor4-test/host_pkg`. Rodar: parar ES, `LD_LIBRARY_PATH=libs:/usr/lib
  SDL_VIDEODRIVER=mali DOTNET_EnableWriteXorExecute=0 ./sor4host`.
- Sequência do jogo (de MainActivity.OnCreate): helpshift/firebase/Wwise preinit → `xna.CreateGame()`
  → SetContentView(pulado) → `game.Run()`.

**PRÓXIMO (GATE C/D)**: obter backtrace NATIVO do crash no OnDeviceCreated
(`DOTNET_EnableCrashReport=1` → json com stack nativo+gerenciado). Provável fix: guardar
`glDrawBuffers`/render-target p/ GLES2 no MonoGame, ou stubar Wwise. Depois: extrair assets p/ o
device (ContentManager desktop lê do filesystem) p/ a 1ª imagem.
⚠️ Logs `[MG]` de debug espalhados (GraphicsContext/GraphicsDevice/Game.cs) — gatear por env depois.

## GATE B = PASS COMPLETO ✅✅ (2026-06-16) — MonoGame GLES RENDERIZA no Mali-450
**Buildei MonoGame.Framework GLES próprio** (híbrido SDL+GLES, net9, v3.8.3.1) do source 3.8.4
e ele **renderiza 20 frames (Clear cornflower) no Mali-450 MP, EXIT=0**. Pipeline completo OK:
.NET 9 + MonoGame GLES + sdl2-compat → SDL3-mali → Mali GLES2.

Patches (reproduzíveis em `port/monogame-gles-patches/`: apply.py + csproj + README):
1. `GraphicsDeviceManager.SDL.cs`: pede contexto SDL **ES profile 2.0** sob `#if GLES`.
2. `OpenGL.SDL.cs`: `BoundApi=ES` sob GLES.
3. `RasterizerState.OpenGL.cs`: `PolygonMode` (desktop-only, nil no GLES) gateado `&& !GLES`.
4. csproj `MonoGame.Framework.SOR4GLES.csproj`: defines `OPENGL;GLES;...;DESKTOPGL`, StbImage via
   NuGet, AssemblyVersion 3.8.3.1, net9, exclui Sensors.
GL cru do device (sonda): Mali-450 MP, OpenGL ES 2.0, GLSL ES 1.00, VAO via OES, FBO core.
Build do MonoGame: `~/.dotnet` SDK 9; saída `/tmp/mgout/MonoGame.Framework.dll`.
Teste: `build/gltest/` (ProjectReference ao csproj GLES). Deploy `/storage/roms/sor4-test`.
⚠️ Há logs de debug `[MG]` temporários em GraphicsContext.SDL.cs e GraphicsDevice.OpenGL.cs (gatear/remover depois).
Restam desktop-only não-tratados (MapBuffer/UnmapBuffer em GetData, DrawBuffer em RenderTarget) —
só crasham se usados; tratar sob demanda.

Próximo: FASE 3 (GATE C) — bootar SOR4.dll com stubs (Mono.Android/EOS/etc) no host GLES.

## GATE B (parte 2) — windowing/GL: PROGRESSO + DECISÃO (2026-06-16)
**Conquistado**:
- Cross-compilei **sdl2-compat** p/ aarch64 (`build/sdl2-compat/build-arm64/libSDL2-2.0.so.0.3200.71`,
  só linka libc, dlopen SDL3). MonoGame carrega via ele → SDL3-mali do device.
- MonoGame DesktopGL **cria janela + contexto GL na tela** (fbdev) via sdl2-compat→SDL3-mali,
  QUANDO o frontend (ES) não segura o /dev/fb0. → **windowing resolvido**.
- Único erro restante do DesktopGL: `requires ARB/EXT_framebuffer_object` — o contexto é **GLES2
  nativo do Mali** (não tem as extensões desktop).

**Aprendizados-chave**:
- O driver **mali-fbdev do SDL3 exige o EGL real do Mali** p/ criar a window surface; sombrear
  `libEGL.so.1` com gl4es-EGL QUEBRA a criação da superfície. → não dá p/ enfiar gl4es por aí.
- **gl4es é BECO SEM SAÍDA para este jogo**: os Effects `.xnb` foram compilados para o **MonoGame
  GLES (Android)** = GLSL ES. Só carregam num MonoGame GLES; o DesktopGL usa MojoShader/GLSL
  desktop e não consome esses shaders. Logo, traduzir desktop-GL→GLES (gl4es) não resolve shaders.
- A ES segura o fb0 → launcher precisa parar o frontend antes (matcher /proc/PID, ver cuphead run.sh).
- `/storage/roms` é **vfat (sem symlink)** → copiar .so com nome real (não ln -s).
- net8 vs net9: MonoGame DesktopGL 3.8.4 público é net8; meu app net9 quebra (System.Collections
  8.0.0.0). Resolvido p/ teste usando net8; no jogo real uso a versão do jogo (net9).

**DECISÃO (caminho do GL)**: **buildar MonoGame 3.8.x do source em modo GLES nativo + net9**
(mesmo sabor do Android, sem deps Android). Casa com os shaders `.xnb` do jogo e roda direto no
Mali GLES2. Windowing já funciona (sdl2-compat→SDL3-mali). gl4es descartado.
Próximo: clonar MonoGame, achar a config GLES (`#if GLES`/OpenGL backend), buildar
MonoGame.Framework.dll GLES net9 v compatível com 3.8.3.1.

**Análise de assets (define rota p/ 1ª imagem)**: 25.224 `.xnb` (quase tudo TEXTURA/sprite),
**ZERO arquivos de shader** (.fx/.mgfx/.glsl). Áudio = Wwise (613 `.wem` + 4 `.bnk`).
Vídeo `.mp4` (cutscenes), fontes `.otf/.ttf` (SharpFont/FreeType). → a **1ª imagem (título/menu)
precisa só de SpriteBatch (shader GLSL-ES EMBUTIDO no MonoGame) + texturas + fontes**. Effects
customizados (se houver, embedded) são poucos e ficam p/ depois. Forte validação da rota GLES.

**Insight SpriteBatch**: o shader do SpriteBatch vem EMBUTIDO no MonoGame.Framework (não do .xnb
do jogo). Build GLES → SpriteEffect em GLSL ES → roda no Mali GLES2. Texturas .xnb são
backend-agnósticas. Por isso a 1ª imagem é alcançável mesmo antes de Effects customizados.

**Versão/assinatura p/ o swap final**: SOR4.dll referencia MonoGame.Framework 3.8.3.1 (strong-named).
Meu build precisa: mesmo PublicKeyToken (assinar com .snk público do repo MonoGame) + AssemblyVersion
3.8.3.1 (ou usar AssemblyLoadContext/resolver p/ redirecionar). Tratar no swap.

## GATE B (parte 2) — windowing/GL: estratégia (HISTÓRICO)
**Muro**: device é Mali-450 **fbdev-puro** (sem /dev/dri, sem X, sem wayland compositor).
- SDL2 do sistema (2.32.69): drivers = x11/kmsdrm/wayland/vivante → **NENHUM serve**.
- **SDL3 do sistema (3.5.0-HEAD) TEM `mali`+`fbdev`+`offscreen`** → é como o 3SX roda.
- gl4es completo presente: `/usr/lib/libGL.so(.1)` (desktop GL sobre Mali/EGL) + `libEGL_gl4es.so.1`.
- MonoGame DesktopGL usa **SDL2** e NÃO traz nativos arm64 (só x64) → preciso prover libSDL2.

**Decisão**: bridge **sdl2-compat** (libSDL2-2.0.so.0 implementada sobre SDL3) → reusa a
SDL3-mali que já funciona no device. Cross-compilar p/ aarch64 (dlopen SDL3 em runtime).
Rodar com `SDL_VIDEODRIVER=mali` + `LD_LIBRARY_PATH` incluindo gl4es libGL.
- Fallback se sdl2-compat falhar (ABI SDL3 preview): SDL2 com driver **offscreen** (EGL Mali) +
  shim de present `glReadPixels→/dev/fb0` (técnica Shadowflare/NFS).

Ferramentas host OK: `aarch64-linux-gnu-gcc`, clang 22, SDL3 headers (3.4.10), cmake/ninja.
Nota MonoGame: 3.8.3.1 não está no NuGet → DesktopGL resolve **3.8.4** (validar GL com 3.8.4;
casar versão exata p/ o swap no jogo fica p/ FASE 3 — possível build do MonoGame do source).

## GATE B (parte 1) = PASS ✅ — .NET 9 CoreCLR RODA no .127
Hello-world **self-contained .NET 9 linux-arm64** (CoreCLR) executou no device:
`.NET 9.0.17 OK on Arm64`, GC + thread + EXIT=0. Kernel Linux 3.14.79 mas userland
NextOS 4.8.2 / glibc 2.43 → CoreCLR feliz. Flag usada: `DOTNET_EnableWriteXorExecute=0`.
- Host: SDK .NET 9.0.315 instalado em `~/.dotnet` (via dotnet-install).
- Build de teste: `build/hello/` (`dotnet publish -c Release -r linux-arm64 --self-contained`).
- **Conclusão**: caminho gerenciado é VIÁVEL. Runtime resolvido. Falta GATE B parte 2 (GL).

## GATE A = PASS ✅ (FASE 1) — inventário
Assembly store = formato **XABA v2**, 58 assemblies em **LZ4 (`XALZ`)**.
Extração: parsear header XABA (5×u32) → tabela índice (116×12B) → descritores (stride 28B,
campos mapping_index/data_offset/data_size) → `lz4.block.decompress` por entry.
Script de extração reproduzível: ver comando no log; saída em `build/extract/asm/asm_NNN.dll`.
Inventário completo: `port/ASSEMBLIES.md`. Destaques:
- **`SOR4.dll` (1.49MB) v1.0.0.0 = código do jogo** (NÃO ofuscado).
- **`StandaloneTypeModel.Android.Retail.dll` (1.09MB)** = modelo/lógica (limpo: só ref SOR4+System).
- **`MonoGame.Framework` v3.8.3.1** (Android/GLES build) — versão a casar no swap DesktopGL.
- **.NET 9** (System.Private.CoreLib 9.0.0.0; todos System.* = 9.0). [corrige estimativa .NET 8]
- Stubs: Mono.Android, Java.Interop, EOSSDK.Android, HelpshiftSDKx.Android,
  Xamarin.GooglePlayServices.*, BillingClient, Firebase, PlayCore, SharpFont.Core.

### Acoplamento Android (medido) — `port/ANDROID-SURFACE.md`
SOR4.dll usa **116 typerefs Android/Java**, mas concentrados:
- **Funcionais (precisam stub útil)**: `Context`/`Activity`/`Application` (paths),
  `AssetManager` (carregar .xnb → bridge p/ filesystem), `ISharedPreferences(+Editor)` (settings).
- **Serviços (stub no-op/offline)**: Google Play Games (achievements/leaderboards/cloud-save),
  Google Sign-In, Billing/DLC, EOS, Helpshift, Firebase, PlayCore.
StandaloneTypeModel = sem Android. → acoplamento isolado em 1 arquivo, gerenciável.

## Alvo de runtime (revisado)
- **.NET 9 linux-arm64** (CoreCLR oficial) — game é net9.0. Mono 6.12 clássico do host NÃO serve.
- Risco a validar JÁ na FASE 2: CoreCLR .NET 9 roda em **kernel 3.14** (glibc 2.43 ajuda;
  knobs: `DOTNET_EnableWriteXorExecute=0`, `DOTNET_GCgen0size`, etc.). Fallback: MonoVM .NET 9.
- MonoGame **DesktopGL 3.8.3.1** (SDL2 + OpenGL) → gl4es p/ GLES2 no Mali-450.

## Log cronológico
- **2026-06-16** — FASE 0. Recon device .127 + repo. Confirmado MonoGame/.NET no APK,
  assemblies em claro. Cuphead do repo é il2cpp (não serve de base de runtime). Sem mono no
  device. Criado scaffold `ports/sor4/`. Commit `d1b7c9e`.
- **2026-06-16** — FASE 1 / GATE A PASS. Extraídos 58 assemblies (XABA+LZ4). Achado SOR4.dll
  (jogo, não-ofuscado) + MonoGame 3.8.3.1 + .NET 9. Mapeado acoplamento Android (116 refs, mas
  isolado/stubável). Docs `ASSEMBLIES.md`/`ANDROID-SURFACE.md`. Próximo: FASE 2 validar .NET 9 no .127.
