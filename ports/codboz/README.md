# COD: Black Ops Zombies — Mali-450 (NextOS)

Port so-loader do **Call of Duty: Black Ops Zombies** (build Android *Marmalade s3e*)
rodando no **Mali-450 Amlogic (Utgard/fbdev)**, sob EmuELEC/NextOS.

> **Adaptação NextOS / Mali-450.** O loader original é de
> **[Producdevity/cod-boz-port](https://github.com/Producdevity/cod-boz-port)**
> (licença **MIT**, © 2026 Producdevity) — todo o crédito da arquitetura do
> so-loader Marmalade s3e (host s3e, decoder LZMA, extrator de APK, empacotamento
> PortMaster) é dele. Aqui ele foi **adaptado para o Mali-450 Amlogic**: recompilado
> no toolchain armhf, apontando pro **SDL2 do sistema**, com **fullscreen por
> free_scale do OSD**, **D-pad como movimento** e um **conjunto de correções de
> áudio**. Os dados do jogo (APK/`.dz`/`boz.s3e`) são **BYO** — você fornece a partir
> de uma cópia Android que **possui legalmente**. Este diretório **não** contém dados
> do jogo.

Validado no Mali-450 (S905, GLES1 em hardware — casa com o `blackops_gles1.dz` do jogo):
**boot, New Game, gameplay, fullscreen 720p, áudio e controle — jogável.**

## O que foi adaptado (vs. upstream)

Tudo isto está em **`src/nextos-fixes.patch`** (aplicar sobre a fonte do upstream e
recompilar com `scripts/build-docker.sh`). O binário já corrigido é o
`codboz_s3e_loader` deste diretório.

### Controle — `s3e_input.c`
- **D-pad = andar** no game-mode. O loader lê **só `SDL_GameController`**; no game-mode o
  movimento original só saía do stick analógico esquerdo. Handhelds **dpad-only** (ex.:
  "USB Gamepad" `0810:0001`) não têm esse stick → não andavam. Agora o movimento também
  aceita o D-pad, espelhando a lógica do cursor.
- **Select+Start = sair** do port (hotkey padrão, direto no binário).

### Áudio — `s3e_audio.c` (+ header)
- **Tiros/SFX que paravam de sair com o tempo.** A assinatura real da s3e é
  `s3eSoundChannelRegister(canal, cbid, fn, userData)` — 4 args; a de 3 do upstream
  **descartava o ponteiro do callback**. A engine registra **END_SAMPLE** em cada canal
  para reciclar as vozes; sem receber o aviso ela achava os 24 canais eternamente
  ocupados e **silenciava sons novos**. Agora o loader guarda os callbacks por canal e
  despacha **END_SAMPLE no *pump* do frame** (`audio_pump` em `eglSwapBuffers`) — nunca
  re-entrante de dentro do próprio `ChannelPlay`, o que corrompia o estado da engine.
- **Música de fundo sumindo.** Na API `s3eAudio`, `repeat=0` = **loop infinito**; o
  upstream tocava 1 vez e parava. Corrigido em `s3eAudioPlay`/`PlayFromBuffer`.
- **Resolução de caminho de música** estendida para `assets/blackops-music/` e
  `assets/deadops-music/` (pedidos por nome puro não resolviam).
- Auditoria opt-in: `CODBOZ_AUDIO_LOG=1` loga cada pedido de som/música/registro.

### Empacotamento — `CODBOZ.sh`
- `LD_LIBRARY_PATH=/usr/lib:…` → **SDL2 do sistema** (o SDL2 bundled do upstream detecta
  "container" e desliga o udev, deixando de enumerar o pad).
- Garante `SDL_GAMECONTROLLERCONFIG_FILE` (gamecontrollerdb do PortMaster) → qualquer pad
  mapeado funciona (gptokeyb **não** serve aqui: a engine ignora teclado/mouse real).
- **Fullscreen** via `free_scale` do OSD Amlogic (o jogo é 640×480 nativo; o OSD escala pro
  painel). Aplicado após a surface existir; restaurado ao sair.
- Sem `gptokeyb`, sem `setsid`.

## Controles

Select = alterna **cursor / game-mode**. **Select+Start = sair.**

| Botão | Ação |
|--|--|
| D-pad | Andar |
| Right Stick | Olhar |
| A | Ação / correr |
| B | Recarregar |
| X | Coronhada |
| Y | Granada |
| L1 / L2 | Mira |
| R1 / R2 | Tiro |
| Start | Pausa |

## Dados do jogo (BYO)

Requer o APK Android de **Black Ops Zombies** (`com.activision.callofduty.blackopszombies`,
v1.0.11). O loader é calibrado byte-a-byte para o `boz.s3e` da build **stock**
(SHA-256 `359ee68b6e0a3a66e921ec9b955b290dedb93135fd3c20904bc1bb6f47b5499d`) — os remendos
de fault do loader têm offsets fixos dessa build. Coloque o APK em
`ports/codboz/apk/game.apk` e rode `codboz_setup` (extrai o payload Marmalade e baixa os
`.dz` de textura/GLES1 do CDN de recursos).

> **Limitação conhecida:** com uma APK que **não** seja a stock `359ee68`, o loader boota
> mas pode crashar em pontos onde os offsets não alinham (ex.: *Quit → menu*). Use a stock
> para o fluxo completo.

## Build

```bash
scripts/build-docker.sh   # do repo upstream: toolchain armhf (Debian buster) + make
```

## Créditos

- **[Producdevity](https://github.com/Producdevity)** —
  [cod-boz-port](https://github.com/Producdevity/cod-boz-port), o loader Marmalade s3e
  original (MIT). Esta pasta é uma adaptação dele para o Mali-450.
- **Ideaworks3D / Activision** — release Android original.
- **LZMA SDK** (domínio público) — decoder usado pelo extrator de APK.
- **SDL2 / SDL2_mixer** — janela, áudio e entrada.

## Licença

O loader mantém a licença **MIT** do upstream (ver `LICENSE.upstream`). As modificações
NextOS aqui seguem a mesma licença MIT. Os dados do jogo continuam sendo da Activision e
**não** são redistribuídos.
