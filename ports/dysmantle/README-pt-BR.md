# DYSMANTLE — NextOS / PortMaster (ARM64)

Port nativo AARCH64 (so-loader, v6). Nenhum dado do jogo e distribuido com
este port — ele usa a SUA copia legal do DYSMANTLE (Android).

> DYSMANTLE e um jogo PAGO da 10tons no Android. Este port NAO inclui o
> jogo e NAO burla nenhuma compra. Voce fornece a sua propria copia legal.

Versao necessaria: **1.4.1.12** (arm64-v8a). Outras versoes nao suportadas.

------------------------------------------------------------------------
## 1. Pegue os arquivos do jogo — APK completo

Copie o seu **APK do DYSMANTLE v1.4.1.12** (arquivo unico, ~800 MB) para:

    roms/ports/dysmantle/

So isso — a primeira abertura mostra a tela de SETUP e extrai tudo dele
(o APK e apagado apos o setup dar certo).

Sem PC? Use o "SAI (Split APKs Installer)" no proprio celular para
exportar o APK da sua copia instalada e copie o arquivo.

------------------------------------------------------------------------
## 2. Primeira abertura — etapas do setup

A primeira execucao mostra a tela de setup com progresso:

    ETAPA 1/4  biblioteca do jogo
    ETAPA 2/4  extraindo dados (~734 MB, contador em MB ao vivo)
    ETAPA 3/4  conserto de texturas (alguns minutos)
    ETAPA 4/4  otimizacao de texturas (SO em alguns devices de pouca
               memoria — essa e LENTA, ate ~15 min. Nao desligue!)

O jogo abre sozinho quando o setup termina. Isso acontece so uma vez.

------------------------------------------------------------------------
## 3. DLCs (The Underworld / Doomsday / Pets and Dungeons)

Se voce TEM DLCs no Android, copie o seu SAVE do Android (com o progresso
do DLC) para:

    roms/ports/dysmantle/gamedata/10tons/DYSMANTLE/save/0/

So destravam os DLCs que o seu save comprova. Nada que voce nao possui
e destravado.

------------------------------------------------------------------------
## 4. Controles

Controle padrao. **SELECT + START = sair** de volta ao menu.

------------------------------------------------------------------------
## 5. Devices / notas tecnicas

- Um binario universal (glibc >= 2.27): ArkOS, dArkOS, ROCKNIX, EmuELEC,
  NextOS, muOS, Knulli e outros CFWs aarch64.
- Device de pouca memoria (classe R36S) com swap/zram usa streaming de
  textura em qualidade NATIVA; sem swap, um cache de otimizacao e gerado
  uma unica vez no setup (etapa 4/4).
- GPUs GLES2 e GLES3 suportadas (automatico).

Problemas? Apague `roms/ports/dysmantle/assets/` e os marcadores ocultos
(`.textures_fixed`, `.etc1_scale`), recoloque o APK e abra de novo para
refazer o setup.
