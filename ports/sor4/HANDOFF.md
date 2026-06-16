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
