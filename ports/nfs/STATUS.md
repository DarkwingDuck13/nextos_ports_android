# NFS Most Wanted (2012) → NextOS Mali-450 (so-loader armhf)

## 🔬 SESSÃO 2026-06-13 PARTE 4 — RE com CAPSTONE: path exato capturado, lookup do archive quebrado
Capstone (5.0.7) está instalado. Escrevi `tools_armdis.py` (desmonta ARM resolvendo refs PIC a
strings via ldr[pc]+add pc) e um scanner de xref próprio. Achados:
- **`SKU::GetFileSystemPath`** = 0x3fba3c (ARM): exige path começando com **`@`** (0x40) senão
  "Invalid path"; depois compara com "published" e aplica SKU. **MAS hookada (NFS_FSPATHLOG) ela
  NUNCA é chamada p/ os databases** — não é o caminho de resolução deles.
- **Função database-open = 0x4f0138** (ARM; r1=path como {begin,end}, r2=flags). Faz 2 chamadas
  virtuais de open no singleton VFS (`bl 0x40e8e8` → vtable[6]@0x18 em 0x4f0294, depois vtable[2]@8
  em 0x4f03d0); se ambas retornam NULL → "Could not open database at %s".
- **HOOK de 0x4f0138 (NFS_FSPATHLOG) capturou o PATH EXATO**: `/published/data/locales.sb`,
  `/published/fonts/fonts.sb`, etc. (com barra inicial).
- **TESTES QUE DESCARTAM forma do path** (NFS_STRIPSLASH reescreve o range {begin+1,end}):
  com `published/data/locales.sb` (nome EXATO da entrada do OBB, SEM barra) o VFS open AINDA retorna
  NULL. Já testado tb: com barra (mount /published), prefixo SKU published.1x (repack merged),
  base published. NENHUM resolve. → **o lookup do índice do archive está quebrado** (a engine LÊ
  todos os nomes do central dir mas o open por nome falha p/ TODOS, mesmo nome exato; nunca faz seek
  baixo p/ ler conteúdo). Causa provável: índice/hash interno do objeto archive não populado/
  inconsistente sob nosso ambiente (possível HASH de nome — ctor 186 = CPU detect p/ CRC32; OU
  container std falhando), OU o VFS open precisa de um filesystem/mount que não foi registrado.
- **PRÓXIMO**: hookar o método VFS open (vtable[6]@0x18 do singleton de 0x40e8e8) p/ ver a busca
  interna e se o índice tem entradas; OU achar como o archive é registrado no VFS ("Mounting SKU"
  só mapeia, mas o FS do OBB precisa estar montado). Ferramentas: tools_armdis.py, scanner xref,
  hook genérico (my_getfspath, NFS_FSPATHLOG aponta p/ 0x4f0138), NFS_STRIPSLASH, NFS_OBBDUMP.
  Decompiler (Ghidra) destravaria rápido. Engine boota 100% — só não acha os dados.

---

## 🔬 SESSÃO 2026-06-13 PARTE 3 — DIAGNÓSTICO PRECISO DO MURO DO OBB (lookup interno)
Investigação profunda do mount do OBB. ACHADOS DEFINITIVOS:
- A engine lê o EOCD + **central directory INTEIRO** (todas as 2411+ entradas, byte-a-byte) e indexa
  TODAS — confirmado via dump (my_fread NFS_OBBDUMP=1): lê `published.1x/texturepacks...`,
  `published.2x/`, `published.4x/`, `published/CC_SeedData.bin`, `published/ParticleBaseTextures/...`
  etc. **O índice está COMPLETO.**
- **A engine NUNCA faz seek p/ offset baixo** (= nunca lê o conteúdo de nenhum arquivo). Todos os
  seeks ficam no central dir (~623M/731M). Logo o **lookup nome→entrada FALHA** antes de qualquer read.
- **REPACK TESTADO E DESCARTADO** (provou que NÃO é layout): mesclei o base `published/*` (1144 arq,
  dirs pequenos) sob `published.1x/*` no OBB (append + rsync delta; OBB no device agora 731MB com AMBOS
  `published/data/locales.sb` E `published.1x/data/locales.sb`). A engine LÊ a entrada nova
  `published.1x/data/locales.sb` no índice (confirmado no dump), MAS o lookup de
  `/published/data/locales.sb` AINDA falha. → A resolução de `/published/X` NÃO gera `published/X` NEM
  `published.1x/X` — gera uma chave que não casa NENHUMA entrada do índice. **É a `SKU::GetFileSystemPath`
  (0x3fbac0; xref achado via scanner PIC próprio) gerando uma chave interna diferente.**
- Ferramentas: scanner PIC ARM próprio acha xrefs a strings (objdump não anota movw/movt). GetFileSystemPath
  em ~0x3fbac0, mount loop ~0x3fb778, AddSKU ~0x3fae7c. binário STRIPPED (1 símbolo bogus, sem fronteiras
  de função) → RE manual inviável sem decompiler.
- **PRÓXIMO (precisa de Ghidra/IDA ou hook runtime):** (1) decompilar SKU::GetFileSystemPath p/ ver a
  chave gerada de "/published/X"; OU (2) hookar a função de lookup/open-in-archive e logar a chave que ela
  busca → adicionar a entrada com ESSE nome no OBB OU reescrever a chave no hook. Possível que a chave seja
  relativa ("data/locales.sb" sem prefixo) ou um HASH. Diagnóstico runtime pronto: NFS_OBBDUMP (dump de
  reads/nomes do OBB via g_obb_fp), NFS_SEEKLOG, NFS_FOPENLOG. OBB merged (731MB, com ambos os paths) está
  no device — útil p/ próximos testes. **NÃO é regressão: engine boota 100%, só não acha os dados.**

---

## 🟢🟢 SESSÃO 2026-06-13 PARTE 2 — ENGINE BOOTA 100% + chega ao CARREGAMENTO DE DADOS (OBB)
Depois dos 4 fixes (abaixo), mais 1 fix destravou o init gráfico inteiro:
5. **JNI_OnLoad dos módulos SECUNDÁRIOS** (main.c): no Android o runtime chama JNI_OnLoad de CADA
   .so (System.loadLibrary). Só chamávamos o do libapp. **libNimble.so** (bridge JNI da EA) cacheia
   o JavaVM no SEU JNI_OnLoad; sem chamar, g_vm=NULL → `EA::Nimble::getEnv` deref NULL → SIGSEGV no
   texto do libNimble (a região anon r-x 12K do crash pós-LoadExtensions = libNimble, identificado
   via strings da região). Fix: iterar g_comb e chamar JNI_OnLoad de cada secundário ANTES do libapp.

**ESTADO ATUAL (enorme):** engine boota 100% → shadergen → OpenGLES20+LoadExtensions → libNimble
InitNimble/SetLanguage/SLG3 → **AttachCurrentThread + carregamento de dados do jogo** (Tier=Low
detectado ✅). **600+ frames SEM crash (EXIT=0).** AINDA SEM IMAGEM porque trava no **carregamento
de assets do OBB**, ANTES do 1º eglSwapBuffers.

**MURO ATUAL = MOUNT DO OBB (não é crash):** A engine abre o OBB, lê o EOCD + central directory
(ZIP, 2411 arquivos, parse byte-a-byte funciona — seek p/ central dir em ~623120127), faz
`[trace] AddSKU: 1x` + `[trace] Mounting SKU: 1x to /published`. Mas TODOS os lookups falham:
`Could not open database at /published/{data/locales.sb,fonts/fonts.sb,layouts/layouts.sb,
flow/default.sb,sounds/music/playlists.sb}` → `[fatal] ScreenFactory: Cannot create object NULL`.
**CAUSA:** o OBB tem `published/` (base, 1514MB: data/fonts/layouts/flow/sounds/stringdata/models/
textures...) E `published.1x/.2x/.4x` (texturas: 48/141/295MB). A engine monta SÓ o SKU
`published.1x`→`/published` (overlay de texturas), mas os databases estão em `published/` (base),
que **nunca é montado**. No Android a engine monta base `published/`→`/published` E sobrepõe o SKU.
Confirmado: todos os arquivos que falham EXISTEM no OBB em `published/X`; `published.1x/fonts/fonts.sb`
NÃO existe. **`SKU::GetFileSystemPath`** (sem símbolo; strings 0x9e45bf "Mounting SKU: ", 0x9e45ff
"published", 0x9e45d0 "Invalid path given to SKU::GetFileSystemPath") faz o mapeamento.
**Tentado e DESCARTADO:** useAssetsFileSystem=1 (NFS_USEASSETSFS — piora, tenta AAssetManager/crash);
hooks mmap/mprotect/dlopen (a engine não os usa p/ o OBB; removidos). **PRÓXIMO (2 caminhos):**
(A) RE da SKU::GetFileSystemPath com decompiler (Ghidra/IDA — sem símbolo, strings via movw/movt, não
dá p/ xref por grep) p/ ver por que o base não monta / forçar mount do base; talvez hookar a função
de mount p/ adicionar o base; (B) WORKAROUND repack: mesclar `published/*` dentro de `published.1x/*`
no OBB (+drop 2x/4x) → SKU mount acha tudo (~1.56GB, repack store + transfer). Possível tb HASH dos
nomes (ctor 186 = CPU detect p/ CRC32 HW; ver getauxval shim) se o índice usar hash. Device S905X tem
CRC32 ARMv8. **Hipótese de hash: improvável** (parse byte-a-byte do índice funciona). Flags úteis:
NFS_FOPENLOG/NFS_SEEKLOG (padrão de IO do OBB confirma EOCD+central dir lidos certo).

---

## 🟢 SESSÃO 2026-06-13 (device .164 nextos/Mali-450) — MURO DO SHADERGEN QUEBRADO + 4 FIXES DE CAUSA-RAIZ
O muro de 3+ sessões (crash no `dynamic_cast` do `im::isis::shadergen`) era **BUG DE
RELOCAÇÃO no so_util**, não RTTI corrompido. Sequência de fixes (todos commitados):

1. **🔑 R_ARM_ABS32 descartava o ADDEND in-place** (`so_util.c`). ABS32 é REL: `*ptr = S + A`,
   com A = valor gravado in-place. `so_resolve` fazia `*ptr = func` (sem +A). Ex: `type_info[0] =
   _ZTVN10__cxxabiv120__si_class_type_infoE + 8` (o "8" é o addend) → TODO ponteiro de vtable de
   type_info ficava **-8** → o dispatch virtual do libcxxabi (`__do_dyncast`) lia lixo → crash.
   Maioria dos ABS32 tem A=0 (por isso bootava até o RTTI). Fix em 2 pontos: (a) `so_relocate`
   PULA UNDEF no ABS32 (senão somava text_virtbase ao addend antes do resolve); (b) `so_resolve`
   captura o addend in-place e soma p/ ABS32 (tabela + dlsym). **AFETA TODOS OS SO-LOADERS deste
   so_util** — bug genérico. Diagnóstico: DC-ENTRY log mostrou `kind=si-8` (off-by-8 exato).
2. **getenv race** (`imports.c`): `my_dynamic_cast` chamava getenv 4x/cast num loop RTTI; a engine
   roda `setenv` em worker thread (realoca+libera o array `__environ`) → getenv concorrente lia
   array morto → SIGSEGV em getenv. Fix: cachear flags NFS_* uma vez (`nfs_cache_flags`).
3. **pthread bionic↔glibc** (`pthread_shim.c` NOVO): bionic mutex/cond/sem ~4B, glibc 24/48/16B →
   a engine alocava no tamanho bionic, glibc acessava além + ldrd desalinhado → SIGBUS em
   `pthread_cond_wait`. Fix: side-table (endereço bionic→objeto glibc real, lazy). Mutex/cond/
   rwlock/once/sem. Mutexes RECURSIVE.
4. **__sF stride 0x54** (`imports.c`): bionic `__sFILE`=84B; a engine faz `fflush(__sF+0x54)`
   (=stdout). Nosso `bionic_sF[3][512]` (stride 512) → `map_sF` não casava → fflush(FILE* lixo)
   → SIGSEGV. Fix: stride 0x54 + offsets 0/0x54/0xA8.

**ESTADO ATUAL:** engine boota → init_array → JNI/GameActivity → OBB → **shadergen RTTI 100% OK
(centenas de dynamic_cast)** → fontes/Paint (jni_shim) → allocator EA (NimbleWrapper Init/
FinishInitialization/InitNimble) → **`[Graphics] OpenGLES20::OpenGLES20()` + `OpenGLES20Ext::
LoadExtensions()` (init GRÁFICO!)**. **MURO ATUAL:** após LoadExtensions, crash sig=11 addr=nil
(r0=r1=0, r2=0xffffffff) com **PC numa região ANON r-x de 12KB** (endereço randomiza por ASLR:
f4e0ff2c/f4e59f2c/f4ec7f2c +0xf2c). Chamado do código gráfico (stack: libapp+0xa129de ← 0x96acd5
← 0x97a633 ← 0x96ebb5 ← 0x7bbe78...). Hipótese: ponteiro de função GL (eglGetProcAddress de uma
extensão) resolvido errado/stub OU JIT de shader do Mali. **PRÓXIMO:** identificar a região anon
(re-rodar + cat /proc/PID/maps), desmontar libapp+0xa129de (vê qual fn-ptr chama), checar
egl_shim_GetProcAddress (retorna NULL/stub p/ extensão? r2=0xffffffff=sentinela "not found").
MAPS no crash: a região do PC é uma anon r-x ISOLADA de **12K** (ex f4ebc000-f4ebf000), entre 2
anon grandes; libMali.so é file-mapped (f6fb8000); nossos módulos (libapp/libc++/etc) são anon
RWX MB-sized. Stack: `[sp+0xc] (anon)+0xf6d` (retorno DENTRO da anon) ← `[sp+0x18] libapp+0xa129de`.
Hipótese forte: a engine resolveu uma fn GL não suportada pelo Mali-450/Utgard via eglGetProcAddress,
o driver devolveu um STUB (na anon 12K) que crasha ao ser chamado (r0=r1=0). Ver libapp+0xa129de
(THUMB? objdump -d simples veio vazio no range — achar o símbolo). Crash handler agora dumpa
"--- regiões r-x ---" (com <<< PC) p/ facilitar. NFS_NORECOVER=1 p/ crash limpo.
Walker próprio de dynamic_cast existe gateado em `NFS_DCWALK` (não usado — relocs corretas bastam).
Rodar: `LD_LIBRARY_PATH=/usr/lib32:. SDL_VIDEODRIVER=mali NFS_INIT=1 ./nfs`. Binário+libs+OBB já
no device .164 em /storage/roms/nfs/.

---

**Jogo:** Need for Speed Most Wanted, `com.ea.games.nfs13_row` v1.3.128 (EA/Firemonkeys/Ironmonkey).
**APK:** `~/Downloads/need-for-speed-most-wanted-v1.3.128.apk` (19.5MB).
**OBB/dados:** `~/Downloads/need-for-speed-most-wanted-v1.3.128-cache-apkvision.zip` → `main.1003128.com.ea.games.nfs13_row.obb` (623MB).

## ✅ Análise de viabilidade (PROMISSOR)
- **GLES2** (libGLESv2 + libEGL, zero GLES3) → passa no filtro nº1 do Mali-450 Utgard. 🟢
- **ABI armhf** (armeabi-v7a, ELF32 ARM, NDK r18) → caminho 32-bit do so-loader (igual GTA CTW/VC, runtime /usr/lib32). 🟢
- **Entry = JNI_OnLoad** em libapp.so + Java `com.ea.ironmonkey.GameActivityMain.nativeOnCreate` → padrão GameActivity+JNI (dominado no DYSMANTLE/Bully). 🟢
- **Risco real = PERFORMANCE** (open-world 3D de 2012 num Mali-450 fraco). Só o teste dirá. 🟡

## Módulos (multi-módulo, igual DYSMANTLE)
- `libapp.so` (11MB) = ENGINE principal (JNI_OnLoad aqui)
- `libNimble.so` (13KB) = bridge JNI pequeno (GameActivity)
- `libfmodex.so` + `libfmodevent.so` = áudio FMOD (middleware comercial)
- `libc++_shared.so` = STL
- NEEDED: libc++_shared, libfmodex, libfmodevent, libGLESv2, libEGL, liblog, libjnigraphics, libNimble, libc/m/dl

## Recon de símbolos (new-port-arm.sh)
- 540 importados (UND), **352 auto-resolvidos (65%)**, 188 a implementar.
- 145 GLES, 48 pthread, 128 passthrough, 30 cxx/abi/log.
- **188 UNKNOWN = maioria libc/posix trivial** (dlopen/dlsym/clock/chmod/fork/feof/ferror/getaddrinfo/getauxval/...) → passthrough glibc.
- Específicos a tratar: `AndroidBitmap_*` (libjnigraphics), `FMOD_*` (áudio), dlopen/dlsym (nossa versão do loader).

## Roadmap (fases, estilo DYSMANTLE)
- [x] **F0 — build:** loader armhf (so_util REL do gtavc-build) + wire dos 188 (libc→glibc), compila.
- [x] **F1 — load:** libapp+libc++_shared+libNimble+FMOD multi-modulo, so_snapshot_symbols+fallback dlsym, 0 UNRESOLVED, JNI_OnLoad+nativeOnCreate achados. ✅ carrega libapp+libNimble+deps, relocs OK, JNI_OnLoad roda sem crash.
- [ ] **F2 — boot JNI:** replicar GameActivityMain.nativeOnCreate + JNI env (igual DYSMANTLE GameActivity struct). Pegar o init da engine.
- [ ] **F3 — GLES2/EGL:** EGL→SDL2 Mali fbdev, contexto GLES2, primeiro frame.
- [ ] **F4 — dados:** montar o OBB (623MB) no path que a engine espera.
- [ ] **F5 — áudio:** FMOD (rodar real ou bridge).
- [ ] **F6 — render/perf:** otimizar p/ Mali-450; controle USB; empacotar.

## Notas/pegadinhas (da experiência)
- ⚠️ Canary bionic tpidr+0x28: pad TLS 256B `_Thread_local` no exe (vale p/ todo so-loader). Ver [[DYSMANTLE]].
- ⚠️ armhf: runtime /usr/lib32 (glibc 2.43 32-bit + libMali.m450 + SDL2), launcher PORT_32BIT=Y. Kernel 3.14 sem time64 → shim nextclock.so (time32) como no GTA CTW.
- ⚠️ Buildar sempre da pasta do port. ES mascarado p/ testes.
- so_util armhf: `~/gtavc-build/loader/so_util.c` (REL relocs).

## F2 (boot JNI) — EM ANDAMENTO + bug de heap latente
- Adicionado ao main.c: so_execute_init_array() por módulo, setup jni_shim, chamada JNI_OnLoad
  + nativeOnCreate(env, fake_this, 0...). MEMORY_MB libapp 320→64 (suficiente, loada 11.6MB).
- ⚠️ **BUG: heap corruption `malloc(): invalid size (unsorted)`** no load do libapp (durante
  so_resolve→dlsym, que mallocaa). DIAGNÓSTICO: F1 commitado funciona (0 corrupção); minhas
  adições F2 EXPÕEM um overflow LATENTE de heap (não-trigger no layout do F1). Descartado por
  bissecção: NÃO é init_array (NFS_SKIPINIT segue quebrando), NÃO é a sonda de heap (removida,
  segue). Suspeito = setvbuf(stdout/stderr,_IONBF) pré-load deslocando o layout e expondo o
  overflow. Removido o setvbuf (não testado — device caiu). PRÓXIMO: testar sem setvbuf; se
  funcionar, AINDA achar o overflow real (em so_snapshot_symbols? comb_append? so_util load?)
  pra não voltar no F2+ (nativeOnCreate alocará muito). debugPrintf alternativo p/ logs sem setvbuf.
- ⚠️ device nextos-87 caiu após ~15 runs (sem rota) — rebootar/power-cycle (lição
  [[reference]] DYSMANTLE: device degrada em sessões longas de teste).

## F2 — CAUSA-RAIZ da heap corruption ACHADA: construtor 11 do libapp
- Instrumentei so_execute_init_array (NFS_INITDBG): **construtor 11/189 do libapp (vaddr 0x68be0/
  file 0x68bfc) corrompe o heap glibc**. Construtores 0-10 passam a sonda; o 11 falha.
- Desmontado: o ctor 11 constrói uma std::string ("AI/Avoidance...") — chama um alocador
  (0x3de128) e escreve ~13 bytes. **Overflow pequeno** que o malloc do bionic tolera (layout de
  chunk diferente) mas o glibc detecta ("malloc(): invalid size (unsorted)").
- **FIX (proven em so-loaders): malloc/calloc/realloc/memalign com PADDING (+64B)** em
  src/imports.c (pad_malloc etc.) → absorve o overflow do engine na folga, sem corromper a
  metadata do glibc. free/realloc usam o mesmo ponteiro base (consistentes).
- ⚠️ NÃO TESTADO (device caiu — roteador reiniciou, possível mudança de IP DHCP). Build OK.
  PRÓXIMO: quando o device voltar, testar NFS_INIT=1 (init_array completa sem corromper?) →
  depois nativeOnCreate → EGL/render.
- init_array agora DEFAULT-OFF (NFS_INIT=1 liga); com o padding, a meta é reativar por default.

## F2 — progresso e MURO ATUAL (engine init multi-thread)
- ✅ **padding malloc (+64B)** ELIMINOU a heap corruption (ctor 11 = std::string overflow que
  bionic tolera). init_array agora roda os ctors 0-185 do libapp.
- ✅ **JNI_OnLoad funciona (0x10002 = JNI_VERSION_1_2)**, 0 crash, no estado DEFAULT (init OFF).
- ⚠️ **MURO: engine init multi-thread.** Com NFS_INIT=1: ctors finais do libapp (186,187,188 —
  config/strtoul que acessam globais nulos) crasham, E um ctor anterior spawna uma WORKER THREAD
  que crasha async em null+0x8 (não é FMOD — skip do FMOD não resolve). Skip por índice (NFS_SKIPCTOR)
  passa os finais mas a worker thread segue crashando. Recuperação por sinal (sigsetjmp/longjmp)
  NÃO funciona cross-thread.
- __stack_chk_guard/__stack_chk_fail: libapp usa o GLOBAL (resolve p/ glibc, estável) — NÃO o slot
  TLS, então o pad TLS do DYSMANTLE é desnecessário aqui (removido; mudava sig11→sig4).
- Infra adicionada (env-gated): NFS_INIT (liga init_array), NFS_SKIPINIT="lib", NFS_SKIPCTOR="i,j"
  (skip ctor por índice, só libapp), NFS_INITDBG (log por ctor), recuperação por sinal (so_util).
- **PRÓXIMO (multi-sessão):** a worker thread da engine precisa de um global que provavelmente é
  setado em nativeOnCreate (race: thread roda antes). Investigar: (1) qual ctor spawna a thread;
  (2) o que a thread deref nula em +0x8; (3) talvez adiar/bloquear as threads até nativeOnCreate,
  ou prover o global. Nível engine-comercial (DYSMANTLE/Hollow Knight). Estado DEFAULT estável
  (boot+JNI_OnLoad) preservado.

## F4 — ENGINE LÊ/PARSEIA O OBB! Frontier = objeto garbage no parse de asset
**PROGRESSO ENORME (do zero ao loading de assets):**
- ✅ softfp ABI shim (RE4): modf/math crashes resolvidos (+log10f e variantes f). A engine é
  SOFTFP (vmov r0,r1,d10 antes de bl modf); glibc é HARDFP → wrappers pcs("aapcs").
- ✅ jni_shim: cadeia File→getAbsolutePath, DisplayMetrics (widthPixels=1280/heightPixels=720 via
  GetIntField/GetFloatField), getObbFullPath, isObbAssets→1, getPackageName, getFilesDir.
- ✅ **OBB ABRE e a engine LÊ/PARSEIA** (fread header 1028B + parse byte-a-byte dos nomes de asset).
- ✅ render loop roda (tick recovery) + janela SDL2 1280x720 Mali fbdev.
- ⚠️ **MURO: durante o parse de asset, a engine cria um OBJETO GARBADE** (ponteiro wild) e ao
  destruí-lo (refcount→0, blx vtable[1]) crasha em memcpy(0,tid,11). Cascata: recuperar uma cópia
  leva à próxima (objeto todo é lixo). mIAssetV/mTextureV ficam vazios → tela preta.
  Frontier em libapp+0x47a920 (release-refcounted) ← 0x624a98 ← 0x4c971c.
- HIPÓTESE p/ o objeto garbage: ABI softfp num campo de struct do asset-dir, OU detalhe de ABI
  libc++ (shared_ptr/string control block), OU leitura de tamanho/contagem com endian/softfp errado
  no parse. PRÓXIMO: instrumentar o parse (0x4c971c/0x624a98), ver o objeto sendo criado; comparar
  com mais softfp (sincos? __aeabi float helpers?); checar se mais funções math via ponteiro.
- Flags de teste: NFS_INIT=1 NFS_SKIPCTOR="186,187,188" + NFS_TICKRECOVER/NFS_FOPENLOG/NFS_PTLOG/
  NFS_DLSYMLOG/NFS_NORECOVER. Hooks: pthread(NULL-attr) fopen open fread dlsym(softfp). OBB em
  /storage/roms/nfs/data/Android/obb/<pkg>/main.1003128.<pkg>.obb (623MB, deployado).

## F4 — DESCOBERTA: os "crashes" são ASSERTS DELIBERADOS (raise SIGSEGV, si_code=-6)
- O crash recorrente é `pthread_kill/raise(SIGSEGV)` (libc+0x7c720 = gettid→tgkill(getpid,tid,11)),
  **si_code=-6 (SI_TKILL)** = a engine RAISA SIGSEGV DE PROPÓSITO nos asserts (debug build, estilo
  breakpoint de debugger). Crash handler agora IGNORA si_code<=0 (NFS_NOASSERTIGNORE p/ desligar).
- Mas ignorar o assert → a engine usa o objeto garbage → crash REAL (blx r1 com r1 lixo, 0xe12fff36).
  Então o assert PROTEGE contra um objeto genuinamente garbage criado no parse de asset.
- Com assert-ignore + tick-recovery: render loop roda 300 frames, mas assets vazios → tela preta.
- **HIPÓTESE LÍDER p/ o objeto garbage: ABI libc++ iostream/string.** A engine lê o OBB byte-a-byte
  (= std::istream::get) e cria objetos. Se uma VTABLE de iostream/streambuf/locale resolveu pra
  libstdc++ (GNU) em vez da libc++ (LLVM snapshot), ou um símbolo _ZTV* caiu no dlsym errado → ABI
  divergente → objeto garbage. PRECEDENTE: DYSMANTLE precisou expor basic_filebuf::open do snapshot.
- **PRÓXIMO:** (1) checar se _ZTVSt*/_ZTVNSt* (vtables iostream) e std::locale/ctype resolveram pro
  snapshot libc++ ou pro libstdc++ (grep UNRESOLVED + comparar endereços); (2) forçar as vtables
  iostream do snapshot na tabela; (3) instrumentar 0x624a40/0x626e50 (criação do objeto). Maps
  lookup no crash handler já mostra PC/LR por módulo.

## F4 — relocações DESCARTADAS (todas aplicadas)
- libapp usa relocações PADRÃO (DT_REL 388264B=48533 relocs, REL não-RELA, não-Android-packed).
  A seção .rel.dyn cobre os 388264B completos → so_relocate (que itera .rel.dyn/.rel.plt) aplica
  TODAS. Vtables (.data.rel.ro) relocadas OK. NÃO é relocação faltando.
- Logo o objeto garbage é genuíno do parse: objeto alocado-mas-não-construído (vtable=lixo do
  malloc), OU asset não-encontrado no OBB → objeto lixo retornado → assert/crash ao usar/destruir.
- PRÓXIMO: instrumentar o parse (qual asset/nome a engine procura e não acha; ver se o índice do
  OBB é lido certo; checar se falta um arquivo além do OBB — só 1 fopen). Possível: a engine espera
  um 2º arquivo (patch obb / config) ou o índice do OBB precisa de outro parse.

## F4 — config strings preenchidos (não era o garbage), frontier confirmado
- jni_shim agora retorna ApplicationVersion="1.3.128", DeviceName="NextOS", Locale="en_US",
  Language="en", OsVersion="9" (eram vazios). A engine loga corretos, MAS o objeto garbage
  persiste (mesmo crash 0xe12fff36 pós assert). Logo a versão/config NÃO era a causa.
- 0x626e50 = tokenizer (split por '.'/',') com callback (blx r7=global). Garbage object segue
  no parse de asset, antes de ler dados (só ~1046 bytes lidos do OBB: header 1028 + início do dir).
- RULED OUT cumulativo: relocações, softfp (todas+f variants), arquivos faltando, asserts
  (deliberados si_code=-6), version/config strings, screen size, OBB path/abertura.
- **O bug é fundo no parse do índice do OBB / criação do objeto de asset** — precisa de RE focada
  multi-sessão (instrumentar a criação do objeto entre o header-read e o crash; entender o formato
  do índice do OBB EA; ver se o objeto é não-construído vs asset-não-achado). Cheguei a ~95% do boot.
