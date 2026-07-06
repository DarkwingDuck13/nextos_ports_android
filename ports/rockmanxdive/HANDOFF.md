# ROCKMAN X DiVE Offline - handoff 2026-07-06 (revisao Claude)

## Regra de device

- Usar somente `192.168.31.90` (EmuELEC, kernel 3.14.79, Mali-450 Amlogic-old).
- Nao usar `.114` nem `.79`.
- Path remoto: `/storage/roms/ports/rockmanxdive`.
- Workspace local: `/home/felipe/nextos_ports_android/ports/rockmanxdive`.
- Depois de ~5-6 runs seguidos o device fica lento (throttle/estado CRIWARE residual):
  o boot chega a frames cada vez mais baixos. Se aparecer hang cedo (f<200) ou
  "Killed" ainda carregando `libcri_ware_unity.so`, deixe o device descansar e re-rode.

## RESUMO DA CAUSA RAIZ (o que estava errado no diagnostico anterior)

O handoff anterior dizia "fallback ABM nao validado porque o boot nao chamou
OrangeBootup.Start" e culpava flakiness. **A causa real nao era flakiness: o comando
de teste omitia 4 flags que INSTALAM os hooks essenciais.** Sem eles nada do
`ABM_GOOD_FALLBACK`/`UI_SUB_COPY` tem onde agir e o boot managed nem sobe.

Comparando o log bom (`logs/rxd_logtxt_title_norm_180s.txt`, que chegava a f=450) com
os runs que falhavam, os 4 flags ausentes eram:

- `TER_RXD_ABSPY=1`            -> instala 63/65 hooks do AssetsBundleManager
                                  E os hooks de cena (LoadSceneAsync/AsyncOperation).
                                  Banner esperado: `[RXD_ABSPY] 63/65 hooks instalados`.
- `TER_RXD_UI_GUARD=1`         -> instala hooks do UIManager (Awake/Update/etc).
                                  Banner: `[RXD_UI_AWAKE] hooks UIManager basicos instalados ... guard=1`.
- `TER_RXD_PRELOAD_NULL_GUARD=1` -> guards de preload.
                                  Banner: `[RXD_PRELOAD_NULL] guards instalados=3`.
- `TER_RXD_AUDIO_BOOT_FAKE=1`  -> faz `AudioManager.Init` PULAR `LoadAcf`/`Preload`.
                                  Sem ele: crash SIGSEGV DETERMINISTICO em f=120 no
                                  caminho `rxd_boot_start_cr_hook -> rxd_audio_init_hook
                                  -> rxd_audio_loadacf_hook -> rxd_owrl_load_hook` (CRIWARE
                                  ACF chama delegate lixo -> blr). Banner bom:
                                  `[RXD_AUDIO] Init fake: IsInitAll/IsInitSystemSE=1, pula LoadAcf/Preload`.

Com os 4 flags: OrangeBootup.Start dispara em f=4, passa o crash de audio (f=120),
carrega designs/fonts/texturas, e ENTRA na cena splash (`ChangeScene now=splash f=290`).
Verificado no device (run "run4").

## O BLOQUEIO DO TitlePolicy (a tela preta documentada)

No f=450, ao entrar na splash, o jogo instancia um `AssetsBundleManager` NOVO e VAZIO
(`Awake self=... manifest=(nil)`) que substitui o manager bom do boot (que tinha o
manifest e resolvia tudo em f=100-260). O DontDestroyOnLoad nao preserva o singleton na
troca de cena neste port (mesmo problema do UIManager, cujo antigo aparece `src_alive=0`).
Resultado: `GetBundleID("ui/ui_titlepolicy")` roda no manager vazio -> `abid=(nil)` ->
asset nil -> `ArgumentException: The Object you want to instantiate is null`.

Fix (ja no codigo, so precisa dos flags):
- `TER_RXD_GAAL_TITLE_NORM=1` + `TER_RXD_ABM_GOOD_FALLBACK=1`
  -> quando o manager corrente nao tem manifest, roteia GetBundleID/GetAssetAndAsyncLoad
     para `g_rxd_abm_good` (o manager do boot capturado, que tem o manifest completo).
- `TER_RXD_UI_SUB_COPY=1` + `TER_RXD_UI_SUB_DEAD_FROM=440`
  -> copia as refs do UIManager bom morto para o novo vazio da splash.

Observacao: em f=100-260 o boot manager É o `g_rxd_abm_good` (self==good), entao o
branch de fallback do GetBundleID nao dispara ali (condicao `good != self` falsa) —
o fallback so age no manager vazio da splash. Ele NAO causa o crash de f=120 (esse era
o audio/ACF).

## ANTES DA SPLASH TERMINAR: ativacao de cena presa em 90%

Sem o pump de cena o `OrangeSceneManager.OnStartChangeScene` fica preso em `state=3`
(`cur=WaitForSeconds`) para sempre: o `LoadSceneAsync` da splash para em `progress=0.900`
porque o Unity segura a ativacao ate `allowSceneActivation=true`. No log bom quem destrava
é o pump `[RXD_LOADSCENE] pump` + `[RXD_ASYNC] set_allowSceneActivation value=1` +
`[RXD_MSCENE] SetActiveScene`. O op é capturado automaticamente pelo hook USCENE do jogo
(via ABSPY), entao NAO precisa de `TER_RXD_LOADSCENE_INDEX`. Flags:

- `TER_RXD_LOADSCENE_PUMP=1 TER_RXD_LOADSCENE_PUMP_BG=1 TER_RXD_LOADSCENE_PUMP_FINAL=1`
- `TER_RXD_LOADSCENE_ACTIVATE=1 TER_RXD_SCENEVT_GUARD=1`
- `TER_RXD_LOADSCENE_PUMP_FROM=305 TER_RXD_LOADSCENE_PUMP_PERIOD=10`
- `TER_RXD_SET_ACTIVE_MANAGED=1 TER_RXD_SET_ACTIVE_MANAGED_FROM=395`

(from/period sao aproximacoes do log bom; ajustar se a splash carregar em outro frame.)

## COMANDO COMPLETO (rodar em device descansado)

```sh
cd /storage/roms/ports/rockmanxdive
# matar instancias orfas antes (regra):
for p in $(ls /proc | grep -E '^[0-9]+$'); do e=$(readlink /proc/$p/exe 2>/dev/null); \
  case "$e" in *rockmanxdive*) kill -9 $p;; esac; done; sleep 2

nice -n 19 timeout -s KILL 240 env \
 CUP_DLLOG=1 CUP_EGPLOG=0 TER_RENDERRET=1 \
 TER_CHOREO=1 TER_CHOREO_INLINE=1 TER_CHOREO_NATIVE=1 \
 TER_RXD_SKIPWAIT=1 TER_RXD_CRI_FAKE=1 \
 TER_RXD_ABSPY=1 TER_RXD_UI_GUARD=1 TER_RXD_PRELOAD_NULL_GUARD=1 \
 TER_RXD_AUDIO_BOOT_FAKE=1 \
 TER_RXD_DRIVEBOOT=1 TER_RXD_FORCEAUDIOREADY=1 \
 TER_RXD_FORCECONSOLEREADY=1 TER_RXD_FORCECONSOLEREADY_FROM=35 \
 TER_RXD_FORCELOCALEREADY=1 \
 TER_RXD_LIFECYCLE_TICK=1 TER_RXD_LIFECYCLE_FROM=411 \
 TER_RXD_LIFECYCLE_RESUME=1 TER_RXD_LIFECYCLE_FOCUS=1 \
 TER_RXD_SPLASHDRIVE=1 TER_RXD_SPLASHDRIVE_FROM=420 TER_RXD_SPLASHDRIVE_UPDATE=1 \
 TER_RXD_FORCE_ONSTART=1 TER_RXD_DRIVESCENE_FROM=301 \
 TER_RXD_LOADSCENE_PUMP=1 TER_RXD_LOADSCENE_PUMP_BG=1 TER_RXD_LOADSCENE_PUMP_FINAL=1 \
 TER_RXD_LOADSCENE_ACTIVATE=1 TER_RXD_SCENEVT_GUARD=1 \
 TER_RXD_LOADSCENE_PUMP_FROM=305 TER_RXD_LOADSCENE_PUMP_PERIOD=10 \
 TER_RXD_SET_ACTIVE_MANAGED=1 TER_RXD_SET_ACTIVE_MANAGED_FROM=395 \
 TER_RXD_UI_SUB_DEAD_FROM=440 TER_RXD_UI_SUB_COPY=1 \
 TER_RXD_GAAL_TITLE_NORM=1 TER_RXD_ABM_GOOD_FALLBACK=1 \
 CUP_FRAMES=900 TER_SHOT=1 TER_SHOT_FRAME=620 \
 ./run.sh
```

## CHECKLIST de validacao (grep no log.txt)

1. `OrangeBootup.Start ... f=4`                                (boot managed subiu)
2. `[RXD_AUDIO] Init fake: ... pula LoadAcf/Preload f=120`     (sem crash de audio)
3. `[RXD_ABSPY] 63/65 hooks instalados` / `[RXD_UI_AWAKE] ... guard=1`
4. `ChangeScene ... now=splash f=290`
5. `[RXD_ASYNC] set_allowSceneActivation op=... value=1`       (cena destrava do 90%)
6. `ChangeSceneComplete ... now=splash f~401`
7. `GetAssetAndAsyncLoad bundle=UI_TitlePolicy` seguido de
   `[RXD_ABM] TitlePolicy usando manager bom` e `title id abid=0x...` (NAO nil)
8. `[SHOT] ... nao-pretos=` >> 144 (imagem real) e `draws_acum` > 0

Se em (7) o `abid` continuar nil MESMO no manager bom, o nome do bundle esta errado:
testar variantes (`UI_TitlePolicy`, `ui/UI_TitlePolicy`, `ui/ui_titlepolicy`) — a
normalizacao atual chuta `ui/ui_titlepolicy` (rxd_abm_gaal_hook). Nesse caso, dumpar
os nomes do manifest do manager bom (hookar a iteracao do dict de bundles) e casar o real.

## Estado confirmado no device (2026-07-06)

- run4 (4 flags de hook + fallback, SEM pump de cena): boot OK, sem crash, chegou a
  `now=splash f=290`, travou em `OnStartChangeScene state=3` (LoadSceneAsync em 90%).
- run6 (config completa COM pump de cena, device descansado, ate f=895 limpo):
  progrediu MUITO mais:
    * `OnStartChangeScene` avancou 3 -> 4 -> 5 (`set_allowSceneActivation value=1` disparou);
    * `OrangeSplashSwitcher` foi CRIADO (self=0x7dd5d72420; antes era nil);
    * entrou em `Splash.OnStartSplash` e rodou ate `state=3`.
  Travou em `Splash.OnStartSplash state=3 cur=WaitForEndOfFrame`, esperando um
  SEGUNDO op de load preso em 90%: `[RXD_LOADSCENE] pump ... ptr=0x7ec0004e80 state=0
  progress=0.900 ready=0`. `[SHOT]` ainda preto (nao-pretos=144), draws=0.

## PROXIMO PASSO PRECISO (onde parou)

No log bom, o que fez a cena/op completar foi o SCENEOP force:
`[RXD_SCENEOP] progress force op=0x7e02d807e0 age=39 from=20` e depois
`[RXD_SCENEOP] isDone force op=... age=49 from=45` -> op vira `done=1 progress=1.0`.

No run6 esse force NAO dispara para o op novo `0x7ec0004e80` porque ele nao esta na
tabela de tracking (`rxd_sceneop_find` retorna -1) — o `rxd_async_done_hook` /
`rxd_async_progress_hook` (main.c ~5953-5990) so forcam ops rastreados. O pump acha o op
pela vtable guard, mas o tracking de SCENEOP e uma tabela separada alimentada pelo hook
USCENE (`rxd_usm_loadasync_hook`, g_rxd_manual_scene_op / rxd_sceneop_track). Esse segundo
op (provavelmente a cena/asset que a splash dispara depois do Capcom logo) nasce por um
caminho que nao passa pelo USCENE, entao nao entra no tracking.

Hipoteses para destravar (ordem de tentativa):
1. Fazer o pump/scenevt registrar no tracking de SCENEOP o op que ele acha pela vtable
   (nao so o do USCENE) — assim o progress/isDone force passa a valer para 0x7ec0004e80.
2. Ou baixar limiares e alargar o alvo do force para qualquer AsyncOperation com
   `progress>=0.9 && !allowSceneActivation` (cuidado p/ nao forcar ops legitimos).
3. Confirmar QUAL yield o `Splash.OnStartSplash state=3` espera (é a espera do Capcom
   logo/CriMana terminar? um `LoadSceneAsync` do titulo? um bundle?). Hookar/logar o
   corpo do estado 3 do `<OnStartSplash>d__11.MoveNext` para saber o que ele poll-a.

Depois que a splash chegar ao load de `UI_TitlePolicy`, o `ABM_GOOD_FALLBACK` ja deve
rotear pro manager bom (validar o checklist item 7).

A maquinaria toda ja esta no binario; nao houve mudanca de codigo — so faltou (a) os 4
flags de hook do boot e (b) fechar esse ultimo hop de coroutine da splash.

## >>> ATUALIZACAO: TitlePolicy RESOLVIDO + fluxo managed COMPLETO (run8) <<<

O 5o flag que faltava era **`TER_RXD_SCENEOP_FORCE=1`** (+ `TER_RXD_SCENEOP_DONE_FROM=45`).
Sem ele o op async da cena splash nunca era rastreado (`rxd_sceneop_store` so grava com
esse flag), entao `isDone/progress force` nao aplicava e o `LoadSceneAsync` ficava em 90%,
travando `Splash.OnStartSplash` em state=3.

Com ele (run8, confirmado no device, boot->f=1385 sem crash):
- `[RXD_SCENEOP] track op=...` + `progress force` + `isDone force` DISPARAM;
- `Splash.OnStartSplash` avanca 4 -> 5 -> 6 -> -1; **`ChangeSceneComplete=1`**;
- **TitlePolicy RESOLVIDO**: `[RXD_ABM] TitlePolicy norm mgr=0x7e54280ea0 ... abid=0x7e17c4c000
  name=ui/ui_titlepolicy hash=9874aaf976053752668c69474ac82ca8 size=10243 package=package0`
  -> abid NAO-nil (fallback pro manager bom funcionou);
- `[RXD_UILB] LoadUI.b__0 ... asset=0x7e5422cdc0(GameObject)` -> **asset NAO-nil, SEM
  ArgumentException**. A UI do titulo instancia.

=> O bloqueio original de "tela preta no TitlePolicy" esta RESOLVIDO. O comando completo
   (secao acima) + `TER_RXD_SCENEOP_FORCE=1 TER_RXD_SCENEOP_DONE_FROM=45` leva o jogo do
   boot ate a UI do titulo carregada.

## >>> BLOQUEIO REAL RESTANTE: pipeline de render nunca roda (draws=0 / swap=0) <<<

Mesmo com o fluxo managed completo, a tela continua preta porque **nada e desenhado nem
apresentado**:
- `draws=0`, `glDrawElements` ~0-1, `eglSwapBuffers=0` no run INTEIRO;
- `[RENDERRET] ret=0` todo frame -> `nativeRender` faz early-out e NUNCA chega ao swap.

IMPORTANTE: o log bom antigo (`title_norm`) TAMBEM tinha draws=0. Ou seja, **o render
nunca funcionou neste port** — o codex parou antes, no fluxo managed. Este e o proximo
grande alvo (separado, mais dificil).

Pistas capturadas (`TER_RENDERSTATE=1`, RSTATE em g_unity_base+0x182a000):
`b2992=0 b3036=1 b3040=1 b3048=1 b3068=2 b3069=0 p3056=(nil) app+340=1 app+341=0 app+344=0`.
- `p3056=(nil)`: ponteiro nulo no bloco de render-state (candidato: GfxDevice/surface/
  render-context nao inicializado) — investigar o que deveria estar em +3056.
- `nativeRender` retornando 0 = Unity acha que nao deve renderizar (pause/surface/gfx).

Alavancas de render ja no binario para tentar (nenhuma resolveu sozinha ate agora):
`TER_RXD_KEEP_RENDERING(+_DIRTY)`, `TER_RXD_RENDER_GATE(+_FROM/_PERIOD/_KEEP3048)`,
`TER_RXD_CAMERA_RENDER(+_FROM)`, `TER_GL_TESTRECT` (mas `vk_gl_end` NAO da swap — sem
eglSwapBuffers nada aparece no fbdev), `TER_RENDERSTATE`, `TER_SWAPLOG`.
Comparar com como Terraria FNA/Dead Space resolveram draws=0 (mem: Terraria=EGL/
eglGetProcAddress; DeadSpace=projecao/tamanho JNI zerado). O caminho e descobrir por que
`nativeRender` early-out e forcar o GfxDevice a submeter+apresentar (eglSwapBuffers).

Nota flakiness: o boot as vezes nao chama `OrangeBootup.Start` (boot=0) mesmo com flags
certos — re-rodar. E o device fica lento apos ~5-6 runs seguidos.

## >>> ATUALIZACAO CHAVE: O PRESENT/GL FUNCIONA (TESTRECT vermelho na tela) <<<

`TER_GL_TESTRECT=1` desenhou um retangulo por GL e o screenshot mostrou 36864 px
RGB(255,0,0) = **retangulo VERMELHO real na tela**. Logo GL + eglSwapBuffers + fbdev
present ESTAO 100% OK (no fbdev o Unity usa o EGL REAL do Mali direto; o SHOT e' capturado
no my_eglSwapBuffers que roda -> present acontece). => O bloqueio NAO e' present/EGL.
O bloqueio e' que o **Unity nao renderiza a CENA** (`draws` fica fixo em 3-13 = so' o boot
do engine). Lever candidato: `TER_RXD_CAMERA_RENDER=1 ..._FROM=100 ..._PERIOD=2` chama
Camera.Render() por frame (rxd_force_camera_render_tick, main.c ~6677).

CUIDADO: `CUP_GLES_MAJOR=2` parece INDUZIR boot=0 (OrangeBootup nao dispara) — 2 runs
seguidos falharam so' por adiciona-lo. O `-force-gles20` ja e' injetado via cmdline
(cmdline_fd, main.c ~847), entao provavelmente nao precisa do CUP_GLES_MAJOR.

RESULTADO do CAMERA_RENDER (testado com boot=1 + titleUI=1): `rxd_force_camera_render_tick`
ACHA 4 cameras ("Main Camera" en=1 active=1) e chama Camera.Render() em todas
(`[RXD_CAMRENDER] done len=4 rendered=4`), MAS produz **0 draw calls** — as cameras
renderizam VAZIO. `draws` fica 13 ate ~f=600 (boot/splash) e cai p/ 0 depois; `nativeRender`
segue `ret=0`. Ou seja: forcar Camera.Render() NAO desenha a cena.
  => Hipoteses do porque a camera renderiza nada:
     (a) A UI do titulo e' um Canvas ScreenSpace-OVERLAY -> NAO renderiza via Camera.Render()
         (overlay tem passe proprio no player-loop, que nao roda com ret=0). Testar:
         setar o Canvas p/ ScreenSpace-Camera, ou achar/chamar o passe de UI (Canvas.Render/
         UIR batch) direto.
     (b) O render-target/viewport das cameras esta invalido (p3056 nil).
     (c) Culling/layer: nada no frustum. (Main Camera x2 sugere cameras 2D/UI).
  => Proximo passo p/ imagem: descobrir por que a Main Camera desenha 0 e/ou destravar o
     passe de UI overlay (o titulo e' UI). O player-loop render precisa rodar de verdade
     (nativeRender ret=1 sustentado com conteudo) — KEEP_RENDERING sozinho nao basta.

## >>> CAUSA DA TELA PRETA (render) — analise por camadas (2026-07-06) <<<

O `nativeRender` do Unity roda mas quase nada aparece. Investigado com TER_RENDERSTATE
(bloco de render-state em g_unity_base+0x182a000) + CUP_DRAWCOUNT + CUP_GLES_MAJOR.
Sao TRES problemas independentes empilhados:

CAMADA 1 — `nativeRender` early-out (ret=0):
  Do f=5 em diante `[RENDERRET] ret=0` -> o player nao renderiza. `p3056` (ponteiro em
  +3056 do render-state) vira nil no f=2 e nao volta. `TER_RXD_KEEP_RENDERING=1
  TER_RXD_KEEP_RENDERING_DIRTY=1 TER_RXD_KEEP_RENDERING_FROM=0` seta app+340=1 (dirty)
  e faz `ret=1` voltar -> render continua rodando. (parcialmente resolvido)
  [Tentei tambem TER_RXD_REGFX: re-chamar nativeRecreateGfxState com janela ANW nova
   por geracao (g_anw_gen) p/ restaurar p3056 — NAO restaurou o ponteiro. Codigo fica
   no binario mas nao foi o lever certo.]

CAMADA 2 — GfxDevice criado como GLES3 (Mali-450 e GLES2/Utgard mas exporta simbolos
  GLES3 falsos):
  O `egl_shim_create_window` tenta ES3.0 primeiro e so cai p/ ES2 se o CreateContext
  FALHAR — no Mali o ES3 "passa", entao o Unity cria device GLES3 e desenha quase nada
  (`draws=3` fixo). Forcando **`CUP_GLES_MAJOR=2`** (contexto ES2) os draws sobem
  3 -> 13 (verts 189). (lever correto; MESMO fix do Mega Man X nesse device)

CAMADA 3 — SHADERS GLES3 no bundle (o bloqueio final da imagem):
  `strings sharedassets0.assets` mostra `#version 300` (GLES3) alem de `#version 100`.
  Num contexto ES2 esses shaders NAO compilam -> "shader compile fail" -> geometria
  desenhada com programa invalido = preto. O `my_glShaderSource` do dive só faz
  ALPHAFIX, **NAO traduz #version 300 es -> 100**.
  => O Mega Man X (MESMO device) resolveu isso TRANSPILANDO os shaders OFFLINE
     (ports/megamanx/transpile_shaders.py, data.unity3d) + hook MMX_XLATE de runtime.
     No dive os shaders estao em bundles assetpack CRIPTOGRAFADOS (decifrados em runtime),
     entao o caminho viavel e um TRADUTOR EM RUNTIME dentro de `my_glShaderSource`:
       - `#version 300 es` -> `#version 100`
       - vertex: `in`->`attribute`, `out`->`varying`
       - fragment: `in`->`varying`; declarar/rotear `out vecN frag` -> `gl_FragColor`
       - `texture(`->`texture2D(`, `textureLod(`->`texture2DLodEXT(`
       - remover `layout(...)`, `flat`, etc.
     e ligar o hook SEMPRE (hoje so' liga sob CUP_DRAWSPY/TEXHALF — ver linha ~3008).

RECEITA DE RENDER (a testar junto com o comando managed completo):
  `CUP_GLES_MAJOR=2` + `TER_RXD_KEEP_RENDERING=1 TER_RXD_KEEP_RENDERING_DIRTY=1
   TER_RXD_KEEP_RENDERING_FROM=0` + (tradutor de shader a implementar) + `CUP_DRAWCOUNT=1`.
  Diagnostico: `CUP_SHADERDUMP=1` despeja o GLSL que chega no glShaderSource (achar os
  #version 300 reais do jogo e validar a traducao).

STATUS: causa 100% mapeada. Camadas 1 e 2 tem lever conhecido (KEEP_RENDERING +
CUP_GLES_MAJOR=2, draws 3->13). Falta a camada 3 (tradutor de shader runtime) p/ imagem
de verdade — e' o mesmo muro que o Mega Man X teve, so' que aqui via runtime (bundles
criptografados).
