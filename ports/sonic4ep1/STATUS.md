# Sonic 4 Episode I — STATUS de debug do boot (so-loader Marmalade/s3e)

## 🟢🟢 s3 RUN (2026-06-29) — VIRADA HISTÓRICA: engine s3e BOOTA INTEIRO, módulo .s3e CARREGA+RODA, 0 crash
**De "crash do surface no frame 1" → boot completo de 937 linhas, áudio inicia, engine no loop de OS-tick.**
Receita vencedora (JÁ no run.sh) = `HOOK62D90 + NO_CAPFIX + FAKE_FILEREG + TLSMAP(default)`, SEM SURFOBJ.

### As 4 correções (em ordem de descoberta), todas no main.c:
1. **setViewNative RELIGADO** — `0x3a9f4` NÃO é "config-parser" (erro da s2); a tabela JNINativeMethod
   diz que é o nativo `setViewNative`. Estava hookado pra no-op (main.c) → o engine nunca cacheava os
   method-IDs do view (glSwapBuffers/doDraw/soundInit...) → surface NULL. Agora só stuba se
   `SONIC4EP1_STUB_SETVIEW`. Isso sozinho já levou o boot do frame-1-crash até o doDraw/tick.
2. **NO_CAPFIX** — o CAPS FIX antigo (`0x58d60 ldr→mvn r3,#0`, caps=0xffffffff) marcava TODOS os
   subsistemas como "já registrados". O device-register `0x58cdc` faz `r4 = requested & ~[caps]` e
   registra os bits de r4 → com caps=0xffffffff, r4=0 → **NÃO registra NADA**, incl. ThreadCore
   (bit 0x8, init `0x7f928` que cria as TLS keys do surface em 0xc9a74). Desligado (`SONIC4EP1_NO_CAPFIX`).
   (mvn r3,#8 = "tudo menos ThreadCore" também funciona p/ rodar 0x7f928, mas NO_CAPFIX+FAKE_FILEREG
   é a rota limpa que carrega o módulo.)
3. **FAKE_FILEREG** — com caps naturais o dispatch TENTA registrar tudo, mas o **File-register
   `0x5f02c` FALHA** (so-loader sem backend real) → device-register dá bail (ret=1) → **s3eMain
   (`0x267fc`) NÃO chama bootstrap B → o módulo do jogo .s3e nunca carrega**. File já roda via nossos
   hooks, então `my_filereg_5f02c` retorna 0 (skip) → dispatch CONTINUA e registra Fibre+ThreadCore+
   Surface (criam as TLS keys). Hook em `0x5f02c` (NÃO trampolinar: 1ª instr é `ldr [pc]` PC-rel).
4. **TLSMAP (a chave, default ON)** — emula `s3eThreadLocal` get=`0x82d60`/set=`0x82d70` com um **mapa
   global key→valor** (`my_s3e_tls_get/set`, main.c). O esquema nativo guarda a "key" em globais
   (0xc9a74 etc.) e faz `pthread_get/setspecific(key-1)`, mas no so-loader as keys vêm inválidas
   (malloc-ptr/0) → getspecific NULL → crash do surface/fibre/threadcore (todos usam a MESMA key
   0xc9a74). Como o so-loader é **single-thread** (main dirige tudo), o mapa global dá round-trip
   perfeito e **conserta surface+fibre+threadcore DE UMA VEZ** (convergente, não whack-a-mole).
   Desliga com `SONIC4EP1_NO_TLSMAP`.

### 🎯 MURO ATUAL (s3 fim): `Error loading resources (Not implemented)` — loader do módulo .s3e
- O engine **lê o .s3e INTEIRO** (703KB: reads 2+11+524288+178920 via HOOK62D90), inicia áudio
  (soundInit 44100), entra no loop (runOnOSSignal→runOnOSTickNative), faz enableRespondingToRotation,
  e aí o **engine** (não o game) loga via s3eDebugOutputString: **"Error loading resources (Not
  implemented)"** e **TRAVA DURO** (linha 937 = última; 0 ticks depois; processo vivo mas parado).
- Caller = base-rel **0x7b7dc** = dentro de `s3eStackSwitchCallS3EFuncR12_4Args_Fast_Void_NoSS_FastLock`
  (thunk de stack-switch/fibre do s3e que chama `s3eDeviceLoaderCallStart` 0x5b454 + `bx ip`). Ou seja
  o **loader de módulo/recurso .s3e** retorna "Not implemented" — provável LOADER do s3e não-registrado
  ou um passo do load (decompress LZMA 0x50e78 / relocação ARM / s3eDeviceLoaderCallStart) stubado.
- 🔎 **RAIZ ACHADA (decomprimindo o .s3e)**: o `Sonic4epI.s3e` é **LZMA-alone** (header `5d 00 00 01 00`;
  os reads 2+11=13B = header LZMA) → descomprime p/ módulo **XE3U** de ~1.9MB (game code + config
  Marmalade). O módulo CARREGA e RODA (suas strings estão em memória e ele chama s3eDebugOutputString).
  As strings `"Error loading resources (%s)"` + `"Not implemented"` + **`resources.dz` / `high.dz` /
  `mid.dz` / `ApkExpansion` / `obb status %d` / `obb not found: [%s]`** estão no módulo: o game carrega
  os **recursos do jogo via OBB/APKExpansion** (`resources.dz`). A extensão **s3eAPKExpansion**
  (libs3eAPKExpansion.so — carregada OK, RegisterExt OK) é a versão Android (JNI/OBB) e retorna
  **"Not implemented"** sem o sistema de expansão do Android → `%s`="Not implemented" → game aborta.
  Funcs da ext: `s3eAPKExpansionGetAbsolutePath / GetDownloadState / GetMainExpansionFilename /
  Initialize / Start / Stop`. **resources.dz JÁ ESTÁ no device** (`/storage/roms/ports/sonic4ep1/`,
  27MB — há tb `resources.dz.testcopy` 86MB; conferir qual é o certo).
- 🎯 **PRÓXIMO**: fazer o resource-load achar `resources.dz` localmente — hookar/stubar as funcs do
  s3eAPKExpansion p/ devolver: GetDownloadState=DOWNLOADED, GetAbsolutePath/GetMainExpansionFilename
  = caminho do `resources.dz` local. O game então abre resources.dz (via nossos hooks s3eFile) e
  carrega os recursos. (Achar como o game pega os ponteiros das funcs: s3eExtGetHash + RegisterExt
  da ext; assinaturas das 6 funcs.) Capturar tela só após passar daqui (precisa glSwapBuffers).
- 🔬 **DIAGNÓSTICO PRECISO do "Not implemented" (s3 cont, 2026-06-29)**: NÃO é APKExpansion nem
  glGetString (game não chama nenhum dos dois antes do erro). O erro é um **TIMEOUT de
  `pthread_cond_timedwait`**: o caller de s3eEdkErrorSet é **base-rel 0x82c68** (engine), numa
  func de wait (entry 0x82b64 = s3eThread sem/cond-wait): `pthread_cond_timedwait` retorna
  **110 (ETIMEDOUT)** → seta `s3eEdkErrorSet(group, code=1000, type=0)` → o game formata
  "Error loading resources (%s='Not implemented')". Ou seja: **o load de recursos é ASSÍNCRONO**
  (a main thread faz cond_timedwait esperando um job/worker completar) e **o produtor NUNCA
  sinaliza** → timeout. `SONIC4EP1_TRACE_ERRORS=1` (stuba s3eEdkErrorSet→0) faz o game IGNORAR e
  continuar, mas aí entra em **loop infinito** (13k+ timeouts) retentando — nunca abre resources.dz.
  **Threads em runtime** (`/proc/PID/task/*/wchan`): main=`futex_wait` (o cond_timedwait),
  2 workers=`futex_wait` IDLE (esperando job que nunca chega), Mali GPU thread, signal thread.
  Os workers EXISTEM mas o job de recursos **não é despachado** pra eles. ⚠️ Corrigi o TLSMAP p/
  **per-thread** (composite key thread+key, mutex) — necessário p/ o worker não colidir com a main,
  mas NÃO resolveu sozinho (job ainda não despacha).
- 🎯 **PRÓXIMO (muro real)**: o job-system do s3e. Achar (a) quem chama o wait 0x82b64 (o ponto de
  espera do load de recursos), (b) o PRODUTOR (quem deveria postar/sinalizar o job nos workers idle),
  (c) por que o dispatch não acontece no so-loader. Possíveis: queue do job-system não inicializada,
  ou o engine espera rodar o job INLINE (single-core) e nosso setup confunde, ou falta um worker
  específico. Quando o job completar → recursos carregam → primeira tela.
- Captura: `touch /dev/shm/ep1_shot` só salva se o frame loop presentar (trava antes ainda).
- ⚙️ **TENTATIVA s3 (opt-in `SONIC4EP1_APKEXP`, default OFF)**: `install_apkexpansion_hooks()` em
  main.c computa a base da apkexp.so (`RegisterExt - 0xd1c`) e hooka as 6 funcs (ordem por endereço:
  GetAbsolutePath@0x8a0, GetDownloadState@0x900, GetMainExpansionFilename@0x950, Initialize@0x9a0,
  Start@0x9fc, Stop@0xa6c) p/ devolver path=deploy_dir, state=3(complete), filename="resources.dz".
  🔴 **CRASHA no hook_arm**: o write no text da apkexp.so faulta (text R-X) e o `mprotect` que adicionei
  NÃO resolve (write em base+0x8a4 ainda SIGSEGV) — o `so_load` mapeia o .so de um jeito que o mprotect
  não torna gravável (file-backed? offset?). **PRÓXIMO**: (a) investigar so_load/so_util — como o text
  da extensão é mapeado e por que não fica RWX; OU (b) abordagem alternativa: registrar MINHAS funcs do
  s3eAPKExpansion no s3eEdkRegister (em vez de chamar o RegisterExt real da .so), OU servir os callbacks
  s3eEdk que os wrappers usam (`ldr r3,[r0]; blx r3`) via jni_shim. Default (sem APKEXP) = estado bom de
  937 linhas. Palpites a refinar quando hookar: GetDownloadState "complete" (valor real?), e se o game
  abre `resources.dz` (27MB) ou precisa do `.testcopy` (86MB).

---


## 🟢 s3 STUDY (2026-06-29) — ANÁLISE ESTÁTICA (NextOS pediu "só estudar, não mexer no device")
### ⭐ ACHADO PRINCIPAL (provável ROOT do muro do surface): `setViewNative` foi STUBADO POR ENGANO.
A sessão anterior achou que `0x3a9f4` era "o parser de config que crasha (objeto NULL)" e
hookou pra no-op (`my_cfg_load_noop`, main.c:1101, **ON por padrão** — run.sh não seta
`SONIC4EP1_NO_CFGSHIM`). MAS a tabela JNINativeMethod do `libs3e_android.so` diz que
**`0x3a9f4` = `setViewNative`** (confirmado: assinatura JNI (JNIEnv*,jobject,jobject) +
disasm faz NewGlobalRef[env+84]/GetObjectClass[env+380]/GetMethodID na `view` e guarda os
handles em globais `[g+12/16/20/24]`). Ou seja, hoje o `setViewNative(env,thiz,view)` chamado
em main.c:1847 cai no no-op → **os handles do view/Java NUNCA são cacheados** → o caminho de
surface/present do engine deref NULL → **o crash do surface** (TLS getter 0x82d60 / 0x5db04 /
0x7be40). Não é "subsistema surface não-inicializado às cegas"; é **um native essencial
desligado**. 🎯 FIX a testar (quando voltar ao device): **remover SÓ o hook de 0x3a9f4**
(linha 1101) — deixar os 4 s3eConfig getters (0x52560/0x522b4/0x524d4/0x521f4). Como
setViewNative faz chamadas JNI de verdade (NewGlobalRef/GetObjectClass/GetMethodID/Call*),
o jni_shim PRECISA servir essas chamadas com handles válidos do "view" fake, senão
setViewNative guarda lixo e crasha — ESSE é o trabalho real do surface (servir os callbacks
JNI do present, não reconstruir a struct campo-a-campo).

### Mapa da CADEIA DE BOOTSTRAP NATIVA (existe inteira; corrige a narrativa "bootstrap não roda"):
`runNative`(**0x3c204**, table-confirmed) converte 3 jstrings → chama `s3eMain`(**0x2688c**→`0x267fc`):
- `0x6e9e8` s3eMemory init (se !=0 → return cedo)
- `0x13c44` **bootstrap A** (HOOKADO por `marm_gate_13c44`, mas chama o ORIGINAL via tramp):
  cpuinfo init `0x3e964` (precisa retornar 0 = sucesso; **a nota antiga "cpuinfo=0 → device-init
  não roda" estava INVERTIDA**: em `0x58cdc`/`0x58d44` o `bne 0x58e50` (early-return r0=1) só é
  tomado se cpuinfo!=0; logo cpuinfo=0 = SEGUE e registra subsistemas) → device-register
  `0x58cdc` (caps gated; sessão ant. forçou caps com patch `0x58d60 ldr→mvn r3,#0`) → **rom://
  setup `0x26890`** → **seq de register de subsistemas `0x13db0`**: `0x59544, 0x57280`(debug),
  **`0x5f798`(FILE)**, **`0x7e1b8`(SURFACE)**, `0x5be40`(ext), `0x7fa54`(surface-display),
  `0x7c2c0` + `0x58cdc`.
- `0x267b4` (gates 0x134a4/0x12e0c) → `0x13e28` **bootstrap B** (roda o app): inclui
  `s3eSurfaceSetup`(**0x7cdbc**) em 0x13f40.

### Nativos JNI registrados (tabela JNINativeMethod, addr confirmados):
`setViewNative`=**0x3a9f4**, `setPixelsNative`=**0x3bed8**, `runNative`=**0x3c204**,
`runOnOSTickNative`=**0x3abc8**. Lista completa de nativos: doDraw, eglWaitNative, initNative,
onAccelNative, onCompassNative, onKeyEventNative, onOrientationChangedNative, runNative,
runOnOSThreadNative, runOnOSTickNative, setCharInputEnabledNative, setPixelsNative,
setViewNative, shutdownNative. **Surface = framebuffer SOFTWARE** (setPixelsNative dá um int[],
engine renderiza nele e dá present via callback no view Java) — NÃO é EGL-backed. Por isso o
present precisa dos handles do view (setViewNative) + jni_shim servindo os Call*Method do blit.

### Surface por TLS (per-thread): getter `0x82d60`=pthread_getspecific(key-1); setter `0x82d70`;
key-create `0x82d88`(=s3eThreadLocalCreate, em 0x7f944/0x7f9a8 na init do surface). Os
setspecific do surface estão em 0x7ec64/0x8087c/0x80c30/0x810f4/0x812f0/0x81560/0x81a88 (subsist.
surface 0x7e-0x81). ⚠️ se o surface for criado numa thread e lido em outra → NULL (validar que
runNative e o frame-loop rodam na MESMA thread; em main.c são sequenciais na mesma função, então
ok — o problema mais provável é o setViewNative stubado, não thread-mismatch).

### jni_shim JÁ é maduro (confirma que o fix é simples): a vtable JNIEnv tem TODOS os índices que
setViewNative usa (21 NewGlobalRef, 22 DeleteGlobalRef, 31 GetObjectClass→fake class, 33
GetMethodID→fake id, 95 GetObjectField→fake object). E o caminho de PRESENT já está servido:
`CallVoidMethod(view,"glSwapBuffers")`→egl_shim_swap+os_tick; idem doDraw/glInit/soundInit/
audioPlay (jni_shim.c CallVoidMethodV/CallIntMethodV). Ou seja, o engine, no Android, PRESENTA
chamando métodos Java no view; o so-loader já intercepta isso. **setViewNative só cacheia o view
+ os method-IDs (glSwapBuffers/doDraw/soundInit/...) pro engine poder chamá-los.** Stubá-lo
(s2) = engine fica sem esses handles → surface object nunca é criado → crash. (s2 usou um
loop de os_tick MANUAL em main.c como present alternativo e desligou o setViewNative — mas isso
não cria o surface object.)

### 🎯 PRÓXIMO PASSO (quando NextOS liberar o device — está em PAUSA "só estudar"):
(1) tirar SÓ o hook de 0x3a9f4 (main.c:1101) — manter os 4 s3eConfig getters.
(2) rodar; setViewNative deve cachear os handles (jni_shim já cobre tudo) → surface object criado
   → TLS != NULL → some o crash 0x5db04/0x7be40.
(3) se setViewNative crashar mesmo assim, logar QUAL Call*Method/Field ele faz com o view fake e
   ajustar o retorno no jni_shim. Loop build→run→fix a partir daí até IMAGEM.
NÃO precisa reconstruir surface struct campo-a-campo (caminho divergente da s2) — é só religar o
native certo. Risco baixo, reversível.
---

Device alvo: **192.168.31.79** (Mali-450 Amlogic S905L, EmuELEC, kernel aarch64, fbdev).
Binário: `sonic4ep1` (loader ARMHF), engine **Marmalade s3e** (`libs3e_android.so` armv7).
Captura de tela: `touch /dev/shm/ep1_shot` → port faz glReadPixels → `/dev/shm/ep1_shot.raw`
(+ .txt com WxH). NÃO usar /dev/fb0 (preto).

## RECEITA DE DEPLOY RECUPERADA (o "run.sh que fazia tudo" perdido) — 2026-06-28

O binário/loader funciona; o que sumiu era o **layout de deploy + env de launch**.
Reconstruído nesta sessão. Para o boot ir o MAIS LONGE possível (até o load do
executável do jogo), precisa de TUDO isto:

1. **rom:// = assets/ do APK extraído**. O jogo enumera `assets/` (Sonic4epI.s3e +
   AUDIO/*.mp3 + s3e_splash.jpg + extdata.save). Extrair:
   `unzip -o sonic4ep1.apk 'assets/*' -d romfs` → deploy_dir = `romfs/assets`.
2. **UM ÚNICO .s3e** no cwd e no deploy_dir. Se houver 2 (ex.: `Sonic4epI.s3e` +
   cópia minúscula `sonic4epi.s3e`, ou o `.xe3u`), o engine acha config em vários
   e aborta: **"Multiple config settings found - embedded in multiple s3e files"**.
   → manter só `Sonic4epI.s3e`; mover `Sonic4epI.xe3u`/duplicatas pra fora.
3. **SEM s3e.icf/app.icf externos** quando o .s3e carrega (ele tem ICF embutido,
   lido via openFromMemory 5798 bytes). icf externo + embutido = mesmo "Multiple
   config". (Antes do case-fix o .s3e NÃO carregava e aí o icf externo era preciso.)
4. **Env de launch**:
   `SONIC4EP1_RUN_NATIVE=1 SONIC4EP1_DEPLOY_DIR=<abs romfs/assets> SONIC4EP1_ARENA=1
    SONIC4EP1_RESOLVE=1 LD_LIBRARY_PATH=/usr/lib32`
   - **ARENA=1 é OBRIGATÓRIO**: sem ele o malloc/free do engine cai na glibc e
     corrompe (double free / "corrupted size vs prev_size"). A arena isolada
     (mmap, free=no-op) está em imports.c, gated por `SONIC4EP1_ARENA`.

## FIXES NOVOS NO CÓDIGO (main.c) — 2026-06-28

- **Open case-INSENSITIVE** (`ep1_ci_open`): o s3e File baixa nomes pra minúscula
  (`Sonic4epI.s3e` → `sonic4epi.s3e`). FS case-sensitive falhava → .s3e não
  carregava → crash. Agora resolve cada componente case-insensitive no cwd e no
  `g_deploy_dir`. (técnica do Castlevania). **SEM isso o jogo nunca carrega.**
- **Close IDEMPOTENTE** (registro `g_ep1_open_files` + `ep1_track_open/close`): o
  engine fecha alguns handles 2× (open→read→close + cleanup fecha de novo). fclose
  dobrado liberava o cookie da glibc 2× → "double free detected in tcache 2".
  Agora só fecha se ainda aberto.
- pixel buffer com margem 256KB (+ modo guard-page `SONIC4EP1_GUARD_PIXELS`).

## ESTADO ATUAL DO BOOT (quão longe vai) — MUITO mais longe que no início

Com a receita acima, em ordem, o engine agora:
1. caps OK, .s3e abre (case-fix), ICF embutido lido (openFromMemory 5798).
2. config resolvida: getDeviceModel "NextOS", getRstDir (cwd abs), getCacheDir,
   getTmpDir, getLocale en_US, fixOrientation.
3. **chama doDraw → runOnOSTickNative** (loop de render ATIVO) e GetIntArrayElements
   do framebuffer.
4. tenta **carregar o executável do jogo `Sonic4epI.s3e`** via VFS interno.

## MURO FINAL = VFS interno do s3e (drive table vazia)

O engine abre `Sonic4epI.s3e` (o módulo executável do jogo) pelo **VFS INTERNO**
(`0x4136c` → resolve via `0x62d90` → `0x62adc`), NÃO pela API pública s3eFileOpen
(que eu hooko). Crash: **`0x62f28`** (`ldrb r3,[r3,#4]`, r3 = file-node->field8 =
drive ptr = NULL). Stub de 0x62d90→0 dá **"Can't open s3e file Sonic4epI.s3e"** —
ou seja, 0x62d90 é essencial (é o open do executável).

### Estrutura do drive (RE de 0x62adc, = s3eFileListNext+0xb50):
- drive table base = global pc-rel em `0x62b30` (FS global, ~`0xc9a88`).
- slot de drive em **+1008 (0x3f0)**: flag byte `[FS+1008]`, struct ptr `[FS+1016]`.
  Se flag==0 (drive não registrado) → erro/retorna 0; o open falha.
- drive struct: func ptr em `[drive+52]` (0x34), byte em `[drive+4]`.
- 0x62adc chama `0x64b0c` (type, ==3 = drive?), senão tail-call `0x62360`.
- setup do rom:///ram:// fica em `0x26890` (roda no exec; investigar se registra
  o drive ou se falha sem os pré-requisitos do bootstrap s3e).

### TENTATIVA s2 (s3eFileAddUserFileSys + relink) — FALHOU, mas ensinou muito:
`SONIC4EP1_ROMDRIVE=1` faz `ep1_register_rom_drive()` (main.c, disparado no 1o
doDraw via `ep1_rom_drive_register_once` chamado em jni_shim call_os_tick):
monta callbacks (nossos wrappers my_s3eFile*) → chama `s3eFileAddUserFileSys`
(**RETORNA 0 = OK**, registra no slot de usuário +3280, flag 0→1) → copia 284B do
slot +3280 pro slot rom +1008. **MAS o crash 0x62f28 PERSISTE**: o rom drive lê
`slot+8` como PONTEIRO pra um descritor rom-fs (com byte +4, func +52); o user-FS
guarda dados INLINE no slot (slot+8 = 0). Structs INCOMPATÍVEIS → relink não serve.
→ precisa construir o descritor rom-fs específico em `[FS+1016]`, não copiar user-FS.
(Default: ROMDRIVE OFF; reach=render loop + crash no VFS, igual sem ROMDRIVE.)

### DIAGNÓSTICO DEFINITIVO (gdb, dump dos slots no crash):
**TODOS os slots de drive estão ZERADOS** (flag=0, ptr@+8=0 em +156/+440/+724/
+1008/+1292/+1576). Ou seja: a init do s3e File subsystem que registra os drives
built-in (rom://, ram://, tmp://) **NUNCA roda** no so-loader (pulamos o bootstrap
s3e). A API pública (s3eFileOpen etc., que eu hooko→fopen) funciona SEM drives, mas
o load do executável .s3e usa o VFS interno direto → drive NULL → crash. Não há
nenhum drive funcionando pra copiar de referência → o descritor rom-fs (+8: byte+4,
func+52, + protocolo open/read) teria que ser construído do ZERO (RE do backend de
drive do s3e). É o muro profundo de verdade.

### s2 cont — AVANÇO REAL: crash do VFS NULL RESOLVIDO, agora "Can't open":
`SONIC4EP1_ROMDRIVE=1` (ep1_register_rom_drive): registra um drive via
`s3eFileAddUserFileSys` (addfs=0 OK, slot usuario +3280) + escreve um desc minimo
em slot[+8] dos slots built-in 0-4 (+156/+440/+724/+1008/+1292). Isso **MATA o
crash 0x62f28** (o resolve 0x62adc do path sem-scheme retorna slot 0=FS+156, e
slot0[+8]=desc evita o deref NULL). 🔑 base/FS conferidos (FS=base+0xc9a88, slot0
em +156, NÃO +1008). Agora o muro é **"Can't open s3e file Sonic4epI.s3e"**:
- A função de open do .s3e é **base+0x390xx** (acha via __builtin_return_address no
  hook de s3eDebugErrorShow → caller +0x3911c). Faz `blx [r4+540](path)`; se retorna
  3 → "Can't open". `[r4+540]` = open baseado em drive (matching 0x62360, scheme-based).
- 0x62360 itera os 14 slots e faz MATCH por scheme; meu drive (copiado do user-FS)
  NÃO casa o path sem-scheme → open falha. desc[+52]=my_drive_func NUNCA é chamado
  (filtrado antes pelo campo de scheme do slot, offset desconhecido).
- 🆚 OBS: o engine TAMBÉM abre 'sonic4epi.s3e' pela API PÚBLICA (meu hook 0x63438→
  fopen, retorna handle OK) mas o **loader do módulo .s3e usa o VFS interno**, não a
  pública. Por isso a pública abrir não basta.
PRÓX p/ destravar: achar o offset do campo SCHEME no slot (284B) e setar vazio/rom,
OU hookar base+0x390xx (a func de open do .s3e) pra abrir via fopen e devolver um
estado de módulo válido (depois vem LZMA-decompress 0x50e78 + load do módulo ARM).

### s2 cont — CADEIA DO VFS DESTRINCHADA (camada por camada), muro = subsistema File não-inicializado
Com `SONIC4EP1_ROMDRIVE=1` (ep1_register_rom_drive, no 1o doDraw) já passei por:
1. **drive registrado** (s3eFileAddUserFileSys slot 11 + copia p/ slots built-in 0-4;
   o resolve sem-scheme usa slot 0=FS+156). Descritor slot[+8] vem ZERADO; escrevi
   desc[+52]/[+64]=my_drive_func. → matou o crash 0x62f28.
2. **open ptr do file-manager era NULL**: `[mgr+540]` (mgr=base+0xc8904 → +540=base+0xc8b20).
   O open do .s3e (base+0x39040) faz `if([mgr+540]==0) "Can't open"`. A init 0x145f4
   PREENCHE a vtable do mgr (offsets 392-544) duma tabela de THUNKS (r3=base+0xc1ed4),
   mas 0x6ce74 ZERA +504..+544. Setei `[mgr+540]=base+0x43e9c` (o thunk da tabela,
   entry +0xa94). → **"Can't open" SUMIU**, o open EXECUTA.
3. 🔴 muro atual: o thunk base+0x43e9c faz `ldr r3,[V+24]; bx r3` com **V=base+0xc7b38
   (em .BSS, ZERADO)** → [V+24]=NULL → bx 0 → crash. V = a INSTÂNCIA do driver de FS;
   sua vtable (open/read/etc.) nunca foi preenchida (a init do subsistema File não roda).
RAIZ: o **subsistema s3e File não é inicializado** no so-loader — manager vtable +
thunks + **driver-instance vtable V (base+0xc7b38)** + descritores, tudo NULL em
camadas. Patchar ponteiro-a-ponteiro é divergente (muitas camadas). FIX convergente:
achar/rodar a init do subsistema File (que constrói o manager, instancia o driver rom
e preenche V), OU preencher V (base+0xc7b38) com os impls reais do driver rom (open=
[V+24]) — achar esses impls na .text. Flags: SONIC4EP1_ROMDRIVE, SONIC4EP1_NO_FMOPEN.

### s2 cont — AVANÇO REAL: open do .s3e FUNCIONA (hook 0x62d90), agora muro=SURFACE
🔑 **`SONIC4EP1_HOOK62D90=1`**: hook em `0x62d90` (open interno do loader do .s3e,
assinatura (path,mode,flags)) → nosso `ep1_file_real_open` (fopen case-insensitive).
**MATA o "Can't open s3e file"** — o engine recebe o handle e AVANÇA (faz
enableRespondingToRotation). [V+24] era a func de DEBUG-output, não open (me
confundiu); o open real é 0x62d90.
🔴 muro novo: **objeto de surface NULL** — `0x5db04` lê via TLS (`0x82d60` =
pthread_getspecific(key-1), key em `base+0xc9a74`=6); valor NULL → crash 0x5db18.
`SONIC4EP1_SURFOBJ` tenta pthread_setspecific(5, buffer_zerado) na main thread mas
NÃO persiste no getspecific da thread do acesso (key/thread mismatch; base+0xcf784
vinha lixo 0x4). Surface é OUTRO subsistema não-inicializado.
**CONFIRMADO DIVERGENTE:** o so-loader pula o BOOTSTRAP do device s3e → TODO
subsistema vem NULL (file ✅, surface 🔴, e virão áudio/input/module-loader). Patchar
um a um não converge em ciclos razoáveis. FIX convergente de verdade = fazer o
bootstrap do s3e rodar (achar por que runNative não inicializa os subsistemas; o
binário do Codex rodava o bootstrap — provável call de init no main.c que se perdeu),
OU recuperar o setup original. Flags novas: SONIC4EP1_HOOK62D90, SONIC4EP1_SURFOBJ.

### 🎯 ESTADO AO FIM DA s2 (2026-06-28) — open+read do .s3e FUNCIONAM, travando no surface
Config que avança mais (já no run.sh): HOOK62D90 + SURFOBJ (+ ARENA/RESOLVE/DEPLOY_DIR).
- ✅ **open do .s3e** via hook 0x62d90 -> nosso fopen.
- ✅ **read COMPLETO do .s3e** via nossos hooks (log: READ 512+5798+524288+178920 =
  os 703KB inteiros). O executável do jogo É LIDO/descomprimido.
- ✅ **surface TLS**: hook no getter `0x82d60` (=pthread_getspecific(r0-1)) devolve um
  buffer ZERADO não-NULL p/ qualquer key 0 / TLS NULL (g_ep1_surfobj). Passou de
  0x5db04 e 0x832d0 (paths "if(obj) else default").
- 🔧 último patch (não testado, build OK): g_ep1_surfobj[+8] aponta p/ g_ep1_scratch
  (campo aninhado que 0x7be40 escreve em [obj+8]+12).
- 🔴 MURO ao parar: **0x7be40** (e adiante) — o surface object é uma struct com
  PONTEIROS ANINHADOS; o buffer zerado resolve scalars mas cada campo-ponteiro
  precisa apontar p/ memória válida (whack-a-mole na struct do surface). RSS morre
  rápido (crash em ~4s). CONVERGE (cada fix avança 1 muro) mas é a struct do surface
  do s3e sendo reconstruída campo a campo.
RAIZ (inalterada): bootstrap do device s3e não roda -> subsistemas NULL. file ✅,
surface em andamento. Próx: continuar mapeando os campos-ponteiro do surface object
(0x7be40 [obj+8], etc.) OU achar/rodar o init real do surface (s3eSurfaceCreate).
Flags da sessão: HOOK62D90, SURFOBJ, ROMDRIVE, NO_FMOPEN, NO_VFILL, DUMPSLOT.

### PRÓXIMO PASSO (para a IMAGEM):
Registrar um drive real-FS na drive table: setar flag `[FS+1008]=1` + struct com
vtable de file-ops (`[drive+52]` = função de resolve/open que cai no nosso fopen).
OU achar/forçar `0x26890` a registrar rom://. OU hookar `0x62d90`/`0x4136c` pra
servir o conteúdo de Sonic4epI.s3e do nosso fopen (devolvendo um file-node sintético
com field8 != NULL). Reconstruir o backend de drive do s3e às cegas é o esforço
restante (GRANDE mas localizado: 1 drive + vtable mínima).

## Binários de referência (device /storage/roms/ports/sonic4ep1/)
- `sonic4ep1` = MEU build atual (tem case-fix, idempotent close, frame loop, capture).
- `sonic4ep1.new` (Jun27 21:36) / `.test` (Jun27 07:57) = builds intermediários do
  Codex. **NÃO são o build final**: sem case-fix não carregam o .s3e (open
  'sonic4epi.s3e' minúsculo → nil), runNative volta limpo, tela preta. O build
  final do Codex (que "jogou uma fase") + o run.sh foram perdidos (nunca commitados).
- `sonic4ep1.apk` (38MB) = APK original (fonte do assets/ e do rom://).

## Endereços-chave (libs3e_android.so)
- gate/caps: `0x13c44` (hook icf/deploy_dir), caps `0x58cdc`/`0x58d60` (CAPS FIX mvn).
  0x58cdc = loop de habilitar caps (NÃO é registro de drive; meu ID antigo errado).
- /proc/cpuinfo parser `0x3e970` (SEMPRE retorna 0 → device-init 0x58e50 não roda).
- VFS interno: open `0x4136c`, resolve `0x62d90`, drive-lookup `0x62adc`, crash `0x62f28`.
- s3eFileRemoveUserFileSys `0x6897c` (existe API de user-FS → talvez s3eFileSet... p/ registrar drive).
- main/exec do s3e: `0x2688c`→`0x267fc`; rom:// setup `0x26890`.
- string-intern config: `*0xc875c` (alocada calloc). surface-flush stub `0x8330c`.
