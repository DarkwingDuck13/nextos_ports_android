# Bully2 Status

## 2026-06-29

Criado scaffold separado em `ports/bully2`.

Decisoes da base:

- foco inicial em Mali-450;
- manter assets e streaming originais;
- nao usar `BULLY_ETC*`, bake, cache, half-res, paging ou tuning de distancia;
- adaptar apenas o necessario para Android/Bionic/JNI/EGL/GLES2.

Arquitetura inicial:

- `main.c`: carrega `libc++_shared.so` e `libGame.so` via so-loader AArch64;
- `jni_shim.c`: executa lifecycle Android estatico do `GameNative`;
- `imports.c`: shims Bionic/NDK e compatibilidade GLES2 minima;
- `egl_shim.c`: contexto SDL2/Mali fbdev visivel;
- `asset_archive.c`: fallback indexed-compat; o modo padrao atual usa `NvAPK`
  nativo do `libGame.so`.

Proximo passo: build local, corrigir erros de compilacao e instalar no Mali-450 para primeiro log real.

## Primeiro teste Mali-450

Resultado no device `192.168.31.154`:

- carregou `libc++_shared.so` e `libGame.so` sem imports nao resolvidos;
- passou por `JNI_OnLoad`, `implOnInitialSetup`, activity/surface/resume;
- criou EGL via SDL2 `mali` em 1280x720;
- iniciou `GameMain`, `RenderThread`, `Sound`, `SCListener`, `SCUpdate` e `CDStreamThread`;
- chegou em gameplay 3D no campus com HUD/minimapa/personagem visiveis;
- capturas salvas localmente: `bully2-60.png`, `bully2-120.png`, `bully2-180.png`.

Controles:

- adicionado `bully2.gptk`;
- `Bully2.sh` agora inicia `gptokeyb` por padrao e exporta `BULLY2_INPUT=gptk`;
- o binario converte teclas/mouse do gptokeyb em eventos JNI `GameNative`;
- teste pelo launcher confirmou `gptokeyb -1 bully2 -c /roms/ports/bully2/bully2.gptk` e log `[pad] BULLY2_INPUT=gptk`.

## Streaming nativo / escola

Mudancas validadas:

- `BULLY2_NVAPK_MODE=native` virou padrao: nao hooka `NvAPKOpen/Read/Size`,
  deixando o `libGame.so` usar o proprio `NvAPK`/`ZIPFile`;
- o launcher registra `OS_ZipAdd data_0.zip` ate `data_4.zip`;
- `CDStreamThread` inicia normalmente;
- `emustation.service` foi mascarado no Mali-450 durante testes para ES nao
  disputar tela/input;
- modo `BULLY2_EVICT=native` agressivo foi testado e rejeitado: chamou
  `RemoveUnusedModelsInLoadedList + RemoveIslandsNotUsed + MakeSpaceFor` apos
  `LoadScene`, mas crashou logo depois da transicao (`sig=11 addr=0x10a8`);
- modo atual seguro: `BULLY2_EVICT=gc`, que hooka `LoadScene`, chama apenas
  `CTxdStore::GarbageCollect`, e registra counters sem despejo destrutivo.

Resultado do teste `gc`:

- processo continuou vivo apos `LoadScene` em gameplay;
- screenshot salvo: `bully2-gc-state.png`;
- `CStreaming`/`CTxdStore`/`TextureHeapHelper` exportados reportaram memoria
  de textura como zero, mesmo com RAM do sistema subindo, entao o proximo estudo
  precisa descobrir onde o build Android contabiliza/segura as texturas reais.

## Streaming / RAM sem swap

Instrumentacao nova:

- `imports.c` rastreia GL textures por `glGenTextures`, `glBindTexture`,
  `glTexImage2D`, `glCompressedTexImage2D`, `glTexStorage2D` e
  `glDeleteTextures`;
- logs pesados agora sao opt-in: `BULLY2_STREAMLOG`, `BULLY2_GLMEMLOG` e
  `BULLY2_GLLOG`;
- `Bully2.sh` ganhou watchdog para matar `bully2/gptokeyb` antes do SSH morrer.

Resultados no Mali-450, swap desligado:

- melhor padrao atual: `BULLY2_EVICT=onlow`,
  `BULLY2_LOWMEM_TIDYTEX=1`, `BULLY2_LOWMEM_TIDYTEX_FORCE=1`;
- passou por `LoadScene` e estabilizou sem swap em ~220 MB de GL vivo, com o
  device ainda acessivel por SSH;
- `BULLY2_TEX_BUDGET_HOOK=1` com budget 96 MB foi rejeitado: aumentou reload e
  nao reduziu o pico;
- `BULLY2_LOWMEM_PROCESS=1` foi rejeitado: aumentou GL/RSS;
- `BULLY2_EVICT_TXD=1` encontrou `RemoveNonReferencedTxds`, mas removeu 0 TXDs
  no cenario testado;
- `BULLY2_EVICT_MEMOBJ=1` chamou `MakeSpaceForMemoryObject`, mas causou mais
  reload/thrash e nao virou padrao.

Logs locais salvos em `ports/bully2/logs/`.

## Perfis adaptativos de textura

O scaler arbitrario `BULLY2_TEX_SCALE_PCT=70` foi rejeitado: gerou tela preta.
O caminho estavel e o half exato estilo `BULLY_TEX_HALF`.

Perfis atuais no binario/menu:

- instalacao limpa seleciona `Textures=Medium` (`512`),
  independente da RAM;
- `texture_profile.cfg` persiste `low`, `medium` ou `high` e tem prioridade no
  proximo boot;
- `BULLY2_TEXTURE_PROFILE=low|medium|high` fica apenas como diagnostico;
- a matriz antiga de `384`/`768`/`1024`/porcentagem saiu do starter;
- o `Bully.sh` nao exporta mais `BULLY2_TEX_HALF*`: o binario le o arquivo salvo
  e aplica `Low=256`, `Medium=512`, `High=full` sozinho.

Resultados historicos em Mali-450/1GB, swap desligado:

- `512`: validado pelo usuario em gameplay, entrou/rodou com perda perceptivel de
  qualidade; log mostrou economia acima de 130 MB de textura e device estavel;
- `768`: passou pelo primeiro `LoadScene`, swap 0, GL ~143 MB, pico ~147 MB,
  RAM disponivel ~327 MB;
- `1024`: passou pelo primeiro `LoadScene`, swap 0, GL ~153 MB em teste inicial,
  economia menor e qualidade mais proxima do original;
- `256`: passou pelo primeiro `LoadScene`, swap 0, GL ~45 MB, economia ~106 MB
  logo no inicio; fica como emergencia por perda visual alta.

## Clarity e shadows

Clarity:

- `BullySettings::GetResolutionDefault` fica hookado para `RS_High`.
- A troca durante o jogo foi validada sem crash no Mali-450.

Shadows:

- O menu mostra os niveis nativos `Off`, `Low`, `Medium`, `High`.
- `GetShadowLevel()` do jogo mapeia o valor bruto `3` para shadow level `2`,
  que e o nivel alto real do renderizador.
- O crash de `High` no Mali-450 foi isolado em
  `BullyGameRenderer::SetupPostProcess()`: o branch de valor bruto `>=3` cria o
  material/pass `pp_ssao`, e depois `RenderGame()` cai ao enfileirar parametro
  desse material.
- Correcao validada: em Mali/Utgard, `BULLY2_SHADOW_SSAO=0` evita apenas a
  criacao do `pp_ssao` durante `SetupPostProcess`, restaura o valor bruto para
  `3` em seguida e mantem `GetShadowLevel()` no caminho alto.
- Testes Mali-450: boot direto em `High` passou de 3900 frames; troca
  `Medium -> High` ao vivo passou de 4500 frames sem crash.

## Menu Textures

- Metodo antigo rejeitado: clonar `main.content.shadow` em runtime via
  `MenuSettings::InitWithScene` abre o menu, mas o sistema de script/parent da UI
  pode cair em `UIScene::GetParentByClass`/`SerializedResource::ReadClass`.
- Metodo validado: o launcher gera `assets/bully2_patch.zip` pequeno a partir do
  `assets/data_4.zip` legal do usuario. Esse zip contem apenas
  `resource_files.list` e `bully/MenuSettings.xml`, com a linha `Textures`
  abaixo de `Shadows`.
- O loader adiciona `bully2_patch.zip` depois de `data_0.zip`...`data_4.zip` e
  tambem registra o zip em `ResourceManager::RegisterPatchZip`. Nao se altera
  `data_4.zip` nem `data_4.zip.idx`; isso evita quebrar offsets do indice nativo.
- A opcao aparece em ingles como `Textures` e alterna `Low`, `Medium`, `High`.
- Mapeamento runtime: `Low` = perfil `256`, `Medium` = `512`, `High` = full.
- A escolha do menu e persistida em `texture_profile.cfg`; no boot seguinte o
  binario le esse arquivo antes do perfil automatico. O launcher apenas chama
  `tools/ensure-bully-menu-patch.sh` para recriar `bully2_patch.zip`.
- A troca passa por `apply_texture_profile_runtime()` e executa o despejo nativo
  ja validado (`OnLowMemory`, `TidyUpTextureMemory`, `TxdGarbageCollect`,
  `native_stream_evict`) sem gravar cache/texswap/conversao.
- O unload total de texturas residentes foi testado com
  `ResourceManager::GetAllLoaded<Texture2D>()` + `Texture2D::AttemptUnload()`.
  Ele libera a memoria GL, mas foi rejeitado como padrao: depois da troca o jogo
  fica preto porque nem todas as texturas residentes sao repedidas
  imediatamente pelo streaming.
- Padrao atual: mudar `Textures` altera o perfil GL em runtime, persiste a
  escolha e agenda reload seletivo das texturas residentes. O caminho validado
  usa `Texture2D::AttemptUnload()` e, na mesma textura/frame, chama
  `ResourceManager::Reload()`. O unload bruto total fica apenas em
  `BULLY2_TEX_RELOAD_ON_CHANGE=attempt` para diagnostico.
- O poll por `/tmp/bully_tex_profile` foi desligado por padrao para nao aplicar
  estado velho de testes no primeiro frame; agora so roda com
  `BULLY2_TEX_PROFILE_POLL=1` ou `BULLY2_TEX_PROFILE_FILE`.
- Build Docker glibc 2.30 validado no Mali-450 com hash
  `d2294cec95ccf534cd67895703c4ee64157283b52deb266c7c333c8d0ccdb403`;
  launcher recriou `assets/bully2_patch.zip`, loader carregou
  `[drv] OS_ZipAdd bully2_patch.zip`/`RegisterPatchZip bully2_patch.zip`, boot
  seguiu vivo, a opcao `Textures` apareceu visualmente no Settings e o log
  confirmou persistencia via `[texmenu] persisted profile=low`.
- Testes Mali-450 de reload em runtime:
  - `low -> medium`: 1222 texturas tentadas, GL 41 MB -> 3 MB, processo vivo;
  - `low -> high`: 1360 texturas tentadas, GL 45 MB -> 3 MB, processo vivo;
  - `high -> low`: 1367 texturas tentadas, GL 194 MB -> 3 MB, processo vivo.
  Resultado visual posterior: tela preta/sem repedir texturas; caminho marcado
  como diagnostico, nao padrao.
- Testes Mali-450 do reload seletivo novo:
  - `ResourceManager::Reload(tex)` sozinho nao mudou GL (`uploads/del` parados);
  - `AttemptUnload(tex)` + `ResourceManager::Reload(tex)` por textura reupou de
    verdade (`del` 0->22, `uploads` 764->786) e reduziu GL 9 MB -> 3 MB;
  - fila completa do menu (`High -> Low`, 112 residentes, batch 1) ficou viva
    ate frame 2400 sem crash.
- Fix de menu: `Textures` agora usa template proprio
  `SettingRowTextureOption`, com `Low`/`Medium`/`High` inicial no template. Isso
  evita herdar o `string(textvalue)="Test"` do template original.
- Fix de persistencia visual em runtime: o binario hooka
  `MenuSettings::InitWithScene`, `MenuSettings::Update` e usa
  `MenuSettings::UpdateOption("textures", label, id)` depois da troca e durante
  a tela de Settings. O arquivo salvo ja mudava para `high`; o bug restante era
  apenas o texto da UI voltando para `Medium`.

## Menu Light

- `Light` e uma opcao separada de `Textures`; nao faz downscale e nao cria cache.
- Padrao: `Off`.
- Mapeamento:
  - `Off`: carrega todos os mapas;
  - `Low`: redireciona mapas `_s.tex` (specular) para `bully/blacktexture.tex`;
  - `Medium`: redireciona mapas `_n.tex` (normal) para `bully/skinbase_n.tex`;
  - `High`: redireciona `_s.tex` e `_n.tex`, equivalente ao
    `BULLY_TEX_LIGHT=1` antigo, mas sem deixar asset ausente.
- Persistencia: `light_profile.cfg`, com override por `BULLY2_TEX_LIGHT`.
- Menu: o patcher agora gera `Textures` e `Light` no mesmo
  `assets/bully2_patch.zip`; as linhas customizadas nao gravam `string(value)`
  numerico, para evitar texto visual como `Medium1`/`Low1`.
- Filtro nativo: em modo `BULLY2_NVAPK_MODE=native`, o port hooka
  `OS_ZipFileOpen`, `NvAPKOpen` e `NvAPKOpenFromPack` apenas para redirecionar
  esses assets de detalhe quando o perfil pede. Em modo compat, o mesmo filtro
  roda em `nv_open`.

## Controles L2/R2

- Recuperado do Bully 1 o caminho de troca de item por touch: a build Android
  nao cicla item/arma pelos enums normais de gamepad `14/15`; ela espera o toque
  nos botoes touch do HUD.
- No modo `BULLY2_INPUT=gptk`, `f` e `g` agora disparam
  `_Z14AND_TouchEventiiii` nas coordenadas relativas validadas do slot de arma:
  `f` = anterior, `g` = proximo.
- O `bully2.gptk` mantem o mapeamento validado do Bully 1: fisico `L2` chega
  como `l1 = f`, fisico `R2` chega como `r1 = g`; `L1/R1` seguem em `u/i` para
  mira/disparo.
- Tambem foi coberto o fallback nativo SDL: trigger esquerdo/direito em eixo
  dispara o mesmo tap de item se algum device nao passar pelo gptokeyb.
- Para diagnostico, `/dev/shm/bully_tap` aceita `prev`, `next` ou `x y` e
  `BULLY2_TAP_LOG=1` loga cada tap.
- O metodo rejeitado foi retornar "arquivo nao existe" para `_s/_n`: no streaming
  real isso crashou em `sig=11 addr=0x10d5` logo apos a troca para `Low`, porque
  o jogo espera receber um `Texture2D` valido para o material.
- Runtime: a troca chama `apply_light_profile_runtime()`, persiste o valor pelo
  menu e nao forca reload residente por padrao; o novo perfil vale para assets
  streamados depois da troca. `BULLY2_TEX_LIGHT_RELOAD_ON_CHANGE` fica apenas
  como diagnostico.
- Testes Mali-450 com build glibc 2.30 hash
  `8f2d8526ff34e8133ef5045e0ccb952768dcc4f1f40c66e3026f00d1891ceee0`:
  - runtime `Off -> Low` no frame 1200: processo vivo ate frame 2100, swap 7 MB,
    redirects `_s.tex -> bully/blacktexture.tex`;
  - boot direto em `Low`: processo vivo ate frame 2100, swap 9 MB, redirects
    especulares no streaming inicial;
  - boot direto em `High`: processo vivo ate frame 2100, swap 13 MB, redirects
    `_n.tex -> bully/skinbase_n.tex` e `_s.tex -> bully/blacktexture.tex`;
  - runtime `Off -> High` no frame 1200: processo vivo ate frame 2100, swap
    18 MB, redirects de normal/specular apos a troca.
- Validacao do patch gerado pelo launcher:
  `SettingRowTextureOption name="textures" ... string(textvalue)="Medium"` e
  `SettingRowTextureOption name="light" ... string(textvalue)="Off"`, sem
  `string(value)` nas linhas customizadas.
- Ainda falta validacao visual longa pelo usuario em gameplay real para comparar
  a qualidade de `Off/Low/Medium/High`; a estabilidade basica de boot/runtime
  no Mali-450 ja foi validada.

## Launcher v11 limpo

- `Bully.sh` do pacote foi reduzido para 88 linhas.
- A extração BYO saiu do launcher e ficou em
  `tools/extract-bully-data.sh`, mantendo splash/progresso.
- A geração de `assets/bully2_patch.zip` saiu do launcher e ficou em
  `tools/ensure-bully-menu-patch.sh`.
- O launcher nao exporta mais textura, Light, Clarity, Shadows, streaming,
  reload, heap ou lowmem. Esses defaults agora ficam no binario:
  - `Textures=Medium` por padrao ou arquivo `texture_profile.cfg`;
  - `Light=Off` por padrao ou arquivo `light_profile.cfg`;
  - `Clarity=High`;
  - `Shadows=Medium`;
  - SSAO de shadow desligado automaticamente em Mali/Utgard;
  - stream distance `60` em Medium, `50` em Low, nativo em High;
  - reload seletivo de textura em batch 1 por padrao.
- Teste Mali-450 com launcher limpo:
  - `medium/off`: boot pelo `/roms/ports_scripts/Bully.sh`, vivo ate frame 1200;
  - `high/high`: boot sem env de perfil, binario leu os arquivos salvos, redirecionou
    `_n/_s` e ficou vivo ate frame 1200.
