# Pixel Cup Soccer — HANDOFF (so-loader Mali-450, Unity 2022.3.62 IL2CPP)

> Sessão 2026-06-23. Diagnóstico EXAUSTIVO do muro. Loading screen renderiza; jogo não passa dela.
> Device dev = Mali-450 **<device-ip>** (senha <senha>). Dados em /storage/roms/ports/pixelcup.

## ONDE ESTÁ (1 frase)
A tela de LOADING (o malabarista) renderiza e CONGELA: o `nativeRender` do 1º frame bloqueia em
`WaitForJobGroup` esperando um **job de GFX** que nunca completa porque a render thread do Unity
(`UnityGfxDeviceWorker`) ou (a) deadlocka no driver Mali Utgard fazendo `glClear` concorrente com a
present thread, ou (b) fica ociosa pq o comando de render nunca é sinalizado pra ela.

## DIAGNÓSTICO DEFINITIVO (via gdb no device — gdb/gdbserver/strace existem em /usr/bin)
Truque-chave p/ backtrace de SO carregado manualmente (sem CFI normal):
`add-symbol-file libunity.so -o <load_base>` (base vem do log `so_load: load base =`). Aí o `.eh_frame`
dá unwinding correto. Símbolos = `??` (stripped) mas offsets batem: `addr - load_base`.

Offsets confirmados na libunity (Unity 2022.3.62):
- `0x659e20` = **nativeRender** (wrapper com setjmp; chama 0x6423ec)
- `0x698660` = **WaitForJobGroup**
- `0x4008ac` = **Baselib Semaphore::Acquire** (futex; count em `[x19]`, espera count>=1)
- `0xbac960` = **GfxDeviceWorker run loop** (start via thread-wrapper 0x571328)

Estado parkado (todas as threads em futex):
- **Thread 1 (main/UnityMain)**: nativeRender(0x659e78) → WaitForJobGroup(0x698660) → Semaphore::Acquire(0x4008ac) → futex_wait. **count=0** (nunca sinalizado).
- **GfxDeviceWorker (thread 4)**: run loop(0xbac960) → Semaphore::Acquire(0x4008ac) → futex_wait, OCIOSA esperando comando de render. (Com vsync ON: trava em `glClear`→`_mali_frame_builder_write_lock` segurado pela present thread no swap.)
- **16 "Background Job." workers**: NUNCA rodaram nada (futex word baseline -8388607=0xFF800001).
- **3 "Job.Worker"**: rodaram ~6500 jobs do boot (count moveu) — dispatch FOREGROUND funciona.
- **Loading.Preload / Loading.AsyncRead**: ociosas, count=0 (nunca recebem trabalho).
- `datapack.unity3d` (conteúdo do jogo) **nunca é nem aberto** (fd ausente). data.unity3d (loading scene) abre normal.

## RAIZ (dois muros que se compõem)
1. **MT-rendering / native-gfx-jobs do Unity 2022 + Mali Utgard**: a render thread faz GL concorrente
   com a present thread interna do Mali → deadlock no frame-builder lock. Mali-450 Utgard é
   single-context p/ window. (Ports que funcionam — RE4, Crazy Taxi — usam GL single-thread.)
2. **Sinal produtor→consumidor não entregue**: main agenda job de gfx mas o semáforo do
   GfxDeviceWorker (e o de conclusão do main) fica count=0. NÃO é lost-wakeup (FUTEXPOLL não destrava;
   count fica 0 = produtor nunca postou). Mesma classe do job-system.

## O FIX CERTO (e por que está bloqueado)
**Forçar GL single-thread** (`-force-gfx-direct`) resolveria os dois — é o que o Terraria/RE4 fizeram.
MAS:
- ⛔ **Unity 2022.3 NÃO lê /proc/cmdline** (nem self nem <pid>): o log `[CMDLINE]` nunca dispara, e
  passar via argv REAL `./pixelcup -force-gfx-direct ...` TAMBÉM é ignorado (16 workers + GfxDeviceWorker
  nascem do mesmo jeito). O `cmdline_fd()`/injeção do scaffold Terraria é morta aqui.
  (Terraria funcionou pq Unity mais antigo LÊ /proc/cmdline.)
- ⛔ **boot.config `gfx-disable-mt-rendering=1` é INEFETIVO** (já está no boot.config e o GfxDeviceWorker
  continua fazendo GL) — o player-setting "Multithreaded Rendering" baked em globalgamemanagers sobrepõe.
- ⛔ **MALI_SINGLEBUFFER=1 quebra**: eglCreateWindowSurface retorna NULL → "Unable to initialize Gfx API".

## PRÓXIMOS PASSOS (ordem de prioridade p/ a próxima sessão)
1. **Patchar o player-setting MT-rendering em globalgamemanagers** (dentro de data.unity3d): achar o
   bool `m_MTRendering`/`m_RenderThread` no PlayerSettings serializado e flipar p/ 0. data.unity3d é
   bundle comprimido (LZ4) — precisa descomprimir/editar/recomprimir, OU servir um globalgamemanagers
   editado via asset_redirect.
2. **Patchar a libunity**: a decisão "threaded gfx" (GfxDeviceClient vs GfxDevice direto) é um bool lido
   no init do GfxDevice. O start-routine do worker (0xbac960) é passado INDIRETO (não adrp — via tabela/
   struct), então rastrear a criação exige seguir o ponteiro em runtime (gdb: bp em pthread_create,
   ver quem passa 0xbac960). Achar o `if(threaded)` e forçar o ramo direto.
3. **Achar a fonte REAL dos args do Unity Android** (Java intent / método JNI) e injetar
   `-force-gfx-direct -job-worker-count 0`. Há 2× `GetStringUTFChars -> ""` no boot (candidatos a command line).
4. **Replicar o egl_shim do RE4** (render thread = dona da window + blit de FBO offscreen; workers = pbuffer).
   Complexo mas PROVADO p/ Unity MT no Mali. Ver re4/src/egl_shim.c + main_re4.c (linha ~1794 BLIT na render thread).

## MUDANÇAS DE CÓDIGO FEITAS NESTA SESSÃO (src/main.c)
- **cmdline via fopen** (my_fopen): Unity lê /proc/<pid>/cmdline via fopen, não open. Adicionado handling
  (cmdline_fd). [Correto, mas Unity 2022 não lê cmdline de jeito nenhum — inócuo aqui.]
- **vsync forçado 0** (mux_SwapInterval, default ON; CUP_NOVSYNCPATCH desliga): a present thread não segura
  o frame-builder lock esperando vsync. Reverte o GfxDeviceWorker do deadlock-em-glClear p/ ocioso-limpo
  (estado melhor p/ depurar; NÃO resolve o muro). CUP_VSYNC=N força outro intervalo.
- **EGLMUX (opt-in CUP_EGLMUX)**: multiplexa eglMakeCurrent (1 thread dona da window real, resto pbuffer)
  + eglSwapBuffers (só a dona). COM ele os frames AVANÇAM (melhor sinal!) mas a tela fica PRETA porque
  native-gfx-jobs divide o GL de 1 frame entre threads (clear numa, draw noutra) e pbuffer-ar as não-donas
  deixa o frame incompleto. Confirma que o fix real é desabilitar native-gfx-jobs/MT, não multiplexar.

## FERRAMENTAS DESTA SESSÃO (reusar)
- `iter.sh [segs]` — build+deploy+run+captura threads/fds/screenshot. Passa env PC_/TER_/CUP_/SDL_/MALI_.
- `gdbpeek.sh`, `gdbpeek2.sh`, `gdb10.sh` (no device /tmp) — backtraces com símbolos.
- Screenshot: `cat /dev/fb0` (1280x720x4 BGRA, pegar primeiros 1280*720*4 bytes).
- Matar: por /proc/*/exe (regra do porter). ⚠️ scp às vezes segfalta local (sshpass) — repetir.

## REGRAS (memória)
- Git master-only, ZERO co-autor. Matar+confirmar 0 instâncias antes de lançar.
- Default run (sem CUP_EGLMUX) = loading screen congelada = checkpoint conhecido (não-quebrado).

---
## ATUALIZAÇÃO 2026-06-23 (sessão cont.) — CAMADA GFX RESOLVIDA, async load é a parede
✅ **FIX APLICADO: `m_MTRendering=False` no globalgamemanagers** (via UnityPy, dentro do data.unity3d).
   - data.unity3d repackado (LZ4), deployado no device (size 25734566). Backup `.orig`.
   - Script: `~/.upy-venv/bin/python` (UnityPy 1.25); env `UnityPy.load(...)`, achar PlayerSettings,
     `tree["m_MTRendering"]=False`, `obj.save_typetree(tree)`, `env.file.save(packer="lz4")`.
   - RESULTADO: o deadlock GL do Mali (GfxDeviceWorker em glClear) NÃO mais bloqueia. O main
     **AVANÇOU**: saiu do gfx-WaitForJobGroup e agora para no **PreloadManager** (libunity+0x876b24,
     spin em 0x54a000 esperando async op). A loading sprite chega a render alguns frames (md5 varia
     entre runs) e congela no async load.
⚠️ ainda há thread "UnityGfxDeviceW" (criada mas o GL roda na main agora; sem o deadlock).

🔴 **PAREDE ATUAL = async load do datapack não dispatcha** (= muro documentado da sessão anterior, agora
   SEM o gfx por cima). Estado (gdb, MT off):
   - main: nativeRender(0x659e78)→0x642720→0x4d84d0/4d8224/4d81e4→0x4e041c→**PreloadManager 0x876b24**→0x86a1c4→0x3ac290→spin 0x54a000.
   - Loading.Preload + Loading.AsyncRead: OCIOSAS em Semaphore::Acquire(0x4008ac), count=0 (nunca sinalizadas).
   - datapack.unity3d NUNCA aberto.
   - TER_FUTEXPOLL=2 + CUP_PRELOAD_TICK NÃO destravam → comando de leitura **nunca é enfileirado**
     (lado PRODUTOR; confirma "fila vazia" da sessão anterior). NÃO é lost-wakeup.

🎯 **PRÓXIMO (foco único agora que gfx está resolvido):** consertar o dispatch do async read:
   (a) registrar a thread produtora (main) no job/IO-system p/ o enqueue ir pra fila drenada (workers
       registram no startup; achar RegisterExternalThread/InitializeJobThread); OU
   (b) forçar load SÍNCRONO: patchar AssetBundle.LoadFromFileAsync→LoadFromFile, ou interceptar o
       AsyncReadManager p/ ler inline na main; OU
   (c) achar+consertar o enqueue real (a deque órfã). Régua: ports/terraria/HANDOFF.md (mesmo muro).
   Faking (PC_SKIPJOBWAIT) descartado: NullReferenceException (datapack não carregado = dados null).

---
## WRAP-UP 2026-06-23 (fim da sessão cont.) — descobertas SÓLIDAS + correções
🟢 **MAIOR GANHO: render loop RODA, engine VIVA (NÃO deadlockada).** Com `m_MTRendering=False`
   (globalgamemanagers patchado), rodando com `CUP_FRAMES=999999999`: a render loop conta
   `[render 180]→600→1080→1500→1920...` ~30fps continuamente, e SAI LIMPO (rc=0) ao bater o limite
   de frames. **Antes eu interpretei "congelado/morreu" errado:** era só o CUP_FRAMES default (600)
   terminando a loop (~20s) → exit normal. NÃO há crash nem deadlock do main.
🟢 NullRefs no boot (linha ~460 Rewired DS4 + linha ~492 standalone) são **CAPTURADOS/não-fatais**
   (engine segue 1920 frames depois). Red herring p/ o load (como a sessão anterior já dizia).
🔴 **datapack.unity3d nunca é aberto** (fd ausente em todos os polls 12-60s). Tela do loading fica
   ESTÁTICA (md5 5703378b constante) apesar de 1920 frames — pode ser logo estático (não bug) OU
   o load não progride.
⚠️ **strace FALHOU** (arquivo 0 linhas: passei `ENV=v ENV=v` DEPOIS de `timeout` → strace tentou
   exec a env-var como programa). Então "datapack nunca tentado" NÃO está confirmado. P/ refazer:
   `strace -f -e trace=openat -o /tmp/s.txt env PC_INLINETASK=1 ... ./pixelcup` (env ANTES, ou
   exportar antes). REFAZER no próximo: confirmar se datapack/AssetBundle.LoadFromFileAsync é chamado.

🎯 **NOVO ENTENDIMENTO p/ próxima sessão:** o main NÃO está preso (loop roda). Então o bloqueio do
   datapack é (a) o C# de loading não CHEGA a pedir o load (bootstrap travado/condicional), OU
   (b) pede mas o async não progride. **Decidir com:** 1) strace openat CORRETO (datapack tentado?);
   2) hookar `il2cpp_raise_exception` (trampolim, achar método que lança o NullRef da linha 492 — usar
   pc_resolve_il2cpp_addr já existente); 3) checar se C# Update roda (a sprite anima? logo estático?);
   4) achar o controller da loading scene no il2cpp e ver o que dispara o datapack. Investigar tb o
   flood de `[SEM] post 0x...` (3 tids em loop apertado — spin de worker? ou wait-loop do load?).
   ⚠️ Rodar SEMPRE com CUP_FRAMES=999999999 (senão sai em 600 frames e parece "travar").

---
## SESSÃO 2026-06-23 (cont. 2) — CADEIA C# MAPEADA + MURO = JOB-SYSTEM (RE4-class), CONFIRMADO
Tooling novo: **Il2CppDumper roda via dotnet** (`DOTNET_ROOT=~/.dotnet DOTNET_ROLL_FORWARD=LatestMajor
~/.dotnet/dotnet ~/cuphead-build/dump/Il2CppDumper/Il2CppDumper.dll <libil2cpp.so> <global-metadata.dat>
/tmp/pcdump`). dump.cs (19MB) = todas as classes+RVAs. Metadata v31. RVA do dumper = `g_il2cpp_base+RVA`
(bate com os patches existentes, ex DiskSpace 0xd158ac).

🎯 **BLOCO C# IDENTIFICADO:** a tela do malabarista = `CPCSLoadingState : CScreenStageBase` (TypeDefIndex
739). `update()` (RVA 0x1654DE4) faz timer 0.2s → chama `resourceRequestsEnded()` (RVA 0x16550A0) → se
true, transiciona. `resourceRequestsEnded` itera `mResourceRequests` (List<ResourceRequest> @ this+0x248)
e exige, p/ cada um, `isDone` (icall 0x29cb7c4) **E** AudioClip `loadState==2`(Loaded) (icall 0x299da18).
`startLoadingAudioClips()` (RVA 0x16552AC) dispara os `Resources.LoadAsync` de áudio. **Esses async loads
NUNCA completam** → loading congela.

🔧 **CUP_SKIPRESWAIT** (main.c, gated): patcha `resourceRequestsEnded` (il2cpp+0x16550A0) → `movz w0,#1;
ret`. Instala o hook eglSwap (cond no patch_got da eglSwapBuffers inclui CUP_SKIPRESWAIT). Resultado: a
tela TRANSICIONA — MAS a main **continua em WaitForJobGroup** (carregando o próximo conteúdo async) →
tela preta, datapack ainda não abre. **Prova que o muro é universal (todo load async), não só a loading.**

🧱 **MURO RAIZ = JOB-SYSTEM ASYNC DISPATCH (idêntico Terraria/RE4):** gdb (add-symbol-file libunity.so -o
<base do log 'so_load: load base'>; libs são so_load=mmap anon, NÃO aparecem em /proc/maps por nome —
pegar base do LOG). Backtrace main: nativeRender(U+0x659e78)→0x642720→0x4d84d0/8224/81e4→0x4df9f4→
**WaitForJobGroup U+0x698660/0x698748**→Semaphore::Acquire U+0x4008ac→futex U+0xd4abd8. WaitForJobGroup
(func U+0x6986d8) ANTES de bloquear chama execute-jobs (blr [mgr_vtbl+1712] @0x698710) mas NÃO completa
o grupo → bloqueia em U+0xbacad8. Threads (todas PARKED em futex): Loading.AsyncRead, Loading.Preload,
Job.Worker 0-2, 16 Background Job. `resources.resource`(162MB áudio) e `datapack.unity3d`(401MB) **NUNCA
abertos** (strace openat). Só data.unity3d (sync, boot scene) abre → **SYNC funciona, ASYNC não despacha.**

🔬 **Enqueues PROVADOS corretos p/ AsyncRead/Preload** (auto-postam o próprio semáforo): AsyncRead enqueue
U+0x425508 = push ring + tail-call Semaphore::Release U+0x400a08 no sem (mgr+0xd4). Preload loop U+0x4ee330
processa(0x4ee568)+Acquire(0x400808). **Logo o problema NÃO é AsyncRead/Preload — é o JOB que alimentaria
eles que nunca roda.** A main agenda o job-group (target counter [jobgrp+268], jobgrp=[mgr+9496]) mas
worker nenhum o executa e o self-execute da main (WaitForJobGroup) não acha → **job em deque órfã** (main
= thread do NOSSO loader, índice de job-system possivelmente inválido; = diagnóstico Terraria).

📦 **DATAPACK = Play Asset Delivery** (`UnityDataAssetPack`): engine monta via func U+0x63f308 (caller
0x63fafc). Gate1 U+0x56cf5c = `[0x11c6358]!=0` (mgr de asset pack existe → PASSA). Gate2 U+0x417bfc
lookup "UnityDataAssetPack" no registro do CONTEXTO-DE-MOUNT (x19) → **-1** (não achou) → erro U+0x3f84c4,
não monta. Registro init em U+0x634f60 (registra UnityDataAssetPack+UnityStreamingAssetsPack via JNI
vtbl+1336) mas é OUTRO objeto (asset-pack-mgr) ≠ o do mount-context. jni_shim já responde
getAssetPackState→COMPLETED(4) e getAssetPackPath→/storage/.../pixelcup, mas getPath() COLIDE com
File.getPath (retorna .../userdata, ERRADO p/ asset pack). **MAS isso é secundário: mesmo montando o
datapack, o load das cenas dele é async → cai no mesmo muro do job-system.**

🚧 **VEREDITO:** o port está 100% gated pelo **muro do job-system async** (RE4 foi FECHADO por isso;
Terraria contornou FINGINDO mas só funcionou pq não precisava de async real — pixelcup PRECISA: áudio,
cenas, datapack). Fake (SKIPRESWAIT/SKIPJOBWAIT) → dados null/NullRef. **Único caminho p/ jogável = fazer
o job AGENDADO realmente EXECUTAR.** Próximo: achar a func SCHEDULE/enqueue do job-group (quem escreve
[jobgrp+268]) e por que o enqueue não chega na deque que o worker (e o self-execute da main) consulta;
OU registrar a main thread no job-system (achar o storage do índice de thread — NÃO é TLS+0x28=canary).
Ferramentas: /tmp/disasm.sh (il2cpp by file-off), /tmp/udis.sh (libunity by vaddr), /tmp/pcdump/dump.cs.

---
## SESSÃO 2026-06-23 (cont. 3) — ORPHAN-DEQUE CONFIRMADO; muro = thread-index da MAIN no job-system
🟢 **GANHOS DE MÉTODO (corrigem erros das sessões anteriores):**
- ⚠️ **DOIS logs distintos**: `printf`/so_load("load base") → **stdout** = `/tmp/pc_run.out`; `fprintf(stderr,...)` (FX, [r N>, KICK) → **stderr** = `/storage/roms/ports/pixelcup/debug.log` (reaberto em main(), unbuffered). Ler o ARQUIVO certo (sessões anteriores liam pc_run.out e achavam "congelado no choreographer" — FALSO).
- ✅ **render loop RODA** (avança a ~frame 2-3 e ESTACIONA dentro de um nativeRender). NÃO congela no frame 0.
- ⚠️ **libunity de pixelcup ≠ Cuphead** (2022.3.62f3, 18156936 bytes, md5 2e6ffdf2). **TODOS os offsets CUP_* de PreloadManager (0x8733a8/0x873a90/0x8739c4) e o singleton 0x12b9380 são STALE de Cuphead** → `[libunity+0x12b9380]`=**0** (lido via /proc/mem). NÃO confiar em offset libunity que não foi re-derivado p/ pixelcup. (il2cpp RVAs do dump SÃO de pixelcup, ok.)
- ⚠️ **gdb 17.2 no device é instável** neste alvo (34 threads, stripped, mmap-anon): só `thread apply all bt N` p/ FILE funciona; `printf`/`info registers`/`x`/`-ascending`/`set logging` saem VAZIOS. Evitar gdb p/ inspeção de memória — usar **/proc/PID/mem + python3** (existe no device).

🔬 **INSTRUMENTAÇÃO NOVA (commitada, default OFF):**
- `TER_FUTEXLOG=1`: loga WAIT/WAKE (tid,comm,uaddr) — JÁ existia, agora explorado.
- **`PC_KICKWORKERS=N`** (main.c): thread independente (`kick_worker_thread`, ~50Hz após PC_KICKWARM ciclos)
  que registra o uaddr de cada thread-worker (Job.Worker/Background Job./Loading.*/BatchDelete) quando ela
  faz FUTEX_WAIT (em `wsem_record`), e depois "kicka" cada um. Modos: **1**=`Semaphore::Release` engine
  (U+0x400a08) **2**=post+wake (`*ua++` + FUTEX_WAKE) **3**=só FUTEX_WAKE. Valida o uaddr SEM crashar via
  `FUTEX_WAKE count=0` (retorna -EFAULT se não-mapeado). `PC_KICKMS`/`PC_KICKWARM` ajustam.

🧱 **DIAGNÓSTICO DEFINITIVO (3 experimentos limpos):**
1. **Futex trace (TER_FUTEXLOG)**: os ~22 workers (Job.Worker/Background/Loading) cada um parkado no SEU
   semáforo (uaddrs em array stride 0x8180) que **NUNCA recebe WAKE**. A main cicla por vários waits que
   SÃO acordados (renderiza) mas seu wait final NUNCA é acordado (presa em WaitForJobGroup).
2. **PC_KICKWORKERS=3 (wake puro)**: acorda os 22 workers continuamente → **NADA** (frame fixo 3, datapack
   nunca abre, jogo vivo). Workers acordam, recheckam count, redormem = **não há job na fila global deles**.
3. **PC_KICKWORKERS=2 (post+wake)**: empurra os workers PASSANDO o Acquire → **NADA** (frame fixo 2). Eles
   entram no steal-loop e não acham job. **PC_KICKWORKERS=1 (engine Release) CRASHA** (uaddr≠base do objeto Release).
   ⟹ **O job que a main espera NÃO está em nenhuma fila que os workers consultam.** Está na **deque LOCAL
   órfã da main** (work-stealing), invisível aos workers E ao self-execute da própria main.

🎯 **RAIZ = thread-index inválido da MAIN no job-system.** Disasm de WaitForJobGroup (fresh pixelcup):
- `0x698660` = ENQUEUE: escreve no ring `[[mgr+9496]+256]` indexado por head `[jobgrp+264]` vs cap `[jobgrp+268]`.
  **NÃO faz Semaphore::Release de worker nenhum** (por isso kick não ajuda — não é assim que workers acordam).
- `0x6986d8` = WaitForJobGroup(mgr=x0→x19): chama **self-execute** `blr [[x19]+1712]` (0x698714) → checa
  done-byte `[x19+9736]` → se não-done espera no sem `[x19+9472]` via `bl 0xbacad8`. **O self-execute
  ([mgr_vtbl+1712]) NÃO roda o job da main** = usa o thread-index (errado) p/ escolher a deque → lê a deque
  errada → não acha o próprio job → bloqueia.
- `mgr` (job-manager singleton) vem por arg/vtable; o global NÃO foi achado estaticamente (callers via vtable;
  0x12b9380 é stale). Sem mgr não deu p/ dumpar o ring (confirmar head vs cap = H1-nada-enfileirado vs H2-órfão;
  os 3 experimentos apontam **H2/órfão**).

🔜 **PRÓXIMO (foco único): achar+corrigir o thread-index da main.**
   (a) achar o storage do índice de job-worker (NÃO é ELF-TLS: libunity tem 0 PT_TLS/TLS-reloc; é bionic
       TLS-slot via tpidr+offset OU array global indexado por gettid/pthread_self). Disasm: achar o
       `[mgr_vtbl+1712]` (execute) — é a MESMA func do loop dos workers — e ver como ela lê o índice da
       thread corrente; replicar a registração p/ a main (ou escrever o índice certo no slot da main).
   (b) p/ pegar `mgr` em runtime sem gdb: hookar 0x6986d8 (entrada) gravando x0 num global (estilo cr1_tramp),
       ou achar o singleton via /proc/mem escaneando o stack da main (x19 salvo) por um ptr cujo +9496→ring sano.
   (c) alternativa: hookar WaitForJobGroup p/ rodar o execute em LOOP até o grupo completar — só funciona se
       o execute achar o job (= depende de corrigir o índice; senão loopa à toa).
   ⚠️ kick de worker é BECO PROVADO (job não está na fila deles). Default run (sem PC_*) = checkpoint conhecido.

---
## SESSÃO 2026-06-23 (cont. 4) — LOCALIZAÇÃO REAL do stall (clean config): pthread_cond completion-wait @0x651400
⚠️ **CORREÇÃO do diagnóstico**: no run LIMPO (só `CUP_FRAMES=999999999`, MT-off no device), **NENHUMA thread
está em WaitForJobGroup** (varri todos os stacks via /proc/PID/mem por return-addr 0x698748 — ZERO). O
"WaitForJobGroup" das sessões anteriores era de OUTRA config (PC_INLINETASK/TER_CHOREO → deadlock GFX).

🎯 **STACK REAL da main (tid==pid) parada (~frame 3), via /proc/mem (kstk SP do /proc/TID/syscall):**
`nativeRender(0x659e20) → 0x641a64 → 0x6424e0 → 0x64ec70 → 0x55c79c → 0x651400 → 0xd613c0 → futex`
- `0x55c740` = **std::call_once / lazy-init DCLP** (ldar [x20]; cbz → init via `blr x21`).
- `0x651400` (corpo em 0x65140c) = a função que BLOQUEIA. Cria objeto (vtable lu+0x1104b08), aloca,
  **pthread_mutex_lock@plt (0x6514ac)**, agenda trabalho (bls 0x65b1b0/0xd60490/0x65b140 — setam target
  em [sp+8]), depois LOOP de espera:
  ```
  651530: x8=[x19+88]; cbz->651540 ; 651538: x8=[x8]; cbnz->651550(DONE)
  651540: pthread_cond_wait@plt(cond=x19+0x88, mtx=x20) ; 65154c: b 651530 (re-check)
  ```
  = espera até `[[x19+88]]!=0` (resultado) / `[x19+184]==[sp+8]` (target de conclusão). **= PARALLEL-FOR /
  ScheduleAndWait**: main agenda N chunks p/ workers + espera o contador de conclusão chegar no target.

🧱 **POR QUE TRAVA (confirmado):** `CUP_CONDPOLL=50` (pthread_cond_wait_fake faz timedwait+re-check, lógica
   VALIDADA em pthread_fake.c:160-171) **NÃO destrava** → o predicado [x19+184] **NUNCA é satisfeito** =
   **NÃO é lost-wakeup; os worker-chunks NUNCA são executados.** Os workers (Baselib futex) kickados
   (PC_KICKWORKERS=2/3) acordam mas não acham trabalho → os chunks estão na **deque LOCAL órfã da main**
   (main = thread do loader, sem índice de job-worker válido → enfileira p/ deque que ninguém rouba e a
   própria main não self-executa os chunks restantes; só fez o chunk dela e foi esperar).
   ⟹ **MESMO MURO RE4-class**: dispatch de work-stealing quebrado pq a MAIN não está registrada no job-system.

🔜 **PRÓXIMO (o ÚNICO caminho real, já bem cercado):**
   1. **Registrar a main como worker** OU **corrigir o thread-index da main** p/ que (a) os workers roubem os
      chunks E (b) a self-execução ache os chunks. Achar o storage do índice: NÃO é ELF-TLS (0 PT_TLS/reloc
      na libunity) nem o canary tpidr+0x28. Disasm da função execute/steal (chamada no loop dos workers,
      perto de 0xbacad8 Acquire) p/ ver como lê o índice da thread corrente.
   2. **OU forçar o parallel-for a rodar INLINE** (1 thread): achar onde o nº de chunks/workers é decidido
      (target [sp+8] em 0x651400) e forçar target=trabalho-que-a-main-faz, OU fazer a main executar TODOS os
      chunks antes do wait (hook em 0x651400 ou no scheduler 0x65b140).
   3. Ferramentas p/ achar o produtor/consumidor: **TER_CONDWHO** + **condtrace** (pthread_fake.c: loga
      `[CT] WAIT tid cslot caller`) — rodar e ver se ALGUÉM sinaliza o cond; **/proc/mem stack-walk** (provado
      melhor que gdb aqui — gdb 17.2 instável neste alvo, só `bt` p/ FILE funciona).
   ⚠️ Kick de worker = BECO PROVADO. CUP_CONDPOLL = não é lost-wakeup. Default run (sem PC_/CUP_) = checkpoint.

### nota cont.4b — CPU-spoof NÃO reduz o pool de workers (beco)
`CUP_1CORE=1 TER_JOBINLINE=1` (sysconf=1, /proc/cpuinfo=1proc, /sys/cpu/*=0, sched_getaffinity=1) **NÃO**
reduz: Unity ainda cria **3 Job.Worker + 16 Background Job. + Loading.Preload/AsyncRead**. Unity lê o core
count por uma via que o spoof não cobre (provável openat raw a /sys, ou cache antes dos GOT-patches), e os
16 Background são pool FIXO independente de cores. ⟹ não dá p/ forçar parallel-for inline reduzindo workers.
O caminho real continua sendo registrar a main no job-system (índice de thread) — item 1 acima.

---
## SESSÃO 2026-06-23 (cont. 5) — CAMINHO A (forjar thread-index) → diagnóstico REFINADO + duas paredes LIGADAS
Foco: hipótese "main não-registrada no job-system → deque órfã". Instrumentei o TLS (controlamos 100%
via sh_key_create/getspecific/setspecific) e desmontei o staller real. Conclusões SÓLIDAS (corrigem sessões anteriores):

🟢 **O JOB-SYSTEM GERAL FUNCIONA.** Com TER_CONDTRACE: a main faz VÁRIOS cond_wait em `libunity+0x63a13c`
   que SÃO sinalizados por workers (tids diferentes) e completam — workers EXECUTAM os parallel-for da main.
   ⟹ a tese "deque órfã / dispatch totalmente quebrado" está REFUTADA p/ o caso geral.

🎯 **A PAREDE REAL (config limpa) = 1 singleton lazy-init em `libunity+0x65140c`** (via call_once `0x55c79c`
   ← `0x64ec70` ← nativeRender). Ele cria um objeto de conclusão em `[obj+0x88]` (vtable `0x1104b08`),
   agenda trabalho e espera `cond_wait` em **`0x65154c`** até `[[obj+0x88]]!=0`. Esse cond (cslot do heap,
   NÃO da stack) **NUNCA é sinalizado** (TER_CONDTRACE: 0 signals p/ ele) → trava só AQUI. As dezenas de
   `0x63a13c` antes dele completam normal.

🔬 **key=14 = Baselib per-thread job-info** (PC_TLSLOG=1, env): cada worker lê `key=14` com valor próprio
   (stride 0x40 = array da tabela de workers); a main lê `key=14 -> (nil)` na 1ª vez e o GetOrCreate
   (`libunity+0x593880`) aloca p/ ela um struct individual de 32B com `[24]=1` (flag "thread EXTERNA").
   PC_MAINWORKER=1 (limpa `[24]` da main) testado → SEM efeito (não é a decisão de roteamento).

🧪 **PC_QREDIR (novo)**: hookei `d6e410` (GetCurrentThreadJobQueue) p/ redirecionar a main à fila de um
   worker. RESULTADO: `[QREDIR]` NUNCA logou ⟹ **o staller `0x65140c` NÃO usa o caminho d6e410/d5166c**.
   Seu enqueue real é via `0x65b1b0`/`0xd60490`/`0x65b140` (token de conclusão montado em d60490), consumidor
   ainda não localizado. (d6e410 só é usado por d5166c/d6f34c, que esse job não toca.)

🔑 **PC_INLINETASK (já existente) FINGE a conclusão DESTE staller** (patch em `0x651530` = topo do loop de
   wait de `0x65140c`; seta node[0]=1, 2 hits). Com ele o boot **AVANÇA** e bate na PRÓXIMA parede:
   **`IOException: Disk full. Path /storage/roms/ports/pixelcup/[Unknown]`** + NullRef. strace openat:
   **Unity NUNCA tenta abrir datapack.unity3d** (0 openat) → o erro é na **RESOLUÇÃO DE PATH do Play Asset
   Delivery**, antes de qualquer open ("[Unknown]" = componente de path nulo). = EXATA parede da cont.3
   (mount do UnityDataAssetPack: gate U+0x417bfc lookup "UnityDataAssetPack" → -1).

📌 **DUAS PAREDES AGORA LIGADAS E ISOLADAS:**
   (A) singleton nativo `0x65140c` — FINGÍVEL via PC_INLINETASK (sem crash imediato).
   (B) mount/path do datapack (Play Asset Delivery) — vira a parede ATIVA quando (A) é fingida.
   ⟹ **Caminho recomendado p/ próxima sessão: PC_INLINETASK (bypassa A) + consertar (B)** — fazer Unity
   LOCALIZAR `bin/Data/datapack.unity3d`. O arquivo ESTÁ no device (401MB, bin/Data/). O bug é só a
   resolução de path do PAD retornar "[Unknown]". Atacar o gate nativo da cont.3 (U+0x63f308 mount /
   U+0x417bfc lookup) OU achar o método JNI de path que Unity chama e que devolve nulo (≠ getAssetPackPath,
   que já respondemos). jni_shim.c:760 `getPath/getAbsolutePath` devolve `.../userdata` (colide com a
   localização do pack — possível causa do "[Unknown]"; testar desambiguar por contexto do caller).

🔧 **INSTRUMENTAÇÃO NOVA (no tree, default OFF, reusável):**
   - `PC_TLSLOG=1` (main.c sh_*): loga KEYCREATE/SET/GET do TLS do Unity com caller simbolizado. Achou key=14.
   - `unity_install_hook4(off,fn,&orig)` (main.c): detour em libunity com relocação de prólogo (trata adrp) +
     trampolim p/ chamar o original. (irmão do ter_install_hook4 que era só il2cpp.)
   - `PC_QREDIR=1` + `my_d6e410`/`g_worker_q` (main.c): scaffold de redirect de fila (provou que d6e410 ≠ caminho do staller).
   - `PC_MAINWORKER=1`: limpa flag [24] da thread-info da main (sem efeito; manter p/ referência).
   Ferramentas de RE: /tmp/U.asm (objdump full da libunity), TER_CONDTRACE/TER_CONDWHO (pthread_fake.c).
   ⚠️ Default run (sem PC_/TER_) = loading congelada = checkpoint. PC_INLINETASK = avança até parede (B).

---
## SESSÃO 2026-06-23 (cont. 6) — 🟢 DISK-FULL RESOLVIDO (dica do porter); parede única isolada = mount PAD
🟢 **"IOException: Disk full" RESOLVIDO.** Causa: o **`/tmp` (tmpfs 416MB) do device estava 100% cheio**
   do MEU lixo acumulado de sessões (dezenas de `pc_fb*.raw`/`shot_*.raw` de 24MB + /tmp/cores). Mono/Unity
   escreve temp e o `write()` real dava **ENOSPC → "Disk full"**. FIX = `rm /tmp/*.raw /tmp/shot_* ...`
   (/tmp 100%→0%). Confirmado: 0 ocorrências de "Disk full" depois. (porter: "terraria teve isso e foi
   resolvido lá" — era a mesma classe.) ⚠️ Manter /tmp do device limpo entre runs (iter.sh acumula .raw).
   Defensivo extra commitado: **my_statfs64 agora INFLA o espaço livre p/ 50GB** (f_blocks/f_bfree/f_bavail),
   pois o cartão do device só tem ~936MB livres e o real podia disparar checagens de "low storage".

🟢 **NullReferenceException = Rewired** (input DualShock4 native helper), **CAPTURADO/não-fatal** (confirmado:
   "Rewired: Exception setting up native Android input helper"). NÃO é o datapack. Red herring (handoff já dizia).

📌 **ESTADO LIMPO ATUAL (PC_INLINETASK=1 + display env + /tmp limpo):** loading screen RENDERIZA (md5
   5703378b, o malabarista, estável) — SEM disk-full, SEM erro fatal. Mas **congela**: o `datapack.unity3d`
   NUNCA é carregado. `PC_INLINETASK` é OBRIGATÓRIO (sem ele o singleton nativo 0x65140c trava a main →
   tela 100% PRETA, nem o loading renderiza).

🧱 **PAREDE ÚNICA REMANESCENTE = mount nativo do Play Asset Delivery** (parede cont.3, agora ISOLADA e SEM
   ruído de disk-full): Unity chama `getAssetPackState(UnityDataAssetPack/UnityStreamingAssetsPack)` → nós
   respondemos COMPLETED(4) via `nativeStatusQueryResult(name,4,0)`. MAS Unity **NUNCA chama getAssetPackPath**
   nem tenta abrir o datapack (strace openat: 0 hits de datapack). O mount nativo (U+0x63f308) faz lookup
   "UnityDataAssetPack" num registro (x19) que dá **-1** → não monta → datapack não carrega → loading congela.
   Testado `getAssetPackPath`→`bin/Data` (CUP_PACKPATH env): SEM efeito (mount falha ANTES de usar o path).
   ⟹ o callback `nativeStatusQueryResult` marca COMPLETED mas NÃO registra a LOCALIZAÇÃO do pack no registro
   que o mount consulta. **PRÓXIMO (único foco): consertar o registro/mount do PAD** — (a) achar o callback
   nativo que registra location (≠ nativeStatusQueryResult; talvez `nativeOnAssetPackStateUpdate` com path/
   bytesDownloaded/totalBytes) e chamá-lo com o path real, OU (b) hookar o lookup U+0x417bfc / mount U+0x63f308
   p/ servir "/storage/roms/ports/pixelcup/bin/Data/datapack.unity3d". Reanalisar U+0x634f60 (registra via
   JNI vtbl+1336 — em QUE objeto) vs x19 do mount. jni_shim getAssetPackPath agora aceita env CUP_PACKPATH.

🔧 Novo no tree (env-gated): CUP_PACKPATH (override getAssetPackPath), my_statfs64 infla 50GB (sempre on).
   ⚠️ Run que RENDERIZA loading = env completo `TER_NOSTORAGEPATCH=1 CUP_FRAMES=999999999 SDL_AUDIODRIVER=pulse
   TER_SCREEN_W=1280 TER_SCREEN_H=720 PC_INLINETASK=1 TER_CHOREO=1`. Screenshot: cat /dev/fb0 (fb 1280x1440x4
   double-buf; visível = 1ºs 1280x720x4). Limpar /tmp do device ANTES de rodar.

---
## SESSÃO 2026-06-23 (cont. 7) — mount do datapack NÃO é a parede; raiz = async-IO órfã (cont.2)
🔬 **PC_PADHOOK (novo): hook do mount do datapack `libunity+0x63f308`.** Loga a string que ele busca por
   "UnityDataAssetPack" (corestr_get extrai a core::String). RESULTADO: o hook instala mas **`[PAD] mount`
   NUNCA loga → o mount 0x63f308 NUNCA é chamado.** ⟹ **a parede é UPSTREAM do datapack.** A análise da
   cont.3 ("gate U+0x417bfc → -1") estava certa sobre a função (`0x417bfc` = `String::find`, NÃO lookup de
   registro — busca "UnityDataAssetPack" DENTRO da string-arg do mount) mas o mount nem é alcançado.
   Strings confirmadas: 0x14dc54="UnityDataAssetPack", 0x11a28d="assets/bin/Data", 0x132892="datapack.unity3d".
🔬 **PC_QREDIR capturou a fila da main do d6e410**: q=heap, **vtable no HEAP** (0x557f..., não libunity) →
   confirma que o caminho d6e410/d5166c NÃO é o do staller 0x65140c (que usa 0x65b1b0/d60490). d6e410 é
   p/ outro subsistema async. (push[496] e vtable em heap = não é vtable estática; cuidado ao interpretar.)

🧱 **PAREDE REAL (refinada) = LOAD ASYNC DOS RECURSOS não dispara** (= raiz cont.2 PreloadManager). Com
   PC_INLINETASK (singleton nativo fingido) a loading C# roda e chama `CPCSLoadingState.startLoadingAudioClips`
   → `Resources.LoadAsync` dos áudios → mas `resources.resource` (162MB) e `datapack.unity3d` **NUNCA abrem**
   (strace: só data.unity3d). `resourceRequestsEnded` espera os áudios (isDone + AudioClip loadState==2) que
   nunca completam → loading CONGELA, nunca chega a montar o datapack. As threads Loading.Preload/AsyncRead
   ficam IDLE. = mesma classe do job-system: o JOB que alimentaria o AsyncRead nunca roda (órfão).
   ⟹ **RAIZ ÚNICA consistente em cont.2-cont.7: trabalho async agendado pela thread de loading vai p/ fila
   não-drenada (main/loading NÃO registrada como worker do job-system).** O parallel-for GERAL (0x63a13c)
   funciona pq usa o scheduler geral (workers roubam); o async-IO/singleton usam fila thread-local órfã.

🎯 **PRÓXIMO (3 rotas, todas miram a MESMA raiz):**
   (a) **Registrar a loading/main thread no job-system** p/ o enqueue async ir p/ fila drenada (achar a
       registração que os workers fazem no startup — a tabela de worker-index; NÃO é pthread-key, é tpidr
       nativo OU array global). É o fix que cura TUDO (singleton + áudio + datapack) de uma vez.
   (b) **Forçar AsyncReadManager.Read / Resources.LoadAsync a rodar INLINE/SYNC** na thread de loading
       (interceptar o read p/ ler o arquivo direto + postar conclusão). Achar AsyncReadManager.Read
       (cont.3: enqueue U+0x425508 push ring + Semaphore::Release mgr+0xd4; Preload loop U+0x4ee330).
   (c) **Drenar a fila thread-local da main**: achar o método "execute one job" do queue e rodá-lo numa
       helper-thread ou no wait. (d6e410 NÃO é o caminho do staller — investigar o scheduler 0x65b1b0/d60490.)
🔧 Novo no tree (env-gated OFF): PC_PADHOOK (mount hook + corestr_get), CUP_PACKPATH. Default=checkpoint.

---
## SESSÃO 2026-06-23 (cont. 8) — registro da MAIN como worker: FUNCIONA mas NÃO drena o async
🟢 **Mecânica de registro DECIFRADA e implementada (PC_MAINREG, sem crash):**
   - `0x63a260` = **RegisterWorkerThread**: insere o descritor numa lista global encadeada
     (`0x11cd000+0xa58`, lock `0x11cd000+0xa2c`), grava gettid em [+32], signal no cond [+40]+0x28.
   - Layout do descritor (capturado via PC_WREG hook em 0x63a260): [+0]/[+8]=links, [+16]=buffer,
     [+40]=lock GLOBAL (igual p/ todos), [+56]=fn ptr libunity (corpo do worker), [+80]=fila por-worker
     (stride 0x1c0), [+88]=0x1b. Snapshot estável em g_wdesc_template (copiar BYTES, não o ponteiro —
     a memória do desc do worker é transiente e vira lixo→[+40]=NULL→crash em pthread_mutex_lock).
   - try_register_main(): forja desc (copia template, links=0, [+16]=buffer próprio, [+80]=fila da main
     do d6e410, [+32]=0) e chama 0x63a260. **Registra OK, loading renderiza, 0 crash.**
🔴 **MAS não destrava:** datapack/resources continuam fechados. PC_MAINREG (mesmo +PC_KICKWORKERS=3) NÃO
   faz os workers rodarem o trabalho async da main. CONCLUSÃO FIRME: **a fila thread-local do async (d6e410)
   NÃO está no caminho de work-stealing dos workers** (eles roubam de OUTRO tipo de fila, a do parallel-for
   geral que já funciona @0x63a13c). Registrar a main e/ou kickar não basta — os workers parkados não
   consultam essa fila.
🎯 **PRÓXIMO (raiz, agora ainda mais cercada): DRENAR a fila thread-local do async pela PRÓPRIA main no
   ponto de wait.** No fluxo normal do Unity, a thread que faz schedule-and-wait (0x65140c) "ajuda a
   executar" sua própria fila antes/durante o wait; a nossa main faz cond_wait puro. Achar o método
   "processa 1 job" da fila do d6e410 (mq, vtable em HEAP — investigar a estrutura real do objeto; pode
   ser closure/std::function, não vtable C++ clássica) e chamá-lo em loop no trampolim do 0x651530 (onde
   hoje o PC_INLINETASK só FINGE node[0]=1). Alternativa: redirecionar o scheduler do singleton
   (0x65b1b0/d6c43c/d5166c) p/ usar o caminho do parallel-for GERAL (que funciona) em vez do thread-local.
🔧 Novo no tree (env-gated OFF): PC_WREG (captura descritor), PC_MAINREG (+PC_QFIELD/PC_USEWCTX), PC_PADHOOK,
   corestr_get, unity_install_hook4. Default=checkpoint. ⚠️ Limpar /tmp do device antes de rodar.
