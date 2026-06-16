# Streets of Rage 4 — Port NextOS (MonoGame/.NET) — HANDOFF / diário de bordo

Objetivo: rodar **Streets of Rage 4** (APK Android v1.4.5) no device **192.168.31.127**
(Mali-450). Trabalho autônomo, commitando cada conquista. Plano completo aprovado em
`~/.agente/plans/polymorphic-weaving-leaf.md`.

> Atualizar este arquivo a cada descoberta/decisão (padrão NFS/Banjo). Convenção do projeto:
> registrar SEMPRE o estado git. Commits em PT, SEM Co-Authored-By.

---

## Fatos confirmados do APK (FASE 0/1)
APK fonte: `/home/nextos/Downloads/Streets-of-Rage-4-v1.4.5-unlocked-apkvision(1).apk`
(1,9 GB, 27.234 arquivos).

- Engine = **MonoGame + .NET-for-Android (MonoVM)**. NÃO é Unity, NÃO é FNA.
- Libs nativas: `libmonosgen-2.0.so`, `libmonodroid.so`, `libxamarin-app.so`,
  `libassemblies.arm64-v8a.blob.so` (assembly store), `libSystem.*.Native.so`,
  `libopenal.so`, `libWwise.so`, `libfreetype.so`, `libharfbuzz.so`,
  `libEOSSDK.so`, `libpairipcore.so` (PAIRIP anti-tamper), `libstub.so`.
- Assembly store com **~99 PE (MZ) em claro** → assemblies extraíveis.
  Nomes vistos: `MonoGame.Framework.dll`, `System.Private.CoreLib.dll` (.NET moderno/8),
  `Mono.Android.Runtime.dll`, e o jogo provável: `StandaloneTypeModel.Android.Retail.dll`
  (+ DLLs ofuscadas curtas: `Qe.dll`, `Ts.dll`, etc.).
- Assets: `.xnb` (conteúdo MonoGame) em `assets/`, + banks Wwise.

## Estratégia (decidida)
**NÃO so-loader** (lógica é C# gerenciado, não nativa). Caminho =
**runtime .NET nativo + MonoGame DesktopGL/GLES**:
extrair assemblies → rodar em .NET 8 arm64 no device → host próprio (Program.Main sem
AndroidGameActivity) → MonoGame DesktopGL → gl4es p/ GLES2 no Mali-450 → stubar
Mono.Android/EOS/Helpshift/Billing/pairip.

## Device 192.168.31.127 (recon FASE 0)
- Mali-450 Utgard, **GLES2-only**, **fbdev** (/dev/fb0, /dev/fb1, sem /dev/dri).
- Kernel 3.14.79 EMUELEC aarch64, **glibc 2.43** (moderno — bom p/ .NET 8), 4 cores.
- `/storage` = 996 MB livres (NÃO usar p/ dados). **`/storage/roms` (p3) = 21 GB livres** → usar.
- GL: `/usr/lib/libMali.m450.so` (=libMali.so=libEGL/libGLESv2). **gl4es: `libEGL_gl4es.so.1`**.
- SDL: `libSDL2-2.0.so.0.3200.69` (2.32, provável mali) + SDL3. `SDL_VIDEODRIVER=mali` p/ fbdev.
- gptokeyb em `/storage/roms/ports/PortMaster/`.
- **Sem runtime .NET/Mono no device** (só LÖVE em PortMaster/runtimes). → prover runtime nós mesmos.
- SSH: `root@192.168.31.127` (sem senha via chave já configurada). Regra: nunca relançar sobre
  instância viva (matcher por /proc/PID/exe — ver `ports/cuphead/run.sh`).

## Convenções de launcher (do port Cuphead, reaproveitar)
- GAMEDIR = `/storage/roms/<jogo>`; launcher device-aware (fbdev→mali / kmsdrm→x5m).
- fbdev: `SDL_VIDEODRIVER=mali LD_LIBRARY_PATH=/usr/lib`, EGL real do Mali.
- Matar instância viva por inode antes de lançar; `nohup ./bin > run.out 2>&1 &`.

---

## GO/NO-GO gates
- [ ] **A** assemblies extraíveis em IL legível
- [ ] **B** contexto GL a partir de C# no device (gl4es)
- [ ] **C** jogo sobe sem dep Android fatal
- [ ] **D** primeira imagem (objetivo central)

## Riscos abertos
- Runtime .NET 8 arm64 em kernel 3.14 (glibc 2.43 ajuda; testar; fallback self-contained/Mono).
- Acoplamento Mono.Android / pairip dentro do código do jogo.
- Shaders `.xnb` (DX bytecode) → MojoShader/GL (pode exigir recompilar conteúdo de Effect).

---

## GATE C — boot: MUITO PERTO (2026-06-16 noite) — crash na 1ª carga de asset
**Estado atual exato** (cadeia de logs `[MG]` confirma): boot vai até dentro do
`OnDeviceCreated` → `PreloadingScreen.Initialize()`:
1. `new SpriteEffect` ✅  2. `Effect ctor(byte[])` ✅  3. `new SpriteBatcher` ✅  4. SpriteBatch ctor **COMPLETO** ✅
5. **CRASH** em `asset_cache.get<TextureProxy>("gui/mobile/left_filler")** — segfault NATIVO,
   ANTES de `Texture2D.PlatformConstruct` (sem log `[MG] Tex2D`) e ANTES do `AssetManager.Open`
   gerenciado (sem log `[asset]`).

**Causa provável**: `asset_cache.get` chama `CommonLib.AssetManagerWrapper.get_AssetManager()` +
`.Exists(string)` ANTES do `Context.Assets.Open()`. O `AssetManagerWrapper` provavelmente usa um
**handle de asset nativo (NDK AAssetManager)** derivado do AssetManager Java — que no meu stub é
um objeto uninitialized (`GetUninitializedObject`, Handle=0/lixo) → ponteiro inválido → segfault
nativo. (O managed `AssetManager.Open` JÁ está bridgeado p/ filesystem e funcionaria se alcançado.)

**PRÓXIMO PASSO CLARO (retomar aqui)**:
- Dumpar IL de `CommonLib.AssetManagerWrapper::Exists(string)` e `::get_AssetManager()` (achar o
  P/Invoke nativo / uso do Handle).
- Fix provável: Cecil-patchar no SOR4.dll `AssetManagerWrapper.Exists`→`return true` e forçar o
  caminho do `Context.Assets.Open()` (já bridgeado), OU bridgear o método nativo de asset.
  Infra de patch pronta: `port/tools/injector` (Cecil).
- Assets: já extraí `gui/preload`+`gui/mobile`+`shader` em `build/game_assets` (~11MB). O resto
  (1.9GB) vai p/ device/R2 quando a 1ª imagem sair.
- Depois: shaders custom `.xnb` (15, GLSL ES) devem compilar no meu MonoGame GLES; validar.

**Notas técnicas acumuladas (GATE C)**:
- `Threading.BlockOnUIThread` (Platform/Threading.cs): roda inline se `IsOnUIThread()` senão
  enfileira p/ `Threading.Run()` (game loop). Durante OnDeviceCreated o loop NÃO roda → se algo
  carregar fora da UI thread, trava. Texture2D.PlatformConstruct usa BlockOnUIThread.
- O jogo tem P/Invoke "Wwise" (áudio nativo) e `Wwise.load_bank` — tratar na FASE 5.
- ⚠️ MUITOS logs `[MG]` de debug no source do MonoGame (GraphicsContext/GraphicsDevice/Game.cs/
  SpriteBatch/Shader/Texture2D/Threading) + `[host]`/`[asset]` — GATEAR por env (ex: SOR4_TRACE)
  ou remover antes do build final. `SOR4_DUMPGLSL=1` dumpa GLSL dos shaders.
- Pacote de teste no device: `/storage/roms/sor4-test/host_pkg`. Rodar (parar ES):
  `LD_LIBRARY_PATH=libs:/usr/lib SDL_VIDEODRIVER=mali DOTNET_EnableWriteXorExecute=0
   SOR4_ASSETS=$PWD/assets ./sor4host`.

## GATE C — boot do jogo: QUASE (2026-06-16) — carrega tudo, crash no OnDeviceCreated do jogo
**Conquistado**: o host `sor4host` (`port/host/`) **carrega SOR4.dll + 15 stubs Android + MonoGame
GLES**, roda `CommonLib.xna.CreateGame()` (cria `CommonLib.xna+StandaloneGame` : Game), e
`game.Run()` inicializa o **contexto GLES + GraphicsDevice 100%** (PlatformInitialize, FBO,
states, ApplyRenderTargets — tudo OK). 
**Crash atual**: segfault nativo (139) dentro de **`GraphicsDeviceManager.OnDeviceCreated`** →
handler de DeviceCreated DO JOGO (em SOR4.dll). É crash NATIVO (não NullRef gerenciado, pois
funções GL ausentes retornam null=NullRef; logo é Mali driver/gl4es/Wwise ou GL call inválida).
Suspeitos: RenderTarget2D/`glDrawBuffers` (GLES3, nil no Mali GLES2), shader compile, ou Wwise.

**Infra de boot que funciona**:
- Stubs Android (15) via `port/tools/stubber/` (Cecil: zera corpos + retarget corelib→System.Runtime).
- MonoGame compat `SOR4Compat.cs`: `AndroidGameActivity:Android.App.Activity`(stub) + `Game.Activity`.
- Host: AssemblyLoadContext.Resolving resolve dlls do dir por nome simples (ignora versão/PKT) →
  SOR4 referencia MonoGame 3.8.3.1 strong-named mas carrega meu unsigned (Core é leniente). ✓
- Pacote em device `/storage/roms/sor4-test/host_pkg`. Rodar: parar ES, `LD_LIBRARY_PATH=libs:/usr/lib
  SDL_VIDEODRIVER=mali DOTNET_EnableWriteXorExecute=0 ./sor4host`.
- Sequência do jogo (de MainActivity.OnCreate): helpshift/firebase/Wwise preinit → `xna.CreateGame()`
  → SetContentView(pulado) → `game.Run()`.

**PRÓXIMO (GATE C/D)**: obter backtrace NATIVO do crash no OnDeviceCreated
(`DOTNET_EnableCrashReport=1` → json com stack nativo+gerenciado). Provável fix: guardar
`glDrawBuffers`/render-target p/ GLES2 no MonoGame, ou stubar Wwise. Depois: extrair assets p/ o
device (ContentManager desktop lê do filesystem) p/ a 1ª imagem.
⚠️ Logs `[MG]` de debug espalhados (GraphicsContext/GraphicsDevice/Game.cs) — gatear por env depois.

## GATE B = PASS COMPLETO ✅✅ (2026-06-16) — MonoGame GLES RENDERIZA no Mali-450
**Buildei MonoGame.Framework GLES próprio** (híbrido SDL+GLES, net9, v3.8.3.1) do source 3.8.4
e ele **renderiza 20 frames (Clear cornflower) no Mali-450 MP, EXIT=0**. Pipeline completo OK:
.NET 9 + MonoGame GLES + sdl2-compat → SDL3-mali → Mali GLES2.

Patches (reproduzíveis em `port/monogame-gles-patches/`: apply.py + csproj + README):
1. `GraphicsDeviceManager.SDL.cs`: pede contexto SDL **ES profile 2.0** sob `#if GLES`.
2. `OpenGL.SDL.cs`: `BoundApi=ES` sob GLES.
3. `RasterizerState.OpenGL.cs`: `PolygonMode` (desktop-only, nil no GLES) gateado `&& !GLES`.
4. csproj `MonoGame.Framework.SOR4GLES.csproj`: defines `OPENGL;GLES;...;DESKTOPGL`, StbImage via
   NuGet, AssemblyVersion 3.8.3.1, net9, exclui Sensors.
GL cru do device (sonda): Mali-450 MP, OpenGL ES 2.0, GLSL ES 1.00, VAO via OES, FBO core.
Build do MonoGame: `~/.dotnet` SDK 9; saída `/tmp/mgout/MonoGame.Framework.dll`.
Teste: `build/gltest/` (ProjectReference ao csproj GLES). Deploy `/storage/roms/sor4-test`.
⚠️ Há logs de debug `[MG]` temporários em GraphicsContext.SDL.cs e GraphicsDevice.OpenGL.cs (gatear/remover depois).
Restam desktop-only não-tratados (MapBuffer/UnmapBuffer em GetData, DrawBuffer em RenderTarget) —
só crasham se usados; tratar sob demanda.

Próximo: FASE 3 (GATE C) — bootar SOR4.dll com stubs (Mono.Android/EOS/etc) no host GLES.

## GATE B (parte 2) — windowing/GL: PROGRESSO + DECISÃO (2026-06-16)
**Conquistado**:
- Cross-compilei **sdl2-compat** p/ aarch64 (`build/sdl2-compat/build-arm64/libSDL2-2.0.so.0.3200.71`,
  só linka libc, dlopen SDL3). MonoGame carrega via ele → SDL3-mali do device.
- MonoGame DesktopGL **cria janela + contexto GL na tela** (fbdev) via sdl2-compat→SDL3-mali,
  QUANDO o frontend (ES) não segura o /dev/fb0. → **windowing resolvido**.
- Único erro restante do DesktopGL: `requires ARB/EXT_framebuffer_object` — o contexto é **GLES2
  nativo do Mali** (não tem as extensões desktop).

**Aprendizados-chave**:
- O driver **mali-fbdev do SDL3 exige o EGL real do Mali** p/ criar a window surface; sombrear
  `libEGL.so.1` com gl4es-EGL QUEBRA a criação da superfície. → não dá p/ enfiar gl4es por aí.
- **gl4es é BECO SEM SAÍDA para este jogo**: os Effects `.xnb` foram compilados para o **MonoGame
  GLES (Android)** = GLSL ES. Só carregam num MonoGame GLES; o DesktopGL usa MojoShader/GLSL
  desktop e não consome esses shaders. Logo, traduzir desktop-GL→GLES (gl4es) não resolve shaders.
- A ES segura o fb0 → launcher precisa parar o frontend antes (matcher /proc/PID, ver cuphead run.sh).
- `/storage/roms` é **vfat (sem symlink)** → copiar .so com nome real (não ln -s).
- net8 vs net9: MonoGame DesktopGL 3.8.4 público é net8; meu app net9 quebra (System.Collections
  8.0.0.0). Resolvido p/ teste usando net8; no jogo real uso a versão do jogo (net9).

**DECISÃO (caminho do GL)**: **buildar MonoGame 3.8.x do source em modo GLES nativo + net9**
(mesmo sabor do Android, sem deps Android). Casa com os shaders `.xnb` do jogo e roda direto no
Mali GLES2. Windowing já funciona (sdl2-compat→SDL3-mali). gl4es descartado.
Próximo: clonar MonoGame, achar a config GLES (`#if GLES`/OpenGL backend), buildar
MonoGame.Framework.dll GLES net9 v compatível com 3.8.3.1.

**Análise de assets (define rota p/ 1ª imagem)**: 25.224 `.xnb` (quase tudo TEXTURA/sprite),
**ZERO arquivos de shader** (.fx/.mgfx/.glsl). Áudio = Wwise (613 `.wem` + 4 `.bnk`).
Vídeo `.mp4` (cutscenes), fontes `.otf/.ttf` (SharpFont/FreeType). → a **1ª imagem (título/menu)
precisa só de SpriteBatch (shader GLSL-ES EMBUTIDO no MonoGame) + texturas + fontes**. Effects
customizados (se houver, embedded) são poucos e ficam p/ depois. Forte validação da rota GLES.

**Insight SpriteBatch**: o shader do SpriteBatch vem EMBUTIDO no MonoGame.Framework (não do .xnb
do jogo). Build GLES → SpriteEffect em GLSL ES → roda no Mali GLES2. Texturas .xnb são
backend-agnósticas. Por isso a 1ª imagem é alcançável mesmo antes de Effects customizados.

**Versão/assinatura p/ o swap final**: SOR4.dll referencia MonoGame.Framework 3.8.3.1 (strong-named).
Meu build precisa: mesmo PublicKeyToken (assinar com .snk público do repo MonoGame) + AssemblyVersion
3.8.3.1 (ou usar AssemblyLoadContext/resolver p/ redirecionar). Tratar no swap.

## GATE B (parte 2) — windowing/GL: estratégia (HISTÓRICO)
**Muro**: device é Mali-450 **fbdev-puro** (sem /dev/dri, sem X, sem wayland compositor).
- SDL2 do sistema (2.32.69): drivers = x11/kmsdrm/wayland/vivante → **NENHUM serve**.
- **SDL3 do sistema (3.5.0-HEAD) TEM `mali`+`fbdev`+`offscreen`** → é como o 3SX roda.
- gl4es completo presente: `/usr/lib/libGL.so(.1)` (desktop GL sobre Mali/EGL) + `libEGL_gl4es.so.1`.
- MonoGame DesktopGL usa **SDL2** e NÃO traz nativos arm64 (só x64) → preciso prover libSDL2.

**Decisão**: bridge **sdl2-compat** (libSDL2-2.0.so.0 implementada sobre SDL3) → reusa a
SDL3-mali que já funciona no device. Cross-compilar p/ aarch64 (dlopen SDL3 em runtime).
Rodar com `SDL_VIDEODRIVER=mali` + `LD_LIBRARY_PATH` incluindo gl4es libGL.
- Fallback se sdl2-compat falhar (ABI SDL3 preview): SDL2 com driver **offscreen** (EGL Mali) +
  shim de present `glReadPixels→/dev/fb0` (técnica Shadowflare/NFS).

Ferramentas host OK: `aarch64-linux-gnu-gcc`, clang 22, SDL3 headers (3.4.10), cmake/ninja.
Nota MonoGame: 3.8.3.1 não está no NuGet → DesktopGL resolve **3.8.4** (validar GL com 3.8.4;
casar versão exata p/ o swap no jogo fica p/ FASE 3 — possível build do MonoGame do source).

## GATE B (parte 1) = PASS ✅ — .NET 9 CoreCLR RODA no .127
Hello-world **self-contained .NET 9 linux-arm64** (CoreCLR) executou no device:
`.NET 9.0.17 OK on Arm64`, GC + thread + EXIT=0. Kernel Linux 3.14.79 mas userland
NextOS 4.8.2 / glibc 2.43 → CoreCLR feliz. Flag usada: `DOTNET_EnableWriteXorExecute=0`.
- Host: SDK .NET 9.0.315 instalado em `~/.dotnet` (via dotnet-install).
- Build de teste: `build/hello/` (`dotnet publish -c Release -r linux-arm64 --self-contained`).
- **Conclusão**: caminho gerenciado é VIÁVEL. Runtime resolvido. Falta GATE B parte 2 (GL).

## GATE A = PASS ✅ (FASE 1) — inventário
Assembly store = formato **XABA v2**, 58 assemblies em **LZ4 (`XALZ`)**.
Extração: parsear header XABA (5×u32) → tabela índice (116×12B) → descritores (stride 28B,
campos mapping_index/data_offset/data_size) → `lz4.block.decompress` por entry.
Script de extração reproduzível: ver comando no log; saída em `build/extract/asm/asm_NNN.dll`.
Inventário completo: `port/ASSEMBLIES.md`. Destaques:
- **`SOR4.dll` (1.49MB) v1.0.0.0 = código do jogo** (NÃO ofuscado).
- **`StandaloneTypeModel.Android.Retail.dll` (1.09MB)** = modelo/lógica (limpo: só ref SOR4+System).
- **`MonoGame.Framework` v3.8.3.1** (Android/GLES build) — versão a casar no swap DesktopGL.
- **.NET 9** (System.Private.CoreLib 9.0.0.0; todos System.* = 9.0). [corrige estimativa .NET 8]
- Stubs: Mono.Android, Java.Interop, EOSSDK.Android, HelpshiftSDKx.Android,
  Xamarin.GooglePlayServices.*, BillingClient, Firebase, PlayCore, SharpFont.Core.

### Acoplamento Android (medido) — `port/ANDROID-SURFACE.md`
SOR4.dll usa **116 typerefs Android/Java**, mas concentrados:
- **Funcionais (precisam stub útil)**: `Context`/`Activity`/`Application` (paths),
  `AssetManager` (carregar .xnb → bridge p/ filesystem), `ISharedPreferences(+Editor)` (settings).
- **Serviços (stub no-op/offline)**: Google Play Games (achievements/leaderboards/cloud-save),
  Google Sign-In, Billing/DLC, EOS, Helpshift, Firebase, PlayCore.
StandaloneTypeModel = sem Android. → acoplamento isolado em 1 arquivo, gerenciável.

## Alvo de runtime (revisado)
- **.NET 9 linux-arm64** (CoreCLR oficial) — game é net9.0. Mono 6.12 clássico do host NÃO serve.
- Risco a validar JÁ na FASE 2: CoreCLR .NET 9 roda em **kernel 3.14** (glibc 2.43 ajuda;
  knobs: `DOTNET_EnableWriteXorExecute=0`, `DOTNET_GCgen0size`, etc.). Fallback: MonoVM .NET 9.
- MonoGame **DesktopGL 3.8.3.1** (SDL2 + OpenGL) → gl4es p/ GLES2 no Mali-450.

## Log cronológico
- **2026-06-16** — FASE 0. Recon device .127 + repo. Confirmado MonoGame/.NET no APK,
  assemblies em claro. Cuphead do repo é il2cpp (não serve de base de runtime). Sem mono no
  device. Criado scaffold `ports/sor4/`. Commit `d1b7c9e`.
- **2026-06-16** — FASE 1 / GATE A PASS. Extraídos 58 assemblies (XABA+LZ4). Achado SOR4.dll
  (jogo, não-ofuscado) + MonoGame 3.8.3.1 + .NET 9. Mapeado acoplamento Android (116 refs, mas
  isolado/stubável). Docs `ASSEMBLIES.md`/`ANDROID-SURFACE.md`. Próximo: FASE 2 validar .NET 9 no .127.
