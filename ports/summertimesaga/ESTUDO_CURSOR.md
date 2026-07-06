# Summertime Saga — estudo do cursor engasgando

Data: 2026-07-06. Device: 192.168.31.79.

## APLICADO (2026-07-06, aguardando teste do usuário — jogo NÃO foi aberto)

Stage 1 aplicado SÓ em `renpy/display/core.py` (deployado no device;
`py_compile` OK; sem rebuild do .so; launcher/input.c/egl intactos):

1. **Corrida de dois leitores eliminada** — `_SummertimeCursorDisplayable.render`
   não lê mais o arquivo; a posição vem só do pump (fonte única). Isso mata
   o "pulo" do cursor.
2. **Pin de 60fps eterno → pacing gated** — o self-redraw do displayable era
   `0.016` (60fps pra sempre, o que fritava o Mali-450). Agora: `0.033`
   (~30fps) enquanto move, `0.05` (~20Hz) ocioso só pra captar o início de
   um novo movimento em ≤50ms.
3. **Foco/hover throttle 83Hz→30Hz** — `_summertime_next_motion_time` de
   `0.012` para `0.033`, cortando o recomputo de foco por passo (o custo
   real em telas cheias de hotspot).
4. **Crosshair só quando EGL off** — `render` só desenha o crosshair se
   `SUMMERTIME_CURSOR=0` (config atual), deixando pronto o modo "EGL desenha,
   displayable vira marca-passo invisível" sem cursor duplo, caso se ligue
   o overlay depois.

Nota de projeto: como o displayable é o "marca-passo" de frames (o
`check_redraws` do render.pyx só dispara p/ displayable no `render_cache`),
ligar o overlay EGL NÃO deixaria mais liso que o crosshair nesse teto de
30fps — por isso o launcher não foi tocado (menor risco, visual equivalente).

Controles: **já eram nativos** (`src/input.c`: evdev → SDL onNativePad/Joy/Hat).
Nada a fazer neles.

### Validação pendente (quando o usuário abrir)
- `SUMMERTIME_PERF=1`: comparar `[PERF] fps/avg/max/>20ms/>40ms` — esperado
  menos spikes ao mover e CPU/calor baixos ocioso.
- Manual: andar com o cursor numa cena cheia de hotspots — sem pulo/engasgo;
  clique cai onde o cursor está.
- Se AINDA trepidar nas cenas mais pesadas → Stage 2 (FBCOPY, abaixo).

---

## Plano original (referência)

## Sintoma
Cursor engasga / pula / trava ao andar dentro do gameplay. É um jogo 100%
de cursor (Ren'Py / librenpython.so), então precisa ser liso.

## Causa raiz (confirmada lendo o código)

Existem DUAS implementações de cursor e o port hoje roda a **pior
combinação** (`Summertime Saga.sh` linhas 66-67):

- `SUMMERTIME_CURSOR=0`  → overlay EGL liso **DESLIGADO**
- `SUMMERTIME_RENPY_CURSOR=1` → cursor desenhado pela engine **LIGADO**

Consequências dessa combinação:

1. **Pin de 60fps full-frame (o principal).**
   `_SummertimeCursorDisplayable.render()` (core.py ~177) chama
   `renpy.display.render.redraw(self, 0.016)` a CADA render. Como ele é o
   `config.mouse_displayable`, isso re-arma um redraw de 16ms pra sempre →
   a engine fica presa redesenhando a TELA INTEIRA a 60fps o tempo todo,
   até parado. No Mali-450, com as cenas grandes do Summertime, o device
   não sustenta → frame-time estoura (por isso existem os contadores
   `[PERF] >20ms/>40ms` no egl_shim) → engasga.

2. **Cada passo do cursor passa pelo pipeline pesado.**
   move → poll de arquivo → post `MOUSEMOTION` → **recomputo de foco sobre
   todos os hotspots da tela** → composite full → swap, até 60Hz. O
   recomputo de foco em telas cheias de hotspot é caro e é feito por passo.

3. **Corrida de dois leitores (o "pula").**
   `_summertime_pump_virtual_cursor` (seq `_summertime_cursor_seq`) e
   `_SummertimeCursorDisplayable.render` (seq `_summertime_cursor_draw_seq`)
   leem o MESMO arquivo `/dev/shm/summertime_vcursor` com seqs separados e
   ambos escrevem `_summertime_cursor_pos`. A posição desenhada e a
   lógica/hover divergem frame a frame → cursor pula.

4. **Batimento de taxas.** 4 cadências dessincronizadas: 8ms (compute em
   input.c), 16ms/8px (escrita do arquivo), 12ms (throttle do MOUSEMOTION),
   16ms (self-redraw). Passos irregulares → trepidação.

5. **I/O de arquivo por frame** em Python (open/read/parse) tanto no pump
   quanto no render → variância de latência.

### Por que o overlay EGL foi desligado (e por que foi a escolha errada)
Foi desligado pra evitar "cursor paralelo" (desync entre onde o cursor
aparece e onde o clique cai). Mas o overlay EGL é o ÚNICO caminho que pode
ser liso nessa GPU: desenha o cursor barato (scissor-clear) logo antes do
swap, independente do custo da cena. O desync se resolve mantendo UMA fonte
de verdade (`summertime_cursor_x/y`, que já é compartilhada) tanto pro
desenho EGL quanto pro hover/clique do Ren'Py.

### Detalhe que importa (limite físico)
Quem faz o swap é o Ren'Py: o overlay EGL só atualiza quando a engine
desenha um frame. Ou seja, o cursor ainda precisa que FRAMES sejam gerados
ENQUANTO se move. O truque é disparar redraws baratos SÓ enquanto o
analógico está defletido, com taxa limitada, e ficar 100% idle (0 redraw)
quando centrado — em vez do pin atual de 60fps eterno.

## Solução preparada (staged)

### STAGE 1 — "EGL visual + Ren'Py só-hover + redraw só ao mover" (barato, deve resolver)

**A) Launcher `Summertime Saga.sh` (linhas 66-67):**
```
export SUMMERTIME_CURSOR=1        # liga overlay EGL liso (cursor VISUAL)
export SUMMERTIME_RENPY_CURSOR=0  # engine NÃO desenha cursor (mata o pin 60fps)
```

**B) `renpy/display/core.py`:**
- `_summertime_install_virtual_cursor` já retorna cedo se
  `SUMMERTIME_RENPY_CURSOR==0` → com o flip acima, o `mouse_displayable` e o
  self-redraw de 16ms somem por completo. (nenhuma mudança extra aqui.)
- Em `_summertime_pump_virtual_cursor`: torná-lo SÓ-HOVER e de baixa taxa:
  - throttle do `post_mouse(MOUSEMOTION)` para ~30Hz (33ms) — suficiente pro
    highlight de hover, corta o custo de foco pela metade.
  - remover as chamadas `renpy.display.render.redraw(_summertime_cursor_displayable, …)`
    (não há mais displayable).
  - ler a posição de UMA fonte só (sem o segundo leitor no render).
- **Redraw gated por movimento:** quando o pump detectar que a posição
  mudou desde o último frame, agendar UM redraw (cap 30fps) pra manter
  swaps acontecendo enquanto move; quando não mudou, não agendar nada → a
  engine idle a 0 redraw. Implementar com um displayable mínimo (1px)
  sempre presente recebendo `render.redraw(…, 0.033)` SÓ quando mover, ou
  `renpy.display.interface.force_redraw=True` guardado por timer de 33ms.
  O frame gerado roda o hook de swap do EGL → o cursor EGL anda liso.

**C) `src/input.c` (ajuste fino opcional):**
- Manter a dupla escrita (floats compartilhados p/ EGL + arquivo p/ hover);
  ambos derivam de `g_mouse_x/y`, então EGL e clique batem.
- Opcional: alinhar o coalescing do arquivo/motion a ~30Hz p/ não bater.

### STAGE 2 — só se a Stage 1 ainda trepidar nas cenas mais pesadas
"Cursor 60fps desacoplado via FBO da cena salva (padrão FBCOPY, igual ao
port LEGO Batman)": depois do frame de cena do Ren'Py, capturar a cena num
FBO/textura; um loop dedicado de cursor faz blit(cena salva)+cursor e swap
a 60fps, independente do redraw/foco da VN. Desacopla 100% a suavidade do
cursor do custo da cena. Mais trabalhoso — deixar como fallback.

## Validação (quando aplicar)
- Rodar com `SUMMERTIME_PERF=1` e olhar `[PERF] fps/avg/max/>20ms/>40ms`
  antes/depois. Alvo: ao mover, avg perto do vsync (16-33ms), poucos >40ms;
  idle = ~0 redraw (CPU/calor baixos).
- Manual: mover o cursor por uma cena cheia de hotspots — sem pulo/engasgo;
  confirmar que o clique cai onde o cursor está.

## Arquivos/âncoras
- `src/input.c`: cursor_thread (8ms, ~L165), mouse_move_to (L98),
  emit_pointer_file (L81), floats `summertime_cursor_x/y` (L62).
- `src/egl_shim.c`: draw_port_cursor (L344, scissor-clear), gate
  `SUMMERTIME_CURSOR` (L345), swap hook (L371), contadores [PERF] (L381).
- `renpy/display/core.py`: pump (L93), displayable+self-redraw (L147-178),
  install/`mouse_displayable` (L181-195), chamada do pump no loop (L3266).
- `Summertime Saga.sh`: envs de cursor (L66-67).

---

## Sessao 2026-07-06 (noite) — cursor de HARDWARE + perf/texturas

### Cursor de hardware (APLICADO, aprovado "liso e limpo perfeito")
- `/dev/fb1` = Amlogic OSD2, camada de cursor por hardware (32x32 ARGB,
  `osd_cursor_hw` no kernel 3.14). O loader pinta a seta no fb1 e move via
  `ioctl(FBIO_CURSOR)` a 125Hz na cursor_thread -> fluidez TOTAL,
  independente do fps da engine. Fallback automatico sem fb1 (R36S).
- `fb1_init` roda ANTES do SDL_main (setenv SUMMERTIME_FB1_CURSOR=1 precisa
  existir quando o os.environ do Python nasce).
- core.py: com FB1 ativo nao desenha crosshair, nao forca frames; escreve
  /dev/shm/summertime_hover no CHANGE de foco -> seta fica VERMELHA no hover.
- Saida: summertime_fb1_blank() no Select+Start + `echo 1 > fb1/blank` no
  trap do launcher (senao a seta fica por cima da ES).
- QUADRADO PRETO: OSD2 nao mescla alpha por pixel; transparencia correta e
  por COLOR KEY: fundo do sprite = magenta 0x00FF00FF + sysfs
  `color_key=0x00ff00ff` + `enable_key=1` (osd2_update_color_key no kernel).
- Cursor original do jogo: NAO existe no APK Android (build touch).

### Audio (RESOLVIDO)
- Sem som porque o launcher exporta HOME=$GAMEDIR e o pacat nao achava o
  servidor Pulse (erro engolido por 2>/dev/null). Fix no binario (audio.c):
  ensure_pulse_server() detecta /run/pulse/native e seta PULSE_SERVER.

### Perf/texturas
- PERF real: cena estatica ~10fps (98ms) -> gargalo e render/GPU, nao foco.
- MOUSEMOTION (recomputo de foco ~80ms) baixado p/ 10Hz ao mover.
- Assets: 22.862 PNGs; 6.307 > 1280x720 (540 em 1920+). Na GPU vira
  RGBA8888 cru (Ren'Py GLES2 nao usa compressao).
- Downscale por config.py: `max_texture_size=(SUMMERTIME_MAX_TEX,idem)`,
  default 1280; launcher agora seta 1024 (bg 1920x1080 -> 1024x576, -70%
  VRAM/banda). Usuario aprovou visual do downscale ("downscale perfeito").
- ETC1: DESCARTADO (sem alpha; arte do jogo e quase toda com alpha; encoder
  lento; loader Ren'Py nao le comprimido).
- PLANO B (se 1024 nao bastar): converter upload p/ 16-bit no
  wrap_glTexImage2D do imports.c (RGB565 opaco / RGBA4444 com alpha) =
  metade da banda, preserva alpha, art style cartoon esconde bem.
