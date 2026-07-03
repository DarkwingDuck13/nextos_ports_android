# COD: Black Ops Zombies — adaptação NextOS Mali-450

Adaptação para **Mali-450 Amlogic (EmuELEC/NextOS)** do port so-loader Marmalade s3e
**[Producdevity/cod-boz-port](https://github.com/Producdevity/cod-boz-port)** (MIT). Este
diretório contém **apenas nossa adaptação** — sem dados do jogo (APK/.dz/boz.s3e).

Validado no device `.90` (S905, 832MB+swap, `libMali.m450.so` = GLES1 em hardware, casa
com o `blackops_gles1.dz` do jogo). Testado: **boot, New Game, gameplay, fullscreen,
áudio e controle — tudo OK**.

## O que mudamos vs upstream

1. **`CODBOZ.sh`** — launcher para nossa base:
   - `LD_LIBRARY_PATH=/usr/lib:...` → usa o **SDL2 do sistema**. O SDL2 bundled do porter
     detecta "container" e desliga o udev, deixando de enumerar o pad.
   - Garante `SDL_GAMECONTROLLERCONFIG_FILE` (gamecontrollerdb do PortMaster) → o loader lê
     **só SDL_GameController**, então qualquer pad precisa de mapping (gptokeyb NÃO serve aqui).
   - **Fullscreen** via `free_scale` do OSD Amlogic (o jogo é 640x480 nativo; o OSD escala
     pro painel). Aplicado ~4s após lançar; restaurado ao sair.
   - Sem gptokeyb, sem setsid.

2. **`src/nextos-dpad-movement.patch`** — patch em `src/s3e_input.c`:
   - No game-mode, o **movimento agora aceita D-pad** (além do stick analógico), espelhando
     `input_update_cursor`. Handhelds **dpad-only** (ex.: "USB Gamepad" 0810:0001) não têm
     stick esquerdo e sem isto não andavam. Recompilar via `scripts/build-docker.sh` do upstream.

3. **`codboz_s3e_loader`** — loader já recompilado (armhf) com o patch acima.

## Controles

Select = alterna cursor/game-mode. Game-mode: **D-pad = andar**, right-stick = olhar,
A = action/sprint, B = reload, X = melee, Y = granada, L1/L2 = mira, R1/R2 = tiro, Start = pause.

## APK: use a STOCK `359ee68…`

O loader do porter é calibrado **byte-a-byte** para o `boz.s3e` da APK stock
(SHA-256 `359ee68b6e0a3a66e921ec9b955b290dedb93135fd3c20904bc1bb6f47b5499d`). Os remendos de
fault (bucket-allocator, null-buffer, empty-string) têm offsets fixos dessa build.

APKs diferentes desalinham esses offsets e crasham em pontos variados:
- `v1-0-111` (SHA 5c38f3ee) → SIGSEGV no **New Game** (offset 0x374074).
- MOD `v1.0.11MOD` (SHA 1dce43ef) → passa o New Game mas SIGSEGV no **Quit→menu**
  (offset 0x25b05c; `r3=0x30303030`="0000", provável string adulterada pelo mod).

**Recomendado: APK stock `359ee68`** — com ela os remendos do loader alinham e o fluxo
completo (inclusive Quit→menu) funciona. Copie para `ports/codboz/apk/game.apk` e rode o
`codboz_setup` (extrai `boz.s3e.unpacked` + baixa `blackops_etc.dz`/`blackops_gles1.dz` do CDN).

**Limitação conhecida** (só com a MOD): o "Quit to main menu" dentro do jogo crasha
(tela preta + cursor). Contorno: saia do port pelo hotkey/ES e relance = menu fresco.
