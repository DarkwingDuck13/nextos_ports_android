# LCS (GTA Liberty City Stories) → Mali-450 — HANDOFF p/ próxima sessão (atualizado s4, 2026-06-21)

> ATUALIZACAO 2026-06-23 s7.11: PAUSA solicitada pelo porter. Estado salvo; nao continuar
> resolvendo o mesmo bug agora.
>
> Estado deixado:
> - build local compilada e deployada no device;
> - `run-playable.sh` iniciado novamente;
> - jogo aberto em gameplay, `./lcs` vivo, `state=9 post-draw`, `FinishCutscene called=2`;
> - screenshot final: `~/lcs-build/shot-left-open-playable-20260623.png`.
>
> Descoberta importante:
> - O fix `LCS_SHADOWS_OFF=1` completo limpou o gameplay perto do carro nas capturas locais
>   (`shot-fullshadowoff-gameplay.png`, `shot-left-open-playable-20260623.png`).
> - MAS a primeira cutscene do onibus ainda mostrou retangulo preto grande mesmo com sombras
>   totalmente desligadas:
>   `~/lcs-build/shot-fullshadowoff-cutscene-noskip.png`.
> - Frame posterior/interior limpo:
>   `~/lcs-build/shot-fullshadowoff-cutscene-noskip-late.png`.
> - Portanto esse retangulo especifico da cutscene nao deve ser tratado como `CShadows`/`CDynamicShadows`
>   primeiro. A pista atual e pass/material de world-stream, detail texture, textura/mipmap/compressao,
>   ou overlay de mundo fora do sistema classico de sombras.
>
> Testes nao conclusivos:
> - `BULLY_TRILINEAR=1` e `LCS_NO_WORLD_ALPHA=1` seriam bons A/B, mas nos runs curtos morreram
>   cedo antes de gerar screenshot. Nao usar esses resultados como prova.
> - `LCS_GLSTATE=1` pegou draws normais de mundo no frame inicial (`prog=39/99`, muitos draws opacos),
>   reforcando que nao e so shadow pass classico.
>
> Quando voltar:
> - Nao refazer PVS/SHADOWS_OFF do zero.
> - Se for atacar o retangulo do onibus, partir de `shot-fullshadowoff-cutscene-noskip.png` e isolar
>   pass/material de world-stream.

> ATUALIZAÇÃO 2026-06-23 s7.10: quadrados pretos de chao/pista tratados como shadow pass.
> O porter observou que o chao quebrado corrigia quando o farol do carro acendia. As fotos
> `photo_2026-06-23_00-24-09.jpg`,
> `photo_2026-06-23_00-24-08 (2).jpg` e
> `photo_2026-06-23_00-24-08.jpg` mostraram que a textura do piso existia,
> mas uma mascara escura grande e dura era aplicada por cima. Isso aponta para sombra/lighting,
> nao para asset ausente puro.
>
> Patch aplicado:
> - nova chave `LCS_SHADOWS_OFF=1` por default em `run30.sh` e `run-playable.sh`;
> - no-op granular para:
>   `RenderShadow`, `FinishStoringShadowAndRender`, `CWorld::CastShadow`,
>   `CWorld::CastShadowSectorList`,
>   `CCutsceneObject::CreateShadow`,
>   `CDynamicShadows::Begin/End/UpdateCamera/PreRenderSetup`,
>   `CShadows::StoreShadowForPed`, `CShadows::StoreShadowForVehicle`,
>   `CShadows::StoreShadowForTree`, `CShadows::StoreShadowForPole`,
>   `CShadows::StoreShadowForPedObject`,
>   `CShadows::StoreShadowToBeRendered`,
>   `CShadows::RenderStoredShadows`, `CShadows::RenderStaticShadows`,
>   `CShadows::RenderExtraPlayerShadows`,
>   `CShadows::UpdateStaticShadows`, `CShadows::UpdatePermanentShadows`;
> - zera knobs de shadow relacionados:
>   `dv_renderEntityShadow_*`, `dv_shadowStrength`, `dv_fRecieveShadowsRadius`,
>   `CurrentShadowStrengthDampVal`, `CurrentLightShadowStrengthDampVal`,
>   `CurrentPoleShadowStrengthDampVal`;
> - nao usa `LCS_GFX_LOW=1`; o perfil agressivo continua proibido.
>
> Evidencia salva:
> - `~/lcs-build/shot-bus-shadowoff.png`: primeira cutscene/rua com `LCS_SHADOWS_OFF=1`,
>   sem o bloco preto grande na pista.
> - `~/lcs-build/shot-bus-shadowoff-2.png`: segunda cena/interior tambem renderizando.
> - `~/lcs-build/shot-playable-shadowoff-default.png`: gameplay com Toni/carro/HUD e chao limpo
>   usando o default novo.
> - `~/lcs-build/shot-fullshadowoff-gameplay.png`: validacao nova com `SHADOWS_OFF` completo
>   (dynamic/cutscene/update tambem desligados), gameplay perto do carro sem os poligonos pretos
>   grandes das fotos.
>
> Pendencia imediata:
> - repetir a primeira cutscene do onibus com run sem auto-skip para validar aquele ponto exato.
>   O run s7.10 pulou as cutscenes cedo e validou gameplay, nao o frame do onibus.
>
> Nota de GTASA Vita: o padrao confirmado no repo estudado e reduzir efeitos caros
> (`disable_detail_textures=1`, `disable_tex_bias=1`, `disable_ped_spec=1`) e aplicar fixes
> especificos, nao ligar tudo. Para LCS, a correção segura agora e granular no `CShadows`.
>
> Default atual: `PVS_CLEAN=1`, `GFX_PREFS=1`, `SHADOWS_OFF=1`, `GFX_LOW=0`,
> `TRIGGER_BUTTONS=0`, `TRIGGER_AXES=0`, movimento no analogico esquerdo raw 0/1.

> ATUALIZAÇÃO 2026-06-22 s7.8: artefatos de chao/cenario grandes corrigidos.
> A causa dos "quadrados/planos" que apareciam no chao, ruas e cutscene nao era textura faltando:
> era overlay/debug de zonas PVS ligado no engine. O log provou:
> `dvPVSRenderWorldZones=1`, `dvPVSRenderCameraZones=1`, `dvPVSZonesAlpha=0.100`.
>
> Patch aplicado:
> - `LCS_PVS_CLEAN=1` por default em `run30.sh` e `run-playable.sh`.
> - O cleanup zera apenas visualizacao/debug PVS/bounding boxes:
>   `dvRenderPVSZones`, `dvPVSRenderWorldZones`, `dvPVSRenderCameraZones`,
>   `dvPVSZonesAlpha`, `dvRenderPVSdStuffAsPink`, `dvDebugWorldShader`,
>   `dvRenderWorldBoundingBoxes`, `dvRenderWorldParentBoundingBoxes`.
> - Isso NAO desliga o PVS real/culling; preserva o fluxo nativo e remove a sobreposicao visual.
>
> GTASA/Bully aplicado de forma segura:
> - `LCS_GFX_PREFS=1` agora e default; `LCS_GFX_LOW=0` continua default.
> - Foi corrigida a escrita dos `dvDebug*`: `gRenderDynamicShadows`, `dvbRenderStoredShadows`,
>   `dv_bShowShadowMap`, etc. sao objetos de 64 bytes e precisam escrever o bool em `+61`;
>   `dvLodDistanceScale` e objeto float de 80 bytes e precisa escrever em `+76`.
> - Perfil leve validado: sombras dinamicas/stored off, cutscene shadows off, reflections pref off,
>   LOD scale 0.75. O perfil agressivo `LCS_GFX_LOW=1` segue proibido por enquanto porque crashou no boot.
>
> Evidencia salva:
> - `~/lcs-build/shot-pvs-clean-gfxprefs-2026-06-22.png` = gameplay limpo, sem planos PVS.
> - `~/lcs-build/run-pvs-clean-gfxprefs-final-2026-06-22.log` = 150s, saida limpa, `Mali teardown OK`.
> - `~/lcs-build/progress-pvs-clean-gfxprefs-final-2026-06-22.txt`.
> - `~/lcs-build/run-pvs-clean-alpha-diag-2026-06-22.log` = prova `wsa`/RenderAlpha e PVS debug vars.
> - `~/lcs-build/shot-pvs-clean-alpha-diag-2026-06-22.png`.
>
> Estado no device: binario novo deployado, `run30.sh` e `run-playable.sh` atualizados. Para repetir:
> `sh /storage/roms/ports/lcs/run-playable.sh`.
> Default atual: `PVS_CLEAN=1`, `GFX_PREFS=1`, `GFX_LOW=0`, `TRIGGER_BUTTONS=0`, `TRIGGER_AXES=0`,
> `ENABLE_EXIT_HOTKEY=0`, movimento no analogico esquerdo raw 0/1.

> ATUALIZACAO 2026-06-22 s7.7: marco jogavel confirmado pelo porter.
> O run atual esta "rodando bem": fluxo nativo `newgame`, 2 cutscenes puladas por Start nativo,
> camera OK, mundo visivel e analogico esquerdo andando pelo joystick raw 0/1. D-pad nao alimenta
> mais movimento por default e L2/R2 nao entram como botoes enquanto o enum nao for confirmado.
>
> Evidencia salva:
> - `~/lcs-build/run-camera-ok-fullprofile-2026-06-22.log`
> - `~/lcs-build/run-playable-leftanalog-skipnative-2026-06-22.log`
> - `~/lcs-build/progress-playable-leftanalog-skipnative-2026-06-22.txt`
>
> Trechos chave do run jogavel:
> - `FinishCutscene called f=2279` e `f=3670`.
> - Gameplay depois dos pulos: `cut=0/0/0`, `fade=0`, `nVis=47..55`.
> - Input fisico: `[input] ... axis0/axis1 ... src=raw`.
> - Ped/camera mudando juntos: `pedpos=1420.x,-195.x` ate `1417.x,-201.x` e camera acompanha.
> - O porter confirmou em tela: "tudo rodando bem".
>
> Script novo para repetir o estado bom no device:
> `sh /storage/roms/ports/lcs/run-playable.sh`
> Ele chama `run30.sh` com perfil leve (`GLSTATS=0`), inicia `newgame`, pulsa Start (`9`) ate
> 2 `FinishCutscene called`, mantem `LCS_MOVE_RAW=1` nos eixos 0/1 e `LCS_TRIGGER_BUTTONS=0`.
> Se precisar diagnosticar controle, rode o mesmo script com `LCS_GLSTATS=1` ou
> `LCS_RAWAXISDIAG=1`, mas para jogar deixar leve.

> ATUALIZAÇÃO 2026-06-22 s7.6: inverter o mapeamento para o comportamento correto.
> O porter confirmou que o D-pad estava andando e o analogico esquerdo estava com funcao errada/zoom.
> Isso nao e o alvo: o padrao de GTASA Vita e `MAPPING_PED_MOVE_X/Y = ANALOG_LEFT_X/Y` e D-pad
> separado para acoes/menu/radio/zoom. Portanto o default foi ajustado:
>
> - `LCS_DPAD_AS_AXIS_ONLY=0`: D-pad deixa de alimentar `axis0/axis1` de movimento por default.
> - `LCS_MOVE_RAW=1`, `LCS_MOVE_AXIS_X=0`, `LCS_MOVE_AXIS_Y=1`: movimento vem dos eixos raw 0/1
>   do joystick fisico. O device reportou `USB Gamepad` com `axes=4 hats=1 buttons=12`; D-pad deve
>   estar no hat, analogico esquerdo normalmente em raw 0/1.
> - `LCS_RAWAXISDIAG=1` existe para calibrar sem chute; loga `[rawaxis] ... a0/a1/a2/a3`.
> - `LCS_PADBRIDGE_MOVE=1` continua necessario: ele espelha o eixo de movimento nos offsets que
>   `GetPedWalk*` realmente le quando `m_bSwapNippleAndDPad` esta ativo.
>
> Run aberto no device para playtest:
> `LCS_MAXSECONDS=420 LCS_START=newgame LCS_STARTFRAME=120 LCS_INPUTDIAG=1 LCS_PADDIAG=1
>  LCS_RAWAXISDIAG=1 LCS_DPAD_AS_AXIS_ONLY=0 LCS_MOVE_RAW=1 LCS_MOVE_AXIS_X=0 LCS_MOVE_AXIS_Y=1`.
> O log inicial confirmou `move_raw=1`, `dpad_axis_only=0` e raw neutro `a0/a1/a2/a3 ~= 0.004`.
> Proximo passo: mexer analogico esquerdo; esperar `axis0/axis1 src=raw` e Toni andar.
> Se o analogico ainda nao andar, trocar os envs para `LCS_MOVE_AXIS_X=2 LCS_MOVE_AXIS_Y=3` ou
> inverter sinais apos olhar `[rawaxis]`.

> ATUALIZAÇÃO 2026-06-22 s7.5: movimento visual comprovado e causa do "nao anda" isolada.
> O problema principal de movimento era mapeamento nativo: `CPad::m_bSwapNippleAndDPad` faz
> `GetPedWalkLeftRight/UpDown` ler offsets de D-pad (`18/20/22/24`) em vez dos offsets do stick
> (`2/4`). Como o perfil estavel suprimia botoes D-pad em gameplay, camera pelo analogico direito
> funcionava, mas Toni nao andava.
>
> Patch aplicado:
> - `LCS_PADBRIDGE_MOVE=1` por default: em gameplay, espelha o eixo esquerdo nos offsets de
>   movimento que `GetPedWalk*` realmente le quando o swap esta ativo.
> - `LCS_TRIGGER_BUTTONS=0` por default: L2/R2 continuam como eixos 4/5, mas nao viram botoes 6/7
>   ate o enum ser confirmado; reportado que L2 fechava o jogo.
> - `LCS_INPUT_PROBE_ONLY=1` existe so para teste automatizado isolado; ignora SDL fisico e aceita
>   apenas `/dev/shm/lcs_btn` e `/dev/shm/lcs_axis`.
>
> Prova visual:
> - `~/lcs-build/shot-walk4-before-2026-06-22.png`
> - `~/lcs-build/shot-walk4-after-2026-06-22.png`
> - `~/lcs-build/run-walk4-proof-2026-06-22.log`
> - `~/lcs-build/progress-walk4-proof-2026-06-22.txt`
>
> Resultado: par gameplay/gameplay com HUD normal, `CRASH=0`, log `LY=+1.00 -> UDLR=0100`; Toni
> sai da traseira do carro e a camera acompanha. Run de prova usou `probe_only=1`; o `run30.sh`
> normal fica com `probe_only=0`, portanto o controle fisico continua ativo.

> ATUALIZAÇÃO 2026-06-22 s7.4: reprodução direta do input em gameplay.
> O teste correto foi feito: `LCS_START=newgame`, Start nativo ate 2 `FinishCutscene called`,
> depois D-pad/analogico em gameplay. Resultado: sem crash no perfil atual.
>
> Artefatos:
> - `~/lcs-build/run-inputdiag2-after-input-2026-06-22.log`
> - `~/lcs-build/progress-inputdiag2-after-input-2026-06-22.txt`
> - `~/lcs-build/run-button-axis-no-crash-2026-06-22.log`
>
> Trecho chave:
> - `FinishCutscene called f=178` e `f=205`.
> - D-pad injetado: `button=14`, `button=15`, `button=12` em `state=9`.
> - Analogico injetado: `axis0=1.000`, `axis1=-1.000`.
> - Controle fisico real tambem apareceu depois: `phys=1` em botoes e `src=sdl` em eixos.
> - D-pad fisico em gameplay foi convertido para eixo:
>   `dpad-as-axis dx=-1 dy=0 buttons-suppressed=1`.
> - O processo continuou ate pelo menos `f=2144 state=9 post-draw`, `CRASH=0`.
>
> Patch novo:
> - `LCS_DPAD_AS_AXIS_ONLY=1` por default no `run30.sh`: em gameplay, D-pad fisico nao chama
>   `onJoyButtonDown/Up` dos enums 12..15; vira `setJoyAxis`.
> - Escape reversivel: `LCS_DPAD_BUTTONS=1` ou `LCS_DPAD_AS_AXIS_ONLY=0`.
> - `LCS_INPUTDIAG=1` loga bordas de botao/eixo sem GLSTATS pesado. Use junto de `LCS_PADDIAG=1`
>   quando reportarem input travando.
>
> Estado pratico: o crash/freeze ao apertar direcional/analogico nao reproduz mais. Se voltar,
> capturar o ultimo `[input]` em `run.log`; se houver SIGSEGV, o crash handler imprime PC/LR/offset.

> ATUALIZAÇÃO 2026-06-22 s7.3: falso "travou ao apertar controle" diagnosticado.
> O input fisico chegou (`[padbridge] UpdatePads state=9 mask=0x10`), mas a thread principal estava
> dormindo em `fat_file_fsync -> SyS_fdatasync`. Causa: heartbeat de LCS fazia `fdatasync()` todo
> frame em `/storage/roms` (VFAT/SD). Bully, que roda bem, nao faz fsync por frame no loop jogavel.
>
> Patch aplicado:
> - `hb()` agora e leve por default: `LCS_HB_EVERY=30`, sem `fdatasync`.
> - Modo forense antigo: `LCS_HB_EVERY=1 LCS_HB_FSYNC=1`.
> - `restartguard` reduzido para 4 logs; spam so com `LCS_RESTARTDIAG=1`. Esta limpeza ja compila
>   localmente e entra no proximo deploy/restart; a sessao aberta no device nao foi interrompida.
>
> Playtest aberto no device com binario novo:
> - PID `18954`, perfil estavel, `GLSTATS=0`, `PADDIAG=1`, `PADBRIDGE_DIRECT=0`, `LCS_HB_EVERY=30`.
> - 2 cutscenes puladas com Start nativo: `FinishCutscene called f=165` e `f=191`.
> - Input fisico depois disso: `mask=0x10`.
> - Snapshot avançou ate pelo menos `f=7544 state=9 post-draw`, sem `CRASH`.
> - Artefatos:
>   `~/lcs-build/run-playtest-hb-light-2026-06-22.snapshot.log`,
>   `~/lcs-build/progress-playtest-hb-light-2026-06-22.snapshot.txt`,
>   `~/lcs-build/heartbeat-playtest-hb-light-2026-06-22.snapshot.txt`.
>
> Regra nova: perfil jogavel nao usa `LCS_GLTRACE`, `LCS_DRAWHB` nem `LCS_HB_FSYNC`. Isso fica so
> para investigacao curta de wedge/reboot.

> ATUALIZAÇÃO 2026-06-22 s7.2: controles/gameplay confirmados pelo caminho correto.
> Para testar controle, nao deixar o port remover cutscenes nem pular ponteiro: preservar
> `LCS_START=newgame` e pular as cutscenes com `Start` nativo (`/dev/shm/lcs_btn` enum `9`).
>
> Artefatos locais novos:
> - `~/lcs-build/run-stable-native-axis.full.log` = fluxo com Start nativo, gameplay por milhares
>   de frames, `fade=0`, `nVis=47..49`, `cam=1425.6,-195.6,51.3`, Toni em `1420.1,-195.2,50.3`.
> - `~/lcs-build/run-stable-axis2.full.log` = prova de input: `[probe] axis 0 = 1.000`,
>   `axis=1.00`, `gaxis=0.00,1.88`, `cpad=128`, ped andando em gameplay visivel.
> - `~/lcs-build/run-control-clean2.full.log` = prova limpa: depois de 2 `FinishCutscene`, eixo
>   entra so em gameplay visivel (`fade=0`, `nVis=20`) e Toni anda de `1427.4` para `1425.x`.
> - `~/lcs-build/progress-stable-axis2.txt`.
>
> Perfil estavel para repetir:
> `LCS_START=newgame LCS_STARTFRAME=120 LCS_STREAMER_MAX=160 LCS_RESOURCEDRAIN_MAX=8
>  LCS_ENABLE_POP=1 LCS_ENABLE_HELI=1 LCS_ENABLE_USERDISPLAY=1 LCS_NO_FADEHACK=0
>  LCS_CUTSCENE_PAD_SKIP=1 LCS_CUTSCENE_SPLINEFIX=1 LCS_CUTSCENE_CLEAR_AFTER_FINISH=1
>  LCS_CUTSCENE_RESTORE_CAMERA=1 LCS_GLSTATS=1`.
>
> Cuidados:
> - Escrever `0` em `/dev/shm/lcs_btn` NAO solta botao; aperta enum 0. Para Start, escrever so `9`
>   em pulsos ate `grep -c "FinishCutscene called" run.log` dar 2.
> - Nao usar `PADBRIDGE_DIRECT`; o caminho valido e JNI/setJoyAxis (`/dev/shm/lcs_axis`).
> - O shim ja le `SDL_GameController` e envia eixos por `setJoyAxis`; para o controle fisico,
>   testar este mesmo perfil com `LCS_PADDIAG=1` e `LCS_AXIS_DEADZONE=0.10..0.18`, sem `PADBRIDGE_DIRECT`.
> - So injetar eixo depois de `FinishCutscene called` x2 e gameplay visivel (`fade=0`, `nVis>0`,
>   `ped=0x...`), senao o input entra no handoff/fade.
> - O combo agressivo `STARTFRAME=12 + FE25/FE25_POSTREADY + POP/HELI off + STREAMER_MAX=1`
>   regrediu para wait do streamer em f154; nao e o perfil de controle.

> ATUALIZAÇÃO 2026-06-22 s7.1: estado mais novo.
> O fluxo já passa por menu/start nativo, 2 cutscenes 3D e chega em gameplay por 300s sem crash.
> O crash pós-2ª-cutscene em `CCutsceneMgr::Update_overlay -> CreateCutsceneObject ->
> CEntity::CEntity -> memcpy(NULL, ..., 11)` foi corrigido sem instalar wrapper cedo no boot.
> A correção é limpar somente o estado de load remanescente quando a 2ª cutscene termina:
> `ms_cutsceneLoadStatus=2->0`, `ms_numLoadObjectNames=4->0`, preservando `ms_numCutsceneObjs=11`.
>
> Artefatos principais:
> - `~/lcs-build/run-postreconcile-delay60-1.log` = 300s, 2 cutscenes -> gameplay, teardown limpo.
> - `~/lcs-build/progress-postreconcile-delay60-1.txt`
> - `~/lcs-build/run-updateoverlay-early-wedge-1.log` = tentativa ruim; nao usar wrapper cedo.
> - `~/lcs-build/run-postreconcile-firsttransition-stuck-1.log` = `GAMEPLAY_RELEASE_DELAY=45` pode
>   prender depois da primeira transicao.
>
> Config atual mais segura: manter `LCS_GAMEPLAY_RELEASE_DELAY=60`, `LCS_CUTSCENE_CAMPROCESS=1`,
> `LCS_CUTSCENE_CAMPROCESS_STOPPOS=0.960`, `LCS_CUTSCENE_FINISH_POS=0.985`,
> `LCS_PADBRIDGE_DIRECT=0`. `PADBRIDGE_DIRECT` continua proibido por enquanto.
>
> Observacao nova do relogio: `LCS_NOON=1` congelava o HUD em 12:00 durante gameplay. Foi trocado
> para pulso de boot limitado por `LCS_NOON_FRAMES` (default 300). Para reproduzir o hack antigo,
> usar `LCS_NOON_STICKY=1`.
>
> Proximo alvo real: controles/movimento jogavel. O eixo via JNI chega ao engine (`cpad=127` nos
> logs), mas ainda falta provar translação do Toni/camera follow em gameplay. Depois disso atacar
> textura/streaming preto e lentidao.

> ATUALIZAÇÃO 2026-06-22: este handoff antigo ficou parcialmente obsoleto.
> O bloqueio principal NÃO é mais "mundo 3D/PVS vazio". O fluxo já renderiza cutscene 3D real
> com prédios/personagem/câmera. Novo marco: com `LCS_CUTSCENE_CAMPROCESS_STOPPOS=0.960` +
> `LCS_CUTSCENE_FINISH_POS=0.985`, o jogo passou pelas 2 cutscenes, finalizou limpo e chegou em
> gameplay real com Toni, HUD, minimapa, carro, árvores e mundo 3D.
>
> Artefato principal:
> - `~/lcs-build/run-fe25-finishpos-1.log`
> - `~/lcs-build/shot-fe25-finishpos-1.png`
>
> Caveat importante: `FINISHPOS` é mitigação do handoff, ainda sensível a timing. Um run com
> `LCS_PADBRIDGE_DIRECT=1` crashou antes de qualquer input em:
> `BatchedWorld::C_WorldRenderManager::GetFreeDrawCall`.
> Então NÃO usar `PADBRIDGE_DIRECT` por enquanto; testar controle pelo caminho JNI nativo
> (`/dev/shm/lcs_axis` -> `setJoyAxis`) com o binário atual.
>
> O blocker anterior era um crash no renderer mobile/batched world durante o fim da primeira cutscene:
> `BatchedWorld::C_WorldRenderManager::GetFreeDrawCall -> cWorldStreamEx::DrawPrimitiveRef ->
> CMattRenderer::Render -> RenderScene`.
>
> Patch importante já aplicado: `LCS_FE25` agora é protegido por `!lcs_cutscene_active()`, então
> scripts/câmera de gameplay não rodam cedo demais durante cutscene. Também foi aplicado patch de
> input para enviar eixo por `setJoyAxis` mesmo sem SDL GameController físico. Use `STATUS.md`, seção
> "SESSÃO 7 (2026-06-22)", como fonte atual.
>
> Artifacts:
> - `~/lcs-build/shot-fe25-finishpos-1.png` = prova visual de gameplay real após 2 cutscenes.
> - `~/lcs-build/run-fe25-finishpos-1.log` = run limpo com FINISHPOS.
> - `~/lcs-build/run-fe25-finishpos-padbridge-crash-1.log` = não usar PADBRIDGE_DIRECT.
> - `~/lcs-build/shot-fe25-cutgate-camprocess-1.png` = prova visual da cutscene 3D real.
> - `~/lcs-build/run-fe25-cutgate-camprocess-1.log` = 120s sem crash com FE25 protegido.
> - `~/lcs-build/run-fe25-long-clean-1.log` = crash novo no renderer no handoff.
> - `~/lcs-build/run-fe25-no-camprocess-1.log` = sem CAMPROCESS a cutscene congela no meio e o
>   device reinicia; com CAMPROCESS ela progride, mas crasha no fim. A pista atual é controlar o
>   handoff de câmera/cutscene antes do renderer entrar em estado misto.

Leia também `STATUS.md` (histórico s1→s4, detalhadíssimo) e as notas internas do projeto.

═══════════════════════════════════════════════════════════════════════════════
## 🎉 ESTADO ATUAL: HUD + GAMEPLAY RENDERIZAM. Falta só o MUNDO 3D (geometria).
═══════════════════════════════════════════════════════════════════════════════
Prova visual: `~/lcs-build/MARCO-HUD-GAMEPLAY-2026-06-21.png` — HUD completo do GTA LCS
(radar/bússola, relógio 12:00, barra de vida, ícone de arma, $00000000, controles touch) +
nome da localização **"Atlantic Quays"** (Portland/LCS). Jogo em gameplay REAL: player spawnado
e persistente, câmera posicionada em coords reais (1250,-1157,12.7).

O QUE FALTA: a geometria 3D do mundo (prédios/ruas). Só o HUD/2D desenha; o viewport 3D fica escuro.
→ É problema do **PVS** (ver "BLOQUEIO" abaixo). Tudo o mais funciona.

═══════════════════════════════════════════════════════════════════════════════
## DEVICE / ACESSO
═══════════════════════════════════════════════════════════════════════════════
- IP: **<device-ip>** | login **root** (ssh).
- Mali-450 Utgard, GLES2, fbdev, ~832MB RAM. EmuELEC. ES masked (bom p/ teste).
- ⚠️ A TV fica PRETA depois que o lcs sai (ES masked, nada redesenha) — é normal, NÃO é wedge.
- **Swap PERSISTENTE 2GB no EEROMS**: `/storage/roms/swap2g.img` (auto-on no boot via
  `/storage/.config/custom_start.sh`). `/storage/roms` é **VFAT**. NÃO editar `autostart.sh`.
- Pasta no device: `/storage/roms/ports/lcs/` (lcs + libGame.so + libc++_shared.so +
  libVendor_mpg123.so + gamedata/data_main.wad + data_music.wad, ~2GB já transferidos).
- gdb + gdbserver disponíveis no device.

═══════════════════════════════════════════════════════════════════════════════
## ✅ REGRA DE OURO RESOLVIDA: NÃO TRAVA MAIS (device de pé 4.5h sem reboot)
═══════════════════════════════════════════════════════════════════════════════
O "travamento + tela preta + reboot" entre runs ERA o Mali não-liberado no exit. RESOLVIDO:
- `lcs_mali_teardown()` (jni_shim.c): glFinish REAL (dlsym, fura nosso no-op) + eglMakeCurrent(NULL)
  + eglTerminate ANTES do `_exit`. Sem isso o processo morre com jobs Utgard em voo → Mali wedge →
  próxima run herda → reboot. Chamado nos exits (LCS_MAXSECONDS/MAXFRAMES) e no hotkey SELECT+START.
- **Runs curtos**: `LCS_MAXSECONDS=N` = `_exit` limpo por TEMPO (sem kill externo → sem D-state).
- Harness pronto: **`run30.sh`** no device (mata lcs antes E depois, swap, watchdog backstop 55s só se
  travar DURO, poller heartbeat, env do gameplay). É só `ssh ... 'sh /storage/roms/ports/lcs/run30.sh'`.

═══════════════════════════════════════════════════════════════════════════════
## COMO RODAR (ciclo de 30s, copiar e colar)
═══════════════════════════════════════════════════════════════════════════════
```sh
# build no PC:
cd ~/nextos_ports_android/ports/lcs && bash build.sh   # warnings de ld do SDL2 são NÃO-FATAIS
# deploy:
ssh root@<device-ip> 'kill lcs antigo'  # run30.sh já faz isso
scp lcs run30.sh root@<device-ip>:/storage/roms/ports/lcs/
# roda (sai limpo em ~26s, device fica usável):
ssh root@<device-ip> 'sh /storage/roms/ports/lcs/run30.sh'
# logs persistentes na SD (sobrevivem a tudo):
ssh root@<device-ip> 'grep "\[glstats\]" /storage/roms/ports/lcs/run.log | tail'
# screenshot do gameplay -> PNG:
scp root@<device-ip>:/storage/roms/ports/lcs/shot_gameplay.raw ~/lcs-build/
python3 ~/lcs-build/raw2png.py ~/lcs-build/shot_gameplay.raw ~/lcs-build/shot_gameplay.txt out.png
```
Config estável atual no `run30.sh` (= HUD + gameplay):
`LCS_START=newgame LCS_STARTFRAME=12 LCS_MAXSECONDS=26 LCS_GLSTATS=1 LCS_BOUNDSTREAM=1
 LCS_STREAMER_MAX=80 LCS_UNFADE=1 LCS_NOPVSCULL=1 LCS_NOON=1 LCS_NODOFADE=1`

═══════════════════════════════════════════════════════════════════════════════
## 🔴 BLOQUEIO ATUAL (atacar): MUNDO 3D não renderiza = PVS crasha
═══════════════════════════════════════════════════════════════════════════════
Na engine Leeds da LCS o mundo é STREAMADO via PVS (`cWorldStream` + `indust.img` 144MB +
zonas `indust.xml`), NÃO via placement IPL clássico. CONFIRMADO (RENDERDIAG): em gameplay rodam
RenderScene (clássico) E MattRenderScene (mobile) E ConstructRenderList, mas `nVis=0` nos dois →
**setores do mundo VAZIOS sem o PVS**. Então o PVS é OBRIGATÓRIO p/ o mundo 3D.

Habilitar o PVS (`LCS_NOPVS=0`) CRASHA. Localização precisa (gdb, `LCS_NOCRASHHANDLER=1`):
- `PVS::LoadPVSZones(j)` @libGame+0x565ed8 lê `indust.xml` (388807B, um `<scene>` com
  `<unit meter="0.01" name="centimeter">` + geometria, formato 3DS-export) via
  `LogicalFS_OpenBundleFile` → handle.getSize(vtbl+0x30=388808 OK) → `calloc(size+1)` →
  read(vtbl+0x10) → `TiXmlDocument::Parse(buffer)`.
- CRASH: scanner de nome/char do TiXml anda ALÉM do buffer (`ldrb w12,[x12,#1]` com x12 corrompido;
  num parser de char @libc 0x7f96..4f38). `x19=NULL`. **O offset do fault VARIA entre runs**
  (0x2d97 / 0x30b1) = **corrupção de heap** durante o parse do DOM gigante (388KB → milhares de nós).
- JÁ DESCARTADO (não adianta refazer): getSize garbage (é 388808, certo); AAsset_getBuffer
  faltando (implementei, não mudou); o `+1` no XML (removido, não mudou).

### O QUE FAZER (próxima sessão):
1. **gdb single-step no parse do TiXml** p/ achar a FONTE da corrupção (qual alloc/escrita estoura).
   Use `LCS_NOCRASHHANDLER=1` (deixa o gdb pegar o SIGSEGV). Script base: `gdb_pvs.sh` no device.
   Foque em: a alocação/escrita ANTES do scan que corrompe o terminador; ou se o TiXmlString::assign
   (TiXmlAttribute::Parse @0x7b9e60) usa o allocator do engine (CMemoryHeap) que estoura/overflow.
2. ALTERNATIVA: o engine foi compilado p/ bionic; talvez dependa do `strtod`/scanner do bionic. Testar
   interceptar strtod/strtof/sscanf e null-terminar defensivamente.
3. ALTERNATIVA: hookar `_Z24LogicalFS_OpenBundleFilePKci` (já tem stub diag `my_OpenBundleFile`) p/
   servir o XML por um handle NOSSO com vtable própria (getSize/read corretos do WadArchive) — descarta
   suspeita do FSWadFile do engine.
4. ALTERNATIVA: dumpar `indust.xml` pra disco (adicionar write em aa_open) e validar o XML / ver o
   conteúdo no offset do crash.

═══════════════════════════════════════════════════════════════════════════════
## FLAGS DE ENV (todas gated, jni_shim.c) — referência
═══════════════════════════════════════════════════════════════════════════════
GAMEPLAY/RENDER:
- `LCS_START=newgame` — fluxo REAL (CMenuManager::StartNewGame). (=force usa feobj+25 cru = loop re-init.)
- `LCS_NODOFADE` — pula DoFade()@0x520d1c em s9 (REMOVE o overlay preto = a "tela preta"). ESSENCIAL.
- `LCS_UNFADE` — GetScreenFadeStatus→0 SÓ em s9 (abre o gate de render; no menu CRASHA).
- `LCS_NOON` — CClock=12:00 (lighting de dia).
- `LCS_BOUNDSTREAM` + `LCS_STREAMER_MAX=N` — limita lglWaitForStreamerToFinishTasks (lglBufferCreator
  NUNCA "termina"; achado via STREAMDIAG). Sem o bound, InitialiseWhenRestarting trava no streamer.
- `LCS_NOPVSCULL` — zera dv* PVS (não resolve o mundo, mas inofensivo).
RUN/DEBUG:
- `LCS_MAXSECONDS=N` — exit LIMPO por tempo (+teardown). `LCS_MAXFRAMES=N` — idem por frame.
- `LCS_GLSTATS` — log/frame: dScr/dFbo/nVis/fade/cam/ped + rs/matt/crl (renderers). SEM fdatasync.
- `LCS_NOCRASHHANDLER` — não instala nosso handler (deixa gdb pegar o SIGSEGV).
- `LCS_NOPVS=0` — HABILITA o PVS real (default é no-op; com 0 = roda e CRASHA, ver bloqueio).
- DIAG: `LCS_RENDERDIAG`, `LCS_MATT2CLASSIC`, `LCS_OBFDIAG`, `LCS_STREAMDIAG`(install_hooks), `LCS_XMLPLUS1`.
- ⚠️⚠️ **NUNCA `LCS_GLTRACE` em gameplay** — faz fdatasync por op-GL → satura a SD FAT → FALSO "wedge"
  (foi o "BOSS FINAL streaming" fantasma da s2). Já desmascarado.

═══════════════════════════════════════════════════════════════════════════════
## MAPA TÉCNICO (o que JÁ está resolvido — NÃO refazer)
═══════════════════════════════════════════════════════════════════════════════
- Loader 2-módulos (libc++_shared + libGame), 0 imports não resolvidos. malloc→glibc (não é pool fixo).
- Driver JNI GTAJNIlib_* (jni_shim.c). EGL interceptado → SDL2-mali (engine cria surface em viewOnSurfaceChanged).
- WAD: XOR {0xAF,0x66} → WadArchive próprio na aa_open/w_fopen. AAsset_read/seek/getLength/getBuffer/close.
- Boot state-machine: OS_ApplicationTick s0-9. feobj+21=gate tick. Estado real = *(text_base+0x7fd000+2232).
- NEW-GAME REAL: CMenuManager::StartNewGame(&FrontEndMenuManager) → DoSettingsBeforeStartingAGame → a
  engine sequencia GameStart(CGame::Initialise)+InitialiseWhenRestarting(spawn player) sozinha. NÃO forçar feobj+25.
- No-ops/hooks (install_hooks): cxa_guard, NVThreadSpawnJNIThread, PVS::LoadPVSZones (DESLIGAR p/ mundo 3D!),
  SocialClub/cloud, SetupPostProcessShaders (TRAVA Utgard — manter no-op), CCutsceneMgr::DeleteCutsceneData,
  RenderMenus skip@s9, DoFade skip@s9 (LCS_NODOFADE), GetScreenFadeStatus→0@s9 (LCS_UNFADE), mip-skip ETC.
- lcs_mali_teardown no exit (resolve wedge entre runs).

═══════════════════════════════════════════════════════════════════════════════
## ARQUIVOS
═══════════════════════════════════════════════════════════════════════════════
- Código: `~/nextos_ports_android/ports/lcs/src/` — build `bash build.sh` → `lcs`.
- `~/lcs-build/`: libGame.so (análise), MARCO-HUD-GAMEPLAY-2026-06-21.png, raw2png.py, shots, gdb out.
- Toolchain objdump: `~/NextOS-Elite-Edition/build.*Amlogic-old.aarch64-4/toolchain/bin/aarch64-libreelec-linux-gnu-objdump`
  (o objdump do sistema NÃO reconhece aarch64). Mapear offset→função: `nm -D -C libGame.so`.
- Refs em disco: `~/re3-src` (re3 GTA3, MESMO renderer: CRenderer/RenderScene/ConstructRenderList),
  `~/NextOS-Elite-Edition/sources/re3`+`reVC`, `~/nextos_ports_android/ports/bully` (MESMA libGame engine).
- APK fonte: `~/Downloads/gta-liberty-city-stories-2.4.379-mod-menu-5play.apk`.

═══════════════════════════════════════════════════════════════════════════════
## LIÇÕES (não repetir os erros)
═══════════════════════════════════════════════════════════════════════════════
1. NUNCA LCS_GLTRACE em gameplay (fdatasync satura SD = falso wedge). Use LCS_GLSTATS (sem fsync).
2. Saída SEM teardown do Mali = wedge entre runs. SEMPRE lcs_mali_teardown antes do _exit.
3. so_symbol (=so_find_addr) ABORTA FATAL se não acha → use so_find_addr_safe p/ símbolos opcionais.
   Conferir o COMPRIMENTO no mangling (FindPlayerPed=_Z13..., não _Z12...).
4. UNFADE/NODOFADE só em state 9; no menu crasham.
5. "tela preta" no gameplay era o overlay do DoFade, NÃO FBO/streaming/lighting.
