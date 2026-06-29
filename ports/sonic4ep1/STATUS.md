# Sonic 4 Episode I — STATUS de debug do boot (so-loader Marmalade/s3e)

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
