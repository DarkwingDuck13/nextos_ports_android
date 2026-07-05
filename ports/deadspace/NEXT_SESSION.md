# Dead Space - Proxima sessao

Data: 2026-07-05
Device alvo unico: `192.168.31.114`

## O que foi feito nesta sessao (rota nativa de controle)

Controles reescritos por injecao nativa — ver STATUS.md secao
"CONTROLE NATIVO". Resumo:

- Gameplay: sticks vao direto em `InputForwarderTouchDPad::sendDPadEvent`
  (movimento e camera nativos, suaves), botoes viram
  `Hud::doSpecialAction` na thread do jogo (hook em 0x81608).
- Menus: cursor virtual (stick move, A toca, B = BACK nativo).
- Debounce do dpad (adaptador Twin USB flickerava sozinho — era isso
  que fazia o menu pular/nao confirmar).
- Validado por autoinput + framedumps: entrou no Chapter 1, andou,
  girou camera, mira/tiro/reload/melee sem crash.

## Como testar manualmente

```
cd /storage/roms/ports/deadspace && ./DeadSpace.sh
```

- Menu: mexa o analogico esquerdo -> aparece o crosshair ciano;
  A confirma no cursor, B volta.
- Gameplay: esquerdo anda (empurrar tudo = correr), direito gira a
  camera, LT segura mira, RT atira, X recarrega, B melee, Y troca
  arma, LB stasis, RB kinesis, Start pausa.

## Calibragem rapida (se precisar)

- Movimento lento/rapido: `DS_MOVE_AMP=120` (padrao auto ~95).
- Camera lenta/rapida: `DS_LOOK_AMP=200` (padrao auto ~100-160).
- Cursor: `DS_CURSOR_SPEED=14`.
- Remap de botao: `DS_BTN_RT=8` / `DS_BTN_LB="6"` / hold: `DS_BTN_RB="10,h"`.
- Desligar tudo: `DS_NO_NATIVE=1` (volta rota antiga).

## Pendencias

1. Teste manual do NextOS com o controle fisico (sensacao dos sticks).
2. Apos pegar o plasma cutter, confirmar tiro/reload/mira com arma.
3. Identificar idx 0/13/15 restantes do doSpecialAction se algum botao
   parecer sem funcao (usar `DS_NATIVELOG=1` + `DS_BTN_x=idx`).
4. Testes longos (RAM/watchdog) e fechamento limpo.

## Referencias tecnicas

- Ver INPUT_STUDY.md (mapa de simbolos/offsets) e STATUS.md.
- Enderecos chave: onUpdateEvent 0x81608, sendDPadEvent 0x7c614,
  doSpecialAction 0x7541c (ids 0x0352fb91+idx), isPaused 0x6e498,
  Tweaks::get 0x22f7b8; hud=layer+0x260, scheme=hud+0x10,
  fwdMove=scheme+0xd4, fwdLook=scheme+0xe0.
