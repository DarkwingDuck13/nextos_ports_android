# NFS Most Wanted (Mali-450 so-loader) — HANDOFF p/ próxima sessão (2026-06-15)

Device em **<device-ip>**. Port em `ports/nfs/` (neste repo).
Build: `./build.sh`. Rodar no device: `cd /storage/roms/nfs && ./go.sh` (ou os g*.sh).
Ghidra: `~/re-tools` (`export GHIDRA_INSTALL_DIR=~/re-tools/ghidra_12.1.2_PUBLIC JAVA_HOME=~/re-tools/jdk-21.0.11+10`).
RE: projeto JÁ ANALISADO em `~/re-tools/proj_an` (nfsan); decompile rápido c/
`python3 ~/re-tools/dec_an.py <addr>`; capstone confiável p/ Thumb c/ `python3 ~/re-tools/fdis.py <addr> [N]`
(libapp é mistura ARM/Thumb; .text VA=file offset 1:1; addr PAR=ARM, ÍMPAR=Thumb).
Workflow de teste de tela: `cp auto.raw snap.raw` no device + scp + PIL `frombytes RGBA 1280x720 + FLIP_TOP_BOTTOM`.
auto.raw é escrito a CADA present (race c/ scp → snapshot via cp; md5 do auto.raw p/ detectar mudança).

## 🎨🟢🏆 PARTE 18 (2026-06-15) — TEXTURAS MAGENTA/VERDE DO CENÁRIO RESOLVIDAS (ABI softfp/hardfp)
Confirmado: "todas as texturas estão ok, bonito mesmo". **CAUSA-RAIZ = mismatch de ABI de float.**
`readelf -h`: libapp.so (engine) = **SOFT-FLOAT** (0x5000200, float args nos regs CORE r0-r3);
nosso binário/shim = **HARD-FLOAT** (0x5000400, float no VFP). Os wrappers GL com **float args**
(egl_shim.c) liam os floats do VFP = LIXO → cor de material (glUniform4f) e **cor de vértice
default (glVertexAttrib4f)** garbage → céu/prédios **magenta/verde saturados, DIFERENTES por
objeto** (carro/estrada usam ARRAY de cor/textura, não float-arg → corretos; por isso só o cenário).
**FIX:** `__attribute__((pcs("aapcs")))` em TODOS os wrappers GL com float args (lê os floats dos
regs CORE = softfp, igual ao engine) + wrappar os que faltavam e registrar em nfs_shims:
glUniform1f/2f/3f, glVertexAttrib1f/2f/3f, glClearColor, glBlendColor, glClearDepthf, glDepthRangef,
glLineWidth, glPolygonOffset, glSampleCoverage, glTexParameterf (+ pcs nos já-existentes
glUniform4f/glVertexAttrib4f). Mesma técnica que jni_shim já usava p/ ScreenDensity (float de
retorno softfp). **🔑 LIÇÃO p/ TODO so-loader: binário Android=softfp, runtime armhf=hardfp →
qualquer fn do shim que o engine chama passando FLOAT precisa pcs("aapcs"); as *fv/Matrix*fv (só
ponteiro) NÃO.** Diag (default-off) p/ chegar aqui: NFS_SWAPRB/ETCDUMP/NOLIGHT/NOHEMI/NOVCOL/CMASKLOG/
FORCEGREEN/UNILOG. Becos descartados: ETC1 do cenário decodifica normal (texture2ddecoder), colorMask
(0 no CMASKLOG), R/B swap (magenta simétrico, listras do carro azuis ok), lighting (NOLIGHT/NOHEMI
não corrigiram). Commit 80921ad. ⚠️ Os HANGS no boot (frame ~1260/1350) eram o **emustation
voltando**→`systemctl mask emustation` resolve (não só stop). FALTA POUCO.

## 🔊🟢🏆 PARTE 17 (2026-06-15) — ÁUDIO FUNCIONANDO! init + latência + música + SFX

**O "33" do init NÃO era INVALID_SPEAKER — era falha de criação de thread.** Via Ghidra
(libfmodex), a cadeia `System::init(0x404dc) → FUN_a50ec → FUN_a9120` mostrou que `FUN_a9120`
é um helper de **criação de thread** que retorna `0x21`(=33) em falha de pthread. Ele cria a
thread do mixer com **SCHED_FIFO de tempo-real (prio 90-99)**, que falha no so-loader
(bionic→glibc; o sched_param fica aliasado dentro do attr glibc de layout diferente).
Confirmado por inline-hook (`NFS_FMODHOOK`): das candidatas, só `FUN_a9120` retornava 33.

### Fixes aplicados (todos em src/, default-ON; gameplay= grun.sh c/ NFS_SOUND):
1. **Thread do mixer (imports.c `my_fmod_create_thread` + `fmod_install_replace`):** detour de
   8 bytes substitui `FUN_a9120` por criação de thread **glibc limpa, normal (sem RT)**.
   → `System::init -> 0`, `EventSystem::init REAL -> 0`, OpenSL→SDL abre (44100/2ch).
2. **Stub EventSystem::init reescrito (imports.c):** instala o thread-fix, faz `setOutput(22=
   OpenSL)`, e chama o **EventSystem::init REAL** (antes pulava→SoundManager sem EventSystem).
3. **Latência — backpressure no ring (opensles_shim.c):** o shim aceitava enqueues ilimitados
   → ring crescia de ~280ms a ~1s (a "batida atrasada 1s"). Agora a drenagem do callback do
   SDL dispara MENOS callbacks ao FMOD quando o ring passa do alvo (`target_ring_bytes`,
   NFS_RINGMS=60ms) → **ring estável ~80ms, sem deriva**. SDL buffer 4096→1024 (~93ms→23ms).
4. **Música (jni_shim.c):** `isAnyMusicPlaying`/`isMusicActive` (CallStaticBooleanMethod
   retornava 1) → o jogo achava que havia música do usuário e **suprimia a própria trilha**
   (MusicManager::PlayNextTrack break). Agora MID_IS_MUSIC_PLAYING → 0. **Música toca.**
5. **🔑 SFX/eventos (imports.c): prioridade RT das threads do mixer = DEFAULT-OFF.** Com RT
   FIFO 99, as threads de áudio **starvavam o loader** → `getEvent` dava 89(EVENT_NOTFOUND)
   p/ motor/transmissão e 19 p/ nitro/pneus/sirene (RACY). Com RT-off, o loader respira →
   **motor, nitro, pneus, sirene da polícia CARREGAM** (só `transmission/sports` resta=19).
   NFS_RT=1 reativa RT moderado (prio 5) se houver chiado/underrun.

### Wrappers FMOD de diagnóstico (em nfs_shims[], gated NFS_SNDLOG; sempre chamam o real):
`EventSystem::load`, `setMediaPath`(C++/C), `System::createSound/createStream`(C++/C),
`EventSystem::getEvent`. Logam só falhas (ret!=0). `fmod_force_sw()` limpa FMOD_HARDWARE(0x20)
→ SOFTWARE(0x40) (o jogo pede HW decode de MP3 que não temos). Envs: NFS_SNDLOG, NFS_RINGDBG
(profundidade do ring), NFS_RINGMS, NFS_RT, NFS_FMODRTPRIO, NFS_NOTHREADFIX, NFS_NOFMODSW.

### RESTA (polimento, todos minoritários):
- `transmission/transmission/sports` → 19 (1 evento de troca de marcha).
- `loading_01.mp3` (música da tela de loading) → 19: MP3 com tag ID3; codec MPEG existe no
  libfmodex mas a detecção/stream falha (a música da PLAYLIST in-game toca — é outro caminho).
- Jingle do splash EA/Firemonkeys: race (o som é pedido antes do ui.fev terminar de carregar;
  "só na 1ª vez"). Ordering do jogo, difícil sem mudar timing.
- Investigar: por que streamed waves dão FORMAT(19) (VFS ok/thread-safe; ver se o event system
  pede HW decode internamente — getEvent não passa pelos meus wrappers de mode).

## 🔊🔴 PARTE 16 (2026-06-15) — [SUPERADA pela PARTE 17] hipótese INVALID_SPEAKER (era falsa)
> NOTA: a teoria de speaker-mode abaixo foi REFUTADA na PARTE 17 — o 33 é falha de thread, não
> de speaker. Mantido por histórico do RE feito.

## 🔊🟡 PARTE 16 (2026-06-15) — ÁUDIO FMOD: System::init = 33 (INVALID_SPEAKER) — NÃO RESOLVIDO, MUITO INVESTIGADO

**Estado:** jogo JOGÁVEL (vídeo+controle) mas SEM SOM. O caminho de som é OPT-IN
(`NFS_SOUND=1`, nos launchers `gsound.sh`/`gdbgsnd.sh`); o launcher PADRÃO (`grun.sh`/`go.sh`)
NÃO seta NFS_SOUND → usa o STUB (EventSystem::init finge FMOD_OK, estável, sem áudio).
**NÃO ligar NFS_SOUND no launcher padrão** até resolver (FMOD falha → spam createSound).

### O que o NFS_SOUND faz hoje (src/imports.c, fmod_es_init_stub):
Intercepta `EventSystem::init`. Resolve o System real via `EventSystem::getSystemObject`,
e ANTES do init faz: `setOutput(22=OpenSL)` → `setSpeakerMode` → `getNumDrivers`/`setDriver(0)`
→ `getDriverCaps` → `setSoftwareFormat(44100,PCM16,2)` → **SWEEP de speaker modes** com
`System::init`+`System::close` entre tentativas. Também tem `NFS_OUTSWEEP` (varre output types)
e `NFS_FMODSWEEP` (testa quais setOutput aceita). dlopen de libOpenSLES → shim
(opensles_shim.c, ponte OpenSL ES → SDL2 audio; SDL escolhe driver sozinho = pulseaudio).

### FATOS ESTABELECIDOS (todos medidos no device):
1. **33 = FMOD_ERR_INVALID_SPEAKER** — CONFIRMADO. As strings de erro FMOD estão no
   `libapp.so` na ordem padrão do `fmod_errors.h`: índice 25="FMOD was not initialized
   correctly..." (=INITIALIZATION, é o erro do createSound depois), 33="An invalid speaker
   was passed to this function based on the current speaker mode." (=INVALID_SPEAKER).
2. **A config toda é VÁLIDA e aceita, e mesmo assim init=33:**
   - `setOutput(22)=0`, `getNumDrivers ret=0 n=1`, `setDriver(0)=0`,
   - `getDriverCaps(0) → caps=0x18 (PCM8|PCM16) ctrlrate=48000 ctrlspeaker=2 (STEREO)`,
   - `setSoftwareFormat(44100,PCM16,2)=0`,
   - SWEEP: `setSpeakerMode(2) read=2 init=33`, `setSpeakerMode(1) read=1 init=33`
     → **setSpeakerMode GRUDA** (getSpeakerMode confirma o valor) e init AINDA dá 33.
3. **🔑 É UNIVERSAL — todos os output types dão init=33, INCLUSIVE NOSOUND.** O `NFS_OUTSWEEP`
   testou output 0..24: os aceitos (setOutput=0) foram 0(auto),2(NOSOUND),3,4,5(WAVWRITER*),
   21(AudioTrack),22(OpenSL); os outros deram setOutput=66 (não suportados neste build).
   **TODOS, incluindo NOSOUND (output=2), deram init=33.** NOSOUND não usa hardware nem
   speakers → se ele falha com INVALID_SPEAKER, **o problema NÃO é o output nem o shim OpenSL**,
   é o **núcleo do System::init** (setup de DSP/channel-group/speaker), independente de tudo.
4. **NÃO há FindClass("org/fmod/FMOD")** no trace → FMOD aqui NÃO consulta o AudioManager via
   JNI (hipótese descartada). flags=0 e flags-do-jogo: ambos 33.

### RE feito (libfmodex.so, ARM 32-bit, .text VA=file offset 1:1, segmento LOAD vaddr=0 off=0):
- `FMOD::System::init` (C++) @ **0x9e650**: valida handle via `0x37fc4`, depois chama o init
  interno **0x404dc** com (internal_ptr, maxchannels, flags, extradriverdata).
- `FMOD_System_Init` (C API) @ 0x133f4 (só valida handle em lista, retorna 0x25 se inválido).
- **init interno @ 0x404dc**: erros propagam via `subs r7,r0,#0; bne #0x4057c` após cada
  subchamada (r7=resultado; sai em 0x40524 devolvendo r7). Init do OUTPUT é a chamada indireta
  `ldr pc,[r7,#0x110]` @ 0x406e0 (p/ NOSOUND retorna 0=ok). **O 33 vem DEPOIS do output**, no
  setup de DSP/channel-group/speaker (região 0x40714–0x40a88). Candidatos (subcalls c/ check
  `bne 0x4057c`): `0x78494`@0x408d4, **`0x3a540`@0x40954**, **`0x3aa04`@0x409bc**,
  indireta `[r7+0x24]`@0x40a28, `0x3a9c8`@0x40a48, `0x3a3b8`@0x40a84. NENHUMA tem `mov r0,#0x21`
  direto → o 33 vem de literal-pool ou de call mais fundo dentro dessas.
- ferramenta: capstone 5.0.7 (`Cs(CS_ARCH_ARM, CS_MODE_ARM)`), ler libfmodex em ~/nfs-stage.

### PRÓXIMO PASSO (próxima sessão, com orçamento de contexto fresco p/ RE):
Descobrir QUAL subcall de 0x404dc retorna 33. Opções:
  (a) Instrumentar em runtime: hookar/patchar as subfunções candidatas (ou inserir trampolins)
      e logar o retorno de cada uma; a primeira que dá 33 é a culpada.
  (b) RE estático mais fundo: decompilar 0x3a540 / 0x3aa04 (setup de channel group / DSP head
      com info de speaker) e achar a checagem de speaker que falha. Provável causa-raiz:
      uma TABELA GLOBAL de layout de speakers não inicializada (init_array incompleto?) ou
      falta de FMOD_Memory_Initialize/registro de plugins built-in que o so-loader não rodou.
  (c) Testar se o SoundManager do jogo tem um System PRÓPRIO (não o do EventSystem): os
      "createSound failure" vêm de `SoundManager::createSound`; se o SoundManager faz seu
      próprio System_Create+init, hookar EventSystem::init não basta — hookar `System::init`
      global. Ver no libapp como o SoundManager obtém o System.
Refs de outros ports com som (o usuário citou): Cuphead (deu trabalho), Sonic, Bully — todos
conseguiram som; ver receitas deles nas notas internas do projeto e ports/*/src.

### Como reproduzir o diagnóstico:
```
# no device (.164), nfs MORTO antes:
cd /storage/roms/nfs
export LD_LIBRARY_PATH=/usr/lib32:/storage/roms/nfs SDL_VIDEODRIVER=mali NFS_INIT=1 NFS_FORCEINPUT=1 NFS_SOUND=1
NFS_OUTSWEEP=1 ./nfs 100000 2>&1 | grep -aiE "OUTSWEEP|SWEEP|System::init|getDriverCaps"
```
Envs úteis: NFS_SPKMODE=N (força speaker mode), NFS_OUTSWEEP=1 (varre outputs),
NFS_NOSWEEP=1 (1 tentativa só), NFS_FMODOUT=N (output type), NFS_SWRATE/NFS_SWCH.

## 🏆🏎️ PARTE 15 (2026-06-15) — GAMEPLAY RENDERIZA! (mundo 3D + carro + pista + HUD)
**JOGÁVEL no Mali-450!** O launcher PADRÃO (grun.sh) renderiza a corrida: carro (Dodge
Challenger), pista, ambiente, HUD — ordenação 3D correta. **2 fixes (egl_shim.c, default ON):**
1. **DEPTH-CLEAR (mundo 3D preto):** o depth-test rejeitava TODA geometria 3D (só HUD, que
   desenha sem depth, aparecia). DEPTH=24 existe mas o clear no Mali-450 deixava o buffer ~0
   (≠1.0=far) → fragmentos z>buffer falham GL_LESS → preto. FIX my_glClear: força
   `glClearDepthf(1.0)+glDepthMask(1)` antes de cada clear com GL_DEPTH_BUFFER_BIT (0x100).
   PRESERVA ordenação (≠ NFS_NODEPTH que só desabilita). NFS_NODEPTHFIX desliga.
2. **ATLAS-HACK no 3D (prédios VERDES):** o atlas_rebind (fix de UI 2D: liga o último atlas
   em draws com tex=0) estragava a geometria 3D sem textura → verde saturado. FIX: atlas_rebind
   só em draws PEQUENOS (c<64 = UI); pulado nos grandes (3D).
**RESTA (polish):** (a) fundo/céu MAGENTA (skybox/tonemap/formato de textura? — só o distante;
carro+pista corretos); (b) ÁUDIO (FMOD "not initialized correctly"; createSound falha — ver
opensles_shim/FMOD init); (c) confirmar JOGABILIDADE real (carro responde a input de
direção/acelerador? in-game pode usar touch via tap-detector OU gamepad nativeOnMotionEvent/
nativeOnKeyEvent; a corrida roda c/ timer mas pode ser auto-drive do tutorial). Diag:
NFS_NODEPTH/NOCULL (draws grandes), NFS_NODEPTHFIX, NFS_NOATLASHACK, EGLCFG log.

## PARTE 14 (2026-06-15) — EULA BYPASSED → DENTRO DA CORRIDA (HUD ok, mundo 3D PRETO)
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
**🔬 DIAGNÓSTICO 3D FEITO (PARTE 14):** NFS_GLTRACE → `bind0=60 bindN=0 draw_fbo0=2512
draw_fboN=0` = **TODOS os draws vão pro FBO 0 (tela), ZERO offscreen** (não é problema de
composição FBO). clears=60 mask=0x4500 (color+depth+stencil). NFS_DRAWLOG (BIGDRAW n≥64) →
geometria 3D É desenhada no FBO 0: draws n=162/n=606 texturados (u0=7) prog=53 blend=1
(max n visto = 606, PEQUENO p/ pista inteira → talvez malha grande da pista não submetida,
só objetos menores/carros). NFS_SHADERLOG → **shaders 3D compilam+linkam OK** (os "NOT FOUND"
são extensões opcionais de program-pipeline que o Mali-450 não tem, benignos). CONCLUSÃO: 3D
não é FBO nem shader-compile → **câmera/transform 3D errado (geometria off-screen; locators
de câmera faltando) OU estado (blend=1 em geometria opaca, depth/cull)**. PRÓXIMO: (a) dumpar
a MVP/projeção dos draws 3D (uniforms via NFS_UNILOG) — ver se a matriz é degenerada/zero;
(b) testar glDisable(GL_BLEND)+glDisable(DEPTH_TEST)+glDisable(CULL_FACE) nos BIGDRAWs p/ ver
se a geometria aparece; (c) checar se a malha grande da pista é submetida (max n só 606);
(d) por que os locators de câmera (locator_camera_rearview etc) não são achados na cena.
Diag em egl_shim.c: NFS_DRAWLOG/GLTRACE/SHADERLOG/SHADERDUMP/UNILOG; gdraw.sh/gsh.sh launchers.

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
