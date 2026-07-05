# Mega Man X (Capcom) → Mali-450 — HANDOFF completo (para a próxima seção)

## 2026-07-05 s10 — 🏆 CONTROLES COMPLETOS (A/X/Y/L1/R1) + MODO CURSOR com SELECT

**O que foi resolvido nesta seção (provas em `runs/`-style screenshots via fb0):**

1. **Mapeamento REAL do `game_key` (enum GAMEKEY no global-metadata.dat, validado por disasm):**
   `0=UP 1=UP_2 2=DOWN 3=DOWN_2 4=LEFT 5=RIGHT 6=ATTACK 7=CHANGE(arma) 8=JUMP 9=DASH 10=DASH_2`.
   ⚠️ O mapa antigo (2=JUMP 3=SHOT) estava ERRADO — o "pulo" do FORCE_IDX=2 do s9 era o **DASH**
   (game_key[2]=DOWN compartilha a máscara 0x8000 word1 com game_key[9]=DASH). NÃO existe
   START/SELECT/WEAPON_PREV/NEXT no game_key (herança keitai). Helpers: isKeyTrg=0xde8338,
   isKeyOn=0xde86a8, isKeyRel=0xde84f0.
2. **PULSO de trigger (a causa do A/X/Y mortos):** o FORCE_IDX funciona porque segura held+real
   TODO frame; botão físico dava 1 frame de real (perdido) e o kd0 constante matava a detecção
   0→1 do controlKey. Fix: `mmx_ctrl_sample()` (1 amostra por chamada do controlKey, compartilhada
   pelos DOIS pontos de injeção) arma `MMX_CTRL_PULSE` frames (default 3) de plano real por borda.
   **PROVADO: A=pulo (X no alto do salto), X=tiro (rajada de buster na tela), Y=dash (pose de dash).**
3. **Botões (layout FÍSICO do pad do usuário, que chega trocado no SDL: físA=SDL1, físB=SDL0,
   físX=SDL3, físY=SDL2):** A=JUMP(8), Y=SHOT(6), X=DASH(9), B=DASH (BTN_DASH2), L1/R1=CHANGE(7)
   (o jogo só tem UMA ação de troca, ambos ciclam), START=pause (ver muro), SELECT=modo cursor,
   SELECT+START=sair. **KeyEvents nativos dos botões de face SUPRIMIDOS** (BUTTON_B nativo dava
   dash e conflitava com o remap; `MMX_NATKEYS=1` restaura). Envs: `MMX_CTRL_BTN_*` (botão SDL),
   `MMX_CTRL_IDX_*` (índice game_key), `MMX_CTRL_PULSE`. **VALIDADO por screenshot: pulo no
   SDL1, rajada de buster no SDL2, dash no SDL3.**
4. **🖱️ MODO CURSOR (estilo COD BOZ) FUNCIONANDO:** SELECT alterna game-mode↔cursor-mode.
   Cursor = crosshair GL (scissor+clear estilo vkbd, com bind de FBO 0 + colorMask no swap) movido
   por dpad/analógico (X segurado = fino), A = toque real (down/move/up, arrastar funciona).
   Teclas do jogo são suprimidas em cursor-mode; SELECT nunca vaza pro jogo.
   **PROVADO ponta-a-ponta: título→(FORCE_TITLESTART)→MAIN MENU→cursor até STORY→tap duplo→
   scn_LOAD_DATA→scn_STAGE_run.** 🔑 Menus do jogo pedem **2 taps** (1º seleciona, 2º confirma).
   ⚠️ O hook do eglSwapBuffers agora também é patchado com `MMX_GAMEPAD` (era só
   rs/TER_SHOT/NUKEKB/JOBWORKERS0 — sem isso o cursor não desenhava).
5. **GKDUMP:** `MMX_CTRLSPY=1` agora dumpa as máscaras de game_key[0..10]/def_key[0..15] quando
   populadas (diagnóstico de mapeamento).

**MUROS ABERTOS:**
- **PAUSE em gameplay (quase):** por disasm, NÃO existe touch-region da engrenagem (enum TOUCH_ID
  não tem pause) — a engrenagem usa o sistema interno "Touch group" (coords do screen virtual
  keitai, não 1280x720) e abre o menu como **DIALOG**: `initDialog(self,30,título,msg,cb)`
  @0xe101d4 + `scn_setStep(self,19)` @0xdecdac (sequência da engrenagem em 0xe02d6c-0xe02d98).
  START já chama isso (mmx_try_pause, na thread do jogo via hook do controlKey) mas com args
  NULL não abriu (sem crash). Falta replicar os args reais dos campos estáticos de
  0xe02d64-0xe02d88 (subagente destrinchando). RVAs úteis: scn_STAGE_run=0xdfc32c,
  selectDialog=0xe0ffb4, endDialog=0xe10158, drawDialog=0xe10bb4, scene obj=[self+0x370],
  step=+56, substep do dialog=+84.
- **STORY via menu = tela preta** (movie do prólogo; codec indisponível no so-loader). O caminho de
  produção continua `MMX_GOSTAGE=0` (auto-start, pula o movie). Cursor cobre menus in-game.
- Troca de arma L1/R1: mapeada por metadata (CHANGE=7); validação visual pendente (fase intro não
  tem armas especiais).

---

## 2026-07-04 s9 — estudo dos botões A/Y/Start: sem regressão, estado restaurado, próximo passo é pulso curto

**Estado salvo para retomar depois:** o device foi restaurado ao checkpoint estável do s8:

- `/storage/roms/megamanx/megamanx` SHA256 `5ecffe70c84c34e8aa6f6a851997232626845eda96348d452f2319973c9317bc`
- `/storage/roms/megamanx/run.sh` SHA256 `42dd1639693645ab39df778ec7a06c5863c677c8ed5b1a58653787d36fc645e3`
- Nenhum processo `./megamanx` ficou rodando no device.
- As tentativas ruins foram revertidas do código e do `run.sh`; não deixar `MMX_CTRL_REAL_HELD=1` no default.

**O que foi testado e NÃO resolveu:**

1. `MMX_CTRL_BTN_JUMP=0` explícito no `run.sh`. Sem efeito; isso já era o default do hook.
2. Override de keycode do A para `SPACE` (`KEYCODE_SPACE=62`), porque o `InputManager` do Unity tem
   `Jump=space/joystick button 0`. Sem efeito; revertido.
3. Híbrido pad nativo + touch só para ações (`A` pulo, `Y` tiro, `START` pause) nas coords antigas
   `MMX_JX/JY`, `MMX_SX/SY`, `MMX_STX/STY`. Sem efeito; revertido.
4. `MMX_CTRL_REAL_HELD=1 MMX_CTRL_BTN_SHOT=3`. Provou que real/trigger sustentado é perigoso: gerou
   **dash infinito**. Não usar globalmente.

**O que foi comprovado:**

- O pad físico chega ao jogo: logs mostram KeyEvents `A=96`, `B=97`, `X=99`, `Y=100`, `LB=102`, `RB=103`.
- O `B` dá dash pelo caminho nativo Android/Unity e deve ser preservado. Não usar `MMX_SWAPAB`, não remapear
  dash globalmente e manter `MMX_CTRL_IDX_DASH=-1` até haver motivo forte.
- `A/Y/X/LB/RB` chegam como KeyEvent, mas a Unity/jogo não converte isso em pulo/tiro nessa fase.
- A prova importante: `MMX_CTRL_FORCE_IDX=2 TER_SHOT=900 sh run.sh` faz o X ficar no ar/pular.
  Screenshot: `runs/2026-07-04-codex-forceidx-study/force_idx_2.png`
  SHA256 `46c70129621eb8e71e0040c55e9441a3b1136041861ff0de1d1a22f743aff3fa`.
  Log: `runs/2026-07-04-codex-forceidx-study/force_idx_2.run.out`
  SHA256 `ee547770f5c16bfa0e4f91681e87a4c1b1c39bc13511aa1b3041c09b3596de12`.
- Conclusão: `game_key[2]` é de fato pulo e o caminho direto `RockmanX.controlKey` consegue acionar ação.
  O problema do A físico não é SDL/keycode simples; é a duração/semântica do trigger.

**Próximo passo recomendado para Claude/Codex:**

Implementar um pulso curto por ação no hook direto, não um `REAL_HELD` global:

- Ao detectar borda de `A`, injetar `game_key[2]` por 2-3 frames nos planos que `controlKey`/`rock_keyAction`
  esperam (`KeyFlagReal`/`key_data` real), depois soltar.
- Para tiro, primeiro fazer sweep com `MMX_CTRL_FORCE_IDX=N` (provável `3`, mas confirmar visualmente/log) e
  então aplicar o mesmo pulso curto ao `Y`.
- Não tocar no `B`: ele já dá dash nativo e qualquer trigger global pode causar dash infinito.
- Se for instrumentar, usar `MMX_CTRLSPY=1` com cuidado e salvar runs separados; os logs verbosos atrasam o jogo.

---

## 2026-07-04 s8 — 🔊 ÁUDIO DESBLOQUEADO: OpenSL/SDL + fallback de streams, sem regressão de gameplay

**Estado atual:** `run.sh` agora sobe **full version + gameplay + pad físico + áudio**. O único muro grande
restante é **navegação de submenus por controle** (pause/stage-select/game-over/customize ainda são touch).

**Causa real do áudio mudo:**

1. O FMOD/Unity desta build escolhia o output **21** (AudioTrack/Java fake) e por isso **nunca chamava
   `slCreateEngine`**. O shim OpenSL estava pronto, mas não era alcançado.
2. O seletor real fica em **`libunity+0x423cf4..0x423cfc`**:
   `cmp w0,#2; mov w8,#21; cinc w21,w8,eq`. Se a checagem não retorna 2, cai no output 21.
3. `MMX_FORCESL=1` força o seletor para **22/OpenSL**:
   `0x423cf8=0x528002c8` (`mov w8,#22`) e `0x423cfc=0x2a0803f5` (`mov w21,w8`).
   Prova em log: `[DLOPEN] libOpenSLES.so -> opensles_shim`, `[SL] slCreateEngine`,
   `CreateOutputMix`, `CreateAudioPlayer`, `bq_Enqueue`, `SetPlayState`, callbacks do pump SDL.
4. Com OpenSL ativo, o próximo erro era `createSound` em modo stream: `mode=0xd2 stream=1 -> 33`.
   Samples (`mode=0x152 stream=0`) já abriam com `-> 0`. O wrapper real é **`libunity+0x9ccaa8`**.
5. `MMX_STREAMFALLBACK=1` hooka esse wrapper e, quando stream falha com 33, tenta de novo como sample
   (`mode & ~0x80`, observado `0xd2 -> 0x52`). Resultado: **0× `Cannot create FMOD::Sound`** no run limpo.

**Receita atual (run.sh):** `MMX_INLINETASK=1 MMX_PATCH=0x34eafc=0x14000005 MMX_NOINTEGRITY=1
MMX_PREFSTRUE=1 MMX_FIXGAME=1 MMX_FULLVER=1 MMX_XLATE=1 MMX_BOOTST=1 MMX_FORCESL=1
MMX_STREAMFALLBACK=1 MMX_GAMEPAD=1 MMX_CTRLHOOK=1 MMX_CTRL_KEYFLAG_PRE=1 MMX_KEYINIT=1
MMX_GOSTAGE=0 MMX_GOSTAGE_F=280`.

**Validação anti-regressão:**

- Baseline pós-build sem `MMX_FORCESL`/`MMX_STREAMFALLBACK`: `GOSTAGE` e `CTRLHOOK` continuaram OK, sem logs
  novos de áudio. Ou seja, o caminho novo é gated por env.
- Run limpo de ~45s com `MMX_FORCESL=1 MMX_STREAMFALLBACK=1`: OpenSL iniciou, callbacks SDL rodaram,
  stream fallback removeu os `Cannot create FMOD::Sound`, e o jogo ainda entrou na fase via `setGoStage`.
- Screenshot: `runs/2026-07-04-codex-audio-forcesl-fallback/audio_forcesl_fallback.png`
  SHA256 `2dea0c00f899aec7aa23ee8c9de416a421a9df7c2b91125318ed8fc93fd02a2b`.
- Log salvo: `runs/2026-07-04-codex-audio-forcesl-fallback/run.out`.

**Ferramentas novas:** `MMX_FORCESL=1` (força output 22/OpenSL no libunity), `MMX_AUDIOSPY=1`
(diagnóstico verboso do `createSound` real em `libunity+0x9ccaa8`), `MMX_STREAMFALLBACK=1`
(retry de streams FMOD como sample quando retornam 33).

---

## 2026-07-04 s7 — 🏆🏆 DESBLOQUEADO + JOGÁVEL: BUY VERSION fora, X anda na fase pelo pad

**Sequência de vitórias (commits `921fa65`, `83e5016`, `8a787fe`):**

1. **VERSÃO COMPLETA DESBLOQUEADA** (`921fa65`). O gate do trial é **`RockmanX.IsInAppPayment(this,a,b)`**
   (il2cpp+0xdd82c4). Em `TITLEMENU_run` (df6318): `bl IsInAppPayment; tbz w0,#0, buyPath` — TRUE=segue
   (`scn_goLoadScene`), FALSE=fica no menu/buy. `STAGE_init` gateia 5× igual. O FIXGAME forçava **FALSE**
   (=trial). Agora sob `MMX_FULLVER` força **TRUE** → **"BUY FULL VERSION" some do menu**, aparecem
   **STORY MODE + RANKING MODE** (menu completo). Não era a ProductInfo.isUnlock/isUse (essas já eram
   forçadas true e não bastavam) — era o IsInAppPayment.
2. **GAMEPLAY JOGÁVEL** (`83e5016`). `MMX_GOSTAGE=N` chama **`RockmanX.setGoStage(this,N)`** (il2cpp+0xe0fa74),
   que **monta os dados da fase** e roteia PROLOGUE→STAGE (cena 12). O bounce do PROFORCE era pular pra
   cena 12 crua SEM dados (fase vazia termina→título); `setGoStage` monta tudo = "New Game". **PROVA:
   `runs/2026-07-03-codex-ctrl-fixed-forceidx/GAMEPLAY_x_dash_right.png` = o X DASHANDO na fase intro
   (rodovia/oceano), HUD completo, `scn_STAGE_run` 42× persistente.** Subagente confirmou por disasm:
   `scn_STAGE_run` lê input INCONDICIONALMENTE (sem gate de demo); TRIAL_DEMO(cena 20) é só o attract do
   título, NUNCA alcançado via Story. Cenas: TITLEMENU=6 STAGESEL=9 STAGE=12 PROLOGUE=16 TRIAL_DEMO=20.
3. **PAD FÍSICO FUNCIONA** (`8a787fe`). Faltava **`MMX_GAMEPAD=1`** — ele liga `mmx_gamepad_frame` (poll do
   pad SDL → `g_btn`). Sem ele `mmx_gp_button` sempre retorna 0 e só o `MMX_CTRL_FORCE_IDX` de debug andava.
   **PROVA: com `MMX_GAMEPAD=1` + input `right` (via g_btn, NÃO force) o CTRLHOOK mostra `gp=0x2000`
   (RIGHT) → `key` bit setado → X dasha na fase (`pt_14.png`).** run.sh liga tudo + **auto-start**
   (`MMX_GOSTAGE=0`) já que o menu principal é touch-only → o jogador cai direto na fase controlando o X.

**Receita jogável (run.sh):** `MMX_INLINETASK=1 MMX_PATCH=0x34eafc=0x14000005 MMX_NOINTEGRITY=1
MMX_PREFSTRUE=1 MMX_FIXGAME=1 MMX_FULLVER=1 MMX_XLATE=1 MMX_BOOTST=1 MMX_GAMEPAD=1 MMX_CTRLHOOK=1
MMX_CTRL_KEYFLAG_PRE=1 MMX_KEYINIT=1 MMX_GOSTAGE=0 MMX_GOSTAGE_F=280`
(s8 adicionou `MMX_FORCESL=1 MMX_STREAMFALLBACK=1` para áudio).

**Ferramentas novas:** `MMX_GOSTAGE=N`(New Game/fase N via setGoStage), `MMX_ILPATCH=0xOFF=0xWORD`(patch cru
no .text do libil2cpp). Mapa de controle: dpad/analógico=mover, A=pulo, X=tiro, Y/RB=dash, LB=arma, START.

**MUROS ABERTOS (o resto da missão):**
1. **ÁUDIO foi resolvido no s8** com `MMX_FORCESL=1 MMX_STREAMFALLBACK=1`. O texto abaixo é histórico.
2. **NAVEGAÇÃO DE SUBMENUS POR CONTROLE** — o menu principal é contornado pelo auto-start, mas submenus
   in-game (pause, stage-select pós-intro, game-over, customize) são touch e precisam de pad→touch. O menu
   NÃO responde a tecla (`isKeyTrg(game_key[2])` existe em fases posteriores do TITLEMENU, mas o grid
   principal é touch puro — testado: decide via pad não moveu). Toque injetado FUNCIONA
   (`MMX_AUTOTOUCH=1 MMX_TOUCHX/Y`, coords 1280×720 — tap em Story (290,385) entra no fluxo). Falta um
   cursor/seta pad→touch (o jogo renderiza via EGL, não fb0 direto → desenhar cursor exige quad GL no
   my_eglSwapBuffers). Alternativa: mapear botões do pad a toques nas posições fixas de cada menu.

---

## 2026-07-03 s6 — 🏆 CONTROLE RESOLVIDO NA RAIZ: era BUG DE LEITURA (maps cache velho), NÃO keymap vazio

**A causa-raiz do "controle não funciona" de TODAS as sessões anteriores foi encontrada e corrigida.**
O sintoma que travou o s4/s5 (`key_data len=0`, `game_key glen=0`) era **FALSO**. Os arrays de keymap
SEMPRE estiveram alocados corretamente pelo `.ctor` do RockmanX (`KeyFlag=uint[3]`, `KeyFlagReal=uint[3]`,
`key_data=uint[7][]`, `def_key=uint[47][]`, `game_key=uint[11][]`).

- **O bug:** `mmx_managed_array_len()` (helper que lê o `Length` do array em `arr+0x18`) usava
  `addr_readable()`, que valida o ponteiro contra um **snapshot ANTIGO de `/proc/self/maps`** (`g_maps_buf`).
  Esse cache foi tirado ANTES do GC heap do il2cpp ser mapeado → os endereços `0x7e../0x7f..` do heap
  gerenciado **não estão no cache** → `addr_readable` retorna falso → `mmx_managed_array_len` retorna **0**
  pra arrays perfeitamente válidos. Todo o pipeline de injeção (`mmx_ctrl_apply`) lia máscara vazia e não
  fazia nada. O dump RAW provou: `nGameKey len0x18=0xb` (=11), `KeyFlag len0x18=3`.
- **O fix (commit desta seção):** `mmx_managed_array_len`/`mmx_ref_array_get`/`mmx_u32_array_data` leem
  o array **direto** (os ponteiros vêm de campos managed já validados), com só uma guarda de ponteiro
  (`mmx_ptr_sane`: não-nulo, alinhado a 8, faixa de heap). Sem `addr_readable`.
- **PROVA ponta-a-ponta (logs no device):** com `MMX_CTRL_FORCE_IDX=5` (força RIGHT):
  `[CTRLHOOK] force=5 ... key=00000000/00000001/00000000` (KeyFlag recebeu o bit de game_key[5]) e depois
  do controlKey original: `[CTRLHOOKKD] kdlen=7 ... kd0=00000000/00000001/00000000` — **`key_data[0]`
  (o plano que `isKeyOn`/`isKeyTrg` leem) recebeu o bit.** O `rock_keyMove` chama `isKeyOn(game_key[5])`
  = `key_data[0] & game_key[5]` = **RIGHT pressionado**. Idem esquerda (idx 4), etc.
- **Encoding decodificado (disasm):** `isKeyOn(mask)` → `key_data[0] & mask`; `isKeyNow(mask)` →
  `KeyFlag & mask`; a `mask` é `game_key[acao]`. `controlKey()` ingere `KeyFlag`(0x2c0) nos planos de
  `key_data`(0x2d0) e faz o shift de trigger. `game_key` mapeia ação→bits físicos: `rock_keyMove` lê
  `game_key[4]`=LEFT, `game_key[5]`=RIGHT (offsets 64/72). Mapa em `mmx_ctrl_apply`: UP=0 DOWN=1 JUMP=2
  SHOT=3 LEFT=4 RIGHT=5 (tunável por `MMX_CTRL_IDX_*`).
- **run.sh atualizado** liga os controles por padrão: `MMX_CTRLHOOK=1 MMX_CTRL_KEYFLAG_PRE=1 MMX_KEYINIT=1`
  (+ boot + shaders + `MMX_FULLVER`). Pad físico SDL → input do jogo (mesmíssimo caminho do force-idx).
- **`MMX_KEYINIT`** ficou como rede de segurança: realoca os arrays externos via `il2cpp_array_new` +
  chama `initKey`/`initTouchKey`/`setGameKey` SE algum dia vierem realmente vazios (com o fix, não vêm).

**MUROS AINDA ABERTOS (SEPARADOS do controle — não são bug de input):**
1. **Menu é TOUCH.** Segurar tecla no MAIN MENU não confirma. `MMX_AUTOTOUCH=1 MMX_TOUCHX=290 MMX_TOUCHY=385`
   (coords 1280×720, painel "STORY MODE") ENTRA no fluxo de story (log mostra `scn_LOAD_DATA/STAGESEL/STAGE`).
   Falta: pad→touch nos menus (estilo MM5/6) pra navegar sem PC/mouse.
2. **Trial/demo volta ao título.** Story mode roda um trial curto e retorna a `scn_TITLEMENU` (gate de
   versão completa em nível de DADOS, não o getter que o `MMX_FULLVER` força). É o mesmo "trial-mode
   blocking gameplay" que o memory já listava. Enquanto isso, `MMX_PROFORCE_SCENE=12` cai num demo que
   também bounce em ~10s — não dá janela estável pra capturar o X andando por injeção.
   ⇒ Próxima missão pra ver o X andando de fato: resolver o gate de trial (fullver no nível de save/dados)
     OU navegar via pad→touch até uma fase real e persistente.

**Runs desta seção:** `runs/2026-07-03-codex-ctrl-fixed-forceidx/` — `fb_menu.png` (MAIN MENU, Story
destacado, fullver ok), sequência `seq_*/pl_*` (fluxo story→bounce), e os logs com a prova `kd0` acima.

---

## 2026-07-03 s5 — pausa solicitada: Xbox nativo confirmado no Unity, controle ainda bloqueado por keymap vazio

**Estado salvo antes da pausa:** não há processo `megamanx` rodando no device. Último binário foi buildado e enviado para `/storage/roms/megamanx/megamanx`. Runs/logs/screenshot salvos em `runs/`.

**Vitórias confirmadas:**
- **Render:** X e fase continuam limpos. Sem retorno do magenta/bloco amarelo.
- **Full version:** `MMX_FULLVER=1` agora força `ProductInfo` via hooks de `CStoreKit.getProductInfo`/`MakeProductData`; nos runs forçados para fase não aparece mais o gate "BUY FULL VERSION".
- **Xbox nativo existe e está parcialmente funcionando:** o jogo/Unity Input System reconhece o device virtual como `XboxOneGamepadAndroid`; `nativeInjectEvent` retorna `ret=1`; Unity consulta `MotionEvent.getAxisValue(...)` e aceita `KeyEvent`/`MotionEvent`.
- **Regra crítica de teste:** para validar controle, usar `MMX_KEEP_CONTROLKEY=1` ou `MMX_CTRLHOOK=1`. Sem isso, `MMX_FIXGAME` neutraliza `RockmanX.controlKey` e o teste vira falso negativo.

**Controle — achado final antes da pausa:**
- `MMX_CTRLHOOK=1` hooka `RockmanX.controlKey` em `il2cpp+0xdd6d60`.
- Field dump confirmou offsets reais:
  - `KeyFlag +0x2c0`
  - `KeyFlagReal +0x2c8`
  - `key_data +0x2d0`
  - `def_key +0x2d8`
  - `game_key +0x2e0`
  - `touchkey_tbl +0x2e8`
  - `flg_touchKey +0x2f0`
  - `flg_touchPress +0x300`
  - `flg_touchRelease +0x308`
- No run pausado, o auto-pad chega ao hook: `gp=0x2000 act=0x8` = direita ativa. Porém `key_data len=0` e `game_key glen=0` continuam zerados mesmo na fase forçada por `MMX_PROFORCE_SCENE=12`.
- Conclusão prática: **não é mais problema de "o controle não chega". O estado do pad chega. O bloqueio atual é que o fluxo forçado entra na fase sem inicializar a tabela de keymap (`key_data/game_key`).**

**Próximo passo recomendado ao retomar:**
1. Tentar inicializar/forçar o keymap antes ou durante `scn_STAGE_run`:
   - investigar `RockmanX.initGame`/rotas de menu real que preenchem `key_data/game_key`;
   - ou copiar `def_key`/montar arrays para `game_key` se `def_key` estiver preenchido.
2. Plano B mais direto: injetar nos arrays touch do próprio `RockmanX` (`flg_touchKey`, `flg_touchPress`, `flg_touchRelease`, `touchPress_x/y`) em vez de depender de `game_key`.
3. Plano C: manter caminho Xbox nativo e instrumentar `UnityEngine.InputSystem` (`Gamepad`, `InputAction`, `AndroidGamepad`) para descobrir por que a lógica Capcom não consome o gamepad.
4. Áudio segue pendente: FMOD ainda spamma `Cannot create FMOD::Sound instance for clip "0"`/`"114"`; no último run foram muitas linhas de erro. Só atacar depois de controles.

**Runs novos importantes:**
- `runs/2026-07-03-codex-native-xbox-keep-controlkey/`
  - Xbox nativo com `MMX_KEEP_CONTROLKEY=1`.
  - `nativeInjectEvent` aceitou eventos (`ret=1`) e a fase renderizou.
- `runs/2026-07-03-codex-fielddump-rockmanx-input/`
  - Dump confirmou os offsets de `RockmanX` listados acima.
- `runs/2026-07-03-codex-ctrlhook-actlog-pause/`
  - Último run antes da pausa.
  - Prova chave: `CTRLHOOK enter ... gp=0x2000 act=0x8`, mas `key_data len=0` e `game_key glen=0`.
  - `shot.png` salvo com X renderizando limpo na fase.

---

## 2026-07-03 s4 — 🏆 X RENDERIZA NA FASE (shader CutOut resolvido) + infra de controle/fullver

**GRANDE AVANÇO:** o X (que aparecia **magenta/bloco amarelo** na fase) agora **renderiza limpo**. Era o shader `Sprites/CutOut` (sprite do X + cenário) caindo no `Hidden/InternalErrorShader`. Commit `01cbfc7`.

- **Causa-raiz (3 bugs no `transpile_shaders.py`)** — o Unity rejeitava shaders PRÓPRIOS do jogo (os que NÃO existem no Terraria enxertado; Default/TMP funcionavam por serem enxerto do Terraria):
  1. Bloco de macros `UNITY_SUPPORTS_UNIFORM_LOCATION` sem preâmbulo HLSLCC virava GLSL inválido (`#define  #define  #else`).
  2. `precision highp float;` cru no fragment → Mali-450 (Utgard) **não tem highp no fragment** → agora emite o bloco guardado (`#ifdef GL_FRAGMENT_PRECISION_HIGH`).
  3. 🔑 **O trailing de channel/binding (`m_ChannelCount`, ~13 bytes com `19000000`) DEPOIS do GLSL era DESCARTADO** no rebuild do subprograma → Unity: `Failed to load GpuProgram from binary shader data`. Agora preservado.
- **Como reproduzir a fase com o X:** receita do §8 abaixo + `MMX_PROFORCE_SCENE=12 MMX_PROFORCE_HIT=80` (pula direto pra fase via `scn_goLoadScene`, il2cpp+0xdecf2c). Confirmado: intro (rodovia/carro/caminhão/READY) renderiza, 0 `Failed to load GpuProgram`.
- **⚠️ data.unity3d NÃO é versionado.** O device tem o data com o CutOut já corrigido. Pra regerar limpo: `python3 transpile_shaders.py <data.unity3d>` (do APK pristino). O transpilador do repo já está corrigido e validado (guard + trailing).

**Controles — infra pronta (commit `f2b00a0`), FALTA validar em gameplay:**
- O pad físico É lido via SDL (`[MMX_GAMEPAD] SDL pad js0: USB Gamepad`). KeyEvent injetado é aceito (ret=1) MAS o jogo **ignora** (é port mobile: lê `RockmanX.controlKey` = TOUCH da tela, não gamepad Android).
- Implementei **pad→touch (estilo MM5/6)**: `MMX_GP_TOUCH=1` converte o pad em toques nas coords dos controles virtuais (D-pad radial 1 dedo + botões dedos separados), multitouch real no `jni_shim` (`g_mt_*`, getX/getY/getPointerId por índice). Emissão verificada (D-pad (250,530), pulo (1170,610)). Coords tunáveis: `MMX_DP_CX/CY/OFF`, `MMX_JX/JY` (pulo), `MMX_SX/SY` (tiro), `MMX_DX/DY` (dash), `MMX_WX/WY` (arma).
- ⛔ **MURO:** o touch injetado retorna **ret=0** (KeyEvent era ret=1) e o Unity só lê o touch em certos estados. E a fase do PROFORCE é uma **DEMO que volta ao menu** — não deu pra segurar gameplay controlável pra validar. **Próximo:** ou confirmar que o touch chega no `controlKey` em gameplay real, OU hookar `controlKey` pra injetar o bitmask direto.

**Versão completa (fullver) — infra pronta (commit `3415699`), "BUY FULL VERSION" AINDA aparece:**
- `MMX_FULLVER=1` agora **varre todos os métodos** e força a TRUE os getters bool de compra: `CStoreKit.getProductUnlock` (FALTAVA!), `getProductUse`, `HasNumberOfPurchases`, etc. (`MMX_STOREDUMP`/`STOREDUMP2` listam a API).
- ⛔ **MURO:** "BUY FULL VERSION" persiste e a fase continua bounce. Determinante NÃO é o getter bool — o menu lê o **objeto `CStoreKit.ProductInfo` (campo `.use`/`.unlock`)** cacheado por `CStoreKit.MakeProductData()` no boot. **Próximo passo (estava começando):** hookar `getProductInfo(id)` ou `MakeProductData` (via `hook_arm64`, tramp `mk_tramp`) e forçar os campos bool do ProductInfo = 1. Ver API no `STORE2` dump (CStoreKit: `getProductInfo/1 -> ProductInfo`, `setProductUse/2`, `MakeProductData/0`).

### 🎯 MISSÕES PRÓXIMA SEÇÃO (prioridade)
1. **Liberar versão completa DE VERDADE** (destrava gameplay estável, que destrava o teste de controle): hookar `CStoreKit.getProductInfo`/`MakeProductData` e forçar `ProductInfo.use=unlock=true`. Objetivo: "BUY FULL VERSION" some + Story Mode entra em gameplay PERSISTENTE (não a demo que volta).
2. **Controles:** com gameplay estável, validar `MMX_GP_TOUCH` (o X anda/pula?). Tunar coords se o D-pad/botões estiverem no lugar errado. Se o touch não chegar no `controlKey`, hookar `controlKey` e injetar o bitmask do pad direto (estilo MM5/6 dpad-bitmask).
3. **Áudio:** FMOD spamma `Cannot create FMOD::Sound instance for clip "114"/"47"`. Investigar o caminho FMOD no so-loader.

---


> Estado em 2026-07-03 (s3). **Boota, renderiza a tela-titulo real do Mega Man X sem magenta, aceita force-start ate o menu/tela "Buy Full Version", e o touch Android ja entra no parser da Unity.** O muro antigo `draws=0` foi superado. O foco atual e contornar o gate de versao completa/menu para chegar em gameplay, depois fechar controles e audio.

## 2026-07-03 s3 — avanço critico salvo

- **Visual/shaders resolvidos:** a tela-titulo aparece limpa. A tela magenta antiga era `Hidden/InternalErrorShader`, principalmente TMP/texto, nao clear color.
- **Payload atual bom:** `payload/assets/assets/bin/Data/data.unity3d`
  - SHA256: `197d3481015ae047e47d424be8a8bb3d73562e6465c0092581166767bdaf0edc`
  - Tamanho: ~155 MiB, UnityPy uncompressed.
  - Base: `runs/2026-07-03-codex-shader-sentinel-data/data.unity3d.ref-ter-overlap`
    SHA256 `8d882d2e49935cf590f3105d6eb0d297e42b5785af35e300778e5ca68c167fe9`.
  - Fix final: `tmp-textcore-alias`, copiando o corpo real do shader Terraria `Hidden/TextCore/Distance Field SSD` para os shaders TMP do MMX em `resources.assets` pathIDs 388-398, preservando nomes.
- **Run visual de referencia:** `runs/2026-07-03-codex-shader-tmp-textcore-alias`
  - Screenshot limpo SHA256 `4c9da24dca2a5a8ada814cafaf50b4eed5c6ea16a60b495f64e4b5644ecffacc`.
  - Magenta pixels cairam para 0.
- **Input Android parcial:** `MMX_AUTOTOUCH=1` injeta `MotionEvent`; com `MMX_TOUCHLOG=1`, Unity chama `getPointerCount`, `getActionMasked`, `getX`, `getY`. Isso prova que o evento entra, mas a checagem propria da tela-titulo nao aceita o toque ainda.
- **Force-start temporario:** `MMX_FORCE_TITLESTART=1` NOPa o `cbz` em `scn_TITLE_run+0x20c` (`libil2cpp+0xdf53d0`) e faz a tela-titulo avancar para `scn_TITLEMENU`. O usuario viu "Buy Full Version" no device; `MMX_RXSPY=scene` confirmou `scn_TITLEMENU_run/draw` rodando.
- **Controle real ainda pendente:** `MMX_KEEP_CONTROLKEY=1` deixa `RockmanX.controlKey` ativo sem crash imediato, mas o touch ainda nao muda a tela-titulo sozinho.
- **Audio ainda pendente:** no menu/tela seguinte o log spamma FMOD `Cannot create FMOD::Sound instance for clip "114"`. Na tela-titulo aparece clip `"47"`.
- **Nao usar PixelCup como referencia:** o usuario confirmou que PixelCup ainda nao roda. A referencia util para shaders continua sendo Terraria.

---

## 0. TL;DR — o que fazer na próxima seção

1. Ler este arquivo + `STUDY.md` + a memória `project_megamanx_mali450`.
2. O muro antigo **`draws=0` foi resolvido**. Nao volte para o diagnostico de BootScene/Addressables como problema principal sem nova evidencia.
3. Proximo passo concreto: resolver a tela/menu **"Buy Full Version"**. Suspeita principal: patches IAP/prefs atuais retornam `false` para algo que deveria indicar produto comprado/full unlock (`CStoreKit.getProductUse`, `getProduct`, prefs criptografadas ou metodo equivalente).
4. Depois de passar do menu: fechar controles reais. `MMX_AUTOTOUCH` entra no parser Android da Unity, mas talvez o jogo use `RockmanX.controlKey`/estado proprio em vez do toque cru.
5. Audio FMOD fica para depois do primeiro gameplay, mas ja esta marcado: clips `47`/`114` falham.

---

## 1. O jogo

- **APK:** `~/Downloads/mega-man-x-androeed.store-0-1730359376.apk`
- **Engine:** Unity **2021.3.39f1 IL2CPP**, metadata **v29**, **arm64-v8a só** (não tem armv7).
- Pipeline **built-in** (SEM URP). Exige **GLES3.0**. Conteúdo 100% local (sem download).
- **SEM pairip** (o LVL/DRM foi crackeado no APK), MAS tem **Google Play Integrity** próprio (classe `IntegrityCheck`) — contornado (ver §4).
- `libil2cpp.so` **exporta a API `il2cpp_*` (236 símbolos)** → dá pra resolver classes/métodos por `dlsym`, sem hardcodar offset. É o que o `mmx_i2sym()` faz.
- Tem japonês (regra #5: forçar inglês) — **ainda pendente**, só lidar quando renderizar.

## 2. Base do port = Terraria

- Port em `nextos_ports_android/ports/megamanx/`. Base = `ports/terraria` (Unity 2021.3.56 IL2CPP, mesma geração).
- Copiei `src/` inteiro (so_util/egl_shim/jni_shim/opensles/pthread_fake/sem_shim/native_pad/gamepad/renderscale/imports.gen.c/etc).
- `main.c` = do Terraria (hooks Cuphead/Terraria ficam getenv-gated, inertes por padrão). Adicionei os blocos `MMX_*` (ver §7).
- `jni_shim.c`: identidade corrigida — pacote `jp.co.capcom.rockmanx` (era `com.and.games505.TerrariaPaid`), paths `megamanx`.
- `boot.config` = do Terraria (`androidUseSwappy=0` + `gfx-disable-mt-rendering=1`).

## 3. Device / build / deploy / debug

- **Device de trabalho:** Mali-450 Amlogic-old, EmuELEC 4.8, **IP `192.168.31.79`**, `root` sem senha.
  - SSH leve (senão o KEX pós-quântico não fecha): `ssh -o KexAlgorithms=curve25519-sha256 root@192.168.31.79`
- **Build:** `bash build.sh` no diretório do port (usa toolchain aarch64 do NextOS-Elite). Sai `megamanx` (ELF aarch64). Os 2 warnings de `libSDL2.so bad subsection length` são benignos (linkagem OK, "BUILD OK" aparece).
- **Deploy binário:** `scp -o KexAlgorithms=curve25519-sha256 megamanx root@192.168.31.79:/storage/roms/megamanx/`
- **Payload do jogo** (assets/lib do APK) fica em `/storage/roms/megamanx/` no device e em `ports/megamanx/payload/` no host (NÃO versionado — ver `.gitignore`).
- **Rodar com segurança (OBRIGATÓRIO):** `ports/megamanx/dbgrun.sh` — `nice -n 19` + `timeout` + watchdog + mata instância antiga por **exe-symlink** (`/proc/*/exe`, NÃO `pgrep -f path` porque o cmdline é relativo `./megamanx`).
  - Uso: `sh /storage/roms/megamanx/dbgrun.sh <SEGUNDOS> VAR1=x VAR2=y ...`
  - 🚨 **NUNCA** rodar o jogo/gdb no device sem `timeout` na PONTA DO DEVICE → busy-wait asfixia o sshd → **perde o device, precisa reboot físico**. Já aconteceu 2×. Ver memória `feedback_debug_device_asfixia_harness`.
- **Log:** o `dbgrun.sh` grava em `/storage/roms/megamanx/debug.log`. `grep` nele por SSH.
- **Framebuffer (ver o que tá na tela):** `dd if=/dev/fb0 of=/storage/roms/megamanx/fbr.raw bs=1M count=4`, `scp` pro host, converter RGBA→PNG (BGRA na verdade: trocar canais B↔R). 1280x720. Magenta uniforme (255,0,255) = clear color = cena vazia (não é error-shader).

## 4. ✅ RESOLVIDO — Play Integrity DRM (`MMX_NOINTEGRITY=1`)

- `BootScene.Start` faz `await IntegrityCheck.Start()`. **`IntegrityCheck.Start()` retorna um `IEnumerator` (coroutine Unity), NÃO um `UniTask`** — o await é via Cysharp **`EnumeratorAsyncExtensions.GetAwaiter[T]`**.
- ⚠️ **ARMADILHA (erro que cometi e corrigi):** patchar `IntegrityCheck::Start → mov x0,xzr; ret` (retornar null) faz `GetAwaiter(null)` lançar **`ArgumentNullException: Value cannot be null`** → boot morre → draws=0. Foi o que travou por horas achando que era "UniTask".
- ✅ **FIX CERTO (no `mmx_nuke_integrity()`, `main.c` ~linha 387):**
  1. `get_IsSuccess → mov w0,#1; ret` (integrity "passou").
  2. DEIXAR `Start()` rodar (cria+retorna o enumerator real, não-null) e patchar o **`MoveNext` do state-machine aninhado `IntegrityCheck/<Start>d__4` → `mov w0,#0; ret`** (MoveNext=false = enumeração terminada na hora, enumerator válido, await completa SEM rodar a chamada Play Integrity que crasha).
  - Achado via `il2cpp_class_get_nested_types` (pega o nested cujo nome contém "Start") + `get_method_from_name("MoveNext")`.
- **Resultado: a `ArgumentNullException` sumiu, a integrity passa limpa.** (log: `[NOINTEGRITY] IntegrityCheck.<Start>d__4::MoveNext -> false`).

## 5. ✅ RESOLVIDO — boot job-system + shaders GLES3→GLES2

- **Boot (job-system):** `MMX_INLINETASK=1 MMX_PATCH=0x34eafc=0x14000005`
  - `MMX_INLINETASK`: trampolim em `libunity+0x350580` (per-object-task loop) chama `mmx_inline_task(obj)` que finge conclusão (`node=*(obj+88); *node=1`) → sai da espera `0x35059c`. Análogo ao Terraria `0x2f37a4`.
  - `MMX_PATCH=0x34eafc`: `WaitForJobGroup` (`b.ge`→`b`) sai imediato. ⚠️ **Isto quebra o load assíncrono** (ver §6). Sem isso → deadlock no frame 2.
- **Shaders:** o jogo só traz shaders GLES3 (platform 9) + Vulkan (18), ZERO GLES2. O Mali-450 é GLES2 puro. Solução: **`transpile_shaders.py`** (roda OFFLINE no host, in-place no `data.unity3d`):
  - Traduz o GLSL HLSLcc `#version 300 es → 100`, `in/out → attribute/varying` (por stage), flatten de UBO (`uniform BLOCK {…}` → uniforms soltos), remove `layout`/`flat`, `texture()→texture2D()`, `out vec4 → gl_FragColor`, adiciona precision.
  - Muda `programType` interno GLES3(2/3/4)→GLES2(5) SEMPRE + **atualiza o campo LENGTH da entrada** (offset,LENGTH,0) + relabela platform 9→5.
  - **Shaders do jogo COMPILAM+LINKAM 0 falhas.** 🔑 Descoberta: o Unity resolve as funções GL via **`dlsym(RTLD_DEFAULT)`, NÃO `eglGetProcAddress`** — o hook `MMX_XLATE` (glShaderSource) mora no `my_dlsym`.
  - Rodar de novo se reextrair o data: `python3 transpile_shaders.py /caminho/data.unity3d` (precisa `UnityPy` + `lz4`).

## 6. ⛔ MURO ATUAL — `draws=0` (cena de boot não carrega)

- Sintoma: roda 60fps estável, 0 exceções agora, mas `[FPS] draws/f=0` sempre. FB = magenta (clear).
- Diagnóstico: `BootScene.Start` passa da integrity mas **parkeia num `await` posterior sem lançar exceção**.
- **Hipótese principal:** esse await é o **load assíncrono de cena/Addressables**, que usa o job-system pra I/O async. Como o `MMX_PATCH` faz `WaitForJobGroup` retornar "pronto" sem os jobs terem rodado, o load nunca completa de verdade → cena nunca ativa → nenhum renderer → draws=0.
- **Coisas já descartadas como blocker:** transpile (data original tbm dava draws=0), input (getDevices=áudio, benigno), workers acordam mas o main trava ANTES de dispatchar.

### Dois caminhos para atacar o muro:

**(A) Descobrir exatamente onde trava (rápido, diagnóstico):**
- Inline-hook no `BootScene.<Start>d__5::MoveNext` = **`il2cpp_base + 0xe3b438`** logando `*(int*)(coro + 0x10)` (o `<>1__state`).
  - Campos da coroutine (do BOOTDUMP): `+0x10 <>1__state:int`, `+0x18 <>t__builder:AsyncVoidMethodBuilder`, `+0x38 <>4__this:BootScene`, `+0x40 <>u__1:UniTask.Awaiter`.
  - Já existe `void mmx_bootmn(void*)` no `main.c` (~linha 258) que loga o state — mas trocar methodPointer NÃO funciona (delegate já cacheado). **Usar `hook_arm64(il2cpp_base+0xe3b438, ...)`** (inline no endereço de código, funciona apesar do cache). Ver exemplo `CUP_CRSPY` linha ~4937.
  - Saber o state onde ele congela → resolver o `bl` target daquele bloco (via `MMX_RESOLVE`, que mapeia offset→`Classe::método`) → é o método que não completa.

**(B) Consertar o job-system de verdade (a correção "certa"):**
- Fazer o Unity rodar os jobs INLINE na própria thread: chamar `JobsUtility.JobWorkerCount = 0`.
- Existe `ter_jobworkers0()` no `main.c` (~linha 446) MAS usa offsets `il2cpp` **HARDCODADOS do Terraria** (`0x73c860` etc) — **ERRADOS pro Mega Man X**. Precisa reescrever pra resolver `il2cpp_domain_get`/`assemblies`/`class_from_name`/`runtime_invoke` via **API exportada** (`mmx_i2sym`, que o `mmx_nuke_integrity` já usa).
- Se `JobWorkerCount=0` funcionar, o `WaitForJobGroup` original (SEM o skip `MMX_PATCH`) deve completar sozinho → tirar o `MMX_PATCH` e ver se o load async anda.

## 7. Ferramentas de debug (env vars, todas getenv-gated no `main.c`)

- `MMX_INLINETASK=1` — trampolim per-object-task (destrava boot). **Essencial.**
- `MMX_PATCH=0xADDR=0xWORD` — escreve uma word de 32 bits no `.text` do libunity. Recipe usa `0x34eafc=0x14000005` (skip WaitForJobGroup).
- `MMX_NOINTEGRITY=1` — bypass Play Integrity (§4). **Essencial.**
- `MMX_PREFSTRUE=1` — `getBoolean` de prefs retorna 1 (aceita termos/EULA salvos).
- `MMX_BOOTDUMP=1` — enumera métodos/campos da coroutine do BootScene + imprime o methodPtr/offset do MoveNext. (foi assim que achei `0xe3b438`).
- `MMX_INTDUMP=1` — enumera métodos/campos da IntegrityCheck.
- `MMX_RESOLVE=1` — mapeia uma lista de offsets (`bl` targets) → `Classe::método`.
- `MMX_NOTERMS=1` — nuka viewTerms/viewParentCtrl/viewError (⚠️ retornam IEnumerator; nukar pra null quebra igual à integrity — provavelmente NÃO usar, ou aplicar a mesma técnica de MoveNext→false).
- `MMX_WORKERPOLL=N` — futexpoll só nas threads não-main (job workers).
- `MMX_XLATE` — hook glShaderSource/Compile/Link (transpile em runtime; não precisa pois o data já foi transpilado offline).
- `CUP_DRAWCOUNT=1` — liga o contador `[FPS] draws/f`.
- Diversos `CUP_*`/`TER_*` herdados do Terraria/Cuphead (inertes por padrão).

## 8. Receita estável atual (para reproduzir o estado)

```
sh /storage/roms/megamanx/dbgrun.sh 45 \
  MMX_INLINETASK=1 MMX_PATCH=0x34eafc=0x14000005 \
  MMX_NOINTEGRITY=1 MMX_PREFSTRUE=1 CUP_DRAWCOUNT=1
```
→ boota, 60fps, integrity limpa, shaders compilam, `draws=0`.

## 9. Regras do projeto (memória) que valem aqui

- Debug SEMPRE via `dbgrun.sh` (nice+timeout+watchdog) — asfixia perde o device.
- Commit direto no `master`, sem co-autor/atribuição Claude, chamar o usuário de "usuário" (nunca nome/email real).
- Nunca forçar `SDL_VIDEODRIVER`/`SDL_AUDIODRIVER` (vêm do sistema).
- Payload do jogo (assets/.so/data.unity3d) e o binário NÃO vão pro git (`.gitignore`).
- Commitar na hora que o usuário der "ok"/"funcionou".

## 10. Arquivos-chave

- `src/main.c` — o so-loader (300K). Blocos MMX_* citados acima com números de linha aproximados.
- `src/jni_shim.c` — JNI + identidade + `getBoolean` (MMX_PREFSTRUE).
- `transpile_shaders.py` — transpilador de shader offline.
- `dbgrun.sh` — harness de debug seguro no device.
- `run.sh` — launcher (recipe embutida).
- `STUDY.md` — a jornada completa documentada.
- Memória: `project_megamanx_mali450.md`, `feedback_debug_device_asfixia_harness.md`.
