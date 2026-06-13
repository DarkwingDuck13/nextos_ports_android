# DYSMANTLE → Linux aarch64 (NextOS / PortMaster)

Port via **so-loader**: o `libNativeGame.so` (Android arm64, engine 10tons NX) carregado
dentro de um ELF Linux glibc, com shims de JNI/EGL/OpenSLES/GameActivity. Roda do
**Mali-450 (GLES2/fbdev)** até devices novos (**GLES3/KMSDRM**) — a resolução segue o
framebuffer do device automaticamente.

- **Jogo:** DYSMANTLE **v1.4.1.12** (Android). Mundo aberto, crafting, "destrua tudo".
- **Áudio:** Oboe real → OpenSLES shim → SDL2 (pump thread dedicada, sem engasgo).
- **Controle:** Paddleboat nativo. Com PortMaster, o **gptokeyb** mapeia o pad do seu
  CFW (layout em `dysmantle.gptk`); sticks/gatilhos continuam **analógicos**.
- **Sair do jogo:** **SELECT+START**.

## BYO-data (dados NÃO inclusos)

Este zip tem só o port. Você precisa da **sua cópia legal** do DYSMANTLE Android 1.4.1.12:

1. Do seu APK/instalação, extraia:
   - `lib/arm64-v8a/libNativeGame.so`
   - `lib/arm64-v8a/libc++_shared.so`
   - a pasta `assets/` completa (data.pak, data-gfx1200.pak, data-localizations.pak, ...)
2. Copie tudo para a pasta `dysmantle/` do port:
   ```
   ports/dysmantle/libNativeGame.so
   ports/dysmantle/libc++_shared.so
   ports/dysmantle/assets/data.pak
   ports/dysmantle/assets/...
   ```
3. Abra **DYSMANTLE** na lista de ports.

> ⚠️ Alguns APKs modificados vêm com as texturas JPEG/PNG **vazias** dentro do
> `data.pak` (só as versões `.ktx` ETC2). Sintoma: erros "Not a JPEG" no log e UI sem
> imagem. Conserto no PC: `tools/fix_empty_textures.py` (precisa Python +
> `texture2ddecoder` + `Pillow`) decodifica os ETC2 e preenche os slots vazios do pak.

## Opções (edite no topo do `DYSMANTLE.sh`)

| Variável | Default | Efeito |
|---|---|---|
| `DYSMANTLE_TEXSCALE` | `1.3` | Reduz texturas por este fator (FPS/memória). `1.2`/`1.1` = mais leve; comente p/ qualidade total |
| `DYSMANTLE_SWAPINT` | `0` | vsync off (o pacing fica com a engine; evita trava em 30fps) |
| `DYSMANTLE_GLVER` | `2.0` | caminho de shaders ES2 (funciona também em GPU ES3) |

## Controles (gptokeyb, `dysmantle.gptk`)

| Pad | Jogo |
|---|---|
| Stick esquerdo | Mover (analógico) |
| Stick direito | Câmera (analógico, direto do pad) |
| A / B / X / Y | Ações (confirmar/cancelar/usar) |
| D-pad | Quick slots de item (4 direções) |
| L1 / R1, L2 / R2 | Mira/ataque/ciclar (gatilhos analógicos) |
| L3 / R3 | Funções de stick |
| START / SELECT | Pause / Mapa |
| **SELECT+START** | **Sair do port** |

## Requisitos

- aarch64, GLES2+, glibc ≥ 2.38 (CFWs recentes: NextOS, ROCKNIX, muOS, Knulli...).
- ~1GB RAM livre. PortMaster instalado (gptokeyb).

## Créditos

- Port/engenharia reversa: **felc18-blip** (NextOS Elite).
- Jogo: © 10tons Ltd — compre o jogo! (Android/Steam/consoles)
- Base so-loader: padrão dos ports Android do NextOS (syberia/lswtcs de mtojek, Apache-2.0).
