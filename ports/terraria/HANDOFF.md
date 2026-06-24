# HANDOFF — Terraria (Unity 2021.3.56f2 IL2CPP) → Mali-450 so-loader

## 🟢 2026-06-17 NOITE — ESTADO VALIDADO: CONTROLES + AUDIO + PLAYER/MUNDO + SEM TELA PRETA
**Regra operacional do projeto:** antes de copiar binário novo ou lançar build novo, sempre fechar/matar o Terraria no device. Se o jogo estiver criando mundo, **não matar**: aguardar terminar.

Estado validado pelo porter:
- Controles estão bons e devem ser preservados.
- Áudio está bom: FMOD roda em 24000 Hz e `TER_STREAMFALLBACK=1` mantém música/SFX.
- Player foi criado pelo fluxo nativo do Terraria: `/storage/roms/terraria/Players/Player.plr`.
- Mundo foi criado e salvo pelo jogo: `/storage/roms/terraria/Worlds/Cama_da_Postura_de_Decepção.wld`.
- Tela preta alguns segundos após entrar no mundo foi corrigida neutralizando `Terraria.Graphics.Renderers.LittleFlyingCritterParticle.Update` com `TER_FIXNANPART=1`.

Estado de controles que NÃO deve ser perdido:
- `run.sh`: `TER_GAMEPAD=1 TER_CTRL=1 TER_GPAD=1 TER_NAVMENU=1 TER_FIXSP=1 TER_NOVKBD=1`.
- `TER_RSCURSOR` fica fora do default. Não reativar: causou cursor duplicado/quebra de gameplay.
- `TER_OSK` fica fora do default. Não reativar teclado virtual: quebrou navegação/estado de menu.
- Menu: `TER_NAVMENU` dirige hover/click por regiões reais (`GUIInputRegionManager`), D-pad/stick esquerdo navega, `A` confirma, `B` volta, `LB/RB` mantêm abas nativas.
- Gameplay: stick direito move o cursor nativo via `GamePad.GetState`; o menu não deve forçar cursor quando `Main.gameMenu=false`.

Estado de criação de nome:
- `run.sh` usa `TER_AUTONAME=1 TER_PLAYER_DEFAULT=Player TER_WORLD_DEFAULT=World`.
- Autonome preenche campos nativos e `Main.PendingPlayer.name`; não abre teclado e não deve mexer nos controles.

Estado de boot/render:
- `run.sh` usa `CUP_GCOFF=1 TER_INLINETASK=1 TER_SKIPJOBWAIT=1 TER_NUKEKB=1 TER_FIXNANPART=1 CUP_NOLOGFILE=1`.
- `TER_SKIPJOBWAIT` ficou necessário para boot/menu. A criação de mundo pode demorar vários minutos; não considerar travado só porque `Worlds/` ainda está vazio.

## 🟡 2026-06-17 NOITE — SAVES GERADOS INVALIDADOS; VOLTAR AO CREATE NATIVO COM AUTONOME
**Regra operacional do projeto:** antes de copiar binário novo ou lançar build novo, sempre fechar/matar o Terraria no device.

Estado atual:
- O porter não mexeu nos controles; tratar o bug recente como save/estado inválido, não como regressão voluntária do usuário.
- Os saves `Player.plr`, `Player_2.plr` e `Player_3.plr` gerados no device foram invalidados: ao carregar gameplay, houve tela preta/estado ruim e exceção Unity em mundo.
- `ports/terraria/default_players/` deve ficar sem `.plr` default até existir um save realmente validado.
- O caminho atual é criar personagem do zero pelo menu original do Terraria.
- `run.sh` liga `TER_AUTONAME=1 TER_PLAYER_DEFAULT=Player TER_WORLD_DEFAULT=World` e mantém `TER_NOVKBD=1`, sem `TER_OSK`.
- O autonome só preenche campos internos de nome e `Main.PendingPlayer.name`; ele não deve abrir teclado virtual nem mudar navegação/controle.

Detalhe importante do gerador em `src/main.c`:
- Flag manual: `TER_MAKECLEANPLAYER="Player,Player 2,Player 3"`.
- Ele roda só quando a flag está ligada e não participa da navegação normal. Não usar como default.
- Ele cria `GUIPlayerCreateMenu`, roda `Setup()`, seta o nome em `Main.PendingPlayer.name` e chama o `CreateAndSave()` original do menu.
- Não voltar ao caminho antigo de salvar `Main.LocalPlayer`: isso clona o player ativo com desbloqueios/progresso e trava ao carregar.
- Não recolocar chamada do gerador em `ter_ctrl_feed`: isso roda no caminho de input/render e já causou crash cedo ao chamar `il2cpp_string_new`.

## 🟢 ESTADO FINAL VALIDADO 2026-06-17 — CONTROLES + PLAYER SAVE FUNCIONANDO
**Regra operacional do projeto:** antes de copiar binário novo ou lançar build novo, sempre fechar/matar o Terraria no device.

Estado validado depois do problema do teclado:
- A solução prática do nome/personagem é **não usar teclado**.
- Existe um player pronto em `ports/terraria/default_players/Spedyleonik.plr`.
- `run.sh` cria `Players/` e garante que cada `.plr` de `default_players/` exista lá, sem sobrescrever save existente.
- Para deixar 2 ou 3 opções prontas, basta adicionar mais `.plr` validado em `ports/terraria/default_players/`; o launcher copia todos.
- No device, o save está instalado em `/storage/roms/terraria/Players/Spedyleonik.plr`.
- Confirmado em teste real: **"deu certo"**.
- `run.sh` default fica sem `TER_OSK`; manter `TER_NOVKBD=1`.
- A navegação horizontal no menu foi corrigida: em linhas com vários botões (`Voltar | Aleatório | Criar`), esquerda/direita agora troca coluna.

Não reabrir como default:
- Não ligar `TER_OSK` no launcher.
- Não depender do mini teclado para criar personagem.
- Não remover o player padrão sem outro `.plr` validado.

## 🟢 ESTADO FINAL VALIDADO 2026-06-17 — CONTROLES FUNCIONANDO, TECLADO DESLIGADO
**Regra operacional do projeto:** antes de copiar binário novo ou lançar build novo, sempre fechar/matar o Terraria no device.

Este é o estado validado com `funciouuu` e que não pode ser perdido.

Estado correto:
- `run.sh` default usa `TER_GAMEPAD=1 TER_CTRL=1 TER_GPAD=1 TER_NAVMENU=1 TER_FIXSP=1 TER_NOVKBD=1`.
- `TER_NUKEKB=1` fica ligado no boot para tirar o caminho de teclado do Unity da jogada.
- `TER_RSCURSOR` fica **fora** do default. Não reativar: causou cursor duplicado/quebra de gameplay.
- O menu funciona por `TER_NAVMENU`: D-pad/stick esquerdo navega, `A` confirma.
- O teclado virtual fica desligado (`TER_NOVKBD=1`). A tentativa de teclado quebrou o estado bom dos controles.
- O teste que provou navegação usou temporariamente `TER_GPVIRT=1 TER_CTRLLOG=1 TER_NAVDUMP=1 TER_MENULOG=1`; depois foi relançado limpo.
- Relançamento limpo feito com PID `145895`, sem flags de diagnóstico.

Não mudar sem teste completo:
- Não trocar `TER_NUKEKB` por `TER_KBFIX` no default.
- Não remover `TER_NAVMENU` do default.
- Não ligar `TER_RSCURSOR` no default.
- Não voltar o mini teclado para o default.

## ⚠️ TENTATIVA ANTERIOR COM MINI TECLADO — NÃO USAR COMO DEFAULT
**Regra operacional do projeto:** antes de copiar binário novo ou lançar build novo, sempre fechar/matar o Terraria no device.

Estado atual importante:
- `run.sh` padrão mantém **cursor nativo do Terraria**. Não reativar `TER_RSCURSOR` por padrão.
- Default atual: `TER_GAMEPAD=1 TER_CTRL=1 TER_GPAD=1 TER_FIXSP=1`.
- O caminho de cursor custom/overlay (`TER_RSCURSOR`) continua no código apenas como diagnóstico/opt-in; ele causou cursor duplicado no gameplay e não deve ser usado como base.
- `TER_NUKEKB` continua fora do `run.sh`. O input real de teclado precisa de `KeyboardInput.Update` vivo.
- Correção JNI nova: `TER_KBFIX=1` agora resolve `UnityPlayer.currentActivity.PressedStates` por reflection/JNI:
  - `GetStaticFieldID(currentActivity)` retorna uma sentinela estável.
  - `CallObjectMethodA(getDeclaringClass)` no fluxo armado devolve uma classe fake válida.
  - `GetFieldID(PressedStates, "")` retorna o fieldID fake e `GetObjectField(PressedStates)` devolve `boolean[512]`.
  - Log validado no device: `[KBFIX] GetObjectField(PressedStates) -> boolean[512]`.
  - Depois disso, a exceção `Field PressedStates or type signature not found` parou no boot.

Bug novo atacado:
- Ao criar personagem novo, Terraria/Unity chama `showSoftInput(...)`, mas no so-loader não existe teclado Android real.
- O log provou o caminho:
  - `GetMethodID(showSoftInput, (Ljava/lang/String;IZZZZLjava/lang/String;IZZ)V)`
  - `CallVoidMethod(showSoftInput)`
  - Unity registra os nativos `nativeSetInputString`, `nativeSetInputSelection`, `nativeSetKeyboardIsVisible`, `nativeSoftInputClosed`.

Correção implementada:
- `jni_shim.c` agora intercepta `showSoftInput`/`hideSoftInput` e mantém um estado de soft keyboard ativo.
- `main.c` desenha um mini teclado controlado pelo controle quando `showSoftInput` abre.
- Controles do mini teclado:
  - D-pad/stick esquerdo: escolher letra.
  - `A`: inserir letra; se estiver na tecla `OK`, confirma.
  - `X` ou `B`: apagar.
  - `Y`: espaço.
  - `Start`, `RB`, `RT` ou `R3`: finalizar; se vazio, usa `TER_VK_DEFAULT` ou `PLAYER`.
  - `Select`: cancelar.
- Ajuste posterior: validamos que o teclado ficou bom, mas `Start` não confirmava no controle. A última tecla da última linha agora é `OK`, confirmável com `A`; `RB/RT/R3` também confirmam como alternativas ao `Start`.
- Ajuste posterior 2: ao clicar `OK`, o Unity fechava e imediatamente chamava `showSoftInput` de novo com `text=""`; isso apagava o nome e reabria o teclado. Correção:
  - `jni_shim.c` preserva o último texto confirmado e suprime até 3 reopens vazios imediatos (`[SOFTINPUT] suppress empty reopen -> keep ...`).
  - `main.c` engole o gamepad por alguns frames depois do `OK`, para o `A` não clicar novamente no campo por trás.
- Enquanto o teclado está ativo, `GamePad.GetState`/InControl são zerados para o jogo, evitando que A/D-pad mexam no menu por trás.
- Opt-out: `TER_NOVKBD=1`.

Build/deploy feito:
- `./build.sh` OK.
- Antes de copiar, processo antigo foi morto e não havia PID restante.
- Copiado `terraria` e `run.sh` para `<device-ip>:/storage/roms/terraria/`.
- Lançado build inicial do teclado via `sh run.sh`, PID `123936`.
- Depois do ajuste de confirmação (`OK` na grade + `RB/RT/R3`), build/deploy OK e novo PID `125132`.
- Depois do ajuste de reopen vazio/pós-OK, build/deploy OK e novo PID `126308`.
- Log do novo boot confirmou os nativos:
  - `nativeSetKeyboardIsVisible`
  - `nativeSetInputString`
  - `nativeSetInputSelection`
  - `nativeSoftInputClosed`
- Build/deploy posterior do `TER_KBFIX` OK, PID `136646`; processo vivo, sem exceções novas no boot.

Próximo teste:
- No device, entrar em criar personagem e validar se o overlay `[VKBD]` aparece na TV.
- Validar se, agora com `KeyboardInput.Update` sem exceção, o nome digitado aparece/grava na tela do Terraria.
- Se letras aparecem no overlay mas o jogo não habilita o botão de continuar, revisar a ordem dos callbacks `nativeSetInputString`/`nativeSetInputSelection`/`nativeSoftInputClosed` ou alimentar diretamente o campo IL2CPP de nome.

## 🟢 SESSÃO 2026-06-17 (continuação 2) — stick direito move cursor; bug real era cursor invisível + A sem clique
**Regra operacional do projeto:** antes de copiar binário novo ou lançar build novo, sempre fechar/matar o Terraria no device.

Atualização importante descoberta pelo porter:
- Existem **dois cursores**.
- No gameplay, o cursor nativo do Terraria aparece; desenhar nosso overlay ali duplica o cursor.
- No menu inicial, o cursor nativo/principal fica **invisível**, e o overlay que desenhamos precisa seguir esse cursor interno.
- Correção aplicada depois disso: `TER_RSCURSOR` agora alimenta também `UnityEngine.Input.mousePosition` e `GUIInputRegionManager.SetMousePosition`, convertendo da tela real `1280x720` para o espaço lógico do jogo (`screenWidth/screenHeight`, visto no log como `902x507`).
- O overlay (`ter_rscursor_draw`) agora só desenha no menu (`Main.gameMenu=true`), salvo se `TER_RSCURSOR_DRAW_INGAME=1`.
- Log esperado no build atual: `[RCURSINK] real=640,360 game=451,254/902x507 ... girm=451,254 ovr=1 menu=1`.
- Ajuste posterior: `Main.gameMenu`/`Main.myPlayer` não resolvem neste build (`nil` no log). Para remover o cursor duplicado no gameplay, o gate agora usa `Main.player[]` + offset de `Terraria.Player.active` e desliga o overlay quando **qualquer Player ativo** existir. Log no menu ainda deve mostrar `playerActive=0`; no gameplay deve virar `playerActive=1`.

Diagnóstico ao vivo do porter:
- O analógico direito **funcionava**: por duas vezes ele conseguiu passar por cima de opções.
- O problema real era que o cursor estava **invisível**.
- Ao acertar uma opção "por sorte", o botão A também **não confirmava/iniciava**, porque o clique do mouse virtual estava desligado no caminho `TER_RSCURSOR`.

Correção aplicada em `ports/terraria/src/main.c`:
- `ter_rscursor_draw()` desenha uma mira visível no framebuffer real, depois de `rs_present()` e antes de `eglSwapBuffers`.
- O desenho usa `glScissor + glClear`, sem shader, para ficar simples e independente do pipeline do Terraria.
- `ter_fna_mouse_getstate()` agora faz A/B virarem botões de mouse por padrão quando `TER_RSCURSOR=1`.
  - Opt-out: `TER_RSCURSOR_NOCLICK=1`.
  - Compat antigo: `TER_RSCURSOR_CLICK=1` ainda funciona.

Build/deploy/teste feito:
- `./build.sh` OK.
- Antes de copiar: `pkill -9 terraria` no device e `pgrep` confirmou que não havia processo vivo.
- Copiado `terraria` novo para `<device-ip>:/storage/roms/terraria/terraria`.
- Lançado com `TER_GPAXLOG=1 TER_RSCURSORLOG=1 sh run.sh`, PID `118979`.
- Log confirmou:
  - `[TGP-HOOK] FNA Mouse.GetState ... hookado`
  - `[RCURDRAW] cursor overlay ON size=14 thick=3 screen=1280x720`
  - `[RCUR] 640,360 ... bounds=1280x720`

Próximo passo: validar na TV se a mira aparece e se A confirma/inicia. Se hover aparecer mas A ainda não acionar, investigar se o menu em questão lê outro caminho além de FNA `Mouse.GetState`.

## 🟢 SESSÃO 2026-06-17 (continuação) — CONTROLE agora usa SDL_GameController/Xbox, não js0 cru
**Regra operacional do projeto:** antes de copiar binário novo ou lançar build novo, sempre fechar/matar o Terraria no device.

O caminho de controle foi refeito para parar de chutar layout de `/dev/input/js0`.
- `ter_gamepad_poll` agora usa **SDL_GameController** como fonte física única. O SDL normaliza para layout **Xbox**.
- `run.sh` padrão agora liga `TER_GAMEPAD=1 TER_CTRL=1 TER_GPAD=1 TER_RSCURSOR=1 TER_FIXSP=1`.
- `TER_NAVMENU` saiu do `run.sh` padrão. Ou seja: sem hover/cursor artificial por regiões de menu no caminho normal.
- `TER_GAMEPAD` agora só faz poll do estado Xbox. O antigo hook FNA Keyboard/Mouse ficou separado em `TER_FNAINPUT` apenas para diagnóstico.
- `TER_CTRL` alimenta InControl (`Controller.ControllerDevice.GetKeyRaw/GetAxisRaw`).
- `TER_GPAD` alimenta XNA `Microsoft.Xna.Framework.Input.GamePad.GetState`.

Mapeamento entregue ao jogo:
- D-pad: `SDL_CONTROLLER_BUTTON_DPAD_*` -> InControl DPadX/Y e XNA DPad.
- A/B/X/Y: botões Xbox padrão -> Action1/2/3/4 e XNA flags.
- LB/RB: shoulders.
- LT/RT: eixos analógicos reais + botão quando > 0.30.
- L3/R3: stick buttons.
- Sticks: LeftX/LeftY/RightX/RightY reais, com deadzone `TER_GP_DEADZONE` default `0.18`.

Build/deploy/teste feito:
- `./build.sh` OK.
- Copiado `terraria` e `run.sh` para `<device-ip>:/storage/roms/terraria/`.
- Lançado com `TER_GPAXLOG=1 sh run.sh`, PID visto: `117505`.
- Log confirmou:
  - `[TGP] SDL_NumJoysticks=1`
  - `[TGP] js0: "USB Gamepad" isGameController=1`
  - `[TGP] Xbox layout via SDL_GameController js0: Twin USB PS2 Adapter`
  - `[TGPAX]` mostrou D-pad, A/B/X/Y, left stick e right stick variando.

Próximo passo: validar no menu e gameplay se o controle agora se comporta como Xbox real. Se algo ainda não bater, ajustar a camada que o Terraria espera (InControl/XNA), não voltar para mapeamento manual de eixo cru.

## 🔴 SESSÃO 2026-06-17 (madrugada) — ÁUDIO RESOLVIDO, mas CONTROLE REGREDIU (LEIA PRIMEIRO)
**Device `<device-ip>` (ssh root/`nextos`), jogo `/storage/roms/terraria/`. `sh run.sh`. Commit atual `a0f1f99`.**
**Build/deploy: `cd ~/nextos_ports_android/ports/terraria; ./build.sh; ssh ...kill; scp terraria .89`.**

### 🟢 ÁUDIO — RESOLVIDO DE VERDADE (confirmado de ouvido). NÃO MEXER, só manter.
Eram DOIS problemas, ambos resolvidos:
1. **Acelerado** (1.84×): o FMOD mixa a **24000 Hz** (não 44100). Causa achada por `fmodGetInfo(env,thiz,info)@libunity+0x8112b0` chamado direto: **info 0=samplerate(24000), 1=blockSize(1024 frames), 4=channels(2)**. O SDL abria a 44100 → 44100/24000=1.84×. FIX: abrir o SDL na taxa/canais REAIS do fmodGetInfo. Também medi empiricamente (sentinela 0xAB no buffer) que fmodProcess escreve 4096 B/chamada — bate com 1024 frames stereo. (A teoria velha de "capacidade do DirectByteBuffer" estava ERRADA.) ⚠️ o offset da struct do System `*0xc7c2f0`/`+0x60` é NÃO-CONFIÁVEL (fmod_read_format sempre falhava) — use `fmodGetInfo`.
2. **Engasgando**: back-pressure curto. FIX: `bp=6` blocos default + pré-enche a fila antes de despausar. Tunável `TER_AUDIO_BP`/`TER_AUDIO_RATE`/`TER_AUDIO_CH`/`TER_AUDIO_FRAMES`.
   **Código do áudio a MANTER:** `fmod_audio_thread` (main.c, ~3984) + `g_fmod_cap` (jni_shim.c, =32768 folga).

### 🔴 CONTROLE — EU QUEBREI NESTA SESSÃO. RECUPERAÇÃO = RESTAURAR de `ff34d71`.
**FATO: no commit `ff34d71` (início desta sessão) o controle FUNCIONAVA** (menu + gameplay + cursor;
tinha sido validado no Tutorial na sessão anterior). **As minhas mudanças de controle desta sessão
regrediram.** Sintoma final (gameplay): cursor **volta sempre pro mesmo ponto**, stick direito **só anda
na horizontal (não sobe/desce)**, e o **D-pad move o cursor**. "Antes de dormir tudo funcionava."

**🎯 PLANO DE RECUPERAÇÃO (fazer primeiro, é mecânico e seguro):**
RESTAURAR o código de CONTROLE para a versão `ff34d71`, **mantendo** o áudio. As funções a restaurar
(copiar de `ff34d71` por cima das atuais em `src/main.c`), SEM tocar em `fmod_audio_thread`:
- `ter_gamepad_poll` (eu mexi: leitura de gatilhos/L3/R3, `SP` do cursor 1/110→1/240 tunável, gatei o
  fallback do stick-ESQUERDO no cursor atrás de `TER_GP_CURLEFT`).
- `ter_ctrl_feed` (eu adicionei `g_inj_btn[6,7,10,11]` LTrig/RTrig/StickL/R e `g_inj_axis[6,7]`).
- `ter_menu_nav` (eu adicionei: cheque `/tmp/ternonav`, gate `gameMenu`, e um "MODO CURSOR LIVRE" no
  stick direito que eu **removi** mas pode ter deixado resíduo; confira contra `ff34d71`).
- Globais que adicionei: `g_gp_log[12]→[16]` + `g_lt_analog/g_rt_analog` + `g_fcmode/g_fcx/g_fcy` +
  campo `MM.fgameMenu`. (Pode deixar declarados, só não devem ALTERAR o comportamento de `ff34d71`.)
Comando p/ ver a versão boa: `git show ff34d71:ports/terraria/src/main.c`. Depois rebuild+deploy e
confirmar que o controle voltou ao normal. SÓ DEPOIS pensar em adicionar Xbox-completo/idioma.

**Por que provavelmente quebrou (hipóteses p/ a próxima sessão, NÃO confirmadas):**
- O cursor "volta pro mesmo ponto" + "D-pad move o cursor" no GAMEPLAY = o `ter_menu_nav` está
  rodando NO JOGO (não só no menu) e forçando `g_cursor` pra um item do HUD + movendo por D-pad. Tentei
  gatear com `gameMenu` (`MM.fgameMenu && !ter_getb(...)`) mas o porter disse "nada mudou" → provável que
  **`gameMenu` NÃO resolveu** (campo NULL → gate vira no-op) OU o nome do campo está errado. Conferir se
  `Terraria.Main.gameMenu` resolve (logar) OU usar outro sinal de "está no jogo" (ex.: `Main.menuMode`
  == -1? netMode? a presença de um `Player` ativo?). No `ff34d71` o ter_menu_nav NÃO atrapalhava o
  gameplay — descobrir POR QUÊ (talvez no ff34d71 o gameplay não tinha regiões GIRM navegáveis e algo
  que mudei fez passar a ter; ou o cursor do gameplay no ff34d71 vinha de outro caminho).
- "Stick direito só horizontal" = **layout de eixos do controle do porter ≠ do meu chute** (assumi
  RX=eixo3, RY=eixo4; o Y dele deve estar noutro eixo). PRECISA logar os eixos reais: lançar com
  `TER_GPAXLOG=1` (já existe; loga `[TGPAX] ax0..ax5 cur`), mexer o stick direito
  cima/baixo/esq/dir, e LER o log p/ achar os eixos certos. (Há `axlog.sh` no device pra isso.)
  Ajustar `TER_GP_RX`/`TER_GP_RY` (e LX/LY) ao controle dele. **NÃO chutar — medir.**

### 🟡 IDIOMA / dropdowns do Settings (descoberto, NÃO entregue — fazer só depois do controle voltar)
- Abas do Settings trocam com **LB/RB (shoulder)**. Up/down navega itens. ✅ (no ff34d71 já anda).
- O **dropdown de Idioma** (e Autopause/etc.) NÃO está nas regiões do GUIInputRegionManager (fica num
  overlay à direita, ~x556 no espaço UI 902×507) e **NÃO responde a up/down nem a LT/RT** (nem
  ControllerDevice nem XNA GamePad). **SÓ responde à POSIÇÃO do cursor (Mouse) + clique (A).** PROVA:
  posicionei o cursor em (556,284) e cliquei → selecionou "Português brasileiro" e a UI virou PT-BR.
  → A solução precisa de um jeito de **mover o cursor livre no MENU** (sem quebrar o gameplay!).
  Minha tentativa (free-cursor no stick direito) QUEBROU o gameplay (por isso foi removida). A próxima
  tentativa tem que ser **só-no-menu** (gate de `gameMenu` FUNCIONANDO) ou via um botão dedicado.
- ⚠️ A língua do save no device pode estar em **PT-BR** (mudei durante o teste). Se quiser EN, reabrir
  Settings→Idioma (ou apagar o ajuste no PersistentUserData).

### 🧪 Infra de teste criada (gated, só liga com env; não afeta produção)
`TER_GPVIRT=1`+`/tmp/tergp` (tokens up/down/a/b/.../`rs:DX:DY`/`cur:X:Y`), `TER_SHOTLIVE=1`+`touch
/tmp/tershot`→`shot.ppm` (1280×720, ver via `scp`+PIL), `TER_GPAXLOG=1` (eixos), `TER_CTRLLOG` ([NAV]/
[FCUR]), `/tmp/ternonav`. Scripts no device: `navhelper.sh goto/snap`, `val.sh`, `navtest.sh`, `axlog.sh`,
`shotlaunch.sh`. fmodGetInfo offset 0x8112b0; createSound 0x806cb4; GetKeyRaw 0xc5c51c; GetAxisRaw 0xc5c2f0.

---

## ✅✅✅ SESSÃO 2026-06-17 (noite) — (PARCIALMENTE SUPERADA: controle regrediu depois; ver acima)
**Device `<device-ip>` (ssh root/`nextos`), jogo em `/storage/roms/terraria/`. Lançar: `sh run.sh`.**
**Commit `ebb55c3`. Build/deploy: `cd ~/nextos_ports_android/ports/terraria; ./build.sh; scp terraria .89`.**

### 1) 🔊 ÁUDIO ACELERADO — RESOLVIDO (causa-raiz, não era taxa)
O `DirectByteBuffer` do FMOD reportava capacidade **32768B (8192 frames)** ao `fmodProcess`
(`jni_GetDirectBufferCapacity`), mas o pump (`fmod_audio_thread`) só enfileirava **4096B (1024
frames)** no SDL. fmodProcess **enche o buffer inteiro e avança o clock do mixer** nesse tanto →
o FMOD andava **8× mais rápido** que o playback → áudio acelerado. **FIX:** `g_fmod_cap=4096`
(bloco DSP padrão do FMOD mobile) reportado **E** enfileirado (1:1 com o clock). Taxa fixada em
44100/2 (= `getProperty(OUTPUT_SAMPLE_RATE)` do init; **não** confiar no offset-read — o MIXSPY
provou que o mixer 0x805a94 nem é chamado e a struct do System não é a assumida). Math: pump
back-pressured a ~43 chamadas/s × 1024 = 44100 frames/s = real-time. ✅ Tunável: `TER_AUDIO_BUF`
(bytes/chamada), `TER_AUDIO_BP` (blocos back-pressure, def 4). **Falta só confirmar de ouvido.**

### 2) 🎮 CONTROLE XBOX COMPLETO — FEITO
`g_gp_log` agora tem 16 estados. Lê do js0 (layout xpad/SDL): A0 B1 X2 Y3, LB4 RB5, Back6 Start7,
**LT=eixo2 RT=eixo5** (gatilhos), **L3=btn9 R3=btn10** (cliques), DPad=eixos6/7. `ter_ctrl_feed`
mapeia p/ Controller.Buttons do InControl: Action1-4, ShoulderL/R, **LTrig=6/RTrig=7** (dirige
EIXO+botão), Options=8, **StickL=10/StickR=11**, Back=12. Eixos InControl 0-7 (incl. LTrig/RTrig
analógicos). `my_gamepad_getstate` (XNA, TER_GPAD) também completo. **TER_GPAD NÃO é necessário** em
produção (tudo via TER_CTRL).

### 3) ⚙️ SETTINGS 3ª ABA + IDIOMA — FEITO
- **Abas**: trocam com **LB/RB (L1/R1)** — R1=próxima, L1=anterior (shoulder, ControllerDevice). ⚠️ A
  legenda na tela diz `[LT]/[RT]` mas o que troca de verdade é o **shoulder**. 3ª aba (Video) OK.
- **Up/down dentro da aba**: `ter_menu_nav` (já funcionava).
- **🔑 DROPDOWN (Idioma/Autopause/etc.)**: os itens **NÃO estão nas regiões do GUIInputRegionManager**
  (overlay à direita, x~556 no espaço UI 902×507) e **não respondem a up/down nem a LT/RT** (nem
  ControllerDevice nem XNA). Só respondem à **POSIÇÃO DO CURSOR (Mouse) + clique**. SOLUÇÃO: **MODO
  CURSOR LIVRE** no `ter_menu_nav` — empurrar o **stick DIREITO** entra no modo (suspende nav por
  linhas), move um cursor livre (TER_FCSPEED, def 8) e **A clica** o item sob o cursor; DPad cima/baixo
  volta pra nav por linhas. ✅ Validado: Settings → Idioma → Português brasileiro (UI inteira virou
  PT-BR). **A língua já está em PT-BR no save.**

### 🧪 Infra de teste nova (gated, NÃO afeta produção)
- `TER_GPVIRT=1` + `/tmp/tergp`: tokens `up/down/left/right/a/b/x/y/start/select/l1/r1/lt/rt/l3/r3`,
  `rs:DX:DY` (stick direito), `cur:X:Y` (fixa cursor livre). Dirige o menu por ssh.
- `TER_SHOTLIVE=1` + `touch /tmp/tershot` → `shot.ppm` (1280×720) sob demanda (scp+PIL p/ ver).
- `/tmp/ternonav` (runtime) suspende o override de hover. `navhelper.sh` no device: `goto <linha>`
  (requer TER_CTRLLOG), `snap <arq>`. `val.sh`/`navtest.sh` = launchers de teste.
- `TER_CTRLLOG` loga `[NAV]`/`[FCUR]`; `TER_NAVDUMP` dumpa regiões; `TER_AUDIOSPY` (cs_hook+mix_hook).

### Offsets-chave: (libunity) fmodProcess 0x811378, mixer 0x805a94, createSound 0x806cb4, output
*0xc7c2f0; capacidade BB em `g_fmod_cap` (jni_shim.c). (libil2cpp) GetKeyRaw 0xc5c51c, GetAxisRaw
0xc5c2f0, GamePad.GetState 0xe114ac, SetMousePosition 0xcbf18c. Cursor livre = `g_fcmode/g_fcx/g_fcy`.

---

## 🔊🎮 SESSÃO 2026-06-17 (tarde) — SOM SAINDO (HISTÓRICO, resolvido acima)
**Device: `<device-ip>` (ssh root / senha `nextos`). Jogo em `/storage/roms/terraria/`.**
**Lançar pra jogar: `ssh root@<device-ip> 'cd /storage/roms/terraria; sh run.sh'`** (run.sh já tem som).
Diário de build/test: `cd ~/nextos_ports_android/ports/terraria; ./build.sh` → `scp terraria` → `sh audiotest.sh 80`.

### ✅ SOM AGORA SAI DO ALTO-FALANTE (confirmado de ouvido "to ouvindo bem")
O FMOD aqui é o **áudio nativo do Unity (FMOD Ex 4.x) DENTRO de libunity.so** (NÃO libfmod separada,
NÃO FMOD Studio; org.fmod.FMODAudioDevice = backend AudioTrack do Unity). 3 fixes (todos em `src/main.c`):
1. **🔑 PUMP CORRIGIDO** (`fmod_audio_thread`): o nativo **`fmodProcess`@libunity+0x811378 retorna 0 em
   SUCESSO** (-1 se output NULL), NÃO um byte-count. O pump antigo só enfileirava se `r>0` → **NUNCA
   mandava PCM** (silêncio mesmo com mixer rodando). Agora enfileira quando `r==0`. Bytes/chamada =
   `blk*ch*2` (PCM interleaved s16). Output via `fmodProcess` mixa `blk` frames (`0x805a94`), `blk`=
   `[system+0x7f4]`. SDL device NULL = backend AUTO (pulse/pipewire/alsa, portável). Back-pressure
   (~6 blocos). `system` = `*(*(libunity+0xc7c2f0) + 0x60)`.
2. **SFX funcionam 100%**: createSound de samples (mode `0x10000252` OPENMEMORY_POINT+COMPRESSEDSAMPLE)
   → **result 0** (78 sons OK no teste). O wrapper público createSound = libunity+0x806cb4 (impl
   real 0x7bcf98; valida handle via 0x7b3d68).
3. **MÚSICA via fallback** (`TER_STREAMFALLBACK`, ON no run.sh): a música usa **FMOD_CREATESTREAM**
   (mode `0xd2`, fonte = file-callback do Unity) e o **open de stream (0x864f78) falha INTERNAL=33**
   no so-loader (maquinaria de stream). `cs_hook` detecta a falha e **refaz createSound SEM o bit
   0x80** → carrega como SAMPLE (mesma fonte) → toca. Resultado: "Cannot create" = 0, música carrega.
   Tabela de erros FMOD Ex do Unity: code 33(0x21)=INTERNAL, 25(0x19)=FORMAT (ErrorString@0x3ceb7c,
   tabela em VMA 0xb66948, 8B/código).

### ⚠️ PROBLEMA ABERTO #1 — ÁUDIO TOCA RÁPIDO/ACELERADO (pitch alto)
**Relato: "áudio acelerado no gameplay ENQUANTO O VÍDEO RODA BEM"** (vídeo OK ~58fps, só o som rápido).
Logo NÃO é frame-pacing; é taxa/contagem do PCM. Análise até agora (INCOMPLETA, resolver na próxima):
- Abro SDL a **44100/2ch** e enfileiro `blk*ch*2=4096` bytes/chamada (blk=1024). MAS descobri que em
  vários boots o `fmod_read_format` **caiu no FALLBACK** (44100/2/1024) — o output `*0xc7c2f0` não ficou
  pronto dentro dos 8s do loop de detecção → os "44100" podem NÃO ser os valores reais do FMOD.
- Confusão de offsets: `fmodGetInfo`@0x8112b0 diz samplerate=`[system+0x7d4]`, channels=`[system+0x7c8]`.
  MAS o mixer 0x805a94 trata `[system+0x7c4]` como FORMATO (cmp #0xf) e usa `[system+0x7d4]` como
  multiplicador — inconsistente com "rate=44100". **Preciso de ground-truth.**
- **FERRAMENTA PRONTA**: `TER_AUDIOSPY=1` instala `mix_hook` no mixer (0x805a94) → loga 3× o **count(x2)
  REAL** + campos do system (7c4/7c8/7d4/7f4/7f8/97e8). RODAR: `TER_AUDIOSPY=1` no audiotest.sh e ler
  `[MIXSPY]`. Hipótese principal: **count está em SAMPLES (não frames)** → enfileiro 2× → ~2× rápido;
  fix seria `bytes = blk*2` (ou achar o samplerate real e abrir SDL nele). Confirmar com o MIXSPY:
  ver se `count` casa com 1024 frames stereo (4096B) ou só 2048B; e qual offset tem o rate real
  (procurar 22050/24000/44100/48000 no dump). Ajustar `fmod_read_format` + a conta de bytes.
- Gates: `TER_AUDIO` (liga pump), `TER_STREAMFALLBACK` (música→sample), `TER_AUDIOSPY` (cs_hook +
  mix_hook diagnóstico). run.sh tem AUDIO+STREAMFALLBACK (sem SPY).

### ⚠️ PROBLEMA ABERTO #2 — MAPEAMENTO XBOX COMPLETO (faltam muitos botões)
O porter quer o controle mapeado **como um Xbox completo** (todos os botões; hoje só D-pad↑↓ + A + L1/R1
navegam). Pesquisar na internet o mapeamento PADRÃO Xbox (A/B/X/Y, LB/RB, LT/RT, Start/Back/Guide,
L3/R3, D-pad, sticks) e o que o Terraria espera (InControl/ControllerDevice). A infra de input está em
`src/gamepad.c` + `ter_gamepad_poll`/`ter_input_hook` em `main.c` (substitui
`Controller.ControllerDevice.GetKeyRaw`@il2cpp+0xc5c51c e `GetAxisRaw`@0xc5c2f0 com o estado do js0).
Mapear TODOS os botões/eixos do js0 (SDL) → os action-IDs do InControl. Ver memória de controles na
sessão 2026-06-17 (manhã) acima.

### ⚠️ PROBLEMA ABERTO #3 — SETTINGS: 3ª ABA / SELEÇÃO DE IDIOMA não funciona
No menu inicial → Settings: a navegação não consegue **selecionar a 3ª aba** nem entrar em
itens tipo **Language** e escolher o idioma (ex.: clicar Settings → Language → selecionar). O
`ter_menu_nav` (TER_NAVMENU) navega só ↑/↓ por linhas e ←/→ é L1/R1 nativo; submenus/abas horizontais
e dropdowns (idioma) não respondem. Investigar a navegação de ABAS (provável `UILinkPointNavigator`
ou tabs próprias) + seleção em dropdown. Talvez precise do mapeamento Xbox (#2) completo primeiro.

### 📂 Arquivos tocados nesta sessão
`src/main.c`: `fmod_audio_thread` (pump reescrito), `fmod_read_format`, `cs_hook`+`g_stream_fallback`
(stream→sample), `mix_hook` (MIXSPY), install em ~linha 4214 (gated TER_AUDIOSPY/TER_STREAMFALLBACK).
`run.sh`: + `TER_AUDIO=1 TER_STREAMFALLBACK=1`. Offsets-chave libunity: fmodProcess 0x811378,
mixer 0x805a94, fmodGetInfo 0x8112b0, createSound 0x806cb4/impl 0x7bcf98, stream-open 0x864f78,
ErrorString 0x3ceb7c, output global *0xc7c2f0. Ferramentas RE: capstone (`/tmp/xref.py`,
`/tmp/disasm_arm.py`); VMA==fileoff p/ .text/.rodata em libunity.

---

# HANDOFF — Terraria (Unity 2021.3.56f2 IL2CPP) → Mali-450 so-loader

## 🏆🎮 SESSÃO 2026-06-17 — CONTROLES + SINGLE PLAYER RESOLVIDOS (LEIA PRIMEIRO)
**CONTROLE FUNCIONA NO MENU E NO GAMEPLAY (incl. cursor no jogo) — validado no Tutorial.**
Caminho CORRETO descoberto: o build mobile usa **InControl/Controller.ControllerDevice** pra TUDO
(menu + gameplay + cursor), NÃO o FNA `GamePad.GetState` (que NUNCA é chamado, 0×). Alimentamos via:
- **`Controller.ControllerDevice.GetKeyRaw`@il2cpp+0xc5c51c** e **`GetAxisRaw`@0xc5c2f0** SUBSTITUÍDOS
  (`my_ctrl_getkeyraw/getaxisraw`) devolvendo o estado do js0 (`g_gp_log`/`g_gp_axis`). O `Update`
  do device computa _KeyState/edges/AxisValue → PlayerInput/GUIController* nativos respondem.
- Força `ControllerActionManager.Instance._controllerActive=1` (off 0x30) toda frame (senão a GUI
  fica em modo touch e ignora o controle). (`ter_ctrl_force_active`)
- **Navegação do MENU** (`ter_menu_nav`, TER_NAVMENU): só ↑/↓ por LINHAS agrupadas (dedup + cluster
  por Y) do `GUIInputRegionManager.Instance` (_mouseX@0x14/_mouseY@0x18, regiões em arr@0x48 structs
  16B xMin/xMax/yMin/yMax). Hover via hook de `SetMousePosition`@0xcbf18c (substitui coords pela
  nossa, com relocação adrp). ←/→ é o L1/R1 NATIVO. Espaço de coord da UI ≈ **902×507** (não 1280!).
  Clique = A → Mouse.GetState.LeftButton (g_gp_log[4]) sobre a posição.
- **CURSOR do gameplay**: eixo PROPORCIONAL × `TER_CURSPEED` (default 0.65) — era digital ±1 (rápido).

**SINGLE PLAYER tela preta RESOLVIDA** (`ter_fix_singleplayer`, TER_FIXSP) — 2 bugs em série:
1. `OldSaveSynchronise.CopyOldSaves` → NullReferenceException (migração save antigo, path nulo) → no-op.
2. `GUILowDiskSpacePopup.DiskSpace`@0xd158ac retornava ≤50MB (statfs nativo quebrado, espaço real
   93GB) → patch retorna 1GB → some "Your device is low on storage". +jni_shim StatFs/File getXspace.
Agora Single Player → tela **New Player** (criação de personagem) renderiza.

**BOOT**: `run.sh` precisa `CUP_NOLOGFILE=1` (log em arquivo trava init) + `CUP_FRAMES=999999999`
(default dev=600 encerrava antes do menu). Boot é INTERMITENTE (às vezes trava no logo ~frame 300,
busy-loop do job-system) — relançar resolve.

**Env de produção (run.sh)**: `CUP_GCOFF=1 TER_INLINETASK=1 TER_SKIPJOBWAIT=1 TER_NUKEKB=1
CUP_NOLOGFILE=1 CUP_FRAMES=999999999 TER_GAMEPAD=1 TER_CTRL=1 TER_NAVMENU=1 TER_FIXSP=1`.

**FALTA (próximas missões, em andamento):**
- 🔤 **Nome do personagem (sem teclado)**: ao clicar Name, o campo FOCA (amarelo) mas NENHUM teclado
  abre (`DrKeyboard_IME`/Android não existe no so-loader). O jogo TEM `DrKeyboard_Touch`/`_XBO` e
  `Terraria.GameContent.UI.States.UIVirtualKeyboard` (teclado console). PLANO: forçar a factory
  `DrKeyboard_Base.Create`@0x7b66c4 a escolher `DrKeyboard_Touch` (renderiza teclas na tela → clicar
  c/ o cursor) ou `_XBO` (D-pad), patchando os `get_isSupported`. Bloqueia criar personagem→mundo.
- 🌍 **Criação de mundo**: depende do teclado (nome do mundo). Tutorial JÁ dá um mundo jogável (preset).
- 🔊 **Som (investigado 2026-06-17, NÃO resolvido)**: o FMOD usa o backend **AudioTrack Java
  (`org.fmod.FMODAudioDevice`)**, NÃO OpenSL ES (nem dá dlopen em libOpenSLES → o opensles_shim
  NÃO engata). Sequência: getProperty(OUTPUT_SAMPLE_RATE)=44100 → RegisterNatives(fmodGetInfo@?,
  `fmodProcess(ByteBuffer)@`, fmodProcessMicData) → GetMethodID(FMODAudioDevice.<init>) → **"FMOD
  failed to initialize the output device (60=FMOD_ERR_OUTPUT_INIT)"** → "Cannot create FMOD::Sound".
  CAUSA: o jni_shim NÃO tem handler de NewObject/FMODAudioDevice (init cai no genérico → falha) E
  não há thread Java chamando fmodProcess. INFRA JÁ EXISTE mas DESLIGADA: `fmod_audio_thread`
  (main.c ~3777, gated por `if(0)` na ~4495) chama fmodProcess a cada 10ms; jni_shim tem o
  DirectByteBuffer (g_fmod_pcm[32768], jni_fmod_bytebuffer/pcm). FALTA (2 caminhos):
  (A) COMPLETAR AudioTrack: fingir FMODAudioDevice.init/start OK no jni_shim (NewObject→fake device
      taggeado, init→sucesso/bufsize, start→OK) + ligar fmod_audio_thread + **escrever g_fmod_pcm no
      SDL/PulseAudio** (a thread atual só chama fmodProcess, NÃO manda o PCM pro alto-falante — peça
      que falta). Formato do PCM via fmodGetInfo (provável 16-bit stereo 44100/48000).
  (B) FORÇAR OpenSL: fazer o FMOD escolher OUTPUTTYPE_OPENSL (o opensles_shim já resolve OpenSL→SDL→
      Pulse). Tentar SDK menor em my_sysprop (hoje "25") ou setOutput. Menos provável (o jogo
      registrou FMODAudioDevice = decidiu AudioTrack).
  ⚠️ Mali-450 = PulseAudio (ver memória nextos_openal_pulse). Precisa confirmar de OUVIDO.
- 🕹️ Ajustar `TER_CURSPEED` ao gosto (testar o feeling do cursor no jogo).

## 🎮🎮 CONTROLES — SESSÃO 2026-06-16 (HISTÓRICO — superado acima)
## 🎮🎮 CONTROLES — PRÓXIMA SESSÃO (2026-06-16) — LEIA PRIMEIRO
**Status: jogo RODA no menu (60fps, renderiza), mas NENHUM input funciona no menu.**
**🔑 PISTA DECISIVA: até um MOUSE USB REAL não funciona no menu (testado).** Logo o problema
NÃO é a nossa injeção — é que **input nenhum (real OU injetado) chega ao menu do Terraria**.

### ✅ JÁ DIAGNOSTICADO NESTA SESSÃO (não re-investigar):
- O Terraria **CHAMA `Mouse.GetState/0` E `Keyboard.GetState/0` TODA FRAME** (confirmado com
  contador [FNAMOUSE]/[FNAKB]). **Logo o UPDATE RODA e LÊ o input** — a hipótese "jobs quebram o
  update" está REFUTADA.
- O nosso hook do `Mouse.GetState` preenche o layout PADRÃO FNA (X=int[0], Y=int[1], LeftButton=int[3]
  =offset12) e o cursor (`g_cursor_x/y`) move com o analógico — MAS o menu não reage.
- ⚠️ O "mouse USB real não funciona" é PORQUE o nosso hook do Mouse.GetState SOBRESCREVE o mouse real
  (esperado, NÃO é sinal de que o engine não lê input).

### 🎯 HIPÓTESE PRINCIPAL (CORRIGIDA 2026-06-17): hookar `GamePad.GetState`, NÃO touch/mouse.
⚠️ A hipótese "touch-only" estava ERRADA — confirmamos que o Terraria mobile suporta SIM
controle/teclado/mouse. Estudo completo (decompilação da FNA): **TERRARIA-INPUT-ESTUDO-FNA-vs-
SOLOADER-2026-06-17.md** (doc interna). Achados decisivos:
- O menu do Terraria num CONTROLE usa `UILinkPointNavigator` (D-pad), dirigido por `GamePadInput`
  que lê **`Microsoft.Xna.Framework.Input.GamePad.GetState(0)`** (API FNA/XNA, NÃO InControl direto).
- `CurrentInputMode` é mútuo-exclusivo: com gamepad "ativo" (modo XBoxGamepadUI=4) →
  `IgnoreMouseInterface=true` → **o cursor de mouse que dirigimos é IGNORADO**. Hookamos Keyboard
  (0xe16030) e Mouse (0xe18388) mas **NUNCA o GamePad.GetState** → terra-de-ninguém (mouse ignorado,
  gamepad não alimentado).
- BUG concreto: o layout de `MouseState` que assumimos (`[8]=scroll [12]=Left`) está provavelmente
  ERRADO — na FNA é `{X@0,Y@4,LeftButton@8,Right@12,Mid@16,XB1@20,XB2@24,Scroll@28}` (32B). Clique
  ia pro offset errado.
**PRÓXIMO PASSO:** hookar `GamePad.GetState/1` @**0xe114ac** (struct-return x8, já temos o shim)
retornando `GamePadState{IsConnected=true, Buttons/DPad/ThumbSticks do js0}` e deixar o
`GamePadInput`/`UILinkPointNavigator` NATIVOS dirigirem o menu (mode auto-vira XBoxGamepadUI).
Layout do GamePadState e flags dos botões XNA estão no doc de estudo. Garantir
`Main.SettingBlockGamepadsEntirely==false` + `FocusHelper.AllowInputProcessingForGamepad→true`.
Mesmo padrão do cuphead (hook da API de input GERENCIADA ← js0).

### O que JÁ FOI FEITO (não repetir):
- ❌ `nativeInjectEvent(KeyEvent)`: BECO. Retorna false, nunca lê o evento (Unity 2021 espera ponteiro
  nativo AInputEvent do NDK, não um KeyEvent Java fake).
- ✅ Capability de **patch de método il2cpp em runtime** (ter_nuke_methods/ter_input_hook em main.c):
  acha classe.método por nome (il2cpp_class_from_name/get_method_from_name @ offsets no main.c) e
  patcha o methodPointer. Struct-return (x8) resolvido com shim inline `mov x0,x8; ldr x16,[pc+8]; br x16`.
- ✅ Leitor js0 self-contained (`ter_gamepad_poll`): g_gp_log[12] (estado lógico+edge) + g_cursor_x/y
  (cursor movido pelo analógico). Funciona (eixos lidos, cursor move). TER_GAMEPAD=1.
- ✅ **Device Xbox 360 virtual** reconhecido pelo InControl (jni_shim: getDeviceIds→[1], getDevice
  estático→device, vendor 1118/produto 654/getSources 0x1000611).
- ❌ Hook `Keyboard.GetState/0` (@0xe16030): retorna js0 como setas/Enter — **menu NÃO responde**.
- ❌ Hook `Mouse.GetState/0` (@0xe18388): cursor move (cur chega nos cantos) — **menu NÃO responde**.
- ⚠️ MouseState = 36 bytes (9 ints): [0]=X [4]=Y [8]=scroll [12]=LeftBtn [16]=Right... (confirmar
  layout — o LeftButton pode estar em offset diferente). KeyboardState = 8 uints bitmask + 1 int.

### Becos que NÃO vão ajudar:
- gptokeyb/uinput: o mouse USB REAL já falha → o engine não lê input do OS; injetar via uinput usa
  o MESMO caminho OS→engine que está quebrado. Não adianta.

### Offsets úteis (libil2cpp, base = g_il2cpp_base):
Keyboard.GetState/0=0xe16030 /1=0xe164d0 · GamePad.GetState/1=0xe114ac · Mouse.GetState/0=0xe18388
/1=0xe17c84 · PlayerInput.UpdateInput/0=0x837e9c · Main.Update/1=0xfb97d0 · Main.DoUpdate/1=0xfb9a3c.
Toda a infra de gamepad está em main.c (ter_gamepad_poll/ter_input_hook/ter_fna_*) + jni_shim.c (device).
Env de teste: `CUP_GCOFF=1 TER_INLINETASK=1 TER_SKIPJOBWAIT=1 TER_NUKEKB=1 TER_GAMEPAD=1 TER_GPLOG=1`.

---

## 🏆🏆🏆 RODANDO + RENDERIZANDO + MENU A 60FPS (2026-06-16) 🏆🏆🏆
**Terraria so-loader Unity 2021.3 IL2CPP RODA no Mali-450 Utgard: splash Re-Logic → MENU, 60 FPS,
cores corretas (confirmado na TV).** Receita (env de lançamento):
```
CUP_GCOFF=1 TER_INLINETASK=1 TER_SKIPJOBWAIT=1 TER_NUKEKB=1
SDL_VIDEODRIVER=mali LD_LIBRARY_PATH=/usr/lib:$GAMEDIR
```
**Os 5 fixes que destravaram (em ordem):**
1. **TER_INLINETASK** — trampolim em libunity+0x2f37a4 que FINGE a conclusão do per-object
   future-task (seta node + incrementa contador c10360). O dispatch nativo pros worker threads
   está quebrado no so-loader (workers ociosos, nunca alimentados); fingir destrava sem precisar
   consertar o dispatch. (ter_inline_task + trampolim, main.c)
2. **TER_SKIPJOBWAIT** — pula o WaitForJobGroup (0x2f1d48); os job-groups gerais também não
   completam (mesmo dispatch quebrado). Com (4) embaixo, o abort que isso causava vira warning.
3. **dl_iterate_phdr REAL** wirado (era stub) — o unwinder C++ acha o eh_frame de libunity/libil2cpp;
   exceções C++ (shader/currentActivity) viram CAPTURADAS em vez de std::terminate→abort.
4. **TER_NUKEKB** — usa a API il2cpp REAL (exportada: il2cpp_class_from_name/get_method_from_name)
   p/ patchar o methodPointer de `KeyboardInput.Update` → `ret`. Eliminou a exceção `Field
   PressedStates not found` (reflection Java fake) que rodava 5×/frame. (ter_nuke_methods, main.c)
5. **🔑 FIX DA TELA PRETA — my_eglGetProcAddress roteando egl* via egl_route**: o engine pegava o
   `eglChooseConfig` REAL do Mali via eglGetProcAddress (o ds_route só roteava GL, não egl*). O Mali
   Utgard rejeitava os configAttribs (GLES3) com EGL_BAD_ATTRIBUTE → GfxDevice virava NULL-renderer
   → ZERO chamadas GL → tela preta. Roteando egl* → egl_shim_ChooseConfig (ignora attribs, devolve
   a config válida da window SDL) → o render funciona. (main.c my_eglGetProcAddress)

**Capability nova poderosa: patching de método il2cpp em runtime** (ter_nuke_methods) — acha
qualquer classe.método por nome e patcha o code. Reusável p/ neutralizar qualquer método managed
problemático.

**FALTA (polish):** (a) controles (input — gamepad.c existe, wirar ao Terraria); (b) áudio (FMOD
falha a init do output device — "Error initializing output device"); (c) my capture (glReadPixels lê
black por double-buffer/FBO — ler front buffer ou o FBO do worker; vê-se na TV, é só diagnóstico);
(d) jobs rodam FINGIDOS (INLINETASK/SKIPJOBWAIT) — o jogo roda, mas se algo depender de job real,
revisitar; (e) empacotar PortMaster. Device .89. Commits: 482b2a6 (IMAGEM) + anteriores.

---


> Para a próxima sessão: o usuário vai dizer **"continuar terraria"**. Leia este arquivo
> inteiro primeiro. Projeto iniciado DO ZERO em 2026-06-16. Device `.89` (Amlogic-old,
> Mali-450 Utgard, fbdev, EmuELEC 3.14.79 aarch64, senha ssh `nextos`/root).

## 🧭🧭 SESSÃO 2026-06-16 (++noite, "topologia do job-system") — DADOS NOVOS SÓLIDOS
**Resultado:** sem imagem ainda, MAS o muro do job-system foi MAPEADO com dados reproduzíveis
(antes era teoria). Corrigi 1 suposição errada do handoff e estreitei a causa-raiz.

**⛔ CORREÇÃO DE SUPOSIÇÃO (FATO 1 estava parcialmente errado):** as threads da ENGINE NÃO passam
pelo nosso `pthread_create_fake`. `pthread_create` **não está** na `PT_LIST` (main.c:114) — nem
`pthread_key_create`/`getspecific`/`setspecific`. Elas resolvem p/ o **glibc REAL** (so_resolve
passthrough). Isso é OK p/ correção (glibc cria thread + keys per-thread certas), mas significa que
NÃO temos hook nativo na criação das threads da engine por padrão. (FATO 1 acertou que mutex/cond/sem
passam pelos shims; errou ao implicar que pthread_create também passa.)

**🔬 INFRA NOVA (gated, default OFF) — commitar:** 
- `TER_JOBLOG=1` agora (a) roteia o `pthread_create` da engine pelo nosso trampoline
  (`install/patch_pthread_shim`, só sob TER_JOBLOG) e (b) loga p/ cada thread criada:
  `[JOBTHR] tid start=libunity+0xOFF arg=<ctx> a[0..5]` (pthread_fake.c thr_trampoline).
- `ter_unity_base()`/`ter_il2cpp_base()` exportados de main.c (symbolizar offsets).
- Reusei `TER_FUTEXLOG` (já existia) p/ topologia WAIT/WAKE.

**📊 TOPOLOGIA MEDIDA (env: `TER_JOBLOG=1 TER_FUTEXLOG=1 TER_FAKEACK=1 CUP_GCOFF=1 TER_SKIPTASKWAIT=1`,
chega ao frame 3 `[r3>`):**
- TODAS as threads têm `comm=UnityMain` (o `pthread_setname_np_fake` é no-op → nenhuma é renomeada;
  os nomes "Job.Worker"/"Background" do handoff antigo vieram do gdb, NÃO do nosso runtime).
- Worker loop comum = **`libunity+0x23741c`** (a maioria das ~29 threads). `arg` = contexto per-worker
  (array contíguo em 0x7f40_1c…; spacing **0x88** p/ o pool de 16 "background", **0x1d0** p/ 3 threads
  dedicadas). 2 threads especiais usam start=**`libunity+0x80f768`** (prováveis controllers).
- O worker lê um **manager GLOBAL** em `*(b87000+0xc78)` (x23→x20 em 0x23743c-48; se 0 → lazy-init
  `0xfdafc`), registra-se numa **lista global** em `c10240` sob lock `c10208` (fn `0x2f0418`), e executa
  o job via ponteiro em `[arg+56]`/`[arg+64]` (blr). O `mrs tpidr_el0; ldr [x,#40]` é só o **canary**
  (slot bionic +0x28, coberto pelo pad TLS) — NÃO é índice de worker.
- **WAIT (quem dorme onde):** 3 workers no sem do pool **`…105360`**; 16 background no sem **`…1c40e0`**;
  3 dedicadas em sems próprios (`…1c9eb0/ca080/ca250`, spacing 0x1d0, = arg+0x80); +1 thread só em `…107af0`.
- **WAKE (quem acorda quem):** a **MAIN (tid pai)** só faz `FUTEX_WAKE` em **`…107af0`** (22×) →
  acorda **1 thread dispatcher** que espera lá; essa dispatcher acorda a main de volta em `…a7a358` (3×).
  Handshake main↔dispatcher FUNCIONA. **MAS `…105360` e `…1c40e0` (os pools de worker) NUNCA recebem
  wake de NINGUÉM no run inteiro.** (grep WAKE = 0 nesses uaddr.)

**🎯 CAUSA-RAIZ ESTREITADA:** os workers estão **saudáveis e corretamente PARKED** esperando trabalho;
o bug é 100% no lado **ENQUEUE/SIGNAL**: a main agenda jobs (alvo do contador sobe) mas **o trabalho
nunca chega à fila que os workers (e o work-steal da própria main, `0x2c6754`) consultam**, e os pools
nunca são sinalizados. Não é lost-wakeup (FUTEXPOLL acorda os workers e eles acham fila VAZIA — já testado).
A main só conversa com 1 dispatcher (`…107af0`), nunca com os pools.

**🥇 HIPÓTESE #1 p/ a PRÓXIMA SESSÃO (mais promissora):** a **MAIN thread não está REGISTRADA** no
job-system. Os workers se registram via `0x2f0418` (lista global `c10240`). A main é a thread do NOSSO
loader (não criada pela engine) → pode nunca ter recebido um **índice de thread do job-system** (a fila
é deque per-thread; o push da main vai p/ um deque ÓRFÃO que ninguém drena). **TESTE decisivo (barato):**
detour de 8 bytes na ENTRADA de `0x2f0418` logando `tid` → ver se `tid==getpid()` (main) aparece.
Se a main NÃO registra → achar o `RegisterExternalThread`/`InitializeJobThread` da Unity 2021 e
forçá-lo p/ a main (ou registrar a main manualmente antes do 1º schedule).
**Push de job identificado:** `0x2eaafc` (`array[count++]=item`, sem lock/sem-post — caller segura lock);
tracear quem chama e p/ qual `x1`(fila) vs a fila que o worker dequeia.

**Becos desta sessão (NÃO resolvem):** TLS nativo (libunity tem **0 relocs TLS**, sem PT_TLS → não usa
`__thread`; índice de worker NÃO vem de TLS-ELF). Flag "threaded" `c0da20` é setada em `0x2eaae8` por 2
checks de capability no init — forçá-la não muda o enqueue (handoff já sabia). pthread_create hook não
muda comportamento (só loga).

## 🔬🔬 SESSÃO 2026-06-16 (madrugada, "dispatch a fundo via gdb") — RAIZ COMPARTILHADA ACHADA
**Sem vídeo ainda, MAS a raiz dos DOIS muros foi unificada e MUITO estreitada.** gdb está no device
(`/usr/bin/gdb`, sem python). Snapshots via scripts auto-limpantes em `/storage/roms/terraria/gdb*.sh`.

**🎯 DESCOBERTA-CHAVE (muda a estratégia): os DOIS muros têm a MESMA raiz.**
- O contador `c10360` que o **WaitForJobGroup** (frame 3, `0x2f1d1c`) espera é incrementado por
  **`0x2f3a98`** = a **CONCLUSÃO do per-object future-task** (frame 2, `0x2f37c0`). Ou seja: o
  `TER_SKIPTASKWAIT` (que pula a wait do per-object task) é o que CAUSA o hang da frame 3 — sem o task
  rodar, o contador nunca sobe → WaitForJobGroup trava. **Sem o skip**: trava no próprio per-object task
  (frame 2). **Com o skip**: trava no WaitForJobGroup (frame 3). MESMO worker que não roda. 
- ⚠️ **PARAR de usar SKIPTASKWAIT como solução** — ele mascara o problema. O alvo é fazer o WORKER do
  per-object task EXECUTAR o functor (que chama `0x2f3a98` → counter++ → destrava ambos).

**📊 ESTADO RUNTIME (gdb, env `CUP_GCOFF=1 TER_SKIPTASKWAIT=1`, frame 3):**
- `FLAG c0da20 = 1` (forçada) ou `0` (default) → NÃO muda nada. `COUNTER c10360 = 0`. Manager
  `*(b87000+0xc78)` = válido.
- **Main** (gdb confirmou, simbolizado): chain = high-level frame-sync `U+0x2d7620` → frame-budget
  `U+0x2ea804` → `WaitForJobGroup U+0x2f1d58` (`pthread_cond_wait` no cond `c10330`, mutex `c10308`).
  A própria main roda o predicado `0x2c6754` (retorna false) e aí dorme — ela TENTA ajudar mas não acha job.
- **TODAS as 19 threads de job** (`Job.Worker 0-2` + `Background Job.` ×16) + Loading.Preload/AsyncRe/
  BatchDelete estão **SAUDÁVEIS e ociosas** em `futex_wait` (`U+0x6ecd54`), esperando trabalho — NÃO é
  GC-suspend, NÃO é estado ruim. Só nunca são alimentadas.
- Worker chain (gdb, stack raw simbolizado): `0x23741c`(entry)→`blr [arg+64]`(run-fn, ret `0x2374f8`)→
  `0x113a04`→`0x19f654`→`0x1a0414`→`0x1a195c`→sem Acquire (`futex_wait` em `0x1a1958`, sem=x19, count=[x19]).
  O Release/post do sem (acorda worker) é `0x1a1a54` (`futex_wake 0x6ecd60`). **No run inteiro, NINGUÉM
  posta o sem dos pools** (FUTEXLOG: 0 WAKE neles). A main só posta UM sem (`…107af0` = 1 thread só, que
  é um worker `0x23741c` normal numa fila SEPARADA, arena `0x7f50`); os pools (`…105360` 3-workers /
  `…1c40e0` 16-workers, arena `0x7f40`) nunca recebem post.

**🧠 DIAGNÓSTICO ATUAL (o gap exato): JOB AGENDADO MAS NUNCA ENFILEIRADO.** A main incrementa o ALVO
(`WaitForJobGroup` espera counter>=target, target = `*(c0d8e0)`+índice = nº agendado), mas:
- O job NÃO está na fila dos workers (FUTEXPOLL acorda os workers e eles acham fila VAZIA — testado).
- O work-steal da própria main (`0x2c6754`) não acha job.
→ Entre "Schedule (target++)" e "enqueue na deque do worker + post sem", **o ENQUEUE/flush falha**.
Provável: a checagem de "workers ociosos disponíveis" no submit lê 0 (mesmo com workers ociosos) →
pula o post; OU o worker não registra seu estado "idle" corretamente. O submit do per-object task é
`0x2f3680`(ctor+submit+wait) → `0x2c59e4`(lock+`0x7a799c`+unlock) → `0x7a799c` (enqueue real, container
em `0x7axxxx`) — investigar `0x7a799c` e quem deveria postar o sem do pool dedicado.

**✅ Becos FECHADOS com dados (NÃO É):** GC (sem FAKEACK roda igual — o sigmask-fix já suspende as
threads; FAKEACK é DESNECESSÁRIO agora). Flag threaded `c0da20` (forçar 0/1 não muda). Worker count:
nem `sysconf` (CUP_1CORE), nem boot.config `job-worker-count=0` reduzem (Unity lê CPU de
`/sys/devices/system/cpu/{present,possible}` = `0-3`). `TER_1CPU` (novo: redirige esses p/ "0") REDUZIU
Job.Worker 3→1 mas Unity clampa a min 1 e os 16 Background são fixos → não destrava. FUTEXPOLL
(force-wake) não destrava (fila vazia). SKIPJOBWAIT (pular WaitForJobGroup) = **ABORT** libil2cpp+0x7b14d0
(os results dos jobs SÃO necessários). FORCETHREADED não avança além do SKIPTASKWAIT sozinho.

**🔑 PRÓXIMO PASSO (claro):** RE de `0x7a799c` (o enqueue do submit) — achar a checagem que decide
postar (ou não) o sem do worker, e por que ela vê "0 workers ociosos". OU: implementar execução INLINE
do functor do per-object task na própria main (functor em `obj[0]`, vtable `b59e48`; rodar antes da wait
`0x2f37a4` em vez de pular). Comparar o estado "idle-count" do pool entre o submit e o worker (gdb: dump
do manager `*(b87000+0xc78)` e da estrutura do pool). Device `.89`. Infra nova: `TER_1CPU`,
`TER_FORCETHREADED`, `TER_JOBLOG` (todas gated, default OFF).


## ⚡⚡ TL;DR REESCRITO (2026-06-16 madrugada++ / sessão "1 erro por vez")
🟢 **A CONCLUSÃO PESSIMISTA ANTERIOR ESTÁ REFUTADA. O port é viável (como cuphead).**

**FATO 1 — REFUTADO "threads bionic-static":** `readelf -sW --dyn-syms` em libil2cpp.so E libunity.so
mostra `pthread_create@LIBC`, `pthread_kill`, `pthread_cond_*`, `sem_*`, `sigaction@LIBC`, `syscall`
TODOS como **UND** → TODAS as threads (incl. GC Finalizer/Loading/Job.Workers) passam pelos NOSSOS
shims (pthread_fake/sem_shim/my_syscall). Temos controle total. A afirmação "bionic-static, bypassa
interposição em 3 níveis" era FALSA (a sessão anterior grepou errado).

**FATO 2 — o shim de cond FUNCIONA:** instrumentei `pthread_cond_wait/signal/broadcast_fake`
(TER_CONDTRACE): a main passa por DEZENAS de cond-waits (caller `libunity+0x2f02e8`) que SÃO
sinalizados e ela acorda. Sem lost-wakeup.

**FATO 3 — o muro REAL (não é o GC):** com `TER_FAKEACK=1 CUP_GCOFF=1` passa-se do GC e a main
trava no ÚLTIMO cond-wait: **`libunity+0x2f37c0`** (função em `0x2f367c`), cond `obj+0x88`,
**NUNCA sinalizado** (TER_CONDTRACE confirma: 0 signals nesse cslot). NÃO é GC, NÃO é lost-wakeup,
NÃO é Mali. É um **produtor que nunca roda**.

**FATO 4 — o que é a função `0x2f367c`:** classe C++ templada de **job-queue/thread-pool com worker
dedicado** (módulo libunity ~0x2f0000–0x2f3f00). A main constrói uma "future-task" (functor em
`obj[0]=libunity+0xb62278`, vtable {0x2f3900,0x2f390c,0x7abe04,0x2f37e8,0x2f3800}; fila `obj+0x58`,
mutex `obj+0x60`, cond `obj+0x88`), e BLOQUEIA em `while(node->next==0) cond_wait`. Produtores
(lado worker) = `cond_broadcast` em **0x2f39b0 / 0x2f3a78 / 0x2f3aec**. Workers criados via
`pthread_create` em **0x2f0330 / 0x2f3e30** (no 0x2f0330: `syscall(sched_setaffinity)` antes).
A fase é **init de serialização de classes builtin** (caller `0x2c2038` referencia a string
"Mismatched serialization in the builtin class '%s'"). Acontece no **frame 2 do nativeRender**,
logo após o setup do Choreographer (HandlerThread/Looper/Handler/FrameCallback — JNI: sendToTarget).

**FATO 5 — becos JÁ testados nesta sessão (NÃO resolvem):**
- `CUP_1CORE=1` (sysconf=1 core) — workers já criados, não muda.
- `-force-gfx-direct` (corrigido de `-force-gfx-st`, que NÃO é arg real) — **a engine NEM lê
  `/proc/self/cmdline`** (o `[CMDLINE]` nunca loga; usa `il2cpp_set_commandline_arguments`).
  A thread `UnityGfxDeviceW` continua viva. Injeção de cmdline atual = código MORTO.
- `CUP_SEMPOLL=20 CUP_CONDPOLL=20 TER_FUTEXPOLL=20` (acorda TODOS os waiters periodicamente) — NÃO
  destrava → o worker, mesmo acordado, não acha trabalho na fila dele → **o dispatch p/ o worker
  está quebrado/ausente**, não é lost-wakeup.
- handleMessage driver (sendToTarget→invoca Handler$Callback.handleMessage) — `sendToTarget` só tem
  o método-ID CACHEADO; **nunca é CHAMADO** antes da trava. O Choreographer é setup, não o gatilho.

## 🟢🟢 AVANÇO (mesma sessão, +tarde): PASSOU DO FRAME 2! + job system identificado
- **`TER_SKIPTASKWAIT=1`** (patch binário libunity+0x2f37b0 `cbnz x8 -> b 0x2f37c4`, pula a wait da
  per-object task): **frame 2 AGORA COMPLETA** (`<r2]` aparece, nunca aparecia) e **frame 3 entra
  (`[r3>`)**. A saída 0x2f37c4 só faz mutex_unlock+ret (não deref o item) → pular é seguro p/ avançar.
- Frame 3 trava num NOVO wait: **`libunity+0x2f1d1c` = WaitForJobGroup** (`while([0xc10360] < target)
  cond_wait`, mutex/cond GLOBAIS em 0xc10308/0xc10330). Contador completado **0xc10360 = 0**.
- Agendador = **`0x2ea800`** (na vdd a função começa antes; bl WaitForJobGroup está em 0x2ea800).
  Decide threaded (flag byte **0xc0da20**==1 + itemcount>=24). Antes de esperar, tenta work-steal via
  **`0x2c6754`** (try-execute-1-job, checa flags do job system em **0xc0a180..0xc0a18c**) — retorna
  false (não acha job) → vai pro wait.
- Workers (`Job.Worker 0-2` + 16 `Background Job.`): **utime/stime = 0/0** (NUNCA executaram nada),
  affinity cpus=0-3 (OK, não é pin errado). Force-wake (TER_FUTEXPOLL+CUP_SEMPOLL+CUP_CONDPOLL) NÃO
  os faz processar → **o job NÃO está na fila que os consumidores (worker E work-steal da main) olham**.
- **`TER_SKIPJOBWAIT=1`** (pula tb 0x2f1d48 do WaitForJobGroup) → **ABORT (sig 6)**: "The referenced
  script on this Behaviour (Game Object '<null>') is missing!" → os job-results SÃO necessários (não dá
  p/ só pular; tem que EXECUTAR).
- **`TER_JOBINLINE=1`** (sched_getaffinity → 1 CPU, p/ Unity criar 0 workers e rodar inline): NÃO
  destravou (volta ao wall do frame 2). O worker-count NÃO vem do sched_getaffinity (ou a per-object
  task não depende dele). Investigar de onde vem o nº de workers (get_nprocs? /sys/devices/system/cpu/
  online|possible? um global do boot.config?).

## 🎯🎯 SMOKING GUN (mesma sessão, +++tarde): JobQueues DESCONECTADAS
Via `/proc/<tid>/syscall` (uaddr=campo2, op=campo3) + TER_FUTEXLOG (loga FUTEX_WAIT op 0/9 e
FUTEX_WAKE op 1/10 que passam pelo `syscall` import / my_syscall):
- Os workers esperam num **futex PRIVATE** (op=0x80) via helper **libunity+0x6ecd04**
  (`futex_wait_private(uaddr)`; o `futex_wake_private(uaddr,n)` é **0x6ecd60**; o PLT 0x86700
  "expf" é na vdd `syscall`). Worker bt: `syscall → my_syscall → libunity+0x6ecd54`.
- `Job.Worker 0-2` esperam todos em **uaddr=0x7e90105360** (semáforo COMPARTILHADO do pool).
  `Background Job.`(16) em 0x7f201c40e0. `Loading.Preload` em 0x7f20bdf130. `Loading.AsyncRe` em 0x7ec0000544.
- A MAIN faz FUTEX_WAKE em **0x7e90107af0**(16×), 0x7f201c3dd8, 0x7f20a7a358 — **NENHUM coincide com
  os uaddr dos workers**. O semáforo dos Job.Worker (0x7e90105360) **NUNCA recebe wake de ninguém**
  no run inteiro (`grep WAKE.*0x7e90105360` = 0).
- **CONCLUSÃO: o scheduler e os workers estão em JobQueues/semáforos DESCONECTADOS.** O scheduler
  sinaliza semáforos que nenhum worker espera; os workers esperam semáforos que ninguém sinaliza.
  Provável **2 instâncias do JobQueue** (workers ligados à instância A criada no init; scheduler/main
  usando instância B) OU a fila do scheduler ≠ a fila que os workers consomem. (NÃO é lost-wakeup:
  FUTEXPOLL acorda os workers e eles acham a fila VAZIA; NÃO é __cxa_guard: é interno do libunity, OK.)
- Becos: `0x2ea9dc` (branch `cbz [0xc0da20]`) é só epílogo/return, NÃO execução inline → não dá p/
  forçar serial por aí. `TER_JOBINLINE` (sched_getaffinity→1 CPU) não mudou o worker-count.

**🔑 PRÓXIMO PASSO DECISIVO p/ a próxima sessão:** descobrir POR QUE há 2 instâncias / fila desconectada.
- Achar o GLOBAL que guarda o ponteiro do JobQueue (o scheduler 0x2ea800 e o worker-loop 0x6ecd54
  o leem). Comparar o `this` do JobQueue no scheduler vs nos workers (gdb: dump o objeto, ver o
  endereço do semáforo embutido). Se diferem → 2 instâncias.
- Ver o INIT do JobQueue (quem cria os workers + o semáforo) e se roda 2× no nosso loader (init_array
  duplicado? JNI_OnLoad 2×? um global reiniciado?). Hookar a criação dos workers (pthread_create em
  0x2f0330) e logar o ponteiro do JobQueue/semáforo passado a cada worker vs o que o scheduler usa.
- Alternativa de fix se for 2-instâncias: forçar o scheduler a usar a MESMA instância dos workers
  (ou vice-versa) — ou impedir o segundo init.
- Outra pista a investigar: o uaddr do worker (0x7e90105360) e o do main-wake (0x7e90107af0) estão na
  MESMA arena (0x7e90...), diff 0x2790 → instâncias adjacentes alocadas no mesmo pool/heap.

**🔑 SÍNTESE DO MURO:** DOIS mecanismos de worker (1: per-object future-task classe 0x2f367c/0x2f3e30,
threads dedicadas Loading.Preload/AsyncRe/BatchDelete; 2: job system global 0x2ea800/0x2f1d1c, Job.Worker/
Background). AMBOS: worker criado e ocioso em wait; main agenda+espera; **o trabalho nunca chega ao
worker** (force-wake não resolve, contador fica 0). A CONDTRACE prova: o cond em que a main espera
NUNCA é sinalizado. **Causa-raiz comum provável: o ENQUEUE (main→fila do worker) não coloca o item na
fila que o worker consome** (≠ lost-wakeup). 

**PRÓXIMO PASSO MAIS PROMISSOR:** instrumentar o lado PRODUTOR/ENQUEUE: tracear cond_signal/broadcast +
sem_post + FUTEX_WAKE FEITOS PELA MAIN (e por qq thread) ANTES da main esperar — ver SE o enqueue
acontece e p/ qual fila/cond vai. Comparar com o cond em que o worker dorme. Se o enqueue sinaliza um
cond ≠ do que o worker espera → achamos o mismatch. (Análogo ao TER_CONDTRACE, mas no lado do post.)
Hooks candidatos: entrada das fns produtoras 0x2f39b0/0x2f3a78/0x2f3aec (per-object) e o enqueue do
job system (achar quem incrementa o "submitted"/empurra na fila antes de 0x2ea800).
Considerar tb: a fila pode ser lock-free (atomics) indexada por WORKER-INDEX/thread — se o índice da
thread (TLS) estiver errado no nosso env, o enqueue vai p/ deque errado. Verificar pthread_getspecific/
TLS keys do job system.

**PRÓXIMOS PASSOS (ordem antiga):**
1. Hookar a ENTRADA das funções produtoras `0x2f39b0/0x2f3a78/0x2f3aec` (log `this`+caller) e a
   start_routine dos workers `0x2f0330/0x2f3e30` → confirmar que NENHUM worker chega ao "complete"
   e descobrir ONDE o worker bloqueia (qual sem/cond/fila de ENTRADA dele).
2. Achar o ENQUEUE (lado main→worker): a main deveria enfileirar a task numa fila GLOBAL do pool +
   acordar o worker, ANTES de esperar. Tracear se isso acontece (cond/sem/futex do worker).
3. Identificar a classe exata (templada): comparar com Unity 2021.3 (provável `JobQueue`/`ThreadPool`/
   `WorkStealingQueue` ou `PersistentManager` load). Ferramentas: `~/re-tools`, objdump do cross.
4. Plano B: binary-patch p/ pular a wait (estilo NOGCWAIT) e ver o PRÓXIMO muro (progresso), aceitando
   crash provável (a task pode ser necessária).

**Infra de diagnóstico criada (já no código, gated por env):**
- `TER_CONDTRACE` (pthread_fake.c): loga `[CT] WAIT tid/cslot/caller` de todo cond-wait + `[CT] SIGNAL/
  BROADCAST p/ cslot da MAIN`. Decisivo.
- `TER_FUTEXLOG` (main.c my_syscall): loga caller de cada FUTEX_WAIT (⚠️ gettid usava nº errado;
  arm64=178, não 186 — corrigir se reusar).
- handleMessage wiring (jni_shim.c): obtainMessage/sendToTarget→`jni_handlemessage` (inerte por ora).
- Scripts no device em `/tmp/ter*.sh` (diag de threads/bt/obj-dump/FP-walk via /proc/mem+dd).
- **Bases libunity/libil2cpp** (loader mmap manual, NÃO aparecem por nome em /proc/maps): pegar as 2
  regiões `r-xp` anônimas (não /usr/lib, não /storage) — a maior(~48MB)=libil2cpp, ~12MB=libunity.

## (HISTÓRICO — pré-refutação) ⚡ TL;DR — onde estávamos (2026-06-16 noite)
🎉🎉 **3 muros resolvidos: GUID + CASESENSITIVE + ENLIGHTEN. O jogo BOOTA E CARREGA INTEIRO.**
Cria contexto **GLES2 no Mali-450**, FMOD, carrega 1ª cena, InControl v1.8.11, **`stopActivityIndicator`
(loading TERMINA!)**, monta o Choreographer de frames. **MURO NOVO**: o `nativeRender` do **frame 2
TRAVA** (não retorna) — main thread (UnityMain) presa em wait de semáforo (job-system / async-load),
no setup do **Choreographer** (HandlerThread+Looper+FrameCallback "UnityChoreographer"). Frames 0 e 1
completam; frame 2 entra e nunca sai. **0 swaps ainda (sem imagem na tela).**
**Objetivo do usuário: IMAGEM do jogo.**

## ✅ JOB-SYSTEM DESTRAVADO (2026-06-16 noite tarde) — 2 RAÍZES: `syscall` stub + race no pthread_fake
Frame 2 travava: a main (nativeRender) ficava em `sh_sem_timedwait` esperando jobs que nunca
terminavam. Duas causas:
1. 🔑 **`syscall` STUBADO** (retornava 0). O job-system do Unity usa `syscall(SYS_futex, FUTEX_WAKE)`
   CRU p/ acordar a main; com o stub no-op, o wake nunca chegava e as Job.Worker/Background
   busy-spinavam no `stub_syscall` (visto no gdb bt). FIX: `my_syscall` forward real (arm64: nº de
   syscall igual bionic/glibc/kernel) + set_import/patch_got nos 2 libs. Agora as threads BLOQUEIAM
   de verdade no futex (sem busy-spin). **LIÇÃO: `syscall` cru tem que ser REAL no so-loader.**
2. 🔑 **race no lazy-init do `pthread_fake.c`** (`cond_get`/`mtx_get`/`sem_get`/`rwl_get`): 2 threads
   tocando um primitivo ESTÁTICO (PTHREAD_*_INITIALIZER=0) ao mesmo tempo criavam CADA UMA o seu
   objeto glibc e a 2ª sobrescrevia → uma espera no cond C1, outra sinaliza C2 → **LOST WAKEUP**
   (travas entre frames). FIX: lock GLOBAL só no caminho de criação (impede double-create); fast-path
   = leitura SIMPLES do slot (NÃO usar `__atomic_*`/LDAR — os slots bionic podem ser 4-byte-aligned
   → LDAR de 8B faulta **SIGBUS**; confirmado fault @...d4). Com isso, frame 2 COMPLETA sem band-aid.

## 🧱🧱 MURO REAL IDENTIFICADO (2026-06-16 madrugada): il2cpp GC STOP-THE-WORLD
**O Choreographer era RED HERRING.** A main trava (frame 1 OU 2, nondeterminístico por threshold de
alloc) em `sh_sem_timedwait` chamado de **`libil2cpp+0x74f2c0`** = **`WaitForThreadsToSuspend`**
(fn @0x74f260, referencia `"pthread_kill failed at suspend"`): o GC faz stop-the-world, manda
**SIGPWR(30)** p/ suspender as threads e ESPERA cada uma dar ACK (semáforo, timeout 10ms, RETRY
infinito). **Uma thread nunca dá ACK.**
- **Diagnóstico DEFINITIVO** (via hook `pthread_kill` [TER_PKLOG] + `/proc/<tid>/status` SigBlk):
  o GC manda SIGPWR p/ 2 threads; a **thread A** recebe o restart SIGXCPU(24) (=deu ACK), a
  **thread B (0x7f72f89...≈GC Finalizer)** só recebe SIGPWR repetido, NUNCA o restart. Motivo:
  GC Finalizer / Loading.Preload / 1 UnityMain têm **SigBlk=0x7ffffffe7ffbbed9** = bloqueiam quase
  TODOS os sinais async incl. **SIGPWR**. São threads de suspensão **COOPERATIVA** (bloqueiam SIGPWR
  de propósito; deveriam suspender via safepoint, não via sinal). Thread B está PRESA num bloqueio
  nativo (futex/sem do NOSSO shim) → nunca chega num safepoint → o GC tenta preemptar via SIGPWR →
  bloqueado → DEADLOCK.
- **Becos testados (NÃO resolvem):** pthread_sigmask ABI-fix (correto, necessário, mas insuf.);
  `il2cpp_gc_disable` (CUP_GCOFF, offset 2021 corrigido p/ 0x73ca6c — NÃO previne o stop-the-world);
  desbloquear SIGPWR no thread-create (TER_SIGUNBLK → REGRIDE p/ frame 1: threads suspendem sem
  restart → freeze); filtrar o block de SIGPWR (TER_SIGFILTER → mesma regressão); SEMPOLL/FUTEXPOLL/
  CONDPOLL (não acordam p/ safepoint); **TER_NOGCWAIT** (patcha 0x74f260→`mov w0,#0;ret`: REMOVE a
  espera do GC — confirmado, SEMWHO some — MAS a main cai num pthread_cond_wait NOVO que também
  trava: a engine precisa do stop-the-world COMPLETAR, não ser pulado).
- **⛔ DESCOBERTA-CHAVE (por que os shims NÃO resolvem):** as threads do GC (GC Finalizer, Loading.Preload)
  são **bionic-STÁTICAS** — libunity/libil2cpp **NÃO importam `pthread_create`/`clone`** (readelf: 0).
  Elas se criam via `clone` INLINE, setam a máscara via `rt_sigprocmask` INLINE (svc), e bloqueiam via
  futex INLINE — **bypassando TODOS os nossos shims** (sem/cond/futex/sigmask). Confirmado: o registro de
  threads (pthread_create_fake) mostra os alvos do GC como `?(unreg)`; hook de `rt_sigprocmask` no
  `my_syscall` = **0 hits**; desbloquear SIGPWR nos nossos waits NÃO muda o SigBlk delas (continua
  0x7ffffffe7ffbbed9). Ou seja: **não dá p/ alcançar a máscara/wait dessas threads por interposição**.
- **Becos adicionais:** GC-safe-wait (desbloquear SIGPWR em volta dos nossos sem/cond/futex waits,
  OPT-IN TER_GCSAFEWAIT) — não alcança as threads bionic-static. rt_sigprocmask filter (TER_NORTFILTER) —
  0 hits (inline). Identificador de thread por nome no [PKILL] (TER_PKLOG agora loga comm/tid).
- **BYPASS testado (TER_NOGCWAIT=1 TER_NOSUSPEND=1 + CUP_GCOFF=1):** `my_pthread_kill` ENGOLE SIGPWR/
  SIGXCPU (nenhuma thread suspende) + `WaitForThreadsToSuspend`→ret0. **PASSA do muro do GC** (SEMWHO
  some). MAS a main cai numa nova espera produtor-consumidor em **`libunity+0x2f37a4`** (fn ctor @0x2f3680,
  vtable 0xb59e48): cria uma FILA em `obj+0x58` + cond `obj+0x88` e ESPERA um item que NUNCA chega. Polls
  (COND/SEM/FUTEX) e o driver do Choreographer (TER_CHOREO) NÃO alimentam essa fila. Provável ESTADO
  CORROMPIDO do bypass (a main "vagou" pra um loop de worker que não devia rodar nela). Bypass = beco
  pouco confiável. 0 swaps/sem imagem ainda.
- **🔑 BYPASS LIMPO via FAKEACK (melhor que NOGCWAIT):** `my_pthread_kill`, ao ver SIGPWR/SIGXCPU,
  **POSTA o sem de ACK** que o WaitForThreadsToSuspend espera (= nosso sem_shim em `il2cpp+0x31666a0`)
  + engole o sinal. O GC conta o ACK e segue o **fluxo NORMAL** (suspend+restart). Env: `TER_FAKEACK=1`
  (com CUP_GCOFF). PASSA do GC. MAS cai na MESMA espera produtor-consumidor `libunity+0x2f37a4` que o
  NOGCWAIT → confirma que é o PRÓXIMO PASSO LEGÍTIMO (não corrupção). 
- **⛔ DIAGNÓSTICO FINAL (por que para aqui): DEADLOCK TOTAL.** Com FAKEACK, /proc mostra **TODAS as
  threads PARADAS** (futex_wait/nanosleep/Mali — ZERO em R). A main espera um produtor; o produtor
  espera outra thread; cascata até tudo parar. As threads que precisariam ACORDAR umas às outras
  (Job.Workers, GC Finalizer, Loading — **bionic-static**) bloqueiam em **futex INLINE (svc)**, fora
  do nosso símbolo `syscall` → os poll-wakes (SEM/FUTEX/COND) NÃO as alcançam, e os lost-wakeups do
  scheduler do Unity não dá p/ band-aidar nelas. **Conclusão honesta: a multi-threading desse jogo
  (threads bionic-static + GC cooperativo + job-system) está ALÉM do que o so-loader (interposição
  glibc) alcança — as threads bypassam a interposição em 3 níveis (clone, sigmask, futex inline).**
- **PRÓXIMO (caminhos viáveis restantes):** (0) RE de quem PRODUZ na fila `obj+0x58`/sinaliza `obj+0x88`
  (achar o produtor e por que não roda) — se for legítimo, dirigir; se corromp., o bypass é beco.
  (1) **NEUTRALIZAR o StopWorld/RestartWorld inteiro**: o
  `WaitForThreadsToSuspend`(0x74f260) NÃO tem `bl` caller (chamado indireto/inline). TER_NOGCWAIT patcha
  ele p/ `ret 0` (some o wait) mas a main cai noutro `pthread_cond_wait` (provável RestartWorld). Achar e
  patchar o PAR StopWorld+RestartWorld inteiro (RE do GC_stop_world/GC_start_world internos do bdwgc) —
  com GC desligado o scan de stack é mínimo. (2) Achar no libil2cpp ONDE a thread bionic-static seta a
  máscara que bloqueia SIGPWR e patchar esse código estático (difícil). (3) Investigar se o bdwgc tem
  env/modo de suspensão alternativo. (4) Reconsiderar a base do loader: usar um runtime que NÃO seja
  bionic-static p/ as threads do GC (improvável). Device .89.

## 🧱 (HISTÓRICO/RED-HERRING) MURO — `nativeRender` do FRAME 2 trava no CHOREOGRAPHER. (correção: é o frame 2 — `[r2>`
entra, `<r2]` nunca; frames 0,1 OK). A ÚLTIMA chamada JNI é `sendToTarget` — o engine montou o
Choreographer (FrameCallback "UnityChoreographer" + HandlerThread+Looper+Handler), postou o frame
callback e a main parou em `sh_sem_timedwait` (confirmado via gdb bt). 0 swaps (sem imagem). NÃO é
lost-wakeup (TER_FUTEXPOLL/SEMPOLL/CONDPOLL + race-fix NÃO destravam — a condição genuinamente nunca
fica pronta). Diagnóstico de threads (gdb): main em sh_sem_timedwait; 1 UnityMain em Mali
`_mali_uku_wait_for_notification` (GPU); workers/loading PARKED.

### Driver do Choreographer (WIP, OPT-IN `TER_CHOREO`) — dispara doFrame mas NÃO destrava ainda
Implementado: captura do proxy FrameCallback (flag em FindClass(Choreographer$FrameCallback) → próximo
newInterfaceProxy), Method sentinel doFrame (getName→"doFrame"), args Object[1]={Long(nanos)}
(GetArrayLength→1 idx171, GetObjectArrayElement→Long idx173, CallLongMethod/longValue→nanos), e uma
**driver-thread** (`choreo_driver_thread` em main.c) que ESPERA a captura (senão thread_attach cedo
CRASHA), faz `il2cpp_thread_attach` (via g_il2cpp_base+0x73ccb4, domain_get +0x73c860 — dlsym NÃO acha
símbolos do so_util), e chama `jni_choreo_doframe` ~60Hz. RESULTADO: doFrame DISPARA e roda igualzinho
a um Runnable que FUNCIONA (ambos só batem idx7=FromReflectedMethod e retornam limpo, sem crash) — MAS
a main continua presa. **Hipótese**: (a) a espera é o **handleMessage do Handler.Callback** (cadeia
Message→handleMessage→postFrameCallback→doFrame), não o doFrame direto — precisa dirigir o
Handler$Callback (também é proxy); OU (b) a espera é o **GPU/Mali** (não o Choreographer) e doFrame é
red-herring. Próximo: (1) achar via gdb o offset libunity do caller do sh_sem_timedwait e RE o que ele
espera (Choreographer vs GPU vs async-load); (2) se Choreographer, dirigir handleMessage; (3) testar se
a Mali `_mali_uku_wait_for_notification` está esperando um job que não completa (GPU submit travado).

## 🧱 (HISTÓRICO) MURO — nativeRender frame 2 trava (Choreographer / job-wait) — RESOLVIDO acima
- Frame 0 `[r0>`→`<r0]` OK (il2cpp+scene), frame 1 `[r1>`→`<r1]` OK (InControl/Choreographer/stopActivityIndicator),
  frame 2 `[r2>` ENTRA e **nunca** `<r2]`. Loop do driver chama nativeRender direto; trava DENTRO dele.
- Tail = só `[SEM] post/wait`: main (tid X) faz `wait count=1` num semáforo; job-threads fazem `post`.
  Clássico hang de job-completion / async scene-load OU espera do callback do Choreographer.
- Frame 2 monta `Choreographer$FrameCallback`(UnityChoreographer)+HandlerThread+Looper+`obtainMessage`/
  `sendToTarget` — nosso jni_shim trata tudo GENÉRICO (no-op) → o `doFrame` do FrameCallback **nunca
  dispara** → se o render depende do callback, trava. boot.config já tem `androidUseSwappy=0`.
- **PRÓXIMO PASSO**: (a) backtrace da thread travada (/proc/<tid>/wchan+stack, ou os offsets do wait
  em libunity) p/ saber se espera o Choreographer OU um job-group/async-load; (b) se Choreographer:
  fazer o shim DISPARAR o doFrame(frameTimeNanos) do FrameCallback (drive manual a cada frame do nosso
  loop); (c) se async-load/job: reusar os helpers do cuphead (CUP_DRAINWAIT/`wait_all`/`preload_step`/
  drivecr — ver setup ~main.c:3300-3478) — testar habilitar p/ Terraria. Capturar `shot.ppm` (TER_SHOT=N)
  assim que sair do frame 2.

## ✅ MURO ENLIGHTEN VENCIDO (2026-06-16 noite) — RAIZ: `memalign` stubado
- Crash era SIGSEGV em libunity+0x85dbe4 (ctor de GeoArray `str x8,[NULL]`); o `this` vinha de uma
  alloc do Enlighten (HLRTManager, label 9) que retornava **NULL**.
- 🔑 **RAIZ: `memalign` era a ÚNICA fn de alloc STUBADA** (todas as outras — malloc/calloc/realloc/free/
  posix_memalign — eram passthrough). libunity E libil2cpp importam `memalign`; o allocator interno do
  Enlighten usa memalign p/ memória alinhada → stub devolvia 0 → allocator real devolvia NULL → ctor
  recebe this=NULL → crash. Descoberto com hook do wrapper de alloc (0x861928) logando `mm=<singleton>`
  válido + `[STUB] memalign` antes de cada alloc.
- **FIX**: `my_memalign` (impl real via posix_memalign; alinhamento≥sizeof(void*) e pot2) + set_import/
  patch_got em libunity E libil2cpp. CONFIRMADO sozinho resolve (0 `[STUB] memalign`, sem crash, load
  completo). **LIÇÃO so-loader: garantir que TODAS as fns de alloc (incl. `memalign`/`aligned_alloc`/
  `valloc`) sejam reais — uma stubada quebra subsistemas (GI/Enlighten) longe da causa.**
- Rede de segurança opcional `TER_ENLFIX` (patch 0x861928→my_enl_alloc com fallback malloc) — default OFF.

## ✅ MUROS VENCIDOS NESTA SESSÃO (2026-06-16 tarde)
10. **🔑🔑 GUID "is empty" → RAIZ = `stat64` não redirecionado.** libunity importa **`stat64`**
    (NÃO `stat`!). O leitor de arquivos `ReadAllBytes`@0x21db60 pega o TAMANHO via
    `GetFileSize`@0x22b7c0 → `stat64(path)`. Só redirecionávamos `stat`/`lstat`, então `stat64`
    caía no passthrough glibc com o path CRU `assets/bin/Data/unity_app_guid` (não existe em
    disco) → falha → size 0 → lê 0 bytes → guid VAZIO → "re-extract" → trava todo o resource
    system → "Unable to initialize". O `open()` funcionava (redirecionado, fd válido 36B) mas o
    **size** vinha de `stat64` cru. FIX: `my_stat64`/`my_lstat64` (redirect + glibc stat64; arm64
    `struct stat`==`struct stat64`) + set_import/patch_got em libunity E libil2cpp. CONFIRMADO:
    log agora `read(fd,n=36)->36 first='9b73490b-...'`. **LIÇÃO GERAL so-loader: conferir `stat64`
    (e `fstatat`/`statx`) além de `stat` — bionic costuma importar a variante 64.**
11. **CASESENSITIVETEST abort → `/data/local/tmp` não existe (/ é squashfs RO).** O jogo cria um
    arquivo em `/data/local/tmp` p/ testar case-sensitivity; create falha → exceção C++ → (com
    `dl_iterate_phdr` STUBADO o unwinder não acha landing pad) → `std::terminate`→abort.
    FIX: redirect `/data/local/tmp` → `/tmp` (tmpfs gravável) no topo de `asset_redirect` (SEM
    access-check, é p/ criar) + `my_open` agora passa o **mode** do `O_CREAT` no branch de redirect.
12. **Instrumentação `TER_GUIDLOG`** (gated, off por padrão): hooks my_read/my_lseek64/my_fstat64/
    my_mmap64/my_fdopen logam ops no fd do guid. Foi como achei que o size vinha 0. Deixei no código.

## 🐞 PISTA p/ depois (dl_iterate_phdr)
`dl_iterate_phdr` está STUBADO (`[STUB] dl_iterate_phdr` no log) → retorna 0. Se o jogo lançar
exceção C++ real depois, o unwinder do libil2cpp/libc++ não acha o eh_frame → `std::terminate`.
Há `so_register_eh_frame`/`so_record_phdr` no main.c (p/ libunity) mas o SÍMBOLO `dl_iterate_phdr`
não está wirado como custom → libil2cpp pega o stub. Considerar implementar um `my_dl_iterate_phdr`
real (itera os PT_LOAD de libunity+libil2cpp) e set_import/patch_got — pode prevenir aborts futuros.

## 🔁 LOOP DE TRABALHO (build → deploy → test)
```sh
cd ~/nextos_ports_android/ports/terraria
./build.sh                                   # cross arm64 -> ./terraria (erros SDL2 "subsection" = warning, ignore)
ssh root@<device-ip> 'killall -9 terraria; rm -f /storage/roms/terraria/terraria'
scp terraria root@<device-ip>:/storage/roms/terraria/
ssh root@<device-ip> 'cd /storage/roms/terraria; export TER_SHOT=12 CUP_DLLOG=1; sh test.sh 60 250'
# ler: ssh ... 'cd /storage/roms/terraria; grep -aE "ALOG|CRASH|Unable|<r0]|GfxDevice|SHOT" eng.log; tail -8 eng.log'
```
- **`test.sh N F`** = SEMPRE `timeout -s KILL N` + mata leftovers (N seg, F=CUP_FRAMES).
- **CUP_DLLOG=1** liga log de open/stat redirect (`[open-redir]`, `[stat-MISS]`).
- **TER_SHOT=N** = grava `/storage/roms/terraria/shot.ppm` na N-ésima troca de buffer
  (glReadPixels). Verificar imagem: `scp shot.ppm` local → `python3 -c "from PIL import Image; Image.open('shot.ppm').save('shot.png')"` → Read shot.png.
- Env de bypass: `TER_NOSTORAGEPATCH=1` (desliga NOP storage), `TER_NOPTSHIM=1` (desliga pthread shim).

## ☠️ REGRA CRÍTICA: NUNCA rodar sem timeout
Run sem `timeout` deixa a thread `UnityMain` (detached) IMORTAL em busy-spin → pina os 4 cores
→ sshd não responde (banner timeout) → OOM não mata → **só power-cycle físico do device resolve**.
Já aconteceu (1h+ travado). SEMPRE `test.sh`. Se travar: pedir ao usuário pra religar o device.

## 🗺️ Arquitetura
- DO ZERO reusando o **plumbing do loader Unity do cuphead** (`ports/cuphead/src/*`): so_util,
  jni_shim (FalsoJNI), egl_shim, sem_shim, **pthread_fake.c**, opensles_shim. A RE de offsets
  2017.4 do cuphead foi DESLIGADA (`if (0 ...)`) — NÃO se aplica ao 2021.3.
- Imports: `gen-unity-imports.sh libunity.so libil2cpp.so` → `src/imports.gen.c` (passthrough
  via dlsym + stub log). `recon_fill_passthrough()` é chamado antes de cada `so_resolve` (main.c).
  Ao regenerar, sempre `mv src/imports_unity.gen.c src/imports.gen.c`.
- **Payloads** (`.gitignore`, BYO-data, JÁ no device): `payload/lib/*.so`, `payload/assets/assets/bin/Data/**`.
  Origem: APK `/home/root/Downloads/Terraria-v1.4.5.6.4terariaapk.com.apk` (Unity IL2CPP + PAIRIP;
  PAIRIP IGNORADO, global-metadata LIMPO `af1bb1fa`).
- **Device**: `/storage/roms/terraria/{terraria, lib*.so, bin/Data/**, userdata/}`. O `boot.config`
  no device (≠ do APK) tem `gfx-disable-mt-rendering=1`, `androidUseSwappy=0`, `gfx-enable-*gfx-jobs=0`.

## ✅ FIXES JÁ FEITOS (commitados — ordem dos muros vencidos)
1. boot.config: MT-render OFF + Swappy OFF → destravou hang do `nativeRender` frame 0.
2. **setjmp/longjmp** stub→passthrough (SIGSEGV); +sig*/gettid/prctl/newlocale; `__errno`→`__errno_location`.
3. **PAD** (Play Asset Delivery): `getAssetPackState`→`nativeStatusQueryResult(name,4,0)` COMPLETED;
   `getAssetPackPath`→"/storage/roms/terraria"; package `com.and.games505.TerrariaPaid` (era hollowknight).
4. **"Not enough storage space"**: NOP em libunity `0x2d8fac` (`tbz w0,#0,0x2d9068`; gate 0x22b7e0 retorna falso). Gated `TER_NOSTORAGEPATCH`.
5. **statfs/statfs64** interceptados (my_statfs64 mede GAMEDIR real).
6. **FORTIFY bionic** (`__memmove_chk`/`__strlen_chk`/`__vsnprintf_chk`/`__memcpy_chk`/`__strcpy_chk`/
   `__strcat_chk`/`__snprintf_chk`/`__FD_SET_chk`) stub→impl reais (heap corruption "free invalid size").
   **strlcpy/strlcat** impl reais. readlink + wide-char (swprintf/wcs*/isw*/tow*) passthrough.
7. **🔑 pthread bionic→glibc** (RAIZ do SIGBUS pós-il2cpp_init): cond/mutex/rwlock eram passthrough
   (bionic struct + glibc op = ponteiro lixo). `install_pthread_shim()`/`patch_pthread_shim()` wira
   o conjunto completo → `pthread_fake.c`. Gated `TER_NOPTSHIM`.
8. **SDK_INT**: `GetStaticIntField(SDK_INT)`→30 (era 0 → Unity abortava).
9. asset_open tira prefixo "assets/".

## 🪤 ARMADILHAS APRENDIDAS
- **objdump rotula `@plt` por heurística — pode ERRAR.** O crash "num_get" era na verdade
  `pthread_cond_wait` (confirmar símbolo pelo GOT slot via `readelf -r <offset>`).
- so_resolve só resolve UNDEF; símbolos WEAK DEFINIDOS (C++ templados) já são bindados por
  so_relocate (base+st_value) — OK.
- regex SAFE do gerador precisa cobrir bionic-isms (strlcpy, `__*_chk`, wide-char, setjmp);
  senão viram stub-0 = corrupção silenciosa. pthread mutex/cond NÃO podem ser passthrough.

## 📋 PRÓXIMOS MUROS PROVÁVEIS (depois do guid)
1. Carregar `data.unity3d` (asset pack) — pode precisar do path certo no getAssetPackPath / VFS.
2. **Criar contexto GLES2 no Mali fbdev** — Unity cria EGL via libEGL real do Mali + ANativeWindow
   (my_aw_* → g_fbdev_win). Pode ter quirks (config, força `-force-gles20` já default no cmdline).
3. 1º frame → `shot.ppm`. Quirks Mali Utgard depois (highp→mediump, FBO depth-stencil, etc.).
4. Áudio (opensles_shim/FMOD) e controle (gamepad.c) — fase final.
