# 🦔 Sonic 4 EP2 — STATUS (s3 2026-06-26)

## Onde estamos
Boot completo: logo SONIC TEAM → título inglês → **Main Menu inglês navegável por gamepad**
(Start/Multiplayer/Help&Options) → seleciona **Start**. 🔴 **MURO: tela TRAVA EM AZUL/ROXO
sólido** (RGB **66,101,255** — NextOS confirmou "azul claro meio roxo", NÃO laranja; o dump cru
BGRA pode parecer laranja). World map NÃO renderiza.

## 🔬 s3 — DIAGNÓSTICO PROFUNDO DA TELA AZUL (muito reduzido o espaço de busca)
Device .79 (192.168.31.79, emuelec, root senha vazia). Binário/dados em
`/storage/roms/ports/sonic4/`. **Captura de tela**: `dd if=/dev/fb0 of=/dev/shm/fb.raw bs=1M
count=8` — fb0 é **1280x1440 (double-height)**, **BGRA 32bpp**, a metade de CIMA [0:720] é o
display. (O screenshot do egl_shim `touch /dev/shm/dys_shot` NÃO funciona: roda só em
`egl_shim_SwapBuffers`, mas nós presentamos via `egl_shim_present`→SDL_GL_SwapWindow.)

### O que a tela azul É (confirmado por gdb)
- O azul vem de **`fox_FrameRender+0x80`** (0x4acb34+0x80) chamando glClearColor(68,102,255) via
  `amDrawBegin` (0x1f13c0). = o render TOPO de quadro limpa azul e **NÃO desenha NENHUMA task de
  cena** por cima → cena ativa vazia (mesma classe do "menu não cria a task").
  (técnica: `break glClearColor` → backtrace; depois `break amDrawBegin` lê o caller via `$lr` e a
  cor RGBA via `r2`.)

### O que a tela azul NÃO é (ECONOMIZA tempo — todos REFUTADOS por gdb no processo travado)
- **NÃO é falha de leitura de arquivo.** O "Read error" repetido no log é **BENIGNO/spurious**:
  `tsFRead` (0x1fe298) lê FILE* raw via fread (retorna os bytes OK) e DEPOIS checa
  `*(u16*)(FILE+12) & 0x40`. Offset 12 é layout **bionic**; no **glibc** offset 12 = `_IO_read_base`
  (ponteiro) → o teste dispara aleatório → imprime "Read error" mas **o dado FOI lido**. Red herring.
- **NÃO está travado carregando recurso.** Thread FS (`amFS_proc` 0x1ff5e4) IDLE (cond_wait, nada
  pendente). `CResourceManagerTask::waitSetUpFinProc` (0x235460) **não é mais chamado**.
- **NÃO é a tela de loading nem o build de shader.** `CSSLoadingTask::mainProc` (0x24e96c) **não é
  chamado**; o gate da loading `SsReadyBuildIsFinished` (0x497984) tampouco; a global do
  `ss::ready::CBuild` (vaddr **0x99e4b4**, struct[0/4/8/12]) está **toda ZERO** (build inativo).
  ⇒ já PASSAMOS do loading; o jogo ASSENTOU num estado quieto sem cena.

### A transição Start→world map (mecânica mapeada)
- Menu "Start" → `CMainMenuStateDecision::Next` (0x22a458, roteia por índice cmp 0/2/5) →
  `Sonic4F2F::onMainMenuToMainGame` (0x1e42ac) / `onMainMenuToWorldMap` (0x1e42a4) — **ambos só
  chamam `Sonic4F2F::showInterstitial`** (0x1e3f10).
- `F2FExtension::showInterstitial(type, std::function& cb, bool b)` (0x4f13f4): com
  getInternetState!=0 e isUserRemoveAds==0 e slot vazio → **ARMAZENA o cb** no singleton F2F
  (offset +8 functor, +12 invoker). **NÃO há path "pula ad e chama cb agora"** — o cb SÓ é invocado
  por `callbackInterstitialAds` (= "o ad fechou").
- `callbackInterstitialAds(type,result)` (JNI 0x4fa6cc) → `callBackInterestitial` (0x4f136c): se
  `[singleton+8]!=0` e type!=4 → **invoca o cb** ([singleton+12]). Com result 0/2 invoca direto.
  ⚠️ NÃO limpa o cb após invocar.

### ✅ BUG REAL CORRIGIDO neste s3 (mas NÃO era a raiz do azul)
Antes: o main loop chamava `callbackInterstitialAds` **a cada 10 frames pra sempre** → re-invocava
o cb armazenado sem parar. **FIX**: o jni_shim detecta o método Java **`showInterstitial`(I)V**
(GetMethodID name="showInterstitial", chamado por `Android_showMoPubInterstitial` 0x4f9dbc LOGO
após armazenar o cb) e seta `jni_inter_pending`; o main loop dispara `callbackInterstitialAds`
**UMA vez** nesse instante (simula ad fechado, timing perfeito). (commits desta sessão.)

### 🔴 MURO ATUAL / PRÓXIMO PASSO
Mesmo invocando o cb UMA vez no momento perfeito (confirmado `interCB FIRE @frame 1317
showInterstitial->ad closed`), o world map **NÃO é criado** (funcs `dm::world_map::CWorldMap`
0x2576ac/createFileStart 0x257674/task proc 0x25555c **nunca disparam**) — tela continua azul.
⇒ **Invocar o cb armazenado sozinho NÃO cria o world map.** Investigar a SEGUIR:
1. O que o `std::function` cb realmente FAZ ao ser invocado (break no invoker [singleton+12];
   achar o singleton: `callBackInterestitial` carrega via GOT 0x84ef30). Talvez ele só feche
   banners/retome, e a criação real do world map venha de OUTRO gatilho.
2. `CMainMenuStateDecision::Next` (0x22a458): pra qual estado o índice 0 ("Start") roteia de fato
   (3 vtables de next-state). Talvez "Start" precise de **save data** / vá por um caminho diferente
   (SyncSaveData→onMainMenuToWorldMap) que nossos patches de exit desviam.
3. Quem cria a **task `CWorldMap`** (top-level): replicar o gatilho como fizemos pro menu.
4. Hipótese: o world map exige um save válido (stsSavePathData) ou um estado de conta GPG que
   forçamos mas que deixa o fluxo "Start" sem destino.

## Flags
SONIC_AUTOSTART (press único: A@600 título→menu, A@1300 menu Start), SONIC_GLERR/PIXDIAG,
SONIC_KEEPSIGNIN/KEEPJP/KEEPADS/KEEPDEMOGATE/etc. `SONIC_DATADIR=/storage/roms/ports/sonic4`.
Harness: `sh runsonic.sh <segundos>` (mata, lança com env, captura fb). libfox.so vaddr==fileoff
(1:1); device addr = load_base + vaddr (load_base no log: "text=0x...").
