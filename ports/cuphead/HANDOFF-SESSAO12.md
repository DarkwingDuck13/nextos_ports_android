# Cuphead Mali-450 — Sessão 12 (2026-06-11)

## ⏩ COMO RETOMAR (próxima sessão) — quickstart
**Estado:** código commitado+pushado (origin+private, commit 7cdd71f). Device .164 (root/—,
DHCP pode mudar IP: tentar `<device-ip>`; se não, `ssh root@<device-ip>` na subrede local).

**Objetivo nº1:** o load assíncrono da cena do MAPA não completa (1 op de integração
PERSISTENTE com done72=0 desde o boot; vt u+0x123e658, integ=0x872758). NÃO é crash nem
memória. Fix CIRÚRGICO (levers globais quebram o boot — ver seção MURO). Ler a seção
"🎯 MURO REAL ISOLADO" + "LEVERS DESCARTADOS" abaixo. Trabalho de RE pode ser LOCAL primeiro
(libunity.so + dump em ~/cuphead-build/re e ~/cuphead-build/dump), sem device.

**Setup do device (perde no reboot — refazer):**
```
ssh root@<device-ip>
systemctl stop emustation; systemctl mask emustation; pkill -9 emulationstation   # libera RAM
echo 1 > /proc/sys/vm/overcommit_memory
L=$(losetup -f); losetup $L /storage/roms/nextos.swap; mkswap $L; swapon -p 20 $L  # swap 2GB
```
**Rodar + navegar até o mapa:** `cd /storage/roms/cuphead-recon && sh go-map.sh` (TEXHALF=256,
P2 bloqueado). Esperar disclaimer (`grep inputwait debug.log`). Navegar via /tmp/gpcmd
(CUP_GPVIRT): `go`(disclaimer, 2x) → `accept`(START) → `accept`(save NEW) → ~40 `accept`
(LIVRO/capa CUPHEAD) → ~16 `jump` OU combo right+jump (DIÁLOGO Elder Kettle, avança c/ JUMP,
é LONGO) até ter controle (ações Dash/Shoot polled) → `pause` → 3x `down` (passo único) →
`accept` (EXIT TO MAP). nav-map.sh faz isso mas as contagens de accept variam (livro come
toques em transições). Pra PSPY: rodar go-map.sh com CUP_PSPY=1 (e SEM DRAINWAIT/DRAINPRELOAD,
que travam). Capturar fb: `head -c 3686400 /dev/fb0 > /tmp/fb.raw` (BGRA, converter swap R<->B).

## Ponto de partida (s11)
- INÉDITO: controle + entrada na fase pela primeira vez, estável.
- 3 destraves: (1) overloads de input por NÚMERO de ação (7 que faltavam);
  (2) feel do menu (lixo -32767 do USB ao conectar envenenava calibração + scroll 1/frame);
  (3) deploy cutscene livro/intro/mundo-1 + redirect de paths relativos.
- Memória RESOLVIDA: UnloadUnusedAssets+GC nas transições + swap reordenado pro ext4.
  Pico de 220MB passou suave, heap 31→23MB nas trocas. Memória deixou de ser o muro.

## Muro real isolado (s11): crash casa→mapa-do-mundo
- sig=11 fault=0x18 pc=libunity+0x541cdc. x0=0 x1=0xffffffffff.
- Análise s12 (RE libunity.so + Il2CppDumper):
  - 0x541cdc = `ldr x8,[x0,#24]` logo após `bl 0x8f7c48` (helper inline ldp x8,x1,[GO+0x38]).
    Resolve {scene, idx} de um GameObject; deref [scene+24] SEM null-check.
  - Pilha cheia de Instantiate/SpriteMask/Tilemap.RefreshTile → instanciação da
    cena do mapa. Um GameObject veio com scene=NULL (não enraizado/destruído).
  - **Diagnóstico duplo**: o GCEVERY=1800 da s11 era um tick CEGO. Caiu no MEIO
    do load assíncrono do mapa (f=10800, rss subindo) e o UnloadUnusedAssets varreu
    objetos meio-construídos → scene NULL → crash. Memória NÃO era a causa direta;
    a LIMPEZA mal-cronometrada era um gatilho, + a função sem null-check é o bug real.

## Fixes s12 (aplicados)
1. **CUP_SCENEGUARD** (default ON): ilha PROT_EXEC a <128MB da libunity substitui o
   `bl 0x541cd8`. Inlina o helper e, se scene==NULL, devolve fake_scene zerada
   (idx 0, array de zeros) em vez de deixar o caller deref [NULL+24]. Conta hits;
   o render loop loga "[SCENEGUARD] scene NULL interceptada".
2. **GCEVERY com gate de ociosidade**: a limpeza só dispara quando o PreloadManager
   está OCIOSO (preloadQ[+224]==0 && integQ[+256]==0) por 90 frames seguidos.
   Reusa o ptr do mgr capturado pelo PSPY (agora também ligado quando CUP_GCEVERY).
   Sem mgr capturado → espera ~20s (1200f) como fallback.
3. **Áudio (FMOD→OpenSL→SDL2)**: "FMOD failed to initialize the output device".
   Causa provável: SLObjectItf vtable[2]=GetState era stub_success (não escrevia
   *pState) → FMOD lia estado lixo pós-Realize e abortava. Fix: obj_GetState
   devolve REALIZED(2) nos 3 objetos (engine/player/outmix). + instrumentação
   [SL] (slCreateEngine/CreateOutputMix/CreateAudioPlayer/fmt/bq_Enqueue) e
   [DLSYM:SL] pra ver o que o FMOD pede.

## Config (go.sh atual)
FORCEINTEG+NO872774+GCOFF+CLAMPSIG=4096+TEXHALF=512+FORCESTARTCR+SAPATH+
NOREFRESHDLC+GAMEPAD+MEMLOG+GCEVERY=1800. SCENEGUARD é default-ON (CUP_NOSCENEGUARD desliga).

## Bundles
- 339/419 AssetBundles deployados (700MB em /storage/cuphead-sa).
- Mundo 1 COMPLETO (flower/veggies/frogs/slime/blimp/forest/tree + músicas).
- 80 faltam (1033MB): mundos 2/3/4 + chefes DLC. p3 está 100% cheia (315MB livres)
  → liberar espaço antes de deployar o resto. Não bloqueia mundo 1.

## Áudio — diagnóstico REVISADO (s12)
- O GetState fix NÃO resolveu, e o motivo ficou claro: **slCreateEngine NUNCA é
  chamado** (0 logs [SL]/[DLSYM:SL]). FMOD faz `dlopen(libOpenSLES)` mas escolhe
  OUTRO output (AudioTrack via JNI), não OpenSL. A falha "FMOD failed to initialize
  the output device" vem logo após `[JNIEnv idx 216 chamado]` (stub JNI devolvendo
  0/NULL que o FMOD precisa pra criar o AudioTrack).
- Ou seja: o caminho OpenSL (onde está todo o shim SDL2) está MORTO; o FMOD vai no
  AudioTrack/JNI e morre lá. Caminhos pra resolver:
  (a) implementar os métodos JNI de AudioTrack que o FMOD chama (getMinBufferSize,
      AudioTrack ctor/write/play) — trabalhoso;
  (b) FORÇAR o FMOD a usar OpenSL (FMOD_OUTPUTTYPE_OPENSL) — investigar se há
      setting/JNI que decide o output (AudioManager.getProperty já é fakeado);
  (c) descobrir o que idx 216 é e por que o FMOD aborta.
- Áudio fica como tarefa separada; NÃO bloqueia gameplay. O GetState fix foi mantido
  (correto e inócuo) p/ quando o caminho OpenSL for forçado.

## Navegação autônoma (s12)
- TAPINPUT (GetAnyButtonDown 0xCC2854) tem 0 pulses: o disclaimer NÃO usa esse
  método; usa Rewired (coberto pelo gamepad.c). Logo, pra dirigir sem controle
  físico: CUP_GPVIRT=1 + escrever em /tmp/gpcmd ("go"=força todo botão; up/down/
  left/right; ou nome de ação). gp_poll precisa de js0 aberto (gp_fd>=0) E gp_virt.

## Validação visual autônoma (s12) — framebuffers capturados
- DISCLAIMER renderiza perfeito (GLES2): textos, Steam/GOG, "GABE DEVELOPER".
- TÍTULO renderiza perfeito: Cuphead+Mugman, logo, raios vermelhos, botão ENTER.
- SAVE SELECT renderiza perfeito: 3 slots NEW + HUD de controles (CONFIRM/BACK/DELETE).
- Navegação autônoma SEM controle físico: CUP_GPVIRT=1 + /tmp/gpcmd.
  Disclaimer→título→save-select dirigido por software ("go"/"accept").
- GCEVERY idle-gate CONFIRMADO no log: "[GCEVERY] limpeza f=N (mgr ocioso)" 3×,
  nunca durante load. Sem crash, sem regressão.

## Bug do harness de navegação (corrigido s12)
- Botão virtual (/tmp/gpcmd) não era SOLTO: gp_btn[j] ficava 1 p/ sempre (joydev
  não manda button-up de botão virtual) -> gp_btn_prev travava em 1 -> nenhum edge
  GetButtonDown novo -> menu de save não confirmava/navegava.
  FIX: gv_btn_held lembra o botão forçado; ao expirar gv_frames, zera gp_btn[held].
  Agora cada comando vira UM toque limpo (edge).

## GAMEPLAY ALCANÇADO (s12) — casa do Elder Kettle JOGÁVEL
- Naveguei autonomamente até DENTRO DO JOGO: disclaimer→título→save NEW→cutscene do
  LIVRO (gato+dados, renderiza lindo)→CASA do Elder Kettle (Cuphead+Mugman+diálogo
  "TAKE THIS POTION", interior da casa renderizando perfeito). Tudo GLES2, framebuffers
  capturados (fb_h=casa). SCENEGUARD NÃO precisou disparar até aqui.

## Crash casa→mapa: 3 camadas (s12)
1. SIGSEGV 0x541cdc (scene NULL deref) -> SCENEGUARD. PASSOU.
2. SIGSEGV 0x8f9b1c (mesh index count insana ~0x10000000 em 0x8f9914) -> MASKGUARD
   (clamp count>0x40000). Instalado.
3. **FREEZE (não-crash) = PLAYER 2 FANTASMA** (causa apontada pelo porter!).
   NullReferenceException em **PlayerManager.Update()** <- Cuphead.Update(), todo frame,
   render TRAVA. CAUSA-RAIZ: os hooks de input (gamepad.c) faziam (void)self e devolviam
   o MESMO input p/ TODOS os players Rewired -> quando o jogo lê o input do Player 2,
   recebe o nosso -> P2 "entra" sozinho com dados meio-init -> Update() estoura null.
   **FIX: player_blocked(self)** — Rewired.Player.get_id = ldr w0,[self,#0x1C] (P1=0,
   P2=1+). Bloqueia (input neutro) p/ id>=1 em TODOS os hooks (string+int overloads).
   id<=0 (P1 + System/menu) liberado. "go" ainda passa disclaimer via P1. CUP_GP_P2 libera.
   ⚠️ ESTE provavelmente é o "trava ao sair da casa" REAL; os guards nativos (SCENE/MASK)
   cobrem os SIGSEGV, mas o freeze era o P2.

## ⚠️ DEVICE WEDGED (fim s12): OOM-thrash no load do mapa
- Com o fix do P2, ao sair da casa o device entrou em THRASH pesado (sshd sufocado,
  30 tentativas de reconexão sem sucesso, rede responde ping mas ssh não completa
  handshake). = OOM no load do mapa-múndi (cena MAIS PESADA do jogo) saturando
  832MB+swap. PRECISA POWER-CYCLE manual (mesmo padrão de s7).
- Não deu pra ler o debug.log final (ssh morto) — não sei se chegou a renderizar o
  mapa antes de afogar, ou se afogou no load. Os guards (SCENE/MASK) e o fix P2 podem
  ter funcionado; o muro agora é MEMÓRIA pura no load do mapa.

## PREPARADO p/ retomar (quando device voltar)
- go-map.sh (em ports/cuphead/, FALTA SCP pro device): config de teste do mapa com
  **CUP_TEXHALF=256** (texturas no cap 256² = 1/4 da RAM das de 512²; maior alavanca
  de memória) + GPVIRT/GPLOG. Binário com fix P2 JÁ está no device.
- Navegação: go(disclaimer)→accept(título→save NEW→livro→casa)→sair p/ mapa.
- Se TEXHALF=256 ainda afogar: (a) emergency UnloadUnusedAssets quando avail<40MB
  (cuidado: durante load pode corromper, ver s11); (b) conferir swap no ext4 (rápido)
  vs vfat (lento=thrash); (c) cortar mais texturas (TEXHALF=128) ou pular bundles
  não-essenciais do mapa.

## RETOMADA pós power-cycle (s12, parte 2) — GRANDES VITÓRIAS
- Ambiente: ES **mascarado** (systemctl mask emustation; volta sozinho senão), swap
  2GB via loop1 (prio 20)+512MB, overcommit=1. go-map.sh com TEXHALF=256.
- **OOM RESOLVIDO**: com TEXHALF=256+swap+ES-off, memória ficou saudável por TODA a
  navegação (avail 90-220MB, swap quase intacto). O device NÃO afoga mais. A run
  anterior wedgeou só por falta dessas 3 medidas.
- **PLAYER 2 FANTASMA RESOLVIDO de vez**: faltava gatear o GetAnyButtonDown. Há 2:
  0xCC2854 (AnyPlayerInput GLOBAL, do disclaimer — NÃO gatear) e 0x11a672c
  (Rewired.Player.GetAnyButtonDown, self=Player — GATEAR). Criei gp_GetAnyButton(Down)_player
  com player_blocked. PROVA: menu de pause NÃO tem mais "REMOVE PLAYER 2".
- Naveguei autônomo até a transição casa→mapa via EXIT TO MAP (pause menu).
  ⚠️ menu de pause usa GetAxis("MenuVertical"); navegação por /tmp/gpcmd precisa de
  "down" RÁPIDO/repetido (0.4s) p/ o cursor mover; toque único às vezes não pega.

## CADEIA DE CRASHES DO LOAD DO MAPA (casa→mapa) — 3 guards
1. 0x541cdc deref [scene+24], scene=NULL -> **SCENEGUARD** (ilha, fake-scene). OK.
2. 0x8f9b1c str array[count-1], **count==0** -> count-1=0xffffffff OOB -> **MASKGUARD**
   clampa count p/ [1,0x40000]. CONFIRMADO no log: "[MASKGUARD] count=0 -> 1". OK.
3. 0x8f9b88 ldr [x0,#24], **x0=NULL** (x19, passado de 0x541dcc; deriva da fake-scene)
   -> **NULLGUARD** (skip se arg0==NULL). RECÉM-ADICIONADO, testando agora.
- ⚠️ RAIZ COMUM: a fake-scene zerada do SCENEGUARD VAZA — o código de tilemap/mesh
  downstream deriva objetos NULL dela. Pode haver MAIS crashes na cadeia (whack-a-mole,
  estilo sessões 6-9). Considerar: em vez de fake-scene, fazer 0x541c9c PULAR a montagem
  do mesh quando scene=NULL (RE maior), OU achar pq os GameObjects do mapa têm scene NULL
  (registro de cena do so-loader).
- Tudo só-leitura/guard, gameplay (casa) 100% OK. NullRef única em PlayerManager.Update
  é benigna (1×, não-fatal) — provavelmente o GetAnyButtonDown GLOBAL.

## Scripts
- go-map.sh (TEXHALF=256+GPVIRT), nav-map.sh (navegação automática; ajustar contagens
  de accept — o livro/diálogo variam de duração). Ambos no device.

## FIX DE RAIZ — CUP_SCENESKIP (substitui o island fake-scene)
- Confirmado: MASKGUARD (count=0->1) e NULLGUARD (0x8f9b88 arg0=null) DISPARARAM, mas a
  cadeia continuou (4º crash 0x541e54 `ldr w9,[x19]` x19=null) — tudo DENTRO de 0x541c9c.
  Raiz: GameObjects do mapa com scene NULL passando por 0x541c9c, que cascateia nulls.
- NOVO: hook na ENTRADA de 0x541c9c. scene = *(void**)(arg0+56) (o helper 0x8f7c48 lê
  isso). Se NULL -> pula a função inteira (return 0; epílogo é void, caller 0x541c2c
  ignora retorno). Não monta o mesh daquele GO (não renderiza) mas ZERO crash na cadeia.
- Mantidos MASK/NULLGUARD como cinto-e-suspensório (count=0 / arg0=null podem ocorrer
  p/ GOs com scene válida tb). nav-map.sh: livro come MUITO accept (transições comem
  toques); precisei ~34+26 accepts. down=1 passo OK (dir-hold curto).

## CONCLUSÃO s12: load do mapa = null-derefs SISTÊMICOS (multi-função)
- Cadeia observada (cada um numa função/local diferente da instanciação do mapa):
  1. 0x541cdc (scene+24)        -> SCENEGUARD/SCENESKIP
  2. 0x8f9b1c (count==0 OOB)     -> MASKGUARD (count[1,0x40000]) — DISPAROU
  3. 0x8f9b88 (arg0 null)        -> NULLGUARD — DISPAROU
  4. 0x541e54 (x19 null)         -> dentro de 0x541c9c (coberto p/ SCENESKIP em tese)
  5. 0x542258 (x8=0, fault 0xc)  -> FUNÇÃO NOVA (lr 0x54224c); SCENESKIP nem disparou
- ⚠️ SCENESKIP (skip 0x541c9c se [arg0+56]==null) NÃO disparou no último teste, mas
  crashou em 0x542258 -> ou o "scene null" se manifesta diferente do offset 56, OU são
  objetos null DIVERSOS (não só "scene") espalhados pelo grafo do mapa.
- DIAGNÓSTICO: NÃO é bug pontual. O grafo de objetos da cena do MAPA não está
  inicializado no so-loader -> dezenas de funções internas da Unity (Tilemap/SpriteMask/
  mesh/scene) leem campos null. Whack-a-mole NÃO converge.

## DIREÇÃO DA RAIZ (próxima sessão — substancial)
- Hipótese A: a CENA/level do mapa (não só os atlas de textura) não é carregada. Temos
  339/419 bundles (atlas_world_* sim), mas pode faltar o bundle de CENA/tilemap do mapa
  ou um level*.assets. Conferir o que o jogo tenta abrir no EXIT TO MAP (CUP_DLLOG /
  open-redir no momento da transição) e deployar o que faltar.
- Hipótese B: SceneManager do so-loader não seta a "active scene"/scene-handle do mapa
  -> GameObjects nascem sem scene. RE de SceneManager.LoadScene/sceneCount no il2cpp.
- Ferramenta: ligar CUP_DLLOG só na transição p/ ver opens/misses do mapa; Il2CppDumper
  p/ achar o método que carrega a cena do overworld (Scenes.* / Level* / WorldMap*).

## 🎯 MURO REAL ISOLADO (s12 fim) — load assíncrono do mapa NÃO COMPLETA (NÃO é memória!)
- Com SCENESKIP, a transição casa→mapa NÃO CRASHA mais. Mas trava na AMPULHETA de loading:
  o render spina ~1560fps VAZIO (loop roda, nada renderiza) — depois HANG (15540 fixo).
  Memória saudável (avail 232MB). Confirmado: trava no loading mesmo com texturas feias.
- PSPY no momento travado (PreloadManager mgr=0x55c540f980): **preloadQ=1 integQ=1** presas.
  jobmgr +70/+168/+16c = 0 (sem jobs pendentes do worker).
  - **PQ[0]** op=...d180 vt=**u+0x123e8d8** done72=**0** w64=0 w68=0
    bg(+80)=u+0x873e68 integ(+88)=u+0x44cc18 q(+112)=u+0x873fd0
    vtable: [0]=0x72b8ec [1]=0x873fac [2]=0x873cac [3]=0x873ca4 [4]=0x44cbd4 [5]=0x449454 [6]=0x1af59c [7]=0x1af5a4
  - **IQ[0]** op=...ae40 vt=**u+0x123e658** done72=**0** w64=1679844000 w68=127
    bg(+80)=u+0x871f34 integ(+88)=u+0x872758(IntegrateOp) q(+112)=u+0x44cbe8
    vtable: [0]=0x872c0c [1]=0x872cd4 [2]=0x873cac [3]=0x873ca4 [4]=0x44cbd4 [5]=0x449454 [6]=0x872744 [7]=0x87274c
- DIAGNÓSTICO: a op de preload da cena do mapa fica com done72=0 — o processamento de
  BACKGROUND (op->vt+80 / 0x873e68) nunca marca a op como pronta -> nunca migra p/ conclusão
  -> AsyncOperation.isDone nunca true -> loading eterno. MESMA máquina do boot do título
  (sessões 4-9: integQ/FORCEINTEG/WaitForAll). ⚠️ CUP_DRAINPRELOAD=8 PIOROU (hang em vez de
  spin) — uma das chamadas UpdatePreloadingSingleStep BLOQUEIA (provável integ da IQ op
  esperando algo). PQ[0] precisa do BACKGROUND completar, não da integração.
- PRÓXIMO (próxima sessão, substancial): RE de 0x873e68 (bg da PQ op vt 0x123e8d8) — por que
  done72 fica 0 (thread UnityPreload 0x8736cc não roda? bloqueada? espera semáforo?).
  Identificar a classe da op pelas vtables (0x123e8d8 / 0x123e658) no Il2CppDumper/RE. Ver se a
  UnityPreload thread está viva (ela seta op[72]=1). Il2CppDumper em ~/cuphead-build/dump.

### ⚠️ LEVERS BRUTOS TESTADOS E DESCARTADOS (s12) — fix DEVE ser cirúrgico
- **CUP_DRAINPRELOAD=8** (drive UpdatePreloadingSingleStep/frame): PIOROU — render HANG (uma
  chamada bloqueia na integ da IQ op).
- **Remover CUP_NO872774** (FORCEINTEG NOPa 0x872774 = força integração global): **QUEBRA O
  BOOT** (render trava em 1860 ANTES do disclaimer — a race que NO872774 evita no título volta).
- **CUP_DRAINWAIT+DRAINWAIT_GFX** (WaitForAll/frame = force-complete): **QUEBRA O BOOT** (render
  1860). PSPY no boot travado: **preloadQ=0 integQ=1** — JÁ EXISTE 1 op de integração PERSISTENTE
  (done72=0) desde o BOOT. O fluxo normal TOLERA ela (não a espera → boot/menu OK); a tela de
  loading do MAPA ESPERA por TODAS as ops → trava nessa op eterna.
- 🔑 INSIGHT-CHAVE: a op presa (vt 0x123e658, integ=0x872758) NUNCA completa e está pendente
  desde o início. Fix cirúrgico = OU completar essa op específica (RE do que falta no
  IntegrateOp 0x872758 p/ ELA setar done72=1) OU fazer a tela de loading do mapa NÃO esperar
  por ela (achar a checagem isDone/HasPendingOperations da loading-screen e excluir essa op).
  NÃO usar levers globais (quebram o boot do título).

## ESTADO FINAL s12 (grandes vitórias — gameplay alcançado)
- ✅ OOM/wedge RESOLVIDO: TEXHALF=256 + swap 2GB(loop1) + ES mascarado + overcommit=1.
  Memória saudável por TODA a navegação (avail 79-220MB). Device não morre mais no mapa.
- ✅ PLAYER 2 FANTASMA RESOLVIDO: player_blocked(id>=1) em TODOS os hooks de input
  (string+int) E nos GetAnyButton(Down) de Player (0x11a66f8/0x11a672c), mantendo o
  AnyPlayerInput GLOBAL (0xCC2854) livre p/ o disclaimer. Pause menu sem "REMOVE PLAYER 2".
- ✅ GAMEPLAY: disclaimer→título→save→livro→casa do Elder Kettle JOGÁVEL, controles OK.
- ✅ Navegação autônoma: CUP_GPVIRT + /tmp/gpcmd. dir-hold curto (down=1 passo, CUP_GPV_DIRF).
  Diálogo do Elder Kettle avança com JUMP (não só accept); é longo.
- 🔻 Áudio: FMOD vai no AudioTrack/JNI (não OpenSL); falha init. Pendente, não bloqueia.
- 🔻 MAPA: bloqueado por null-derefs sistêmicos (acima).
- Binário atual: ~/nextos_ports_android/ports/cuphead/cuphead (SCENESKIP+MASK+NULLGUARD+
  P2-gate+dir-hold). Device: /storage/roms/cuphead-recon/. Scripts: go-map.sh, nav-map.sh.
  (skip da montagem de mesh p/ GO sem scene). Quando o mapa renderizar: validar controle
  no mapa (entrar numa fase), depois áudio (FMOD AudioTrack/JNI), depois empacotar.
- Se ainda travar: investigar se MASKGUARD precisa disparar / mais guards de cena.
- ⚠️ captura de fb via /dev/fb0 fica FLAKY (0 bytes) durante cena nova; usar logs
  (render rate, CRASH, hits) como sinal primário; fb quando der.
- Validar áudio com o GetState fix (esperar [SL] CreateAudioPlayer + bq_Enqueue).
- Re-ativar GC pós-título com segurança (gate de ociosidade) p/ não OOM no gameplay.
