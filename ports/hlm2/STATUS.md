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

> ✅ **s4: CONTROLES FUNCIONANDO** ("mil vezes melhor") apos 2 fixes: (1) mapa do dex (passthrough,
> nao inventar) (2) **deploy.sh mata o processo ANTES do scp** — o bug real era binario VELHO no device
> (scp nao sobrescreve binario em execucao -> "text busy" silencioso -> fixes nunca rodavam).

## 🎮 CONTROLES ✅ — input pela EXTENSAO `getControllerValue` (NAO gamepad nativo)
HLM2 le o controle por uma extensao GML: `gamepad_init`/`isControllerConnected`/`getControllerValue(index)`.
Implementado via JNI (double-array p/ ler os args). **NAO usar gptokeyb** (converteria p/ teclado e a
extensao veria "sem controle"). NAO registrar o gamepad NATIVO do GameMaker (conflito = botoes embaralhados).

### Mapa DEFINITIVO (decompilado de com.dalmac.hotlinemiami2.Vibrate.getControllerValue no dex)
`getControllerValue(index)` = PASSTHROUGH do INPUT FISICO CRU (NAO acao). O JOGO faz o binding interno
(incl. rebind in-game Options→Controls). index → input fisico:
```
 0=lx  1=ly  2=rx  3=ry      (eixos analogicos, -1..1)
 4=l2  5=r2                  (gatilhos, 0..1)
 6=l1  7=r1                  (ombros)
 8=a   9=b  10=x  11=y       (faces)
12=dpx 13=dpy                (dpad como eixo)
14=dpadUp 15=dpadDown 16=dpadLeft 17=dpadRight
18=menu 19=options 20=l3 21=r3
```
Lição: NAO inventar mapa de acao — devolver o estado fisico SDL exato; o jogo (codigo do dalmac) ja
tem o mapeamento padrao. Erro anterior: tratei idx0-3 como UP/DOWN/LEFT/RIGHT quando sao os EIXOS lx/ly/rx/ry.
Como achar: classes.dex → method `getControllerValue` na classe `com/dalmac/hotlinemiami2/Vibrate` →
packed-switch index→campo (parser DEX em python, ver historico).

### Ferramentas de RE (env)
- `HM_CTRLLIVE=1` loga cada index ativo (id<->botao fisico). `HM_POLLORDER=N` dump da ordem de polling no frame N.
- `HM_SWEEP=1 HM_SWEEPSTART HM_SWEEPHOLD` força um index por vez (achar efeito). `HM_NOMASH=1` nao mashea antes.
- `HM_AUTOEXT` autopilot (mash→walk). `HM_DUMPBIND` (g_VAR aponta p/ descritor, valor nao-direto).
- `KZ_SHOTEVERY=N` + `HM_SHOTSEQ=1` screenshots em `/tmp/kz_shot_FRAME.raw` (glReadPixels).

## 💾 SAVE / CONTINUE (investigado s4)
- **Salva OK**: `save.dat` (JSON: hardlevels/scores globais) + `tempsave` (43KB checkpoint, room rmTutorialFloor1).
  Log `СОХРАНЕН ... tempsave`. PERSISTE apos kill/reopen. Zero corrupcao. `GetSaveFileName` (RunnerJNILib)
  resolve pro cwd (=port dir, launcher faz cd) -> mesmo path salvar/ler.
- **CONTINUE APARECE** no menu (confirmado por captura do fb0: NEW GAME/**CONTINUE**/EDITOR/OPTIONS/QUIT).
  Binario tem `[LOAD TEMPSAVE]`. WADTEMP/Music (62MB) extraido 1x e PERSISTE (nao re-extrai).
- 🔑 **"tela preta" = LOAD SINCRONO**, NAO bug de shader/textura/save: durante o load de nivel o render loop
  fica preso em Process() (nao retorna) -> tela congela na ultima frame (preta na transicao). Lento no Mali-450.
  fb0 SEMPRE tem o conteudo certo (menu/gameplay renderizam cor cheia). Captura: `dd if=/dev/fb0` (fb e 1280x**1440**
  double-buffer; metade de cima [0:720] = display).

## 🖤 RAIZ DA "TELA PRETA" RESOLVIDA (s5) — alpha=0 no OSD Amlogic
A tela preta (que parecia ser save/shader/blank/pan) era o **canal ALPHA=0**: certas telas
(o MENU) renderizam no fb com alpha=0 (transparente); o **OSD do Amlogic composita usando o
alpha** → transparente → preto no HDMI (RGB estava certo no fb!). Telas opacas (NOTICE/gameplay,
alpha=255) apareciam; menu (alpha=0) preto → falso-correlacionava com o save (save→menu).
**Diagnostico:** `dd if=/dev/fb0` + comparar o byte alpha (NOTICE 86% opaco vs MENU 94% alpha-0).
**Fix:** força alpha=255 no fb antes do swap (glColorMask so-alpha + glClear). HM_NOALPHAFIX desliga.
🔑 LICAO REUSAVEL p/ qualquer GLES no Amlogic: se renderiza no fb mas TV preta, cheque o ALPHA.

## 🔊 MÚSICA ✅ RESOLVIDA (s6 2026-06-30) — poll-worker no decode thread
**RAIZ:** a música é STREAMED via um pool de threads de decode da engine (`DecodingThreadPool` /
`DecodingThread` / `DecodingTask`), NÃO o `VorbisMemoryStream` (esse é o caminho dos SFX, síncrono via
`cAudio_Sound::DecodeMemoryStream`). Fluxo da música: `scrPlaySong` → `Audio_StartSoundNoise` →
cria `DecodingTask(VorbisFileStream "WADTEMP/Music/<song>.ogg")` → `DelegateTask` → `QueueTask` na
fila de uma das 4 `DecodingThread`. As 4 workers (`DecodingThread::ThreadFn`) Tickam 1× no startup e
**dormem num condvar `yyal` que NUNCA acorda** (o notify do `QueueTask` não propaga pelo nosso
pthread_bridge) → a task fica QUEUED e nunca roda `Tick`→`Setup`→`Run`→`VorbisStream::Read` → silêncio.
(`DecodingThreadPool::TickAll` só Ticka no main thread se `mode==0`; o pool roda em modo threaded → no-op.)

**FIX (default ON; desliga com `HM_NOMUSFIX`):** hook `DecodingThread::ThreadFn` → `my_dthread_fn_poll`:
cada worker vira um POLL que chama o `Tick` REAL da PRÓPRIA thread a cada ~3ms (em vez de dormir no
condvar). Quando a task é enfileirada, a thread dona pega no próximo poll: `AddBufferedData` (splice
incoming→active) → `Setup` (abre o stream, state→2) → `CheckSetup` (task→2) → `Run` → `VorbisStream::Read`
(decode Vorbis real) → enche o `BufferQueue` → a engine mixa música+SFX → enqueue no player OpenSL.
Sem corrida com o main thread (cada thread só mexe na PRÓPRIA fila, protegida pelo mutex `yyal` interno).
Validado .79: `DecodingTask::Run` 671×, `VorbisStream::Read` 303× em 70s, bq_Enqueue 9/13 com PCM
não-zero (peak até 15060), mixpeak não-zero, 0 crash. Título renderiza + música toca.

🔑 **Ferramenta nova reusável: `hook_arm64_trampoline(addr, dst)`** (so_util.c) = hook COM call-through
(salva os 4 primeiros insns num trampoline executável + jump p/ addr+16; recusa prólogo PC-relativo
ADRP/ADR/B/BL/LDR-literal). Diag verbose da música: `HM_MUSLOG=1`.

## ⏳ FALTA
- Travar qual acao (pickup/finish/lockon) em cada index restante (verificacao no gameplay).
- Empacotar PortMaster (launcher `Hotline Miami 2.sh` PRONTO, sem gptokeyb, SELECT+START=sair) + R2.
- (música ✅, SFX ✅, vídeo ✅, controle ✅, save ✅ — port ~95% feature-complete).

## Build/run
- `bash build.sh` (toolchain NextOS aarch64) → `hlm2`. Deploy: scp p/ `/storage/roms/ports/hlm2/`.
- `bash run_test.sh [segs]` (mata anterior por /proc/*/exe, roda, puxa log+shot).
- Aberto sem timeout: `nohup ./hlm2 >/tmp/hlm2.log 2>&1 &` no device.
- BYO-data: `libyoyo.so` + `hlm2.apk` (game.droid) + assets extraidos loose (NAO vao pro git).
