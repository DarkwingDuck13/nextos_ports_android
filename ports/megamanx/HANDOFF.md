# Mega Man X (Capcom) → Mali-450 — HANDOFF completo (para a próxima seção)

## 2026-07-03 s5 — pausa solicitada: Xbox nativo confirmado no Unity, controle ainda bloqueado por keymap vazio

**Estado salvo antes da pausa:** não há processo `megamanx` rodando no device. Último binário foi buildado e enviado para `/storage/roms/megamanx/megamanx`. Runs/logs/screenshot salvos em `runs/`.

**Vitórias confirmadas:**
- **Render:** X e fase continuam limpos. Sem retorno do magenta/bloco amarelo.
- **Full version:** `MMX_FULLVER=1` agora força `ProductInfo` via hooks de `CStoreKit.getProductInfo`/`MakeProductData`; nos runs forçados para fase não aparece mais o gate "BUY FULL VERSION".
- **Xbox nativo existe e está parcialmente funcionando:** o jogo/Unity Input System reconhece o device virtual como `XboxOneGamepadAndroid`; `nativeInjectEvent` retorna `ret=1`; Unity consulta `MotionEvent.getAxisValue(...)` e aceita `KeyEvent`/`MotionEvent`.
- **Regra crítica de teste:** para validar controle, usar `MMX_KEEP_CONTROLKEY=1` ou `MMX_CTRLHOOK=1`. Sem isso, `MMX_FIXGAME` neutraliza `RockmanX.controlKey` e o teste vira falso negativo.

**Controle — achado final antes da pausa:**
- `MMX_CTRLHOOK=1` hooka `RockmanX.controlKey` em `il2cpp+0xdd6d60`.
- Field dump confirmou offsets reais:
  - `KeyFlag +0x2c0`
  - `KeyFlagReal +0x2c8`
  - `key_data +0x2d0`
  - `def_key +0x2d8`
  - `game_key +0x2e0`
  - `touchkey_tbl +0x2e8`
  - `flg_touchKey +0x2f0`
  - `flg_touchPress +0x300`
  - `flg_touchRelease +0x308`
- No run pausado, o auto-pad chega ao hook: `gp=0x2000 act=0x8` = direita ativa. Porém `key_data len=0` e `game_key glen=0` continuam zerados mesmo na fase forçada por `MMX_PROFORCE_SCENE=12`.
- Conclusão prática: **não é mais problema de "o controle não chega". O estado do pad chega. O bloqueio atual é que o fluxo forçado entra na fase sem inicializar a tabela de keymap (`key_data/game_key`).**

**Próximo passo recomendado ao retomar:**
1. Tentar inicializar/forçar o keymap antes ou durante `scn_STAGE_run`:
   - investigar `RockmanX.initGame`/rotas de menu real que preenchem `key_data/game_key`;
   - ou copiar `def_key`/montar arrays para `game_key` se `def_key` estiver preenchido.
2. Plano B mais direto: injetar nos arrays touch do próprio `RockmanX` (`flg_touchKey`, `flg_touchPress`, `flg_touchRelease`, `touchPress_x/y`) em vez de depender de `game_key`.
3. Plano C: manter caminho Xbox nativo e instrumentar `UnityEngine.InputSystem` (`Gamepad`, `InputAction`, `AndroidGamepad`) para descobrir por que a lógica Capcom não consome o gamepad.
4. Áudio segue pendente: FMOD ainda spamma `Cannot create FMOD::Sound instance for clip "0"`/`"114"`; no último run foram muitas linhas de erro. Só atacar depois de controles.

**Runs novos importantes:**
- `runs/2026-07-03-codex-native-xbox-keep-controlkey/`
  - Xbox nativo com `MMX_KEEP_CONTROLKEY=1`.
  - `nativeInjectEvent` aceitou eventos (`ret=1`) e a fase renderizou.
- `runs/2026-07-03-codex-fielddump-rockmanx-input/`
  - Dump confirmou os offsets de `RockmanX` listados acima.
- `runs/2026-07-03-codex-ctrlhook-actlog-pause/`
  - Último run antes da pausa.
  - Prova chave: `CTRLHOOK enter ... gp=0x2000 act=0x8`, mas `key_data len=0` e `game_key glen=0`.
  - `shot.png` salvo com X renderizando limpo na fase.

---

## 2026-07-03 s4 — 🏆 X RENDERIZA NA FASE (shader CutOut resolvido) + infra de controle/fullver

**GRANDE AVANÇO:** o X (que aparecia **magenta/bloco amarelo** na fase) agora **renderiza limpo**. Era o shader `Sprites/CutOut` (sprite do X + cenário) caindo no `Hidden/InternalErrorShader`. Commit `01cbfc7`.

- **Causa-raiz (3 bugs no `transpile_shaders.py`)** — o Unity rejeitava shaders PRÓPRIOS do jogo (os que NÃO existem no Terraria enxertado; Default/TMP funcionavam por serem enxerto do Terraria):
  1. Bloco de macros `UNITY_SUPPORTS_UNIFORM_LOCATION` sem preâmbulo HLSLCC virava GLSL inválido (`#define  #define  #else`).
  2. `precision highp float;` cru no fragment → Mali-450 (Utgard) **não tem highp no fragment** → agora emite o bloco guardado (`#ifdef GL_FRAGMENT_PRECISION_HIGH`).
  3. 🔑 **O trailing de channel/binding (`m_ChannelCount`, ~13 bytes com `19000000`) DEPOIS do GLSL era DESCARTADO** no rebuild do subprograma → Unity: `Failed to load GpuProgram from binary shader data`. Agora preservado.
- **Como reproduzir a fase com o X:** receita do §8 abaixo + `MMX_PROFORCE_SCENE=12 MMX_PROFORCE_HIT=80` (pula direto pra fase via `scn_goLoadScene`, il2cpp+0xdecf2c). Confirmado: intro (rodovia/carro/caminhão/READY) renderiza, 0 `Failed to load GpuProgram`.
- **⚠️ data.unity3d NÃO é versionado.** O device tem o data com o CutOut já corrigido. Pra regerar limpo: `python3 transpile_shaders.py <data.unity3d>` (do APK pristino). O transpilador do repo já está corrigido e validado (guard + trailing).

**Controles — infra pronta (commit `f2b00a0`), FALTA validar em gameplay:**
- O pad físico É lido via SDL (`[MMX_GAMEPAD] SDL pad js0: USB Gamepad`). KeyEvent injetado é aceito (ret=1) MAS o jogo **ignora** (é port mobile: lê `RockmanX.controlKey` = TOUCH da tela, não gamepad Android).
- Implementei **pad→touch (estilo MM5/6)**: `MMX_GP_TOUCH=1` converte o pad em toques nas coords dos controles virtuais (D-pad radial 1 dedo + botões dedos separados), multitouch real no `jni_shim` (`g_mt_*`, getX/getY/getPointerId por índice). Emissão verificada (D-pad (250,530), pulo (1170,610)). Coords tunáveis: `MMX_DP_CX/CY/OFF`, `MMX_JX/JY` (pulo), `MMX_SX/SY` (tiro), `MMX_DX/DY` (dash), `MMX_WX/WY` (arma).
- ⛔ **MURO:** o touch injetado retorna **ret=0** (KeyEvent era ret=1) e o Unity só lê o touch em certos estados. E a fase do PROFORCE é uma **DEMO que volta ao menu** — não deu pra segurar gameplay controlável pra validar. **Próximo:** ou confirmar que o touch chega no `controlKey` em gameplay real, OU hookar `controlKey` pra injetar o bitmask direto.

**Versão completa (fullver) — infra pronta (commit `3415699`), "BUY FULL VERSION" AINDA aparece:**
- `MMX_FULLVER=1` agora **varre todos os métodos** e força a TRUE os getters bool de compra: `CStoreKit.getProductUnlock` (FALTAVA!), `getProductUse`, `HasNumberOfPurchases`, etc. (`MMX_STOREDUMP`/`STOREDUMP2` listam a API).
- ⛔ **MURO:** "BUY FULL VERSION" persiste e a fase continua bounce. Determinante NÃO é o getter bool — o menu lê o **objeto `CStoreKit.ProductInfo` (campo `.use`/`.unlock`)** cacheado por `CStoreKit.MakeProductData()` no boot. **Próximo passo (estava começando):** hookar `getProductInfo(id)` ou `MakeProductData` (via `hook_arm64`, tramp `mk_tramp`) e forçar os campos bool do ProductInfo = 1. Ver API no `STORE2` dump (CStoreKit: `getProductInfo/1 -> ProductInfo`, `setProductUse/2`, `MakeProductData/0`).

### 🎯 MISSÕES PRÓXIMA SEÇÃO (prioridade)
1. **Liberar versão completa DE VERDADE** (destrava gameplay estável, que destrava o teste de controle): hookar `CStoreKit.getProductInfo`/`MakeProductData` e forçar `ProductInfo.use=unlock=true`. Objetivo: "BUY FULL VERSION" some + Story Mode entra em gameplay PERSISTENTE (não a demo que volta).
2. **Controles:** com gameplay estável, validar `MMX_GP_TOUCH` (o X anda/pula?). Tunar coords se o D-pad/botões estiverem no lugar errado. Se o touch não chegar no `controlKey`, hookar `controlKey` e injetar o bitmask do pad direto (estilo MM5/6 dpad-bitmask).
3. **Áudio:** FMOD spamma `Cannot create FMOD::Sound instance for clip "114"/"47"`. Investigar o caminho FMOD no so-loader.

---


> Estado em 2026-07-03 (s3). **Boota, renderiza a tela-titulo real do Mega Man X sem magenta, aceita force-start ate o menu/tela "Buy Full Version", e o touch Android ja entra no parser da Unity.** O muro antigo `draws=0` foi superado. O foco atual e contornar o gate de versao completa/menu para chegar em gameplay, depois fechar controles e audio.

## 2026-07-03 s3 — avanço critico salvo

- **Visual/shaders resolvidos:** a tela-titulo aparece limpa. A tela magenta antiga era `Hidden/InternalErrorShader`, principalmente TMP/texto, nao clear color.
- **Payload atual bom:** `payload/assets/assets/bin/Data/data.unity3d`
  - SHA256: `197d3481015ae047e47d424be8a8bb3d73562e6465c0092581166767bdaf0edc`
  - Tamanho: ~155 MiB, UnityPy uncompressed.
  - Base: `runs/2026-07-03-codex-shader-sentinel-data/data.unity3d.ref-ter-overlap`
    SHA256 `8d882d2e49935cf590f3105d6eb0d297e42b5785af35e300778e5ca68c167fe9`.
  - Fix final: `tmp-textcore-alias`, copiando o corpo real do shader Terraria `Hidden/TextCore/Distance Field SSD` para os shaders TMP do MMX em `resources.assets` pathIDs 388-398, preservando nomes.
- **Run visual de referencia:** `runs/2026-07-03-codex-shader-tmp-textcore-alias`
  - Screenshot limpo SHA256 `4c9da24dca2a5a8ada814cafaf50b4eed5c6ea16a60b495f64e4b5644ecffacc`.
  - Magenta pixels cairam para 0.
- **Input Android parcial:** `MMX_AUTOTOUCH=1` injeta `MotionEvent`; com `MMX_TOUCHLOG=1`, Unity chama `getPointerCount`, `getActionMasked`, `getX`, `getY`. Isso prova que o evento entra, mas a checagem propria da tela-titulo nao aceita o toque ainda.
- **Force-start temporario:** `MMX_FORCE_TITLESTART=1` NOPa o `cbz` em `scn_TITLE_run+0x20c` (`libil2cpp+0xdf53d0`) e faz a tela-titulo avancar para `scn_TITLEMENU`. O usuario viu "Buy Full Version" no device; `MMX_RXSPY=scene` confirmou `scn_TITLEMENU_run/draw` rodando.
- **Controle real ainda pendente:** `MMX_KEEP_CONTROLKEY=1` deixa `RockmanX.controlKey` ativo sem crash imediato, mas o touch ainda nao muda a tela-titulo sozinho.
- **Audio ainda pendente:** no menu/tela seguinte o log spamma FMOD `Cannot create FMOD::Sound instance for clip "114"`. Na tela-titulo aparece clip `"47"`.
- **Nao usar PixelCup como referencia:** o usuario confirmou que PixelCup ainda nao roda. A referencia util para shaders continua sendo Terraria.

---

## 0. TL;DR — o que fazer na próxima seção

1. Ler este arquivo + `STUDY.md` + a memória `project_megamanx_mali450`.
2. O muro antigo **`draws=0` foi resolvido**. Nao volte para o diagnostico de BootScene/Addressables como problema principal sem nova evidencia.
3. Proximo passo concreto: resolver a tela/menu **"Buy Full Version"**. Suspeita principal: patches IAP/prefs atuais retornam `false` para algo que deveria indicar produto comprado/full unlock (`CStoreKit.getProductUse`, `getProduct`, prefs criptografadas ou metodo equivalente).
4. Depois de passar do menu: fechar controles reais. `MMX_AUTOTOUCH` entra no parser Android da Unity, mas talvez o jogo use `RockmanX.controlKey`/estado proprio em vez do toque cru.
5. Audio FMOD fica para depois do primeiro gameplay, mas ja esta marcado: clips `47`/`114` falham.

---

## 1. O jogo

- **APK:** `~/Downloads/mega-man-x-androeed.store-0-1730359376.apk`
- **Engine:** Unity **2021.3.39f1 IL2CPP**, metadata **v29**, **arm64-v8a só** (não tem armv7).
- Pipeline **built-in** (SEM URP). Exige **GLES3.0**. Conteúdo 100% local (sem download).
- **SEM pairip** (o LVL/DRM foi crackeado no APK), MAS tem **Google Play Integrity** próprio (classe `IntegrityCheck`) — contornado (ver §4).
- `libil2cpp.so` **exporta a API `il2cpp_*` (236 símbolos)** → dá pra resolver classes/métodos por `dlsym`, sem hardcodar offset. É o que o `mmx_i2sym()` faz.
- Tem japonês (regra #5: forçar inglês) — **ainda pendente**, só lidar quando renderizar.

## 2. Base do port = Terraria

- Port em `nextos_ports_android/ports/megamanx/`. Base = `ports/terraria` (Unity 2021.3.56 IL2CPP, mesma geração).
- Copiei `src/` inteiro (so_util/egl_shim/jni_shim/opensles/pthread_fake/sem_shim/native_pad/gamepad/renderscale/imports.gen.c/etc).
- `main.c` = do Terraria (hooks Cuphead/Terraria ficam getenv-gated, inertes por padrão). Adicionei os blocos `MMX_*` (ver §7).
- `jni_shim.c`: identidade corrigida — pacote `jp.co.capcom.rockmanx` (era `com.and.games505.TerrariaPaid`), paths `megamanx`.
- `boot.config` = do Terraria (`androidUseSwappy=0` + `gfx-disable-mt-rendering=1`).

## 3. Device / build / deploy / debug

- **Device de trabalho:** Mali-450 Amlogic-old, EmuELEC 4.8, **IP `192.168.31.79`**, `root` sem senha.
  - SSH leve (senão o KEX pós-quântico não fecha): `ssh -o KexAlgorithms=curve25519-sha256 root@192.168.31.79`
- **Build:** `bash build.sh` no diretório do port (usa toolchain aarch64 do NextOS-Elite). Sai `megamanx` (ELF aarch64). Os 2 warnings de `libSDL2.so bad subsection length` são benignos (linkagem OK, "BUILD OK" aparece).
- **Deploy binário:** `scp -o KexAlgorithms=curve25519-sha256 megamanx root@192.168.31.79:/storage/roms/megamanx/`
- **Payload do jogo** (assets/lib do APK) fica em `/storage/roms/megamanx/` no device e em `ports/megamanx/payload/` no host (NÃO versionado — ver `.gitignore`).
- **Rodar com segurança (OBRIGATÓRIO):** `ports/megamanx/dbgrun.sh` — `nice -n 19` + `timeout` + watchdog + mata instância antiga por **exe-symlink** (`/proc/*/exe`, NÃO `pgrep -f path` porque o cmdline é relativo `./megamanx`).
  - Uso: `sh /storage/roms/megamanx/dbgrun.sh <SEGUNDOS> VAR1=x VAR2=y ...`
  - 🚨 **NUNCA** rodar o jogo/gdb no device sem `timeout` na PONTA DO DEVICE → busy-wait asfixia o sshd → **perde o device, precisa reboot físico**. Já aconteceu 2×. Ver memória `feedback_debug_device_asfixia_harness`.
- **Log:** o `dbgrun.sh` grava em `/storage/roms/megamanx/debug.log`. `grep` nele por SSH.
- **Framebuffer (ver o que tá na tela):** `dd if=/dev/fb0 of=/storage/roms/megamanx/fbr.raw bs=1M count=4`, `scp` pro host, converter RGBA→PNG (BGRA na verdade: trocar canais B↔R). 1280x720. Magenta uniforme (255,0,255) = clear color = cena vazia (não é error-shader).

## 4. ✅ RESOLVIDO — Play Integrity DRM (`MMX_NOINTEGRITY=1`)

- `BootScene.Start` faz `await IntegrityCheck.Start()`. **`IntegrityCheck.Start()` retorna um `IEnumerator` (coroutine Unity), NÃO um `UniTask`** — o await é via Cysharp **`EnumeratorAsyncExtensions.GetAwaiter[T]`**.
- ⚠️ **ARMADILHA (erro que cometi e corrigi):** patchar `IntegrityCheck::Start → mov x0,xzr; ret` (retornar null) faz `GetAwaiter(null)` lançar **`ArgumentNullException: Value cannot be null`** → boot morre → draws=0. Foi o que travou por horas achando que era "UniTask".
- ✅ **FIX CERTO (no `mmx_nuke_integrity()`, `main.c` ~linha 387):**
  1. `get_IsSuccess → mov w0,#1; ret` (integrity "passou").
  2. DEIXAR `Start()` rodar (cria+retorna o enumerator real, não-null) e patchar o **`MoveNext` do state-machine aninhado `IntegrityCheck/<Start>d__4` → `mov w0,#0; ret`** (MoveNext=false = enumeração terminada na hora, enumerator válido, await completa SEM rodar a chamada Play Integrity que crasha).
  - Achado via `il2cpp_class_get_nested_types` (pega o nested cujo nome contém "Start") + `get_method_from_name("MoveNext")`.
- **Resultado: a `ArgumentNullException` sumiu, a integrity passa limpa.** (log: `[NOINTEGRITY] IntegrityCheck.<Start>d__4::MoveNext -> false`).

## 5. ✅ RESOLVIDO — boot job-system + shaders GLES3→GLES2

- **Boot (job-system):** `MMX_INLINETASK=1 MMX_PATCH=0x34eafc=0x14000005`
  - `MMX_INLINETASK`: trampolim em `libunity+0x350580` (per-object-task loop) chama `mmx_inline_task(obj)` que finge conclusão (`node=*(obj+88); *node=1`) → sai da espera `0x35059c`. Análogo ao Terraria `0x2f37a4`.
  - `MMX_PATCH=0x34eafc`: `WaitForJobGroup` (`b.ge`→`b`) sai imediato. ⚠️ **Isto quebra o load assíncrono** (ver §6). Sem isso → deadlock no frame 2.
- **Shaders:** o jogo só traz shaders GLES3 (platform 9) + Vulkan (18), ZERO GLES2. O Mali-450 é GLES2 puro. Solução: **`transpile_shaders.py`** (roda OFFLINE no host, in-place no `data.unity3d`):
  - Traduz o GLSL HLSLcc `#version 300 es → 100`, `in/out → attribute/varying` (por stage), flatten de UBO (`uniform BLOCK {…}` → uniforms soltos), remove `layout`/`flat`, `texture()→texture2D()`, `out vec4 → gl_FragColor`, adiciona precision.
  - Muda `programType` interno GLES3(2/3/4)→GLES2(5) SEMPRE + **atualiza o campo LENGTH da entrada** (offset,LENGTH,0) + relabela platform 9→5.
  - **Shaders do jogo COMPILAM+LINKAM 0 falhas.** 🔑 Descoberta: o Unity resolve as funções GL via **`dlsym(RTLD_DEFAULT)`, NÃO `eglGetProcAddress`** — o hook `MMX_XLATE` (glShaderSource) mora no `my_dlsym`.
  - Rodar de novo se reextrair o data: `python3 transpile_shaders.py /caminho/data.unity3d` (precisa `UnityPy` + `lz4`).

## 6. ⛔ MURO ATUAL — `draws=0` (cena de boot não carrega)

- Sintoma: roda 60fps estável, 0 exceções agora, mas `[FPS] draws/f=0` sempre. FB = magenta (clear).
- Diagnóstico: `BootScene.Start` passa da integrity mas **parkeia num `await` posterior sem lançar exceção**.
- **Hipótese principal:** esse await é o **load assíncrono de cena/Addressables**, que usa o job-system pra I/O async. Como o `MMX_PATCH` faz `WaitForJobGroup` retornar "pronto" sem os jobs terem rodado, o load nunca completa de verdade → cena nunca ativa → nenhum renderer → draws=0.
- **Coisas já descartadas como blocker:** transpile (data original tbm dava draws=0), input (getDevices=áudio, benigno), workers acordam mas o main trava ANTES de dispatchar.

### Dois caminhos para atacar o muro:

**(A) Descobrir exatamente onde trava (rápido, diagnóstico):**
- Inline-hook no `BootScene.<Start>d__5::MoveNext` = **`il2cpp_base + 0xe3b438`** logando `*(int*)(coro + 0x10)` (o `<>1__state`).
  - Campos da coroutine (do BOOTDUMP): `+0x10 <>1__state:int`, `+0x18 <>t__builder:AsyncVoidMethodBuilder`, `+0x38 <>4__this:BootScene`, `+0x40 <>u__1:UniTask.Awaiter`.
  - Já existe `void mmx_bootmn(void*)` no `main.c` (~linha 258) que loga o state — mas trocar methodPointer NÃO funciona (delegate já cacheado). **Usar `hook_arm64(il2cpp_base+0xe3b438, ...)`** (inline no endereço de código, funciona apesar do cache). Ver exemplo `CUP_CRSPY` linha ~4937.
  - Saber o state onde ele congela → resolver o `bl` target daquele bloco (via `MMX_RESOLVE`, que mapeia offset→`Classe::método`) → é o método que não completa.

**(B) Consertar o job-system de verdade (a correção "certa"):**
- Fazer o Unity rodar os jobs INLINE na própria thread: chamar `JobsUtility.JobWorkerCount = 0`.
- Existe `ter_jobworkers0()` no `main.c` (~linha 446) MAS usa offsets `il2cpp` **HARDCODADOS do Terraria** (`0x73c860` etc) — **ERRADOS pro Mega Man X**. Precisa reescrever pra resolver `il2cpp_domain_get`/`assemblies`/`class_from_name`/`runtime_invoke` via **API exportada** (`mmx_i2sym`, que o `mmx_nuke_integrity` já usa).
- Se `JobWorkerCount=0` funcionar, o `WaitForJobGroup` original (SEM o skip `MMX_PATCH`) deve completar sozinho → tirar o `MMX_PATCH` e ver se o load async anda.

## 7. Ferramentas de debug (env vars, todas getenv-gated no `main.c`)

- `MMX_INLINETASK=1` — trampolim per-object-task (destrava boot). **Essencial.**
- `MMX_PATCH=0xADDR=0xWORD` — escreve uma word de 32 bits no `.text` do libunity. Recipe usa `0x34eafc=0x14000005` (skip WaitForJobGroup).
- `MMX_NOINTEGRITY=1` — bypass Play Integrity (§4). **Essencial.**
- `MMX_PREFSTRUE=1` — `getBoolean` de prefs retorna 1 (aceita termos/EULA salvos).
- `MMX_BOOTDUMP=1` — enumera métodos/campos da coroutine do BootScene + imprime o methodPtr/offset do MoveNext. (foi assim que achei `0xe3b438`).
- `MMX_INTDUMP=1` — enumera métodos/campos da IntegrityCheck.
- `MMX_RESOLVE=1` — mapeia uma lista de offsets (`bl` targets) → `Classe::método`.
- `MMX_NOTERMS=1` — nuka viewTerms/viewParentCtrl/viewError (⚠️ retornam IEnumerator; nukar pra null quebra igual à integrity — provavelmente NÃO usar, ou aplicar a mesma técnica de MoveNext→false).
- `MMX_WORKERPOLL=N` — futexpoll só nas threads não-main (job workers).
- `MMX_XLATE` — hook glShaderSource/Compile/Link (transpile em runtime; não precisa pois o data já foi transpilado offline).
- `CUP_DRAWCOUNT=1` — liga o contador `[FPS] draws/f`.
- Diversos `CUP_*`/`TER_*` herdados do Terraria/Cuphead (inertes por padrão).

## 8. Receita estável atual (para reproduzir o estado)

```
sh /storage/roms/megamanx/dbgrun.sh 45 \
  MMX_INLINETASK=1 MMX_PATCH=0x34eafc=0x14000005 \
  MMX_NOINTEGRITY=1 MMX_PREFSTRUE=1 CUP_DRAWCOUNT=1
```
→ boota, 60fps, integrity limpa, shaders compilam, `draws=0`.

## 9. Regras do projeto (memória) que valem aqui

- Debug SEMPRE via `dbgrun.sh` (nice+timeout+watchdog) — asfixia perde o device.
- Commit direto no `master`, sem co-autor/atribuição Claude, chamar o usuário de "usuário" (nunca nome/email real).
- Nunca forçar `SDL_VIDEODRIVER`/`SDL_AUDIODRIVER` (vêm do sistema).
- Payload do jogo (assets/.so/data.unity3d) e o binário NÃO vão pro git (`.gitignore`).
- Commitar na hora que o usuário der "ok"/"funcionou".

## 10. Arquivos-chave

- `src/main.c` — o so-loader (300K). Blocos MMX_* citados acima com números de linha aproximados.
- `src/jni_shim.c` — JNI + identidade + `getBoolean` (MMX_PREFSTRUE).
- `transpile_shaders.py` — transpilador de shader offline.
- `dbgrun.sh` — harness de debug seguro no device.
- `run.sh` — launcher (recipe embutida).
- `STUDY.md` — a jornada completa documentada.
- Memória: `project_megamanx_mali450.md`, `feedback_debug_device_asfixia_harness.md`.
