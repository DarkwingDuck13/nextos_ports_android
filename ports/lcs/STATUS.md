# GTA: Liberty City Stories (LCS) → NextOS / Mali-450 — so-loader

Port iniciado 2026-06-21. Base = port do Bully (engine `libGame.so` War Drum), mas a LCS usa
framework War Drum **mais novo** (JNI `GTAJNIlib_*`/`RockstarJNIlib_*`, dados WAD via
`LogicalFS`, EGL auto-gerenciado). Device de trabalho: **.88** (root/<senha>, Mali-450 Utgard).

## Estado atual: 🟡 BOOTA, ESTÁVEL, CARREGA TUDO — mas tela PRETA (state-machine travada)
O jogo carrega 100% dos dados, passa todos os crashes, roda o frame loop estável — porém a
máquina de estados principal fica em **GS_START_UP (estado 0)** e não avança pro frontend/menu,
então `GTAGameTick` não enfileira draws → tela preta (engine apresenta preto todo frame).

## O que JÁ funciona (infra dura — tudo resolvido)
- ✅ Loader 2-módulos (libc++_shared + libGame), 0 imports não resolvidos (8 stubs em imports.c).
- ✅ Driver JNI LCS (`jni_shim.c`): JNI_OnLoad, setters (setOSVersion/DeviceInfo/PrivateFilesDir
  [com barra!]/AssetManager/markInitialized), viewOnSurfaceCreated/Changed/Resume, loop viewOnDrawFrame.
- ✅ **EGL interceptado** (imports.c): a engine cria a surface dentro de viewOnSurfaceChanged
  (eglGetDisplay/Initialize/ChooseConfig/CreateWindowSurface/CreateContext/MakeCurrent) → todos
  redirecionados pro contexto **SDL2-mali** (sem BAD_ALLOC). GL = Mali-450 ES2 1280x720.
- ✅ **WAD decifrado + servido**: `data_main.wad`/`data_music.wad` são **XOR `{0xAF,0x66}`**
  (header decifra p/ magic "DAWL"). FAT indexada por HASH (não dá p/ desempacar offline por nome).
  Solução: `aa_open`/`w_fopen` montam um `WadArchive` próprio (`WadArchive::OpenWad(file,enc=1)`,
  10112 entries) e servem por nome via `WadArchive::OpenFile`+`FSWadFile::Read` (memória, +null p/ XML).
- ✅ Bypasses de crash (no-op hooks): `PVS::LoadPVSZones` (TiXml crasha), `SocialClubHandler::Update`,
  `C_PcSave::InitCloudSaveSystem`/`UpdateCloudSave` (online/cloud sem conexão).
- ✅ Boot até frame loop estável (frame 2760+ sem crash), carrega config de gameplay
  (TIMECYC/SURFACE/PEDSTATS/HUD.TXD/FONTS.TXD) + ~5000 texturas .pvr do WAD.

## O bloqueio preciso (próximo passo)
Máquina de estados: `stobj = *(text_base+0x7f9000+704)`; estado em `*(stobj+0x110)` (enum);
menu = 0x45 ou 0x33 (`isOnMainMenuScreen`). **Fica em 0 (GS_START_UP) para sempre.**
`GTAGameTick` (chamado por viewOnDrawFrame) exige `*(stobj+21)==1` p/ tickar — está 0; forçar
=1 CRASHA (0x373448) pois o tick não está pronto. Ou seja: a sequência de startup que avança
0→...→frontend não roda. Falta achar/chamar o que inicializa CGame e avança o estado (provável:
ordem/args do gate Rockstar, ou um `CGame::Initialise`/`RsEventHandler`/loop de startup que não
está sendo dirigido). Investigar: o que escreve `*(stobj+0x110)` (writes em [x,#272]:
0x369194,0x3dcfa4,0x4d9110,0x5279c0...) e o que seta `stobj+21`.

## Build / Run / Debug
- Build: `ports/lcs/build.sh` (toolchain Amlogic-old aarch64) → binário `lcs`. SDL2 dá warnings
  de ld não-fatais (binário sai ok, igual Bully).
- Deploy: matar lcs por `/proc/*/exe` (kill -9; ⚠️ **NUNCA matar durante chamada GL** → trava em
  D-state `_mali_osk_wait_queue_wait_event`, precisa reboot). scp falha se lcs vivo (text busy).
- Run: `cd /storage/roms/ports/lcs; SDL_VIDEODRIVER=mali LD_LIBRARY_PATH=/usr/lib:$PWD
  LCS_DATA_DIR=$PWD/gamedata ./lcs >run.log 2>&1`.
- **Teste sem D-hang**: `LCS_MAXFRAMES=N` → `_exit(0)` limpo após N frames (sem kill externo).
- Screenshot autoritativo (dd /dev/fb0 NÃO reflete o mali EGL): `touch /dev/shm/lcs_shot` →
  glReadPixels em my_eglSwapBuffers → /dev/shm/lcs_shot.raw (1280x720 RGBA, flip vertical) + log
  `[shot] nz=N` (não-pretos). Gates de debug: LCS_FORCEGATE, LCS_FORCETICK, LCS_NOPVS, LCS_DRVSWAP.
- Crash handler (main.c): imprime `libGame+0x(PC-text_base)` + backtrace FP. Mapear com
  `nm -D -C libGame.so` (binário stripped → usar -D). Script de mapeamento usado em ~/lcs-build.

## Arquivos
- Port: `~/nextos_ports_android/ports/lcs/` (src). Libs+símbolos: `~/lcs-build/` (libGame.so,
  SYMBOLS-jni.txt, shot.png). APK: `~/Downloads/gta-liberty-city-stories-2.4.379-mod-menu-5play.apk`.
- Device: `/storage/roms/ports/lcs/` (lcs+libGame.so+libc++_shared.so+libVendor_mpg123.so+gamedata/).

## ATUALIZAÇÃO 2026-06-21 (sessão 2): 🟢 MENU 100% + ENTRA NO GAMEPLAY (state 9)
PROGRESSO ENORME — boot→menu→gameplay funcionando:
- ✅ **MENU PRINCIPAL renderiza** (capa GTA LCS + Start Game + opções) — `MENU-RENDERIZANDO-2026-06-21.png`.
- ✅ Boot state-machine destravada: `OS_ApplicationTick` estados 0-9 via jump-table @0x23f1ca.
  Avanço por flags: state1→2 = `CommonAPI_HandlePlaylistFinishInit(1)` (flag 0xa441b0);
  state2→3 = `feobj+40=1` (GameLoaded); states 3-6 auto; state7=MENU; state7→8 = `feobj+25=1`
  (Start Game) → state8 `GameStart()` → state9 = `GameCoreTick` (GAMEPLAY).
- ✅ Game-tick gate: `feobj+21=1` (`feobj=*(text_base+0x7f9000+704)`); state real = `*(*(text_base+0x7fd000+2232))`.
- ✅ Fixes p/ chegar ao gameplay: no-op `glFinish` (wedge Mali no load), `SetupPostProcessShaders`
  (FBO pós-proc wedge), `CCutsceneMgr::DeleteCutsceneData` (restart→RestoreWithJumpCut→FindPlayerVehicle
  null crash); `RenderMenus` skip no state 9 (loading-screen CSprite2d::Draw crash); mip-skip ETC.
- ✅ **CHEGA AO STATE 9 (gameplay), ~40+ frames** antes do bloqueio. Frame de gameplay capturado =
  uniforme clear-color (0.11,0.13,0.11) → mundo AINDA carregando (draws congelados em 114).

### 🔴 BLOQUEIO FINAL: wedge do Mali durante o streaming do mundo (state 9)
Wedge ~frame 68-82: `texMB=60 texN=5293` (NÃO é memória — 60MB; é COUNT: 5293 texturas, o jogo
streama TODOS os veículos). Provável estouro de descritores/MMU do Utgard pelo NÚMERO de texturas
OU um GL-op específico no flood de uploads. Mundo não renderiza ainda (fase de load). Próximo:
reduzir count/load (eviction CStreaming::MakeSpaceFor, ou TEX_LIGHT estilo Bully) OU achar o GL-op
exato do wedge (tracer no heartbeat). Mirar: sobreviver o load → mundo renderiza.

### INFRA DE TESTE (regras do porter — NÃO fazer ele reiniciar)
- **Watchdog no device**: `( sleep 75; <lcs vivo?> && reboot -f ) &` antes de rodar → auto-recupera
  wedge SEM intervenção manual. (Mali kill durante GL = D-state, só reboot resolve.)
- **Heartbeat fsync** (`heartbeat.txt`): última fase/frame/texMB sobrevive ao wedge.
- **Testes curtos**: `LCS_MAXFRAMES=N` (_exit limpo), `LCS_STARTFRAME=N` (auto-start cedo).
- ES **masked**. Swap em `/storage/roms/bigswap.img`. gdb+gdbserver no device.
- Envs: LCS_START=force|tap, LCS_STARTFRAME, LCS_MAXFRAMES, LCS_NOTICK, LCS_MENUS9, LCS_POSTPROC,
  LCS_KEEPMIP, LCS_NOPVS, LCS_STATELOG, LCS_HB.

## ATUALIZAÇÃO 2026-06-21 (sessão 3): 🔑 VIRADA — "wedge do streaming" era FANTASMA; raiz real = NEW-GAME RE-INIT LOOP
**Mudança fundamental de entendimento. O "BOSS FINAL: wedge do Mali no streaming" da sessão 2 NÃO
era a GPU — era ARTEFATO DE MEDIÇÃO do meu próprio tracer.** Cadeia de erros capturada (gdb/proc/glstats):

### 1. ❌ "wedge do Mali no streaming" = `fdatasync` do gltrace saturando a SD FAT (NÃO é a GPU)
Com `LCS_GLTRACE=1` eu fazia `fdatasync` a CADA op-GL. No flood de ~5000 uploads/poucos frames =
milhares de fdatasync/s na SD vfat → `fdatasync`→`fat_file_fsync`→`wait_on_page_bit`→D-state.
Backtrace do "wedge" (`/proc/PID/stack`): **`SyS_fdatasync`**, não Mali. A thread do Mali estava
OCIOSA (`_mali_osk_notification_queue_receive`). **SEM `LCS_GLTRACE`, o streaming COMPLETA** (uploads
sobem ~1350/s, terminam) e o jogo **RODA gameplay (state 9) a ~1 fps, frames avançando 35→125+**.
Lição: tracer com fsync por op-GL é veneno; o "wedge" some sem ele.

### 2. ✅ Gameplay RODA mas tela = clear-color uniforme (avg 28,32,28, stdev 0). Mundo NÃO desenha.
`[glstats]` por frame em state 9: **`dScreen=4 dFbo=0 nVis=0`** — só ~4 draws/frame (HUD/fade),
ZERO geometria, ZERO entidades visíveis. NÃO é problema de FBO/blit (hipótese FBO refutada:
`LCS_POSTPROC=1` TRAVA de vez em f=38 — post-proc é wedge real do Utgard, no-op justificado).

### 3. 🎯 RAIZ: new-game nunca inicia de verdade. `cam=(0,0,0)`, `ped=(nil)`, `fade=2` (FADE_2).
Gate de render (confirmado no re3 `main.cpp`): `if(... && GetScreenFadeStatus()!=FADE_2){ ConstructRenderList(); RenderScene(); }`.
`GetScreenFadeStatus()` retorna **2 (FADE_2)** → bloco inteiro pulado. E `LCS_UNFADE` (força fade→0)
NÃO resolve: câmera está na ORIGEM e player é null → nada pra renderizar.
- `GameStart()` (state 8) só chama `CGame::Initialise()` (init de SISTEMAS), **NÃO `CGame::InitialiseWhenRestarting()`**
  (spawn do player via `ReInitGameObjectVariables`→`SetupPlayerPed` + reset câmera/fade).
- Forçamos `feobj+25=1` (state 7→8→9) pulando o new-game real → player nunca spawna.

### 4. ✅ Chamando `CGame::InitialiseWhenRestarting()` (`LCS_INITRESTART=1`): player SPAWNA (1 frame `ped=0x5580e9a400`)
…mas trava em **`lglWaitForStreamerToFinishTasks()`** (loop `usleep`): a main thread pumpa o streamer
(`lglStreamerTickFromMainThread`) mas **`lglHasStreamerFinishedTasks()` NUNCA fica true** — 1 creator
(lglWorldCreator/ModelCreator/BufferCreator/+Destroyers) tem task pendente que nunca dren­a.
`LCS_BOUNDSTREAM=1` limita o wait (400 ticks) e destrava, mas `finished=0` permanente.

### 5. 🎯🎯 LOOP DE RE-INIT (raiz central): `main.scm` recarregado **85× (≈1/frame)**.
O log mostra o ciclo repetindo: `PARTICLE.CFG→WEAPON.DAT→WEAPON_MULTI.DAT(FALTA)→PEDGRP.DAT→main.scm→ARROW.DFF→ZONECYLB.DFF→race_arrow→(repete)`.
**O jogo re-inicializa o new-game/script TODO FRAME e nunca progride** → o script (main.scm) nunca roda
pra posicionar o player/câmera e levantar o fade. **Pré-existente** (não foi o INITRESTART). Provável
ligação com (4): o init espera o streamer terminar (nunca termina) → retry infinito.

### PRÓXIMO PASSO (lead preciso p/ próxima sessão):
**Por que `lglHasStreamerFinishedTasks()` nunca fica true?** Identificar QUAL creator tem task pendente
permanente (instrumentar os 5 `lglXxxCreator::hasPendingTasks` — ponteiros em globais 7fa000+2952 /
7fd000+248 / 7fd000+1824 / 7fe000+2552 / 7fe000+224). Suspeitas: (a) thread async do loader travada
(uma thread fica em state R rodando), estilo job-system do RE4 (sh_sem); (b) task referencia asset
que falha (WEAPON_MULTI.DAT FALTA? ou modelo do gameplay). Resolver o streamer → init completa →
script roda → player posicionado → mundo renderiza. DEPOIS: religar menu real + controles (pedido do porter).

### Flags novas desta sessão (jni_shim.c, todas gated): 
`LCS_GLSTATS` (log dScreen/dFbo/nVis/fade/cam/ped/veh por frame — SEM fdatasync), `LCS_UNFADE`
(GetScreenFadeStatus→0), `LCS_INITRESTART` (chama CGame::InitialiseWhenRestarting ao entrar state 9;
`LCS_IRDELAY`), `LCS_BOUNDSTREAM` (limita lglWaitForStreamerToFinishTasks; `LCS_STREAMER_MAX`).
⚠️ NUNCA usar `LCS_GLTRACE` em gameplay (satura SD→falso "wedge"). Default (sem env) = MENU estável.

## ATUALIZAÇÃO 2026-06-21 (sessão 4): 🎉 HUD + GAMEPLAY RENDERIZAM ("Atlantic Quays") — falta só o mundo 3D
MARCO GRANDE. Screenshot `~/lcs-build/MARCO-HUD-GAMEPLAY-2026-06-21.png`: HUD completo do GTA LCS
renderizando (radar/bússola, relógio 12:00, barra de vida, ícone arma=punho, $00000000, controles
touch) + nome da localização **"Atlantic Quays"** (Portland/LCS). Jogo em GAMEPLAY REAL.

### Cadeia de fixes desta sessão (fluxo REAL, runs ≤30s, device estável):
1. ✅ SAIDA LIMPA DO MALI (resolve "tela preta+reboot"): `lcs_mali_teardown()` (glFinish real +
   eglMakeCurrent NULL + eglTerminate) antes do _exit -> o Mali NÃO fica wedge entre runs. PROVADO
   estável por 13 ciclos sem reboot forçado. Exit por TEMPO: `LCS_MAXSECONDS` (_exit limpo, sem kill).
2. ✅ FLUXO REAL de new-game: `LCS_START=newgame` chama `CMenuManager::StartNewGame(&FrontEndMenuManager)`
   (-> DoSettingsBeforeStartingAGame) em vez do `feobj+25` cru -> SEM o loop de re-init (main.scm 2-3×,
   não 85×), player spawna e PERSISTE, câmera posicionada em coords reais (1250,-1157,12.7).
3. ✅ `LCS_BOUNDSTREAM` (limita lglWaitForStreamerToFinishTasks; o lglBufferCreator nunca "termina" —
   identificado via STREAMDIAG dos 8 creators). `LCS_STREAMER_MAX` ajusta frames-vs-load.
4. ✅ `LCS_UNFADE` (state-9-only! no menu crasha) -> GetScreenFadeStatus→0 abre o gate de render.
5. ✅🔑 `LCS_NODOFADE` -> pula `DoFade()` (@0x520d1c) no state 9. ERA A CAUSA DA TELA PRETA: o overlay
   preto do fade (FADE_2) era desenhado POR CIMA do mundo. Sem ele: HUD aparece (nz 0→99.9%).
6. ✅ `LCS_NOON` -> CClock::ms_nGameClockHours=12 (relógio 12:00 no HUD).

### 🔴 PRÓXIMO: mundo 3D não renderiza (só HUD/2D). RAIZ = PVS.
LCS usa `MattRenderScene` + PVS (Potentially Visible Set). No-opamos `PVS::LoadPVSZones` (crasha).
`LCS_NOPVS=0` (habilita) CRASHA: null-deref em memcpy (libc+0x7d8b4, x0=NULL src=0x2d97) DENTRO do
TiXml (libGame+0x7b9ed8/0x7baf84/0x7b7e64) parseando `indust.xml` (388807B, header OK `<?xml ...>`)
<- PVS::LoadPVS (0x561b68/0x567b70) <- GameStart (0x5249e4). `dvPVSOnlyRenderVisibleModels=0` etc NÃO
ajudam (o renderer precisa das ZONAS, não só do flag). LEAD: corrigir o parse TiXml do indust.xml
(ver serving .xml em aa_open: +1 null) OU achar caminho de render sem PVS. Config atual (sem NOPVS=0)
= HUD+gameplay estável. Flags run30.sh: newgame+UNFADE+NODOFADE+NOON+BOUNDSTREAM+NOPVSCULL.

## ATUALIZAÇÃO 2026-06-21 (sessão 4 cont.): refinamento do blocker do mundo 3D = PVS (world-stream)
- RENDERDIAG: em gameplay (s9) AMBOS rodam ~1×/frame: RenderScene (clássico) + MattRenderScene (mobile)
  + CRenderer::ConstructRenderList. MAS `nVis=0` nos dois → os SETORES do mundo estão VAZIOS (0 entidades).
  Na engine Leeds da LCS o mundo é STREAMADO (cWorldStream + indust.img 144MB + PVS zones indust.xml),
  NÃO via placement IPL clássico. Sem PVS, o world-stream não popula os setores → nVis=0 E MattRenderScene
  (que depende das zonas PVS) não desenha. CONCLUSÃO: PVS é OBRIGATÓRIO p/ o mundo 3D; corrigir o crash.
- CRASH do PVS refinado (gdb, LCS_NOCRASHHANDLER=1): NÃO é getSize/buffer NULL (getSize=388808 correto;
  AAsset_getBuffer implementado; remover o +1 do XML NÃO resolve). indust.xml é um `<scene>` com
  `<unit meter="0.01" name="centimeter">` + geometria (3DS-export). Crash em parser de NÚMERO da libc
  (strtod-like @0x7f96..4f38: `ldrb w12,[x12,#1]`...`sub #0x2d`...`cmp #0x32`) escaneando ALÉM de uma
  string sem terminador; x19=NULL; offset do fault VARIA entre runs (0x2d97/0x30b1) = CORRUPÇÃO DE HEAP
  durante o parse do DOM gigante (388KB). Precisa gdb single-step/ASAN-like p/ achar o overflow fonte.
  Flags diag novas: LCS_RENDERDIAG, LCS_MATT2CLASSIC, LCS_OBFDIAG, LCS_NOCRASHHANDLER, LCS_XMLPLUS1.

═══════════════════════════════════════════════════════════════════════════════
## SESSÃO 5 (2026-06-21, continuação): CUTSCENE DE INTRO — máquina de FINISH mapeada
═══════════════════════════════════════════════════════════════════════════════
Contexto: o dev anterior levou a cutscene de intro (cscoach/Toni chega a LC) a RENDERIZAR em 3D
(biblioteca+personagem+geometria texturizada) — salto além do "mundo preto" da s4. MAS ela
fica em LOOP: toca, e nunca entrega o gameplay ("entra em gameplay e pula pra vídeo"). o dev anterior
tentou forçar FinishCutscene/HasCutsceneFinished/skip → loop persiste (tratava sintoma).

DIAGNÓSTICO COMPLETO (disassembly libGame.so + diag read-only no device, flags novas LCS_CUTSCENE_TIMEDIAG/LCS_FLYBYDIAG/LCS_CUTSCENE_SPLINEFIX):
1. Nos PRIMEIROS frames (f≈15) o jogo está em GAMEPLAY REAL em Portland (cam=1250,-1157,12.7,
   ped spawnado, pdens/cdens=1.0, cut=0/0/0). AÍ o script dispara a intro → cam pula p/ a cutscene.
2. `CCutsceneMgr::Update()` @0x2ec5b4 = só trampolim p/ `Update_overlay()` @0x2eede0 (a lógica real).
3. RELÓGIO da cutscene (`*(0x7f9000+384)`, segundos) AVANÇA certo: 0→56+; gstate(`*(0x7f9000+8)`)
   = 1(load)→2(play); gate(`*(0x7f9000+16)`)→1; delta(`*(0x7f9000+472)`)≈5. Tudo OK aqui.
4. FINISH gate (HasCutsceneFinished @0x2ef294): `flag=*0x84ad72; if(flag!=1)return TRUE; else
   return CCamera::GetPositionAlongSpline()==1.0`. GetPositionAlongSpline = `*(float*)(TheCamera+336)`
   = m_fPositionAlongSpline. **Fica CONGELADO em 0.001765 = 100/56667 → nunca chega a 1.0 → loop.**
5. Quem avança +336 = `CCam::Process_FlyBy` @0x31c860: position = flybyTime(CCam+160) / totalDur,
   totalDur=(uint)(pathTime[26*10-9]*1000). Path CARREGA OK (LoadPathSplines, 26 pts, dur REAL=56.667s;
   times monotônicos 0,12.6,15.8,...56.7). flybyTime avança ((delta/50)*1000)/frame.
6. 🔑 Process_FlyBy é chamado só ~2× (init f≈49 + 1 advance f≈53 → flybyTime 0→100) e PARA. pos
   congela. cammode (m_arrCams[0].mode @ TheCamera+416+28 = TheCamera+444) FICA EM 17 (modo cutscene/
   spline) — NÃO sai de 17. Mesmo assim Process_FlyBy não é re-despachado todo frame. (m_nMode=CCam+28;
   stride CCam=664; m_arrCams base=TheCamera+416. mode dispatch via jump-table em CCam::Process @0x315c0c.)
7. AUTO-FINISH NATIVO do Update_overlay (@0x2ef01c→0x2ef11c FinishCutscene): gates = flag0x84ad72 bit0,
   strcasecmp(nome), **mode==17 ✓**, gstate==2 ✓, `clock_ms+1000 > GetCutSceneFinishTime()` (✓: clock
   chegou a 84s ≫ 56s) → daí faz `CCamera::Fade()` + espera flag `*(0x7f9000+464)` + contador → FinishCutscene.
   HasCutsceneFinished tbm é POLLADO pelo SCM em `CRunningScript::ProcessCommands700To799` (opcode HAS_CUTSCENE_FINISHED).
8. SPLINEFIX (forçar +336 = clock/dur): pos CHEGOU a 1.0 e a câmera até MUDOU de shot, MAS gstate
   ficou 2 / cut=1/1/0 — **HasCutsceneFinished=true NÃO terminou a cutscene** → ninguém consome o poll
   naquele momento; o fim real depende do AUTO-FINISH nativo (fade) ou do SCM no ponto certo.
9. 🎯 SUSPEITA FORTE (testando): o auto-finish nativo faz FADE-OUT e ESPERA o fade; mas `fade` fica
   preso em FADE_2 o tempo todo PORQUE **LCS_NODOFADE + LCS_UNFADE (hacks p/ ver HUD) DESLIGAM o sistema
   de fade** — exatamente o que o fim natural da cutscene precisa. main.scm carrega só 2× (loop da s3
   RESOLVIDO), init 1× → jogo estável. Teste: rodar cutscene SEM fade-hack (LCS_NO_FADEHACK=1 no run30.sh).
RESULTADO NO_FADEHACK: fade VOLTOU a funcionar (fade=0, antes preso em 2) -> hack confirmado
como quebra do fade. MAS cutscene ainda nao termina (auto-finish tbm precisa do spline completo).

🎯🎯 CAUSA-RAIZ UNICA ACHADA (resolve o loop): `CGame::Process` @0x33c46c faz
`x21=*(0x7f9000+704)`(=feobj); `if(feobj[25]==0) goto skip;` PULA CTheScripts::Process (o SCM)
E CCamera::Process (camera/flyby); so CCutsceneMgr::Update roda sempre (por isso o relogio anda
mas o flyby congela e o script nunca termina). **feobj+25 fica 0 a cutscene INTEIRA** (225 frames
gstate=2; =1 so 1 frame no f14). Confirmado com fe25 no [ctime]. => camera nao processa (flyby so
2x, spline congela em 0.001765) E SCM nao roda (nao consome HasCutsceneFinished). UM gate so.

FIX NATURAL (LCS_FE25, gated): manter feobj+25=1 em state 9 -> CGame::Process roda o SCM+camera
sozinho. RESULTADO: REVELA O PROXIMO BLOQUEIO -> o SCM roda e CRASHA (sig=11, x0=NULL) no opcode
START_NEW_SCRIPT: `CTheScripts::StartNewScript()` retorna NULL e o codigo faz `add x3,x0,#0x68`
sem null-check (CRunningScript::ProcessCommands0To99 @0x3dff4c <- Process @0x3df1dc <- CTheScripts::
Process @0x3deef8 <- CGame::Process). StartNewScript=NULL = pool de scripts nao pronto qdo forcamos
feobj+25 no meio do load (f17, gstate=1). => forcar feobj+25 por-frame eh hack; o certo eh entrar
no state 9 JA com feobj+25=1 e o sistema de script inicializado (fluxo menu->jogo real / GameStart
+InitialiseWhenRestarting setando o estado "jogo rodando" + CTheScripts::Init/pool).

PROXIMO PASSO (proxima sessao): por que CTheScripts::StartNewScript retorna NULL (pool CRunningScript
nao alocado/cheio no nosso forced-newgame)? Garantir CTheScripts::Init/pool no fluxo de new-game, OU
descobrir qual passo do menu->jogo seta feobj+25 persistente (sem re-init loop) + inicializa scripts.
Refs p/ estudar: PSVita GTASA, nosso reVC so-loader, e o BULLY (MESMA libGame.so) — como
eles entram no jogo com scripts+camera vivos. Bully = melhor ref (engine identica).
GANHOS confirmados desta sessao: (1) loop da s3 (main.scm 85x) RESOLVIDO - agora 2x, init 1x.
(2) fade-hack NODOFADE/UNFADE quebra o fade da cutscene - tornar condicional (LCS_NO_FADEHACK).
(3) toda a maquina de finish da cutscene mapeada. Flags novas (gated): LCS_CUTSCENE_TIMEDIAG,
LCS_FLYBYDIAG, LCS_CAMDIAG, LCS_CUTSCENE_SPLINEFIX, LCS_NO_FADEHACK(run30.sh), LCS_FE25.

═══════════════════════════════════════════════════════════════════════════════
## SESSÃO 6 (2026-06-22, continuação): CÂMERA DA CUTSCENE — REORIENTAÇÃO
═══════════════════════════════════════════════════════════════════════════════
Contexto: o porter relatou "câmera anda um pouco e para" (geral, em TODAS as cutscenes/vídeos),
"semáforo grande", "quadrado preto no chão". o dev anterior tinha acabado de adicionar um helper manual
`LCS_CUTSCENE_FLYBY_DIRECT` (chama CCam::Process_FlyBy à mão pelo wrapper de Update_overlay).

ACHADOS (disassembly + análise dos logs existentes, SEM regredir nada):

1. 🔑 GATE DA CÂMERA = feobj+25 (ONE-SHOT). `CGame::Process`@0x33c46c:
   `ldrb w8,[x21,#25]; cbz w8 -> skip`. Se feobj[25]!=0 roda CTheScripts::Process + CCamera::Process
   (2×) E NO FIM faz `strb wzr,[x21,#25]` (ZERA). Ou seja: feobj+25 é flag de "rodar 1 frame de
   game logic" que a própria CGame::Process consome. Pra a câmera processar todo frame, algo precisa
   re-setar pra 1 a cada frame (é o que LCS_FE25 tenta).

2. 🎯 A CÂMERA DA CUTSCENE JÁ FUNCIONA via SPLINEFIX (default do run30.sh)! Prova no log
   `run-resourcedrain-160-1.log` (MAXSECONDS=160, SEM flyby-direct):
     f=360  cmode=17 clock=8.4s  spline=0.147
     f=720  cmode=17 clock=19.5s spline=0.344
     f=1080 cmode=17 clock=30.7s spline=0.541
     f=1440 cmode=17 clock=43.4s spline=0.765
     f=1800 cmode=17 clock=56.4s spline=0.995
     f=1809 spline=1.000 csfin=1 -> FinishCutscene + restore camera (modo 17->4)
   A câmera (modo 17=flyby/cutscene) percorre a spline 0->1.0 em 56s e a cutscene FINALIZA limpo.
   ⇒ "câmera para cedo" = ARTEFATO DOS TESTES CURTOS (45-70s) do o dev anterior com flyby-direct: em 45s o
     jogo (a ~11fps) só chega a f~127 = primeiros ~8s da cutscene. NÃO era regressão da câmera.

3. ❌ O helper FLYBY_DIRECT do o dev anterior é beco sem saída: no seu caminho clk=0.000 e cmode=4 (FOLLOWPED,
   não 17) -> cutuca a câmera errada e só roda 3 frames. Está gated (LCS_CUTSCENE_FLYBY_DIRECT) ->
   NÃO afeta o default. Manter desligado. SPLINEFIX é o caminho bom.

4. ⚠️ NÃO HÁ launcher ES pro lcs (ports_scripts sem entrada). O porter vê a TV enquanto NÓS rodamos
   os testes -> a "config que ele vê" = a do último run. Os últimos runs do o dev anterior eram flyby-direct
   curtos -> daí a impressão de câmera travada.

5. 🔴 PRÓXIMO BLOQUEIO REAL (não é a câmera): handoff cutscene->GAMEPLAY ANDÁVEL + mundo 3D.
   - O run de 160s terminou EXATAMENTE quando a cutscene acabou (f=1809) -> não chegou a testar
     gameplay pós-cutscene. Rodando agora 220s p/ ver o que vem depois.
   - Crash conhecido ao rodar scripts de gameplay (FE25+SCRIPTPOOL): comando de script 600-699 cria
     CObject -> CEntity::CEntity -> CPlaceable::CPlaceable -> memcpy 11 bytes com x0=NULL (src lixo
     0x105ce). Backtrace: 0x32fcd8<-0x347908(CPhysical)<-0x40ba88(CObject)<-0x3ead64(Cmd600To699)<-
     ProcessCommands. Provável init incompleto (streamer bounded) -> object system meio-inicializado.
   - Mundo 3D (PVS) continua o blocker da s4 (LCS_NOPVS=0 crasha no TiXml do indust.xml).

PLANO s6: (a) confirmar handoff cutscene->gameplay (run 220s + filmstrip). (b) se gameplay roda,
atacar mundo 3D (PVS). (c) se crasha no CObject, resolver o init/streamer. Câmera da cutscene = OK.

───────────────────────────────────────────────────────────────────────────────
### s6 AVANÇO (run proven220 + fix fade ciente-de-cutscene)
───────────────────────────────────────────────────────────────────────────────
RUN de 220s (config default SPLINEFIX, SEM FE25/flyby-direct) = SEM CRASH, e revelou
TUDO funcionando exceto o fade do gameplay:
- 2 cutscenes tocam e FINALIZAM (f=1277 dur56.7s, f=3380 dur74.2s) -> restore camera 17->4.
- 🎉 MUNDO 3D RENDERIZA nas cutscenes: shot_seq_1=rua de Portland (prédios, ônibus do Toni,
  loja), shot_seq_3=terraço c/ skyline (Toni+personagem, legenda). nz=85%.
- 🎯 PÓS-2ª-CUTSCENE = GAMEPLAY REAL: f=3380 `cut=0/0/0 fade=0 dScr=787 nVis=11
  cam=1482.9,-178.5,56.1 ped=valido pop/cars populados` -> MUNDO DE GAMEPLAY RENDERIZA
  (787 draws!) por 2 frames. f=3382 o FADE volta pra FADE_2 -> dScr cai pra 1 -> tela preta.
  ⇒ O handoff cutscene->gameplay FUNCIONA; só o fade pós-cutscene não levanta.

FIX (jni_shim.c, s6): `lcs_cutscene_active()` (le CCutsceneMgr::ms_running/ms_cutsceneProcessing).
my_DoFade e my_GetScreenFadeStatus agora forçam fade-aberto SÓ em state9 E !cutscene_active
(gameplay puro); durante a cutscene devolvem o fade REAL (não quebra a cutscene - resolve a
tensão da s5). Habilitar via LCS_NO_FADEHACK=0 (run30 exporta UNFADE+NODOFADE). EM TESTE.

CONCLUSÃO REORIENTADORA: a câmera da cutscene e o mundo 3D NÃO eram o blocker - já funcionam.
O blocker do "gameplay visível" era só o fade pós-cutscene. PVS (s4 "mundo não renderiza")
TAMBÉM já não é blocker: nVis=11 e 787 draws provam que o mundo popula no gameplay.

───────────────────────────────────────────────────────────────────────────────
### s6 CONTROLES + FOLLOW-CAM (estado e próximos passos)
───────────────────────────────────────────────────────────────────────────────
CONTROLES: input CHEGA e o player RESPONDE. Sonda `echo N > /dev/shm/lcs_btn` (12cima/13baixo/
14esq/15dir -> g_axis_state movimento; 0-11 botões). Injetando 12 no gameplay: o Toni VIROU de
frente e apareceu seta de objetivo (shot_seq_walk) = CPlayerPed::ProcessControl roda. Pad físico
"USB Gamepad" detectado e mapeado (SDL_GameController -> JNI onJoyButton/setJoyAxis). ⇒ controles
funcionam no nível de input; falta o feedback completo (camera seguir, validar translação).

FOLLOW-CAM (gameplay): a câmera NÃO segue o player porque CCamera::Process é gated por feobj+25
(=0 no gameplay; one-shot consumido). Tentei LCS_CAMFOLLOW (chamar CCamera::Process/frame no
gameplay): 
  - 1ª versão (gate state9+!cutscene): CRASH @f14 em CCamera::CamControl()<-CCamera::Process()
    (player ainda NULL na fase pre-cutscene; memcpy 11 bytes NULL — MESMO padrão do CObject crash).
  - 2ª versão (+ gate FindPlayerPed()!=NULL): WEDGE DURO -> watchdog reboot -f do device (recuperou
    sozinho). Chamar CCamera::Process por fora do fluxo da engine trava o Mali/engine.
  ⇒ LCS_CAMFOLLOW é INSTÁVEL — fica gated (default OFF, NÃO afeta a config estável). Follow-cam
    de gameplay precisa de outra abordagem (ver abaixo).

🔑 PADRÃO RECORRENTE = o boss da próxima sessão: memcpy de 11 bytes (x2=0xb) com x0=NULL e x1=
ponteiro-lixo-pequeno (0x105ce/0x12158 = base-NULL+offset) aparece em (a) CObject creation por
script 600-699 (caminho FE25) e (b) CCamera::CamControl (caminho CAMFOLLOW). É um GLOBAL/estado
não inicializado no nosso forced-newgame que vários sub-sistemas leem. Resolver ISSO destrava:
feobj+25-por-frame (scripts+camera vivos) -> gameplay INTERATIVO completo (andar+camera-follow+AI).
LEAD próxima sessão: gdb no device (LCS_NOCRASHHANDLER=1) no memcpy @libc+0x7d8b4, ver qual global
(x1 base) está NULL; provavelmente algo que CGame::Initialise/InitialiseWhenRestarting deveria setar
mas o nosso fluxo (BOUNDSTREAM/forced) pula. Refs: re3 CCamera::CamControl + CObject ctor.

ESTADO ESTÁVEL ENTREGUE (default run30.sh, LCS_NO_FADEHACK=0, SEM CAMFOLLOW/FE25):
boot->menu->2 cutscenes 3D (tocam+finalizam)->GAMEPLAY VISÍVEL (Toni no parque, mundo 3D nVis=49,
HUD, carro, árvores), 0 crash, device estável. Câmera de cutscene OK. Input responde. 
FALTA p/ "jogável pleno": follow-cam + translação confirmada (boss do memcpy-NULL), som in-game
(infra OK: openal+music.wad), empacotar launcher ES (não existe ainda).

═══════════════════════════════════════════════════════════════════════════════
## SESSÃO 7 (2026-06-22): FE25 protegido + câmera/cutscene real + novo blocker no renderer
═══════════════════════════════════════════════════════════════════════════════

### Avanço confirmado
- Patch aplicado em `jni_shim.c`: `LCS_FE25` agora só seta `feobj+25` quando:
  `sf == 9`, `gstate == 2`, `gate != 0`, scripts existem, e **`!lcs_cutscene_active()`**.
  Overrides só por env explícito: `LCS_FE25_UNSAFE` ou `LCS_FE25_DURING_CUTSCENE`.
- Resultado: o crash antigo de script/object durante cutscene foi evitado. Durante a cutscene,
  os logs mostram `ok=0 ... cut=1`, então `CTheScripts::Process` não é liberado cedo demais.
- Teste `run-fe25-cutgate-camprocess-1.log`:
  - Env principal: `LCS_FE25=1 LCS_FE25DIAG=1 LCS_CUTSCENE_CAMPROCESS=1 LCS_CUTSCENE_TIMEDIAG=1`.
  - Run de 120s saiu limpo, sem crash.
  - Screenshot: `~/lcs-build/shot-fe25-cutgate-camprocess-1.png`.
  - Imagem confirma cutscene 3D real: prédios, personagem, semáforo, câmera avançando. Não é mais
    tela parada/sem mundo.

### Reorientação importante
- O problema anterior era misturar duas coisas:
  1. **Câmera da cutscene** precisa de `CCamera::Process`, mas pode ser processada sem rodar scripts.
  2. **Scripts do mundo** (`CTheScripts::Process`) não podem rodar enquanto a cutscene ainda está ativa.
- `LCS_CUTSCENE_CAMPROCESS=1` + `LCS_FE25` protegido separa essas duas fases. Isso preserva muito mais
  fluxo nativo e evita o crash de `CObject`/script 600-699 visto antes.

### Novo blocker mais avançado
Teste limpo e longo: `~/lcs-build/run-fe25-long-clean-1.log`

Env principal:
`LCS_MAXSECONDS=360 LCS_START=newgame LCS_STARTFRAME=12 LCS_GLSTATS=1 LCS_FE25=1
 LCS_FE25DIAG=1 LCS_CUTSCENE_CAMPROCESS=1 LCS_CUTSCENE_TIMEDIAG=1 LCS_SCRIPTPOOL_EXTRA=128
 LCS_FORCE_GAMEPAD_UI=1 LCS_TV_DEVICE=1 LCS_NJOY_COUNT=1`

Resultado:
- O jogo chegou muito mais longe: primeira cutscene rodando até `spline=1.000000`.
- Perto do handoff, a câmera salta para posição pós-cutscene/gameplay:
  `cam=1482.9,-178.5,56.1`, `dScr=1047..1166`, `nVis=0..7`, `fade=1->0`.
- Crash em `f=1640`, ainda com flags de cutscene aparentando ativas (`cut=1/1/0`), mas já renderizando
  a cena pós-corte. Isso é handoff/fade/render, não mais o crash inicial da câmera parada.

Backtrace mapeado:
```text
=== CRASH sig=11 code=-6 addr=0x3de7 pid=15847 uid=0 ===
PC libc.so.6+0x7d8b4 / tgkill self-signal path
x0=(nil) x1=0x3df0 x2=0xb

libGame+0x5950c4 BatchedWorld::C_WorldRenderManager::GetFreeDrawCall(...)
libGame+0x562f18 cWorldStreamEx::DrawPrimitiveRef(...)
libGame+0x5627f0 cWorldStreamEx::RenderNoCull(...)
libGame+0x3a0f08 cWorldStream::Render(...)
libGame+0x3a0080 cWorldStream::Render(int)
libGame+0x385094 CMattRenderer::Render()
libGame+0x522e60 RenderScene()
libGame+0x523e8c DrawGame()
libGame+0x5246ec GameCoreTick(float,bool)
lcs+0x83f0 my_GameCoreTick
```

Conclusão nova:
- O próximo blocker não é mais "PVS/XML" nem "câmera não anda".
- O crash atual é no renderer mobile/batched world, em `GetFreeDrawCall`, durante a transição fim
  de cutscene -> cena pós-cutscene. Parece assert/self-sigsegv do engine ao obter draw call, possivelmente
  pool/lista de draw calls inválida/exaurida ou estado de render/resource creator incoerente no handoff.
- Próximo ataque: comparar init/pool de `BatchedWorld::C_WorldRenderManager` com Bully/GTASA, logar
  limites/contadores de draw calls, e testar reduzir/pausar o salto de render do handoff antes de mexer
  em scripts/controle. Também testar um run sem `LCS_CUTSCENE_CAMPROCESS` e outro com skip nativo para
  saber se o crash é causado pelo camprocess manual ou pelo handoff natural do renderer.

### Comparativo sem CAMPROCESS
Teste: `~/lcs-build/run-fe25-no-camprocess-1.log`

Env principal:
`LCS_MAXSECONDS=280 LCS_WATCHDOG_GRACE=60 LCS_START=newgame LCS_STARTFRAME=12 LCS_GLSTATS=1
 LCS_FE25=1 LCS_FE25DIAG=1 LCS_CUTSCENE_TIMEDIAG=1 LCS_CUTSCENE_TIMEDIAG_STEP=120
 LCS_SCRIPTPOOL_EXTRA=128 LCS_FORCE_GAMEPAD_UI=1 LCS_TV_DEVICE=1 LCS_NJOY_COUNT=1`

Resultado:
- Sem `LCS_CUTSCENE_CAMPROCESS`, não apareceu crash handler. O device travou/reiniciou.
- Último heartbeat persistente: `f=1004 state=9 PRE-draw texMB=63 texN=5705`.
- A cutscene ficou praticamente congelada no meio:
  `clock=36.409`, `spline=0.642`, `cam=1044.7,-675.3,18.7`, `cmode=17`, `cut=1/1/0`.
- A câmera/draw state ficou repetindo a mesma posição por muitos frames antes do reboot.

Conclusão do comparativo:
- `LCS_CUTSCENE_CAMPROCESS=1` é necessário para a primeira cutscene continuar até o fim/handoff.
- Mas deixar `CCamera::Process` rodando manualmente até o último frame parece empurrar o engine para
  um estado misto: spline/câmera já em pós-cutscene, flags de cutscene ainda ativas, renderer tentando
  desenhar o mundo novo e caindo em `BatchedWorld::GetFreeDrawCall`.
- Próximo teste técnico: limitar/desligar o `CAMPROCESS` perto do fim da spline e/ou finalizar/limpar
  a cutscene quando `spline` chega em ~0.98..1.00, antes do renderer entrar no handoff incoerente.

### s7 BREAKTHROUGH: FINISHPOS passou as 2 cutscenes e chegou em gameplay real
Patch/teste novo em `jni_shim.c`:
- `LCS_CUTSCENE_CAMPROCESS_STOPPOS=N`: para o processamento manual da câmera quando a spline/pos
  chega no limiar.
- `LCS_CUTSCENE_FINISH_POS=N`: chama `FinishCutscene` quando a spline/pos chega no limiar, antes
  do renderer ficar preso no estado misto do handoff.
- Log `[splinefix]` agora imprime `cur/target/after/write`, para distinguir contador avançando de
  câmera realmente aplicada.

Run que funcionou:
`~/lcs-build/run-fe25-finishpos-1.log`

Env principal:
`LCS_MAXSECONDS=260 LCS_WATCHDOG_GRACE=60 LCS_START=newgame LCS_STARTFRAME=12 LCS_GLSTATS=1
 LCS_FE25=1 LCS_FE25DIAG=1 LCS_CUTSCENE_CAMPROCESS=1 LCS_CUTSCENE_CAMPROCESS_STOPPOS=0.960
 LCS_CUTSCENE_FINISH_POS=0.985 LCS_CUTSCENE_TIMEDIAG=1 LCS_CUTSCENE_TIMEDIAG_STEP=60
 LCS_SCRIPTPOOL_EXTRA=128 LCS_FORCE_GAMEPAD_UI=1 LCS_TV_DEVICE=1 LCS_NJOY_COUNT=1
 LCS_SHOT_FINAL_WINDOW=10`

Resultado:
- Run saiu limpo: `=== run30 done (2 teardown) ===`.
- Screenshot: `~/lcs-build/shot-fe25-finishpos-1.png`.
- Imagem confirma gameplay real: Toni em terceira pessoa, HUD, minimapa, carro, árvores e mundo 3D.
- 1ª cutscene finalizou por posição:
  `f=1691 pos-finish request finish_pos=0.985 clk=55.820 dur=56.667 pos=0.985051`.
- 2ª cutscene finalizou por posição:
  `f=3950 pos-finish request finish_pos=0.985 clk=73.111 dur=74.167 pos=0.985760`.
- Gameplay pós-2ª-cutscene:
  `f=4080 cut=0/0/0 fade=2 nVis=20 cam=1413.4,-197.7,50.4 pedpos=1433.5,-195.9,52.9 pop=1 cars=1`
  e depois `f=4125 fade=0 nVis=20`, `f=4195 fade=0 nVis=20`.

Interpretação:
- Este é o melhor marco até agora: fluxo real `newgame` -> 2 cutscenes 3D -> gameplay com HUD e mundo.
- `FINISHPOS` é mitigação prática do handoff, não solução final nativa. O mesmo ponto ainda pode ser
  sensível a timing se outra flag mexer no frame exato do fim da cutscene.
- O movimento de `pedpos` logo depois do corte ainda não prova controle; pode ser reposicionamento/script.
  Precisa teste de eixo nativo no gameplay.

### s7 CONTROLE: não usar PADBRIDGE_DIRECT; próximo teste é eixo JNI nativo
Teste com `LCS_PADBRIDGE_DIRECT=1` crashou antes de qualquer input ser injetado:
`~/lcs-build/run-fe25-finishpos-padbridge-crash-1.log`.

Crash:
```text
libGame+0x5950c4 BatchedWorld::C_WorldRenderManager::GetFreeDrawCall(...)
x0=(nil) x1=0x13d2 x2=0xb
```

Notas:
- Não houve `[probe]` nem input antes do crash; portanto o crash não prova falha do controle.
- O crash foi o mesmo renderer/handoff, com timing diferente: a primeira cutscene finalizou mais cedo
  (`f=1621`) e as flags de cutscene reativaram antes do handoff estabilizar.
- `LCS_PADBRIDGE_DIRECT` fica proibido por enquanto. Testar controle pelo caminho JNI nativo:
  `/dev/shm/lcs_axis` -> `setJoyAxis`, sem CPad direto.

Patch de input aplicado:
- `LCS_OS_GamepadIsConnected()` agora retorna conectado quando existe eixo/probe ativo, mesmo sem
  SDL GameController físico.
- `pump_input()` não aborta mais o envio de eixo sem `g_pad`; ele monta `g_axis_state` e chama
  `setJoyAxis` pelo caminho JNI da engine.

Próximo run recomendado:
```sh
ssh -F /dev/null -o StrictHostKeyChecking=no root@<device-ip> 'cd /storage/roms/ports/lcs || exit 1; ( sleep 245; printf "1 -1.0 420\n" > /dev/shm/lcs_axis; sleep 8; printf "0 0.8 240\n" > /dev/shm/lcs_axis ) & LCS_MAXSECONDS=310 LCS_WATCHDOG_GRACE=60 LCS_START=newgame LCS_STARTFRAME=12 LCS_GLSTATS=1 LCS_FE25=1 LCS_FE25DIAG=1 LCS_CUTSCENE_CAMPROCESS=1 LCS_CUTSCENE_CAMPROCESS_STOPPOS=0.960 LCS_CUTSCENE_FINISH_POS=0.985 LCS_CUTSCENE_TIMEDIAG=1 LCS_CUTSCENE_TIMEDIAG_STEP=120 LCS_SCRIPTPOOL_EXTRA=128 LCS_FORCE_GAMEPAD_UI=1 LCS_TV_DEVICE=1 LCS_NJOY_COUNT=1 LCS_PADBRIDGE_DIRECT=0 LCS_PADDIAG=1 LCS_AXIS_DEADZONE=0.10 LCS_SHOT_FINAL_WINDOW=10 sh ./run30.sh'
```

### s7.1 UPDATE_OVERLAY reconciliado: 2 cutscenes -> gameplay por 300s sem crash

Nova descoberta importante:
- O crash pós-2ª-cutscene em `CCutsceneMgr::Update_overlay -> LoadCutsceneData_loading ->
  CreateCutsceneObject -> CEntity::CEntity -> memcpy(NULL, ..., 11)` não era falta de mundo nem
  controle. Era estado de load de cutscene sobrando depois do finish forçado.
- Tentativa descartada: instalar wrapper de `CCutsceneMgr::Update_overlay` desde o boot causou wedge
  cedo em `f=18` (`~/lcs-build/run-updateoverlay-early-wedge-1.log`). Não usar esse wrapper no fluxo
  normal.
- Patch que funcionou: quando as cutscenes obrigatórias terminam, `lcs_cutscene_note_finished()` chama
  `lcs_cutscene_post_finish_reconcile()` e limpa apenas o estado de load:
  `ms_cutsceneLoadStatus=2->0`, `ms_numLoadObjectNames=4->0`, preservando `ms_numCutsceneObjs=11`.

Run estável:
- `~/lcs-build/run-postreconcile-delay60-1.log`
- `~/lcs-build/progress-postreconcile-delay60-1.txt`
- `shot_gameplay.raw/txt` no device depois do run

Resultado:
- 1ª cutscene finalizada: `f=1470 pos=0.985488`.
- 2ª cutscene finalizada: `f=3547 pos=0.985254`.
- Reconcile aplicado: `post-finish reconcile finish f=3547 fin=2 load=2->0 play=0->0 names=4->0 objs=11->11`.
- Gameplay de 300s saiu limpo com teardown Mali: `=== run30 done (2 teardown) ===`.
- Mundo 3D visível em gameplay (`nVis=40..49`) e sem `CRASH`.
- `LCS_GAMEPLAY_RELEASE_DELAY=60` é o valor seguro atual. Com `45`, um run ficou preso depois da
  primeira transição (`~/lcs-build/run-postreconcile-firsttransition-stuck-1.log`).

Estado atual de controle:
- O caminho JNI de eixo chega no engine: logs mostram `axis=1.00`, `gaxis=0.00,1.87`, `cpad=127`.
- Ainda não há prova de translação real do Toni; `pedpos` ficou praticamente igual nesse run. Próximo
  alvo: mapear eixo/botões para movimento jogável sem reativar `PADBRIDGE_DIRECT`.

Correção aplicada após observação do porter:
- `LCS_NOON=1` congelava o HUD em 12:00 durante gameplay, porque escrevia `CClock` todo frame.
- Agora `LCS_NOON` é pulso de boot limitado por `LCS_NOON_FRAMES` (default 300). Para reproduzir o
  hack antigo, usar `LCS_NOON_STICKY=1`.

### s7.7 Marco jogavel: camera OK + analogico esquerdo andando no controle fisico
O porter confirmou o run em tela: "tudo rodando bem".

Estado confirmado:
- Fluxo nativo preservado: `LCS_START=newgame`, sem remover cutscene nem forcar estado cru.
- Cutscenes puladas por Start nativo (`/dev/shm/lcs_btn` enum `9`) ate 2
  `FinishCutscene called`.
- Camera OK no run de cutscene/camera completo.
- Depois dos pulos, gameplay renderiza mundo: `cut=0/0/0`, `fade=0`, `nVis=47..55`.
- Analogico esquerdo fisico chegou como raw axes 0/1 e moveu Toni: `[input] ... src=raw`.
- D-pad nao alimenta mais movimento por default (`LCS_DPAD_AS_AXIS_ONLY=0`) e L2/R2 nao entram
  como botoes (`LCS_TRIGGER_BUTTONS=0`).

Artefatos salvos:
- `~/lcs-build/run-camera-ok-fullprofile-2026-06-22.log`
- `~/lcs-build/progress-camera-ok-fullprofile-2026-06-22.txt`
- `~/lcs-build/run-playable-leftanalog-skipnative-2026-06-22.log`
- `~/lcs-build/progress-playable-leftanalog-skipnative-2026-06-22.txt`

Trechos chave:
```text
FinishCutscene called f=2279
FinishCutscene called f=3670
[input] f=4278 state=9 axis0=-0.875 src=raw
[input] f=4284 state=9 axis1=-0.773 src=raw
[glstats] f=4308 ... cut=0/0/0 ... nVis=53 ... pedpos=1417.2,-200.7,50.4
```

Script jogavel novo no repo/device:
```sh
sh /storage/roms/ports/lcs/run-playable.sh
```

Esse script usa perfil leve para jogar:
- `LCS_GLSTATS=0`, `LCS_INPUTDIAG=1`.
- `LCS_START=newgame LCS_STARTFRAME=120`.
- Auto-pulso de Start (`9`) ate 2 `FinishCutscene called`.
- `LCS_MOVE_RAW=1 LCS_MOVE_AXIS_X=0 LCS_MOVE_AXIS_Y=1`.
- `LCS_DPAD_AS_AXIS_ONLY=0 LCS_TRIGGER_BUTTONS=0`.

Proximos focos, sem mexer no que ja ficou bom:
- Reduzir stutter/streaming no mapa, comparando limites do loader com Bully/GTASA.
- Investigar texturas pretas/piscando em ruas/cutscenes.
- Confirmar mapa completo, pedestres, carros e interiores conforme o streaming avanca.

### s7.6 Controle correto: D-pad nao deve andar; analogico esquerdo deve andar
O porter testou o build s7.5 e apontou o detalhe certo:
- D-pad estava andando.
- Analogico esquerdo estava com funcao errada/zoom.
- O alvo correto e inverter isso: D-pad volta a ser botao nativo/zoom e analogico esquerdo vira
  movimento.

Referencia GTASA Vita estudada:
- O port oficial trata controles como mapeamento configuravel (`controls.txt`).
- No esquema Vita-enhanced/Xbox 360: `MAPPING_PED_MOVE_X = ANALOG_LEFT_X`,
  `MAPPING_PED_MOVE_Y = ANALOG_LEFT_Y`, `MAPPING_LOOK_X/Y = ANALOG_RIGHT_X/Y`.
- Conclusao para LCS: nao prender movimento ao D-pad fisico; usar fonte analogica para `CPad`.

Patch aplicado:
- `run30.sh`: default `LCS_DPAD_AS_AXIS_ONLY=0`. O D-pad fisico nao e mais convertido para
  `axis0/axis1` de movimento por default.
- `run30.sh`: default `LCS_MOVE_RAW=1`, `LCS_MOVE_AXIS_X=0`, `LCS_MOVE_AXIS_Y=1`.
- `src/jni_shim.c`: abre tambem `SDL_Joystick` raw alem de `SDL_GameController`.
- `src/jni_shim.c`: movimento (`axis0/axis1`) pode vir de raw axes configuraveis; logs agora
  distinguem `src=raw`, `src=sdl`, `src=dpad`, `src=probe`.
- `src/jni_shim.c`: `LCS_RAWAXISDIAG=1` loga `[rawaxis] f=... a0=... a1=... a2=... a3=...`.

Run aberto para teste no device:
```sh
LCS_MAXSECONDS=420 LCS_WATCHDOG_GRACE=70 LCS_START=newgame LCS_STARTFRAME=120 \
LCS_INPUTDIAG=1 LCS_INPUTDIAG_START=1 LCS_INPUTDIAG_MAX=800 \
LCS_PADDIAG=1 LCS_RAWAXISDIAG=1 LCS_RAWAXISDIAG_EVERY=12 LCS_RAWAXISDIAG_MAX=360 \
LCS_DPAD_AS_AXIS_ONLY=0 LCS_MOVE_RAW=1 LCS_MOVE_AXIS_X=0 LCS_MOVE_AXIS_Y=1 \
LCS_AXIS_DEADZONE=0.12 LCS_GLSTATS=0 sh ./run30.sh
```

Evidencia inicial:
```text
[run30] ... dpad_axis_only=0 ... padmove=1 padclear162=1 move_raw=1 move_axes=0,1 ...
[pad] raw "USB Gamepad" axes=4 hats=1 buttons=12
[rawaxis] f=132 state=9 axes=4 a0=0.004 a1=0.004 a2=0.004 a3=0.004
```

Proximo teste/criterio:
- Mexer analogico esquerdo: esperar `[rawaxis] a0/a1` mudar e `[input] axis0/axis1 ... src=raw`.
- D-pad: nao deve aparecer mais `dpad-as-axis`; se o jogo nativo aceitar o enum, deve voltar a
  zoom/menu/acao nativa em vez de andar.
- Se analogico esquerdo nao mexer raw `0/1`, calibrar sem recompilar:
  `LCS_MOVE_AXIS_X=2 LCS_MOVE_AXIS_Y=3` ou inverter por patch pequeno se o sinal vier trocado.

### s7.5 Movimento visual comprovado: swap nativo fazia o ped ler D-pad, nao stick
Nova descoberta a partir do sintoma relatado:
- Analogico direito movia a camera perfeitamente.
- D-pad e analogico esquerdo nao moviam Toni.
- L2 fechava o jogo.

Assembly confirmou o motivo do movimento parado:
- `CPad::m_bSwapNippleAndDPad` altera `GetPedWalkLeftRight/UpDown`.
- Com o swap ativo, essas funcoes leem offsets de D-pad (`18/20/22/24`) em vez dos offsets do stick
  (`2/4`).
- Nosso perfil estavel tinha `LCS_DPAD_AS_AXIS_ONLY=1`, entao os botoes D-pad 12..15 eram
  suprimidos em gameplay para evitar o caminho instavel. Resultado: camera recebia RX/RY, mas
  movimento do ped ficava sem fonte.

Patch aplicado:
- `LCS_PADBRIDGE_MOVE=1` por default no `run30.sh`: em state 9, espelha `g_axis_state[0/1]` nos
  offsets de stick (`2/4`) e tambem nos offsets de D-pad que `GetPedWalk*` le com swap ativo
  (`18/20/22/24` e pares auxiliares `26/28/30/32`).
- `LCS_TRIGGER_BUTTONS=0` por default: L2/R2 continuam como eixos 4/5, mas nao sao enviados como
  botoes 6/7 ate o enum real ser confirmado.
- `LCS_INPUT_PROBE_ONLY=1` adicionado para teste automatizado isolado; ignora SDL fisico e aceita
  apenas `/dev/shm/lcs_btn`/`lcs_axis`. Nao e o modo jogavel normal.

Prova visual limpa:
- Run com `probe_only=1`, fluxo nativo `newgame`, 2 cutscenes puladas por Start, `CRASH=0`.
- `~/lcs-build/shot-walk4-before-2026-06-22.png`: gameplay com HUD, Toni colado na traseira do carro.
- `~/lcs-build/shot-walk4-after-2026-06-22.png`: depois de `LY=+1.00`, Toni saiu da traseira do
  carro e a camera acompanhou.
- Logs:
  `~/lcs-build/run-walk4-proof-2026-06-22.log`,
  `~/lcs-build/progress-walk4-proof-2026-06-22.txt`.
- Linha-chave no log: `move=1 lx=0.00 ly=1.00 UDLR=0100`, processo vivo e sem `CRASH`.

### s7.2 START nativo + controle: caminho de teste jogavel confirmado
A pergunta do porter estava certa: se o objetivo e controle/gameplay, o melhor teste e preservar o
fluxo `newgame` e pular as cutscenes com `Start` nativo, nao remover cutscene nem forcar estado cru.

Runs novos preservados:
- `~/lcs-build/run-stable-native-axis.full.log`
- `~/lcs-build/run-stable-axis2.full.log`
- `~/lcs-build/run-control-clean2.full.log` (prova limpa: eixo so depois de 2 cutscenes + gameplay visivel)
- `~/lcs-build/progress-stable-axis2.txt`

Perfil que voltou ao estado estavel:
`LCS_START=newgame LCS_STARTFRAME=120 LCS_STREAMER_MAX=160 LCS_RESOURCEDRAIN_MAX=8
 LCS_ENABLE_POP=1 LCS_ENABLE_HELI=1 LCS_ENABLE_USERDISPLAY=1 LCS_NO_FADEHACK=0
 LCS_CUTSCENE_PAD_SKIP=1 LCS_CUTSCENE_SPLINEFIX=1 LCS_CUTSCENE_CLEAR_AFTER_FINISH=1
 LCS_CUTSCENE_RESTORE_CAMERA=1 LCS_GLSTATS=1`

Importante:
- Nao usar o combo agressivo `STARTFRAME=12 + FE25/FE25_POSTREADY + POP/HELI off` para teste de
  controle. Esse caminho passou do f113 com `STREAMER_MAX=1`, mas voltou a travar em wait do streamer
  perto de f154 e deixou camera/fade ruins.
- No injetor de botao, escrever `0` em `/dev/shm/lcs_btn` NAO e release; e apertar o enum 0. Para
  pular cutscene, escrever somente `9` em pulsos ate `FinishCutscene called` aparecer 2 vezes.
- O marcador `note finished count=2` nao aparece nesse perfil. Use `grep -c "FinishCutscene called"`
  ou gameplay visivel (`s=9`, `fade=0`, `nVis>0`, `ped=0x...`) como gatilho.

Resultado do `run-stable-native-axis.full.log`:
- Passou pelo fluxo e ficou em gameplay por milhares de frames sem crash.
- Final limpo por timeout/teardown (`RUN_RC=124` esperado pelo timeout externo), sem Mali wedge.
- Gameplay estavel: `fade=0`, `nVis=47..49`, `dScr~615..619`, camera real
  `cam=1425.6,-195.6,51.3`, Toni em `pedpos=1420.1,-195.2,50.3`.

Resultado do `run-stable-axis2.full.log`:
- O caminho de input JNI funciona. Log:
  `[probe] axis 0 = 1.000 hold=600` ->
  `axis=1.00`, `gaxis=0.00,1.88,0.00,0.00`, `cpad=128,0,0,0`.
- Com eixo ativo, o ped se moveu em gameplay visivel: exemplo `pedpos=1431.2,-195.8,52.2`
  indo para `1429.x...` enquanto `fade=0`, `nVis=20`, `dScr=339..340`.
- Depois que o hold acabou, o log voltou para `axis=0`, `gaxis=0`, `cpad=0` e o ped estabilizou
  perto de `1421.1,-195.2,50.2`.

Resultado limpo do `run-control-clean2.full.log`:
- 2 cutscenes puladas por `Start` nativo: `FinishCutscene called f=173` e `f=198`.
- Eixo disparou somente depois de gameplay visivel: `[probe] axis 0 = 1.000 hold=600` em `f=434`,
  ja com `cut=0/0/0`, `fade=0`, `nVis=20`, `dScr=341`.
- `CPad` recebeu o input: `axis=1.00`, `gaxis=0.00,1.88,0.00,0.00`, `cpad=128,0,0,0`.
- Toni andou de `pedpos=1427.4,-195.6,51.1` para `1425.x,-195.5,50.x` enquanto o eixo estava
  ativo, e depois estabilizou perto de `1421.0,-195.2,50.2` quando o hold acabou.

Conclusao atual:
- Controles por eixo chegam ao engine e movem Toni; camera acompanha em gameplay.
- O shim ja le `SDL_GameController` e envia eixos via `setJoyAxis`; falta testar/calibrar o controle
  fisico real nesse perfil estavel, com `LCS_PADDIAG=1`, `LCS_AXIS_DEADZONE=0.10..0.18` e
  `PADBRIDGE_DIRECT=0` (continua proibido por timing/crash renderer).
- Para proximos testes, pular as cutscenes com `Start` nativo e injetar eixo so depois de
  `FinishCutscene called` x2 + `fade=0/nVis>0`, ou o eixo pode disparar durante handoff/fade.

### s7.3 Travamento ao apertar botao: nao era crash de controle, era I/O diagnostico pesado
Teste pedido pelo porter ("assim que tento andar aperto o botao ele trava") reproduziu uma pista
importante: o input fisico chegava ao jogo, mas a thread principal ficava parada em syscall de disco.

Estado observado no device antes do patch:
```text
[padbridge] UpdatePads state=9 mask=0x10 direct=0 sel=0 back=0 UDLR=0010
/proc/<pid>/stack:
  sleep_on_page
  filemap_fdatawait_range
  filemap_write_and_wait_range
  fat_file_fsync
  SyS_fdatasync
```

Conclusao:
- O controle nao crashou o engine. O direcional chegou (`mask=0x10`) e o processo continuava vivo.
- O falso "freeze ao apertar" vinha do heartbeat fazendo `fdatasync()` em todo frame no VFAT/SD.
- Comparacao com Bully confirmou o padrao correto: o port funcional nao faz `fdatasync` por frame;
  ele loga frame de forma espacada (`f < 5 || f % 120 == 0`) e deixa o loop de gameplay leve.

Patch aplicado em `src/jni_shim.c`:
- `hb()` agora escreve heartbeat leve por default: `LCS_HB_EVERY=30`, sem fsync.
- Modo forense antigo ainda existe quando precisar caçar wedge/reboot:
  `LCS_HB_EVERY=1 LCS_HB_FSYNC=1`.
- `restartguard` tambem foi reduzido para 4 logs por default; spam completo so com
  `LCS_RESTARTDIAG=1`. Esta limpeza ja compila localmente e entra no proximo deploy/restart
  (a sessao aberta no device nao foi interrompida).

Run aberto para playtest com binario novo:
- Device: PID `18954`.
- Snapshot local:
  - `~/lcs-build/run-playtest-hb-light-2026-06-22.snapshot.log`
  - `~/lcs-build/progress-playtest-hb-light-2026-06-22.snapshot.txt`
  - `~/lcs-build/heartbeat-playtest-hb-light-2026-06-22.snapshot.txt`
- Perfil: mesmo fluxo estavel, `GLSTATS=0`, `PADDIAG=1`, `PADBRIDGE_DIRECT=0`,
  `LCS_HB_EVERY=30`, sem `LCS_HB_FSYNC`.
- Resultado: 2 cutscenes puladas por Start nativo (`FinishCutscene called f=165` e `f=191`),
  input fisico apareceu depois (`mask=0x10`) e o jogo continuou avancando ate pelo menos
  `f=7544 state=9 post-draw`. Sem `CRASH` no snapshot.

Nova regra:
- Perfil jogavel nunca deve usar `LCS_GLTRACE`, `LCS_DRAWHB` ou `LCS_HB_FSYNC`.
- Heartbeat com fsync e GL trace ficam apenas para investigacao curta de wedge, nao para gameplay.

### s7.4 Repro direta do "apertou direcional/analogico": input entra, jogo nao crasha no perfil atual
Pedido do porter: pular as cutscenes com Start, chegar rapido no gameplay e provocar o erro com
direcional/analogico para capturar o ponto exato.

Mudancas aplicadas:
- `run30.sh` agora exporta/loga `LCS_DPAD_AS_AXIS_ONLY` e `LCS_INPUTDIAG`.
- `src/jni_shim.c` ganhou `LCS_INPUTDIAG=1`: log leve de borda de botao/eixo com `frame`, `state`,
  origem `phys/probe`, e supressao de D-pad fisico quando ele e convertido para eixo.
- Em gameplay (`state=9`), D-pad fisico fica por default como eixo (`LCS_DPAD_AS_AXIS_ONLY=1`).
  Para voltar ao comportamento antigo: `LCS_DPAD_BUTTONS=1` ou `LCS_DPAD_AS_AXIS_ONLY=0`.

Artefatos:
- `~/lcs-build/run-button-axis-no-crash-2026-06-22.log`
- `~/lcs-build/progress-button-axis-no-crash-2026-06-22.txt`
- `~/lcs-build/run-inputdiag2-after-input-2026-06-22.log`
- `~/lcs-build/progress-inputdiag2-after-input-2026-06-22.txt`

Resultado da reproducao controlada:
```text
FinishCutscene called f=178
FinishCutscene called f=205
[input] f=252 state=9 button=14 DOWN phys=0 probe=1
[input] f=260 state=9 button=15 DOWN phys=0 probe=1
[input] f=329 state=9 axis0=1.000 src=probe
[input] f=419 state=9 axis1=-1.000 src=probe
```

Resultado com controle fisico real aparecendo no mesmo run:
```text
[input] f=741 state=9 axis0=-1.000 src=sdl
[input] f=801 state=9 dpad-as-axis dx=-1 dy=0 buttons-suppressed=1
[input] f=867 state=9 button=0 DOWN phys=1 probe=0
[input] f=927 state=9 button=1 DOWN phys=1 probe=0
[input] f=1006 state=9 button=6 DOWN phys=1 probe=0
```

O processo continuou vivo e avançando depois disso:
```text
progress3: f=2144 state=9 post-draw
grep -c CRASH run.log => 0
```

Conclusao atual:
- O crash/freeze de input nao reproduz mais no caminho atual. Botao, D-pad, analogico injetado e
  input fisico real chegaram ao engine em gameplay sem `CRASH`.
- A maior causa confirmada do "travou ao apertar" era o `fdatasync()` por frame. A outra defesa
  necessaria e nao deixar D-pad fisico entrar como botao direto em gameplay; ele vira eixo, igual
  o padrao pratico visto em Bully/GTAVC (separar botao/evento de movimento analogico).
- Se voltar a travar no device, manter `LCS_INPUTDIAG=1 LCS_PADDIAG=1` e olhar o ultimo `[input]`
  antes do travamento. Se houver SIGSEGV, o crash handler ja imprime PC/LR/libGame offset.

### s7.8 Quadrados/planos no chao/cenario: overlay PVS/debug ligado, nao textura faltando
Pedido do porter: investigar os quadrados pretos/planos no chao, ruas, primeira cutscene do onibus
e gameplay. Tambem foi pedido estudar GTASA porque ele reduz sombras/detalhe para rodar liso no
Mali-450.

Diagnostico:
- O screenshot anterior `~/lcs-build/shot-stable-after-l2-start-gfxsafe.png` mostrava planos
  translúcidos enormes atravessando rua/predios. Isso parecia pass de alpha/PVS/debug, nao simples
  textura rosa/preta.
- Foi adicionado `LCS_ALPHA_DIAG` para contar `cWorldStream::RenderAlpha` e `LCS_PVS_CLEAN` para
  zerar apenas visualizacao/debug PVS.
- O run `~/lcs-build/run-pvs-clean-alpha-diag-2026-06-22.log` confirmou no boot:
  ```text
  [pvs] clean debug draws rz=...(0) rw=...(1) rc=...(1) alpha=...(0.100)
  ```
  Ou seja: `dvPVSRenderWorldZones=1`, `dvPVSRenderCameraZones=1` e `dvPVSZonesAlpha=0.100`
  estavam ligados. O engine estava desenhando as zonas PVS por cima do mundo.
- `RenderAlpha` tambem e muito usado (`wsa` cresce milhares), mas nao deve ser desligado: ele e
  necessario para foliage/arvores/alpha normal. O problema corrigido foi o overlay/debug de PVS.

Patch aplicado:
- `src/jni_shim.c`:
  - adicionou `dv_f32_get/set`;
  - corrigiu escrita de `dvDebug*` para offsets internos:
    bool em `+61`, s32/float em `+76`;
  - adicionou `lcs_apply_pvs_debug_cleanup()`:
    `dvRenderPVSZones=0`, `dvPVSRenderWorldZones=0`, `dvPVSRenderCameraZones=0`,
    `dvPVSZonesAlpha=0.0`, `dvRenderPVSdStuffAsPink=0`, `dvDebugWorldShader=0`,
    `dvRenderWorldBoundingBoxes=0`, `dvRenderWorldParentBoundingBoxes=0`;
  - adicionou hook opcional `LCS_ALPHA_DIAG`/`LCS_NO_WORLD_ALPHA` em
    `cWorldStream::RenderAlpha` apenas para diagnostico.
- `run30.sh` e `run-playable.sh`:
  - `LCS_PVS_CLEAN=1` por default;
  - `LCS_GFX_PREFS=1` por default;
  - `LCS_GFX_LOW=0` mantido por default.

GTASA/Bully aplicado:
- Estudo local de GTASA Vita mostrou o padrao certo para Mali: reduzir detail/shadows/LOD em vez de
  ligar efeitos caros. Referencias locais:
  - `/home/root/gtavc-build/refs/gtasa_vita/loader/config.c`
  - `/home/root/gtavc-build/refs/gtasa_vita/loader/main.c`
  - docs internas: "ESTUDO - GTA SA vs Bully no Mali-450 (streaming, DXT, texturas)"
- `LCS_GFX_PREFS=1` agora aplica o perfil leve:
  - menu prefs: dynamic shadows/reflections/detail/LOD off/baixo;
  - overrides de shadow/detail/game detail;
  - `CCutsceneMgr::ms_useCutsceneShadows=0`;
  - `gRenderDynamicShadows=0`, `dvbRenderStoredShadows=0`,
    `dv_RenderWorldIntoShadowMap=0`, `dv_bShowShadowMap=0`;
  - `dvLodDistanceScale=0.75`, `ALLOW_LOD_REDUCTION=1`.
- `LCS_GFX_LOW=1` continua proibido como default: o perfil agressivo com no-op de sombras/reflexos
  crashou no boot (`libGame+0x730090` via libc memcpy/null). Proximo passo nele deve ser granular,
  um subsistema por vez.

Evidencia:
- `~/lcs-build/shot-pvs-clean-alpha-diag-2026-06-22.png`: gameplay limpo sem os planos PVS.
- `~/lcs-build/shot-pvs-clean-gfxprefs-2026-06-22.png`: perfil leve `PVS_CLEAN=1/GFX_PREFS=1`
  renderizando Toni, carro, HUD, arvores e chao sem o overlay gigante.
- `~/lcs-build/run-pvs-clean-gfxprefs-final-2026-06-22.log`: run leve de 150s, finalizou por
  `LCS_MAXSECONDS`, `Mali teardown OK`, sem `CRASH`.
- `~/lcs-build/progress-pvs-clean-gfxprefs-final-2026-06-22.txt`.

Estado atual no device:
- Binario novo deployado.
- `/storage/roms/ports/lcs/run30.sh` e `run-playable.sh` atualizados.
- Repetir estado atual:
  ```sh
  sh /storage/roms/ports/lcs/run-playable.sh
  ```

### s7.9 - 2026-06-23 - shadow pass granular para os blocos pretos no chao

Motivacao:
- O porter fotografou o gameplay com farol apagado/aceso e notou que o chao quebrado corrigia
  quando o farol do carro iluminava a pista.
- Isso muda o diagnostico: o piso/asset esta presente, mas recebe uma mascara escura grande
  e dura por cima. O problema e coerente com shadow/lighting pass quebrado no Mali-450.
- O teste mais simples sugerido foi a primeira cutscene do onibus: se o quadrado preto da pista
  sumir ali, o mesmo fix tende a resolver o jogo todo.

Estudo GTASA:
- O repo GTASA Vita local confirma o padrao de Mali: reduzir efeitos e shaders caros de forma
  seletiva. Defaults relevantes em `loader/config.c`:
  `disable_detail_textures=1`, `disable_tex_bias=1`, `disable_ped_spec=1`.
- O GTASA tambem tem fix especifico de sombra classica em `loader/gfx_patch.c`; portanto o caminho
  correto para LCS e granular, nao usar o antigo `LCS_GFX_LOW=1` inteiro.

Patch aplicado:
- Nova chave `LCS_SHADOWS_OFF`:
  - default `1` em `run30.sh` e `run-playable.sh`;
  - reversivel com `LCS_SHADOWS_OFF=0`.
- Em `src/jni_shim.c`, quando `LCS_SHADOWS_OFF=1`:
  - zera `dv_renderEntityShadow_BUILDING/VEHICLE/PED/OBJECT/MULTI/MULTIVEHICLE/NOTHING`;
  - zera `dv_shadowStrength`, `dv_fRecieveShadowsRadius`,
    `CurrentShadowStrengthDampVal`, `CurrentLightShadowStrengthDampVal`,
    `CurrentPoleShadowStrengthDampVal`;
  - no-op apenas em:
    `CShadows::RenderStoredShadows`,
    `CShadows::RenderStaticShadows`,
    `CShadows::RenderExtraPlayerShadows`.
- `LCS_GFX_LOW=1` continua proibido como default; ele no-opava subsistemas demais e ja crashou no boot.

Evidencia:
- Run com `LCS_SHADOWS_OFF=1`, `newgame`, sem helper de Start automatico, deixou a primeira cutscene
  correr pelo fluxo nativo.
- Screenshot salvo:
  `~/lcs-build/shot-bus-shadowoff.png`.
  A rua/pista aparece limpa, sem o bloco preto grande visto antes.
- Screenshot posterior:
  `~/lcs-build/shot-bus-shadowoff-2.png`, segunda cena/interior renderizando normalmente.
- Screenshot final do run jogavel default:
  `~/lcs-build/shot-playable-shadowoff-default.png`, Toni/carro/HUD/chao renderizando sem os
  poligonos pretos grandes.
- A captura manual agora usa `LCS_SHOT_KEEP_LAST=1` por default, evitando o problema de screenshot
  antiga "melhor" sobrescrever a comparacao.

Estado atual:
- Binario novo compilado e deployado.
- Scripts deployados com `LCS_SHADOWS_OFF=1` por default.
- Proximo run jogavel deve ser:
  ```sh
  sh /storage/roms/ports/lcs/run-playable.sh
  ```

Proximos focos:
- Testar fisicamente controles completos em gameplay com os defaults novos:
  `PVS_CLEAN=1`, `GFX_PREFS=1`, `TRIGGER_BUTTONS=0`, `TRIGGER_AXES=0`.
- Resolver Start/pause menu se ainda nao abrir no controle fisico.
- Investigar o marcador/triangulo escuro visto no alto da captura `shot-pvs-clean-gfxprefs`; pode ser
  marker nativo de objetivo/carro, mas deve ser checado depois que movimento e pausa estiverem bons.
- Performance/stutter: agora que o overlay PVS saiu, medir com perfil leve antes de qualquer corte
  agressivo. Nao usar `GLSTATS`/`RENDERDIAG` em playtest normal porque eles reduzem muito o ritmo.

### s7.10 - 2026-06-23 - SHADOWS_OFF completo para blocos pretos projetados

Nova evidencia:
- As fotos do porter (`photo_2026-06-23_00-24-09.jpg`,
  `photo_2026-06-23_00-24-08.jpg`, `photo_2026-06-23_00-24-08 (2).jpg`)
  mostram poligonos escuros grandes com borda dura sobre o chao. O padrao segue o mundo
  e muda com farol/luz, entao e projeção/sombra, nao textura base ausente.
- O primeiro `LCS_SHADOWS_OFF` ainda era parcial: desligava render de sombras armazenadas,
  mas deixava `CDynamicShadows`, `CCutsceneObject::CreateShadow`,
  `CShadows::UpdateStaticShadows` e `CShadows::UpdatePermanentShadows` vivos.

Patch incremental:
- `LCS_SHADOWS_OFF=1` agora tambem no-opa:
  `CCutsceneObject::CreateShadow`,
  `CDynamicShadows::Begin/End/UpdateCamera/PreRenderSetup`,
  `CShadows::UpdateStaticShadows`,
  `CShadows::UpdatePermanentShadows`.
- `LCS_GFX_LOW=1` continua fora do default. Coronas/bloom/reflections ficaram fora deste patch
  porque o defeito confirmado e sombra/projecao; desligar efeitos demais pode mascarar outros bugs.

Validacao:
- Build compilada e deployada no device.
- Log confirma todos os hooks `SHADOWS_OFF NO-OP`, incluindo dynamic/cutscene/update.
- Screenshot de gameplay atual:
  `~/lcs-build/shot-fullshadowoff-gameplay.png`.
  Toni/carro/HUD/chao renderizam e nao aparecem os poligonos pretos grandes das fotos.
- O run atual pulou as cutscenes muito cedo pelo fluxo de input/start, entao a primeira cutscene
  do onibus ainda precisa de um run especifico sem auto-skip para validar aquele ponto exato.

Estado deixado:
- Device rodando gameplay com binario novo e `LCS_SHADOWS_OFF=1` completo.
- Processo ativo observado: `./lcs`, heartbeat `state=9 post-draw`.

### s7.11 - 2026-06-23 - pausa solicitada, estado salvo

Pedido do porter:
- Parar por enquanto e salvar tudo nos MDs.
- Nao continuar tentando resolver o mesmo bug agora.

Estado real antes da pausa:
- Build local compilada e deployada no device com `LCS_SHADOWS_OFF=1` completo.
- `run-playable.sh` foi iniciado de novo no device.
- O jogo ficou aberto em gameplay:
  - processo observado: `./lcs`;
  - `heartbeat`: `state=9 post-draw`;
  - `FinishCutscene called`: `2`;
  - screenshot final salvo em `~/lcs-build/shot-left-open-playable-20260623.png`.

O que ficou provado:
- O bug das placas/retangulos pretos nao e uma coisa so.
- `LCS_PVS_CLEAN=1` removeu os overlays/debug PVS grandes.
- `LCS_SHADOWS_OFF=1` completo removeu os poligonos pretos grandes vistos no gameplay perto
  do carro nas capturas locais:
  - `~/lcs-build/shot-fullshadowoff-gameplay.png`;
  - `~/lcs-build/shot-left-open-playable-20260623.png`.
- Mas a primeira cutscene do onibus ainda mostrou um retangulo preto grande mesmo com:
  - shadow prefs desligadas;
  - `gRenderDynamicShadows=0`;
  - `dvbRenderStoredShadows=0`;
  - `dv_RenderWorldIntoShadowMap=0`;
  - `dv_bShowShadowMap=0`;
  - todos os hooks `SHADOWS_OFF NO-OP`, incluindo dynamic/cutscene/update.
- Evidencia dessa pendencia:
  - `~/lcs-build/shot-fullshadowoff-cutscene-noskip.png`: frame externo do onibus ainda com
    retangulo preto na rua/chao;
  - `~/lcs-build/shot-fullshadowoff-cutscene-noskip-late.png`: frame posterior/interior limpo.

Interpretação atual:
- O retangulo da cutscene do onibus sobrevive ao bloqueio de `CShadows`/`CDynamicShadows`.
- Portanto, para esse caso especifico, a causa mais provavel nao e o shadow pass classico.
- O GLSTATE curto indicou draws normais de mundo no frame inicial (`prog=39/99`, varios draws
  opacos, blend off em parte do mundo), entao a proxima investigacao deve olhar para pass/material
  de mundo, detail texture, textura/mipmap/compressao, ou algum overlay de world-stream fora do
  sistema `CShadows`.

Testes tentados e nao conclusivos:
- `BULLY_TRILINEAR=1` seria um A/B bom para separar ETC1/cache/mipmap, mas no device morreu
  cedo durante load antes dos hooks; nao gerou evidencia visual.
- `LCS_NO_WORLD_ALPHA=1` tambem morreu cedo nesse perfil sem gerar screenshot; nao conclui se
  o retangulo vem do alpha pass.
- Nao insistir nesses testes agora; ficaram registrados para uma proxima sessao com run mais
  controlado.

Proxima sessao, quando voltar:
- Nao refazer o que ja esta provado acima.
- Partir da captura `shot-fullshadowoff-cutscene-noskip.png`.
- Se for atacar esse bug depois, isolar pass/material do world-stream, nao voltar primeiro para
  `CShadows`.
- Manter `run-playable.sh` como perfil jogavel atual para teste fisico.

### s8 - 2026-06-23 - 🎯🔑 RESOLVIDO: "asfalto/chão preto" = CCoronas::LightsMult (mundo escurece, peds não)

CAUSA-RAIZ CRAVADA (RE + A/B no device, alta confiança):
- `CCoronas::LightsMult` (símbolo `_ZN8CCoronas10LightsMultE` @0x816208) é um multiplicador
  GLOBAL que escurece a luz AMBIENTE e DIRECIONAL do MUNDO (re3 Lights.cpp:33-35,48-50 em
  SetLightsWithTimeOfDayColour @0x4b9a44). Vai de 1.0 e CAI ao mínimo 0.6 quando o Sun Corona
  aparece (Coronas.cpp:584), OSCILANDO conforme a câmera vira (Coronas.cpp:109+ usa CamLook).
- Os PEDS são ISENTOS (SetAmbientColoursForPedsCarsAndObjects, +30% próprio) -> por isso o
  CHÃO/mundo escurece mas o Toni NÃO. Bate com TODAS as pistas do porter: "sombra de árvore
  normal, o preto oscila por cima como nuvem, farol/luz revela, some olhando pro sol, pisca na
  chuva, view-dependent".

PROVA A/B (harness test_ab.sh: gameplay + varredura de câmera, mede brilho do chão):
- LightsMult NATURAL (buggy): chão mean ~43 (oscila 39-48)
- LightsMult=1.0 (fix):        chão mean ~44
- **LightsMult=0.6 FORÇADO:    chão mean ~26-30** = IDÊNTICO aos frames pretos do porter (26-32),
  mesma cena/horário (13:54 dia claro), mundo escuro + Toni iluminado. Print: ab_dim_png/a03_g26.png.
  -> prova matemática+visual: o "preto" = LightsMult em 0.6; fixar 1.0 elimina.

FIX (default ON, commitar jni_shim.c):
- Wrapper `my_SetLightsWithTimeOfDay` no consumidor `SetLightsWithTimeOfDayColour` (install_hooks):
  força `CCoronas::LightsMult=1.0` IMEDIATAMENTE antes da engine montar as luzes do mundo (timing
  garantido). + escrita por-frame no loop (redundância).
- Flags: `LCS_NO_LIGHTSMULT_FIX=1` desliga; `LCS_LIGHTSMULT=v` ajusta o valor (testes).
- Era REGRESSÃO nossa (porter: "há >1 dia a 1ª cutscene tava limpa"). LightsMult é comportamento
  normal da engine, mas algo nosso (provável dvLodDistanceScale=0.75 / forçar NOON / câmera) fazia
  ele cair a 0.6 com frequência. Pin 1.0 resolve direto sem depender da causa do drop.

PENDENTE: confirmar ao vivo no terreno costeiro (onde o preto era forte). Stutter dirigindo
= problema SEPARADO (streaming I/O do SD + upload ETC no Mali).

### s9 - 2026-06-23 - 🎯🔑🔑 CAUSA REAL do "asfalto/chão PRETO" = DETAIL TEXTURE (não era LightsMult)

CORREÇÃO da s8: o LightsMult era SECUNDÁRIO (escurece o mundo todo ~40%, fix mantido). A CAUSA
PRINCIPAL do "preto que vem de outro lugar" (porter: clarear a luz só ajudava um pouco, suspeita
de TEXTURA) é a **DETAIL TEXTURE** do chão.

DIAGNÓSTICO DECISIVO (frame em "Saint Mark's", andando pra frente do spawn):
- chão PERTO da câmera = PRETO; chão LONGE = verde/claro. A fronteira é a DISTÂNCIA DE DETALHE.
- A engine aplica uma detail texture no chão dentro de `dv_renderDetailedDistance` (@0x9ca900,
  dv-obj float@+76). No Mali-450 Utgard essa textura renderiza PRETA e MULTIPLICA o chão perto.
- Explica TUDO: perto-preto/longe-claro, "vem de outro lugar"(camada de textura), luz aditiva
  (farol) revela, boost de world-light ajuda só "um pouco", sobrevive a SHADOWS_OFF, view/posição
  dependente. GTASA Vita desliga detail textures pelo MESMO motivo (disable_detail_textures=1).

PROVA A/B (harness test_fwd.sh = gameplay + correr pra frente + filmstrip, mede brilho do chão):
- VANILLA (detail ON):       chão MIN=21 (preto perto, f03/f04 Saint Mark's)
- DETAIL-FIX (detail OFF):   chão MIN=41, maioria 60-105 -> grama perto VERDE/iluminada (limpo).
  Prints: fwd_vanilla/f04.png (preto) vs fwd_detailfix/f04.png (verde).

FIX (default ON, jni_shim.c, frame-loop): pin `dv_renderDetailedDistance=0` (+_InBoat) por frame
-> nenhuma detail texture. Flags: LCS_DETAIL_ON desliga o fix; LCS_DETAIL_DIST=v ajusta.
Binário fix md5 90f2b418. + mantido o fix LightsMult (s8, secundário/world-dim) e canal live
/dev/shm/lcs_lightsmult.

PENDENTE: confirmar ao vivo (deve estar resolvido agora). Stutter dirigindo = SEPARADO.

### s10 - 2026-06-23 madrugada - investigação overnight do chão escuro (HONESTO: parcial)

O porter testou ao vivo: o detail-fix (s9) melhorou (prints "perfeitos") MAS ainda há PRETO. Fotos novas:
(1) DIAMANTE preto HARD-EDGED em volta do CARRO (spawn), (2) ARCO escuro no chão perto a pé.
Resolvido s9 (detail texture) era REAL mas PARCIAL.

INVESTIGAÇÃO (A/B câmera-fixa via canais live /dev/shm/lcs_lightsmult, lcs_dvset, lcs_ambient):
- No PIOR ângulo (varredura de câmera) o chão = 15 (preto) MESMO com detail-off + lightsmult-pin.
- Testei MESMA vista trocando só o knob:
  * dvVertexAmbientBrightness / dvVertexAmbientContrast / gWorldAmbientLight = NÃO mexem (0 efeito).
  * AmbientLightColourForFrame boost (pós-SetLights) = NÃO mexe limpo (renderer não lê isso pós-SetLights).
  * **LightsMult = ÚNICO lever limpo**: 15 -> 32 com LM=2 (uniforme, mas estoura o lado claro).
- CONCLUSÃO: a luz do MUNDO flui por SetLightsWithTimeOfDayColour*LightsMult; ambiente-only por
  fora não pega (renderer móvel CMattRenderer/cWorldStreamEx lê de outro ponto, provável uniform GL
  setado em SetAmbientColours, ou RpLight). Boost de ambiente correto = na FONTE (CTimeCycle current
  ambient antes do SetLights) — NÃO testado ainda.

LEADS NÃO TESTADOS (pra sessão ao vivo):
- DIAMANTE hard-edged do carro = provável **CDynamicShadows** (sombra dinâmica projetada) +
  `cWorldStreamEx::IsShadowShader`@0x561b24 (shadow shader do renderer móvel) renderizando quad PRETO
  no Mali. Nosso SHADOWS_OFF no-opa CDynamicShadows::Begin/PreRenderSetup -> pode deixar a TEXTURA de
  sombra preta/stale sendo PROJETADA (=nós causamos!). TESTAR: SHADOWS_OFF=0 vs =1 no MESMO spot com
  carro; e hookar IsShadowShader. NÃO reproduzi headless (precisa do carro).
- ambiente-na-fonte: boostar CTimeCycle current ambient (offsets ~+11032 do ptr em [7f9000+160]).

FERRAMENTAS LIVE prontas (build c7eb4f1): /dev/shm/lcs_lightsmult (val), /dev/shm/lcs_dvset
("sym val"), /dev/shm/lcs_ambient (val). Repro do escuro: andar pra frente do spawn + girar câmera.
PLANO: sessão AO VIVO - o porter dirige até o diamante, eu toggle candidatos live em segundos.

### s10b - resultado SHADOWS_OFF A/B (REFUTA hipótese) + veredito honesto da noite
A/B câmera-fixa forward+sweep: SHADOWS_OFF=0 (sombras ON) chão MIN=16 med=18 (varia 16-19, sombra
real escurece); SHADOWS_OFF=1 (nosso default) chão FLAT=22 (mais claro). => nosso SHADOWS_OFF NÃO
causa o escuro, AJUDA. E o DIAMANTE hard-edged das fotos NÃO é sombra (com shadows-off ele persiste).

VEREDITO HONESTO (após noite inteira de A/B): o único lever LIMPO do chão escuro é LightsMult
(uniforme, estoura o lado claro). detail-off (s9) ajudou parcial (near-field). ambient-only/vertex
knobs = sem efeito. shadows = não é. O escuro restante é a luz intrínseca do mundo em certos
ângulos. NÃO consegui isolar uma causa "nossa" headless, NEM reproduzir o DIAMANTE hard-edged
(precisa do carro parado no spot). 
PRÓXIMO = SESSÃO AO VIVO: o porter dirige até o diamante, capturo o frame EXATO (finalmente o repro
hard-edged), e toggle candidatos live. Suspeitos restantes do diamante: polígono de chão do MAPA
com vertex-color/textura escura (carro parado em cima), OU env-map/reflexo do veículo projetado.
Build c7eb4f1 com tools live no device. NÃO shipei boost agressivo (escolha do valor LightsMult
fica pro porter ver ao vivo: echo VAL > /dev/shm/lcs_lightsmult).

### s11 - 2026-06-23 - 🎉🔑🏆 RESOLVIDO DE VEZ: "diamante/preto no chão" = ALPHA vazando no compositor HDMI

CAUSA-RAIZ REAL (todas as teorias s8-s10 eram red-herring de RGB): a surface GL é criada com
**ALPHA=8** (egl_shim.c alpha_try[]={8,0}, mali fbdev). O **compositor do Amlogic mistura o plano
GL com o fundo (preto) usando o ALPHA do framebuffer**. Onde o chão/mundo é desenhado deixando
**alpha < 1** no framebuffer, o fundo PRETO VAZA por baixo = o "diamante"/preto hard-edged.

POR QUE NOS ENGANOU A NOITE TODA:
- glReadPixels lê só RGB (ignora alpha) -> MEUS PRINTS NUNCA MOSTRAVAM o preto (era invisível pra mim).
- A TV mostra o resultado ALPHA-COMPOSTO -> o preto aparece SÓ no HDMI ("só eu vejo", visível só na TV).
- LightsMult/detail/shadows/ambient = tudo RGB -> NÃO corrigiam (o bug é alpha).
- Farol/clarão-do-sol desenham OPACO (alpha=1) -> tapavam o vazamento localmente (revelavam o chão).
- Diagnóstico do porter: "algo sujo no caminho pro HDMI". Exato.

FIX (default ON, imports.c my_eglSwapBuffers, ANTES do swap): força o ALPHA do framebuffer inteiro
= 1 (plano opaco) -> sem vazamento. glDisable(SCISSOR/DITHER); glColorMask(0,0,0,1);
glClearColor(0,0,0,1); glClear(COLOR_BUFFER_BIT); glColorMask(1,1,1,1). Flag LCS_NO_ALPHA_FIX desliga.
Binário a1b9a7e. O porter confirmou ao vivo: "deu certo, resolveu tudo".

LIÇÃO REUSÁVEL (qualquer so-loader em Amlogic/Mali-450 fbdev): se aparecer preto/artefato SÓ na TV
e não no glReadPixels, é o ALPHA do framebuffer vazando o fundo no compositor -> forçar alpha=1 antes
do swap (ou criar a surface com alpha=0). glReadPixels NÃO debuga isso (lê só RGB).

NOTA: os fixes s8(LightsMult)/s9(detail) seguem no build mas eram secundários/desnecessários pro
diamante; dá pra re-testar re-ligar detail (LCS_DETAIL_ON=1) p/ visual melhor agora que a raiz é alpha.

### s11b - ESTADO FINAL BOM TRAVADO (porter: "melhorou muito, quase imperceptível")
PERFIL BOM = run-playable.sh DEFAULT (SHADOWS_OFF=1, GFX_PREFS=1, detail-off, lightsmult-pin) +
binário a1b9a7e (FIX DE ALPHA default-on). O porter confirmou: chão limpo, sem diamante, estável.
- ⚠️ NÃO reverter SHADOWS_OFF: com sombras LIGADAS (SHADOWS_OFF=0) a SOMBRA pisca muito (z-fight)
  -> SHADOWS_OFF=1 evita. As "hacks" NÃO eram lixo de debug, ajudam. Manter o default.
- ⚠️ O state-restore do alpha-fix (build 07dcdc7) PIOROU tudo -> REVERTIDO. Fix bom = a1b9a7e (clear
  alpha simples, deixa scissor/dither off; benigno na prática).
- PENDÊNCIAS MENORES conhecidas (quase imperceptíveis, deixadas de lado): vidro do carro
  "quadradinhos" só ao mover (output-path/tile Mali, NÃO aparece no glReadPixels, env-map-off não
  mudou); flicker de sombra só com SHADOWS_OFF=0. Stutter dirigindo = separado (streaming).
Binário bom md5 a1b9a7e deployado. PRÓXIMO ALVO: a definir com o porter (provável stutter dirigindo).

### s12 - 2026-06-23 - Perfil GTASA/performance preparado OFFLINE (sem abrir jogo)

Pedido do porter: estudar GTASA Vita + GTASA do R2 e preparar melhorias de performance sem abrir
o jogo/sem testar ate ele pedir.

Estudo local confirmou o padrao:
- GTASA R2 roda com perfil agressivo: bloom/trilinear/mipmaps/detail/shadows off, `drop_highest_lod=1`,
  decals zerados e debris reduzido.
- GTASA Vita/NextOS usa shader/cache (`scache*`), texdb ETC e cortes de detail/shadow/alpha caros.
- GTASA logado chega a ~222MB RSS em frame 9600; LCS estava em ~443MB RSS + ~209MB swap. Portanto o
  stutter dirigindo e prioritariamente RAM/streaming/swap, nao um bug visual.

Mudancas preparadas, NAO TESTADAS:
- `run-playable.sh`: `LCS_INPUTDIAG` e `LCS_INPUTDIAG_START` agora default 0 para nao gastar log no
  perfil jogavel. Pode religar com env se precisar diagnosticar controle.
- Novo `run-gtasa-perf.sh`: perfil separado inspirado no GTASA, sem mexer no perfil visual bom.
  - Tier 1/balanced: page cap 180MB, streamer 64, drain 4, LOD 0.65.
  - Tier 2/smooth: cap 140MB, streamer 48, drain 3, LOD 0.50, render scale 0.90.
  - Tier 3/potato: cap 110MB, streamer 32, drain 2, LOD 0.40, render scale 0.75, tex-light.
  - Liga CPU governor performance, `PULSE_LATENCY_MSEC=120`, `MALLOC_CHECK_=0`, drivers GL/EGL
    explicitos, diagnostico off, `BULLY_PAGE=1`.
- `run-final.sh`: aceita `LCS_PROFILE=gtasa-perf` para usar o perfil novo quando for testar.
- `ESTUDO-PERFORMANCE-TIERS.md`: atualizado com os tiers e aviso de que ainda nao foi validado.

Importante: nada foi executado/testado por mim, nenhum run novo foi iniciado nesta etapa. Os scripts/docs
foram copiados para o device. Havia processos antigos `./lcs`/`LCS_WALKDIAG` ja rodando antes; foram
encerrados para respeitar o pedido de deixar o jogo fechado. Confirmado depois: `pgrep -af lcs` sem retorno.

### s13 - 2026-06-23 - Primeira medicao perf real + correcao do perfil

O porter liberou testes automaticos. Primeiro run `LCS_PROFILE=gtasa-perf LCS_PERF_TIER=1` foi iniciado
com `run-final.sh`. Resultado: o processo foi morto pelo watchdog em state 7/f=89 antes de gameplay,
sem crash log. Memoria durante load caiu para 64-81MB livres, swap ~61MB; no fim voltou para ~630MB
livres. Interpretacao: nao provar stutter de gameplay ainda; o watchdog estava agressivo para load
longo/streaming com frame parado. Proximos runs devem usar `FREEZE_SEC` maior ou run direto sem esse
watchdog de frame parado durante state 7.

Estudo de codigo apos reVC/GTASA:
- reVC usa alpha=0 na surface, forca 1280x720, corrige mipmap incompleto/`GL_TEXTURE_MAX_LEVEL` e
  documenta que estudar a fonte/loader antes de chutar e o caminho.
- LCS ja possui uma alavanca melhor que eu nao tinha ligado no perfil: `LCS_TEX_HALF`.
- `BULLY_PAGE` registra principalmente texturas do caminho `glTexImage2D`/ETC1-cache; LCS Android
  sobe a maior parte por `glCompressedTexImage2D` ETC, entao `BULLY_PAGE` sozinho nao reduz o grosso.
- `LCS_TEX_HALF` no hook comprimido reenvia mip 1 como nivel 0, cortando textura grande por ~75% sem
  re-encodar. Isso e o corte certo para atacar RAM/FPS primeiro.

Mudanca offline feita depois da medicao:
- `run-gtasa-perf.sh` agora liga `LCS_TEX_HALF=1`.
  - Tier 1: `LCS_TEX_HALF_MIN=1024`.
  - Tier 2: `LCS_TEX_HALF_MIN=512`.
  - Tier 3: `LCS_TEX_HALF_MIN=256` + `LCS_TEX_LIGHT`.

### s14 - 2026-06-23 - Performance/FPS: perfil full-native, streamer por fase, texlight descartado

O objetivo mudou para **ganhar FPS/usar menos RAM sem encolher a imagem**. A imagem precisa
ficar full/native. Resultado: `LCS_RENDER_SCALE` foi removido dos defaults do `run-gtasa-perf.sh`;
fica apenas opt-in manual. O jogo agora roda 1280x720 por default em todos os tiers.

Mudancas implementadas e deployadas:
- `src/jni_shim.c`: `LCS_GFX_MEMLOW` escreve draw distance, `gStreamingMemSize`, limites de
  peds/carros/distancias, densidade de populacao/carros e knobs do streamer.
- `src/jni_shim.c`: novo `streamphase` por estado. Durante load usa create moderado
  (`loadtex/loadbuf`), e ao entrar em gameplay rebaixa para `gametex/gamebuf`. Isso e importante
  porque a engine seta `dvStreamerCreateNumTexturesPerFrame=4000` na transicao para gameplay.
- `src/imports.c`: contador de textura comprimida ETC em `glCompressedTexImage2D`, para o memdiag
  deixar de ficar cego ao caminho principal do LCS.
- `run-gtasa-perf.sh`: `LCS_NO_ENVMAP=1` default no perfil perf, atacando flicker/reflexo de carro
  sem destruir textura.

Medicoes principais:
- Tier 2 full/native, andando por eixo: ~410-447MB RSS, ~165-181MB VmSwap do processo, heartbeat
  avancando bem em state 9. Visual preservado.
- Tier 3 original com `LCS_TEX_LIGHT=1` reduziu flicker/stutter visual, mas **quebrou texturas**:
  pele/personagens ficaram pretos/brancos. Logs mostraram stubs `64x64`, depois `128x128/256x256`.
  Portanto `TEX_LIGHT` nao pode ser default jogavel.
- Filtro `LCS_TEX_LIGHT_MIN_DIM=512` preservou Toni/pele, mas praticamente nao aplicou stubs.
- Filtro `LCS_TEX_LIGHT_MIN_DIM=128` ainda quebrou pedestre/carro na captura
  `~/lcs-build/shot-tier3-tex128-20260623.png`.
- Perfil candidato atual: **Tier 3 safe** = `TEX_LIGHT` off, `TEX_HALF_MIN=256`,
  `NO_ENVMAP=1`, streamer/densidade agressivos. Captura boa:
  `~/lcs-build/shot-tier3-safe-noenvmap-20260623.png`.

Estado atual recomendado para teste:
```sh
cd /storage/roms/ports/lcs
LCS_PROFILE=gtasa-perf LCS_PERF_TIER=3 RUNSEC=360 MAXRESTART=0 FREEZE_SEC=180 sh ./run-final.sh
```

Nao reativar `LCS_TEX_LIGHT` como default ate existir seletor por asset/classe. Ele melhora alguns
flickers porque remove textura, mas e destrutivo para pessoas/carros/itens.

### s15 - 2026-06-23 - Tier 3+ pequeno: FX/reflexos off, menos pop, full-native preservado

Pedido do porter: o jogo ja estava "muito bom"; buscar mais 5-10% sem quebrar imagem/textura.

Mudancas implementadas, buildadas e deployadas:
- `src/jni_shim.c`: novo corte separado por `LCS_GFX_FX_OFF=1`, sem depender de `LCS_GFX_LOW`.
  Ele no-opa `CCoronas::RenderReflections`, `RenderSunReflection`,
  `CMotionBlurStreaks::Render`, `CWeather::RenderRainStreaks` e `RenderLightBloom`.
- `run-gtasa-perf.sh`: default perf agora liga `LCS_SUNREFLECT_OFF=1` e `LCS_GFX_FX_OFF=1`,
  mantendo `LCS_NO_ENVMAP=1`.
- Tier 3/potato foi apertado levemente:
  `peds=3`, `cars=2`, `ped_dist=14`, `veh_dist=20`, `pop=0.20`, `carpop=0.20`.
- Resolucao continua nativa/full: screenshot validado em `1280 720`.
- `TEX_LIGHT` continua desligado por default; nao reintroduzir, pois quebrou pele/peds/carros.

Teste automatico no device:
```sh
cd /storage/roms/ports/lcs
LCS_PROFILE=gtasa-perf LCS_PERF_TIER=3 RUNSEC=900 MAXRESTART=0 FREEZE_SEC=180 sh ./run-final.sh
```

Resultado:
- Entrou em gameplay `state=9` sem crash.
- Hooks aplicados no log: `SUNREFLECT_OFF`, `GFX_FX_OFF`, `NO_ENVMAP`, `streamphase gameplay`.
- Movimento virtual para frente aplicado via `/dev/shm/lcs_axis`; frame avancou ate `f=2549+`.
- Medicao final estabilizada: `VmRSS 443804 kB`, `VmSwap 166696 kB`, RAM livre ~91MB,
  available ~153MB.
- Comparacao com Tier 3 safe anterior (`VmRSS 453580 kB`, `VmSwap 165732 kB`): economizou
  cerca de 9-10MB de RSS sem aumentar swap de forma relevante.
- Janela curta de cadencia: `f=2789 -> f=3089` em 15s, aproximadamente 20 FPS na cena parada.
- Captura visual boa: `~/lcs-build/shot-tier3-plus-fxoff-20260623.png`
  (`1280x720`, personagem/HUD/texturas OK, sem pele preta/branca).

Estado deixado: jogo aberto no device em gameplay com o Tier 3+ ativo para inspeção. Se esse
perfil causar cidade vazia demais, voltar somente os limites de pop/carros, mantendo FX/reflexos off.

## s7 (2026-06-24) — INTRO NATIVO (engine-driven) + decifrado o boot state-machine
🎬 **INTRO RESTAURADO NATIVO:** a engine chama `OS_MoviePlay("Movies/LCS_INTRO_1080.m4v")` no
ESTADO 3 do `OS_ApplicationTick` (no Android = MediaPlayer/Surface Java; no so-loader caía no JNI
fake → no-op → intro pulado). FIX = hookar `OS_MoviePlay` (`_Z12OS_MoviePlayPKcbbf`) → `my_OS_MoviePlay`
→ `lcs_play_intro()`: toca o `intro.m4v` REAL (3 logos Rockstar + abertura, 1080p, COM áudio+legendas)
via ffmpeg do device → `/dev/fb0` (vídeo) + pacat/pulse (áudio), BLOCKING, com SKIP pelo controle
(A/B/START). Vídeo extraído do APK do celular: `res/raw/intro.m4v` (40MB); versão leve `intro720.m4v`
(h264 baseline 720p, 7MB) pra decode em tempo real no Amlogic. Confirmado: SEM botão fantasma (toca
limpo sem input, ffmpeg=2 rodando, engine pausada). Gate `LCS_INTRO=1`. Validado: "vídeo+áudio+
legendas OK". mpv/SDL NÃO desenham no fbdev (só ffmpeg `-f fbdev`). t0 do MAXSECONDS desconta o intro
(g_intro_play_secs).

🗺️ **BOOT STATE-MACHINE DECIFRADO** (`OS_ApplicationTick` @0x53ed14, jump table @0x23f1ca, estado em
[0x7fd000+2232]): st0=init+LoadSettings+OS_PlaylistBeginInit | st1→RockstarGameLoad→st2 (precisa
HandlePlaylistFinishInit, setamos no f=5) | st2→st3 (precisa feobj+40, setamos) | **st3=OS_MoviePlay
(INTRO)→st4** | st4=espera filme + tap-to-skip (LIB_PointerGetButton==2) | **st5=LoadAllTextures+
LoadingScreen** (o LOADING vermelho!) | st6=LoadingScreen+CheckSlotDataValid→st7 | **st7=menu** (se
feobj+25==0 roda CMenuManager via GameCoreTick; se feobj+25 setado→st8→DoFade→GameStart→st9) |
**st9=GameCoreTick (GAMEPLAY)**. `LoadingScreen()`@0x521870 é a MESMA usada no new-game.

🚫 **REMOVIDO ATALHO DO SUBMENU NEW GAME:** `lcs_menu_controller_confirm` chamava
`CMenuManager::StartNewGame` direto no A → PULAVA o submenu New Game (reclamação do porter). Agora
`LCS_MENU_CONFIRM_START=0` default (run30.sh) → menu nativo navega Start Game→submenu sozinho.

🟡 **PENDENTE (precisa controle físico p/ testar):** (1) tap/legal "piscam" — a engine
auto-completa `renderedTapToContinue`/`shownLegalScreen`/`legalScreenState` rápido (NÃO é botão
fantasma — confirmado; NÃO é só o gta_lcs.set salvo — testado removendo). Renderizador embutido
(offsets 0x9ec7a9-b4, sem símbolo, refs via add difíceis). (2) New Game→LOADING vermelho→cutscene:
testar agora que o atalho StartNewGame saiu — a hipótese do porter é que o loading que faltava é a
raiz de muitos bugs de cutscene. Binário lcs md5 1f87cefc. Hooks novos: OS_MoviePlay, HasTappedScreen
(LCS_TAP_NATURAL, mas engine não gateia nele). Flags: LCS_INTRO, LCS_INTRO_AT_FRAME0, LCS_PLAYLIST_FRAME.

## s7b (2026-06-24, sessão autônoma) — FLUXO COMPLETO ATÉ GAMEPLAY + FPS CAP
🎯 **FPS CAP 30 (causa-raiz de "tudo rápido"):** o jogo roda 42-50fps sem teto -> UI/timers da
engine rápidos demais (menu rolava todas opções, tap/disclaimer piscavam). FIX: cap no loop do
driver via clock_gettime+nanosleep, `LCS_FPS_CAP=30` default (=0 desliga). Medido 33fps. O porter
validou navegação do menu "perfeita" depois disso + do MENU PULSE.
🕹️ **MENU PULSE (LCS_MENU_PULSE=1 default):** o bridge mantinha o botão CURRENT setado enquanto
segurado -> menu rolava todas as opções num aperto. FIX: no menu (state 7) pulsa o d-pad/A só na
BORDA (1 frame) -> 1 aperto = 1 movimento. Porter: "pro lado foi resolvido / navego perfeito".
🎬 **FLUXO COMPLETO VALIDADO (via injeção `echo N>/dev/shm/lcs_btn`, A=0):** Start Game (A) ->
submenu New Game (A) -> New Game -> state 9 -> cutscene/streaming (lento ~1fps, frame 224-389) ->
**GAMEPLAY COM MUNDO 3D RENDERIZANDO** (frame pula p/ 944+, ~33fps fluido; Toni andando, carro,
escadas, prédios, árvores de Liberty City — d4/g_25). SAÍDA LIMPA (run30 done, ZERO crash). O
"preto com HUD" inicial (g_05) é só o streaming não-terminado, não bug. **Menu confirm nativo
funciona** (Start Game->submenu sem o hack StartNewGame). O disco cheio (/dev/shm 100%) causava
capturas falhas/tela branca -> limpo (gamedata 7.8G->2.1G).
🟡 **PENDENTE:** (1) tap "touch to continue" vermelho não renderiza (arte segura mas sem texto;
controle pré-frame da legal é frágil - engine recalcula mid-frame). (2) disclaimer tem 2 PÁGINAS
(textos diferentes); página 1 segura (freeze legalScreenState=0), página 2 pisca (valor de state
da pág2 não isolado - janela estreita). (3) cutscenes bus/office parecem abreviadas pelos hacks
LCS_CUTSCENE_* (queremos elas TOCANDO de verdade com luz). PRÓXIMO: com o fps cap, revisitar os
hacks de cutscene (talvez removê-los deixe as cutscenes tocarem naturais agora que o timing é 30fps).
Binário lcs md5 865294a. Flags novas: LCS_FPS_CAP=30, LCS_MENU_PULSE=1, LCS_FORCE_TAPLEGAL=1,
LCS_TAP_NATURAL=1, LCS_LEGAL_HOLD_STATE=0, LCS_FRONTEND_STEP_START. ⚠️NÃO usar LCS_FORCE_TAPLEGAL=0
com LCS_TAP_NATURAL=1 (trava em tela branca: HasTappedScreen=0 sem o step machine).

## s7c (2026-06-24) — CUTSCENES TOCAM (fade-hack era o vilão) + wedge de streaming
🎬🏆 **BUG "HUD do nada antes da cutscene" RESOLVIDO:** após New Game o correto é loading->cutscene1
(SEM HUD), mas aparecia o HUD de gameplay (mapa/vida/$) no mundo preto antes do vídeo. CAUSA: os
fade-hacks `LCS_NODOFADE`+`LCS_UNFADE` (band-aid da s6) forçavam GetScreenFadeStatus!=FADE_2 ->
revelavam o gameplay/HUD durante a fase de loading em vez de deixar a cutscene tocar com seu
próprio fade. FIX: `LCS_NO_FADEHACK=1` agora DEFAULT (desliga UNFADE+NODOFADE). Resultado: New Game
-> loading (preto, SEM HUD) -> **CUTSCENE 1 do ÔNIBUS toca com LUZ DO DIA** (Portland, prédios) ->
**CUTSCENE 2 da mansão com LEGENDA** ("What more could a family guy ask for?") -> gameplay com mundo
renderizando. Sem fade-hack o gameplay NÃO fica preto (a cutscene natural limpa o fade sozinha; o
fade-hack só era "necessário" quando FORÇÁVAMOS gameplay pulando a cutscene).
🔧 **HasTappedScreen gate state-9:** o hook do TAP_NATURAL retornava 0 sempre -> WEDAVA a cutscene
(ela checa HasTappedScreen p/ progressão/skip). FIX: my_HasTappedScreen devolve o valor REAL no
state 9 (cutscene/gameplay); só controla o tap no front-end (state<9).
🟡 **WEDGE DE STREAMING (não-blocker de código):** durante a cutscene 2 (mansão, streaming pesado)
trava em frame NÃO-DETERMINÍSTICO (749 numa run, 869 noutra) = parede de RAM/corrida do job-system
do streamer (igual Bully; memória documenta). Mata limpo no MAXSECONDS, 0 crash. PRÓXIMO: atacar o
streamer (zram/swap tuning OU achar qual lglXxxCreator::hasPendingTasks nunca termina - ver s3).
Binário lcs md5 8a4cbd96. Defaults novos: LCS_NO_FADEHACK=1, LCS_FPS_CAP=30, LCS_MENU_PULSE=1,
LCS_MENU_CONFIRM_START=0. Fluxo COMPLETO funcional: intro nativo->tap->disclaimer p1->menu->Start
Game->submenu New Game->loading->cutscene1 ônibus->cutscene2 mansão (legendas)->[wedge streaming].
