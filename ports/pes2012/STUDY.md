# PES 2012 Mobile — port Marmalade s3e → Mali-450 (.114)

APK: `~/Downloads/com.konami.pes2012_1.0.5-APK_Award.apk` (internacional, EN — **NÃO** a `jp.konami.pesam`).
OBB: `~/Downloads/com.konami.pes2012.zip` → `main.1000005.com.konami.pes2012.obb` (178MB, package.dz derbh).

## Engine (confirmado)
- `libpes2012.so` (527KB, ARM32 softfp, stripped) = loader Marmalade s3e. Exporta a API s3e inteira
  (s3eDevice*, s3eFile*, s3eDeviceExecPushNext = fibre/stack-switch, s3eDeviceYield, s3eCryptoVerifyRsa).
- `assets/PES2012.s3e` = **LZMA-alone → módulo XE3U 3.24MB** (magic `58 45 33 55`=XE3U, header LZMA `5d 00 00 01 00`, descomp 0x3171a4).
- Idioma: `assets/string/{en,fr,ge,it,sp}.bin` — **inglês**, sem japonês. Locale confirmado runtime = `en_US`.
- GLES1 fixed-function (perfil Mali-450 ótimo).

## Base
Copiado de `ports/sonic4ep1` (que TEM source funcional: main.c 2204 linhas + ep1_audio + so_util + softfp).
Divergência-chave vs Sonic: o PES **exporta os s3eFile\* por símbolo** → hookamos os EXPORTS
(`install_s3efile_exports`), muito mais robusto que os offsets internos do build Sonic.

## Estado (s1 2026-07-03) — BOOT + JNI + runNative LIMPOS, 0 crash
Fluxo validado no device (`SONIC4EP1_RUN_NATIVE=1`):
1. Módulo carrega (so_util ELF32), reloca, resolve. init_array OK. +324 símbolos.
2. **JNI_OnLoad OK**, 31 nativos capturados (initNative, setViewNative, runNative, generateAudio, onKeyEventNative...).
3. 15 s3eFile* exports hookados → nosso fopen. Símbolos-patch (getvm/devexit/debug) OK.
4. **initNative → setViewNative → setPixelsNative(1280x720) → runNative → retorna limpo.** Pega os paths + `getLocale`="en_US".
5. Loop de frames (`runOnOSTickNative`) roda, processo estável.

Mudanças no main.c vs Sonic:
- `SO_NAME`="libpes2012.so"; package "com.konami.pes2012"/1000005; removidos módulos extras Sonic.
- Patches de OFFSET Sonic (surface 0x8330c, caps 0x58d60, icf 0x13c44) gateados atrás de `PES_SONICPATCH` (OFF).
- `runNative` do PES tem **2 args String** (Sonic 3) — o call de 5 args funciona (extras em r0-r3 batem, stack ignorado).

## MURO ATUAL — o módulo do jogo nunca é carregado/entrado
`runNative` (file 0x2d92c) lê args → chama `0x2d39c` (scheduler cooperativo s3e) → **retorna cedo**.
`0x2d39c`: `bl 0x4e8b0` (se r0!=0 early-return r5=1) → `bl 0x245a4` (se !=0 → RUN path `0x2ef88`=exec-pump).
O RUN path só roda se o **main do jogo (fibre) estiver registrado**, mas o módulo **PES2012.s3e (XE3U)
nunca é carregado/descomprimido** → nada registrado → retorna.
- **Zero acessos a arquivo** durante runNative (nem com `app.icf`/`s3e.icf`/`PES2012.s3e` no cwd) → o load
  do exec está atrás do device-init/caps, que sai cedo no so-loader (mesma classe do wall Sonic).

Strings do loader confirmam a lógica alvo: `s3e.icf`/`app.icf`/`gameExecutable` → "The executable specified
in the ICF (%s) could not be found. Searching data folder for executable." → "Multiple executable files found."

## PRÓXIMOS PASSOS (sessão 2)
1. Achar por que `0x2d39c` sai cedo: desmontar `0x4e8b0`/`0x245a4`/`0x2ef88` (o exec-pump) e o device-init
   ANTES do runNative (o s3eMain real). Provável gate de caps/config vindo do icf que nunca carrega.
2. Achar o **module-loader** do .s3e (função que faz LZMA-decomp → XE3U → registra o app main). Grep no loader
   por quem chama a descompressão / lê "gameExecutable" / "Invalid .s3e file".
3. Uma vez no exec-load: nosso `s3eFileListDirectory`/`s3eFileOpen` já servem PES2012.s3e do cwd (data-folder search).
4. Equivalentes PES dos patches Sonic (caps, surface-flush, icf-inject) — re-achar offsets no libpes2012.so.
5. OBB: hook de path (o módulo acha por `%s/Android/obb/%s`) → redirecionar pro OBB real (`~/Downloads/...obb` no device).

## 🏆 s2 (2026-07-03) — MURO QUEBRADO: exec CARREGA e o jogo RODA (loop de frames estável)
O muro s1 ("XE3U nunca carrega") era uma CADEIA de 4 causas-raiz. Todas corrigidas; o
exec agora descomprime, o main do jogo roda e o os_tick apresenta frames sem crash.

### Método decisivo
- `runNative@off=0x2d92d` (STUDY s1 estava certo) → `0x2d400` → **`0x2d39c`** (scheduler s3e).
- gdb NÃO firma breakpoints na região do so_util (sw-bp não dispara), MAS **pega sinais**
  (SIGSEGV/SIGABRT) → usar `handle SIG.. stop nopass; bt`. Instrumentar por print no main.c
  (imprimir `runNative@off` etc via `g_main_text_base`). strace no device = decisivo p/ opens.

### As 4 causas-raiz (todas viram patch em `patch_marmalade_guards`, sempre-ON)
1. **CAPS GATE @ 0x413de** (main.c). O exec-loader `0x24348` chama `s3eDeviceCheckCaps(0xa216148)`
   em `0x41374`; em `0x413de` faz `bics r4,r6` (=required & ~registrados) e só passa (`beq 0x414c0`
   → ret 0) se r4==0. Como a ICF (device-caps) nunca carrega, registrados=só 0x10000000 → falha →
   `0x24348` retorna cedo → `0x2d39c` pega o RUN-stub (`0x2ef88=bx lr`) e NADA roda. **Fix:** patch
   `bics r4,r6` (0x43b4)→`movs r4,#0` (0x2400). Confirmado por strace: só então `PES2012.s3e` abre.
2. **CONFIG TABLE @ *0x8b3cc** (main.c). Depois de abrir o exec, o parser de config faz LOOKUP
   (`0x3c50c`: r4=*0x8b3cc; count=[r4+4]) antes de popular → NULL-deref em `0x3c522`. A tabela
   (ponteiro em .bss 0x8b3cc) é normalmente alocada no s3eDeviceCreate (pulado). **Fix:** pré-alocar
   tabela ZERADA (`*0x8b3cc = calloc(...)`) — o add-path `0x3c5e6` cresce buffers via realloc(NULL).
   (Mesma técnica do shim Sonic p/ *0xc875c.)
3. **Operadores C++ / atexit** (imports.c). `_Znaj`/`_Znwj`/`_ZdaPv`/`_ZdlPv`/`atexit` ficavam
   **UNRESOLVED** (loader não linka libstdc++) → `operator new[]` saltava pro nada e crashava no
   parser. **Fix:** `port_shims[]`: `_Znwj`/`_Znaj`→`w_malloc`, `_ZdlPv`/`_ZdaPv`→`w_free`,
   `atexit`→`b_atexit` (ABI-compat 32-bit). Agora 0 UNRESOLVED.
4. **ICF externo = double-free** (DEPLOY, não código). Com `app.icf`+`s3e.icf` externos NA PASTA
   E o config embutido no .s3e, o loader entra no path "Multiple config settings found - embedded
   in s3e file and in ICF file(s)" (str @ 0x70ed4) e faz `double free or corruption (top)`. **Fix:**
   **NÃO deployar app.icf/s3e.icf** — o .s3e tem ICF embutido (23668 bytes, lido via
   openFromMemory). Movidos p/ `_icf_bak/` no device. ⚠️ O port final NÃO deve incluir os .icf.

### Estado após os 4 fixes
- Boot limpo → `initNative`/`setViewNative`/`runNative` → exec descomprime → **game main roda**
  (lê ICF embutido, `listDir '.'` + `raw:///storage/roms/ports/lib`) → `runNative` retorna →
  main.c dirige `runOnOSTickNative` (os_tick=módulo XE3U carregado, addr alto) → **glSwapBuffers
  em loop, 0 crash, processo estável** (EXIT só por timeout).

## MURO ATUAL (s3) — TELA PRETA: game main loop não progride p/ carregar assets
- fb0 real = preto (não é artefato de contexto do glReadPixels; os DOIS buffers 1280x1440@32bpp = 0).
- O jogo roda o os_tick e faz swap, mas **não lê NENHUM asset** (strace: só polling de /sys input/som;
  0 opens de menu/database/string/obb). Idle após `listDir '.'`/`.../lib`.
- Hipóteses p/ s3: (a) **fibre/exec-pump**: o main do jogo roda em fibre (s3eDeviceExecPushNext =
  stack-switch); o RUN-path `0x2ef88` é STUB — talvez o pump per-tick esteja quebrado e o main não
  avance além do startup. (b) game espera **OBB** (`~/Downloads/com.konami.pes2012.zip` →
  main.1000005…obb, 174MB, formato .dz/group.bin) — mas ainda NÃO o requisita. (c) espera módulo
  .s3e adicional em `.../lib` (o listDir do lib dir). (d) falta callback s3e (yield/surface-valid).
- assets/ tem database/{main,save}.bin, menu/{hd,sd}, string/{common,en,fr,ge,it,sp}.bin (achatar
  p/ raiz NÃO destravou — o jogo nem chega a pedir esses).
- **Diagnóstico s3 (gl-trace + threads no PID REAL):** o jogo emite **ZERO chamadas de render**
  (SONIC4EP1_TRACE_GL=1 → só glSwapBuffers nosso; nenhum glClear/glDraw/glTexImage/glBind). Os
  `glInit/glReInit/glTerm` no log são só GetMethodID (registro), NÃO chamadas.
- ⚠️ **Cuidado ao inspecionar threads/gdb:** `timeout ./pes2012 &` → `$!` é o PID do WRAPPER
  `timeout` (64-bit, fica em `sigsuspend` esperando o filho) — NÃO o pes2012. Achar o PID REAL
  por exe-symlink (`readlink /proc/*/exe` = `*/pes2012`). (O "1 thread em sigsuspend" que eu vi
  primeiro era o timeout — red herring.)
- **Processo REAL está SAUDÁVEL e multi-thread** (não deadlock): T1 main em `usleep`
  (frame-loop chamando os_tick), T-swap em `egl_platform_backend_swap`/fbdev (present),
  T-mali `_mali_uku_wait_for_notification` (GPU), T-timer em `sigtimedwait` (timer POSIX que o
  jogo criou), + Pulse audio. Frames avançam (glSwapBuffers em loop), 0 crash.
- **O muro real: a LÓGICA do jogo não progride** — roda o os_tick a cada frame mas não emite
  render nem pede assets (strace: 0 opens de menu/database/string/obb; só polling /sys). Idle
  num estado inicial após `listDir '.'` + `listDir 'raw:///storage/roms/ports/lib'`.
### 🎯 s3 CAUSA-RAIZ LOCALIZADA (2026-07-03): o CÓDIGO do jogo (XE3U) NUNCA é carregado/executado
Refinamento decisivo — tudo que roda é o LOADER; a entry do jogo (IwMain) nunca é chamada:
- **`s3eDeviceYield` é chamado UMA vez só** (hook conta: YIELD #1, ms=10) — e é do LOADER
  (`s3eDeviceStart` 0x27154 chama yield em 0x271d8), NÃO do game. Logo após, runNative retorna.
  No-op no yield (PES_YIELD_NOOP) NÃO faz o jogo loopar → o game main não tem loop rodando.
- **Watchpoint HW confirma: o callback OS-tick `*(base+0x9122c)` NUNCA é escrito** (fica NULL) →
  `runOnOSTickNative`→0x5a4c8 é no-op (checa esse callback). Nenhum pump testado (sched/execpush/
  unyield) resume nada. As chamadas getLocale/getDeviceModel/fixOrientation + ICF-read + listDir
  são TODAS do loader durante s3eDeviceStart, não do jogo.
- **🔑 PROVA FINAL (strace read): só 24576 bytes (3 reads) são lidos do `PES2012.s3e` (1.1MB).**
  = header + ICF embutido (23668B). O resto do arquivo (código XE3U comprimido, →3.24MB) NUNCA é
  lido/descomprimido. **O executável do jogo não é carregado.**
- **Cadeia do exec-load:** runNative→0x2d400→`0x2d39c`(scheduler). Após device-init (0x245a4→
  0x24348, que roda subsystem-init + `s3eDeviceStart` 0x27154 e retorna 0), o scheduler chama
  `0x2d368`: (a) `bl 0x2423c`→`0x2413c` (lê `gameExecutable` do ICF, lê os 24KB) — **se retorna
  !=0, 0x2d368 RETORNA e pula o code-loader**; (b) senão `bl 0x2460c` = **o CODE-LOADER real**
  (deveria ler+descomprimir XE3U + chamar a entry) + `bl 0x248ac`.
- **PRÓXIMO PASSO EXATO (s4):** achar por que o code-loader não roda/completa:
  1. Runtime: qual o retorno de `0x2413c` (em 0x2d374 o `bne` decide se 0x2460c roda). bps não
     firmam no so_util, mas dá pra: patchar log inline, ou hook_arm em 0x2d368/0x2460c/0x248ac
     (contar entradas), ou ler regs via /proc + watchpoint.
  2. Se 0x2460c É chamado mas não lê o resto: desmontar 0x2460c até o read+inflate (zlib
     `inflate`/`inflateInit2_` estão importados; header XE3U magic 0x55334558 validado em 0x231xx,
     campo size/offset em [buf+0x8c]) e ver onde para. Strings: "Invalid S3E file - read failed"
     (0x70bc8), "incorrect data" (0x70b48).
  3. Achar a chamada da ENTRY do exec (após descompressão) e garantir que roda o game main.
- Só DEPOIS disso: OBB (`~/Downloads/com.konami.pes2012.zip`→174MB .dz/group.bin) + assets.

### s4 (2026-07-03) — trampoline-trace (hook_arm, `make_thumb_tramp`) mapeou o exec-load
Instrumentei 0x2413c/0x2460c/0x248ac/0x24504 (env `PES_TRACE_LOAD`, gated off). Resultado:
- `0x2413c → 0` (ok, config-read não bloqueia). `0x2460c → **1**`.
- Em 0x2d368: `bl 0x2460c; cmp r0; bne 0x2d378`(@0x2d390) — **0x2460c=1 ⇒ pula 0x248ac** (o loader
  que faz 0x24348+0x24c24+**0x24504=module-load**). Callers: **0x248ac SÓ de 0x2d392** (nunca
  alcançado pq 0x2460c=1); 0x24504 só de 0x248ac; **0x2460c de 0x2d38a E de s3eDeviceExecPushNext
  (0x40cbc@0x40ce0)**.
- Forcei 0x248ac (NOP no 0x2d390 + FORCE_CODELOAD): rodou como `0x248ac(a=1)→0` MAS **não chamou
  0x24504** e **não leu mais bytes** (arg errado: r0=1 é sobra do retorno de 0x2460c; 0x248ac espera
  descritor do exec, não 1 — tomou caminho errado). ⇒ forçar por aqui é inválido.
- **INTERPRETAÇÃO:** 0x2460c=1 = "exec ACHADO" e é estado NORMAL; o loader 0x248ac (via 0x2d368) só
  roda quando NÃO acha (0x2460c=0). Então o LOAD/descompressão do código deveria acontecer DENTRO
  de 0x2460c (ao achar) OU no dispatch via s3eDeviceExecPushNext — e NÃO acontece (só 24KB lidos).
- **CAUSA-RAIZ s5 (2026-07-03):** 0x2460c lê só ~4KB (`bl 0x49b70`@0x24664, header do exec) e
  **REGISTRA o exec numa TABELA** (global .bss **0x24884**, count em [+32], entradas de 0x2c bytes)
  e retorna 1="registrado/achado". **A descompressão+run do código é DEFERIDA** — o exec fica NA FILA
  mas nunca é DESPACHADO (exec-switch). s3eDeviceExecPushNext(0x40cbc) só chama 0x2460c (registra),
  não despacha. Com 1 passe de scheduler no runNative, o **exec-switch (decompress XE3U + jump entry)
  nunca dispara** → só 24KB lidos, game code nunca roda.
- **s6 (2026-07-03) — o DECOMPRESSOR/loader do exec é 0x24504→0x23054:** `0x24504` faz
  `fopen(path,"rb")`(via 0x470b0) + `bl 0x23054`(loader/decompressor real, callbacks 0x23da4/0x23ddc).
  Só é alcançado via 0x248ac←0x2d392, i.e. quando **0x2460c retorna 0**. Strings do loader:
  "Error reading s3e file %s"(0x713b4). Chamar 0x24504 DIRETO com o path (env PES_CALL_LOADER,
  gated) **retornou 0 e NÃO abriu o arquivo** — 0x23054 precisa de estado do subsistema s3e-file que
  não está pronto ao chamar de fora; não é plug-and-play.
- **🎯 CAUSA-RAIZ s6 (runtime gdb):** o exec-manager global **`0x86350` está TODO ZERO** (primeiras
  palavras=0, count[+32]=0) durante todo o boot — **NÃO-INICIALIZADO**. `0x2460c` faz um check
  (0x247fc: `ldr r6,[0x86350]; cmp r6,#0; beq→ret 1`) e **retorna 1 (short-circuit)** por causa
  disso → 0x2d368 pula o loader. (Há contradição no traço estático do miolo — count=0 deveria ir pro
  add-path/ret 0 — mas o desync thumb + bps-não-firmam impedem confirmar; o fato runtime é: manager
  vazio → 0x2460c=1 → sem load.)
- **PRÓXIMO(s7) — abordagem fresca:** (a) achar o INIT do exec-manager 0x86350 (quem aloca/preenche a
  1ª palavra e o array de execs) — provável parte do s3eDeviceCreate/Start que o so-loader pula;
  rodá-lo ANTES do runNative (como o bootstrap 0x13db0-seq já faz p/ outros subsistemas). (b) OU
  replicar o exec-load: chamar 0x248ac/0x24504 com o ESTADO/descritor correto (não só o path). (c)
  OU último recurso: descomprimir o XE3U manualmente (LZMA `5d 00 00 01 00`, magic 0x55334558,
  size em [buf+0x8c]) + relocar + chamar entry — reimplementa o loader (grande). Infra diag no
  main.c (gated): PES_CALL_LOADER/PES_EXEC_PATH, PES_TRACE_LOAD, PES_FORCE_CODELOAD, make_thumb_tramp.
- Infra de trace reusável no main.c: `make_thumb_tramp()` + wrappers `w_ld_*` (env PES_TRACE_LOAD);
  `PES_FORCE_CODELOAD` (patch 0x2d376+0x2d390). Prologos trampoline-safe: 2413c/2460c/248ac/24504
  (0x24034 NÃO — tem ldr [pc] no prólogo).

## Como rodar (debug)
```sh
cd /storage/roms/ports/pes2012; export HOME=$PWD LD_LIBRARY_PATH=/usr/lib32:$PWD
SONIC4EP1_RUN_NATIVE=1 SONIC4EP1_FILELOG=1 SONIC4EP1_ARG1="$PWD" SONIC4EP1_ARG2="$PWD" ./pes2012
```
Método de debug decisivo = **gdb no device** (`handle SIGSEGV stop nopass; run; bt`).
text_base runtime = `runtime(s3eFileOpen) - 0x4753d`. runNative file off = 0x2d92c.

## 🏆🏆 s8 (2026-07-03) — EXEC CARREGA, DESCOMPRIME E EXECUTA CÓDIGO DO JOGO
Cadeia de fixes que destravou o load (todos gated, ver main.c `PES_TRACE_LOAD` etc — precisam virar sempre-ON):
1. **VFS interno vazio** — o exec-finder usa `0x47e64` (VFS s3e interno, ≠ s3eFileOpen público que
   hookamos). Ele resolve path via tabela de mounts (0x8c69c) VAZIA → falha. Cadeia:
   `0x2460c→0x49b70(resolve path)→0x47e64(→0, falha)→0x2460c ret 1→pula loader`.
   **Fix (hook_arm+make_thumb_tramp):** `w_ld_49b70` = p/ `.s3e` escreve o path em buf e ret 0
   (bypass do resolve); `w_ld_470b0` (fopen interno do loader 0x24504) = p/ `.s3e` chama o
   **s3eFileOpen exportado** (0x4753c, já hookado→fopen real). → 0x2460c ret 0, count++→1,
   **0x248ac→0x24504 (loader) RODA e retorna handle**; exec lido 1.7MB (read de 512KB=descomprimido).
2. **ThreadCore/TLS não inicializado (crash pós-load 0x43804)** — as branches por-bit do
   `s3eDeviceCheckCaps`(0x41374) são os **13 INITS de subsistema** (ThreadCore/TLS=`0x5b198` cria a
   TLS key em 0x8c688, +0x38084/0x3b6ac/0x3f22c/0x3f9a8/0x42788/0x42e00/0x438bc/0x44a64/0x4b89c/
   0x4ee30/0x59e0c/0x5fc00). Meu caps-fix ANTIGO (`0x413de` bics→`movs r4,#0`) fazia o gate passar
   PULANDO todos os inits → TLS key=0 → NULL-deref. **Fix NOVO:** deixar o `bics` original correr e
   patchar os 13 `cmp r0,#0` (após cada `bl <init>`) → `movs r0,#0` (0x2800→0x2000) = força
   sucesso/registro de cada subsistema, gate passa em 0x414c0. (offsets cmp: 414f2 41512 41532 41552
   41572 41592 415b2 415d2 415f2 41612 41632 41652 41672.)

**Estado s8:** exec descomprime+reloca+**executa código do jogo** (PC agora na região do exec
~0xdba00000, FORA do .so). **Crash atual = no CÓDIGO DO JOGO** em `0x..e48a` (`stmia r0!,{r1,r2}`):
o jogo faz append de 8 bytes em `base + (count<<3)` onde `base=[r4]=NULL` (array/estrutura s3e ou do
jogo NÃO-ALOCADA; r4≈0xe209e7a4). Determinístico.
**PRÓXIMO(s9):** (a) provável causa: algum dos 13 caps-inits GENUINAMENTE falha e o force-ret-0
deixa a estrutura NULL que o jogo acessa → identificar QUAL init falha (prólogos têm ldr[pc], trampoline
difícil; usar log ou cmp-read) e fazê-lo suceder de verdade (dep faltando). (b) OU achar o que deveria
alocar o array em [r4]. Milestone: sair de "exec nunca carrega" p/ "jogo executando".

## 🎉🎉🎉 s9 (2026-07-03) — IMAGEM NA TELA: jogo renderiza "Loading..."
Cadeia final de fixes pós-exec-load (todos gated em `PES_TRACE_LOAD`, virar sempre-ON):
- **VFS interno vazio p/ assets**: além do exec, os assets (`menu/hd/menuAssetLoader.group.bin`
  etc) falham no VFS interno. O jogo lê `string/*.bin` via s3eFileOpen (nosso hook, ok), mas os
  `.group.bin` via `access()`+open direto. **CASE**: o jogo pede `menuAssetLoader.group.bin`
  (misto) mas o arquivo é minúsculo; `access()` do EXEC é case-SENSITIVE e NÃO passa pelo nosso
  w_access (import do exec ≠ nosso). ⚠️ **/storage/roms é vfat mas com `shortname=mixed` =
  case-SENSITIVE p/ nomes longos!** Fix: **cp do arquivo minúsculo p/ o nome misto exato**
  (`cp menuassetloader.group.bin menuAssetLoader.group.bin`); + assets achatados na raiz
  (menu/database/string). Também tornei w_access/w_stat/w_open/w_fopen case-insensitive (ci_resolve
  em imports.c) p/ os opens que passam pelos nossos hooks.
- **Memória (heaps s3e)**: o jogo usa múltiplas heaps de tamanho fixo (heap 6 etc). Heaps 1-6 não
  são criadas (config MemSize%d) → "heap 6 is not created" → malloc NULL → crash. Forçar heap 0
  (patch 0x4f190) estoura ("Heap 0 out of memory, 3MB pedido, 964KB livre"). **Fix DEFINITIVO:
  redirecionar a API de memória s3e p/ malloc/free/realloc do sistema** (hook s3eMallocBase 0x4f178,
  s3eFreeBase 0x4e3ec, s3eReallocBase 0x4f360 → malloc/free/realloc; RAM 832MB ilimitada). Sem OOM.
- **Resultado: 0 crash, 33+ swaps estáveis, fb0 mostra "Loading..." renderizado** (texto branco
  canto inf-dir). PRIMEIRA IMAGEM do PES 2012.

## MURO ATUAL (s10) — preso em "Loading..." (falta OBB/dados principais?)
Render esparso (poucos gl draws) = tela de loading. O jogo provável espera o **OBB**
(`~/Downloads/com.konami.pes2012.zip`→main.1000005…obb 174MB) com os dados do jogo (times/estádios),
OU mais assets. PRÓXIMO: strace do que o jogo acessa no loading; montar/mapear o OBB; ver se a barra
de progresso avança. Fixes de load todos gated em PES_TRACE_LOAD (precisam virar sempre-ON + os
`cp` de case-fix precisam ser feitos no deploy/instalador).

## 🎉🎉🎉 s10 (2026-07-03) — LICENÇA DRM PASSADA; jogo pede o OBB
O muro "Loading..." era a **verificação de LICENÇA (Google Play LVL)**:
- O jogo registra nativos `dc/eic/fcc/lc/nc` e chama Java `lc(rsaKey)` (base64 RSA pubkey), depois
  **espera o callback nativo `lc(int responseCode)`** com o veredito. Nosso stub era no-op → travava.
- **Fix (jni_shim.c CallVoidMethod "lc"):** ao chamar Java `lc`, invocar o nativo `lc(env,obj,0)`
  (jni_find_native("lc")). **0 = LICENSED** (LVL). (1/256=NOT_LICENSED → diálogo de erro
  "License verification failed / official version + network necessary [EXIT][MARKET]".)
- ⚠️ **s3eCryptoVerifyRsa (0x3da34) é usado p/ 2 coisas:** (a) assinatura do PRÓPRIO .s3e no LOADER
  (VÁLIDA — deixar rodar o real!), (b) licença no EXEC. Hook GLOBAL forçando "válido" QUEBRA o load
  do .s3e ("Incorrect signature in s3e file"). Fix: `w_s3eCryptoVerifyRsa` caller-aware — se
  return-addr na região do .so (loader) → chama o REAL; senão (exec) → força PES_RSA_VAL. (Na prática
  a licença passa só com lc(0); o RSA-hook do exec é redundante mas inofensivo.)
- **Estado: licença PASSA por default** (licfail=0, sig=0). Agora o jogo mostra o diálogo:
  **"This game requires additional data (180 MB) to launch... [EXIT][CONTINUE]"** = quer o **OBB**.

## MURO ATUAL (s11) — OBB / expansion (180MB) p/ chegar no menu
O jogo precisa do OBB `~/Downloads/com.konami.pes2012.zip` → `main.1000005.com.konami.pes2012.obb`
(178MB, **formato Marmalade package.dz**, magic `79 9c a8 0a`, contém "package.dz"). O jogo checa a
expansion (natives eic/fcc + Java eif/fc, OU apkexp.so via apk_* hooks já no main.c: GetDownloadState→3,
GetMainExpansionFilename→"resources.dz") e, achando "não baixado", mostra o diálogo de download.
PRÓXIMO(s11): deployar o OBB (package.dz) no local esperado + fazer a expansion reportar "downloaded/
complete" (ver se apkexp.so carrega e os apk_* hooks disparam, OU wirar os callbacks eic/fcc JNI com
o path do OBB). Depois o jogo carrega os recursos → MENU. Defaults do port: lic=0 sempre-ON;
assets case-fix (cp minúsculo→misto) + achatados na raiz; s3e-mem→malloc; VFS-bypass; tudo sem env.

## 🎯 s12 (2026-07-03) — GATE DE DRM "Not enough space" BYPASSADO (SPACEPATCH)
O muro "Install error / Not enough space (180 MB)" era a **verificação de expansion do gdrm**
(Google DRM + APK Expansion Downloader da Konami), NÃO um problema real de espaço.

### Diagnóstico (via pestrace ptrace-wrapper + dump de código do exec)
- **statfs64 é IRRELEVANTE**: reescrever o struct de `statfs64("/storage/roms/ports/pes2012")`
  p/ 1GB, 2GB, bsize=1, bavail-como-bytes — NADA muda o erro. O jogo NÃO decide por free-space.
- O jogo abre o OBB (`Android/obb/com.konami.pes2012/main.1000005.com.konami.pes2012.obb` = magic
  `79 9c a8 0a` = formato package.dz), tenta abrir `package.dz` (via scheme `raw://` que chega
  LITERAL ao openat → ENOENT; pestrace redireciona o ponteiro do path +6 e abre fd=18), **valida e
  DELETA** (unlink) o package.dz + database/old.bin, e cai no erro. Copiar OBB→package.dz não ajuda.
- A string "Not enough space" só existe no EXEC decompilado (XE3U, região r-xp `db882000-dbb62000`),
  não em libpes2012.so/pes2012. O erro é montado no Java (jni NewStringUTF) e exibido via `ds()`.
- **Achado do branch** (frame-walk da pilha no handler `ds()` + dump in-process + objdump ARM):
  função grande de loading (FSM). Comparação `bge free>=required` em `dc - 0x52f10` (`aa00002a`);
  o loop antes SOMA os tamanhos dos expansion files (required) e compara com `[r4+0x918]` (bytes
  baixados = 0 sem download real). Bloco de erro (mensagem indexada por `[r4+0x95c]`, código de
  estado; -1=sem erro) em `dc - 0x52d48`; caminho "sem erro, continua" do próprio jogo em `errb-0x6b0`.

### FIX aplicado (jni_shim.c, no RegisterNatives quando o native "dc" registra; ancorado no &dc)
- **SPACEPATCH1**: `dc-0x52f10` `bge`(0xaa00002a) → `b` incondicional (0xea00002a) = free>=required sempre.
- **SPACEPATCH2**: `dc-0x52d48` (1ª instr do bloco de erro, `ldr r0,[sp,#520]`=0xe59d0208) → `b` p/
  o caminho "sem erro, continua" (0xeafffe52). Resultado: **installerr=0, 0 crash, tick estável**.
  Offsets FIXOS no exec (ancorados no endereço runtime do native `dc` via jni_find_native, imune a ASLR).

### MURO ATUAL (s12) — FSM de download preso no "loading" esperando COMPLETE
Com o erro bypassado, o jogo TICA estável (audioGetStatus loop, backlightOn) mas fica na tela de
**loading (draws=1)** — o FSM de download espera o sinal "download complete" que só o **Google
DownloaderService (Java)** entregaria. Confirmado:
- Callbacks nativos `eic(nome,path,size)`/`fcc(idx,size)`/`dc(state)` CRASHAM se chamados direto:
  `dc` despacha p/ um listener one-shot `[*(dc+0x20)+48]` que fica **SEMPRE null** (nenhum
  IDownloaderClient registrado sem o serviço) → `blx null` → SIGSEGV. (opt-in PES_EXPCB / PES_DCDONE.)
- `[r4+0x918]` (bytes baixados) nunca é atualizado (fica 0); patch1 força o check de espaço mas o
  FSM tem outros checks de progresso/estado que continuam "incompleto".
- **PRÓXIMO(s13):** ou (a) achar a variável de estado de download que o tick do FSM consulta e
  forçá-la a COMPLETE, ou (b) registrar um listener fake em `[global+48]` + replicar a lógica que ele
  setaria, ou (c) replicar o DownloaderService. Base runtime: pestrace (raw:// redirect + statfs) NÃO
  é mais necessário p/ o gate (SPACEPATCH cobre). Rodar direto: `bash pt.sh` sem ./pestrace.

## 🚀 s13 (2026-07-03) — FSM de download MAPEADO, "installing" na TELA, muro = assets CRIPTO do OBB
Grande avanço a partir do SPACEPATCH (s12). Corrigido o entendimento do gate e mapeada a máquina de
estados inteira do downloader gdrm.

### Correção do SPACEPATCH (jni_shim.c, ancorado no native `dc`)
- O `bge` em `dc-0x52f10` (estado 8) NÃO era "free>=required→sucesso"; é `if(required[0x918] >=
  freespace) → ERRO 317 "not enough space"`. Como o path do statfs é inválido → freespace=0 e
  required=0 → "0>=0" → erro. **FIX: bge → NOP (0xe320f000)** = nunca desvia = sempre "cabe" =
  fall-through → **estado 9 (installing)**. (Antes eu fazia bge→b, que era o OPOSTO.)

### Máquina de estados do FSM de download (função em `dc-0x534e4`, dispatch em +0x64 `ldrcc pc,[pc,r0,lsl#2]`, jump table 14 entradas; estado em `[r4+12]`)
Instrumentada via hook_arm + trampoline (fsm_hook, PES_FSMLOG): sequência real **0→7→8→9→10**.
- estado 0 = init: `bl delivered(dc-0x541d4)`; retorna `this`(!=0) se arquivos presentes → estado 7.
  (delivered JÁ retorna true — os assets do APK estão soltos no dir.)
- estado 7 = valida cada expansion file (existe + tamanho via 0xdb6823xx). tot=[0x950]=2 files.
- estado 8 = check de espaço (o do SPACEPATCH). Com NOP → estado 9.
- estado 9 = **"installing"** (idx=[0x928] vs tot=[0x950]): `bl install(dc+0x1e4)` extrai file[idx].
  **install é NO-OP nativo** (só `NewStringUTF("package.dz")` — delega ao DownloaderService Java que
  não temos). idx nunca incrementa → trava. **Workaround (fsm_hook): força idx=tot** → estado 10.
- estado 10 = **mount** `0xdb8a34d4("package.dz")` (thumb) → abre package.dz (fd=18) e guarda handle.
  Depois o jogo tenta abrir os assets.

### 2 fixes de suporte no pestrace (src/pestrace.c)
- **raw:// redirect** (já existia): scheme Marmalade "raw://" chega literal ao openat → reaponta +6.
- **NOVO: bloqueia unlink de package.dz** via `ptrace(PTRACE_SET_SYSCALL=23, →getpid)`. O jogo deleta
  a cópia (rejeita) e depois no estado 10 tenta MONTÁ-la → ENOENT → crash. Bloqueado → sobrevive.

### 🧱 MURO ATUAL (s14) — assets do OBB estão CRIPTOGRAFADOS; jogo aborta ao não achá-los soltos
- O jogo lê assets do **FILESYSTEM real** via `my_s3eFileOpen` (o VFS s3e do libpes2012.so NÃO
  monta — bypass do Opus). Os assets do **APK** (menu/, database/, string/) estão soltos EM CLARO e
  funcionam. Mas `sound/` (e muitos grupos) só existem no **OBB**.
- O OBB (=package.dz, magic 79 9c a8 0a) tem índice EM CLARO (1682 nomes/paths backslash + 842
  entradas de 16B `[type][off][sz][sz2]` a partir de 0xb84e) mas os **DADOS de cada .group.bin estão
  CIFRADOS** (stream cipher forte: menuassetloader OBB⊕APK = keystream sem período; NÃO reinicia por
  arquivo; magic 3d030606 ausente no OBB). Sem a chave (que o VFS s3e usaria on-the-fly), não dá p/
  extrair. soundmenu.group.bin: off=131549287 sz=183146 (cifrado).
- Crash real = `s3eDebugErrorShow "Cannot open file sound/menu/soundMenu.group.bin for serialising
  (read)"` → raise(SIGSEGV) [é o próprio jogo abortando, não null-deref].
- **PRÓXIMO(s14):** ou (a) rodar o APK+OBB num Android/Waydroid p/ o jogo descriptografar e extrair os
  assets soltos (menu/database/string vieram do APK; sound/etc precisam vir do OBB descriptografado);
  ou (b) fazer o VFS s3e do libpes2012.so REALMENTE montar o package.dz (ele descriptografa on-the-fly
  ao ler) em vez do bypass my_s3eFileOpen; ou (c) achar a função/chave de descriptografia no exec/lib.
- 📚 **Referência a estudar:** https://github.com/Producdevity/cod-boz-port (material de port).
- Envs de trabalho: PES_FSMLOG (hook+log+skip-install), PES_DUMPFSM, PES_NO_SKIPINSTALL, PES_DELIVERED,
  PES_NO_FSHOOK. pestrace faz raw:// + unlink-block + statfs. Rodar direto (`bash pt.sh`) usa só o
  SPACEPATCH (bge→NOP); p/ passar do estado 10 precisa do pestrace (unlink-block) + package.dz + skip.

## 🔓 s14 (2026-07-03) — Assets do OBB são user-fs CIFRADO; VFS s3e nativo crasha no so-loader
Sessão longa atacando o muro s13 (assets sound/ só no OBB cifrado). Objetivo: usar a
DESCRIPTOGRAFIA nativa do jogo (sem Android).

### Descobertas
- Os assets .group.bin no OBB estão **CIFRADOS** (stream cipher forte, keystream diferente por
  arquivo, entropia 7.997; menuassetloader hd/sd têm keystreams distintos → não é key fixa; offline
  sem chave é inviável). Índice do OBB em claro: 841 basenames lowercase + 841 paths com '\\' + 842
  entradas 16B `[type][off][sz][sz2]` @0xb84e. soundmenu.group.bin off=131549287 sz=183146.
- **O jogo lê os assets via `s3eFileAddUserFileSys`** (user-fs): no mount (estado 10) registra
  `s3eFileAddUserFileSys(cbs, ud=0xb)` → callbacks em `cbs[0]=Open(filename,mode)`,
  `cbs[1]=Read(buf,esz,count,handle)` (Read DESCRIPTOGRAFA on-the-fly). A Open itera archives e chama
  `archive_open(archive,filename,mode)` (0xdb54bdcc); tem um flag-check `[mgr+2]!=0 → fail`.
- **O `s3eFileOpen` EXPORTADO (0x4753c, hookado p/ disco pelo Opus) NÃO alcança o user-fs** (retorna
  nil). E o **`s3eFileOpen` nativo (via trampoline) TRAVA** (loop de resolução OS-fs subindo /storage/
  roms/... p/ paths de asset). Chamar a **Open callback (cbs[0]) direto** evita o hang MAS **crasha
  em `archive_open` → funções s3e internas (libpes2012.so+0x47031/0x490dd)** = o VFS s3e nativo
  precisa de estado/TLS que não existe na thread de loading do so-loader (o muro que o Opus já
  documentou: "VFS s3e não monta / crasha").

### Infra construída (opt-in `PES_NATIVE_ARCHIVE`, main.c)
- **make_thumb_tramp_full**: trampoline thumb que relocaliza `ldr rX,[pc,#imm]` (via literal local)
  E `bl` 32-bit no prólogo (s3eFileOpen tem `push;movs;bl`, GetSize/Close têm ldr-pc). Reusável.
- **Dual-mode s3eFile**: handles nativos (rastreados em g_nat[]) roteiam Read/Seek/Close/GetSize/Tell
  p/ as funções reais; FILE* de disco usam libc. package.dz aberto NATIVO (handle 0x3e8) OK.
- Hook de `s3eFileAddUserFileSys` captura cbs/ud (`g_ufs_cbs`); `ep1_extract_from_archive` chama
  cbs[0]+cbs[1] e grava descriptografado no disco. **Crasha na thread de loading (TLS/estado s3e).**
- TLS-map (my_s3e_tls_get/set, 0x82d60/0x82d70) habilitado p/ PES — NÃO resolveu o crash.

### 🧱 MURO (s15) — 2 caminhos p/ os assets descriptografados
1. **Android/Waydroid (LIMPO, recomendado):** instalar APK+OBB num device Android, rodar (o jogo
   descriptografa+extrai os assets do OBB pro data dir), puxar os assets EM CLARO, deployar no port
   (como já foi feito com menu/database/string que vieram do APK). Moto G 100 (Android 12) foi
   conectado via USB mas caiu (precisa reconectar/autorizar). `adb install --bypass-low-target-sdk-block`.
2. **Consertar o VFS s3e nativo:** inicializar o estado/TLS do file-subsystem s3e p/ a Open callback
   (cbs[0]) rodar sem crash na thread de loading. Ou marshalar a extração p/ a thread main (fsm_hook)
   DEPOIS do índice carregar (no registro o índice ainda está vazio → Open retorna nil sem crash).
- Default do port = OPT-IN off (PES_NATIVE_ARCHIVE): roda estável até o muro do soundMenu ("installing"
  aparece via SPACEPATCH, depois "Cannot open soundMenu" pois o asset cifrado não carrega).

---

## §s15 (2026-07-03) — RAIZ do "Cannot open soundMenu" ISOLADA + ponte VFS (ainda muro)

Sessão longa de RE do fluxo de abertura do asset cifrado. **Descobertas definitivas:**

### RAIZ do "Cannot open"
- O jogo pede `data-gles1/sound/menu/soundMenu.group.bin` (camelCase). O worker tira `data-gles1/`
  e passa `sound/menu/soundMenu.group.bin` ao dispatch.
- **O índice do OBB indexa por BASENAME LOWERCASE** (`soundmenu.group.bin`) — 841 basenames + 841
  paths-diretório backslash (`sound\commentary\en\...`) separados. Magic OBB `79 9c a8 0a`, size
  178822265, "DTRZI" @0x800.
- Mismatch de case/path → o `sub_open` do archive não casa → NULL → **o worker chama
  `s3eDebugErrorShow("Cannot open ... for serialising")` (libpes2012.so+0x581d8) e crasha no
  handler** (libpes2012.so+0x2d92d/0x2d397).

### Estrutura do open (libpes2012.so, base ASLR = s3eFileOpen - 0x4753c)
- `s3eFileOpen` (0x4753c) = **wrapper minúsculo**: `push{r3,lr}; movs r2,#0; bl 0x470b0; pop{r3,pc}`.
- **Worker real = 0x470b0** (= s3eFileOpen - 0x48c). Faz disk + dispatch user-fs. Chamável direto
  como `worker(fn, mode, 0)` (computado em install: `g_worker_open`).
- **O worker RE-CHAMA s3eFileOpen internamente** na resolução → re-entra nosso hook. Por isso
  `real_s3eFileOpen(nome)` faz LOOP com o MESMO nome (não é o trampoline quebrado — é o worker).

### User-fs callbacks (cbs[], dump s3eFileAddUserFileSys) — layout Marmalade
`[0]Open(fn,mode)` `[1]Read(buf,esz,n,h)=DESCRIPTOGRAFA` `[2]Write` `[3]Close(h)` `[4]Seek(h,off,org)`
`[5]Tell(h)`. Dispatcher cbs[0] itera sub-filesystems chamando `sub_open(fs,fn,mode)`.

### Muros dos 2 caminhos testados
1. **Chamar cbs[0] direto (out-of-context):** crasha — pula pra função-ponteiro em DADO
   (0x45eff18, r3==pc), vtables do archive não inicializadas fora do dispatch do runtime.
2. **Backing package.dz NATIVO (PES_PKG_NATIVE):** o mount recursiona/estoura a stack logo após
   registrar o user-fs (o handle nativo re-roteia pela user-fs).
3. **Reescrever p/ basename/lowercase + real_s3eFileOpen:** o worker re-chama o hook → recursão
   (resolvida com guard de profundidade + wrapper my_s3eFileOpen/_impl, __thread g_open_depth).

### Infra pronta no código (main.c) p/ retomar
- `g_open_depth` (wrapper/impl) quebra a recursão; guard em depth>8.
- `g_worker_open` = worker direto (pula wrapper hookado).
- Ponte archive-handle: `g_arch[]`, `is_arch_handle`, `arch_open(bn)` (cbs[0]), e roteamento em
  my_s3eFileRead→cbs[1] / Seek→cbs[4] / Tell→cbs[5] / Close→cbs[3] / GetSize=seek+tell.
- GRP branch (depth==1): lowercase full path → g_worker_open. Re-entry (depth≥2): tenta arch_open(bn).

### PRÓXIMO PASSO (hipótese mais promissora)
No RE-ENTRY (depth≥2, DENTRO do contexto do worker), o cbs[0] DEVERIA estar seguro (vtables prontas),
mas no último teste o REENTRY não disparou (timing g_archive_ready OU run flaky). Confirmar:
(a) g_archive_ready setado quando soundMenu abre; (b) se arch_open no depth 2 casa o basename e
NÃO crasha (contexto do worker pronto). Se casar, rotear leituras→cbs[1] descriptografa → menu.
Alternativa: hookar o `blx r7` do worker (0x4736e = chamada ao sub_open) e lowercasear o filename
ali, deixando o worker achar sozinho (sem re-entrar s3eFileOpen).

---

## §s15b (2026-07-03) — DANGLING cbs CORRIGIDO + count=0 (OBB não montada)

Sessão longa, avanços GRANDES:

### BUG corrigido: g_ufs_cbs era DANGLING
A struct passada a `s3eFileAddUserFileSys(cbs,ud)` é TEMPORÁRIA (o runtime copia). Guardar o
ponteiro → dangling → `cbs[0]` virava lixo (0x449c2d0 heap, paridade errada) → todo arch_open/rota
crashava, UFSLOG nunca chamado. **FIX: copiar os valores no registro** (`g_cbs_copy[8]`, `g_cbs_ok`).
Agora cbs[0]=0xdb4a3595 (válido) e o dispatcher **roda LIMPO sem crash**.

### s3eFileAddUserFileSys (0x4aebc)
Valida 9 callbacks (offsets 0,4,8,12,16,20,24,28,32 = Open,Read,Write,Close,Seek,Tell,...) todos
!=0, senão pula pro fail (0x4af2e). Depois registra.

### RAIZ ATUAL: dispatcher da OBB-fs tem count=0 (nenhuma sub-fs montada)
- `cbs[0]` (OBB Open, no exec decompilado) = DISPATCHER: `ldr global[pc,#0x30]; if(global[+2]!=0)ret0;
  count=global[+4]; itera global2[count] chamando sub_open(fs,fn,mode)`. **global[+2]=0 (ok) mas
  count[+4]=0** → itera 0 filesystems → retorna nil p/ QUALQUER filename/form (basename lower/orig,
  backslash-full, forward-full — todos nil, SEM crash).
- `s3eFileAddUserFileSys` NÃO muda count (pre=0, post=0, r=0) → registra em OUTRO lugar, não na
  global2 da OBB-fs.
- **package.dz é aberto (0x3e8 validado) mas NUNCA LIDO (0 NATIVE Read)** → o mount que lê o índice
  do package.dz e popula global2 (adiciona a sub-fs) NUNCA RODA.

### CONCLUSÃO / PRÓXIMO
O jogo abre package.dz mas não monta (não lê o índice, count fica 0). O mount (state 10 "mount
package.dz") ou é gateado pelo download-complete (que nosso atalho FSM só finge superficialmente —
o mount re-verifica e pula a leitura), ou é uma função-jogo separada não disparada. **PRÓXIMO:
desmontar o handler do estado 10 do FSM (fsm=dc-0x534e4) p/ achar a chamada de mount que lê
package.dz+popula global2, e disparar/destravar ela.** Infra pronta: g_cbs_copy (callbacks válidas),
arch_open (dispatcher chamável), roteamento Read→cbs[1] etc. Só falta a OBB ter as sub-fs montadas.

---

## §s15c (2026-07-03) — CALLBACK de download EXAURIDO = muro s13 no mount (DownloaderService obrigatório)

Confirmação exaustiva de que o mount do OBB (que popula global2/count) só acontece via o fluxo de
**download-complete callback**, que exige o **Google DownloaderService (Java) AUSENTE**:

- **Fluxo do FSM:** state 9(install) → **state 11 (download-wait, LOOP infinito)**. Sem forçar idx,
  fica em 11 pra sempre. **State 11 é CALLBACK-DRIVEN, não poll:** setar idx=tot, bytes(0x918),
  [0x95c]=0/5 (DLSTATE) NÃO avança — o FSM espera o callback do manager.
- **Callbacks nativos eic/fcc/dc** (registrados p/ o DownloaderService chamar): TODOS fazem `blx null`
  (despacham pro download-manager não-inicializado, ausente). eic(name,path,size)=init, fcc(idx,size)=
  file-complete, dc(state)=state-changed. eic e fcc crasham em `blx null` (manager null).
- **dc(5) PARCIALMENTE funciona** com listener no-op injetado em `[*(dc+0x20)+48]` (dc chama o listener
  como FUNÇÃO, não jobject): não crasha, o jogo reage chamando `fc` (Java, file-complete) e referencia
  package.dz — MAS o FSM NÃO avança (fc é no-op'd; o mount real não roda).
- Prover o OBB no nome esperado (`main.1000005.<pkg>.obb` no HOME) não muda (idx fica 0 no check).

**CONCLUSÃO DEFINITIVA:** o caminho de callback é intransponível sem replicar o DownloaderService +
download-manager nativo (grande). **Único caminho offline restante = (b) construir o fs_obj do OBB
manualmente e popular global2** — mas `sub_open`(0xdb54bdcc) é função GRANDE (normaliza path p/
backslash, lê fs_obj@+0x60 flag +0x74, lookup complexo). RE do formato fs_obj + handle + decrypt =
esforço grande/incerto. Toda a infra de decrypt está pronta (g_cbs_copy, arch_open, roteamento cbs[1]);
só falta um fs_obj válido em global2. Env de teste (opt-in, jni_shim): PES_DCDONE+PES_DC_INJECT,
PES_EXPCB(+ONLY bitmask 1/2/4), PES_FAKE_DL+PES_DLSTATE, PES_NO_SKIPINSTALL.

## §s15d — cipher do decrypt é OO (vtable/contexto), reimplementar = research-grade
Read worker (sub_open+0x104): memcpy de handle+16 (buffer JÁ descriptografado); decrypt em bl(sub-0xacce)
usa fs_obj+4 = OBJETO CIPHER (vtable [ctx+4], contexto com destrutor freeando +8/+12/+16/+20). O cipher
é criado no MOUNT com a key (provável embutida build-time Marmalade s3e, mas dentro de estrutura OO).
Handle: +0(pos/limit) +4(offset) +12(entry-ptr) +16(buffer decrypt) +20 +24(size) +28(flag).
fs_obj: +4(cipher-obj) +12(tabela 16B entries) +0x60(substruct) +0x6c(count) +0x74(flag) +0x78(lookup) +0x80.
Construir fs_obj + cipher-ctx offline = reverter cipher OO completo + key init = multi-sessão incerto.

## §s16 — AVANÇO: assets de menu do DISCO (APK em claro) + abort do soundMenu bypassado + RENDERIZA
DESCOBERTA: os `.group.bin` de MENU/database/string estão EM CLARO no disco (extraídos do APK) — em
`menu/hd/menuAssetLoader.group.bin` etc. — mas o jogo pede com prefixo `data-gles1/` e não achava.
FIX: strip de `data-gles1/` em `ep1_file_real_open` + `my_s3eFileCheckExists` (tenta o path sem o prefixo).
Resultado: **menuAssetLoader carrega do disco, o jogo passa dele e RENDERIZA (draws=10, loading screen)**.
Só `sound/*.group.bin` falta (não está no APK, só no OBB cifrado).
Dummy soundMenu.group.bin (cópia de menuAssetLoader OU magic+zeros): **PASSA o abort "Cannot open"**
(sem erro) mas CRASHA no deserializer do grupo (memcpy libc+0x7c720 via trampoline s3e +0x5815c) —
conteúdo errado (texturas de menu como sons) OU grupo vazio inválido. **CUIDADO: runfull.sh tinha
`rm -rf sound` que apagava o dummy — removido.**
PRÓXIMO (tratável, ≠ decrypt do OBB): (a) construir um sound group `.group.bin` VÁLIDO VAZIO
(formato Marmalade s3e CIwResGroup: magic 3d030606 + nome + hash + lista de recursos; deserializer no
exec ASLR) OU (b) stubar o carregamento de sound groups (skip). Aí o menu renderiza (mudo) = IMAGEM.

## §s16b — crash do sound group ISOLADO: lookup de classe de recurso com registry NULO
Crash em `find_resource_class(r0=registry, r1=class_hash)` — função genérica de busca em lista de
entries de 48 bytes ([r0+16]=base, [r0+20]=count, chave em [entry+4]). **r0=NULL** → deref null em
`ldr r2,[r0,#16]`. r1=0xcf609cbe (class hash de uma textura, lido do grupo dummy=cópia do
menuAssetLoader). O registry de classes de recurso está NULO no contexto de carga do sound group —
o subsistema de recursos de SOM não está inicializado (provável dependência do mount do OBB, mesmo
muro DRM). Um sound group REAL (classes de som registradas) poderia evitar, mas r0 nulo sugere
subsistema inteiro não-init. Dois muros no som: (1) cifra do OBB, (2) registry de classes nulo.
Gráficos/menu/database/string do disco funcionam (draws=10). Caller ASLR desalinha no objdump.

## §s16c — offset da função de crash (find_resource_class) p/ retomar patch
crash_func = sub_open + 0xce2ac (sub_open = cbs0 - 0x1577c8). Função LEAF: find(r0=registry,r1=hash)
sobre entries de 48B ([r0+16]=base,[r0+20]=count,chave [entry+4]); crasha em `ldr r2,[r0,#16]` c/
r0=NULL. P/ retomar: hook_arm nesse offset com check `if(r0==0) return 0` e ver se o jogo passa do
soundMenu (registry do serialiser padrão nulo — subsistema de recurso de som não-init).

## §s17 — 720p splash piscando: áudio real abre, status não destrava

Estado visual atual: o jogo sai da tela preta e renderiza em 1280x720 o splash/título
PES2012/estádio/logo. Fica piscando/parado nessa tela, com texto de copyright enorme/estourado no
centro e "PRO EVOLUTION SOCCER" embaixo. Não parece ser ausência simples de PNG: GL desenha batches
de UI/texto, os assets de menu carregam, e input fake/touch chega no shim sem avançar a tela.

Patches/guards estáveis adicionados no caminho:
- `resource_self1c` em `sub_open - 0xb2178`: protege leitura de `[obj+0x1c]` quando ponteiro interno
  inválido.
- `resource_self14` em `sub_open - 0xb2808`: protege leitura de `[obj+0x14]`.
- Esses guards eliminaram crashes de ponteiro inválido durante dumps de registry/resource e testes
  com input/script.

Testes de classe/resource:
- `PES_CLASS_FALLBACK_SEEN=1` recupera classes já vistas (`207e2246`, `a68776be`, `6097ed50`) e evita
  parte dos nulos, mas não muda a tela.
- `PES_CLASS_SYNTH=1 PES_SYNTH_CTOR_PLACEHOLDERS=1` e depois `PES_CLASS_SYNTH_ALL=1` sintetizam também
  `b4502910`, `c8e42197`, `ba7b1ad9`; processo fica vivo, mas a tela continua igual.
- Conclusão: o splash piscando não é resolvido apenas preenchendo `find_resource_class`.

Áudio/música: achado importante corrigindo a hipótese anterior.
- Sem `PES_SKIP_MUSIC_GROUPS` e sem `PES_FAKE_MUSIC_GROUP`, `music/menu/musicMenu00.group.bin` abre de
  verdade pelo OBB via `GRP-ARCH`.
- Log confirmado: `CHKEXIST grp 'data-gles1/music/menu/musicMenu00.group.bin' ... rdy=1` seguido de
  `GRP-ARCH ... -> handle`.
- Com `PES_SKIP_MUSIC_PLAY=1` fica vivo. Sem skip de play também fica vivo, mas não apareceu chamada
  `audioPlay args`; aparece só `audioGetStatus` em loop.

Teste específico de status de áudio:
- Env usado: `PES_AUDIO_STATUS=1 PES_AUDIO_STATUS_LOG=1 PES_AUDIO_STATUS_STACK=1`, mantendo
  `PES_MUSIC_REAL=1 PES_SKIP_SOUND_GROUPS=1 PES_CLASS_FALLBACK_SEEN=1`.
- Resultado: processo vivo, `audioGetStatus -> 1` repetindo, mas continua preso. Não chamou
  `audioPlay`.
- Pilha repetida aponta para o mesmo trecho nativo (`caller=...`, stack com offsets como
  `+0xe830c`, `+0x12f5fc`, `+0x2a5748`), então o status Java não é o único gate.

Próximo ponto mais honesto ao retomar:
1. Parar de tratar como "só áudio"; áudio real de menu já abre. O bloqueio atual parece ser o estado
   de menu/splash esperando recursos/classes ainda incompletos ou algum callback interno.
2. Investigar os hashes ainda nulos em registries reais (`b4502910`, `c8e42197`, `ba7b1ad9`) contra
   os grupos `menu/hd/global*.group.bin` e `menuAssetLoader.group.bin`, mas sem assumir que synth
   basta.
3. Testar com `PES_SKIP_SOUND_GROUPS=0` agora que guards/resource-null estão melhores, para ver se o
   sound real abre ou qual é o próximo crash concreto.
4. Se voltar ao áudio, procurar por que o nativo consulta `audioGetStatus` antes de qualquer
   `audioPlay`; forçar status 1 sozinho já foi descartado.

## §s18 — VIRADA: os guards NULLREG (s17) CAUSAVAM os crashes; jogo renderiza limpo, muro = sound group do OBB

**Descoberta principal (mudou o diagnóstico do s16/s17):** os guards `install_null_registry_guard`
(hooks `resource_call8`, `resource_flags`, `find_resource_class`, `resource_slot`, `resource_array_find`
etc.) NÃO consertavam nada — NEUTRALIZAVAM funções reais do carregamento de recurso. Ex.:
`w_resource_call8` retornava **0 incondicionalmente** p/ centenas de objetos VÁLIDOS (0x498...), quebrando
o registro do registry → depois `find_resource_class(registry=NULL)` → deref null. Ou seja, os guards
eram a FONTE dos crashes (`addr=0xc/0x4/0x10`), não a proteção.

**Prova:** com `PES_NO_NULLREG_PATCH=1` (guards OFF) → **crash=0**, jogo renderiza "Installing..."
(logo PES2012 + barra), FSM completa 0→7→8→9→10 (mount OK), e só **trava (sem crashar)** no sound group.
Com guards ON → crash em draws=1. Guards OFF é estritamente melhor.

**Mudança de default (s18):** `install_null_registry_guard` agora é OPT-IN (`PES_NULLREG_PATCH=1`);
OFF por padrão. `runfull.sh` atualizado p/ o config vencedor:
`PES_NATIVE_ARCHIVE=1 PES_PKG_NATIVE=1 PES_MUSIC_REAL=1 PES_SOUND_REAL=1` (sem guards). → crash=0, 16 swaps.

**Bug corrigido:** `is_pkgdisk_handle` derefava `p->magic` ANTES de checar a lista de membros — com
handle nativo s3e não-ponteiro (ex. `0x3e8`) segfaultava. Agora checa a lista primeiro.

**Arquitetura de assets CONFIRMADA (corrige confusão s16):**
- O jogo lê os grupos de menu do **DISCO** (pré-descriptografados). menuAssetLoader HD (2.7MB) vem do
  **APK** (`assets/menu/hd/menuassetloader.group.bin`). global/globalPES/menuLogo/menuSplash (OBB-only)
  foram descriptografados numa sessão anterior (Jul 4) e estão no disco (magic 3d030606, nome interno
  correto "global" etc. = descrip. VÁLIDA).
- menuAssetLoader é pedido com `rdy=0` (PRÉ-mount) → SÓ do disco. soundMenu com `rdy=1` (pós-mount) → archive.
- **Deletar qualquer grupo do disco → "Cannot open ... for serialising" → crash.** O archive/cifra
  in-process NÃO reproduz os grupos de forma confiável (re-extração de menuAssetLoader/menuLogo = 0 bytes;
  Open out-of-context via bulk `ep1_extract` = **HANG**; muro s14/s15 confirmado).

**Índice do OBB é por HASH, não por nome:** varredura da memória viva do processo por "soundmenu",
"menuassetloader", ".group.bin", "sound/" = **NENHUM** encontrado. sub_open hasheia o nome pedido e
compara hashes. Logo não dá p/ enumerar nomes. O Open por basename falha; por **full-path backslash**
(`sound\menu\soundmenu.group.bin`) o hash CASA (retorna handle) → soundMenu ESTÁ no índice. Mas o
handle vem com entry-ptr apontando p/ lixo (`ffff0318...`) e **Read retorna 0** (buf não-alocado,
size=0x4000). O `ep1_extract` do menu funcionava no jogo NORMAL (in-context, s16) mas sound sempre foi
o holdout (s16: "Só sound/*.group.bin falta").

**soundMenu é OBRIGATÓRIO:** pular (nullar o grupo via SOUNDSKIP) OU pôr um grupo de formato-válido mas
conteúdo-errado (cópia de menuAssetLoader) → **crash** (`addr=0x10`; o jogo espera recursos de SOM, não
texturas). Precisa do sound group REAL descriptografado, OU um sound group VAZIO estruturalmente-válido.

**PRÓXIMO (2 caminhos):**
(A) **Android/Waydroid (limpo, alta probabilidade):** rodar APK+OBB num Android real (host não tem
   binder/waydroid; docker existe → redroid precisa de módulos de kernel; qemu Android-x86 precisa de
   tradução ARM). Extrair TODOS os grupos descriptografados, deployar no disco → jogo lê tudo do disco
   (comprovado que funciona p/ menu) e passa do sound → menu → gameplay.
(B) **Sintetizar sound group VAZIO válido (in-process):** RE do formato CIwResGroup de SOM (classes de
   recurso de som esperadas) p/ construir um `soundMenu.group.bin` com 0 recursos que o deserializer
   aceite; pôr no disco + `PES_SOUND_DISK=1` (som lê do disco). Menu mudo mas jogável. Incerto: o jogo
   pode referenciar sons específicos por hash e crashar em lookup vazio.

## §s19 — WAYDROID: RECEITA COMPLETA p/ rodar PES 2012 (crackeado) em Android x86 — chega ao "Loading..." (Marmalade v5.2.3)

**OBJETIVO:** rodar o APK+OBB num Android real (Waydroid) p/ o jogo descriptografar o OBB num contexto
de mount VÁLIDO, e extrair os assets em claro pro Mali-450 (que lê grupos do disco).

**Setup Waydroid (Arch, kernel 7.0.13 tem binder RUST + binderfs; sessão Wayland):**
1. `pacman -S waydroid lxc`; `mount -t binder binder /dev/binderfs`.
2. **Kernel sem `nft_masq`**: patch `/usr/lib/waydroid/data/scripts/waydroid-net.sh`: `LXC_USE_NFT="false"`,
   `IPTABLES_BIN=$(command -v iptables)` (backend nf_tables funciona via xt_MASQUERADE), `start_iptables`
   com `set +e` + `|| true` (tolerar CHECKSUM/xt faltantes). Rede sobe (waydroid0 192.168.240.1).
3. `waydroid init` baixa lineage-20 (Android 13). **USAR ANDROID 13** (o A11/lineage-18.1 NÃO spawna
   processo ARM neste kernel + logd quebrado). Trocar imagem = extrair system.img/vendor.img em
   `/var/lib/waydroid/images/` + **LIMPAR `~/.local/share/waydroid/data/*`** (downgrade/upgrade de versão
   quebra o PackageManager: `VersionInfo.sdkVersion null`).
4. **Tradução ARM (APK é armeabi)**: libndk (Google ndk_translation, variante Android 13, md5
   `0b2207c...`) → copiar `prebuilts/*` p/ `/var/lib/waydroid/overlay/system/` + props em
   `waydroid_base.prop` (`ro.product.cpu.abilist=...armeabi-v7a,armeabi`,
   `ro.dalvik.vm.native.bridge=libndk_translation.so`, `ro.dalvik.vm.isa.arm=x86`, etc). houdini também
   funciona; o erro do crack NÃO era o tradutor.

**🔑 FIX DO CRACK (o muro real, NÃO é o tradutor):** o `classes.dex` do APK crackeado tem SÓ 16KB
(`com.inject.Decracker`); as classes reais são decriptadas em runtime por `libinject.so` num JAR
`/data/data/PKG/auynn...jar` (aberto+unlinked, 0 bytes no disco). Esse jar tem as classes do jogo +
Marmalade do PES (LoaderAPI etc.) MAS **FALTA `com.ideaworks3d.marmalade.SuspendResumeListener`** →
`NoClassDefFoundError` no `InjectActivity.doInBackground:75`. 
**SOLUÇÃO (repackage):** adicionar `classes2.dex` com **APENAS** a `SuspendResumeListener` (interface:
`onSuspendResumeEvent(SuspendResumeEvent)V`) — extraída via `baksmali d` do Sonic4EP1 stock + `smali a`
de UMA classe (NÃO o dex inteiro do Sonic — versão 2016 causa mismatch `getListenerManager` no
LoaderAPI 2012 do PES). Assim o PES usa o próprio LoaderActivity/LoaderAPI do jar, consistente.
**+ renomear/copiar `libpes2012.so` → `lib/armeabi/libs3e_android.so`** (o LoaderActivity carrega
`libs3e_android`). Resign com `jarsigner` (keystore debug; v1 basta p/ target SDK 5). `pm install -r -t`.

**Permissões (app SDK<23 → diálogo de review bloqueia o spawn):** editar
`~/.local/share/waydroid/data/misc_de/0/apexdata/com.android.permission/runtime-permissions.xml`
(container PARADO): no bloco `com.konami.pes2012` setar TODAS as permissões `granted="true" flags="0"`
(remove `REVIEW_REQUIRED`=0x40). Reinstalar reseta os flags → reaplicar.

**RESULTADO:** InjectActivity → Decracker → Main → **LoaderActivity (Marmalade v5.2.3) + libpes2012.so
carregam** → diálogos "older Android" (OK @tap) + "ARM6/ARM4T" (Continue @tap) → **"Loading..." (tela
do jogo!)**. Chegou ao carregamento nativo do jogo. OBB montado (bind-mount /data/media/0/Android/obb).

**🧱 MURO s19 (mesmo do s13):** trava no "Loading..." — thread PRINCIPAL do Marmalade BLOQUEADA
(utime +1 tick em 40s, I/O flat após ler ~22MB do OBB) enquanto ~8 workers `Thread<NN>` fazem BUSY-SPIN
(852% CPU). É a **FSM de download/licença esperando o Google Play** (Waydroid VANILLA não tem Play
Services) OU **sync de thread do s3e quebrado sob ndk_translation** (workers spinam sem pegar jobs, main
espera). NÃO escreve assets em claro no disco (decripta em RAM). PRÓXIMO: (a) **microG** (waydroid_script)
p/ satisfazer LVL/download; (b) OU houdini (threading diferente); (c) OU s3e single-thread; (d) captura
por Frida/memória se passar do Loading. **ARTEFATOS em /tmp/pesmod/ (pes-min.apk pronto).**

**s19b — CONFIRMADO stall DURO (não é lentidão):** tela "Loading..." IDÊNTICA (screencap 61369 bytes)
por 8+ min. Thread principal (Android looper) idle em epoll_wait (normal); workers `Thread<NN>` em
busy-spin; I/O ~1.25MB/min mas SEM progresso de tela. OBB nem fica aberto (fd) — o wait é o callback
NATIVO de download-complete (gdrm → Play Services ausente). Sem DB do downloader Google (só
savegame.xml `notfirst=true`). **CONCLUSÃO: Waydroid VANILLA não passa do Loading sem Play/microG.**
Ambos os caminhos (Mali port c/ SPACEPATCH passou a FSM mas trava no soundMenu; Waydroid trava NA FSM)
convergem na dependência Google Play. PRÓXIMO REALISTA: (a) microG no Waydroid (LVL p/ 2012 = incerto);
(b) patch nativo do gdrm/FSM no APK (RE do libinject/exec); (c) device Android FÍSICO (nativo, Play real)
= caminho mais confiável p/ decriptar tudo e extrair. Waydroid fica instalado/configurado p/ retomar.

## §s20 — EMULADOR Android Studio (AVD) com GMS: a peça que faltava (config de tradução ARM pendente)

Alternativa ao Waydroid p/ ter Google Play Services (o muro s19). Android Studio emulator (QEMU/KVM):
- **cmdline-tools precisa Java 17** (`jdk17-openjdk`); o path do SDK **NÃO pode ter espaços**
  (sdkmanager quebra: "ClassNotFoundException: CLAUDE"). Fiz bind mount: `mount --bind "<ssd c/ espaços>" /mnt/asdk`.
- SDK grande → colocar no **SSD, não /tmp (tmpfs=RAM, encheu 26G de RAM)**.
- **Launch do emulator quebra com ambiente poluído** ("ANDROID_AVD_HOME is defined... no pes13.ini"):
  usar `setsid env -i HOME=/home/felipe PATH=/usr/bin ANDROID_SDK_ROOT=/mnt/asdk ./emulator @pes13 ...`.
- **API 33 (Android 13) x86_64 google_apis TEM `com.google.android.gms` + `com.android.vending` (Play Store)**
  = exatamente o que o Waydroid VANILLA não tinha (o muro s19). Deve destravar o download/licença.
- **Sem tradução ARM nativa** (`INSTALL_FAILED_NO_MATCHING_ABIS` p/ armeabi; emuladores modernos não
  traduzem armeabi 32-bit). Precisa **libndk** (push em /system: `adb root`+`adb remount`+1 reboot p/ overlayfs).
  API 23 (Android 6) tb tem GMS+Play mas tb sem houdini.
- **⚠️ ONDE PAREI (bug a corrigir):** setei `ro.product.cpu.abilist` no build.prop com **APPEND duplicado**
  (já existia `=x86_64`) → quebrou adbd/boot. **FIX: `sed` p/ SUBSTITUIR as linhas abilist, não duplicar.**
  Depois: reboot, `pm install /tmp/pesmod/pes-min.apk`, push OBB em /sdcard/Android/obb/, rodar — com GMS
  o download deve completar → jogo carrega → decripta OBB (extrair via Frida/memória, decripta em RAM).

**SDK em `/mnt/ARQUIVOS/.../99-TEMP-CLAUDE/claude-1000/android-sdk` (bind: /mnt/asdk). AVD `pes13`
(A13 x86_64 google_apis), libndk já no /system (build.prop com abilist duplicado a corrigir).**
Genymotion (AUR) é alternativa mais fácil p/ GApps+houdini se o AVD resistir.

## §s21 — EMULADOR AVD: GMS confirmado, MAS abilist=armeabi quebra o adbd (native-bridge runtime não integra)

Retomei o AVD com força. Resolvidos vários gremlins do emulador:
- **Launch:** `setsid env -i HOME=/home/felipe PATH=/usr/bin:/bin ANDROID_SDK_ROOT=/mnt/asdk ./emulator @pes13 ...`
  **SEPARADO** do wait (o timeout do comando matava o emulador junto). **Matar TODO qemu órfão + limpar
  `/run/user/1000/avd/running/*` antes de relançar** (órfãos quebram a descoberta do adb). **adb start-server
  ANTES do emulador** (senão "Unable to connect to adb daemon on port 5037" e não binda 5555).
- **GMS/Play confirmados** no image API 33 google_apis (a peça que faltava do Waydroid). ✅
- **libndk instalado COMPLETO** no /system (lib+lib64+bin+etc COM `ld.config.arm.txt`/`cpuinfo.arm.txt`;
  precisa `adb root`+`remount`+**1 reboot** antes do push, senão read-only).
- **🧱 MURO DURO:** setar `ro.product.cpu.abilist=...,armeabi-v7a,armeabi` (necessário: `pm install` valida
  abi contra ele; `--abi armeabi-v7a` dá "ABI not supported"; abilist deriva do abilist32 que vem vazio)
  **QUEBRA o adbd** — o device fica `offline` e some após ~150s, MESMO com libndk completo E
  `-selinux permissive`. A imagem Google APIs (Android 13) do emulador **não tolera injeção do
  native-bridge em runtime** (o LineageOS do Waydroid tolerou). Precisaria integração em BUILD do image.
- **⚠️ RECOMENDAÇÃO:** **Genymotion** (AUR, VirtualBox) é feito p/ isto — instala GApps + ARM translation
  (houdini) integrados no image, sem o problema do abilist. OU rebuildar o system.img do AVD com libndk
  (pesado). O AVD tem GMS mas o ARM 32-bit não roda sem quebrar. Waydroid continua sendo o único ambiente
  onde o jogo REALMENTE rodou (até Loading), faltando só o Play (microG).
