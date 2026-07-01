# FF9 (Final Fantasy IX) so-loader → Mali-450 — HANDOFF

> Unity **2022.3.62f3 IL2CPP arm64**, package `com.square_enix.FFIXww.android_googleplay`.
> Metadata **PLAINTEXT** (magic af1bb1fa), **sem pairip / sem DRM** (repack APKVISION).
> Scaffold reusa **Terraria** (`ports/terraria`). Device de teste: Mali-450 / EmuELEC (mesmo do chrono).
> APK: `FINAL-FANTASY-IX-v1.5.4-full-apkvision.apk`.

---

## s17 2026-07-01 — ✅ regressão do no-skip isolada e corrigida no player externo

> Device .90. Validado em `run_noskip_audiofix2.log` com `FF9_NOSKIPMOVIE=1`.

### ✅ Fix validado
- Regressão confirmada: o baseline default (`./run.sh`, `SKIPMOVIE default ON`) continuava com menu e som OK; o problema aparecia no caminho de vídeo real (`FF9_NOSKIPMOVIE=1`).
- Causa prática no player externo: ele sempre abria `ffmpeg -vn | pacat` mesmo para MP4 sem stream de áudio. `FMV000.mp4` e `FMV001.mp4` são vídeo-only; isso criava pipeline de áudio inútil e interferia na rota HDMI/Pulse/sdlib percebida no menu.
- Patch: `ff9_extmovie_play` agora detecta `Audio:` antes de iniciar `pacat`. Para MP4 silencioso, toca só o vídeo no fbdev e preserva o sink de áudio do jogo.
- Validação final:
  - `FMV000.mp4` tocou real e terminou `status=0x0`;
  - `pacat` não ficou ativo para o MP4 silencioso;
  - sink-input Pulse do jogo seguiu ativo (`s16le 2ch 44100Hz`);
  - menu voltou após a FMV com opções visíveis na captura `/tmp/ff9_noskip_audiofix2_menu_top.png`.

### Próximo foco
- Revalidar visual/áudio no HDMI do usuário enquanto o run `run_noskip_audiofix2.log` está parado no menu.
- Se confirmado, seguir o fluxo no-skip: `down + A` -> `FMV001` -> campo, mantendo o mesmo fix de áudio opcional.

## s16 2026-07-01 — ✅ menu restaurado + achado real sobre áudio dos FMVs iniciais

> Device .90. Run atual deixado parado no menu inicial: `run_menu_restore.log`.

### ✅ Estado validado
- Binário limpo/rebuildado e redeployado depois de descartar dois experimentos não validados:
  - tentativa de misturar `EventInput.GetWorldTriggerButton` no Confirm do campo retornou zero e foi removida;
  - tentativa de alterar o player externo de FMV para outro pipeline de `pacat` foi revertida.
- Fluxo do título recuperado: `FMV000.mp4` tocou inteiro (`Play pid=8067`, `terminou status=0x0`) e retornou ao menu.
- Captura real do framebuffer confirma menu com brilho correto e `NEW GAME` visível: `/tmp/ff9_menu_restore_top.png`.
- O processo ficou vivo/renderizando no menu, sem selecionar New Game.

### Áudio dos vídeos
- `ffmpeg -i` no device mostrou que `FMV000.mp4` e `FMV001.mp4` possuem apenas stream de vídeo H.264; não existe stream de áudio nesses MP4.
- Teste isolado `ffmpeg -t 5 -i FMV000.mp4 -vn ... | pacat` falhou corretamente com `Output file does not contain any stream`.
- Conclusão: ausência de som nesses FMVs iniciais não é falha do `pacat`; o áudio, se existir no Android original, deve vir por eventos/sdlib separados do MP4. O caminho de som do jogo em si segue abrindo: `[SL] SDL audio aberto: 44100Hz 2ch 2048 samples (driver=pulseaudio)`.

### Próximo foco
- Manter o menu/fade atual intacto.
- Investigar os eventos de som/sdlib disparados durante `FMV001`/New Game, especialmente o `KeyNotFoundException` de som key `136` já visto em sessoes anteriores.
- Depois retomar gameplay no primeiro campo e descobrir o trigger correto da sala escura sem pular videos.

## s15 2026-07-01 — ✅ FMVs nativas preservadas + raiz do preto estreitada (EBG mesh/material)

> Device .90. Testes sem pular videos, seguindo fluxo: FF logo/disclaimer/title -> New Game -> `FMV001.mp4` -> field `FBG_N00_TSHP_MAP002_TH_CGR_1`.

### ✅ Avancos validados
- `FF9_TEXDUMP` adicionado como diagnostico opt-in leve de `glTexImage2D`, com modo `texdump-only` para nao ligar roteamento GL amplo nem travar o boot.
- Fluxo no-skip validado de novo: `FMV000.mp4` tocou inteira, menu recebeu `down + A`, `FMV001.mp4` tocou inteira e retornou ao field.
- Atlas real do field foi capturado: textura RGBA `2048x2048`, id 22/variante, com RGB e alpha reais. Portanto o centro preto NAO e asset faltando nem upload de textura vazio.
- `FF9_EBG_SEPARATE=1` forçou `CreateSceneCombined -> CreateScene`, mas a tela continuou com centro preto. Isso reduz a suspeita de "combine path global" simples.
- `FF9_EBG_SKIPBLACKBORDER=1` pulou `CreateBorder overlay=17 rgb=0,0,0`, mas a imagem ficou igual. O retangulo preto nao era apenas essa borda preta.
- `FF9_NOFIELDGUARD=1` agora sobrevive ate o field e gameplay: o player moveu com `right` (`bits=0x20`, pos mudou para ~`168,-1020`). Porem a captura ficou visualmente pior, sem Zidane renderizado, entao remover o guard inteiro nao e o fix.

### Estado visual / proximo foco
- O atlas contem o background real; a tela ainda mostra UI/paredes e centro escuro/preto. O proximo foco real e mesh/material/depth/shader do EBG, ou um guard mais cirurgico que chame partes nativas sem perder os modelos.
- O primeiro field do FF9 tambem e uma sala escura do roteiro; input Confirm chega no field (`bits=0x4000`), mas no ponto testado nao disparou dialogo. Ainda precisa seguir a interacao do gameplay ate acender/progredir a cena.
- Evidencias locais uteis: `/tmp/ff9_field_atlas_rgb.png`, `/tmp/ff9_field_atlas_alpha.png`, `/tmp/ff9_skipblack_top.png`, `/tmp/ff9_nofieldguard2_top.png`.

## s14 2026-07-01 — ✅ fluxo sem skip validado de novo + input de campo via EventInput

> Device .90. Validado em `run_bubbleproc_clean.log`, com `FF9_NOSKIPMOVIE=1`, `FF9_FIELDSTATE=1`, `FF9_FORCECONTROL=1`, `FF9_GPLOG=1`, `FF9_BUBBLELOG=1`, `FF9_EBGDIAG=1`.

### ✅ Avancos validados
- Rebuild/deploy do binario atual (`ports/ff9/ff9`) no device.
- `FMV000.mp4` tocou inteiro e terminou `status=0x0` no frame 3412.
- Menu inicial navegou por `/tmp/tergp`: `down` selecionou `NEW GAME`; `a` chamou `TitleUI.OnNewGameButtonClick`.
- `FMV001.mp4` tocou inteira, sem skip, e terminou `status=0x0` no frame 12134.
- Campo `FBG_N00_TSHP_MAP002_TH_CGR_1` carregou depois da FMV001; EventEngine ficou em `gMode=1 user=1 movement=1`.
- Gameplay/control confirmado: `right` moveu o player de aprox. `(25,0,-1100)` para `(168,0,-1019)` com `bits=0x20` e `moving=1`.
- Patch novo alimenta `EventInput.RecieveDialogConfirm()` em A no campo e consome `BubbleUI.currentInput` por `EventInput.ProcessBubbleInput()` quando a rota do BubbleUI existir.

### Estado visual / proximo muro
- Framebuffer `/tmp/ff9_bubbleproc_field.png`: Zidane, UI e paredes laterais aparecem; centro do field segue preto/sem background.
- Apertar `A` no estado inicial chegou a `EventInput.ReceiveInput` e `RecieveDialogConfirm`, mas `bubble=(nil)` e nao houve `BubbleUI.SetInput`; o `?` visivel no canto nao esta passando pela rota `BubbleUI` naquele ponto.
- Proximo foco real: recuperar background/iluminacao EBG do campo. Logs atuais: `overlayCount=28`, `animCount=3`, `spriteCount=0`, `combine=1`, `upscale=1`, atlas `2048x2048`; `EBG_animationInit` continua com frames de anim ausentes.

## s13 2026-07-01 — ✅ fluxo real ate gameplay controlavel pos-FMV001

> Device .90. Validado em `run_real_latch.log`, sem pular videos (`FF9_NOSKIPMOVIE=1`).

### ✅ Avancos validados
- `FMV000.mp4` e `FMV001.mp4` tocaram inteiros pelo caminho FFmpeg externo, ambos terminaram com `status=0x0` e callback gerenciado.
- `FIELDNRE` default-on patcha `EventCollision.IsNPCTalkable` em `libil2cpp+0x1122ca8`: remove o `NullReferenceException` recorrente em `0x1122cac` e preserva `player=1` no controller.
- `FF9_TITLEPAD` agora desarma depois do primeiro click no menu. Antes, apertar `A` no campo chamava `TitleUI.OnNewGameButtonClick` de novo; agora o input fica livre para gameplay.
- Campo pos-FMV001 esta controlavel: no fluxo real, segurar direita moveu o player de `x=25` para `x=168` (`bits=0x20`, `moving=1`) sem novo `TITLEPAD`.
- Framebuffer validado em `/tmp/ff9_real_latch_field.png`: Zidane e NPC aparecem no campo, UI viva, processo a 60 FPS.

### Proximo muro
- Fundo do campo ainda fica majoritariamente preto/escuro; personagens e bordas aparecem. Proximo foco e recuperar background/iluminacao do field sem perder o fluxo real de videos.
- `KeyNotFoundException` de som key `136` ainda aparece durante New Game. Continua nao fatal com `FF9_NOLOGGER=1`, mas som de campo ainda precisa de tratamento fino.

---

## s12 2026-07-01 — ✅ vídeos reais + New Game até campo renderizando personagem/UI

> Device .90. Estado validado em framebuffer real, com `FF9_NOSKIPMOVIE=1`.

### ✅ Avanços validados
- `FMV000.mp4` toca por FFmpeg externo no framebuffer inferior (`/dev/fb0` yoffset 720), com áudio via `pacat`, e ao terminar chama o callback gerenciado (`EXTMOVIE terminou ... status=0x0`) sem voltar ao preto.
- `New Game` agora passa pelo fade quebrado do título: `TitleUI.Hide(0x134384c)` foi trocado por bypass que invoca o `Action` callback direto. O NRE anterior de `TitleUI.Hide` sumiu.
- `FMV001.mp4` toca real depois de `New Game` (`Load key="FMV001.mp4"`, `Play pid=...`, `terminou status=0x0`) e retorna ao jogo.
- Pós-FMV001 chega em campo: framebuffer mostra Zidane + UI (`pause/help/Menu`) e segue vivo por milhares de frames.
- Controle virtual/TitlePad continua funcionando: `baixo + A` seleciona `NEW GAME`; no campo, segurar direção altera o estado visual/atores.

### Próximo muro
- Campo ainda fica com fundo preto. Log mostra `FF9_FIELDGUARD` no-opando `EBG_animationInit` e `EBG_charAttachOverlay`; próximo foco é trocar esse guard por implementação/patch seguro que permita background/overlays em vez de só sobreviver.
- `KeyNotFoundException` de som key `136` ainda aparece no fluxo, mas por enquanto não mata o processo com `FF9_NOLOGGER=1`.
- `NullReferenceException` em `0x1122cac` ainda aparece perto do FMV/field, mas o processo sobrevive; investigar se impacta o fundo.

---

## s11 2026-07-01 — ✅ menu inicial completo + áudio confirmado + 🎮 navegação Xbox no título

> Device .90. Estado salvo após usuário confirmar na TV: menu visível com iluminação completa e áudio audível.

### ✅ Estado atual validado
- `run.sh` abre por default o caminho do menu inicial: `FF9_FORCE_STARTGAME=1`, `FF9_LOADSCENE=Title`, `FF9_LOADAT=900`, `FF9_GAMEINIT=1`, `FF9_SHOWMENU=0`, `FF9_NEWGAME=0`, `FF9_NODIRECTNG=1`.
- `FF9_FADEDONE=1` agora é default no launcher. Foi a diferença confirmada pelo usuário para completar a iluminação/fade do título/menu no HDMI.
- Captura limpa validada: `/tmp/ff9_shot_default_fadedone_late.png` mostra `CONTINUE`, `NEW GAME`, `LOAD GAME`, `CLOUD DATA` completos.
- Áudio real foi ouvido pelo usuário durante o menu. Launcher mantém `FF9_REALAUDIO=1` e `FF9_REALSOUND=1`.
- Navegação do título resolvida no shim por `FF9_TITLEPAD` default-on quando `FF9_GAMEPAD=1`: captura `TitleUI`, mantém seleção própria `CONTINUE/NEW GAME/LOAD GAME/CLOUD DATA` e desenha sublinhado dourado visível.
- Validação no device: `/tmp/tergp down` moveu o marcador de `CONTINUE` para `NEW GAME`; log: `[FF9_TITLEPAD] selecionado NEW GAME (1)`.
- Validação do confirmar: `/tmp/tergp a` em `NEW GAME` chamou o handler real `OnNewGameButtonClick(this=...)`; processo ficou vivo após o click e entrou no próximo fluxo de load/FMV.

### 🎮 Controle Xbox / TitleUI
- Reusar o padrão dos ports Terraria/Elderand/SOTN: camada normalizada Xbox (`D-pad`, A/B/X/Y, Start/Select, shoulders/triggers) lendo SDL/evdev.
- No FF9, `UnityEngine.Input`/`GetKeyTrigger/EventInput/UIKeyTrigger` recebem o controle (`FF9GP down(11)`, `confirm(0)`), mas o TitleUI do menu NÃO polla esse caminho (`KEYPOLL=0`). O fix foi controlar o TitleUI diretamente.
- `A`/`Start` em `NEW GAME` chama `TitleUI.OnNewGameButtonClick(0x1344634)`. `LOAD GAME` tenta `UICamera.Notify(go,"OnClick")` no `loadGameButton`.
- Próximo foco: completar handlers de `CONTINUE/CLOUD DATA` se necessário e retomar o muro de gameplay/content-load após New Game.

---

## s10 2026-07-01 — ✅ launcher default chega em menu inicial + áudio real + controles virtuais/evdev

> Device .90. Commits: 853d0da, 234c206, 0574222.

### ✅ Estado atual validado
- `run.sh` agora usa por default o caminho validado: OBB real + título/menu + `FF9_REALAUDIO=1`/`FF9_REALSOUND=1` + `FF9_GAMEPAD=1`/`FF9_FORCECONTROL=1`.
- Áudio real sdlib/OpenSL/SDL confirmado no launcher default: `slCreateEngine`, `CreateOutputMix`, `CreateAudioPlayer`, `SDL audio aberto: 44100Hz 2ch`, `bq_Enqueue ... (PCM fluindo!)`.
- Menu inicial confirmado pelo framebuffer real (`/dev/fb0`, 1280x720 BGRA), não só por `shot.ppm`.
- Input virtual default: `TER_GPVIRT=1`, comando via `/tmp/tergp` (`right`, `left`, etc.) é consumido por `TGPV`, mas ainda não navega o TitleUI/NGUI.
- Input físico ampliado: `TER_GP_EVDEV=1` no launcher; a camada de input abre `/dev/input/event0..15` e mapeia teclado/CEC/IR (`setas`, `OK/Enter/Espaço`, `Voltar/Esc`, `Menu`, `Select`, `PlayPause`) para o mesmo `g_gp_log` usado pelo FF9.
- No device atual não existe `/dev/input/js0`; SDL reporta `SDL_NumJoysticks=0`. O caminho evdev é essencial para remoto/teclado nesse hardware.

### Arquivos úteis de evidência local
- `/home/nextos/ff9_virtual_compare.png`: comparação antes/depois de input virtual.
- `/home/nextos/ff9_fb0_after_right.png`: captura real do framebuffer.
- `/home/nextos/ff9_fb0_after_right_bright.png`: mesma captura clareada para visualizar melhor o campo escuro.

### Próximo foco sugerido
- Testar os botões físicos reais do controle/remote no device com `TER_GPEVLOG=1` para calibrar códigos extras, se algum botão vier diferente.
- Depois reduzir ruído de log (`CallObjectMethod(getDevices)`) e manter só logs de diagnóstico opt-in.

---

## s8 2026-06-30 — 🐛 New Game addr fix + ✅ DISCLAIMER RENDERIZA COMPLETO (FASTTWEEN) + muros mapeados

> Device .90. Commits: a025dc5, 38656dd, efa922f, 55d7f10, 0cbf5e9, 9e09a37, 0a49d17, 054732c.

### ✅✅ s8 cont3 — DISCLAIMER RENDERIZA BRANCO/COMPLETO (dica do usuário foi a virada)
- **`FF9_FASTTWEEN` (default-on, commit 9e09a37):** os fades NGUI (UITweener/TweenAlpha) avançam por `Time.deltaTime` que é ~0 no so-loader → o tween engatinha e o disclaimer fica apagado/travado. FIX: hook `UITweener.Update` (mk_tramp no prólogo REAL **0x1276808**; 0x1276804 é stub `b`), acelera `mFactor`(+0x78) na direção de `mAmountPerDelta`(+0x74). **Disclaimer agora renderiza BRANCO FORTE/completo** (usuário confirmou).
- 🔴 **MAS não avança disclaimer→logo→menu.** Tentado sem sucesso: `FF9_FIXDT` (Time.get_deltaTime 0x2567384→1/60; deltaTime provavelmente inlined), `FF9_NOTIFYCLICK` (UICamera.Notify SlideShowHitArea OnClick/OnPress), `FF9_VSYNC_WAIT`+`FF9_VSYNCTICK` (não avança+piora boot), `FF9_STOPSPLASH` (apaga preto), `FF9_TWEENNODELAY` (delay-zero, não-confirmado→opt-in).
- ⚠️ **ES MASCARADO no .90** (`systemctl mask emustation`) — desenhava o menu PORTS por cima do FF9. **DESMASCARAR depois** (`systemctl unmask emustation`).
- 🎯 **PRÓXIMO:** avanço trava num HOLD (provável WaitForSeconds lendo Time.time interno do libunity ~0 — fix de frame-time do player-loop, ligado ao vsync/present) OU tap NGUI via `UICamera.GetInputTouch` delegate custom (+0x220) não-alimentado. NÃO mexer no FASTTWEEN default.

### ✅ s8 cont1/2 — bugfix New Game + menu natural
> **DIAGNÓSTICO (Alt-D) + bugfixes.** Trabalho non-stop, device .90.

### ✅ Achados/fixes desta sessão
1. **🐛 BUG: `OnNewGameButtonClick` estava sendo chamado em `0x134464c` (ERRADO).** O RVA correto do dump il2cpp é **`0x1344634`**. A "correção" de uma sessão anterior (0x1344634→0x134464c) caía no **MEIO da função** (pulava o prólogo → x0 lixo → `blr` num ponteiro lixo → SIGSEGV). **Corrigido** (main.c, render loop FF9_NEWGAME). Agora o New Game **EXECUTA de verdade**.
2. **`currentActivity` NÃO é o bloqueio do New Game** (refutado). Com `FF9_ACTTRACE=1` (loga todo `Call*Method` em `g_current_activity`): **ACTCALL=0 até o crash** — o New Game crashava ANTES de tocar em currentActivity. Hipótese "muro currentActivity" estava errada.
3. **Onde o New Game crasha AGORA (muro real):** `OnNewGameButtonClick(0x1344634)` → `TitleUI.Hide(0x134384c)` → carrega cena/assets do gameplay → **crash em libc (strlen/memcpy, ponteiro lixo 0x160f940002b)** chamado de `libil2cpp+0x10a629c`. As funcs 0x10a5xxx-0x10bd (crash) **NÃO estão no script.json = runtime/gerado do il2cpp (AssetBundle nativo / Burst)**. Última atividade: abre `bin/Data/boot.config` OK, `getAssetPackState(UnityStreamingAssetsPack)=COMPLETED`, depois **dlopen repetido de `libsdlib_android.so` (×9)** + `libFF9SpecialEffectPlugin.so` + `lib_burst_generated.so` (todos `[GOT] dlopen: 0 slots nao achado`). **Plugins ESTÃO deployados** (não é arquivo faltando) — crash é o pipeline nativo de carregar a cena (asset bundle + plugins SiliconStudio/Burst). = muro multi-sessão de CONTEÚDO/plugin.

### 🟢 FLUXO NATURAL renderiza o DISCLAIMER (imagem confirmada, inglês)
- Receita: `FF9_OBB=1 FF9_EMPTYPKG=1 FF9_OBBINIT=1 FF9_SPLASHDONE=1` (sem force) → boot saudável render 6000+, 0 crash, OBBs reais resolvidos (main.obb/patch.obb), EGLFIX 15 configs, só 2 NRE → **tela "Health and Safety Notice" RENDERIZA** (scratchpad/ff9_t4.png).
- **Disclaimer = `TitleUI.SlashScreen : TitleUI.SlideShow`** (campo `spashText` UILabel; `Play(0x1347C9C)`). Avança por **tap em `SlideShowHitArea`** (NGUI UICamera) OU timer. **NÃO poll GetKeyTrigger (KEYPOLL=0), NÃO poll GetAnyButtonDown (`AnyPlayerInput.GetAnyButtonDown`/0xCC2854 NEM existe no FF9 — CUP_TAPINPUT é resíduo do Cuphead), FF9_TAP (Unity Input) não dispara** (rota NGUI). Fica preso (fade dim, ~estático).
- **`FF9_NOLOGIN` (Social.ProcessAuthentication, agora contínuo N/EVERY/STATE) NÃO destrava** — Play Games re-ativa o auth independente. Disclaimer NÃO é gated pelo login (já está na tela).
- 🎯 **PRÓXIMO p/ fluxo natural:** dispensar o `SlashScreen`. Opções: (a) NGUI click programático no `SlideShowHitArea`; (b) callback `postSplashScreenFadeOut` (`TitleUI.SlashScreen.<>c__DisplayClass2_0.<Play>b__0` @0x134DDA8, precisa do DisplayClass `this`); (c) setar `TitleUI.SplashScreenEnabled=false` (+0x248) ANTES do Show p/ PULAR o splash; (d) dirigir `TitleUI.PlaySplashScreen` + o método pós-splash. Dump em scratchpad/ff9dump.

### 🧭 Veredito de escopo (s8) — 2 caminhos pro gameplay, ambos com muro restante
- **A. New Game forçado** (título forçado + OnNewGameButtonClick corrigido): chega no **pipeline nativo de assets do gameplay** → crash runtime. Precisa shim plugins nativos + conteúdo da cena.
- **B. Fluxo natural** (disclaimer renderiza): precisa **dispensar o slideshow** → logo → título natural → New Game natural (singletons init OK). Mais limpo; slideshow-advance é RE de NGUI.
Engine (deadlocks/render/OBB/EGL) FEITO. Resta: dispensar slideshow (B) OU shim plugins+conteúdo (A).

---

## s7 2026-06-25 — 🎮 INPUT INJECTION p/ menu/gameplay; X5M OBSOLETO (título JÁ renderiza sem X5M)

> ⚠️ As seções s2/s3 abaixo estão DESATUALIZADAS no veredito X5M — o **título renderiza** (logo + CONTINUE/NEW GAME/LOAD/CLOUD) via **OBBs REAIS** (fluxo natural) no Mali-450, SEM X5M.
>
> **Fluxo natural OK:** OBBs reais (`userdata/main.obb` 1.05GB + `userdata/patch.obb` 707MB) validados como ZipArchive → disclaimer (Health/Safety, inglês) → logo FF9 → menu de título. NRE=0, 0 crash. Defaults: FF9_EMPTYPKG=1 FF9_OBBINIT=1 FF9_SPLASHDONE=1 FF9_OBB=1.
>
> **🔑 INPUT (s7):** disclaimer/menu esperam input via `AndroidEventInputManager.GetKeyTrigger(Control)` (il2cpp 0x12BF390). `nativeInjectEvent` é beco. Hook `my_GetKeyTrigger` retorna true p/ `g_inject_ctrl` (Control: Confirm0 Cancel1 Menu2 Up10 Down11 Left12 Right13). Driver no render loop: `FF9_AUTOCONFIRM=N`=Confirm a cada N frames; `FF9_INPUTSEQ="ctrl@frame,..."` p/ navegação precisa.
> - 🚨 **LIÇÃO "se voce pula voce trava":** hookar GetKeyTrigger **NO INIT TRAVA O BOOT** (confirmado: sem hook boot OK try1; com hook no init 4/4 hung). FIX = instalação **LAZY** do hook no render loop (`FF9_INJHOOKAT`, default f>=180) — boot roda o fluxo natural intocado, hook entra só após estabilizar.
> - Boot do fluxo natural é LENTO (validação ZIP do OBB 1GB) + ~não-determinístico: harness de retry deve dar >=120s p/ render>=300 e só tratar como hang real se render ficar 0 por >40s.
>
> **✅ s7 RESULTADO (infra de input FUNCIONA):** com lazy-hook + mprotect, boot estável até render 10860, NRE=0, 0 crash, hook instala @f=180 OK, `[INJECT] GetKeyTrigger` dispara. `FF9_AUTOCONFIRM=120` + `FF9_INJHOOKAT=180`.
> - 🔴 **MAS o disclaimer (Health and Safety Notice) NÃO é dispensado por GetKeyTrigger(Confirm=0):** captura +40s e +90s = disclaimer ainda na tela (fade-in completa, fica mais claro mas não avança). `inj=1` (só 1 match) = a tela do disclaimer NÃO faz poll de GetKeyTrigger(Confirm) repetido → Confirm não é o input dele.
> - 🎯 **PRÓXIMO PASSO (retomar aqui):** descobrir o que dispensa o disclaimer. Hipóteses: (a) **timer/auto-dismiss** (esperar +Ns sem input); (b) **tap/touch** (não botão) — disclaimer mobile costuma fechar em qualquer toque; (c) **outro Control** (testar Cancel=1, Menu=2, ou varrer 0..14 via FF9_INPUTSEQ); (d) o disclaimer É um logo/vídeo sequencial e avança sozinho mas LENTO. Logar QUAIS Controls o disclaimer realmente pollar (instrumentar my_GetKeyTrigger p/ logar TODO key chamado, não só o injetado) revela o input certo. Captura: m4_40/m4_90.png em scratch.
> - 🔑 Binário bom = `ff9.INJECT-INFRA-OK` (local + device).

## ESTADO ATUAL (s2 2026-06-24) — 🟢 RENDER LOOP RODANDO 200+ frames, eglSwapBuffers PRESENTANDO; resta LVL/currentActivity p/ conteúdo

**VIRADA s2:** a "tela preta" do s1 NÃO era job-system (diagnóstico s1 ERRADO). Eram **2 deadlocks em série no setup gráfico** + 1 crash, todos resolvidos. Agora o **render loop roda contínuo (200+ frames, 0 crash)** e o **fb0 é presentado** (`00 00 00 ff` = clear opaco, eglSwapBuffers funciona); shaders compilam ("Shader Skybox/Procedural"). O conteúdo do jogo ainda NÃO desenha porque o **C# do jogo lança NullReferenceException TODA FRAME** (3455×) acessando `UnityPlayer.currentActivity` — é o **Google Play Licensing (LVL)** + uso per-frame da Activity (trace: `GetStaticFieldID(BASE64_PUBLIC_KEY)` + `SALT` + `currentActivity` → NRE). Nosso `currentActivity` fake não é uma Activity funcional → NRE → Update() aborta antes de desenhar a cena.

### 4 FIXES s2 (todos no binário; CHOREO_NOWAIT/VSYNC_NOWAIT/NULLGUARD default-ON; polls no run.sh)
1. **CHOREO_NOWAIT** (main.c, patch libunity+0x61efe0 → `b 0x61f000`): o setup do **UnityChoreographer** (0x61eebc) cria um *promise* (obj+88, flag em `*promise`) e BLOQUEIA em `pthread_cond_wait` (loop 0x61efe0) até o pump nativo (0x61f180) marcar o flag + broadcast no cond (obj+136). Esse pump roda na thread "UnityChoreographer" (HandlerThread) que NÃO existe no so-loader (`NewObject(HandlerThread)`=NULL) → promise nunca preenchido → deadlock. O wait é barreira PURA (pós-wait 0x61f000 só faz unlock+ret, não lê o resultado) → bypass seguro. Provado por gdb: setar `*promise=1` destrava a main pro render loop. (string "UnityChoreographer" em rodata 0x139b38 confirma a função.)
2. **VSYNC_NOWAIT** (patch libunity+0x61c5f8 `b.ge`→`b`): 2º muro (frame 3) = frame-pacing (0x5f5150) ESPERA um contador GLOBAL de vsync (g_unity_base+**0x10785e8**) alcançar alvo tempo×refresh via waiter 0x61c5cc (loop 0x61c5f0). Contador é incrementado pelo pump nativo do Choreographer (0x61f2d4) que não existe → nunca anda → deadlock. Retorno do waiter NÃO é usado no delta → bypass seguro (roda sem vsync-lock). [struct@0x1078590: mutex+0, cond+0x28(=0x10785b8), counter+0x58(=0x10785e8)]. ⚠️ Tentei INJETAR o contador da driver-thread (ff9_vsync_tick, FF9_VSYNCTICK) mas omitir o store em +0x5f0 deixava estado stale → null-deref; bypass é mais limpo.
3. **NULLGUARD** (trampolim em libunity+0x439864): 3º muro = SIGSEGV em `ldr w0,[x0,#300]` (getter thunk `…; b 0x460320`) com x0=NULL. Caller 0x55fda8 passa NULL quando o feature-flag checker **0x4e723c** (lê bit em `[obj + idx*4 + 0x160]`) retorna false no nosso env → objeto não criado (0x453368 pulado). Provável feature de render ES3/URP ausente no Mali-450 ES2. Trampolim: `cbz x0`→retorna 0, senão load original + tail-call 0x460320. → transformou o crash num render loop estável de 200+ frames.
4. **Poll-defenses** (run.sh: `CUP_CONDPOLL=100 CUP_SEMPOLL=50 TER_FUTEXPOLL=100`): o boot era NÃO-determinístico (travava aleatório no frame 0/1/3 — corrida job/sem, lost-wakeup). Os polls (timedwait curto → re-checa predicado) tornam o boot CONSISTENTE até o render loop.

### s3 DESCOBERTA-CHAVE (2026-06-24) — o conteúdo preto = DADOS OBB FALTANDO (1.8GB) + shader ES2

Investiguei a NRE per-frame com **dump il2cpp** (Il2CppDumper via `~/.dotnet/dotnet`, metadata v31 plaintext; dump em scratch). Stack-scan (RASCAN no jni_shim, FF9_RALOG) mapeou a cadeia: **`ExpansionVerifier.Start` → `GooglePlayDownloader.GetExpansionFilePath` → `populateOBBData`** usa `UnityPlayer.currentActivity` (getObbDir/packageName) → NRE per-frame (Activity fake inválida) → `Update()` aborta antes de desenhar.

🔑 **FIX da NRE (FUNCIONOU, NRE 3453→1):** patch `GooglePlayDownloader.RunningOnAndroid` (RVA **0x10DA428**) → `mov w0,#0; ret` (FF9_NOOBB default-ON em main.c, via `g_il2cpp_base+0x10DA428`). MAS tela continua PRETA (fb0 = `0xff000000` uniforme, glReadpixels/dd confirmam) porque:

🚨 **`bin/Data` no device tem só 53MB — FF9 são ~1.8GB.** O conteúdo REAL (campos/modelos) está em **2 OBBs DENTRO do APK** que NUNCA extraímos: `assets/main.67.com.square_enix.FFIXww.android_googleplay.obb` (1.1GB) + `patch.67.*.obb` (741MB). São **ZIPs** de `assets/bin/Data/<hash>` (asset bundles addressable). Extraídos p/ `ports/ff9/payload/obb/`. O `RunningOnAndroid=false` PULA a verificação do OBB → sem dados → preto. Path format C#: `{getObbDir}/main.67.{pkg}.obb`.
- ⚠️ Tb apareceu **GLSL link error ES2**: `Uniform '_WorldSpaceLightPos0' differ on precision` (Skybox/Procedural) = o risco ES3→ES2 que o STUDY previu.

➡️ **Para conteúdo na tela faltam (workstream grande):** (1) **provisionar 1.8GB OBB** no device + (2) **wire do loader**: ou prover `currentActivity` FUNCIONAL (getObbDir/getPackageManager/versionCode — emulação Android pesada; o wrap AndroidJavaObject do nosso jobject fake JÁ dá NRE antes de chamar método) OU patchar `GetExpansionFilePath`(0x10D726C)+`populateOBBData`(0x10DDAF4) p/ hardcodar dir+versão(67)+pkg e ler o OBB do disco; + (3) **shader ES2** (shim Mina ES3→ES2 OU X5M ES3-nativo); + (4) **RAM** 1.8GB vs 832MB. 🧭 **Isto é EXATAMENTE o cenário X5M que o STUDY recomendou p/ FF9** (ES3 nativo + RAM folgada). Dump il2cpp + OBBs extraídos prontos p/ a próxima sessão.

### s3+ PIPELINE DE ASSET MAPEADO (via dump) — como o jogo REALMENTE carrega conteúdo
- `ExpansionVerifier.Update` (0x10D7BE0) roda uma máquina de estados (switch em currentState `[this+0x44]`); `ValidateExpansion`(0x10D8580) progride o estado e, no estado final, `Update` chama **`StartGame`(0x10DA368)** (call site 0x10d7f1c, gated pelo switch — forçar exige setar o estado).
- **`StartGame` → `AssetManagerForObb.Initialize`(0x14B8FFC)** + carrega cena via 0x2570884 (gated por bool `[this+0x41]`).
- 🔑 **`AssetManagerForObb`** (dump linha 5449) é o pipeline custom: campos `IsUseOBB`(bool), `obbAssetBundle`(AssetBundle), `fullPath`; consts `AbFileName="aobb.bin"`, `AbCacheDir="abcache"`. `Load<T>(name)`(0x15EF964) carrega asset por nome. `ExtractEntryFromObbSync(obbPath,entryInZip,dstOnDisk)`(0x14B9794) extrai do OBB-ZIP p/ disco (abcache). `GetAssetBundleFilePath`(0x14B9394)/`GetAssetBundleFolderPath`(0x14B9D8C) resolvem o path.
- ⇒ o jogo lê assets via **AssetBundle `aobb.bin`** (provavelmente catálogo/manifest dentro do OBB) → extrai p/ `abcache` → Load por nome. **Arquivos soltos `<hash>` no bin/Data provavelmente NÃO são lidos direto** — vão pelo OBB/aobb.bin. ⚠️ Re-validar: provisionar os **.obb** (não só os soltos) + fazer `GetAssetBundleFilePath`/`IsUseOBB` apontarem certo, OU achar onde `aobb.bin` e descobrir se `IsUseOBB=false` lê de disco. (2.27GB de soltos extraídos p/ `~/ff9-obb-stage` + deploy iniciado em `/storage/roms/ff9/bin/Data`; transfer ~1MB/s lento.) Dump il2cpp completo em scratch p/ continuar.
- 🧭 **Veredito de escopo:** o conteúdo na tela do FF9 é um esforço MULTI-SESSÃO (pipeline OBB/aobb.bin + 1.8GB + currentActivity/path + RAM 832MB + shader ES2→ES3). = o caso **X5M** do STUDY. O trabalho de ENGINE (deadlocks/render) está FEITO; o que resta é dado+pipeline+device.

### s3++ FORCE_STARTGAME funciona; falta DADO (aobb.bin 741MB + hashes) — confirmado por teste
- **`FF9_FORCE_STARTGAME`** (patch ValidateExpansion 0x10D8580 → `currentState=8; ret`, jump table 0x67d9c4 entry[7]=0x9a→StartGame) **FUNCIONA**: NRE=0 (verifier não loopa mais, vai p/ StartGame). Mas SEM os dados, StartGame→AssetManagerForObb.Initialize bloqueia (spin infinito `[STUB] sigsuspend`, só 2 frames, fb=0).
- 🔑 **`aobb.bin` = 741MB** (= conteúdo do patch.obb como AssetBundle master; estava na RAIZ do OBB, extraído p/ `~/ff9-obb-stage/aobb.bin`). `AssetManagerForObb.Load<T>(name)` carrega dele via `GetStreamingAssetsPath()`/`abcache`. As 24552 `<hash>` (de main.obb, ~1.5GB) são bundles addressable adicionais (conteúdo de campo). unity_obb_guid tb na raiz.
- ⇒ p/ o título renderizar: deployar `aobb.bin` (741MB) onde `AssetManagerForObb` abre (StreamingAssets → nosso AAssetManager redirect; achar o path exato em GetAssetBundleFolderPath 0x14B9D8C) + FF9_FORCE_STARTGAME + resolver o hang do thread de asset-load. Depois os hashes p/ gameplay. Transfer ~1MB/s (link lento): aobb.bin ~12min, tudo ~50min. Cenas (level1-29/sharedassets1-29, 7.4MB) JÁ deployadas em bin/Data.
- ⚠️ **+RAM 832MB vs ~2.2GB de assets** e **shader ES2 link errors** (`_WorldSpaceLightPos0 precision`) seguem como muros. **= cenário X5M do STUDY.** Patches engine (CHOREO_NOWAIT/VSYNC_NOWAIT/NULLGUARD/NOOBB default-ON; FORCE_STARTGAME opt-in) prontos. Dump il2cpp + dados extraídos em scratch/`~/ff9-obb-stage`.

### s6 FINAL5 (2026-06-24, PAUSADO pelo NextOS) — 🏆 TÍTULO RENDERIZA; fluxo NATURAL em andamento via OBBs ZIP reais.
**TÍTULO OK (forçado):** `FF9_FORCE_STARTGAME=1 FF9_LOADSCENE=Title FF9_LOADAT=900 FF9_GAMEINIT=1 FF9_EMPTYPKG=1 FF9_OBBINIT=1` (+ todos os defaults) → tela de título completa EM INGLÊS (logo + CONTINUE/NEW GAME/LOAD GAME/CLOUD DATA), 0 crash. ⚠️o relógio que aparece = artefato de FORÇAR a cena (pula o setup). Binário que funciona: `ff9.TITLE-OK-BACKUP` (local) / `ff9.TITLE-OK` (device).
**🔑 O "branco" era BUG DE REDIRECT:** `asset_redirect` (main.c) só checava o hash FLAT `bin/Data/<hash>`; os hashes shardados (bin/Data/<xx>/<hash>) não eram achados. FIX = checar o shard no asset_redirect. Com isso o título renderiza (68196 cores).
**FLUXO NATURAL (lição do NextOS: não pular, seguir o nativo) — EM ANDAMENTO:** o fluxo natural (sem force) TRAVA no ExpansionVerifier. Decodifiquei `ValidateExpansion` (0x10D8580): ele faz `GetExpansionFilePath`+`GetMainOBBPath`(0x10dabfc)+`GetPatchOBBPath` → `File.OpenRead` → **`ZipArchive`+get_Entries+GetEntry** = ABRE OS OBBs COMO ZIP p/ validar. Eu só tinha deployado os CONTEÚDOS extraídos (hashes/aobb.bin), NUNCA os .obb ZIP reais → ZipArchive falha → trava (dica do NextOS "data incompleta" = exato). FIX (feito, falta testar): extraí main.obb(1.05GB)+patch.obb(707MB) do APK (ambos PK/ZIP) → deployados em `/storage/roms/ff9/userdata/{main,patch}.obb`; código diferenciado: 0x10d726c+0x10dabfc→`my_main_obb_path`(main.obb), 0x10d865c→`my_patch_obb_path`(patch.obb) (FF9_MAINOBB/FF9_PATCHOBB env; FF9_OBBLEGACY=1 volta ao modo abcache forçado). **PRÓXIMO AO VOLTAR: rodar `/tmp/ff9realobb.sh` (fluxo natural FF9_OBB=1, sem force) → ver se o verifier valida os OBBs ZIP e progride sozinho até logos/título/menu. Se sim, matar o force.** Teste interrompido pelo NextOS antes de rodar.
**OUTROS PENDENTES:** ~3496 NRE não-fatais no modo forçado (uns assets); boot ~30% confiável (bin/Data gigante → scan lento; harness retry-boot até render≥800); /tmp do device é tmpfs 416MB (logs/captures em /storage/roms/ff9/_t/). main.obb tem TODOS os hashes (já shardados em bin/Data); patch.obb tem o aobb.bin (master, descomprimido em userdata/abcache).

### s6 FINAL2 (2026-06-24) — 🟢 IMAGEM ESTÁVEL + JOGO RODANDO AO VIVO (relógio 21:00→21:34). Falta só centro-branco cosmético.
**Com addressables COMPLETOS (24552 hashes shardados p/ vencer FAT32) o título renderiza ESTÁVEL: relógio play-time AO VIVO + painéis laterais texturizados + texto, 0 crash** (antes crashava ~render2400). FIX FAT32: shard `bin/Data/<xx>/<hash>` + redirect em my_open/my_stat/my_stat64/my_access (helper `shard_alt`). ⚠️ deixei ~13000 flat no root (move on-device é O(n²) em FAT, abortado; redirect cobre flat+shard).
**❌ CENTRO BRANCO persiste (cosmético) — testado SEM sucesso:** addressables completos (shard-opens=0, não mudou), force tardio FF9_LOADAT=2500 (NRE=42 igual), cena "MainMenu" (reinicia o app, render→0). ⇒ o branco NÃO é dado faltando — é a cena Title FORÇADA sem o setup normal de título (o trigger natural boot→título não dispara; forçar pula a init dos sprites do título → painel UI fullscreen com sprite null = branco + 42 NRE one-time não-fatais). Próximo p/ título 100% limpo: achar/disparar o trigger natural boot→título (SceneDirector.Replace pós-verifier) OU emular o setup que popula os sprites do título. ❌ TAMBÉM testado SEM sucesso: `SceneDirector.ReplaceNow("Title")` (0x1486cd0, FF9_USEDIRECTOR=1, transição gerenciada c/ fade) → branco IGUAL (79.8%, NRE=42) ⇒ NÃO é fade preso, é sprite NULL mesmo (a UI Image fullscreen do título tem sprite null → branco; as 42 NRE são esses assets do título não-resolvidos). shard-opens=0 = o jogo NÃO abre os hash p/ esses sprites (resolve via bundle master OU catálogo addressable que não acha). = RE profundo da resolução addressable do título (qual asset/catálogo, por que null). O gatilho Java do StartGame (CallStaticVoidMethod mid=...bbb0 repetido + System.load) não é claramente o trigger de cena. ❌ Cena "Loading" forçada (ReplaceNow) = MESMO branco (79.8%, NRE39). ⇒ ~10 abordagens testadas (cenas Title/MainMenu/Loading × LoadScene-cru/ReplaceNow × timing 400/600/2500 × dados parciais/completos) TODAS dão o branco-de-sprite-null idêntico. RAIZ = sistema **Addressables não-inicializado** ao forçar cena (o boot natural que chama Addressables.InitializeAsync + carrega o catálogo não roda; nosso force pula). PRÓXIMO (RE novo): achar+chamar Addressables.InitializeAsync (async, esperar handle) + garantir catálogo carregado ANTES de forçar a cena; OU descobrir/disparar o gatilho real do boot natural. Boot ~30% confiável atrapalha iteração. ⚠️ /tmp do device é tmpfs 416MB — logs/captures grandes ENCHEM (scp falha); usar /storage/roms/ff9/_t/. ⚠️ boot ~2-3/8 (bin/Data gigante = scan lento).

### s6 FINAL (2026-06-24) — 🟢🟢🟢 IMAGEM! O port RENDERIZA FF9 (texto "21:00" + painéis texturizados). "0 draws" era ARTEFATO do contador.
**VIRADA: NÃO há muro de render.** Com `FF9_FORCE_STARTGAME=1 FF9_LOADSCENE=Title FF9_LOADAT=400 FF9_OBBINIT=1 FF9_EMPTYPKG=1` + addressables deployados, o fb foi de 13 cores (preto/pillarbox) p/ **1211 cores**: texto laranja "21:00" (play-time) + painéis laterais texturizados escuros renderizam. O `CUP_DRAWCOUNT maxdraws=0` era FALSO — a render thread (GfxDeviceWorker) resolve glDrawElements fora dos meus hooks → não conta, mas DESENHA. A imagem prova a pipeline ES2 OK.
**O que faltava p/ a 1ª imagem (tudo resolvido):** (1) EGL ES2 forçado, (2) carregar CENA DE CONTEÚDO via force LoadScene("Title") (boot scene só tem spinner nativo, 0 UI Unity; o trigger natural boot→título não dispara), (3) addressables presentes.
**🟡 2 pendências p/ TÍTULO LIMPO E ESTÁVEL:** (a) **centro branco** = sprite de fundo addressable FALTANDO; (b) **crash/restart** (~render 2400, NRE acumulando) = mais addressables faltando. CAUSA RAIZ = só 16339/24610 hashes couberam (muro FAT32: 1 dir vfat não cabe >~16400 arquivos LFN de 32-hex, ~65536 entradas; ext4 /storage só 995MB). **FIX = shardar os hashes em subdirs (bin/Data/<xx>/<hash>, 256 dirs ~96 arq cada) + redirect no my_open (já hookado) flat→shard** → cabe tudo no vfat de 45GB. Depois: re-testar título limpo + estável.

### s6 (2026-06-24) — 🟢 OBB CARREGA + SHADERS LINKAM + EGL ES2 FORÇADO (detalhe).
**Cadeia toda destravada nesta sessão — falta só o render produzir draws.**

**1. OBB pipeline COMPLETO:** `aobb.bin` (741MB no APK) é **gzip(UnityFS)**. O jogo espera o bundle DESCOMPRIMIDO em `/storage/roms/ff9/userdata/abcache/aobb.bin`. Descomprimido na mão no device (`gzip -dc` → 2.06GB UnityFS). `AssetManagerForObb.Initialize`: gate `String.IsNullOrEmpty(path)==false` E `File.Exists(path)==true` (path = GetPatchOBBPath, override `my_obb_path` default = abcache/aobb.bin) → chega em `GetAssetBundleFilePath` (usa SAPATH) → LoadFromFile OK, **loading indicator START→STOP (carga completa)**.

**2. Libs nativas P/Invoke (senão EntryPointNotFoundException no boot):** `so_load` de `libsdlib_android.so`/`libFF9SpecialEffectPlugin.so`/`lib_burst_generated.so` (helper `load_ff9_aux_lib`, default ON). il2cpp resolve via JNI `ClassLoader.findLibrary(name)` → jni_shim devolve o PATH real → `my_dlopen` casa "sdlib"/"SpecialEffect"/"burst" → sentinel → `my_dlsym` resolve no módulo. ⚠️ `SdSoundSystem_Create` REAL chama slCreateEngine→deref método null do OpenSL→SIGSEGV → **`sd_stub` devolve 1** p/ toda `Sd*` (áudio mudo, sem crash). FF9_REALAUDIO usa reais.

**3. Shaders ES3→ES2 (link error "Uniform '_WorldSpaceLightPos0' differ on precision"):** Unity é ES3 (vertex highp default, fragment mediump). `my_glShaderSource` (FF9_SHFIX default ON) normaliza TODA precisão p/ uma só (FF9_SHPREC=highp default), trocando tokens mediump/lowp/highp. Roteado por DOIS caminhos: `my_dlsym` (libGLESv2 é RTLD_GLOBAL→Unity resolve glShaderSource via dlsym) E `g_gl_proc_router=ds_route` no `egl_shim_GetProcAddress`. **8 shaders normalizados, 0 link errors.**

**4. 🔑 EGL ES2 FORÇADO (era a TELA PRETA original do s1 "eglSwapBuffers=0"):** Unity 2022 pede **ES3** (`EGL_RENDERABLE_TYPE=0x40=ES3_BIT`, `EGL_CONTEXT_CLIENT_VERSION=3`); Mali-450 Utgard é **ES2-only** → `eglChooseConfig` REAL dava `EGL_BAD_ATTRIBUTE` → GfxDevice NULL-renderer → 0 GL → preto. FIX (FF9_EGLFIX, fbdev, default ON, patch_got na libunity): `my_eglChooseConfig` (RENDERABLE_TYPE/CONFORMANT 0x40→0x04, dropa attribs ≥0x3088) + `my_eglCreateContext` (CLIENT_VERSION→2). **Agora: eglChooseConfig r=1 n=15, eglCreateWindowSurface→0x20000001, eglCreateContext→0x40000001, eglMakeCurrent→1 (EGL_SUCCESS), bad_attr=0.** EGL 100% funcional em ES2.

**MECANISMO DE CENA (achado s6):** transições de cena = `SceneDirector` (PersistenSingleton) `Replace(name)`/`ReplaceNow(name)` (RVA 0x1486F58/0x1486CD0); cena título = `TitleUI : UIScene`. `ExpansionVerifier.StartGame`(0x10DA368) só chama `AssetManagerForObb.Initialize` + um `AndroidJavaObject.Call` (Java, no-op no shim) + lê o OBB (File.Open/BinaryReader) — **NÃO faz LoadScene**. `Update` do verifier espera `SplashScreen.get_isFinished`(0x2573820, patch FF9_SPLASHDONE→true). ❌ TESTADO sem sucesso (0 draws, sem load de título): FORCE_STARTGAME, fluxo natural (sem force), FF9_SPLASHDONE, single-thread (gfx-jobs=0). ⇒ o trigger boot→título NÃO dispara (provável: o `AndroidJavaObject.Call` no-opado do StartGame era quem iniciava, OU um boot MonoBehaviour espera flag que não setamos). PRÓXIMO LEAD: forçar `SceneDirector.ReplaceNow("<titleScene>")` via il2cpp_runtime_invoke (achar nome da cena título) OU RE do que o Call do StartGame deveria fazer.

**TESTADO p/ forçar render (TODOS = 0 draws):** FF9_FORCE_STARTGAME, fluxo natural, FF9_SPLASHDONE (SplashScreen.isFinished→true 0x2573820), single-thread (boot.config gfx-jobs=0), **FF9_LOADSCENE=Title FF9_LOADAT=N** (chama SceneManager.LoadScene(0x2570778) via il2cpp_string_new resolvido no contexto g_m_il2cpp — funciona, retorna SEM erro "scene not in build", tela vira preto total — MAS ainda 0 draws). ⇒ Unity NÃO emite NENHUMA geometria independente de cena/EGL/threading. = muro de SUBMISSÃO de render (família Elderand gfxdeviceworker). Flags novas s6: FF9_EGLFIX(default fbdev), FF9_NOEGLFIX, FF9_SHFIX/SHPREC/NOSHFIX, FF9_SPLASHDONE, FF9_LOADSCENE/FF9_LOADAT, FF9_NOOBBPATH/FF9_OBBPATH, FF9_NOAUX, FF9_REALAUDIO.

**🔴 MURO ATUAL: 0 draws (CUP_DRAWCOUNT maxdraws=0), tela preta+pillarbox branca.** Tudo destravado (boot, OBB, bundle, shaders, EGL ES2, lifecycle resume/focus, nativeRender chamado todo frame) MAS o GfxDeviceWorker não emite draw calls. eglMakeCurrent alterna ctx↔NULL (= render MT com worker thread) mas 0 geometria. Mesmo a UI/Canvas do boot scene não desenha. = muro de render do GfxDeviceWorker (família Elderand). HIPÓTESES p/ próxima: (a) -force-gfx-direct NÃO aplicado (forçar single-thread p/ main desenhar direto), (b) nenhuma cena com câmera carregada (StartGame forçado não faz LoadScene; investigar quem dispara o load da 1ª cena pós-verifier), (c) GfxDeviceWorker handshake (doFrame não alimenta a fila nativa de render, igual Elderand). ⚠️ BOOT NÃO-DETERMINÍSTICO (~50% trava em render 0 no [SEM] post/activity-indicator; harness retry-boot até render≥1500).

### s5 (2026-06-24) — 🟢 NRE PER-FRAME RESOLVIDA (de 4954→9). Initialize não aborta mais.
**🔑 Localizada a origem exata da NRE** via gdb-break em `NullReferenceException..ctor` (RVA 0x1fdcb4c) + stack-scan de RAs il2cpp:
chain = `ExpansionVerifier.Update`→`StartGame`→`AssetManagerForObb.Initialize`+0xa8→**`GooglePlayDownloader.GetExpansionFilePath`+0x340** (RA 0x10d75ac).
A NRE NÃO era a reflection de currentActivity (pista falsa de s4) — era um **delegate/`std::function` vazio chamado DENTRO de `GetExpansionFilePath`** (`ldr x8,[x0]; blr x8` em 0x10d758c → handler vazio → throw). `GetPatchOBBPath` (0x10d865c) é função-irmã com o mesmo risco.

**FIX: override IN-PLACE de `GetExpansionFilePath`(0x10d726c) + `GetPatchOBBPath`(0x10d865c) → `my_obb_path()`** (mesma técnica tail-call do SAPATH: `ldr x16,[pc#8]; br x16; .quad fn`). Devolve String il2cpp.
- ⚠️ **O valor DEVE ser arquivo existente NÃO-vazio:** `Initialize` testa `String.IsNullOrEmpty(path)==false` (0x1e75468, tbnz→Init+0x2a4) **E** `File.Exists(path)==true` (0x1f3e9c4, tbz→Init+0x2a4) p/ ALCANÇAR `GetAssetBundleFilePath`(0x14b9394, em 0x14b9168, que usa SAPATH p/ carregar aobb.bin). `""` passa a NRE mas PULA o load. **Default = `/storage/roms/ff9/bin/Data/aobb.bin`** (existe). Env `FF9_OBBPATH`.
- Flag nova: **FF9_NOOBBPATH** desliga o override (default ON).
- ⚠️ Para a Initialize RODAR é preciso **FF9_OBBINIT=1** (a flag NOOBBINIT, gated `!getenv`, NOPa o `bl Initialize` em 0x10DA3F0 por DEFAULT; setar FF9_OBBINIT=1 mantém Initialize).

**Resultado:** NRE 4954→9, CRASH=0, fb0 saiu do preto puro p/ cores variadas (ffffffff/grays). Combo p/ imagem: `FF9_EMPTYPKG=1 FF9_FORCE_STARTGAME=1 FF9_OBBINIT=1` (+OBBPATH/SAPATH default ON). **Próximo: confirmar draws/f>0 + aobb LoadFromFile + 1ª imagem.**

### s4 (2026-06-24, X5M BANIDO — foco Mali-450) — CRASH DO INIT BYPASSADO; resta NRE per-frame
**Contexto:** o usuário baniu o X5M ([[nunca-usar-x5m]]) — todo trabalho no Mali-450, mirando IMAGEM primeiro (logo/UI), não gameplay.

**🔑 BREAKTHROUGH — crash nativo do init Android RESOLVIDO via `FF9_EMPTYPKG`:**
- Ao tornar `currentActivity` funcional, o jogo avançava até a init Android e CRASHAVA nativo em **libunity+0xc16254** (`ldr x8,[x0]; cbz x8; ldr x0,[x8]`, fault=bytes do nome do pacote) — após `getPackageInfo→length()`. O nativo deref os bytes do nome do pacote como ponteiro. Crash-handler ([CR]) agora faz **stack-scan de RAs il2cpp** (mapear via script.json): chain = `ExpansionVerifier.StartGame`→`AssetManagerForObb.Initialize(0x14B8FFC)`.
- **FIX: `FF9_EMPTYPKG=1`** (getPackageName + campo packageName = "") → string vazia → `[x0]=0` → o `cbz` PULA o deref → **0 crash, render loop 200 frames estável.**

**currentActivity AGORA FUNCIONAL (default, era TER_KBFIX):** GetStaticFieldID/GetStaticObjectField(currentActivity)→sentinel `g_current_activity`; GetObjectClass→`UnityPlayerActivity` class; reflection getName/getCanonicalName→"java.lang.Object" (default). +**GetStringChars/GetStringLength UTF-16 (vtable 164/165) que FALTAVAM** (jstrings nossas são char* UTF-8). +GetObjectField p/ packageName/versionName/sourceDir/applicationInfo; campos split*→array vazio (`g_empty_strarr`); getObbDir→bin/Data, getObbDirs→File[1] (`g_file_array`/`g_obb_file_obj`); versionCode→67; getPackageCodePath/getParent→path.
- **SAPATH:** override IN-PLACE de `Application.get_streamingAssetsPath` (FF9 RVA **0x2538CC0**) → `my_streamingAssetsPath`→String il2cpp "/storage/roms/ff9/bin/Data" (il2cpp_string_new por NOME). `AssetManagerForObb` monta o path do aobb.bin daí.

**🔴 MURO ATUAL (resta p/ imagem):** **NRE per-frame em `UnityPlayer.currentActivity`** (FindClass(UnityPlayer)→GetStaticFieldID(currentActivity)→NRE, ANTES do GetStaticObjectField, ~23/frame, caught). Acontece dentro de `AssetManagerForObb.Initialize` (chama GetExpansionFilePath/GetAssetBundleFilePath) → aborta a Initialize ANTES de chegar no path/LoadFromFile → **`my_streamingAssetsPath` NUNCA é chamado, aobb.bin NUNCA abre → cena não carrega → PRETO** (fb0=ff000000+00000000, só clears). É a reflection do tipo do campo no `AndroidJNIHelper.GetFieldID` (sig vazio) — precisa emular o caminho de reflection certo (Class.getField/getType) p/ currentActivity. dump il2cpp em scratch.

**FLAGS s4:** FF9_EMPTYPKG (bypass crash, ESSENCIAL), FF9_FORCE_STARTGAME (switch Update→state 8, one-shot trampolim), FF9_OBBINIT (não-NOP a Initialize do StartGame; default NOPa), FF9_NOSAPATH (desliga override), FF9_OBB (RunningOnAndroid real), FF9_RALOG/FF9_RALOG_AT=N (stack-scan caller: FIDSCAN no GetStaticFieldID, RASCAN no GetStaticObjectField). **PRÓXIMO:** emular a reflection de campo (getField("currentActivity")→Field→getType→Class válido) p/ matar a NRE → Initialize completa → aobb carrega → 1ª imagem. NÃO é muro de device (X5M fora).

### MURO HISTÓRICO (s2/s3 início) — LVL / currentActivity NRE (era sintoma do OBB acima)
- O C# faz check de **Google Play Licensing** no startup (`BASE64_PUBLIC_KEY`/`SALT`/`currentActivity`) E usa `currentActivity` per-frame → NRE (nosso fake não é Activity funcional). A APKVISION "neutralizou DRM" mas o C# ainda tenta. **Caminhos:** (a) prover `currentActivity` funcional + serviço de licença que retorne LICENSED (LicenseCheckerCallback.allow) — emulação de Activity/AndroidJavaObject pesada; (b) achar/stubar o gate de licença no C# (il2cpp dump, metadata plaintext ajuda); (c) ver o que o uso per-frame de currentActivity faz e prover o método que retorna não-null. **Ref:** RE4 (Unity Android so-loader) pode ter currentActivity funcional. ⚠️ JÁ TENTADO e NÃO resolve: `TER_KBFIX` (activity fake) E `FF9_ACT_NULL=1` (currentActivity→NULL) — ambos ainda dão 3453 NRE/run. ⇒ o C# precisa de uma **Activity FUNCIONAL** (com métodos que retornem valores válidos), não fake nem null. Próximo: descobrir QUAL método/campo o C# chama (a NRE é lançada SEM chamada JNI visível entre GetStaticObjectField e o throw → provavelmente Unity AndroidJavaObject falha ao envolver nosso jobject inválido, OU o LVL `BASE64_PUBLIC_KEY` (devolve &fake genérico, não String válida) → decode falha). Usar **il2cpp dump (metadata PLAINTEXT)** p/ achar os métodos C# ao redor da NRE, ou habilitar stack-trace gerenciado.
- ⚠️ NULLGUARD é band-aid (mascara o feature-flag false @0x4e723c). O fix "correto" é fazer a feature existir (ES3→ES2 shim? capability?) mas o band-aid já dá render loop estável.

---

## ESTADO s1 (HISTÓRICO — diagnóstico de job-system estava ERRADO, ver s2) — boot completo, tela PRETA

Boot vai longe **sem crash**:
- **F0** libunity carregada/relocada, imports resolvidos, init_array (426), **JNI_OnLoad=0x10006** OK.
- **F1** libil2cpp carregada/relocada, init_array (24) OK.
- **F2** `initJni` (lê `bin/Data/boot.config` via AssetManager) → `nativeRecreateGfxState` OK → loop `nativeRender`.
- **GL context REAL criado** (Mali fbdev): Unity loga extensões `GL_OES_depth24...`, carrega "unity default resources".
- **C# rodando**: `ApplicationInfo`, `Company Name: SquareEnix`, playerprefs, `runOnUiThread`, `isUaaLUseCase`.
- **Choreographer doFrame disparando** (driver-thread `TER_CHOREO`): "doFrame começou a disparar".
- ⚠️ `eglChooseConfig EGL_BAD_ATTRIBUTE` 8× nos logs do Unity, **mas o context é criado mesmo assim** (fallback) — não é o bloqueio.

🔴 **MURO**: tela **PRETA** (fb0 = preto sólido, estável em t=30/60/90s). **Deadlock de handshake do job-system no startup.**

### Diagnóstico gdb (decisivo)
- **Thread 1 "UnityMain"** bloqueada em `pthread_cond_wait` chamado de **libunity+0x61efe0**. O loop:
  ```
  61efe0: ldr x8,[x19,#88]   ; x8 = obj[88] (fila/queue head)
  61efe4: cbz x8, 61eff0     ; se 0 -> espera
  61efe8: ldr x8,[x8]
  61efec: cbnz x8, 61f000    ; se *obj[88]!=0 -> prossegue (tem item/pronto)
  61eff0: cond_wait(x21=cond, x20=mutex)
  61effc: b 61efe0           ; loop
  ```
  Função entra em 0x61ee80; faz mutex_lock + chamadas JNI/UnitySendMessage(0xc0a860/0xc0aba8/0xc0b700) ANTES do wait.
- **TODAS as threads de trabalho PARKADAS em futex** (syscall): `Loading.Preload`, `Loading.AsyncRe`, `UnityGfxDeviceW`, 16× `Background Job.`, `Job.Worker 0/1/2`, `GC Finalizer`, 3× `AssetGarbageCol`. → ninguém produz; nenhum job foi despachado. **Deadlock de startup**, não livelock.
- O `[SEM] post` storm visto em logs é o **doFrame** do choreo (cada frame posta um sem); no instante do dump tudo está parado.

### Interpretação
É o **mesmo muro do Terraria/Elderand**: o produtor nativo (job/preload/gfx) do startup não é alimentado pelo nosso ambiente fake → a main espera para sempre uma fila (`obj[88]`) que nunca é preenchida. O doFrame do Choreographer **já funciona** (não é o bloqueio do frame-2 puro); o bloqueio é a **fila de job/preload** do boot.

---

## FIXES APLICADOS (commitados — master, origin+private, commit 0991d99)

1. **`so_util.c` so_load reescrito LAYOUT-AGNÓSTICO.** Unity **2022** põe o **1º PT_LOAD NÃO-executável** (R--), com o segmento RX em vaddr≠0 (libunity RX@0x364890, libil2cpp RX@0xe7291c). O loader antigo assumia executável-primeiro: no `else` fazia `if(text_segno<0) goto err_free_so` com `res=0` → retornava "sucesso" com `text_base=nil` → `so_relocate` crashava em base nula. Além disso `text_base=load_base+text_vaddr` era errado p/ código fora do vaddr 0 (RVAs/relocs não batiam). **FIX**: module base = vaddr 0 = `load_base`; mapeia TODOS os PT_LOAD no `p_vaddr`; `text_base=load_base`; `text` cobre `[0,data_lo)` (RX), `data` = span de TODOS os RW `[data_lo,data_hi)`. (Terraria/2021 era RX-first, por isso nunca apareceu.)

2. **Removido `CUP_GCOFF` do run.sh.** Chamava `il2cpp_gc_disable` no OFFSET hardcoded do Terraria `il2cpp_base+0x73ca6c`, que no FF9 é uma **tabela de dados** (0xffffffff) → executar dados = **SIGILL**.

3. **il2cpp C API resolvida por NOME** (não por offset). `il2_domain_get`/`il2_thread_attach` via `so_find_addr_safe("il2cpp_domain_get"/"il2cpp_thread_attach")` no F1 (módulo ativo = il2cpp). libil2cpp **exporta os 241 símbolos `il2cpp_*`**. O choreo_driver_thread usava `0x73c860`/`0x73ccb4` (offsets Terraria) → SIGILL.

### ⚠️ DÍVIDA TÉCNICA (resolver por NOME quando tocar nessas features)
Ainda há MUITOS offsets Terraria/Cuphead hardcoded em `main.c`/`jni_shim.c` que SÓ disparam em features gated (não ligadas no boot mínimo): `0x73c860` dom_get, `0x73c86c` domain_get_assemblies, `0x73c22c` assembly_get_image, `0x73c264` class_from_name, `0x73c28c` class_get_method_from_name, `0x73cc7c` runtime_invoke, `0x73ca44/0x73ca48` field get/set, `0x73cc80` class_init, etc. E offsets de PATCH em libunity (storage-check `0x2d8fac`, job-system `0x2f37a4`/`0x2f1d48`/`0x2eaacc`/`0xc0da20`, NOGCWAIT `0x74f260`...) são **todos do Terraria 2021** → errados p/ FF9 2022. **Não ligar flags TER_*/CUP_* que usem offset sem reverter o offset do FF9 primeiro.**

---

## COMO RODAR

`run.sh` (env mínimo, fbdev Mali-450): `SDL_VIDEODRIVER=mali`, `TER_NOSTORAGEPATCH=1` (pula o NOP do storage-check 0x2d8fac do Terraria, offset errado p/ 2022), `CUP_NOLOGFILE=1`, `CUP_FRAMES=999999999`, `TER_CHOREO=1`. `-force-gfx-direct -force-gles20` já são injetados por default no cmdline (`cmdline_fd`).

Layout no device: binário `ff9` + `libunity.so`/`libil2cpp.so`/aux no **GAMEDIR root** `/storage/roms/ff9/`; assets em `/storage/roms/ff9/bin/Data/` (concatenar os 38 `sharedassets0.assets.split*` em ordem `ls -v` ANTES de deployar). Lançar foreground.

Scripts de debug (no port): `gdbcrash.sh` (catch SIGILL+regs+bt), `gdbstk.sh` (stack da main), `gdball.sh` (PC de todas as threads). Resolver endereços: `addr2line -e ff9 <off>`; bases libunity/il2cpp saem no log (`libunity: text=`, `libil2cpp: text=`).

---

## PRÓXIMOS PASSOS (próxima sessão)

1. **Atacar o deadlock do job-system de startup** (a main espera `obj[88]` em libunity+0x61efe0; workers todos parkados). Caminhos:
   - RE da função 0x61ee80: descobrir QUEM deveria preencher `obj[88]` (que thread/job) e por que não é despachado. É a fila de Preload/AsyncRead? (threads `Loading.Preload`/`Loading.AsyncRe` estão parkadas).
   - Comparar com a receita Terraria que PASSOU desse muro: `TER_INLINETASK`/`TER_FORCETHREADED`/`TER_SKIPJOBWAIT` patcham o job-system de libunity — mas nos OFFSETS do Terraria. **Achar os offsets equivalentes na libunity 2022 do FF9** (job-scheduler "threaded" flag, WaitForJobGroup, per-object task wait) e re-apontar os patches.
   - Hipótese alt: o flag "threaded" do job-system fica 0 no nosso env (boot.config/capability) → scheduler nunca despacha p/ os workers (que existem e estão parados). Era exatamente o caso do Terraria (`TER_FORCETHREADED`, flag em libunity+0xc0da20). Achar o equivalente FF9.
2. Confirmar que o `eglChooseConfig EGL_BAD_ATTRIBUTE` não vira problema depois (context já cria; revisitar só se o render aparecer torto).
3. Regra: **JAMAIS japonês** — FF9 pode defaultar JP; forçar inglês quando chegar no menu (locale/região).

## Refs
- `ports/terraria` (mesmo loader IL2CPP; HANDOFF tem a saga do job-system 2021).
- `ports/elderand` (mesmo muro de handshake de frame/job no Mali-450; muitas sessões).
- `STUDY.md` (recon original).
