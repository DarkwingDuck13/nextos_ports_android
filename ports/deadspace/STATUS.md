# Dead Space NextOS Port - Status

Data: 2026-07-05 (sessao rota nativa de controle)

## APK

- Arquivo base: `/home/felipe/Downloads/dead-space-b1200.apk`
- Pacote: `com.eamobile.deadspace_full_azn`
- ABI: `armeabi` 32-bit somente. Nao ha ARMv8/arm64 no APK.
- Device alvo unico: `192.168.31.114`.

## CONTROLE NATIVO (novo, validado em 2026-07-05)

O problema antigo (analogico esquerdo bugado, camera aos trancos, botoes
mortos) era a mistura de 3 rotas: keycodes Android + toques virtuais +
eventos SDL. A solucao foi injetar direto no pipeline interno do jogo.

### Descobertas de engenharia reversa (libDeadSpace.so, not stripped)

- O build 1.2.0 e 100% touch. NAO existe rota de gamepad/touchpad ativa:
  - `TouchPadAndroidXperiaPlay.java` no DEX e stub vazio;
  - `sTouchPadPointerListeners` nunca recebe listener (por isso o teste
    antigo de camera via modulo 1100 falhou);
  - unico key listener do jogo e `Application::OnKeyDown` (std keys
    MENU/HOME/BACK -> `Hud::doSpecialAction`).
- Pipeline de gameplay:
  - `Hud` = `LayerGameWorld+0x260`; `GameWorld` em `hud+4`.
  - `InputSchemeDPadsRel` embutido em `hud+0x10`.
  - `scheme+0xd4` = `InputForwarderTouchDPad*` movimento (dpad esq).
  - `scheme+0xe0` = `InputForwarderTouchDPad*` camera (dpad dir).
  - `InputForwarderTouchDPad::sendDPadEvent(this,float dx,float dy)`
    (0x7c614, softfp: floats em r1/r2) normaliza, aplica deadzone
    (`fwd+0x30`, vem de Tweaks +0x178/+0x17c) e flag de corrida
    (`-dy > fwd+0x3c`), e despacha evento id 1006.
  - `Hud::doSpecialAction(this,int id,int param)` (0x7541c), ids
    `0x0352fb91+idx`, jump table de 16 acoes.

### Implementacao no loader (src/main.c)

- `install_native_input_hook()` troca a entrada de
  `InputSchemeDPadsRel::onUpdateEvent` (0x81608) por stub em
  `base+0x4f0700` -> `ds_native_onupdate_hook()` (roda NA THREAD DO
  JOGO, por frame). O hook:
  - reproduz a logica original (deadzone via Tweaks conforme mira);
  - injeta stick esquerdo em `sendDPadEvent(fwdMove,...)` e direito em
    `sendDPadEvent(fwdLook,...)` (pixels = amp * stick; amp deriva da
    deadzone, override `DS_MOVE_AMP`/`DS_LOOK_AMP`);
  - solta os dois dpads ao pausar;
  - drena fila SPSC de botoes -> `Hud::doSpecialAction`.
- SDL thread so escreve `g_njoy_*` e enfileira acoes (thread-safe).
- Acoes (idx sobre 0x0352fb91), defaults dos botoes Xbox:
  - A=1 interact, B=5 melee, X=9 reload, Y=2 troca arma,
    LB=6 stasis, RB=10 kinesis, LT=7 mira (hold), RT=8 tiro,
    Start=14 pause, Back=12 back, R3=4 locator.
  - Remap por env: `DS_BTN_A=idx[,h]` etc ("h" = hold manda 1/0).
  - idx7 mira usa param 1/0 (hold); demais tap com param 0.
- Menus (fora do gameplay): cursor virtual desenhado em GLES1
  (crosshair ciano) — stick/dpad move (`DS_CURSOR_SPEED`, def 11),
  A = tap no cursor, B = tecla BACK nativa. `DS_NO_CURSOR=1` desliga,
  `DS_MENU_TAPS=1` religa os taps cegos antigos.
- Debounce de dpad (2 polls) — o adaptador Twin USB PS2 flickerava
  DPAD_DOWN/UP sozinho (era a causa dos menus pulando sozinhos).
- Deteccao automatica menu vs gameplay: hook vivo + hud despausado
  (`native_gameplay_active()`); rota antiga vira fallback automatico.
- `DS_NO_NATIVE=1` desliga tudo e volta para a rota antiga.

### Validacao (autoinput no 114, logs + framedumps)

- Menu -> Play (tap 165,650) -> dificuldade -> cutscene -> Chapter 1.
- Tutorial de movimento completado com stick nativo; tutorial de LOOK
  completado com stick direito nativo (camera girou suave nos dumps).
- `doSpecialAction` mira(7,1)/tiro(8)/mira off(7,0)/reload(9)/melee(5)
  executaram sem crash (Isaac ainda sem arma no comeco, sem efeito
  visual esperado).
- Cursor de menu renderiza sem corromper a UI (`DS_CURSOR_TEST=1`).

### Ferramentas de teste

- `DS_AUTOINPUT=1` navega menu (Play/dificuldade) e roda a sequencia
  de gameplay nativa (andar/camera/mira/tiro/reload/melee) com logs
  `[auto]`/`[native]`.
- `DS_GLDUMP_EVERY=600 DS_GLDUMP_MAX=40` dumpa ppm em /tmp.
- `DS_CONTROLLOG=1` loga teclas/touch + raw do controle a cada 240
  frames; `DS_NATIVELOG=1` loga as acoes nativas.

## Estado anterior (continua valido)

- Imagem GLES1 ok (fix JNI width/height V/A); assets/VFS ok; ETC1 ok.
- Audio ok na rota JNI/SDL (`SetShortArrayRegion`, backpressure etc).
- Launcher com watchdog, swap 512MB opcional, restaura ES ao sair.

## Pendencias

- Confirmar no controle fisico: sensacao dos sticks (ajustar
  `DS_MOVE_AMP`/`DS_LOOK_AMP`/`DS_CURSOR_SPEED` se preciso).
- Confirmar acoes idx 0/6/8/9/13/15 quando houver arma (apos pegar o
  plasma cutter) e ajustar mapa se necessario.
- Testes longos de gameplay/estabilidade.
