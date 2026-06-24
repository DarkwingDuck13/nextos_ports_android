# GTA Liberty City Stories → Mali-450 — ENTREGA (2026-06-23)

## COMO RODAR
No device (EmuELEC, root), via SSH:
```sh
# RECOMENDADO — com logs persistentes + watchdog anti-travamento:
sh /storage/roms/ports/lcs/run-final.sh
# (ou o perfil jogável direto, sem o harness de logs)
sh /storage/roms/ports/lcs/run-playable.sh
```
- O jogo leva ~2 min (2 cutscenes) até o gameplay; o fluxo é automático.
- Controle: analógico esquerdo anda; gamepad físico já mapeado.
- Sair limpo: o run-final encerra sozinho após `RUNSEC` (default 600s) com teardown do Mali.
- Binário final: `lcs` md5 `2fa036a` (libGame.so War Drum/Leeds).

## O QUE FOI FEITO
1. **🏆 BUG PRINCIPAL RESOLVIDO — "chão/asfalto preto":** a surface GL era criada com canal
   ALPHA e o compositor do Amlogic vazava o fundo preto onde o framebuffer tinha alpha<1.
   Aparecia SÓ na TV (não no glReadPixels). **Fix:** forçar alpha=1 no framebuffer antes do
   swap (imports.c, `LCS_NO_ALPHA_FIX` desliga). O porter confirmou: "resolveu tudo".
2. Vsync best-effort (FBIO_WAITFORVSYNC) p/ reduzir tearing dirigindo.
3. Perfil estável default: sombras off (evita flicker de sombra z-fight), detail tratado.
4. **Logs persistentes** (logs/, timestamp) + **watchdog anti-travamento** (run-final.sh).

## O QUE FOI TESTADO (sessão automatizada, ver logs/)
| Item | Resultado |
|---|---|
| Boot / inicialização | ✅ chega ao gameplay (~130s) |
| Render / menus / HUD | ✅ normal, chão SEM preto (brilho 48 vs <20 seria preto), HUD/mundo ok |
| Controles (inject move/câmera/botão) | ✅ registrados no engine (`[input] button=9 DOWN`, `[probe]`) |
| Áudio | ✅ OpenAL carregado (`preload: libopenal.so.1 OK`); som confirmado ok |
| Crash | ✅ nenhum (crash.log vazio) |
| Watchdog + heartbeat | ✅ HEARTBEAT timestampado a cada 5s + mem; mata/reinicia se congelar |
Screenshots do teste: `logs/shots/test0-2.png`.

## LOGS PERSISTENTES (pasta logs/)
- `logs/game_test.log` — tudo, timestampado (boot, eventos do jogo, HEARTBEAT/status, encerramento).
- `logs/audio.log` — backend/áudio (OpenAL/pulse/alsa/pcm).
- `logs/input.log` — controles ([input]/[probe]/onJoyButton/setJoyAxis).
- `logs/crash.log` — só criado se houver SIGSEGV/abort/fault/wedge (entrega = vazio).
- `logs/build.log` — saída do build.
- `logs/shots/` — screenshots de verificação de render.

## WATCHDOG ANTI-TRAVAMENTO (run-final.sh)
- Monitora `heartbeat.txt` (frame). Se o frame ficar parado `FREEZE_SEC` (default 45s) → registra
  em crash.log, **mata o processo travado** e reinicia (até `MAXRESTART`). Nunca deixa o device preso.
- Heartbeat/status a cada 5s (frame + RAM livre + tempo de stall).
- Todos os comandos com timeout; ao sair, garante 0 instâncias do jogo.

## ⚠️ LIMITAÇÃO CONHECIDA (não-bloqueante)
**Trava/stutter ao DIRIGIR rápido** = parede de RAM (igual Bully no Mali-450 832MB). Diagnóstico
completo em `ESTUDO-PERFORMANCE-TIERS.md`: o lcs usa ~480MB RSS; ao dirigir, o streamer carrega
setores novos mais rápido do que libera (o motor mobile tem o gerenciamento de memória gutado,
sem ref-count → não despeja) → RAM enche → swap no SD (lento) → trava. Textura é só 65MB (não é
o gargalo; half-res não resolve). A pé/parado roda liso. NÃO foi pedido mexer no device (zram/swap),
então fica como tuning futuro no orçamento de streaming do motor. Os bugs VISUAIS estão resolvidos.

## ARQUIVOS
- Código: `ports/lcs/src/` (neste repo) — `bash build.sh` → `lcs`.
- Launchers: `run-final.sh` (entrega, logs+watchdog), `run-playable.sh`, `run30.sh`.
- Docs: `STATUS.md` (diário), `ESTUDO-PERFORMANCE-TIERS.md` (RAM), este `ENTREGA.md`.

## ✅ CONTROLE CORRIGIDO (2026-06-23 noite) — LCS_FIX_CONTROLS (default ON)
**Problema:** o stick ESQUERDO tinha funções DUPLICADAS (acionava o analógico E os campos de
D-pad da CPad ao mesmo tempo), então mexer no esquerdo também disparava o zoom/funções do D-pad.
O DIREITO (girar câmera) já estava correto. Zoom de câmera ≠ mover câmera — zoom deve ser do D-pad.
**Causa:** o `m_bSwapNippleAndDPad` estava ATIVO; a gente contornava espelhando o esquerdo nos
offsets de D-pad (18-32) → era essa a duplicação.
**Fix (`LCS_FIX_CONTROLS=1`, default):** força `m_bSwapNippleAndDPad=0` por frame → o engine lê o
ANALÓGICO (offset 2/4) p/ movimento E o D-PAD físico volta a ser as funções NATIVAS (zoom da
câmera). Para de escrever o mirror do D-pad (18-32) → some a duplicação. O gamepad nativo
permanece LIGADO (andar NÃO quebra — ao contrário da tentativa anterior que desligava o nativo).
**Resultado (esquerdo=andar / direito=câmera / D-pad=zoom):**
- esquerdo → analógico → `GetPedWalkUD ret=65421` (não-zero) com `swap=0`, `stick=0,-115`, `dpad=0,0,0,0`
  (verificado in-game via inject; sem duplicação).
- direito → câmera (gamepad nativo, intacto).
- D-pad físico → zoom/funções nativas (não mais acionado pelo esquerdo).
- Jogo saudável: frame avança (f 104→554), RAM 525MB livre, 0 crash. Binário `94f99e4`.
- `LCS_FIX_CONTROLS=0` volta ao comportamento antigo (mirror do D-pad) se precisar.

## ADENDO (2026-06-23 tarde) — downscale e controle
- **Downscale de render TESTADO (A/B pareado) e DESCARTADO:** medição no device, gameplay
  assentado, janela de 20s: **1280x720 = 21 fps** vs **1152x648 (LCS_RENDER_SCALE=0.9) = 20 fps**.
  Renderizar 19% menos pixels NÃO subiu o fps (deu 1 a menos, dentro do ruído) → o jogo é
  **RAM/streaming-bound, não GPU**; downscale NÃO resolve o "trava dirigindo". Além disso o
  `LCS_RENDER_SCALE` no Mali fbdev não faz mode-set do fb0 (fica 1152x648 num canto, imagem menor).
  **Default mantém 1280x720.** Flag continua no egl_shim mas OFF. (Detalhe em ESTUDO §5.1.)
- **Bug de controle identificado:** o analógico DIREITO faz move+zoom+câmera (deveria ser SÓ câmera;
  zoom no D-pad). Causa: o gamepad NATIVO do engine (AND_GamepadUpdate) mapeia o stick direito.
  - Tentativa `LCS_CAMERA_BRIDGE` (stick direito → CPad offset 6/8) + `LCS_NO_AND_GAMEPAD_UPDATE`
    (desliga o nativo): ⚠️ **QUEBROU o andar** (o engine depende do gamepad nativo). NÃO usar como default.
  - Ambos são **opt-in (off por padrão)** → o build default segue com o controle funcionando.
  - PRÓXIMO (com cuidado, sem quebrar): manter o gamepad nativo LIGADO e apenas SUPRIMIR/zerar o
    mapeamento de zoom do stick direito no engine (achar o offset/var de zoom da câmera), em vez de
    desligar o gamepad inteiro. Verificar o offset real do RightStick na CPad (logar [cambridge]/walkdiag).
- **ESTADO SEGURO ENTREGUE:** binário c92868a, default = controle nativo funcionando, chão preto
  resolvido, logs+watchdog. As flags experimentais ficam pra iterar sem risco ao default.
