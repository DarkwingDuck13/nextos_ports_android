# Final Fantasy IX → ESTUDO / PLANO DE PORT (pré-go)

> Recon 2026-06-24. Próximo alvo do FF batch após Chrono Trigger (✅ completo).
> COMEÇAR após o /clear. Referência mestra = **Terraria** (`ports/terraria`), o Unity IL2CPP que JÁ RODA no Mali-450 (mesmo scaffold/loader reusa direto). Comparar tb com **RE4** (Unity so-loader) e **chrono** (cocos, mas mesmas lições bionic/JNI).

## APK
- **`FINAL-FANTASY-IX-v1.5.4-full-apkvision.apk`** (1.87 GB)
- Repack APKVISION (DRM já neutralizado — ver abaixo). Package SQEX.

## Recon (fatos confirmados)
- **Unity 2022.3.62f3, IL2CPP, arm64-v8a** (mesma família do Pixel Cup/FF5; toolchain aarch64 Amlogic-old cobre).
- Libs em `lib/arm64-v8a/`:
  - `libil2cpp.so` (44 MB) — código C# AOT do jogo.
  - `libunity.so` (16.7 MB) — engine Unity.
  - `libsdlib_android.so` (1.45 MB) — **lib de plataforma da SQEX** (JNI/serviços; vai precisar de shims JNI próprios).
  - `libFF9SpecialEffectPlugin.so` (870 KB) — **plugin de render custom** (efeitos especiais; pode precisar shim/no-op se usar caminho GL não-ES2).
  - `lib_burst_generated.so` (5 KB) — **Burst jobs MÍNIMO** (5KB → pouquíssimo Burst, risco baixo vs RE4).
  - `libAPKVISION.so` (1.1 MB) — repacker (substitui o LVL/DRM do Google).
  - `libmain.so` (6.7 KB) — bootstrap Unity Android.
- **🔑 global-metadata.dat PLAINTEXT** — magic `af1bb1fa` (0xFAB11BAF), versão 31. → IL2CPP 100% recuperável (símbolos, dump). **MELHOR que Terraria** (que tinha pairip cifrando o metadata). Igual à versão PDALIFE (metadata plaintext) já vista em outros repacks.
- **GLES**: libunity carrega GLES via dlopen (NEEDED só `libEGL.so`). Tem `force-gles20`/`force-gles30`/`force-gles31` + shaders `#version 300 es`. ⚠️ **libunity NÃO tem `#version 100` (0 ocorrências)** → os shaders internos são ES3. Os shaders DO JOGO ficam nos assets (precisa checar variantes ES2 lá).
- boot.config limpo (sem DRM, `gc-max-time-slice=3`, fullscreen).
- Assets: `assets/bin/Data/` — `globalgamemanagers.assets`, `level0`, `sharedassets0.assets.split0..N` (**split em pedaços de 1 MB → CONCATENAR em ordem** antes de usar) + `Managed/Metadata/global-metadata.dat`.

## Régua de viabilidade — 🟡 VERDE COM RESSALVAS
- IL2CPP NÃO é problema (Terraria provou; so-loader carrega o .so direto). Metadata plaintext = ainda mais fácil que Terraria.
- **2 riscos reais**: (1) **shaders ES3** — FF9 é 3D (modelos sobre fundos pré-renderizados); se os assets não tiverem variante ES2, precisa do **shim ES3→ES2** (**Mina** — funciona bem) OU `-force-gles20` (Unity gera ES2 se os subprogramas existirem). (2) **1.87 GB** — pressão de RAM no Mali-450 (832MB).
- **DECISÃO DE DEVICE (definir no go):**
  - **Mali-450 (.100/.164, 832MB, ES2)** = igual Terraria/chrono, MAS pode precisar shim ES3→ES2 + tuning de RAM (zram/leanpak estilo Dysmantle).
  - **X5M (S905X5M, Mali-G310 Valhall, ES3 NATIVO + mais RAM)** = mata os DOIS riscos de uma vez (ES3 roda nativo, RAM folgada). **Recomendado p/ FF9 especificamente** — é o caso de uso do X5M. Confirmar com NextOS.

## Plano (so-loader, reusar scaffold Terraria)
1. **Copiar scaffold**: `cp -r ports/terraria/src ports/ff9/src` + `build.sh`, `gen-unity-imports.sh`, `run.sh`. É o MESMO loader Unity IL2CPP (libunity+libil2cpp, fake JNI, egl_shim, force-gles20, canary bionic, stat64, memalign/Enlighten, opensles, gamepad).
2. **Extrair APK** → `ports/ff9/payload/`: `lib/arm64-v8a/*.so` + `assets/bin/Data/` inteiro. **CONCATENAR** `sharedassets0.assets.split0..N` → `sharedassets0.assets` (ordem numérica). Manter global-metadata.dat.
3. **Regenerar imports**: `gen-unity-imports.sh` contra o libil2cpp/libunity do FF9 (gera `imports.gen.c` + `jni_idx_stubs.gen.c`).
4. **Adaptar main.c**: SO names já são libunity/libil2cpp (iguais). Carregar tb `libsdlib_android.so` (módulo aux, resolver JNI da SQEX), `libFF9SpecialEffectPlugin.so` (plugin render — registrar/no-op conforme o que ele faz), `lib_burst_generated.so` (Burst, pequeno).
5. **GLES**: começar com `-force-gles20` (já tem no Terraria main.c). Se a cena vier preta/shader-fail → ativar shim ES3→ES2 (Mina/GTASA — ambos funcionam) OU mover pro X5M (ES3 nativo).
6. **Shims provados** (SOTN/RE4/Terraria/chrono): canary bionic tpidr+0x28, R_AARCH64_ABS64 imports UNDEF, stdio __sF, AAssetManager→disco, stat64, EGL→SDL2 do device (egl_shim universal), áudio Unity→pacat/SDL, input evdev/gamepad→Unity (+ **controle padrão Xbox + gptokeyb2**, lição chrono).
7. **Iterar** no device escolhido (matar+confirmar 0 instâncias antes de lançar; foreground bash puro; captura via glReadPixels OU dd /dev/fb0). Fullscreen res nativa automática. Empacotar tar.gz (Desktop + R2 "Final Fantasy IX (NextOS Elite).tar.gz") + launcher PortMaster (gptokeyb2 padrão, SDL_GAMECONTROLLERCONFIG Xbox, sem forçar SDL driver).

## Lições do chrono (recém-aplicadas, reusar)
- **Controle**: conferir ABI dos callbacks de input nativos (no chrono faltava `jstring vendorName` → args deslocavam). Em Unity o input vem via `UnityPlayer`/eventos — validar ABI.
- **Áudio**: manter o ring de saída com folga vs consumo do callback SDL (alvo fixo, NÃO amarrado ao tamanho dos enqueues do jogo) + thread de áudio dedicada (refill desacoplado do framerate) + ZERO log em hot-path de I/O (debugPrintf reabre arquivo → stall).
- **Launcher**: padrão PortMaster puro (`$GPTOKEYB "ff9" &` + `pm_platform_helper` + `SDL_GAMECONTROLLERCONFIG=$sdl_controllerconfig`), SEM gptk inventado, SEM forçar SDL_VIDEODRIVER/AUDIODRIVER.

## Regras herdadas (memória — OBRIGATÓRIAS)
- Git: **master only, ZERO co-autor/menção a Agente**. **HANDOFF/arquivos com IP/senha/nome NÃO vão pro GitHub** (rule nova: nada de info pessoal — varrer antes de commitar). [[no-agente-coauthor-commit]] [[git-workflow-master-only]]
- Matar+confirmar 0 instâncias por /proc/*/exe antes de lançar. [[matar-confirmar-jogo-antes-de-lancar]]
- NUNCA forçar SDL_VIDEODRIVER/SDL_AUDIODRIVER. [[nao-forcar-sdl-driver]]
- JAMAIS japonês — FF9 pode defaultar JP; forçar inglês (locale/região, igual chrono getLocationCode). [[jamais-japones-em-jogos]]
- Limpar/apagar com cuidado (alvos explícitos, staging primeiro). [[limpar-apagar-arquivos-com-cuidado]]
- Não parar até imagem. [[nao-parar-nao-explicar-ate-imagem]]

## Refs de código
- `ports/terraria` — Unity IL2CPP ES2 scaffold (REUSA DIRETO: loader, força-gles20, canary, stat64, memalign/Enlighten, gen-unity-imports.sh).
- `ports/re4` — Unity Android so-loader (JNI/Activity/audio/input).
- `ports/chrono` — lições recentes de controle(ABI)/áudio(buffer)/launcher(gptokeyb2 padrão).
- ES3→ES2 shim: **Mina** e **GTA San Andreas** (ambos funcionam no Mali-450). ⚠️ **NÃO usar Dusklight** como referência — não roda / não converte shaders.

## Devices
- Mali-450 (EmuELEC) — devices de teste locais (IPs/credenciais em notas internas, fora do repo).
- X5M (S905X5M, Mali-G310 ES3+Vulkan, mais RAM) — **recomendado p/ FF9**; confirmar IP no go.
