# LEGO Batman 2 (TT Fusion, libLEGO_SH1.so arm64) -> Mali-450 .92 — ESTADO s1

## Base
Framework = **lswtfa** (LEGO Star Wars TFA, MESMA engine Fusion, FINALIZADO) + kit
GL/áudio do Castle of Illusion. APK: com.wb.goog.legobdc v1.07.9. Bootstrap JNI-driven
(replica GameActivity.onCreate + GLSurfaceView do dex).

## ✅ FUNCIONA (confirmado)
- Boot: so_load + JNI_OnLoad + canary bionic + sequência nativeSet*/AddAssetPack x5 +
  nativeColdBoot + nativeInit + nativeResize(6i) + Resume + FocusChanged.
- **ÁUDIO tocando** (usuário ouviu som): opensles_shim do lswtfa (mp3 via minimp3,
  44.1kHz, música em loop).
- **36 texturas ETC1 (0x8d64) subindo** — passthrough Mali nativo (imports COI passa direto).
- **FMV skip**: startMoviePlayback->TRUE + nativeSkippedMovie no loop (false ABORTAVA).
- **single-context GL handoff** (RAIZ do "muita coisa preta"/BAD_ACCESS): Fusion cria
  contexto de loader em thread async; Mali fbdev recusa 2º contexto SDL+share. Solução
  portada do lswtfa: UM contexto real, revezado por thread (mutex+cond ownership em
  egl_shim.c: gl_acquire/release_if_wanted/park). pthread_bridge b_mutex_lock/cond_wait
  chamam egl_shim_gl_park() antes de bloquear -> loader pega o ctx. Com isso shaders
  compilam em 2 threads e texturas sobem. ZERO crashes.

## 🔴 MURO ATUAL: render-thread dorme esperando "load completo" (0 swaps, tela preta+som)
- Main/render thread: CPU ~7.5% (não é loop infinito nem world-gen) — faz um pouco de GL
  (glVertexAttribPointer num loop de objetos, via geCollision_LineToOctree em 0x26df6c) e
  **nanosleep**, repetindo. Poll-wait de "async load done" que nunca vem.
- Loader thread subiu 36 texturas (lofi + ipad1 .fib, NÃO o main 213MB) e ficou idle.
- Handshake das 2 threads quebrado: render espera loader, loader espera render.
- Nenhum fopen falha; nativeInitializeAssetManager stub (talvez o resto do nível venha via
  AAssetManager_open que não servimos — verificar se o loader precisa dele).
- PRÓXIMO: RE do estado de loading do Fusion — comparar EXATO com lswtfa (que passa disso):
  (1) o que lswtfa faz entre nativeInit e o 1º swap que nós não fazemos; (2) nativeResize
  6-int vs 2-int (insets de cutout); (3) se o loader espera um AAssetManager real (portar o
  asset shim do lswtfa); (4) nativeUpdateMovieInfo(done) além de SkippedMovie.

## Device
run/lb2shot/lb2long/lb2probe.sh no gamedir. Injetores: /dev/shm/lb2_tap "x y", dys_shot.
Dados: files/assetpacks/<pack>/1079/1079/ (extraídos do APK, 1.1GB). 5 packs:
assets_{cutscenes,music,shaders,main,lofi}.
