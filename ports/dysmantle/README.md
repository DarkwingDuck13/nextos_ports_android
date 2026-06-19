# 🎮 DYSMANTLE → Mali-450 / aarch64 Linux (NextOS / PortMaster)

Port do **DYSMANTLE** (10tons NX, Android **v1.4.1.12**) rodando em **aarch64 / Linux /
PortMaster** via **so-loader** do `libNativeGame.so`. Mundo aberto de
sobrevivência/crafting, **OpenGL ES 2.0/3.0**, áudio **Oboe** e controle **Paddleboat**
nativos. Roda do **Mali-450 MP (Utgard) + fbdev** até **Mali novo + KMSDRM**.

---

## 🎮 Como instalar e jogar (BYO-DATA)

Este pacote **não contém os dados do jogo** — você fornece o seu APK legal.

1. Instale o port (PortMaster / copie a pasta pra `ports/`).
2. Coloque o seu **APK do DYSMANTLE v1.4.1.12** (que tem
   `lib/arm64-v8a/libNativeGame.so` + a pasta `assets/` com os `data*.pak`) dentro de
   **`ports/dysmantle/`**.
3. Abra **DYSMANTLE** na lista de Ports. Na **1ª vez** abre uma **janela** com **barra de
   %** que prepara TUDO de uma vez: extrai os dados (~800 MB), **conserta as texturas**
   (anti-branco) e **gera o cache ETC1 offline**. Essa parte **demora alguns minutos**
   (o cache é a parte pesada), mas é **UMA vez só**. Ao terminar, o APK é liberado e o
   **jogo abre sozinho — LIMPO**.
4. Da 2ª vez em diante abre direto no jogo, **sem conversão nenhuma em runtime**.

- 🧊 **Cache ETC1 OFFLINE = sem travadas (v5):** toda textura opaca é convertida pra ETC1
  **na instalação** (`texbake`, multi-thread + `nice`); em jogo o binário só **sobe a
  ETC1 pronta** (zero encode/decode de textura em runtime). Mapas de iluminação
  (normals/specular) ficam RGBA8 (pra luz não quebrar). Marcador: `.etc1_cached`.
- 🎮 **UM binário** universal: `dysmantle` (GLIBC velha, compilado no Docker) roda em
  **qualquer aarch64** (glibc ≥ 2.27: ArkOS/R36S até NextOS/X5M/2.30+). Sem detecção.
- **Texturas:** o `fixpak` preenche os .jpg/.png vazios na 1ª vez **e** tem rede de
  segurança no launcher → **nunca** abre branco/lavado. Marcador: `.textures_fixed`.
- **Controles:** gptokeyb (`dysmantle.gptk`); sticks/gatilhos analógicos direto do pad;
  D-pad = quick slots. **Sair: SELECT+START**.
- **Vídeo/áudio:** auto-detectados (KMSDRM / mali-fbdev; ALSA/Oboe).

---

## 1. O que é

Port via **so-loader**: carregar o `libNativeGame.so` (Android arm64, **GameActivity /
AGDK**) dentro de um ELF Linux glibc, emular o ambiente Android (JNI, GameActivity,
AAsset, Paddleboat, Oboe) e dirigir o loop do jogo com SDL2/EGL/GLESv2 do device.

- **Jogo:** DYSMANTLE **v1.4.1.12** (10tons NX, "API 16.26.06"). Entry `android_main`
  (GameActivity). APK certo: tem `lib/arm64-v8a/libNativeGame.so` (~15 MB) + `assets/`
  com `data.pak` (~570 MB) + `data-gfx1200.pak` + `data-localizations.pak`.
- **Este repo guarda só o CÓDIGO** (`ports/dysmantle/src/*`). Os dados/libs do jogo não
  entram no git (são do APK; BYO-data estilo PortMaster).

## 2. Os fixes críticos (com o PORQUÊ)

- 🔑 **Canary bionic (TLS):** a engine lê o stack-guard de `tpidr_el0+0x28`; sob glibc o
  slot caía em TLS de outra lib e mudava no meio → `__stack_chk_fail`. **Fix:** pad TLS
  `_Thread_local` de 256 B no exe (1º bloco após o TCB) → slot estável. Vale p/ QUALQUER
  so-loader bionic→glibc.
- 🏆 **MUNDO BRANCO:** os XMLs `*Shadows` (feature_level=2) eram pulados no target GL
  tier-1 → `nx_shader+44` (vertex format) = 0 → geometria criada com `alloc(0)` →
  chão/pedras/árvores invisíveis. **Fix:** `hook_getshader` degrada o nome até a variante
  que carrega (`Shadows/Reflections/…/Fur→Diffuse/Lit`).
- 🎨 **Texturas brancas/lavadas:** APKs modados deixam JPEG/PNG vazios no `.pak` (só o
  `.ktx` ETC2 tem dados). **Fix:** `fixpak` (decoder ETC2 puro em C) decodifica no device
  e reencoda JPEG/PNG dentro do `.pak`. Roda na janela de extração + rede de segurança no
  launcher.
- 🔊 **Áudio (Oboe real):** shim `OpenSLES→SDL2` com pump thread de 4ms; PCM float32→S16;
  `__system_property_get("ro.build.version.sdk")="25"`.
- 🎮 **Controle (Paddleboat nativo):** alimentado direto do C (deviceInfo + eventos
  GameActivity key/motion); sticks/gatilhos analógicos.
- ⚠️ **Dynamic Shadows = OFF** (crash no load em Utgard; sem efeito após o fallback).

## 3. Build

- **Nativo:** `ports/dysmantle/build.sh` (toolchain Amlogic-old
  `aarch64-libreelec-linux-gnu-gcc`; linka `-lSDL2 -lEGL -lGLESv2 -ldl -lm -lpthread`).
  Saída: `dysmantle`.
- **Compat (GLIBC_2.27):** `build_compat_gcc.sh` dentro de `debian:bullseye` (gcc-10):
  `docker run --rm -v "$PWD":/repo -v "$SYSROOT":/sysroot:ro debian:bullseye bash
  /repo/build_compat_gcc.sh` → `dysmantle.compat.gcc`.
- **fixpak:** `gcc -O2 -o fixpak src/fixpak.c src/etc2_decode.c src/jpeg_enc.c -ldl`.

## 4. Estado

✅ **Jogável** com mundo + efeitos + som + controle, do **Mali-450 (fbdev/Utgard)** ao
**KMSDRM**. Pacote BYO-DATA no padrão **Bully v9** (janela de extração + 2 binários).
Limitações: dynamic shadows OFF; device de 1 GB engasga em cenas densas (sem crash).
