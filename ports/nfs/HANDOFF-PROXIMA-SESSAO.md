# NFS Most Wanted (Mali-450 so-loader) — HANDOFF p/ próxima sessão (2026-06-15)

Device em **192.168.31.164** (subnet .31, NÃO .0/.1; senha nextos). Port em `~/nextos_ports_android/ports/nfs/`.
Build: `./build.sh`. Rodar no device: `cd /storage/roms/nfs && ./go.sh` (ou os g*.sh).
Ghidra: `~/re-tools` (`export GHIDRA_INSTALL_DIR=~/re-tools/ghidra_12.1.2_PUBLIC JAVA_HOME=~/re-tools/jdk-21.0.11+10`).
RE: projeto JÁ ANALISADO em `~/re-tools/proj_an` (nfsan); decompile rápido c/
`python3 ~/re-tools/dec_an.py <addr>`; capstone confiável p/ Thumb c/ `python3 ~/re-tools/fdis.py <addr> [N]`
(libapp é mistura ARM/Thumb; .text VA=file offset 1:1; addr PAR=ARM, ÍMPAR=Thumb).
Workflow de teste de tela: `cp auto.raw snap.raw` no device + scp + PIL `frombytes RGBA 1280x720 + FLIP_TOP_BOTTOM`.
auto.raw é escrito a CADA present (race c/ scp → snapshot via cp; md5 do auto.raw p/ detectar mudança).

## 🏁🏁 PARTE 14 (2026-06-15) — EULA BYPASSED → DENTRO DA CORRIDA (HUD ok, mundo 3D PRETO)
**🎉 CHEGAMOS AO GAMEPLAY!** Bypass do EULA = criar o arquivo de aceite `/active_accepted`
no disco → o flow PULA a tela active_accept (inacessível: checkbox touch-only + navegação de
foco NÃO funciona p/ NENHUM input — touch/DPAD/stick, só A=confirm como evento). O check do
aceite é via **stat/access** (não open/fopen — por isso não aparecia no FOPENLOG). Paths
criados (accept-setup.sh, chamado por grun.sh/gnet.sh): `data/Android/data/com.ea.games.nfs13_row/files/active_accepted`
+ `data/files/active_accepted` + `data/files/var/active_accepted` (mantidos todos; qual exato
= TODO via NFS_SEEKLOG/stat). **PRECISA netstatus=3** (default no binário) p/ não cair no
NO_CONNECTION_PROMPT. Boot: EULA(skip)→tutorial_check→**race tutorial**. HUD 2D RENDERIZA
(posição P, timer, minimapa c/ seta, velocímetro 888, NITRO/boost) — pipeline 2D ok.
**❌ MURO ATUAL = MUNDO 3D PRETO (só ~6% non-black = HUD).** O renderer **Isis** (3D) roda
(log: "Isis Renderer Capabilities", Tier=Low, PERFORMANCE level=0, "BoundShader: 0/4",
"VertexBufferData", carrega race.m3g + modelos de carros bmw/camaro/etc) mas a cena 3D não
aparece. Sem erro de shader compile/link no log. Warnings suspeitos: "OnSceneChanged: could
not find spike strip locator", "SetTarget: could not find locator_camera_rearview, using
existing rear view camera position" (locators de CÂMERA faltando → câmera pode estar errada/
cena off-screen). Hipóteses 3D: (1) câmera/projeção 3D errada (locators faltando) → mundo
fora de vista; (2) cena 3D renderiza em FBO offscreen não-composto (só HUD chega à tela);
(3) shaders 3D (m3g) do Isis rodam mas saída preta (lighting/depth/clear). Investigar:
NFS_DRAWLOG (fbo/prog por draw), capturar GL_FRAMEBUFFER_BINDING dos draws 3D, ver se a
geometria 3D é submetida (glDrawElements com contagem >0) e p/ qual FBO. Modelos .m3g
(formato M3G/JSR-184) carregam "directly" (warning). gnet.sh `<netstatus>` = launcher.

## PARTE 13 (2026-06-15) — GAMEPAD FUNCIONA + CONECTIVIDADE; muro = checkbox do EULA
**INPUT DO MENU = GAMEPAD (MogaController), não toque nem physicalKey.** O log da engine
mostra `ShowMogaHighlight` no EULA. Caminho: `Java_..._MogaController_nativeOnKeyEvent`
(0x265ea0) recebe (env, thiz, **KeyEvent**) e lê `KeyEvent.getKeyCode()` + `getAction()`
via **CallIntMethodV** (slot 50). Switch em (keycode-0x13); handled: 19-22=DPAD, 96=A
(confirm), 97=B, 99/100=X/Y, 102-105=L1/R1/L2/R2, 108=START, 109=SELECT. getAction DEVE
ser 0(DOWN)/1(UP) senão a engine BAILA.
- **jni_shim.c:** getKeyCode/getAction methodIDs NÃO cacheados (sem Java MogaController) →
  durante a injeção usamos CONTADOR (g_moga_calln): 1ª/2ª chamada CallInt=keycode, 3ª=action.
- **main.c:** injetor `moga.txt` (keycode Android) chama nativeOnKeyEvent c/ KeyEvent fake +
  DOWN/UP. VERIFICADO end-to-end: log "Inside nativeOnKeyEvent 20 → Key Event Key Down →
  Listener Key Press → MogaKeyCode Key Press Dpad Down". A=CONFIRM toca `btn_generic_accept`.
**🌐 CONECTIVIDADE (causa do EULA travado):** pressionar A ATIVA o accept, mas o flow roteava
p/ `NO_CONNECTION_PROMPT` (saída sem ligação no flow = beco) porque o jogo se via OFFLINE.
Cadeia JNI: `Nimble.getComponent()→INetwork`, `INetwork.getStatus()→Network$Status`,
`Status.ordinal()→int`. Stub dava 0 = sem conexão. **FIX (jni_shim.c):** getStatus()→sentinel;
ordinal(sentinel)= **3** (validado por sweep: 0/1/2/4/5 disparam NO_CONNECTION, **só 3 não**).
NFS_NETSTATUS sobrescreve. Com netstatus=3: SEM NO_CONNECTION ✅.
**❌ MURO ATUAL = CHECKBOX "I have read and accept" (texto VERMELHO no topo).** Com netstatus=3,
A (CONFIRM) → fade/transição → **RECARREGA active_accept** (volta ao EULA) porque o checkbox
de aceite NÃO está marcado (accept rejeitado ANTES de escrever /active_accepted — sem erro de
write). O checkbox é **touch-only e inacessível**: (a) toque (nativeTouchScreenEvent) NÃO é
consumido pelo menu (testado, 0 efeito); (b) DPAD não move o foco (foco preso no CONFIRM; A
sempre toca btn_generic_accept; nenhum botão 19-109 marca o checkbox — todos revertem ao EULA);
(c) a navegação/foco do menu parece usar input POLLED, não eventos (igual ao toque). Layout do
EULA: active_accept/aas_inner/frame/**btn_options_small2** (provável CHECKBOX)/btn_options_large_active
(=CONFIRM focado)/btn_options_large_idle. Após aceite o flow vai p/ tutorial_check → carrega
garage.m3g (cena 3D). 3 telas: active_accept{,_eula,_privacy}.sb.
**RE do accept handler FALHOU (tentado): ** stack scan (NFS_STACKSCAN) = MUITO ruidoso
(0x388438/0x7af998 etc = valores STALE na stack; 0x388428 é só um setter trivial,
não o handler). __builtin_return_address(1+) = NULL (engine omite frame pointers, só dá
nível 0 = 0x96afb0 = wrapper Nimble da conectividade). Ghidra decompila ARM/Thumb dessa
região como LIXO (jump tables/Thumb mal-analisado). NÃO persistir nessas vias.
**PRÓXIMO (forçar o aceite — em ordem de promessa):**
  1. **Causa-raiz provável = TOUCH-TAP não é POLLED pelo menu.** O tap-detector 0x54b99c
     (UP subtype 3) DETECTA o tap (retorna 1) mas o caller (nativeTouchScreenEvent) IGNORA
     o retorno e o detector NÃO seta um flag persistente "tap occurred" que o menu leia. No
     Android real o Java usa o retorno OU há um flag. Achar onde o menu LÊ o tap (poll por
     frame) e fazer o detector setá-lo / injetar lá. Isso destravaria o checkbox por toque.
  2. **Forçar o flag do checkbox.** Achar o objeto da tela active_accept (via observer
     KEYOBS=obs do probe) e setar o byte do checkbox, OU achar/patchar o accept handler
     (não pela stack — usar Ghidra com auto-análise melhor OU hook no flow-output-fire).
  2. **Fix navegação POLLED:** achar onde o menu LÊ o estado do DPAD/foco por frame (não
     via evento) e injetar lá — destrava navegar até btn_options_small2 e marcá-lo.
  3. Bypass do flow: forçar avanço de active_accept p/ o próximo node.
Diag: NFS_NETSTATUS, NFS_STACKSCAN; net.txt (ordinal runtime); moga.txt; gnet.sh `<n>`
(launcher c/ NFS_NETSTATUS); mseq.sh `<delay> <kc...>` (sequência de gamepad).

## PARTE 12 (2026-06-15) — CONTROLES: toque DESPACHA mas menu não consome; tecla CHEGA na engine
**FIX REAL aplicado (jni_shim.c): `IsSameObject` (JNIEnv slot 24) estava NO `jni_stub`→retornava 0.**
O dispatch de toque (`nativeTouchScreenEvent` 0x54d764 → getter `0x54a244` itera lista
intrusiva de handlers @VA 0xadfd24 → `IsSameObject(env, handler->view, thiz)`) SEMPRE
falhava o match → todo toque descartado. Agora `IsSameObject` compara ponteiros + passamos
a VIEW REAL do handler como `thiz` (handler->vtable[2]() = 0x3e1cc) → match OK, getter
retorna handler ≠0, vtable9-check passa, evento entregue ao input-target via `r4->vtable[2]`
(=`0x54b99c`, detector de TAP: grava round(coord+0.5) no DOWN, checa |down-up|<14/15px no UP).
**COORDS = PIXELS de tela (NÃO normalizado):** vtable7()=1 (sem escala) → passar px crus
(640,454 p/ CONFIRM). Default agora é raw (NFS_TAPNORM=1 normaliza=ERRADO, só p/ comparar).
**MAS:** mesmo com toque despachando 100% certo (verificado: getter/handler/r4/ev2 logados em
taplog.txt), **o EULA NÃO reage a tap limpo** (CONFIRM/USER AGREEMENT/PRIVACY → tela
IDÊNTICA ee19cc3c; press longo de 1s → mid sem highlight; sem-MOVE idem). Tela do EULA é
ESTÁVEL sem input (ee19cc3c), então o menu NÃO consome o caminho `nativeTouchScreenEvent`.
**Tecla:** `nativeOnPhysicalKeyDown` (0x54cb98) CHEGA na engine — BACK(4) é processado e
sai/quita o app (prova end-to-end!); mas DPAD/ENTER/A/etc (19/20/23/66/96/...) NÃO navegam
o EULA (touch-only accept). Flag de input-disable @0xadfd44 (compartilhada touch+key); NFS_FORCEINPUT zera.
**EULA-bypass por arquivo NÃO serve:** o aceite NÃO é checado via open/stat/VFS (log amplo
NFS_FOPENLOG+SEEKLOG só mostra os flow .sb /published/flow/active_accept*.sb) → é ação
in-engine do flow, sem persistência em arquivo.
**HIPÓTESE p/ continuar (em ordem):**
  1. **Menu usa input POLLED, não o evento.** O detector 0x54b99c só GRAVA estado no
     input-target (offsets +4=state, +8=down_x, +0xc=down_y por ponteiro) e RETORNA bool
     que o caller IGNORA. Algo no `nativeOnRunLoopTick` deve LER esse estado (ou um input
     global diferente). Achar o leitor: hookar métodos do input-target durante o tick, OU
     procurar quem lê os offsets +4/+8/+0xc. Talvez o menu leia OUTRO objeto de input.
  2. **Flow travado em loading/online** (handoff antigo: busy-loop init Nimble/ITracking
     stubado). EULA pode estar não-interativo até load/online completar. Render avança
     (frame 13000+) mas a interatividade do flow pode estar gated. Ver se há overlay de
     loading ativo / destravar o getComponent(ITracking).
  3. Forçar o flow a avançar programaticamente (achar fn "advance flow"/"set accepted" do
     active_accept.sb e chamar), OU passar o GameGLSurfaceView REAL (não 0x3e1cc fake).
Infra de input em main.c: tap.txt "x y"(px), key.txt keycode; NFS_TAPHOLD(frames, default
6), NFS_TAPMOVE(reativa MOVE no hold), NFS_TAPNORM, NFS_TAPRAW, NFS_FORCEINPUT. Logs em taplog.txt.
graw.sh/grun.sh = launchers; gprobe.sh = NFS_INPROBE (estado da lista de toque).

## ✅ O QUE JÁ FUNCIONA (não regredir!)
- **Render completo** (era 100% preto). Causas resolvidas: getTotalMemory=0→2048MB;
  density softfp/hardfp (pcs aapcs no retorno float); 🔑getWidth/getHeight=0→1280×720
  (Primary view 0x0→glViewport 0x0→tudo culled); OBB→ler do disco (isObbAssets→0).
- **TEXTO/fontes** reais do jogo (font_shim.c + stb_truetype carregando gothambook.ttf).
  Bugs resolvidos: JNI Call*MethodV recebem va_list (não varargs); make_jstring COPIA.
- **LOGOS do splash**: EA, Firemonkeys, NEED FOR SPEED MOST WANTED renderizam PERFEITOS.
  (atlas_rebind: sprites de imagem do .sba ligam tex=0=preto; religa o último atlas
  não-quadrado nos draws sem textura. egl_shim.c, PADRÃO ligado, NFS_NOATLASHACK desliga.)
- **EULA** renderiza com texto legível (painéis escuros + "PRIVACY/USER AGREEMENT/CONFIRM").

## ❌ ISSUE A — "LOGO BRANCA / decorações ainda bugadas" (multi-atlas)
SINTOMA: depois do MOST WANTED, a tela de disclaimer ("All experiences...") tem o
**spinner de loading e as decorações (speed_lines/triangles/blur) LAVADOS/brancos**, e
qualquer sprite de imagem após os atlas de menu carregarem fica errado.
CAUSA-RAIZ: os sprites de IMAGEM do .sba (logos/ícones/decorações) **ligam tex=0**
(nenhuma textura) → GLES2 amostra (0,0,0,1) preto opaco. O atlas COLORIDO carrega certo
mas o **link sprite→atlas resolve NULL na engine — game-wide** (só texto/glyph e quads
sólidos ligam textura). O atlas_rebind contorna religando o último atlas grande não-
quadrado, MAS após o splash carregam vários atlas de menu/loading (tex 9-19 ~1020x1016)
→ g_nfs_atlas_tex muda → decorações do splash bindam o atlas ERRADO → lavadas.
⚠️ Per-programa FALHOU (shaders compartilhados texto+logo → religava textura de texto).
PRÓXIMOS PASSOS (em ordem de promessa):
1. **FIX REAL = achar por que a engine binda tex=0** p/ os sprites .sba. O atlas sobe como
   GL tex N (glGenTextures→glBindTexture(N)→glTexImage2D) mas o sprite binda 0 no draw →
   o id da textura no objeto Texture do .sba está 0, OU o sprite referencia outro objeto.
   RE: decompilar quem chama glBindTexture (thunk @0x00b2647c) e glGenTextures (@0x00b26534)
   — refs são PIC NÃO-resolvidas pelo Ghidra; precisa rodar auto-analysis OU scanner
   movw/movt próprio (tools_armdis.py). Achar o registro "Add texture: <name>" e o lookup
   sprite→texture. Diag prontos: NFS_DRAWLOG (fbo/tex/prog/blend por draw), NFS_TEXLOG
   (uploads + "ATLAS candidate"), NFS_TEXDUMP (salva atlas), NFS_SHADERDUMP (fonte dos
   shaders: sh4=texture2D, sh5=varColor*texture2D, sh6=constantColor*texture2D).
2. Heurística melhor (paliativo): correlacionar tex=0 draw ao atlas certo. Difícil sem o
   link. Tentado: per-programa (falhou), global último-atlas (atual, erra multi-atlas).

## ❌ ISSUE B — CONTROLES / passar do CONFIRM (navegação)
SINTOMA: nenhum input (touch nem tecla/gamepad) faz o EULA reagir → preso no CONFIRM.
INFRA JÁ PRONTA (main.c, loop de render):
- **Touch**: escreve `/storage/roms/nfs/tap.txt` com "x y" (px) → DOWN+UP em
  nativeTouchScreenEvent. Floats x,y passados como BITS via int (softfp). EULA: CONFIRM
  em (640, 534); texto de aceite vermelho y172-220 (sem checkbox visível).
- **Tecla/gamepad**: escreve `key.txt` com keycode Android (19=UP 20=DOWN 21=LEFT
  22=RIGHT 23=CENTER 66=ENTER 96=A 97=B 4=BACK) → nativeOnPhysicalKeyDown.
- **NFS_FORCEINPUT=1**: zera a flag global "input desabilitado" (@VA 0xadfd44,
  base=kdown-0x54cb98) todo frame.
DIAGNÓSTICO (por que é descartado):
- TOUCH: nativeTouchScreenEvent(0x54d764) → itera **f_55a244 (lista de handlers de toque)**
  que retorna 0. A comparação usa um VIEW-object; passamos `fake_this`, mas a engine
  registrou o handler p/ o **GameGLSurfaceView REAL** (queried via getGameGLSurfaceView
  JNI) → match falha. Coords chegam CERTAS (verificado o ABI).
- KEY: nativeOnPhysicalKeyDown processa (bl 0x5447d0) mas EULA não reage mesmo c/ flag=0
  (pode ser touch-only OU mesma falta de handler/view).
PRÓXIMOS PASSOS (em ordem de promessa):
1. **BYPASS DO EULA (mais promissor p/ VER o menu sem resolver input):** o jogo gateia o
   EULA por um flag de aceite. Strings: `/active_accepted`, `ActiveAccepted: failed to
   write user accept to "<path>"`, flow `published/flow/active_accept_eula.sb` (lido no
   boot, confirmado NFS_FSPATHLOG). FALTA: achar o PATH onde o aceite é gravado/checado
   (não é um dos .sb de flow — é storage de save/preference). Capturar via hook de
   open/stat (my_open/my_fopen logam; o aceite pode usar stat/access). Criar o arquivo
   no device → EULA pulado → menu aparece. Decompilar a fn que loga "ActiveAccepted:
   failed to write" p/ ver o path e o formato.
2. Passar o **GameGLSurfaceView REAL** como obj no nativeTouchScreenEvent (capturar o
   objeto que a engine usa via getGameGLSurfaceView no jni_shim e reusar como fake_this
   do touch).
3. Registrar o touch listener / investigar a thread de input.
4. Gamepad via MogaController (nativeOnKeyEvent/nativeOnMotionEvent recebem KeyEvent/
   MotionEvent OBJETOS — precisa fakear via jni_shim; DPAD=0x13-0x16, A=0x60 no switch).

## INFRA / PEGADINHAS
- Wrappers GL via TABELA nfs_shims[] (so_resolve usa dlsym, GetProcAddress NÃO pega GL core).
- /dev/fb0 NÃO reflete Mali → usar glReadPixels (NFS_SEQSHOT salva seq_NNNN.raw na vfat;
  converter: Image.frombytes RGBA 1280x720 + FLIP_TOP_BOTTOM). Writes /tmp falham durante nfs.
- ES mascarado. Matar nfs ANTES de scp do binário (file busy). FMOD spam via syscall = thrash.
- Splash: copiei splash_1500.sba (real) por cima dos stubs splash_1775/1333.sba (262B) no
  disco em published.1x/texturepacks_ui/ (16:9 carregava stub vazio).
- Fixes Mali do Bully portados: highp→mediump frag (my_glShaderSource), GL_TEXTURE_MAX_LEVEL
  skip + mipmap→LINEAR (my_glTexParameteri). Ref: ~/nextos_ports_android/ports/bully/src.
