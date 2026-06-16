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

## Log cronológico
- **2026-06-16** — FASE 0. Recon device .127 + repo. Confirmado MonoGame/.NET no APK,
  assemblies em claro. Cuphead do repo é il2cpp (não serve de base de runtime). Sem mono no
  device. Criado scaffold `ports/sor4/`. Próximo: FASE 1 extração do assembly store (GATE A).
