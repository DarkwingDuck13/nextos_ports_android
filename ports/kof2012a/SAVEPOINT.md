# KOF-A 2012 — savepoint (estado JOGÁVEL / empacotado)

Port em `/home/felipe/nextos_ports_android/ports/kof2012a`, alvo Mali-450
(Amlogic-old, EmuELEC .114). `.so`: `lib/libmain.so` (SNK GLES1/OpenSLES, JNI
so-loader). Dados/assets são BYO (não vão pro git público).

## Controles (nativos, qualquer pad — testados no 0810:0001)
- Leitura DIRETA de `/dev/input/jsN` (SDL não lida bem com o 0810). Guard de
  eixo travado (só aceita eixo depois de vê-lo centrado; semeado pelo burst
  init) — cobre eixos 0-7 (dpad em 4/5 nessa unidade, hat vira axis 2/3 em pad
  virtual).
- **Bits nativos conferidos por probe NA LUTA** (o menu é touch-only e ignora
  tecla; a luta lê o GamePad nativo via hook `GamePad::SetKey`):
  direções `LEFT=1 RIGHT=2 UP=4 DOWN=8` (estavam invertidas — corrigido).
  Combate: `P=0x40 K=0x80 S=0x100 E(esquiva)=0x200`, `PAUSE=0x2000`.
- Mapa físico: X→E, A→P, B→K, Y→S; L1/R1/L2/R2→combos (P+K / E+S);
  START→PAUSE; SELECT→cursor; **SELECT+START→sair** (igual Bully).

## Cursor mode (menu é touch-only)
- Ponteiro virtual desenhado (GLES1), move pelo pad, botão de ação = toque.
  SELECT alterna. Default ON (`KOF_CURSOR`). FIFO de teste `KOF_CMD_FIFO`
  (`cursor 0/1`, `cpos fx fy`, `mask hex n`, `fps n`, `resume/pausepos`, `shot`).
- Pause por toque: START abre (tecla) e resume tocando no ↩ (canto sup dir,
  `g_resume_fx/fy=0.99/0.02`) SEM reenviar a tecla (evita "pula pra última
  casa"). `g_paused` trava o reenvio.

## Tela limpa
- Hooks no-op em `GamePad::DrawButton/DrawStick/DrawStick2/DrawGuid` escondem o
  pad de toque (`KOF_HIDE_PAD=1`).

## Flags assados no launcher (fazem abrir/rodar certo)
`KOF_AUTO_SKIP_VIDEO=1` (sem tela preta) · `KOF_NO_MOVIE_OVERLAY=1` ·
`KOF_NO_AUDIO=1` · `KOF_HIDE_PAD=1` · `KOF_FPS=20` · `KOF_CONFIRM_TOUCH=0`.

## Pendências (próxima revisão)
- 🔴 **Áudio**: deadlock multi-lock (SDL callback ↔ pump/prime `g_cb_lock` +
  `SDL_LockAudioDevice`) trava no fim do filme/transições. Por isso o áudio está
  OFF — e sem áudio o passo corre rápido (paliativo `KOF_FPS=20`). Consertar o
  deadlock reativa o som e normaliza a velocidade.
- Filme de intro renderizado (hoje auto-skip); depende do áudio.

## Empacotamento
Full-data privado `.tar.gz` → `r2:black-retro-content/ports_aio/nextos_elite_exclusivos/KOF-A 2012 (NextOS Elite).tar.gz`.
Estrutura: `ports/kof2012a/` + `ports_scripts/KOF-A 2012.sh` + `ports_scripts/images/KOF-A 2012.png`.
