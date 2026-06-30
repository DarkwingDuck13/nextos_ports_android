# Hotline Miami 2: Wrong Number — STATUS (NextOS so-loader, Mali-450)

Engine: **GameMaker Studio YYC** (`libyoyo.so`, mesma do Katana ZERO). Base do port = `ports/katanazero`.
Device de bancada: **192.168.31.79** (Mali-450 Amlogic, EMUELEC, fbdev). `sshpass -p '' ssh root@.79`.

## ✅ FUNCIONA
- **Boot completo**: disclaimer(fan-port, ingles) → Devolver/Dennaton logos → NOTICE(skip violence YES/NO)
  → intro cinematografica ("Midnight Animal") → **GAMEPLAY** (nivel tutorial). Render limpo 1280x720,
  cor cheia, INGLES. 0 crash, ~25 fps.
- **Resolucao automatica** (SDL_GetDesktopDisplayMode) — NAO fixa 720p, adapta por device.
- **game.droid** (60MB) carrega via AAssetManager; resolver de asset **case-insensitive** (cwd+assets/)
  destrava localization + musica (`hlm2music/hlm2_music_desktop.wad`, engine pede `HLM2Music`) + asset_orders.

## 🎮 CONTROLES (em refino) — input pela EXTENSAO `getControllerValue` (NAO gamepad nativo)
HLM2 le o controle por uma extensao GML: `gamepad_init`/`isControllerConnected`/`getControllerValue(index)`.
Implementado via JNI (double-array p/ ler os args). **NAO usar gptokeyb** (converteria p/ teclado e a
extensao veria "sem controle"). NAO registrar o gamepad NATIVO do GameMaker (conflito = botoes embaralhados).

### Mapa DEFINITIVO (decompilado de com.dalmac.hotlinemiami2.Vibrate.getControllerValue no dex)
- **0=UP 1=DOWN 2=LEFT 3=RIGHT** (direcoes) → dpad + analogico ESQ. [poll-order f1000: seq0-3 = 2,3,0,1]
- **18=LOOK X 19=LOOK Y** (mira/cursor) → analogico DIR.
- **5=ATTACK/confirm** → A. [disclaimer: forcar idx5 avanca a tela = confirmar]
- acoes restantes (PICKUP/FINISH/LOCKON) em {4,6,8,9,10,20,21} → mapeadas bijetivas a B/X/Y/LB/RB/LT/RT
  (a refinar qual e qual; o jogo TEM rebind in-game: Options→Controls "CHOOSE X BUTTON").
- 5 bindings de acao no binario: g_VAR_{attack,finish,pickup,look,lockon}_controller.

### Ferramentas de RE (env)
- `HM_CTRLLIVE=1` loga cada index ativo (id<->botao fisico). `HM_POLLORDER=N` dump da ordem de polling no frame N.
- `HM_SWEEP=1 HM_SWEEPSTART HM_SWEEPHOLD` força um index por vez (achar efeito). `HM_NOMASH=1` nao mashea antes.
- `HM_AUTOEXT` autopilot (mash→walk). `HM_DUMPBIND` (g_VAR aponta p/ descritor, valor nao-direto).
- `KZ_SHOTEVERY=N` + `HM_SHOTSEQ=1` screenshots em `/tmp/kz_shot_FRAME.raw` (glReadPixels).

## ⏳ FALTA
- Travar qual acao (pickup/finish/lockon) em cada index restante (verificacao no gameplay).
- Confirmar AUDIO (OpenSLES wired, players ainda nao criados no log; musica wad carrega).
- Empacotar PortMaster (launcher `Hotline Miami 2.sh` PRONTO, sem gptokeyb, SELECT+START=sair) + R2.

## Build/run
- `bash build.sh` (toolchain NextOS aarch64) → `hlm2`. Deploy: scp p/ `/storage/roms/ports/hlm2/`.
- `bash run_test.sh [segs]` (mata anterior por /proc/*/exe, roda, puxa log+shot).
- Aberto sem timeout: `nohup ./hlm2 >/tmp/hlm2.log 2>&1 &` no device.
- BYO-data: `libyoyo.so` + `hlm2.apk` (game.droid) + assets extraidos loose (NAO vao pro git).
