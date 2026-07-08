# GTA VC (arm64) — HANDOFF sessão 2: muro do TEXDB streaming

**Estado:** o binário arm64 **BOOTA o driver impl\* inteiro** e, com os **dados
COMPLETOS** (1.42 GB) no device, **passa do HALT** da sessão 1 e entra no
**loop `implOnDrawFrame`**. Crasha de forma **determinística** no **streaming do
banco de texturas** (`TextureDatabaseRuntime`), na 1ª leitura de `texdb/hud.etc.dat`.

Este é o ÚNICO muro atual. Não é RAM, não é gate, não é o async worker.

---

## O que funciona (confirmado no device, Mali-450 .79)
- Loader 2 módulos: `libc++_shared.so` (2358 sym) + `libGame.so`.
- Resolver: `bully_stub_table` + `revc_pthread_table` (bridge bionic→glibc) +
  snapshot libc++ + `gtasa_stub_table`. **Zero UNRESOLVED.**
- Driver impl\*: `JNI_OnLoad`→`implOnInitialSetup` (gates init=1/susp=0/render=1,
  offsets StorageRootPath do bully bateram)→`implOnActivityCreated`→**GL Mali-450
  GLES2 1280×720**→`implOnSurfaceCreated`/`Changed`→`implOnResume`→loop
  `implOnDrawFrame`.
- Texturas comprimidas já decodificam no boot (fmt=0x8d64 = ETC2/EAC, cadeia de
  mips 128→1). Fontes/HUD abrem: `models/fonts.txd`, `textures/fonts/font1.png`.
- Sequência de arquivos abertos ANTES do crash (100% coerente com boot real do VC):
  `text/american.gxt` → `gta_vc.set` → `data/cutscenehands.xml` → `anim/*.anm`
  (mãos de cutscene) → `models/coll/peds.col` → `models/fonts.txd` →
  `textures/fonts/font1.png` → `texdb/hud.txt` → **`texdb/hud.etc.tmb`** →
  **`texdb/hud.etc.dat`** → **`texdb/hud.etc.toc`** → **CRASH**.

## O muro (RCA completa)
```
=== CRASH sig=11 ===  PC=libc memcpy (libc+0x7d8b4), LR=libc+0x7d8a0
  x0=0 (dst NULL)  x1=0x15121 (src ~offset pequeno)  x2=0xb=11 (n)
  x19=0xb (budget)  x20=AndroidFile obj  x21=1
  cadeia: memcpy(libc) <- NvFRead(tail) <- AND_FileUpdated(+0x9c=0x43da34)
          <- implOnDrawFrame(+0x48=0x44040c) <- nosso frame loop
```
- **`AND_FileUpdated` (0x43d998)** dreno da fila async `AndroidFile::firstAsyncFile`.
  Loop: por entrada, lê `budget=min(cnt, ~11)` bytes por frame via
  `NvFRead(buf=[entry+24], 1, budget, handle=[entry+0])`.
  - `[entry+24]` = dest buffer, `[entry+32]` = restante, `[entry+40]` = next.
  - checa `cbz [entry+24]` (pula se buf==0) ANTES do NvFRead.
- **`NvFRead` (0x443ebc):** `type=handle[0]`; `type==1 → b fread@plt` (fopen),
  `type!=1 → b NvAPKRead@plt`. Tail-call (sem frame) → por isso o PC fica em
  libc/`fread`→`memcpy` com LR ainda apontando pra `AND_FileUpdated`.
- **`NvFOpen` (0x443cfc):** malloc(16); `handle[0]=type`, `handle[8]=file`.
  Tenta `NvAPKOpen`(type 0) → `NvAPKOpenFromPack`(type 1) → `fopen`(type 1).
  Com `GTASA_NO_NVAPK=1` nosso `nv_open` retorna NULL → sempre `fopen`/type=1 →
  **NvFRead cai no `fread`**.
- **Conclusão:** o `fread` está sendo chamado com **dst=NULL** (ou FILE* podre)
  na leitura do **`texdb/hud.etc.dat`**. A entrada async do texdb tem o buffer de
  destino NULL — o `TextureDatabaseRuntime` (o `.toc`/`.tmb` parseia a tabela e
  deveria alocar/apontar o buffer de streaming) não montou o destino.
  - `NvFRead` referencia `TextureDatabaseRuntime::detailTextures` (8be000+…) —
    confirma que o streaming é o do texdb.
  - `x2=11` (budget do frame) e `x19=11` casam: lê em fatias pequenas por frame.

## O que foi TENTADO (e por que não resolveu)
1. **Gate atômico no async worker** (copiado do bully:
   `__atomic_load_n(firstAsyncFile, ACQUIRE)`): NÃO resolveu. O gtasa chamava
   `AND_FileUpdated` incondicional (hack NVEvent); o bully usa o gate. Correto
   manter o gate, mas **não é a causa**.
2. **`GTAVC_NOASYNC=1`** (desliga nosso worker): **crasha IGUAL**, agora vindo de
   `implOnDrawFrame+0x48` — ou seja, **o próprio frame loop do jogo chama
   `AND_FileUpdated`**. Prova definitiva: o bug NÃO é race/timing do nosso worker,
   é a entrada do texdb com buffer NULL.

## PRÓXIMOS PASSOS (por prioridade)
1. **Descobrir por que o buffer de destino do texdb é NULL.** Hookar/loggar o
   `NvFRead` (0x443ebc) — logar `buf, count, handle, handle[0]` na 1ª chamada do
   texdb; e o `.toc` parser (`TextureDatabaseRuntime`) que popula a entrada async.
   O `.toc` diz onde/quanto ler; se o parse falha (endianness? tamanho? formato
   etc/dxt/pvr?), o buffer não é alocado.
2. **Testar as 3 variantes do texdb:** existem `hud.dxt.*`, `hud.etc.*`,
   `hud.pvr.*`. O jogo escolheu **etc** (Mali suporta ETC1, ok). Se o etc estiver
   corrompendo o parse, ver como o jogo escolhe a variante (provável config/caps
   GL) e se dá pra forçar. **NÃO** descartar etc antes de instrumentar.
3. **Comparar com o DYSMANTLE / bully "streaming nativo"** (memória
   `project_dysmantle_streaming_dyspage`, `project_bully_r36s_gles3_etc2`) — a
   mecânica de streaming de página é parecida; ver como lá o buffer é alocado.
4. Se o texdb streaming for grande demais pro caminho async, avaliar **forçar
   leitura síncrona** do texdb (hook em NvFRead: se buf==NULL e for texdb, alocar
   um buffer temporário e completar a leitura, ou pular a entrada com segurança
   pra ver o próximo muro).

## Como rodar (device Mali-450)
```bash
cd ports/gtavc && ./build.sh          # -> ./gtavc (PIE aarch64)
scp gtavc root@<device>:/storage/roms/ports/gtavc/gtavc-nosso
# no device (foreground; ES parado):
cd /storage/roms/ports/gtavc
systemctl stop emustation
for d in $(ls assets); do ln -sfn "assets/$d" "$d"; U=$(echo $d|tr a-z A-Z); ln -sfn "assets/$d" "$U"; done
export LD_LIBRARY_PATH="$PWD:/usr/lib:/usr/lib/aarch64-linux-gnu"
export GTASA_NO_NVAPK=1 GTASA_NODIAG=1 GTASA_NOAUDIO=1 SDL_VIDEO_FULLSCREEN_DESKTOP=1
nice -n 19 ./gtavc-nosso > /tmp/vc.log 2>&1
```
Envs de debug adicionados nesta sessão:
- `GTAVC_NOASYNC=1` — desliga nosso worker (o jogo ainda dreno via implOnDrawFrame).
- `[open] "..."` — loga os 120 primeiros arquivos abertos (em `nv_open`).

## Dados no device
`/storage/roms/ports/gtavc/assets/` = 1.42 GB COMPLETOS (extraído do APK
legendado PT-BR `23de350d-…`). Subdirs: anim audio data data1 models movies
rockstar skins texdb text textures txd (+ flutter_assets/dexopt/orig.apk que NÃO
usamos — bloat do APK, pode limpar depois).

## Offsets-chave (libGame.so arm64, APK 23de350d)
| símbolo | offset |
|---|---|
| implOnInitialSetup | 0x440150 |
| implOnActivityCreated | 0x440250 |
| implOnSurfaceCreated | 0x43cf58 |
| implOnSurfaceChanged | 0x43cf5c |
| **implOnDrawFrame** | **0x4403c4** (chama AND_FileUpdated em +0x48=0x44040c) |
| implOnRockstarSetup | 0x441724 |
| **AND_FileUpdated** | **0x43d998** (crash em +0x9c) |
| NvFOpen / NvFRead | 0x443cfc / 0x443ebc |
| NvAPKRead | via PLT 0x4736b0 |
| OS_MutexObtain/Release | 0x442c88 / 0x442c8c (= b pthread_mutex_lock/unlock) |
