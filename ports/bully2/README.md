# Bully2

Novo port do **Bully: Anniversary Edition** com abordagem original-first.

Escopo inicial:

- alvo primeiro: Mali-450/fbdev;
- loader nativo para `lib/arm64-v8a/libGame.so` Android;
- ciclo Android/JNI reproduzido no Linux;
- `data_0.zip` a `data_4.zip` e `.idx` originais do APK;
- sem cache ETC, sem bake e sem cache externo de textura;
- `NvAPK` nativo do `libGame.so` por padrao (`BULLY2_NVAPK_MODE=native`);
- despejo nativo por `GameNative.implOnLowMemory` + `CGame::TidyUpTextureMemory`
  por padrao (`BULLY2_EVICT=onlow`, `BULLY2_LOWMEM_TIDYTEX=1`);
- launcher PortMaster limpo: extração BYO, patch do menu, controle e execução;
  sem matriz de textura, shadow, clarity, streaming ou reload no `.sh`;
- perfil inicial limpo no binario:
  - instalacao limpa usa `Textures=Medium` (`512`), independente da RAM;
  - o binario le `texture_profile.cfg` ou `BULLY2_TEXTURE_PROFILE`
    (`low`, `medium`, `high`) para diagnostico;
  - a troca real de `Textures` fica no binario/menu, sem matriz de downscale no
    starter.
- controle por eventos JNI nativos do jogo; `L2/R2` no layout validado voltam a
  trocar item/arma por tap sintetico em `AND_TouchEvent`, igual ao Bully 1.
- Clarity fica em `RS_High` por hook nativo.
- Shadows inicia em `Medium` por padrao no binario.
- Shadows ficam visiveis como `Off`, `Low`, `Medium`, `High`. Em Mali-450/Utgard
  o binario desliga automaticamente o SSAO quebrado: o valor bruto `High` continua ativo,
  mas o postprocess `pp_ssao` do Android, que crasha o driver ao vivo, nao e
  criado.
- O menu de Settings ganha a opcao `Textures`, abaixo de `Shadows`:
  `Low` = 256, `Medium` = 512, `High` = full. A troca chama o mesmo perfil GL em
  runtime, sem cache, texswap ou conversao offline.
- Abaixo de `Textures`, o menu ganha `Light`: `Off` carrega tudo, `Low`
  redireciona mapas `_s.tex`, `Medium` redireciona mapas `_n.tex` e `High`
  redireciona `_s.tex` + `_n.tex` para texturas pequenas validas do proprio
  jogo. O padrao e `Off`; a escolha fica em `light_profile.cfg` e o filtro atua
  no caminho nativo de abertura de assets.
- Ao trocar `Textures` durante o jogo, o perfil GL muda em runtime e a limpeza
  nativa roda, mas o unload total de `Texture2D` fica desligado por padrao. O
  teste com `ResourceManager::GetAllLoaded<Texture2D>()` +
  `Texture2D::AttemptUnload()` liberou RAM, mas deixou o render preto porque o
  jogo nao recarrega todas as texturas residentes imediatamente.
- A escolha de `Textures` fica salva em `texture_profile.cfg`. No proximo boot,
  o binario le esse arquivo antes do automatico; o patch do menu e recriado pelo
  helper limpo do launcher.
- A escolha de `Light` fica salva em `light_profile.cfg` e tambem e sincronizada
  no menu por `MenuSettings::UpdateOption`.
- O valor visual das linhas customizadas e sincronizado diretamente no row da
  UI. O patch nao grava `string(value)` numerico nessas linhas, evitando textos
  como `Medium1` ou `Low1`; `MenuSettings::UpdateOption` fica apenas como
  diagnostico via `BULLY2_MENU_NATIVE_UPDATE=1`.
- A linha `Textures` e aplicada por `assets/bully2_patch.zip`, gerado localmente
  a partir do `assets/data_4.zip` legal do usuario. O launcher nao modifica
  `data_4.zip` nem `data_4.zip.idx`. O patch inclui `resource_files.list` e o
  loader registra o zip por `ResourceManager::RegisterPatchZip`.

BYO data: coloque o APK/OBB legal do Bully v1.4.311 em
`ports/bully2/gamedata` na primeira execucao. O helper
`tools/extract-bully-data.sh` mostra o progressor, extrai `libGame.so`,
`libc++_shared.so` e `assets/data_*.zip(+.idx)`, gera
`assets/bully2_patch.zip` e so entao libera o jogo. O zip PortMaster deve
conter todo o port, mas nunca os dados proprietarios do jogo.

Este port pode usar o `ports/bully` antigo como referencia de fatos, mas a implementacao aqui deve continuar limpa e separada.

Debug pesado fica desligado por padrao. Use `BULLY2_STREAMLOG=1`,
`BULLY2_GLMEMLOG=1` ou `BULLY2_GLLOG=1` apenas para diagnostico curto.
O poll por arquivo `/tmp/bully_tex_profile` tambem fica desligado por padrao;
use `BULLY2_TEX_PROFILE_POLL=1` ou `BULLY2_TEX_PROFILE_FILE=/caminho` apenas em
teste controlado.
Ao trocar `Textures` em runtime, o binario usa reload seletivo nativo por
padrao (`BULLY2_TEX_RELOAD_ON_CHANGE=reload`, batch 1). O caminho bruto antigo
fica apenas para diagnostico:
`BULLY2_TEX_RELOAD_ON_CHANGE=attempt`.
Ao trocar `Light`, o padrao nao forca reload de texturas residentes: o perfil
novo vale para streams novos. O reload de Light fica apenas para diagnostico via
`BULLY2_TEX_LIGHT_RELOAD_ON_CHANGE`.
