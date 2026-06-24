# NieR (NierM) — port Android UE4 → NextOS / Mali-450 (so-loader)

> **Status: 🟡 EM ANDAMENTO — loader funciona; bootstrap do alocador do UE4 RESOLVIDO;
> avança até o registro de UObjects.** Sessão 1 (2026-06-21): recon + loader 2-módulos
> carregando o `libUE4.so` inteiro (4419 imports resolvidos) + **deadlock/recursão do
> bootstrap do FMallocBinned2 VENCIDO** (shim FMemory::Malloc/Realloc/Free + __cxa_guard
> estilo-bionic) → init_array progride por FNamePool e chega em
> `UObjectCompiledInDefer→NotifyRegistrationEvent` (registro de UClasses) onde dá assert.
> Render real = **muro arquitetural** (ver §5).

## ⚠️ INSIGHT ESTRATÉGICO: esta APK é uma build **DEVELOPMENT** do UE4
O `AndroidManifest` diz `BuildConfiguration=Development`, e o init_array roda
`RunGetVarArgsTests`, `FDateTimeFormattingRulesTest::RunTest`, Stats e Trace — ou seja, a build
inclui **testes automáticos + Stats + Trace + TODOS os `check()`** rodando no boot. Cada um é uma
camada de deadlock/assert/spin pra furar. **Uma build SHIPPING do mesmo jogo removeria quase tudo
isso** (checks compilados-fora, sem testes/stats/trace, init muito mais curto). Se algum dia
aparecer um APK Shipping de NieRM, a maioria destas paredes some. Esta é a versão "hard-mode".

## PROGRESSÃO DO INIT (sessão 1 — cada camada vencida)
1. ✅ Loader: gnustl + libUE4 (175MB) + 4419 imports + pools (mmap 384MB).
2. ✅ **Deadlock do alocador** (FMallocBinned2 ctor↔Malloc↔GCreateMalloc, recursão com
   GMalloc==NULL; gnustl __cxa_guard bloqueava a re-entrância). **FIX reutilizável p/ QUALQUER
   so-loader UE4:** (a) `__cxa_guard_acquire/release/abort` estilo-bionic (re-entrância da mesma
   thread retorna "feito" em vez de bloquear); (b) **shim de bootstrap do alocador**: hook de
   `FMemory::Malloc/Realloc/Free` que, enquanto `GMalloc==NULL`, usa `memalign`/`realloc`/`free`
   do glibc (+ registro de ponteiros bootstrap p/ rotear o free/realloc deles ao glibc no
   handoff). GMalloc global = `*(libUE4+0xae94c20)`; vtable FMalloc: Malloc=+24, Realloc=+32,
   Free=+40. Hooks via `hook_arm64` ANTES do init_array (callback pre_init, texto writable).
3. ✅ FNamePool / FName init.
4. ✅ **Registro de UObjects (`UObjectCompiledInDefer→NotifyRegistrationEvent`)** — passa, MAS
   dispara `check(ExistingPackageState)`. Capturei a msg hookando **`FDebug::CheckVerifyFailedImpl`
   @ 0x4be9a10** (entrada real; 0x4be9a90 era +128, erro): `expr='ExistingPackageState'
   file='.../CoreUObject/Private/Serialization/AsyncPackageLoader.cpp' line=139`. O `check()` faz
   `PLATFORM_BREAK` (brk) incondicional no call-site → retornar 0 não evita; adicionei **skip de
   `brk`** no crash handler (SIGTRAP/SIGILL, instrução 0xD42xxxxx → pc+=4).
5. 🔴 **FRONTEIRA ATUAL:** o check `ExistingPackageState` (AsyncPackageLoader.cpp:139) dispara
   **6×** durante o init_array (`NotifyRegistrationEvent` @ libUE4+0x4edcdf0). Com o brk-skip ele
   passa dos 6 brks MAS o **código pós-check roda com estado NULL e TRAVA** (não chega em
   "init_array DONE"; thread em estado R). Ou seja: **fatal-by-design — pular troca crash→hang**,
   não resolve. `ExistingPackageState` é genuinamente NULL no registro de UObjects. Raiz provável:
   na inicialização real do UE4,
   `NotifyRegistrationEvent` em static-init **DEFERE** o registro (lista processada depois em
   `ProcessNewlyLoadedUObjects` na engine-init); no nosso ambiente um flag global de "loader
   pronto?" lê diferente → toma o branch que exige `ExistingPackageState`. **Fix s2:** entender
   o gate do defer (achar o global que decide deferir vs registrar-já) e forçá-lo a DEFERIR;
   OU prover o estado de pacote esperado. (NÃO é o __cxa_guard — testar guard real aqui.)
   brk-skip capado em 200 (sai com 42) p/ não spinnar.
6. ✅ **AsyncPackageLoader VENCIDO** com `NotifyRegistrationEvent`→`ret` (no-op; EDL/bookkeeping
   não usado no static-init). Sem mais asserts.
7b. ✅ **Stats** (`FStatGroupEnableManager::GetHighPerformanceEnableForStat` @ 0x4d1d840):
   sondagem de hash em mapa vazio (HashCount=0→`and` com −1→OOB→spin infinito). FIX: hook→
   retorna `TStatIdData*` estatico zerado (stat inválido/ignorado). **Destravou init_array INTEIRO.**
8. ✅ **init_array COMPLETO (722 ctors)** → **`android_main` encontrado**. (Áudio: meu
   `SDL_AUDIODRIVER=dummy` era inválido→SDL_Init fatal; FIX: audio não-fatal no android_shim.c.)
9. ✅ **LogSuppression** (`FLogSuppressionImplementation::DisassociateSuppress` @ 0x4bc0a24):
   `ReverseAssociations.Remove==1` falha centenas de vezes (mapa FName não acha)→no-op `ret`.
10. ✅ **`Entered AndroidMain()`** — a engine REAL roda: event thread, looper, callbacks, sensors,
   `APP_CMD_INIT_WINDOW`. Janela: stubs `ANativeWindow_getWidth/Height`=1280/720,
   `acquire`=devolve janela (antes nier_ret0=0→janela 0×0). Evento `APP_EVENT_STATE_WINDOW_CREATED
   [1280,720]` + `WINDOW_GAINED_FOCUS`. (`check(WindowIn)` LaunchAndroid.cpp:1116 ainda falha mas
   é brk-skipado.)
11. ✅ **GetJavaEnv crash** — `AndroidMain→AndroidJavaEnv::GetJavaEnv` crashava pq **`GJavaVM` era
   NULL**: no Android o sistema chama `JNI_OnLoad` ao carregar o .so (seta GJavaVM); no so-loader
   ninguém chamava. FIX: `main.c` chama `JNI_OnLoad(fake_vm, NULL)` após android_shim_init →
   retorna 0x10006 (JNI 1.6); JNI funciona (FindClass GameActivity, GetMethodID AndroidThunk*).
12. ✅ **SIGSEGV mascarado** — strace revelou um **SIGSEGV REAL (SEGV_MAPERR si_addr=0x52773b31ac)**
   capturado por um handler e re-levantado via `tgkill` → mascarado. Era o **handler de sinais do
   SDL**. FIX: **`SDL_NO_SIGNAL_HANDLERS=1`** (memória do SOR4 já mandava). Com isso **NÃO crasha
   mais**.
13. 🏁 **ENGINE UE4 RODANDO no Mali-450** (sem crash, exit=timeout). Threads vivas: main (engine),
   Trace heartbeat, **PulseMainloop/PulseHotplug (áudio pulse subiu)**, **event thread da UE4 em
   ALooper do_sys_poll**. JNI ok, audio ok, event loop ok.
14. ✅ **Handshake janela→RHI** — `WindowIn`/`GetHardwareWindow_EventThread`==null fazia o RHI
   esperar janela p/ sempre (0 EGL/GL). FIX: hook `FAndroidWindow::GetHardwareWindow_EventThread`
   (@0x5138038)→ponteiro fake NÃO-NULL (egl_shim ignora e usa SDL2; ANW stubs ignoram o ptr). A
   AndroidMain então PASSA do window-wait (gated NIER_NOHWWIN).
15. ✅ **AndroidMain keymap loop** — `AndroidMain+284` loopava `GetKeyMap`(contagem lixo)→TSet
   estouro. FIX: hook `FAndroidPlatformInput::GetKeyMap`@0x51058ac → retorna 0 (loop pulado via
   cbz; keymap de teclado irrelevante). Gated NIER_NOKEYMAP.
16. ✅🔑 **MURO SISTÊMICO DE CONTAINERS (TSet/TMap) RESOLVIDO (sessão 2, 2026-06-22) — era o
   FMallocBinned2.** `AndroidMain+0x3d8` faz ~9 `TSet<uint32>::Emplace` (teclas reservadas) e o
   `EmplaceImpl`@0x445d214 crashava em 0x445d294 (`ldr [x9, w24*12]`). **Diagnóstico definitivo
   via dump do objeto TSet no crash-handler** (lê x21=this por /proc/self/mem): hash-ptr[+64]
   estava NÃO-NULL (0x559b9e3520) e HashSize[+72]=16, MAS o **conteúdo do bucket era LIXO**
   (`0x9b9d35b0` = low32 de um ponteiro vizinho do Elements.Data=0x559b9d8b00) → **outra alocação
   sobrescreveu o buffer de hash = memória sobreposta**. RAIZ = o **FMallocBinned2 do jogo** (ou a
   interação bootstrap↔binned2) entregando blocos sobrepostos no nosso ambiente. **FIX = rotear
   TODO `FMemory::Malloc/Realloc/Free` pro glibc** (memalign/realloc/free; realloc trata NULL==malloc,
   count0==free, e re-alinha >16 via memalign+memcpy). glibc é sólido e uniforme. **DEFAULT ON**
   (reverter p/ binned2 só com `NIER_BINNED2=1`). Isto destrava TSet/TMap em TODO o engine de uma
   vez (a hipótese inicial "hash-ptr NULL / Realloc(NULL)→NULL" foi REFUTADA pelo dump — o hash era
   alocado, o problema era corrupção). Reusável p/ QUALQUER so-loader UE4 com binned2 problemático.
17. ✅🔑 **Handshake AndroidMain←Java (nativeResumeMainInit) — wait-loop infinito RESOLVIDO.**
   Depois da camada 16, a main entrava num loop `FPlatformProcess::Sleep(0.01)` @ AndroidMain+0x4e4
   (0x44344c8) esperando o flag em **libUE4+0xaf0bc94** virar !=0. Esse flag é setado por
   `Java_com_epicgames_ue4_GameActivity_nativeResumeMainInit` (0x4433f74), chamado pela UI-thread
   Java — que NÃO roda no so-loader (mesma classe do JNI_OnLoad da camada 11). A função inteira
   depois espera um 2º flag (handshake cross-thread), então **setamos o flag direto** em main.c
   antes de `android_main` (gate `NIER_NORESUMEINIT`). Passou: loga **"Final commandline:"** +
   **"Created sync event"** + reprocessa APP_CMD_INIT_WINDOW.
18. ✅🔑 **DEADLOCK do GAndroidWindowLock (mutex) RESOLVIDO.** Após o resume-init, a AndroidMain
   (engine) bloqueava em `pthread_mutex_lock`@AndroidMain+0xac4 (0x4434c84) sobre o mutex
   M=`*(libUE4+0xaedda30)`. A **event-thread (pump @0x4442970) travava M em 0x4442aac e ficava
   em `ALooper_pollAll(-1)` SEGURANDO M** → corrida de startup (no UE4 real a AndroidMain pega M
   primeiro). FIX: NOP no lock(0x4442aac)+unlock(0x4442b00) do pump → roda sem segurar M.
   Gate `NIER_NOPUMPLOCKFIX`. (diagnóstico: /proc/wchan futex_wait + stack-scan + xref do mutex.)
19. ✅🔑 **`FMallocBinned2` reconstruído no PreInit abortava — RESOLVIDO.** `FEngineLoop::
   PreInitPreStartupScreen → FMemory::SetupTLSCachesOnCurrentThread → GCreateMalloc →
   FMallocBinned2::ctor → BinnedAllocFromOS` dava **Fatal "munmap (for tail align) failed"**
   (AndroidPlatformMemory.cpp:378): o binned2 faz mmap(size+align) e `munmap` da cauda p/ alinhar;
   no nosso ambiente o mmap volta JÁ-alinhado → cauda len=0 → `munmap(x,0)`=EINVAL → abort. FIX:
   hook `munmap` tolerante (len0/falha de trim = sucesso; best-effort). Agora PreInit roda
   ("Used memory before allocating anything was 0.00MB") e nosso APP_CMD_RESUME flui →
   **ON_START/ON_RESUME/APP_ACTIVATED/RUN_CALLBACK** enfileirados (lifecycle OK).
20. 🔬 **FRONTEIRA ATUAL (fim sessão 2): PreInit FALHA por JNI.** `check(LocalObject)` @
   **AndroidJava.cpp:24** (brk-skip @0x4a0a77c) — uma chamada JNI (FindClass/NewObject) devolve
   NULL no nosso jni_shim → cascata → `check(false) 'Engine Preinit Failed'` @ LaunchAndroid.cpp:463
   → AndroidMain retorna → exit limpo. **Próximo:** achar QUAL objeto Java o PreInit pede em
   AndroidJava.cpp:24 (provável FJavaClassObject de config/asset/OBB) e servir um objeto válido
   no jni_shim. Adiante = RHI criar contexto GL → **muro ES3.1/ETC2 (§5): render impossível no
   Mali-450; só X5M ES3.2)**. ⚠️OBB(799MB)+libUE4(183MB) não cabem no /storage(936MB) do .164.

**📌 RESUMO sessão 2 (2026-06-22): 4 muros vencidos** — (16) corrupção sistêmica de containers
TSet/TMap = FMallocBinned2 entregando memória sobreposta → **alocador todo no glibc** (default ON,
`NIER_BINNED2` reverte); (17) handshake resume-init (flag 0xaf0bc94); (18) deadlock do mutex de
window (pump segurava M no poll); (19) binned2 reconstruído abortava no munmap-de-alinhamento.
Engine agora chega ao **FEngineLoop::PreInit** e falha numa dependência JNI (camada 20).

### (camada 7 original — contexto, agora resolvida pelo 7b acima)
7a. init avança até **init[267]** e a main entra num **spin
   loop infinito** em **libUE4+0x4d1dbc0** — uma **sondagem de hash que nunca acha o sentinela
   vazio** (`cmn w8,#1;b.ne` loop), na área de **FStatGroupEnableManager / Stats** + lookup de
   FName. PC oscila em ~36 bytes (confirmado spin, não trabalho). Provável: tabela de hash (FName
   ou stat-name) inconsistente — possível efeito do __cxa_guard estilo-bionic deixar um hash
   meio-init, OU genuíno da subsistema Stats da build Development. **Fix s2:** (a) disassemblar a
   função que contém 0x4d1dbc0 e achar de qual hash/global ela depende; (b) descobrir o ctor de
   init[267] (logar endereço no so_execute_init_array) e ver se é Stats/teste pulável; (c) testar
   um `__cxa_guard` REAL (não-permissivo) só p/ ver se o hash passa a inicializar certo.

## 1. O que é o APK

- `NieR.Automata.apkaward_1.0-APK_Award.apk` (817MB) — pacote `com.YourCompany.NierM`.
- **Engine: Unreal Engine 4.24.3** (`libUE4.so` 183MB; `EngineVersion=4.24.3`, branch `++UE4+Release-4.24`).
- Entry: `android_main` exportado (NativeActivity glue) + `JNI_OnLoad` + `ANativeActivity_onCreate`.
  Launcher Java = `com.epicgames.ue4.GameActivity` (JNI-pesado).
- Libs nativas: `libUE4.so`, `libgnustl_shared.so` (C++ runtime — **gnustl**, ABI `_ZNSs` COW),
  `libvrapi.so` + `libOVRPlugin.so` (plugins Oculus mortos — stubados).
- Dados: `assets/main.obb.png` (799MB) = OBB renomeado, ZIP contendo
  **`NierM/Content/Paks/NierM-Android_ETC2.pak`** (799MB, flavor de textura **ETC2**).

> ⚠️ **SEGURANÇA:** o APK do apkaward foi re-empacotado com um receiver
> `com.YourCompany.NierM.lifecycle-trojan` + AdActivity + re-assinatura SIGN360 =
> **adware/trojan no lado Java (dex)**. O so-loader **NÃO executa o dex/Java** — só o
> `libUE4.so` nativo — então o trojan não roda no nosso pipeline. Mas o APK está contaminado.

## 2. Arquitetura do loader (2 módulos, igual Bully/reVC)

```
Módulo A = libgnustl_shared.so  -> C++ runtime gnustl (4130 símbolos: std::string COW,
                                   ostream, condition_variable, type_info, __cxa_*, _Unwind_*)
Módulo B = libUE4.so            -> resolve UND via:
              dynlib_functions (egl_shim + gl + ovrp stubs + android_shim + bionic shims)
              + revc_pthread_table (ponte pthread bionic->glibc, mutex RECURSIVO)
              + snapshot(gnustl) + dlsym(RTLD_DEFAULT) das libs do device
              (SDL2/GLESv2/EGL/glibc/libm/libz/libstdc++/libgcc pré-carregadas GLOBAL)
```
Entry = **`android_main`** (NativeActivity). `android_shim` fabrica `struct android_app`
+ JNIEnv falso (`jni_shim`, package `com.YourCompany.NierM`) + SDL. EGL→SDL2 (contexto ES2).

Arquivos-chave: `src/main.c` (orquestra 2 módulos + crash handler + phase markers),
`src/imports.gen.c` (tabela — gerada por new-port.sh e MUITO consertada à mão), `src/so_util.c`
(loader ELF do Bully: snapshot + dlsym fallback), `src/pthread_bridge.c`, `src/egl_shim.c`,
`src/android_shim.c`, `src/jni_shim.c`. Build: `build.sh` (toolchain Amlogic-old aarch64).

## 3. Consertos feitos no scaffold (bugs do new-port.sh)

O `new-port.sh` gera um esqueleto incompleto; foi necessário:
1. **`dynlib_numfunctions` não era definido** → adicionado.
2. **egl mapeado p/ nomes errados** (`eglInitialize_shim`) → renomeado p/ a API real do core (`egl_shim_Initialize`).
3. **37 `pthread_*_fake` indefinidos** → removidos; pthread vem da `revc_pthread_table` + dlsym.
4. **opensles `SL_IID_*_shim`/`sleep_shim` indefinidos** → removidos; só `slCreateEngine` real.
5. **Entradas `A*` (Android NDK) NUNCA foram emitidas** apesar do android_shim defini-las →
   fiadas 22 reais (AConfiguration/AInput/AMotion/AKey/ALooper) + 19 stubs (AAsset*/ANativeWindow*).
6. **Shims bionic internos** adicionados: `__errno`→`__errno_location`, `__isfinitef`, `__sF` (dummy),
   `_ctype_` (dummy), `__assert2`, `__google_potentially_blocking_region_*`, `__system_property_get`.
7. **`__android_log_print/write`** → stderr. **117 `ovrp_*`** → `nier_ret0`.
8. Includes libc/GLES2 no imports.gen.c; preload de libz/libstdc++/libgcc no main.c.

## 4. Estado atual — ONDE TRAVA (sessão 1)

`bash run.sh` no device (Amlogic, EMUELEC, Mali-450, 832MB RAM):
```
preload libs OK · gnustl carrega (4130 syms) · libUE4 relocate OK · resolve 4419 syms OK ·
finalize OK · init_array... <TRAVA AQUI, não chega em "init_array DONE">
```
Backtrace (SIGABRT após 25s) simbolizado via `nm`:
```
FMallocBinned2::FMallocBinned2()  -> FAndroidPlatformMemory::BinnedAllocFromOS()
  -> FMemory::GCreateMalloc() -> FMemory::Malloc() -> FNamePool::FNamePool()
  -> FName::FName() -> FMemory::Malloc() -> ... (padrão se repete)
PC numa lib de sistema (futex/syscall).
```
**Diagnóstico FECHADO (gdb on-device — gdb/gdbserver/strace EXISTEM no .164):**
`.bss` zerado (so_util:237), mutexes da ponte recursivos, pools do alocador alocam OK (hook mmap:
FMallocBinned2 reserva 384MB sem loop/FAILED). São **2 threads**:
- **Thread worker (LWP +2): NORMAL/benigna** — é o heartbeat do Trace: loop `usleep(24000=24ms)`
  → `ldrb w8,[estado]` → dispatch (state 0/1/2). Desassemblado @ libUE4+0x4a09840. NÃO é o culpado.
- **Thread MAIN: `FUTEX_WAIT`** (regs: x8=0x62=98=SYS_futex, x1=0=FUTEX_WAIT, x0/uaddr=
  `libUE4 + 0xa5ac8a8` [global no .bss], x2/val=0x10100) — **bloqueada durante o `init_array`**.
**Conclusão:** um **inicializador estático do UE4 cria/espera um FEvent global** que nunca é
sinalizado no nosso ambiente (a thread que deveria postá-lo não roda / postaria por um caminho
que nosso shim quebra). **Mesmo padrão do deadlock do job-system do RE4** ([[project-re4...]]).
O patch `Writer_Initialize→ret` (Trace) foi testado e **NÃO resolve** (a worker do Trace era
red-herring); fica gated por `NIER_TRACE` mas é inócuo.

### 🔬 CAUSA-RAIZ (avanço da sessão 1b) — recursão na criação do GMalloc
Teste com `__cxa_guard_acquire/release/abort` **permissivos** (não-bloqueantes) **QUEBROU o
deadlock** — prova que a trava era um **guard de init estático do gnustl** segurando a main.
Com o guard liberado, avança e bate em **`SIGABRT` = assertion dentro de
`FMallocBinned2::FMallocBinned2()+608`** (`check()`→FDebug::CheckVerifyFailedImpl→StackWalkAndDump).
Cadeia: **`FMallocBinned2::ctor → FMemory::Malloc → FMemory::GCreateMalloc → FMallocBinned2::ctor`
(RECURSÃO)**. O guard real bloqueava nessa re-entrância (= o deadlock original); o permissivo
deixa passar mas causa **DUPLA CONSTRUÇÃO** do allocator → a assertion dispara.
**3 CAMADAS testadas (todas → mesmo coração):**
1. guard real gnustl → **DEADLOCK** (futex) na re-entrância da MESMA thread.
2. `__cxa_guard` totalmente permissivo → passa mas **DUPLA-CONSTRÓI** FMallocBinned2 → assert
   em `FMallocBinned2::ctor+608`.
3. `__cxa_guard` estilo-bionic (re-entrância retorna "feito") → **SIGSEGV NULL em
   `FMemory::Malloc+0xc0` (0x4ad51c0)**: a re-entrância não constrói, então `GMalloc` segue
   **NULL** e `Malloc` o desreferencia → null-deref.
**RAIZ ÚNICA:** bootstrap recursivo `FMemory::Malloc → GCreateMalloc → FAndroidPlatformMemory::
BaseAllocator → FMallocBinned2::ctor → FMemory::Malloc` com `GMalloc==NULL`. No bionic isso
termina (atribui GMalloc antes do ctor alocar / usa malloc de bootstrap); no nosso ambiente não.
**Fix correto (sessão 2): tornar `FMemory::Malloc` (0x4ad5100) bootstrap-safe:** patchar/hookar
p/ que, enquanto `GMalloc==NULL`, use `malloc()`/`BinnedAllocFromOS` em vez de desreferenciar
NULL. Achar o global `GMalloc` (ldr no prólogo de FMemory::Malloc 0x4ad5100) e/ou inserir um
trampolim. O `__cxa_guard` estilo-bionic atual fica (evita o deadlock); o que falta é o bootstrap.
Flags de diagnóstico já no código (imports.gen.c): hooks de `mmap`/`sysconf`/`pthread_create`/
`syscall(futex)` + `__cxa_guard` permissivo (atualmente ATIVO — remove p/ voltar ao deadlock limpo).

### Endereços-chave (libUE4, offsets relativos ao text_base) — confirmados nesta sessão
- **Evento que a main espera (FUTEX_WAIT):** `libUE4+0xa5ac8a8` (FEvent global no .bss; val=0x10100).
- **Trampolim de thread genérico:** `libUE4+0x4a0950c` (`stp;mov;blr x0;mov x0,xzr;ret` — chama o functor em arg).
- **Corpo da thread worker (functor/arg):** `libUE4+0x4a097fc` = heartbeat do Trace (loop
  `usleep(24000)`→`ldrb [estado]`→dispatch). É a ÚNICA thread criada no init_array.
- `Trace::Private::Writer_Initialize` @ `0x4a0940c` (mmap do buffer de 384MB @ +0x15c; checa
  flag já-init @ +0x120 `tbnz`). Funções do Trace são ENTRELAÇADAS → patchar a entrada de uma
  não impede a outra de spawnar/esperar (por isso o patch Writer_Initialize→ret foi inócuo).

### Próximos passos (sessão 2) — para destravar a main
1. **gdb hardware watchpoint** no FEvent `libUE4+0xa5ac8a8`: relançar, `awatch *(long*)(BASE+0xa5ac8a8)`,
   `continue` — descobrir QUE função escreve/postaria esse evento e por que não é alcançada.
2. **Hookar `syscall`** (logar futex op no uaddr 0xa5ac8a8): ver se há WAKE perdido (lost-wakeup)
   ou se ninguém nunca faz WAKE. Se lost-wakeup → o FEvent do UE4 (sem-based) tem race no nosso
   timing; se nunca-WAKE → a thread que postaria não chega lá.
3. **Achar o caller que faz pthread_create+wait** (não é Writer_Initialize): bt do gdb na main
   com unwind manual, ou breakpoint em pthread_create e `bt`. Patchar ESSE caller p/ pular o
   handshake do Trace (ou forçar o FEvent a já vir "triggered").
4. Alternativa radical: desabilitar Trace por cmdline do UE4 (`-trace=` vazio) — achar como o UE4
   lê a cmdline no nosso android_shim e passar `-notrace`/`-tracehost=none`.

### Próximos passos (sessão 2)
1. Hookar `mmap`/`sysconf`/`sysinfo` e logar o que `BinnedAllocFromOS`/`GetConstants` pedem.
2. gdbserver no device + gdb no host (símbolos em libUE4.so não-stripado) → step no init_array.
3. Vencido o init: encarar a superfície JNI/GameActivity do UE4 + montar o OBB (AAssetManager
   real servindo a pak, ou path do OBB via jni_shim) — `/storage` só tem 936MB, o OBB (799MB) +
   libUE4 (183MB) não cabem juntos → precisa cartão maior ou OBB em outro mount.

## 5. ⛔ MURO ARQUITETURAL (render) — por que imagens no .164 são improváveis

A pak é **`NierM-Android_ETC2.pak`** e contém **só shaders `GLSL_ES3_1` + `SF_VULKAN`**
(zero `GLSL_ES2`) e texturas **ETC2**. O `libUE4.so` TEM o RHI `OpenGLES2` compilado
(`OpenGLES2.cpp`, `bBuildForES2`), mas **não há shaders ES2 cookados** e o Mali-450 (Utgard)
é **ES2-only, sem ETC2, sem ES3, sem Vulkan**. Forçar feature-level ES2 não acha shaders →
sem render. Isto bate com a regra do projeto: *UE4 = muro arquitetural p/ Mali-450*.

Para imagens de verdade haveria 2 caminhos, ambos grandes:
- **Tradutor runtime GLSL ES3.1→ES1.00** (intercepta glShaderSource) **+ decoder ETC2→RGBA**
  **+ emulação de entrypoints ES3** (VAO/UBO/glMapBufferRange) sobre ES2 — escala do compat
  layer inteiro do Dusklight, e os limites de uniforms/varyings do Utgard provavelmente não
  comportam o shader mobile do UE4.
- **Rodar no X5M (.103, Mali-G310, ES3.2)** — único device da frota que roda ES3.1 + ETC2
  **nativo**. É o caminho realista p/ ver NieR renderizar. (Hollow Knight ES3 já renderizou no X5M.)

## 6. Como rodar (diagnóstico)
```bash
# build (host)
bash build.sh                      # -> ./nier (ELF aarch64 PIE)
# deploy (sem OBB; binário+2 libs = 184MB, cabe em /storage)
scp nier libgnustl_shared.so libUE4.so root@<device-ip>:/storage/roms/ports/nier/
# rodar (device): para a ES, foreground, captura stderr
ssh root@<device-ip> 'systemctl stop emustation; timeout 60 bash /storage/roms/ports/nier/run.sh'
```
Device = root login, Amlogic Mali-450 fbdev, 832MB RAM, /storage 936MB livre.
`run.sh` seta `SDL_VIDEODRIVER=mali`, `LD_LIBRARY_PATH=$GAMEDIR`.

## 7. Legal — BYO
Repo = só loader/ferramenta. O APK/OBB do NieR é fornecido pelo usuário. **O APK do apkaward
está contaminado com trojan no dex** (não executado pelo so-loader, mas fique ciente).
