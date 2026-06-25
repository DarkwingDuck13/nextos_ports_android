# FF7 → Mali-450 — STATUS (2026-06-24 sessão modo-foco)

## 🎮 ONDE CHEGAMOS
Boot → logo → **MENU INGLÊS** (NEW GAME/Continue?/Cloud Save) → New Game →
(FMV de abertura LONGO+PRETO) → **CAMPO**: personagens 3D (guardas Shinra) RENDERIZAM.
Controles Xbox via SDL direto. ~28fps.

## ✅ FEITO HOJE
1. **INGLÊS** (regra #5): `setLang(0)`=EN (1=FR,2=DE,3/4=ES/JP). Verificado por screenshot.
   main.c default FF7_LANG=0 + run_dev.sh default 0. ⚠️deployar run_dev.sh junto (roda no device).
2. 🔑**FIX DE PROGRESSÃO**: `MyDecoder.START` (decoder de FMV, classe Java) deve retornar >0
   senão o engine não engata o movie e o field-script espera pra sempre (preto eterno).
   `FF7_FMVSTART` default 1000 → movie engata → toca (preto) → DEPOIS **campo carrega**.
3. **SELECT+START → quit** in-app (ff7_pump_input). FF7 lê SDL_GameController direto →
   launcher NÃO usa gptokeyb (roubaria js0).
4. **Launcher** "Final Fantasy VII.sh" (PortMaster padrão, SEM forçar SDL driver/regra#6,
   SDL_GAMECONTROLLERCONFIG, cria configs 1º-launch, FF7_LANG=0).
5. **FF7_FMVSKIP** (experimental): fw_stop_movie p/ pular o FMV longo e chegar no campo rápido.

## 🔴 FALTA
1. **FUNDO DO CAMPO PRETO (task #4, principal)**: modelos 3D renderizam, background
   pré-renderizado (flevel.lgp) fica preto. Leads: `gfx_field_64/74/78/80/84`.
2. **FMV de abertura** (preto, longo): decodificar VP8. VIÁVEL — `libvpx.a` está no
   toolchain (sysroot). Path: MyDecoder Java→nativo; Vorbis já embutido (áudio do FMV);
   vídeo VP8 via Shader_yuv.fsh (samplerExternalOES + path alt getYUVpatch). Precisa
   demux webm (nestegg/matroska) + libvpx + upload YUV.
3. **ÁUDIO**: OpenSLES via `SL_createPlayer/SL_startPlayer/...` (opensles_shim). Testar.

## DECODER FMV (MyDecoder) — métodos Java (UPPERCASE) e tags no jni_shim.c
START()I FRAME()I GET_POSITION()I GET_TOTALTIME()I GET_YUV_PATCH()Z
AFTER_FRAME()V RESET()V SET_TEXTURE SET_VOLUME(F)V PLAY. movie_frame_counter@0x1cd8c9c.
FF7_FMVLOG loga a sequência. Sequência no New Game: SET_TEXTURE,SET_VOLUME×2,RESET×2,START.

## ⚠️ ARMADILHAS
- run_dev `timeout` manda SIGTERM que o ff7 IGNORA → trava. Use **FF7_MAXFRAMES** p/ saída limpa.
- Matar ff7 por /proc/*/exe ANTES de scp (binário em uso = scp Failure).
- glReadPixels (FF7_SHOTS) → .raw RGBA 1280x720, flip vertical no PNG. fb0 ssh não confiável.

## COMANDOS
Build `./build.sh`. Deploy ff7 (+run_dev.sh). run_dev.sh envs: FF7_AUTOSKIP (B→UP→A
auto-corretivo p/ New Game), FF7_FMVSTART, FF7_FMVSKIP, FF7_FMVLOG, FF7_SHOTS, FF7_MAXFRAMES.
Device .165 (sshpass -p emuelec). Detalhe: memória project_ff7_mali450.md.
