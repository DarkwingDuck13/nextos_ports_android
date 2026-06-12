# Estudo: controles do Bully via gptokeyb (padrão PortMaster) — 2026-06-12

## Problema
Até a v5 o mapeamento de controle ficava **dentro do binário** (`jni_shim.c`
`pump_gamepad()`: SDL_GameController → eventos JNI `implOnGamepadButton*`/
`implOnGamepadAxesChanged`). Funciona, mas cada device/CFW tem um pad
diferente e o binário não tem como conhecer todos. O **gptokeyb** é o
padronizador do ecossistema PortMaster: cada CFW já entrega o gptokeyb
configurado pro SEU hardware (via `control.txt`/`get_controls`), e o port só
declara um `.gptk` com o mapeamento *lógico* → teclas.

## O que o gptokeyb do device suporta (verificado no S905X5M, /usr/bin/gptokeyb)
strings do binário confirmam: `a/b/x/y/l1..l3/r1..r3/start/back/up/down/left/
right`, `left_analog_*`, `right_analog_*`, `mouse_movement_*` (stick → mouse
relativo), `mouse_scale`, `mouse_delay`, `deadzone_*`, hotkey de kill.

## Referência usada (R2, padrão PortMaster real)
`ports_aio/ClassiCube.tar.gz` → `classicube.gptk`: WASD no stick esquerdo,
`mouse_movement_*` no direito (`mouse_scale = 768`), botões em letras, e o
launcher chama `$GPTOKEYB "ClassiCube" -c "classicube.gptk" textinput &`.

## Desenho adotado (v6, modo híbrido)
1. **`bully.gptk` (layout PS2/DualShock)** — mapeamento lógico em teclas:
   Cruz=`x` Círculo=`c` Quadrado=`q` Triângulo=`t` START=`enter` SELECT=`esc`
   L1=`h` R1=`j` L2=`k` R2=`l` L3=`n` R3=`m` dpad=setas; stick esq→`wasd`,
   stick dir→`mouse_movement_*`. (Teclas escolhidas fora de w/a/s/d.)
2. **Launcher** liga o modo com `BULLY_INPUT=gptk` e sobe
   `$GPTOKEYB "bully" -c bully.gptk` (ou `gptokeyb -1` do sistema). Sem
   gptokeyb no device → o env não é setado → **fallback = caminho nativo v5**
   (binário lê o pad direto, nada quebra).
3. **Binário (`pump_gptk()` em jni_shim.c)**:
   - **Botões: SEMPRE do teclado** (é isso que o gptokeyb padroniza). Tecla →
     enum do libGame (0=Cruz 1=Círculo 2=Quadrado 3=Triângulo 4=START 5=SELECT
     6=L3 7=R3 8-11=dpad 16=L1 17=L2 18=R1 19=R2). Gatilhos também viram eixos
     a[4]/a[5] (0/1).
   - **Sticks: ANALÓGICOS do pad quando o SDL ainda o enxerga** (o gptokeyb
     não dá grab nos CFWs testados) → preserva o gradiente andar/correr.
   - **Fallback digital**: se o pad sumir (grab), `wasd`→stick esq (±1) e
     mouse relativo→stick dir (câmera), com suavização (lowpass 0.5) e
     sensibilidade `BULLY_MOUSE_SENS` (default 0.09/px; `mouse_scale` no .gptk
     também regula do lado do gptokeyb).
   - **SAIR = SELECT+START** vira `esc+enter` no teclado (mesmo combo).
4. O jogo já mostra **ícones PlayStation** (`GetGamepadType()=8`/PS3), então o
   layout PS2 fica coerente na tela.

## Riscos/observações
- `GPTOKEYB2` foi tirado do launcher: a sintaxe de config do gptokeyb2 é
  diferente (.ini próprio); com `-c` do gptokeyb1 o `.gptk` acima é garantido.
  (Se um dia precisar, fazer um `bully.gptk2`.)
- Eventos de teclado no SDL/kmsdrm e fbdev chegam via evdev (root) — é o
  mesmo caminho que TODOS os ports GameMaker/gmloader já usam nesses CFWs.
- Se o CFW tiver um gptokeyb que DÁ grab, o caminho digital assume sozinho
  (g_pad fica NULL ou sem eventos novos — checagem é por pad aberto).

## RESULTADO FINAL (2026-06-12, validado no S905X5M)

### Enum REAL de eventos do libGame (confirmado por RE: __type_GamepadButton @0x7e03f8)
0-3=A/B/X/Y (posicional) 4=START 5=BACK 6=LEFTSTICK 7=RIGHTSTICK 8-11=NAV
12-15=DPAD(legado) 16=LEFTSHOULDER 17=LEFTTRIGGER 18=RIGHTSHOULDER 19=RIGHTTRIGGER

### Ligacoes de GAMEPLAY do build MOBILE (touch) — descobertas empiricamente
- O jogo le APENAS eventos (GetGamepadButtons/GetGamepadAxis NUNCA sao chamados
  — provado com instrumentacao; so 3 calls de skip-movie no binario inteiro).
- **16-19 (shoulders/triggers) NAO fazem NADA no gameplay** — o build touch
  religou as 6 acoes "extras" do PS2 no cluster 6/7/12-15:
  - 6 = MIRA/lock-on (e pagina-esq nos menus)
  - 7 = DISPARAR (e pagina-dir nos menus — por isso "paginava no R3" no v5)
  - 12 = olhar p/ tras · 13 = agachar · 14 = item-esq · 15 = item-dir
- implOnGamepadButtonDown: guarda state[24+btn] (base 0x158a9a8, 60B/porta,
  btn<32) + enfileira LIB_InputEvent(6/7).

### Pegadinhas que custaram horas
1. **Comentario inline no .gptk MATA a linha** (parser inih so aceita
   comentario em linha propria). Linhas com `# ...` apos o valor nunca agiram.
2. **Neste pad/CFW o gptokeyb enxerga shoulder<->gatilho TROCADOS**
   (fisico L1 chega como `l2`, fisico L2 como `l1`, idem direita) — provado
   com log [kbd] em ordem controlada. O .gptk cruza as atribuicoes.
3. O caminho de POLL lendo o pad fisico por fora do gptokeyb mascarava tudo
   (botoes de cara "funcionavam" sem gptokeyb). Normalizado p/ teclado.
