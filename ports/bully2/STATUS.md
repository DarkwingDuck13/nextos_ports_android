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
