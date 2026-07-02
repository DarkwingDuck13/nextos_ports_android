# GTA LCS — ESTUDO DO FLUXO NATIVO COMPLETO (2026-07-02)

**Objetivo:** fluxo inteiro igual ao celular, do boot ao gameplay, SEM flash de gameplay
entre as cutscenes e SEM telas pretas:

```
vídeo das logos (intro.m4v)
  → tela de TAP ("touch to continue", texto vermelho)
  → DISCLAIMER/legal (2 páginas)
  → MENU principal (Start Game → submenu New Game)
  → LOADING com BARRA VERMELHA (igual celular)
  → CUTSCENE 1 (ônibus) completa, câmera acompanhando, sem tela preta
  → CUTSCENE 2 (mansão) completa
  → GAMEPLAY já com a mini-cena scriptada (Toni desce as escadas e pega o carro)
```

**Somente estudo. Nenhum fix aplicado.** Binário do repo: `lcs` md5 `9bcfc54e` (build
2026-06-30 17:19, inclui o commit fdcc38a "carência de 2 finishes").

---

## 1. 🔑 DESCOBERTA CENTRAL DESTE ESTUDO (muda tudo)

**A leitura das sessões s5/s6 estava incompleta/invertida.** Disassembly novo de
`CGame::Process` (@0x33c46c) prova:

```
33c48c  ldrb w8, [feobj, #25]        ; feobj = *(text_base+0x7f9000+704)
33c490  cbz  w8 -> 33c510            ; fe25==1 -> BLOCO ONE-SHOT de pós-restart:
33c494    CTheScripts::StartTestScript
33c498    CTheScripts::Process        ; (1º tick do SCM)
33c4a8    CCamera::Process
33c4b4    CStreaming::LoadScene       ; (+RequestSpecialModel/LoadAllRequestedModels)
33c500    CCamera::Process            ; de novo
33c50c    strb wzr, [feobj, #25]     ; consome fe25
33c510  ... por-frame: CPad::UpdatePads, CCutsceneMgr::Update, CMenuManager::Process ...
33c5fc  bl CTheScripts::Process       ; ✅ SCM roda TODO FRAME, INCONDICIONAL
33c6e4  ldrb w8, [feobj, #25]
33c6e8  ldrb w9, [feobj, #21]
33c6ec  orr  w8, w8, w9
33c6f0  cbnz w8 -> pula               ; ❌ CCamera::Process por-frame SÓ RODA se fe25==0 E fe21==0
33c6fc  bl CCamera::Process
```

E em `GTAGameTick` (@0x524bcc):
```
524c14  ldrb w9, [feobj, #21]
524c2c  cmp  w9, #1 ; b.eq pula      ; fe21==1 -> pula LIB_InputUpdate (input nativo)
```

**Nosso driver força `feobj+21 = 1` TODO FRAME** (jni_shim.c:5980-5984, default, só
`LCS_NOTICK` desliga — hack criado na s2 pra destravar o boot). Consequência direta:

- `CCamera::Process` **NUNCA roda** no jogo inteiro (cutscene E gameplay);
- por isso o flyby da cutscene congelava (`Process_FlyBy` 2×, spline preso em 0.0017);
- por isso a câmera não seguia o player (nasceu o `LCS_CAMFOLLOW`);
- por isso a cutscene nunca "terminava" sozinha (`HasCutsceneFinished` exige spline==1.0);
- e daí nasceu a TORRE DE MULETAS: SPLINEFIX (avançar cam+336 na mão), posfix
  (FinishCutscene forçado a 98.5%), CLEAR_AFTER_FINISH, RESTORE_CAMERA, fade-hacks
  (UNFADE/NODOFADE/GetScreenFadeStatus forçado), carência de 2 finishes (fdcc38a),
  GAMEPLAY_RELEASE_DELAY, post-overlay-block, reconcile...

**Quem mexe no fe21 NATIVAMENTE (disasm, base feobj):**
| Função | Ação |
|---|---|
| `CMenuManager::StartGameFromGate` @0x531f1c | `fe21 = 0` |
| `CMenuManager::LoadGameFromGate` @0x530530 | escreve w21 (provável 0) |
| `Java_..._RockstarJNIlib_StartGame` @0x531fc0 | `DoSettingsBeforeStartingAGame()` → `fe21 = 0` |

Ou seja: **no celular, ao confirmar New Game (gate Rockstar), fe21 vira 0** → câmera e
input nativos passam a rodar todo frame → flyby avança → cutscene termina sozinha → o SCM
(que RODA todo frame) sequencia fade→cut2→fade→mini-cena das escadas, tudo com o timing
e os fades originais. Nenhuma das muletas existe no celular.

**E o nosso port JÁ passa pelo caminho certo:** o menu nativo dispara `ShowGate` (JNI),
nós interceptamos (jni_shim.c:945) e o handler deferido chama `RockstarJNIlib_StartGame`
+ `FinishGate` (jni_shim.c:5375-5382) → o fe21 é zerado NATIVAMENTE ali... **e o nosso
loop re-força =1 no frame seguinte, desfazendo.** A raiz do fluxo quebrado somos nós.

**fe25** = one-shot de pós-restart: setado no FIM de `CGame::InitialiseWhenRestarting`
(@0x33c400-404), consumido 1× por `CGame::Process`. Semântica: "primeiro frame após
restart" (roda 1º tick do SCM + posiciona câmera + LoadScene). Não é gate contínuo.
Todos os experimentos LCS_FE25* eram tentativa de reproduzir na mão o que fe21=0 dá
de graça.

---

## 2. Mapa do fluxo nativo na engine (referência)

Boot state-machine: `OS_ApplicationTick` @0x53ed14 (jump table @0x23f1ca, estado em
`[text_base+0x7fd000+2232]`):

| Estado | O que faz | Situação no port |
|---|---|---|
| 0 | init + LoadSettings + OS_PlaylistBeginInit | OK |
| 1 | espera `HandlePlaylistFinishInit` → RockstarGameLoad | setamos no f=5 (OK, equivale ao Java) |
| 2 | → 3 (precisa feobj+40) | setamos 1× (OK) |
| 3 | **`OS_MoviePlay` = VÍDEO DAS LOGOS** | hook → ffmpeg fbdev+pacat (FUNCIONA) |
| 4 | espera filme + tap-to-skip | OK |
| 5 | LoadAllTextures + `LoadingScreen()` (**barra vermelha do boot**) | verificar se renderiza |
| 6 | LoadingScreen + CheckSlotDataValid → 7 | OK |
| 7 | **MENU** (CMenuManager via GameCoreTick); tap/legal nessa janela | OK com muletas (ver §4) |
| 8 | DoFade → GameStart | OK |
| 9 | **GameCoreTick = jogo** (loading new-game, cutscenes, gameplay) | funciona via muletas |

Sequência do New Game nativa: menu → `ShowGate` → `RockstarJNIlib_StartGame`
(DoSettingsBeforeStartingAGame + **fe21=0**) → `CMenuManager::StartNewGame` →
GameStart (state 8→9) → `CGame::Initialise` → `InitialiseWhenRestarting` (**fe25=1**,
spawn player, reset fade) → `LoadingScreen()` @0x521870 chamada repetidamente durante o
load (**a barra vermelha**; a MESMA do boot) → SCM (`main.scm`) roda todo frame e dirige:
`LOAD_CUTSCENE`→`START_CUTSCENE`→espera `HasCutsceneFinished` (spline==1.0 via
`CCamera::Process`→`Process_FlyBy`)→fade→cut2→fade→mini-cena scriptada (escadas→carro).

---

## 3. Por que hoje há FLASH de gameplay entre as cutscenes (e demora)

Cadeia atual (com as muletas, run30.sh):

1. SPLINEFIX avança `cam+336` pelo relógio; em pos≥0.985 (`FINISH_POS`) chamamos
   `FinishCutscene()` na marra + `CLEAR_AFTER_FINISH` (zera ms_running/processing/…) +
   `RESTORE_CAMERA` (FinishCutscene da câmera + RestoreWithJumpCut → **modo follow-ped
   de GAMEPLAY**).
2. Nessa janela o SCM ainda não startou a cut2 (ele mesmo tem passos de fade/load) →
   a engine está, de fato, em estado visual de GAMEPLAY (câmera modo 4, HUD, mundo).
3. Os fade-hacks (UNFADE/NODOFADE ligados por default no run30) revelavam isso →
   **flash de "gameplay fantasma"** + depois tela preta até a cut2 subir.
4. O commit fdcc38a (1/jul) tapa com carência: `my_GetScreenFadeStatus` devolve FADE_2
   e o fade-quad é desenhado até 2 finishes + `GAMEPLAY_RELEASE_DELAY` (60f). É band-aid:
   esconde o flash, mas mantém timing errado, tela preta entre cutscenes e o corte a 98.5%.
   ⚠️ Conferir se o binário NO DEVICE já é o 9bcfc54e — o flash reportado pode ser
   binário antigo OU janela que a carência não cobre (ex.: 1 frame antes do DoFade).
5. A **demora** entre cutscenes = load da cut2 no SD lento + wedge do streamer
   (force-finish 10s `LCS_STREAMER_FORCE_FINISH_SECS`) + carência de 60 frames +
   finish antecipado que dessincroniza do áudio/SCM.

Com fe21 nativo (=0 no jogo), nada disso deveria existir: o SCM faz o fade-out ANTES do
fim da cut1 e só levanta o fade dentro da cut2 → sem janela visível, como no celular.

---

## 4. Estado etapa-a-etapa vs. alvo nativo

| Etapa | Hoje | Nativo? | Pendência/observação |
|---|---|---|---|
| Vídeo das logos | hook `OS_MoviePlay` → ffmpeg `/dev/fb0` + pacat, skip por botão, drop_caches ao fim | ✅ no ponto certo da engine (player externo, aceitável) | nenhuma |
| Tela de TAP | seguramos `renderedTapToContinue=0`; avanço por A/START via `g_frontend_step`+`my_HasTappedScreen` | 🟡 semi-nativo | **texto vermelho "tap to continue" não renderiza** (arte segura, texto não; suspeita CFont não inicializada nessa fase — renderer embutido 0x9ec7a9-b4 sem símbolo) |
| Disclaimer 2 páginas | pág1 segura via `legalScreenState=0`; pág2 **pisca** (state da pág2 não isolado) | 🟡 | mapear os valores de `legalScreenState`/`legalScreenTimer` da pág2 (LCS_LEGAL_DIAG) e segurar cada página; no celular as 2 páginas passam com timer próprio |
| Menu / Start Game / submenu New Game | navegação nativa + MENU_PULSE + FPS_CAP 30 | ✅ validado ("navego perfeito") | nenhuma |
| Gate Rockstar (New Game) | ShowGate interceptado → `RockstarJNIlib_StartGame`+`FinishGate` deferidos | ✅ **já zera fe21 nativamente** | o loop re-força fe21=1 e desfaz (raiz §1) |
| Loading BARRA VERMELHA pós-New Game | **PRETO**: `my_RenderMenus` PULA RenderMenus em state 9 até os 2 finishes (skip criado na s2 por crash `CSprite2d::Draw`) | ❌ | o crash antigo provavelmente era consequência do estado inconsistente da época (fluxo forçado por feobj+25 cru); retestar SEM o skip no fluxo nativo; se crashar, pegar bt e corrigir a causa (ordem de init de sprite/fonte) |
| Cutscene 1 (ônibus) | toca via SPLINEFIX; finish forçado a 98.5% | ❌ muleta | alvo: fe21=0 → flyby nativo → finish natural em 1.0 |
| Transição cut1→cut2 | clear+restore camera+fade forçado (carência) → flash/preto/demora | ❌ | alvo: SCM dirige com fades nativos |
| Cutscene 2 (mansão) | idem cut1; wedge de streaming não-determinístico | ❌ + wedge | wedge é problema SEPARADO (RAM/streamer) — manter `my_lglHasStreamerFinishedTasks` force-finish como rede de segurança |
| Mini-cena gameplay (escadas→carro) | entra em gameplay, mas sem a suavidade scriptada | 🟡 | com SCM+câmera nativos deve sair de graça (SCM roda todo frame — confirmado no disasm) |

Muletas hoje ativas por default (run30.sh) que o caminho nativo deve aposentar:
`LCS_CUTSCENE_SPLINEFIX=1`, `FINISH_POS=0.985`, `CLEAR_AFTER_FINISH=1`,
`RESTORE_CAMERA=1`, `POST_RECONCILE=1`, `GAMEPLAY_RELEASE_DELAY=60`,
`UNFADE=1`+`NODOFADE=1` (via NO_FADEHACK=0), carência de 2 finishes (código),
skip de RenderMenus em s9, `LCS_CAMFOLLOW` (não default), LCS_FE25* (não default).

O que deve FICAR (independe do fluxo): fix ALPHA do compositor (s11), LightsMult pin,
detail-texture off, FPS_CAP 30, MENU_PULSE, controles (FIX_CONTROLS), WAD/loader/EGL,
streamer force-finish (rede de segurança), INTRO_FREE_RAM, RAMGUARD.

---

## 5. Riscos e incógnitas do caminho nativo (mapear ANTES de mexer)

1. **Quem seta fe21=1 no boot?** Não achei writer de 1 no disasm (só clears). Hipóteses:
   default do objeto na construção, ou escrito por registrador (LoadGameFromGate escreve
   w21). O hack nosso nasceu porque o boot travava sem fe21=1 — ou seja, no Android algo
   do lifecycle Java deixa fe21=1 até o gate. ⇒ A forma segura NÃO é "parar de forçar
   sempre", é **espelhar o ciclo**: forçar fe21=1 SÓ até o gate disparar (state <9, ou
   até o `rkgate fire`), e daí em diante NUNCA mais escrever.
2. `OS_ApplicationTick` state 8 checa fe21 (53f1a8: `cbz fe21 → 53f26c`) — entender esse
   branch antes (se fe21=0 cedo demais, o 8→9 pode desviar). O gate dispara DENTRO do
   fluxo do menu, então a ordem natural (fe21=0 só após StartGame) deve satisfazer.
3. Com fe21=0, `LIB_InputUpdate` volta a rodar (GTAGameTick) → o input touch nativo
   re-ativa. Pode conflitar com nosso pad-bridge (duplicação de input?). Observar
   `[input]`/CPad no diag. (No celular é exatamente esse caminho que roda.)
4. O crash antigo de SCM (`StartNewScript` NULL, s5) era por forçar fe25 DURANTE o load.
   No fluxo natural fe25 é setado pelo próprio `InitialiseWhenRestarting` no timing certo
   — não deve ocorrer. Validar mesmo assim (crash handler já loga bt).
5. `my_GameCoreTick` limpa fe21 pós-tick (restartguard) — vira redundante/no-op no
   caminho novo; revisar junto. `my_HasTappedScreen` já devolve valor real em state 9 (OK).
6. Wedge do streamer é ortogonal: cutscene nativa não o cura; manter force-finish 10s.

---

## 6. Plano de validação sugerido (quando for aplicar — em ordem, tudo gated)

- **E0 (sanidade):** md5 do `lcs` no device vs repo (9bcfc54e). Retestar o flash com o
  binário atual antes de qualquer mudança (o fdcc38a pode já ter tapado o sintoma).
- **E1 (diag puro, sem mudança de comportamento):** logar por frame `fe21`, `fe25`,
  gstate, cmode, spline e o momento do `rkgate fire` num run New Game completo.
  Confirmar: fe21 zera no gate e nós re-forçamos.
- **E2 (o experimento-chave):** `LCS_FE21_NATURAL=1` → o loop força fe21=1 apenas
  enquanto `state < 9` E o rkgate ainda não disparou; depois disso NÃO escreve mais.
  Manter TODAS as muletas ligadas ainda. Observar no diag: `CCamera::Process` rodando
  (cmode 17 na cutscene), spline avançando SEM splinefix escrever (write=0 no log).
- **E3:** com E2 OK, desligar em bloco num perfil "nativo": SPLINEFIX/FINISH_POS/
  CLEAR_AFTER_FINISH/RESTORE_CAMERA/carência/fade-hacks (NO_FADEHACK=1)/RELEASE_DELAY.
  A/B contra o perfil atual: cut1 termina em 1.0 sozinha? fades naturais? sem flash?
  mini-cena das escadas roda? timing igual celular?
- **E4:** loading vermelho: remover o skip do RenderMenus em s9 no perfil nativo;
  se crashar, bt → corrigir causa raiz (init de CSprite2d/CFont), não re-esconder.
- **E5:** tap/disclaimer: com fluxo+timing certos, re-testar soltar os holds
  (⚠️ lembrar da trava branca: FORCE_TAPLEGAL=0 + TAP_NATURAL=1 documentada) e isolar
  o state da pág2 da legal; investigar o texto vermelho (CFont init na fase frontend).
- **E6 (de-flag, regra do porter):** o que validar → default no código, sem flag, e
  atualizar run30/run-playable/run-final JUNTOS (drift de launcher já causou regressão).

Critério de aceite final = o fluxo do §Objetivo, olhado NA TV (não em glReadPixels —
lição do alpha s11), sem flash, sem tela preta fora dos fades nativos, com barra
vermelha nos dois loadings e as 2 cutscenes inteiras com câmera viva.

---

## 7. Referência rápida (símbolos/offsets deste estudo)

- `feobj` = `*(text_base + 0x7f9000 + 704)`; `fe21`=+21 (gate câmera/input; zerado pelo
  gate Rockstar), `fe25`=+25 (one-shot pós-restart), `fe40`=+40 (state2→3).
- `CGame::Process` @0x33c46c — bloco one-shot fe25 @33c490-33c50c; SCM por-frame
  @33c5fc; **câmera por-frame @33c6fc gated por `fe25|fe21` @33c6e4-f0**.
- `GTAGameTick` @0x524bcc — fe21==1 pula `LIB_InputUpdate` @524c4c.
- `OS_ApplicationTick` @0x53ed14 — state8 usa fe25/fe21 @53f1a0-ac.
- Clears de fe21: `StartGameFromGate` @0x531f1c, `LoadGameFromGate` @0x530530,
  `RockstarJNIlib_StartGame` @0x531fc0.
- Força fe21=1 todo frame: jni_shim.c:5980 (`LCS_NOTICK` gate). Limpa pós-tick:
  `my_GameCoreTick` jni_shim.c:4274. ShowGate intercept: jni_shim.c:945 + handler 5347.
- Fade gate: `my_GetScreenFadeStatus` 4575, `my_DoFade` 3925, carência
  `lcs_fade_required_finishes` 1526 (fdcc38a). RenderMenus skip s9: 3855.
- Cutscene: tick SPLINEFIX `lcs_cutscene_tick_after_draw` 2459 (restaurado do s6),
  finish forçado `lcs_cutscene_finish_from_clock` 2375, restore camera 2070.
- Disasm completo em scratchpad da sessão (`libgame.asm`, 1.37M linhas) — regenerável:
  `aarch64-linux-gnu-objdump -d libGame.so`.
