# Castlevania: Grimoire of Souls (jp.konami.castlevania) — Mali-450 .79 — STATUS

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
