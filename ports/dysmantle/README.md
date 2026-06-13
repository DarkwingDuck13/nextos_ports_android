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

## BYO-data (dados NÃO inclusos) — tudo automático

Este zip tem só o port (sem os dados do jogo). Você precisa da **sua cópia legal**
do **APK do DYSMANTLE Android 1.4.1.12**. O processo é automático:

1. Copie o seu `.apk` para a pasta do port:
   ```
   ports/dysmantle/SEU_DYSMANTLE.apk
   ```
2. Abra **DYSMANTLE** na lista de ports. Na **1ª vez** o launcher faz tudo sozinho:
   - **extrai** do APK o `libNativeGame.so`, o `libc++_shared.so` e os `assets/` (~700MB);
   - **conserta as texturas** com o `fixpak` (veja abaixo) — leva ~1-2 min, só uma vez;
   - abre o jogo.
3. Pode demorar alguns minutos na 1ª abertura (extração + conserto). As próximas são
   instantâneas (um marcador `.textures_fixed` evita repetir).

> 🧊 **Por que o conserto?** Vários APKs vêm com as texturas JPEG/PNG **vazias** dentro
> do pak (só a versão `.ktx` ETC2 tem dados) → personagem/itens/chão sairiam **brancos**.
> O `fixpak` (incluído) decodifica o ETC2 **no próprio device** e regrava as texturas
> em JPEG/PNG, usando a libturbojpeg/libz do seu CFW. **Não precisa de PC nem Python.**
> (O `tools/fix_empty_textures.py` é só a versão de PC, opcional, p/ quem preferir.)

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
