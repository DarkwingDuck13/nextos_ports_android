# Summertime Saga - estado salvo

Data: 2026-07-05 12:05 no device / 15:05 no host.
Device de teste: 192.168.31.79.

## Estado funcional

- Jogo abre em 1280x720.
- Render, audio e gamepad estavam funcionando antes da pausa.
- ES/emustation foi parado durante o teste.
- Processo do jogo foi encerrado a pedido do usuario.
- Ultimo log no device: `/storage/roms/ports/summertimesaga/logs/run-20260705-120126.log`.
- Screenshot hook respondeu com `1280 720` no ultimo teste.

## Referencia estudada

- `ports/codboz` foi usado como referencia de cursor/input estavel.
- Conclusao aplicada: evitar cursor paralelo por EGL e manter uma rota unica integrada ao ciclo da engine.

## Ajuste atual de cursor

Rota atual:

`evdev -> src/input.c -> /dev/shm/summertime_vcursor / /dev/shm/summertime_vclick -> Ren'Py core.py`.

Alteracoes aplicadas:

- `SUMMERTIME_RENPY_CURSOR=1`.
- `SUMMERTIME_CURSOR=0`, deixando o overlay EGL desativado.
- `src/input.c` agora emite cursor com limite mais fino: 16 ms ou 8 px.
- `renpy/display/core.py` separa sequencia de leitura do pump e sequencia de desenho.
- Movimento do cursor agora posta `MOUSEMOTION` virtual no Ren'Py, alem de atualizar `testmouse.mouse_pos`.

## Validacao feita antes da pausa

- Build local passou: `./build.sh`.
- Sintaxe Python passou: `python3 -m py_compile renpy/display/core.py`.
- Binario, script e `core.py` foram sincronizados para o device.
- Jogo relancou e chegou ate EGL/audio/input:
  - `Window created 1280x720`
  - `audio_open ... pacat=OK`
  - `input: gamepad on /dev/input/event2 (' USB Gamepad          ')`
  - `[shot] 1280x720 salvo`

## Self-contained + auditoria de saida (2026-07-06, sem abrir o jogo)

Hooks migrados do `.sh` para o binario (`src/main.c`, antes do init de video):
- `ss_kill_prior_instances()` — instancia unica (scan /proc), era linhas 46-49 do .sh.
- `ss_clean_shm()` — limpa /dev/shm/summertime_vcursor|vclick(.tmp), era 51-52.
- `ss_ensure_dirs()` — cria saves/saves-cache/game/logs.
- `ss_cpu_performance()` — NOVO: governor performance (ajuda o engasgo do
  cursor no Amlogic old); desliga com `SS_NO_CPUPERF`.
Launcher `Summertime Saga.sh` enxugado: removidas as linhas de kill/rm-shm;
mantido so `mkdir -p logs` (o redirect de log precisa antes) e o `trap
pkill EXIT` como rede de seguranca. Deployado nos DOIS (ports/ e ports_scripts/).

Auditoria Select+Start (SEM abrir):
- `src/input.c` L582: Select+Start -> `_exit(0)` = terminacao imediata a
  nivel de kernel. NAO pode travar: sem teardown do jogo (nao ha
  NativeOnPause pra segfaultar), kernel libera fbdev/DRM/fds. Padrao
  Bully/SOR4 comprovado.
- Launcher NAO faz stop/start de ES (padrao correto; a ES se auto-suspende
  ao lancar o port e volta ao sair) -> nao trava a ES (era exatamente esse
  stop/start que travava no Dead Space; aqui nunca existiu).
- Fluxo: `_exit(0)` -> shell resume apos `./summertimesaga` -> pkill +
  pm_finish -> ES volta limpa. Sem risco de trava (jogo OU ES).
- glibc: meus hooks nao adicionam simbolos >2.30 (opendir/mkdir/unlink/kill
  etc). Os 2.33+ do binario ja eram pre-existentes (pthread/dl/libm/isoc23).

## Pendente ao retomar

- Validar com usuario o cursor dentro do gameplay apos esta ultima correcao.
- Se gameplay estiver suave, empacotar tar/zip completo na Area de trabalho e atualizar o R2.
- Se ainda estiver ruim, proximo ajuste deve ser somente nesta rota Ren'Py: taxa de `MOUSEMOTION`, `redraw()` do cursor e velocidade/deadzone em `src/input.c`.
