# ⚡⚡ s10 (2026-06-24) — RAM CORTADA -89MB (medida) + MURO RE-DIAGNOSTICADO: é RACE não-determinístico

## ✅ FEITO: corte de RAM de -89MB no boot (pré-requisito s10 #0 RESOLVIDO, MEDIDO)
- **BUG**: o so_loader guardava a CÓPIA do arquivo .so CRU (`so_base`, `malloc(so_size)`) de CADA módulo
  o RUN INTEIRO sem uso. `so_free_temp()` só era chamado em `so_unload()` (teardown). libil2cpp=70MB +
  libunity=21MB = **~91MB de heap morto**. so_base só serve em tempo de LOAD (sec_hdr/shstrtab/elf_hdr);
  em runtime os símbolos já estão na imagem (text_base) e o phdr foi copiado p/ g_so_mods.
- **FIX** (so_util.c + main.c): novo `so_free_module_temp(so_module*)` (libera m->so_base + zera o
  espelho global p/ não double-free). Chamado em main.c logo após carregar unity+il2cpp (antes do
  so_use(g_m_unity) final) + `malloc_trim(0)` p/ devolver ao kernel. Tb libera pairipcore's so_base.
- **MEDIDO** (rss_probe novo, lê /proc/self/status): **VmRSS 181372kB → 91896kB no ponto do free (-89MB)**,
  VmSwap=0. Threads passaram de **estado D (swap-stall) → S (sleep limpo)** = boot MUITO mais determinístico
  (exatamente o que o s9 previu). Binário atual md5=3a5f8ef0 (com rss_probe; remover os 2 rss_probe se
  quiser binário limpo — são inofensivos, 2 linhas no log).

## 🔬 MURO RE-DIAGNOSTICADO via gdb FRESCO (device estável) — NÃO é o cond-handshake do s8!
- **O hang é um RACE NÃO-DETERMINÍSTICO no sync de gfx/job, não a fila-cond fixa que o s8 assumiu.**
  Boot chega 100% no render loop (`[render 0]`, roda lógica C# — prefs/permissions/analytics), depois trava.
- gdb run A: **main (UnityMain t1) travada em `pthread_mutex_lock`** chamado de **libunity+0x2d7950**
  (→ wrapper de lock libunity+0x409874 → glibc pthread_mutex_lock). x21=slot do mutex. Função em
  0x2d79xx faz time-math (ucvtf/fmul/fdiv = converte ciclos→tempo) + lock + incremento atômico de
  contador = cheira a frame-pacing/present.
- gdb run B (MESMAS flags): main travada num **futex CRU** (não no mutex). = **boot não-determinístico**:
  trava em pontos DIFERENTES. (Mutexes SÃO shimados: pthread_mutex_lock_fake→mtx_get→glibc RECURSIVE.)
- **GfxDeviceWorker (t5)** + TODOS os "Background Job."/"Job.Worker"/"AssetGarbageCol": no MESMO futex
  via my_syscall de **libunity+0xad9d00** (semáforo de job/gfx). gfxworker espera comando que main
  nunca posta. Threads "Filter1" x2 fazem spin em libunity+0x9d5ffc/0x9d6004. Threads Mali GL (t7/t8)
  em libGLESv2 (_mali_uku_wait_for_notification / _mali_osu_lock_wait) = contexto GL JÁ criado.
- bases do run gdb: UB(libunity)=0x7f8c300000 IB(libil2cpp)=0x7f84e70000 (mudam por run).

## 🔴 `-force-gfx-direct` É CÓDIGO MORTO — Unity 2021.3.42 NÃO lê /proc/self/cmdline
- `cmdline_fd()` injeta "-force-gfx-direct -force-gles20" MAS só está ligado no `my_open()` p/ path
  com "cmdline". **`[CMDLINE]` NUNCA loga** → Unity não abre cmdline via open(). libunity **não importa
  openat** (grep vazio em imports.gen.c); my_fopen tb não trata cmdline. **Logo o single-threaded-gfx
  NUNCA foi entregue ao Unity** — o worker MT sempre nasce. (CUP_1CORE tb não desliga o worker, s8.)
- ⟹ O caminho real p/ matar o GfxDeviceWorker = **BINARY-PATCH na libunity** (forçar o ramo gfx-direct
  ANTES da criação do worker), não cmdline. Achar onde libunity decide threaded-vs-direct.

## 🧵 MAPA de criação de threads (TER_JOBLOG=1) — p/ achar o gfxworker
- start=libunity+**0x368988**: ~25× = job workers + threads genéricas (a[3]=stack).
- start=libunity+**0xb9ab18**: 4× criadas LOGO ANTES de [render 0] (a[2]=0x7f8dd6ddd8 comum) =
  suspeitas FMOD/mixer OU gfx. **CANDIDATO p/ gfxworker — confirmar mapeando start→comm final.**
- start=libunity+**0xbbb9a0**: 3× DEPOIS do render0, perto de [STUB] socket = threads de rede/web.
- TER_JOBLOG loga comm="UnityMain" (nome capturado no nascimento, ANTES do Unity renomear) → p/ casar
  start-routine com o nome final ("UnityGfxDeviceW") precisa logar comm DEPOIS (ex. sleep+reread em
  thr_trampoline, ou attach gdb e casar tid↔LWP↔start).

## 🟢 PRÓXIMO (s11) — em ordem
1. **Mapear start-routine→comm final** (qual dos 3 offsets é o "UnityGfxDeviceW"). Depois disassemblar
   o CRIADOR (call de pthread_create com esse start) e achar o ramo threaded-vs-direct → **binary-patch
   p/ forçar gfx-direct** (main faz GL inline, sem worker, sem handshake). = rota mais limpa p/ IMAGEM.
2. Alternativa: como é RACE não-det, investigar se serializar a criação/sinal do gfxworker (ordem
   determinística) destrava. Mas o patch single-threaded elimina a classe toda.
3. ⚠️ device **REBOOTOU** durante a sessão → conferir swap ativo no reboot (histórico). No kernel 3.14 atual,
   **não usar zram** para evitar wedge; manter o swapfile já configurado e validar com `cat /proc/swaps`.
4. 🟡 ALTERNATIVA FORTE (decisão NextOS): **X5M** (ES3 real + +RAM) elimina o muro de shader (platform 5
   = GLES20 não existe no blob URP, só GLES3x) E muda o threading. Mali-450 exige shim ES3→ES2 + cracar
   este race. NÃO troquei de device sozinho.

## OFFSETS-CHAVE NOVOS (libunity 1.3.22 md5 8fe7856d, relativos à base)
- 0x2d7950  RA na main travada no pthread_mutex_lock (frame-pacing/present; call em 0x2d794c→0x409874)
- 0x409874  wrapper de Mutex::Lock (chama glibc pthread_mutex_lock)
- 0xad9d00  região do wait de semáforo de job/gfx (gfxworker + job workers TODOS aqui via my_syscall)
- 0x9d5ffc / 0x9d6004  spin das threads "Filter1"
- start routines: 0x368988 (jobs), 0xb9ab18 (4×, pre-render, cand. gfxworker), 0xbbb9a0 (rede)

---

# ⚡ s9 (2026-06-23 madrugada) — RAM/SWAP ERA O GARGALO REAL (hipótese do NextOS CONFIRMADA)
**A causa dos stalls não-determinísticos E dos wedges do device era SWAP, não o handshake.**
- Device `.100` usava **swapfile no cartão SD** (`/storage/.cache/swapfile`, swappiness=100). Sob a
  pressão de RAM do elderand (832MB total; il2cpp 70MB anon + heap 96MB + 138MB addressables + GL), o
  kernel swapava agressivo pro SD → a main thread bloqueava em **estado D (uninterruptible) no
  `zcomp_strm_single_find`** (zram single-stream) → boot travava em pontos ALEATÓRIOS (Odin Serializer/
  shaders/handshake) → e o `exit_group` sem teardown travava o display Mali (fbdev) → device inacessível.
- **FIX (na sessão; re-aplicar a cada boot — classifier bloqueou persistir no custom_start.sh):**
  ```
  swapoff /storage/.cache/swapfile
  swapoff /dev/zram0; echo 1 >/sys/block/zram0/reset; sleep 1
  echo 4 >/sys/block/zram0/max_comp_streams   # MULTI-STREAM (mata o gargalo single-stream D-state)
  echo lz4 >/sys/block/zram0/comp_algorithm; echo 700M >/sys/block/zram0/disksize
  mkswap /dev/zram0; swapon -p 100 /dev/zram0; echo 60 >/proc/sys/vm/swappiness
  ```
  ⚠️ **NUNCA `swapoff` com swap CHEIO** (deadlock: precisa de RAM livre p/ swap-in → trava o device em
  D-state; só resolve com reboot). Fazer no boot/fresh (swap vazio). `reboot -f` via ssh limpa.
- **FIX no binário (sem regressão): `egl_shim_emergency_teardown()`** (egl_shim.c) = eglMakeCurrent NULL
  + eglTerminate + SDL_QuitSubSystem(VIDEO); o watchdog `ELD_MAXSECONDS` (default 90s, thread própria,
  exit_group direto) chama ISSO antes de morrer → **não trava mais o fbdev/Mali no exit** (lição LCS/Bully).
- **RESULTADO: com zram multi-stream o boot vai MUITO mais longe e CHEGA no Choreographer** (resolução
  "1280:720" setada, GL ES2 ctx criado, render frame 0, [HANDSHAKE] capturado). Device NÃO trava mais.
- ✅ [HANDSHAKE] capturado: `main sync obj=0x7f20e9b7a0 vtable=lu+0x134b2d0 vt[4]=lu+0x45236c` — MAS
  vt[4] (0x45236c) é um **try-pop**, NÃO o produtor (0x452498 não está nessa vtable). Driblar via vtable
  não é trivial. O objeto da fila do handshake usa vtable lu+0x134b2d0.
- ⚠️ **ainda há não-determinismo no boot** (mesmo com zram): às vezes trava em GL-setup (1002 linhas,
  `[EGL] eglChooseConfig EGL_BAD_ATTRIBUTE` = Unity pede config ES3 no driver ES2→rejeita, fallback),
  às vezes no handshake (1342). = corrida de threads (job/sem/GC) ALÉM da RAM.
- 🔴 muro de shader CONFIRMADO no log: `Desired shader compiler platform 5` (=GLES20) não no blob URP.

## 📊 s9-cont — MEDIÇÃO de RAM + mapeamento COMPLETO do handshake
- **RAM**: elderand RSS=**377MB** + swap **144MB** = ~520MB working set. Cabe em 832MB (emustation
  parado→657MB livre) MAS APERTADO → swapa ~144MB → fricção D-state. **Trimar a pegada ~150MB =
  zero swap = boot determinístico.** Alvos: il2cpp 70MB anon (file-backed?), heap HEAP_MB=96 (libunity
  só 20MB — provavelmente folgado, testar reduzir), addressables.
- **Boot AINDA não-determinístico mesmo com zram**: às vezes chega no handshake (~12s, 1342 linhas),
  às vezes trava em `[render 0]` (~1004-1031 linhas). = CORRIDA no setup do GfxDevice/gfxworker (job/sem),
  ALÉM da RAM. (Reduzir swap deve estabilizar.)
- **Handshake MAPEADO (gdb stack scan, device estável)**: main E UnityGfxDeviceWorker AMBOS em
  `pthread_cond_wait`→`pthread_cond_wait_fake`→**lu+0x45232c** (consumer 0x4521c8 ← **lu+0x34ce34**
  = callback `blr x21` do scope "gfxdeviceworker-affinity"). O **gfxworker está no LOOP DE COMANDOS do
  GfxDeviceWorker** (lu+0x4385c0/0x438a2c) esperando comandos de render da main. A main espera o doFrame
  alimentar a fila dela. **Mutual-wait**: main espera doFrame→aí submete comandos; gfxworker espera os
  comandos. doFrame (nosso JNI) não alimenta a fila nativa da main → trava.
- ❌ Testados (sem destravar, agora SEM confound de RAM): sem CUP_1CORE (trava ANTES, em render0 —
  mais contenção), TER_CHOREO_MSG (re-arma looper/frame — não alimenta). ⚠️ CUP_1CORE é NECESSÁRIO.
- ⚠️ polling agressivo (CUP_CONDPOLL/TER_FUTEXPOLL/CUP_SEMPOLL) **SATURA a CPU fraca → reboot**. NÃO usar.
- ⚠️ **Observação (s10+, kernel 3.14):** fluxo de zram foi descartado por causar travas.
  Revalidar swapfile ativo após reboot e evitar comandos em zram para não reintroduzir wedge.

## 🔬 s9-fim — FEED do frame achado na bancada + INSTABILIDADE do device é o gargalo
- **Bancada (app real, steady-state attach)**: o **thread UnityChoreographer (mesmo que nosso
  choreo_driver_thread fake!)** dirige o feed da fila do frame chamando **lu+0xbff018** per-frame →
  lu+0x45241c → lu+0x452604 (broadcast lu+0x45265c). Cadeia: `[Looper] → lu+0xbff018 → lu+0x45241c →
  lu+0x452604`. ⚠️ **0xbff018 é dispatch GENÉRICO** (peguei x0 com nomes de uniform de shader
  "_LightTextureB0_ST" — é message-passing C++ templated, polimórfico) → NÃO dá p/ chamar cego.
- ✅ INFRA: `TER_FEEDLOG=1` (pthread_fake.c) loga se o feed/produtor NATIVO (broadcast de lu+0x45265c/
  0x452520/região 0x4524xx-0x4527xx/0xbff0xx) roda no NOSSO loader → responde "nosso doFrame dispara o
  feed?". (não consegui rodar ainda: boot travou antes do handshake nas tentativas.)
- 🔴🔴 **DEVICE CRASH-REBOOTA durante runs** (OOM sob pressão de RAM — agravado pelo MEU log pesado em
  /dev/shm que é RAM!). Cada reboot pode mudar as condições de swap (sem zram no fluxo atual) → próximo run pior.
  **Isso TRAVA a iteração.** Conclusão: **reduzir a pegada de RAM não é opcional, é PRÉ-REQUISITO** pra
  conseguir debugar o handshake com estabilidade. (No jogo REAL do NextOS, sem o log pesado, a pressão
  é menor — mas ainda aperta.)

## 🟢 PRÓXIMO (s10) — CRÍTICO: RAM primeiro (senão não dá p/ iterar)
0. **Reduzir pegada de RAM** (PRÉ-REQUISITO p/ estabilidade): so_loader carrega libunity(20MB)+
   libil2cpp(70MB) como mmap ANÔNIMO → não-descartável → swap → OOM-reboot. **Carregar TEXT
   file-backed** (mmap do .so, RO, reclaimable) no so_util.c. Reduz RSS + mata o swap + estabiliza.
   Também: HEAP_MB 96→testar menor; logar MENOS em /dev/shm nos testes.
1. Com device estável: `TER_FEEDLOG=1` → ver se nosso doFrame dispara o feed nativo. Se NÃO → o
   invoke do FrameCallback não chega no handler; achar por quê. Se SIM → feed roda mas fila errada.
2. Handshake (imagem): replicar o que o UnityChoreographer faz (lu+0xbff018 dispatch) — precisa RE do
   message-type, OU patch gfx single-threaded (sem gfxworker).
3. Shaders ES2/ES3 (pixels).

## 🟢 PRÓXIMO (s10-prev) — em ordem
1. **REDUZIR pegada de RAM** (NextOS: jogo é leve, deve caber) → mata o swap → boot determinístico.
   - testar HEAP_MB 96→48; investigar carregar il2cpp/unity file-backed (reclaimable) no so_loader.
2. **Handshake** (o muro p/ imagem): a main precisa que o doFrame alimente a fila nativa dela. O
   handler nativo do doFrame não roda via nosso invoke. Achar via bench (frida no app rodando — attach
   funciona, spawn não) COMO o doFrame real alimenta a fila per-frame (hook 0x452604/0x45265c em
   steady-state + backtrace) e replicar. OU patch p/ gfx single-threaded (sem gfxworker, sem handshake).
3. **Shaders** ES2-vs-ES3x (muro final p/ pixels) — shim ES3→ES2.

## 🟢 PRÓXIMO (s10-orig) — device ESTÁVEL agora, dá p/ iterar à vontade
1. **Re-testar os flags que destravam cond/sem AGORA** (antes os testes eram inválidos: o boot
   travava na RAM antes de chegar no handshake). Candidatos: CUP_CONDPOLL, TER_FUTEXPOLL, TER_CHOREO_MSG,
   CUP_SEMPOLL. Usar `/tmp/eld_flag.sh` (EXTRA=... ; espera handshake; mede progresso + fb0).
2. **Atacar a não-determinismo de boot** (corrida job/sem) — pode ser que estabilizar isso já deixe
   o boot completar. Ver [SEM]/job lost-wakeups (handoff antigo: CUP_SEMPOLL/TER_FUTEXPOLL).
3. **Handshake**: doFrame JNI não alimenta a fila nativa. Achar o handler nativo do doFrame.
4. **Shaders** (muro final): ES3→ES2 shim OU X5M (ES3 real + mais RAM = mata RAM+shader de uma vez).
---

# HANDOFF Elderand s8 (2026-06-23 noite) — TRAVA DO FRAME ISOLADA: handshake de STARTUP do gfxdeviceworker

## TL;DR
A trava pós-Choreographer está **PRECISAMENTE identificada**: é o **handshake ÚNICO de startup do
gfxdeviceworker/Choreographer** (não é per-frame, não é lost-wakeup). Boot vai 100% até o render loop,
roda lógica C#, mas main + UnityGfxDeviceWorker travam num pthread_cond_wait esperando uma fila que
nunca é alimentada porque nosso doFrame (JNI) não dispara o produtor NATIVO. **2º muro confirmado:
shaders (platform 5 = GLES20) não existem no blob URP (só GLES3x) → Mali-450 é ES2 → precisa shim
ES3→ES2 estilo Mina.** Flags de boot inalteradas. Build OK (md5 814ee776).

## FLAGS DE BOOT (as que vão mais longe): `ELD_DEVLIB=1 ELD_NOFATAL_OFF=1 CUP_NOSIGH=1 TER_CHOREO=1 ELD_GCOFF=1`

## 🎯 ROOT CAUSE DA TRAVA (provado com gdb no device + frida no bench)
- **gdb no device (.100)**: main (UnityMain) E UnityGfxDeviceWorker AMBOS parados em
  `pthread_cond_wait` → nosso `pthread_cond_wait_fake` → **libunity+0x45232c** (RA logo após o
  cond_wait). Backtrace trunca aí (libunity é mmap anônimo do so_loader → sem unwind no gdb).
- A função do wait é **libunity+0x4521e8** (loop em 0x452310): faz queue-pop bloqueante —
  `while(!(obj[0x58] && *obj[0x58])) pthread_cond_wait(cond=obj+0x88, mutex=obj+0x60)`.
- **IDENTIDADE (string do profiler scope)**: `UnityChoreographer.platform-android-gfxdeviceworker-affinity`
  (em libunity+0x1010e87, carregada pelo wrapper que chama 0x4521e8). → é o **handshake
  Choreographer ↔ gfxdeviceworker (afinidade de CPU/ready)**.
- ⚠️ **objdump mente nos nomes de PLT** dessa lib (resíduo de lazy-PLT do pairip): `bl 0xc03e0
  <rmdir@plt>` é na verdade **pthread_cond_wait** em runtime (gdb confirma); `bl 0xc0bc0
  <strerror@plt>` = **pthread_cond_broadcast**; `bl 0xc0cf0` = signal. NÃO confiar nos labels @plt.

## 🔬 BENCH (Moto G100) — produtor da fila achado
Trace frida no app REAL (mesma libunity md5 8fe7856d, MESMOS offsets):
- **Produtor (alimenta a fila do handshake) = libunity+0x452498** — state-machine que tranca o
  mutex (obj+0x60), enfileira em obj+0x58 e **broadcasta a cond (obj+0x88) em libunity+0x452520**.
  É chamado como **método VIRTUAL (vtable[4], offset +0x20)** do objeto, de dentro do driver de gfx
  em **libunity+0xbeeb98 (blr x8)** e libunity+0xbff038.
- 🔑 **0x452498 é STARTUP-ONLY**: em 6s de steady-state (attach) = **0 chamadas**. O consumidor
  0x4521e8 também = 0 chamadas em steady. Logo o handshake é **uma vez só, no boot**.
- **Feed per-frame (depois do startup) = libunity+0x45265c** (~90/s em steady) — fila já fica cheia,
  então quase não sinaliza. O outro alto-frequência é libunity+0xf44a50 (~270/s, job/áudio).
- **Conclusão**: na bancada o handshake de startup COMPLETA (produtor 0x452498 roda) → main destrava
  → entra no frame loop. No nosso so-loader o produtor NUNCA roda → deadlock. O produtor é disparado
  pelo processamento do 1º doFrame (vsync real do Android na bancada). **Nosso doFrame (JNI invoke)
  não chega no produtor nativo.**

## ❌ DESCARTADO nesta sessão (sem regressão, tudo gated)
- `CUP_1CORE=1` (sysconf→1 core): **NÃO desliga MT rendering** — o UnityGfxDeviceWorker é criado
  mesmo assim. Trava igual.
- `TER_FUTEXPOLL=50`: não destrava. (E não PODE: glibc pthread_cond_wait faz loop interno no futex;
  timeout cru só re-espera, não retorna pro Unity.)
- `CUP_CONDPOLL=16` (timedwait→wakeup espúrio pro Unity): não destrava → **NÃO é lost-wakeup**, o
  predicado (obj[0x58]) genuinamente nunca fica != 0 (produtor nunca roda).
- `TER_CHOREO_MSG=1` (NOVO: choreo_driver_thread re-dirige handleMessage a cada frame, além do
  doFrame): não destrava. O caminho do Handler/Looper também não alimenta a fila da main.

## 🔄 REFINAMENTO s8-cont (noite) — é MUTUAL-WAIT, não "produtor não chamado"
- Testei spawn frida na bancada várias vezes (com hooks armados ANTES do boot via dlopen-gate):
  **o produtor 0x452498 NÃO dispara de forma confiável** (1ª vez foi fluke). Logo "dirigir o produtor"
  é furada — não é o mecanismo real.
- **VERDADE**: é um **mutual-wait** do handshake gfxdeviceworker. main espera o gfxworker enfileirar
  "ready" (na fila da main, obj+0x58); o gfxworker espera a main enfileirar "init" (na fila DELE).
  Na bancada resolve (main enfileira init→worker seta afinidade→enfileira ready→main segue). No nosso
  loader **a enfileiração entre os dois se PERDE** → ambos travam em 0x452310. O consumidor 0x4521e8
  na bancada provavelmente nem bloqueia (item já presente); no nosso bloqueia (fila vazia).
- per-frame feed 0x452604 usa fila GLOBAL (0x140f8c8), DIFERENTE da fila por-objeto do handshake →
  não serve p/ destravar o startup.
- ⚠️ boot é NÃO-DETERMINÍSTICO: às vezes trava em Odin Serializer (antes), às vezes no handshake.
- ✅ INFRA s8-cont (no binário, gated): **captura do objeto do handshake** em pthread_cond_wait_fake
  (RA==libunity+0x45232c → obj=cslot-0x88 → `g_choreo_syncobj`/`g_choreo_syncobj_gfx`; log [HANDSHAKE]
  com vtable+vt[4]). `TER_HSLOG=1` loga offsets distintos de toda cond_wait da libunity. `ELD_HSOFF=hex`
  ajusta o offset do handshake se mudar. **Confirmar no DEVICE: o offset 0x45232c (calculei via gdb
  contra a "load base"; g_unity_base=text_base — bate, mas confirmar com TER_HSLOG).**
- 🔑 **SAFEGUARD novo (RESOLVE o wedge do device)**: watchdog interno `ELD_MAXSECONDS` (default 90s,
  =0 desliga) numa thread própria que chama `exit_group` DIRETO — não depende de ssh/my_exit. Um boot
  travado num spin não satura mais o sshd (foi o que travou o .100: elderand antigo sem auto-kill +
  ssh-kill do watchdog externo falhou). main.c topo + eld_hard_watchdog.

## 🎯 PLANO s9 (com device .100 de volta) — DIRETO, sem depender da bancada
1. Rodar com TER_HSLOG=1 → confirmar offset do handshake + ver os 2 objetos ([HANDSHAKE] main/gfx).
2. Instrumentar NA NOSSA libunity (hook/trampolim em 0x4521e8 entrada + no enqueue) p/ ver:
   - a main chega a enfileirar "init" pro gfxworker? (se sim, por que o worker não vê?)
   - o gfxworker roda seu corpo (seta afinidade) ou trava antes?
   Capturar os 2 objetos + comparar o grafo wait/signal entre eles.
3. Com os 2 objetos conhecidos, forçar a enfileiração que falta (chamar o enqueue/producer com o
   objeto+comando certos OU religar o sinal perdido). Alternativa: binary-patch gfx single-threaded.

## 🟢 PRÓXIMO (s9) — em ordem de aposta p/ destravar o frame
1. **Disparar o produtor nativo direto** (pular o JNI invoke frágil): no nosso loader, capturar o
   objeto do handshake (hookar/instrumentar a entrada de libunity+0x4521e8 — está na NOSSA libunity
   carregada, dá p/ patch/log) → ler vtable[4] (=0x452498) → chamá-lo com o comando certo. ⚠️ o arg
   x1 do produtor é um PONTEIRO p/ struct de comando (não enum) — reversar o comando (na bancada
   0x452498 recebeu x1=ponteiro heap; dumpar os 1ºs 32 bytes na bancada com attach+force-recreate).
2. **Achar o handler nativo do doFrame** (o que o jnibridge `invoke` despacha): na BANCADA spawn é
   bloqueado pelo pairip (anti-frida do DEX) — **attach no app RODANDO funciona, spawn NÃO**. Catch
   do startup é flaky (1 run pegou via FUZZY: produtor ← lu+0xbeeb9c/lu+0xbff03c). Tentar: attach +
   forçar recreate do GfxDevice (resize/rotação real, não só HOME — HOME não recria). Hookar o
   `invoke` nativo (achar via RegisterNatives) e ver o que doFrame chama natively → replicar.
3. **Binary-patch p/ gfx SINGLE-THREADED** (mata o handshake inteiro): achar onde libunity decide
   criar o UnityGfxDeviceWorker (pthread_create do worker) e forçar caminho single-threaded (main faz
   GL inline). Remove o handshake. Mais invasivo mas elimina a classe de bug.
4. ⚠️ Mesmo destravando o frame, vem o **2º MURO (shaders)** — ver abaixo.

## 🔴 2º MURO (render) — shaders GLES20 não existem no blob (URP é ES3x); Mali-450 é ES2
- Log repete `Desired shader compiler platform 5 is not available in shader blob` (25+×).
  **platform 5 = GLES20** no enum ShaderCompilerPlatform do Unity. Nosso egl_shim cai p/ **ES2**
  no Mali-450 (HW é ES2-only) → Unity escolhe variantes GLES20 → **o blob só tem GLES3x** → shaders
  falham → tela preta mesmo com frame loop ok.
- **Caminho (regra do NextOS: Unity 2D shader sempre conversível)**: reportar **ES3** (p/ Unity
  carregar as variantes GLES3x que EXISTEM no blob) + camada **GLES3→GLES2** estilo Mina/Dusklight
  (rewriter de GLSL ES3.x→1.00 + emulação de UBO + fix depth-stencil FBO). É esforço próprio,
  multi-sessão. Refs: `~/mina-build/libminashim.c`, projeto dusklight (aurora mali450-gles2).
- 💡 **ALTERNATIVA FORTE p/ NextOS decidir**: rodar no **X5M (S905X5M, Valhall, ES3 REAL)** —
  elimina o muro de shader por completo (variantes GLES3x rodam nativas) e provavelmente muda o
  comportamento de threading. Mali-450 .100 exige o shim ES3→ES2. (Não troquei de device sozinho.)

## 🧰 BANCADA DE CAPTURA (ver BANCADA_CAPTURA.md) — caveats novos s8
- frida **attach no app rodando = OK**; **spawn = bloqueado pelo pairip do DEX** (hooks nem armam;
  libunity nem aparece no dlopen hook). Startup só dá p/ pegar com sorte (FUZZY backtrace).
- app aparece como nome "Elderand" (não com.pid.elderand) no frida-ps → attach por PID:
  `PID=$(frida-ps -U|awk '/Elderand/{print $1}')`.
- driver py: `dev.attach(PID)` + script v8. Scripts em /tmp: choreo_trace/prod_trace/steady/perframe.

## INFRA / COMANDOS
- `bash run_test.sh [SECS] "ENV..."` (build+kill+deploy+run+watchdog+screenshot). Log device em
  `logs/_eld_device.log`; screenshot `logs/frame_*.png` (Read tool); preto="uniforme (0,0)".
- Probe de estado de thread no device (syscall+wchan no hang): /tmp/eld_probe.sh (scp+ssh .100).
  gdb bt com offsets: /tmp/eld_bt2.sh (pega libunity base do log e calcula offset).
- objdump: `~/NextOS-Elite-Edition/build.*Amlogic-old.aarch64-4/toolchain/bin/aarch64-libreelec-linux-gnu-objdump`
- device alvo: 192.168.31.100 (emuelec, fb0 1280x1440). ssh sshpass -p archr root@192.168.31.100.
- libs reais: `device_libs_1.3.22/` (libunity md5 8fe7856d). Port: `~/nextos_ports_android/ports/elderand`.

## OFFSETS-CHAVE (libunity 1.3.22 md5 8fe7856d) — confirmados
- 0x4521e8  consumidor/wait do handshake (loop 0x452310; cond=obj+0x88, mtx=obj+0x60, pred obj[0x58])
- 0x452498  PRODUTOR (startup-only) — vtable[4]/+0x20; broadcast em 0x452520
- 0x452604  feed per-frame (broadcast em 0x45265c)
- 0xbeeb98 / 0xbff038  blr x8 que chamam o produtor virtual (driver de gfx)
- 0x1010e87 string "UnityChoreographer.platform-android-gfxdeviceworker-affinity"
- domain_get=0xcdcbd4  thread_attach=0xcdd028 (já usados pelo choreo_driver_thread)

## FIXES s8 no código (sem regressão, gated)
- `TER_CHOREO_MSG=1`: choreo_driver_thread também chama jni_handlemessage a cada frame (testado, não
  resolve — manter gated p/ experimentos). main.c ~4847.
- BANCADA_CAPTURA.md criado (setup+receita da bancada Moto G100).

## REGRAS (lembrar)
- Trabalhar SOZINHO, loops build→run→fix, só falar com imagem ou login. master only, sem co-autor Agente.
- Matar elderand por /proc/*/exe + confirmar 0 antes de scp/run (run_test.sh já faz).
