# Castlevania: Grimoire of Souls (jp.konami.castlevania) — Mali-450 .79 — STATUS

## 🧱 MURO FINAL sessão 1 — RecreateGfxState constrói java.lang.Error (intrínseco) e crasha
Confirmado: o crash (SIGSEGV libunity+0x31fafc, `r1=*(fp+0x108)=1; ldr[r1,#12]`) acontece em **AMBOS os paths de vídeo** (fbdev E egl_shim/kmsdrm), logo no início do `nativeRecreateGfxState`, ao construir um `java.lang.Error`+StackTrace (FindClass Error/StackTraceElement, GetMethodID <init>/setStackTrace, NewStringUTF Class/Method/File). NÃO é falha de gfx nem exceção C++ (`__cxa_throw` não dispara) — é o init do **log/crash-handler da Unity (AndroidJNIHelper C++)** rodando com nosso **JNIEnv fake** e derefenciando um objeto que vale 1. Tentativas que NÃO resolveram (env-gated, default OFF): `CVGOS_CRASHSKIP` (sobrevive ~5, morre na construção aninhada), `CVGOS_SAFEOBJ` (NewObject/NewObjectArray/GetObjectArrayElement→buffer zerado; crash é ANTES do NewObject), `CVGOS_SKIPERRFUNC` (no-op de libunity+0x31fa08 PIORA — a func faz trabalho necessário). Função do crash começa em **libunity+0x31fa08** (push {r4-fp,lr}); caller em ~0x31fcf8; ambos no subsistema de exceção/thread 0x31f... (mesma família do trap-gate).
- **RAIZ FINA (gdb single-step libunity+0x31fa08)**: `mov fp,r0` no prólogo → **fp é o `this` (objeto Unity C++), NÃO frame pointer**. A func lê `[this+0x108]` (=um handle JNI que NOSSO shim retornou e a Unity cacheou no objeto, ex 0x509d4/1) e faz `[handle]`→`[+12]`→`[+24]`→`blx` (dispatch de método via vtable estilo objeto Java). Nossos handles fake (int pequeno OU ponteiro sem layout) não têm o **grafo de objeto** que a Unity C++ espera → deref inválido → SIGSEGV. É o wrapper JNI-object do `AndroidJNIHelper`: exige que jobject/jclass tenham `[obj]`=classe/vtable, campos em offsets. **Overhaul multi-sessão do jni_shim**: dar aos handles fake um layout de objeto derefenciável (vtable zerada + campos) OU mapear a classe específica (java.lang.Error/StackTraceElement) com storage real.
- **ACHADO+FINO (nm do binário)**: `[this+0x108] = 0x509d4 = &pthread_mutex_lock_fake` (nossa função!). A func 0x31fa08 faz `cmp field_108,#0` (0x31fa20) e se NÃO-null deref como objeto → crash. Ou seja: um campo do objeto Unity que deveria ser **NULL** (ou objeto válido) contém o ENDEREÇO da nossa `pthread_mutex_lock_fake`. Provável: (a) **allocator do Unity não zera** a memória do objeto e sobrou lixo (&pthread_mutex_lock_fake de uso anterior) → campo != 0 → entra no path de crash; ou (b) construtor do objeto não completou a init desse campo (passo dependente de JNI falhou silencioso). AÇÃO próxima sessão: (1) garantir que operator-new/calloc/malloc do so-loader ZEREM (ou hookar o alloc do Unity p/ memset 0) — se field_108 for 0, o `cmp` pula o crash; (2) OU tracar quem escreve &pthread_mutex_lock_fake ali (watchpoint no field). Isso conecta com o pthread_fake (bionic mutex 4B vs glibc 40B) — pode haver overflow de mutex embutido no objeto corrompendo campos vizinhos.
- **PRÓXIMO (multi-sessão)**: tornar o JNIEnv fake FIEL o bastante p/ o AndroidJNIHelper C++ construir o java.lang.Error+StackTrace sem crashar. Passos: gdb FIXBASE (libunity@0x30000000) → break libunity+0x31fa08 → single-step até o 1º deref inválido, ver qual campo/objeto (jclass/jmethodID/jobjectArray) a Unity espera válido; dar semântica real a esse handle no jni_shim (provável: NewObjectArray real com storage indexável + GetArrayLength coerente + SetObjectArrayElement que guarda; ou o jclass de StackTraceElement precisar de layout específico). Alternativa: achar e stubar a função Unity que dispara esse log-init (RegisterLogCallback/InitCrashHandling) SEM quebrar o resto.

## ⭐ ATUALIZAÇÃO sessão 1 (tarde/noite) — 2 BREAKTHROUGHS, path natural anda até RecreateGfxState
1. **trap-gate** (libunity+0x31fe38): `nativeRender` baila se `[player+0x104]!=0` → causa da tela preta (NÃO o GC).
2. 🎯 **RAIZ do trap = SIGHUP!** "main thread trapped; signum=1" = a Unity grava o SINAL (1=SIGHUP) que trapou a thread em `curthread->trap`; o gate propaga. **FIX: ignorar+bloquear SIGHUP** (`signal(SIGHUP,SIG_IGN)`+sigprocmask+my_sigaction recusa sig1) → **trapped 175684×→0**, gate retorna 0 NATURALMENTE (sem forçar, sem corrupção). Ligar via default (CVGOS_NOSIGHUPIGN desliga).
3. **Path natural agora**: boot → trap=0 → initJni OK → `nativeRecreateGfxState` roda de verdade e **constrói um `java.lang.Error`+StackTrace** (FindClass Error/StackTraceElement, NewStringUTF Class/Method/File) — a Unity está LOGANDO uma exceção via Java. **Crash (SIGSEGV 139) na máquina C++ de exceção/log**: libunity+0x31fafc `ldr r1,[fp+0x108](=1); ldr r1,[r1,#12]` — local=1 vira ponteiro. `CVGOS_CRASHSKIP=1` sobrevive ~5 skips mas a construção aninhada acaba matando.
- **PRÓXIMO**: (a) descobrir QUE erro a Unity está logando no RecreateGfxState (hook `__cxa_throw`/ler a msg do `java.lang.Error`, ou o que falha no gfx state) — pode ser um check de GL/config; (b) OU deixar a construção do stacktrace-Java robusta (NewObjectArray→buffer válido, NewObject→dummy, SetObjectArrayElement no-op) p/ o log completar e o RecreateGfxState seguir. Provavelmente a exceção é diagnóstico não-fatal → se completar o log, init continua.
- envs atuais no run.sh: `CVGOS_NOTRAPPATCH=1` (path natural, não força gate), `CVGOS_CRASHSKIP=1`, `CUP_NOSIGH=1`, `SDL_VIDEODRIVER=mali`, `CUP_VIDEO=kmsdrm`, `CUP_GLES_MAJOR=2`, `CUP_1CORE=1`.

## Estado (2026-07-01, sessão 1)
Port armv7 Unity 2018.4.11f1 IL2CPP **do zero**, base = glue Unity do Terraria + loader
ELF32-ARM/softfp do Shantae (ambos JOGÁVEL). **Boot completo + present no fb0 funcionando**;
muro atual = **conteúdo (cenas/addressables) não carrega → tela preta**.

### ✅ Funciona
- Build armhf → ELF32 ARM (`build.sh`, toolchain `armv8a-emuelec-linux-gnueabihf`).
- so_load libunity.so + libil2cpp.so (ELF32-ARM, relocs REL implicit-addend; GLOB_DAT/JUMP_SLOT sem somar in-place).
- init_array (486 unity + 45 il2cpp), JNI_OnLoad, 32 native methods (initJni/nativeRender/…).
- initJni (lê `bin/Data/boot.config` via AAssetManager shim) + nativeRecreateGfxState + nativeResume + nativeFocusChanged(1).
- **PRESENT no fb0 OK**: `CUP_VIDEO=kmsdrm` → egl_shim (SDL_CreateWindow + SDL_GL_SwapWindow), `SDL_VIDEODRIVER=mali`, contexto **ES2** (`CUP_GLES_MAJOR=2`). Confirmado: fb0 sai do menu ES → preto (engine limpa a tela).
- Render loop estável (>136k frames), ~7500fps (cena vazia = frames instantâneos).

### 🔧 Fixes-chave desta sessão
- so_util.c: porte completo ELF64/AArch64 → ELF32/ARM (multi-módulo so_save/so_use preservado).
- main.c: crash handler armv7 (`arm_r0..arm_pc` via `mc_reg`), trampolines arm64 stubados.
- **Números de syscall**: header ARM já dá corretos (futex=240, gettid=224, rt_sigprocmask=175) — `#ifndef` não dispara. OK.
- bionic_shims.c: `pthread_cond_timedwait_relative_np` real (relativo→absoluto); `sigsetjmp`→`__sigsetjmp` via patch_got (setjmp não pode ser wrapper).
- run.sh: ASSET_BASE → `/storage/roms/ports/cvgos/bin/Data`; libs no CWD; `CUP_NOLOGFILE=1`; `CUP_FRAMES=999999999` (era CV_FRAMES, errado).

### 🧱 MURO: conteúdo não carrega (tela preta)
- Só abre `bin/Data/boot.config`. **NENHUM** `.ab`/OBB/level0/catalog/scene aberto depois.
- 6 threads só (sem UnityMain/Loading.Preload/GC/job-workers separados) → PlayerLoop roda inline na main; nativeRender retorna instantâneo (cena vazia).
- gdb main thread bt = `main → fsync → nativeRender` em loop (não travado; só girando vazio).
- "main thread is trapped; signum=1" (602×/run) = **GC stop-the-world do il2cpp** (benigno; `CUP_GCOFF=1` PIORA → crash em libc tgkill/pthread_kill). NÃO é o problema.

### 🔬 RAIZ LOCALIZADA (strace do binário, sessão 1 fim)
- Fix aplicado: `jni_shim.c` tinha 3 paths hardcoded `/storage/roms/terraria` (ASSET_BASE + dataPath + userdata) → corrigidos p/ cvgos. Boot.config agora lê do path certo. **MAS sintoma igual.**
- **strace decisivo**: após `bin/Data/boot.config`, o processo SÓ reabre boot.config + `debug.log` em loop. **`global-metadata.dat` NUNCA é aberto** (grep strace inteiro = 0). level0/`.ab`/OBB idem.
- Conclusão: **`il2cpp_init` (runtime C#) NUNCA é chamado** — o engine (libunity) faz initJni→RecreateGfxState→nativeRender(loop vazio) mas **nunca dispara o scripting backend / 1ª cena**. Sem C#, sem conteúdo, preto.
- Causa provável: a sequência de boot da base (main.c) é do **Cuphead/Terraria = Unity 2021**; **CVGoS = Unity 2018.4** tem fluxo de init diferente p/ arrancar o il2cpp/PlayerLoop. Terraria (mesma base) RODA C#, então o mecanismo existe — falta o gatilho certo p/ 2018.4.

### 🚀🚀 BREAKTHROUGH (sessão 1, tarde) — trap-gate + engine RODA + UnityMain spawna
Achado via gdb desmontando `nativeRender`: ele chama um GATE em **libunity+0x31fe38** que lê **`[player+0x104]`**; se != 0 → LOGA "main thread is trapped; signum=%i" e **`nativeRender` BAILA** (pula PlayerLoop/scripting/1ª cena). Esse gate — NÃO o GC — era a causa da tela preta (o "trapped" NÃO é benigno). O campo fica em **1 (signum=1)** e trava tudo.
- **Patch trap-gate** (`main.c`, default ON, `CVGOS_NOTRAPPATCH` desliga): força libunity+0x31fe38 → `mov r0,#0; bx lr`. Com isso **nativeRender ENTRA no trabalho real**: spawna a thread **UnityMain**, consulta PackageManager/ApplicationInfo/metaData/VR mode, roda init do runtime.
- Fixes de suporte que vieram junto: **EHFIX** (resolve `_Unwind_*`/`__cxa_*`/`__aeabi_unwind_*` reais nos 2 libs — eram stub → exceptions C++ matavam); **getString** metaData(VR/samsung/oculus) ausente → NULL (era va_arg lixo "4}"); **stdio guard** (`my_fwrite`/etc. FILE* desalinhado→stderr, wired); **on_crash SIGBUS-skip** (`CVGOS_NOSIGBUSSKIP` desliga) + **`CUP_NOSIGH`** (nosso on_crash pega o SIGBUS em vez da Unity).

### 🧱🧱 MURO ATUAL (sessão 1, fim real) — forçar o gate roda com estado corrompido
Com o trap-gate forçado, a UnityMain roda o init MAS usa **FILE\* lixo FIXO 0xe3520001** (bionic stdio) → glibc `ldaex [&FILE->_lock]` desalinhado → **SIGBUS**, seguido de **cascata** (SIGBUS/SIGILL recorrente em libc, ldaex/ldrex em ponteiros lixo). O SIGBUS-skip pula alguns mas a cascata continua (estado corrompido, não 1 bug isolado).
- **Raiz**: `[player+0x104]=1` NÃO é setado por sigaction (`CUP_NOSIGH` não impede — 175684× sem o patch). É código próprio da Unity marcando a thread como "não-pronta/trapped". **Forçar o gate = rodar PlayerLoop numa thread que a engine considera não-registrada → estado uninit → ponteiros lixo (FILE\* etc.) → cascata.**
- **PRÓXIMO PASSO decisivo**: gdb **watchpoint** em `[player+0x104]` (r4 no gate libunity+0x31fe38) p/ achar QUEM escreve o 1 (o "SetThreadTrapped") e por quê; corrigir a RAIZ (registrar a UnityMain / satisfazer a condição) em vez de forçar o gate. Provável ligação com registro de thread no runtime (classe do muro Terraria/FF9). signum=1=SIGHUP — investigar se um SIGHUP real chega (nohup deveria ignorar) ou se 1 é sentinela "uninit".

### 🔬🔬🔬 ROOT REFINADO via gdb (sessão 1, fim) — o trap PROPAGA de um "current thread" GLOBAL
Desmontando o gate (libunity+0x31fe38) no gdb:
```
push {r4,lr}; mov r4,r0
ldr r3,[r4,#0x104]; cmp r3,#0; beq +0x28   ; se [r4+0x104]!=0 -> LOGA "trapped signum=[r4+0x104]"
+0x28: ldr r0,[GLOBAL_curthread]; cmp r0,#0; cmpne r0,r4; beq +0x64
       ldr r3,[r0(curthread)+0x104]; str r3,[r4+0x104]   ; COPIA trap do curthread global -> r4
...return
```
- **No 1º gate call `[r4+0x104]=0`** (player r4 NÃO nasce trapado). O gate então **COPIA `[GLOBAL_curthread+0x104]` para `[r4+0x104]`** — ou seja o trap vem de um **objeto "current thread" GLOBAL** (ptr em libunity data, offset fixo carregado via `ldr r0,[pc,#100];ldr r0,[pc,r0]` em gate+0x28/+0x2c). Watchpoint no r4 (0xf5a19500) NÃO disparou (fonte errada) mas "trapped" logou 175k× → confirma que a fonte é o **curthread global**, não o r4.
- **PRÓXIMO (preciso)**: achar o endereço do objeto curthread global (ler r0 em gate+0x30) e dar **watchpoint em `[curthread+0x104]`** desde cedo (ou achar o offset do ponteiro global em libunity data) p/ pegar o SETTER do 1. Aí: entender a condição (registro da thread no runtime Il2Cpp) e satisfazê-la, OU zerar o curthread+0x104 no boot (menos correto). Helper de debug: **`CVGOS_FIXBASE=1`** mmapa libunity em 0x30000000 (determinismo p/ breakpoints).
- Ferramentas gdb que funcionaram: `break jni_dump_natives` (após libunity carregar) → `break *(g_unity_base+0x31fe38)` (g_unity_base é símbolo do nosso binário não-stripado).

### 🔬🔬 MURO REFINADO (sessão 1, meio) — libunity não entra na fase player-run
Verificado que o my_dlsym da base JÁ resolve `il2cpp_*` do nosso g_m_il2cpp (wired em libunity via set_import+patch_got). PORÉM no run **NÃO há NENHUM `[DLOPEN]`/`[DLSYM] il2cpp`** — ou seja **libunity NUNCA chama dlopen("libil2cpp")/dlsym("il2cpp_init")**. Combinado com **só 6 threads (sem UnityMain/GC/Loading/workers)** e nativeRender a ~7500fps (instantâneo): **libunity inicializa o gráfico (initJni/RecreateGfxState/present) mas NUNCA entra na fase de "run application" / scripting-backend init**. Sem isso: sem UnityMain, sem il2cpp_init, sem 1ª cena → preto.
- Lifecycle testado igual RE4 (Resume→SurfaceChanged→Focus) — não arranca.
- Provável: falta o gatilho de `UnityInitApplicationNoGraphics`/`PlayerInitEngine`/criação da UnityMain que a Unity 2018.4 espera (talvez um native method a mais, um evento Java, ou chamar a init explicitamente). Precisa **RE do libunity**: achar onde il2cpp_init/PlayerLoadFirstLevel deveria ser chamado e por que o path não é alcançado (disassembly a partir de nativeRender/initJni; comparar com o boot do RE4 Mono que CHEGA a rodar C#).

### 🎯 Hipóteses p/ próxima sessão (foco: fazer il2cpp_init/1ª cena rodar)
0. **[NOVO, prioridade máxima] Disparar o scripting/scene-load do Unity 2018.4**: comparar a sequência de boot que o Terraria (Unity 2021, JOGÁVEL) usa vs o que 2018.4 precisa. Ver se falta: (a) um `nativeSendSurfaceChangedEvent` com surface válida/resize, (b) chamar explicitamente o `PlayerInitEngineGraphics`/`LoadFirstScene`/`il2cpp_init` de libunity, (c) um passo de lifecycle Java (executeGLThreadJobs). Confirmar via strace que `global-metadata.dat` passa a abrir. Hookar/logar `il2cpp_init` (exportado em libil2cpp @0x79dcc4) p/ ver se é chamado.
1. **Addressables/OBB não encontrado**: jogo é addressables (2631 .ab no OBB `main.110`). O bootstrap (level0) provavelmente carrega o **catálogo** do OBB no path Android (`getObbDir()`/`Application.dataPath`+obb) e não acha → coroutine espera p/ sempre → sem cena. AÇÃO: ver que path o jogo pede (hookar getObbDir/getPackageCodePath no jni_shim; logar opens de `catalog*`/`.bundle`/`.hash`); apontar/симlink o OBB (ou extrair os .ab) pro path esperado. OBB já está em `/storage/roms/ports/cvgos/obb/jp.konami.castlevania/main.110...obb`.
2. **Online-gate**: GoS é online (servers mortos). Bootstrap pode travar em Firebase(`libFirebaseCppApp`)/GPG(`libgpg`) init → stub. Ver se há socket/connect ou espera de init de rede.
3. **il2cpp não roda C#**: confirmar que o bootstrap C# (level0 Awake/Start) executa — hookar um método conhecido ou logar il2cpp_runtime_class_init. Se level0 nem carrega, achar o auto-load da 1ª cena.

### Config atual (run.sh)
`SDL_VIDEODRIVER=mali`, `CUP_VIDEO=kmsdrm`, `CUP_GLES_MAJOR=2`, `CUP_1CORE=1`, `CUP_FRAMES=999999999`, `CUP_NOLOGFILE=1`. NÃO usar `CUP_GCOFF` (crasha).

### Deploy
Device .79 (EmuELEC Mali-450 fbdev). `/storage/roms/ports/cvgos/`: cvgos(bin), lib/(9 .so), bin/Data/(APK data), obb/. Rodar: matar cvgos → `nohup bash run.sh`. fb 1280x**1440** (double-height); validar em `[0:720]` do /dev/fb0 (32bpp BGRA). Não versionar binário/libs.
