# GTA SA aarch64 — Mapa de subsistemas (gráficos / gamepad / áudio)

Objetivo: loader próprio **idêntico ao port que roda liso, porém em NOSSO código aarch64**.
O port de referência (`~/gta-sa-deploy/gtasa/`) JÁ É aarch64 → "idêntico porém arm64" é
replicar o comportamento dele (mesma `libGTASA.so`, mesmos hooks, mesmo `config_gtasa.txt`).

## 🎨 GRÁFICOS — GLES2 nativo (SEM shim externo)
- `libGTASA.so` importa **71 chamadas GL, todas ES2** — **zero ES3-only**
  (checado: sem glMapBufferRange/glGenVertexArrays/glTexImage3D/…).
  ⇒ **passthrough direto pro `libGLESv2.so` do Mali**. Diferente do Bully (que forçamos
  por `libgles3-shim.so`); aqui é **no código**, nativo.
- Camada interna do motor: **205 símbolos `emu_gl*`** (RenderWare→GLES) + `RwEngine*`,
  `RenderScene/RenderMenus/Render2dStuff`. Não mexer — é interna.
- Trabalho gráfico real = portar do Vita (`opengl_patch.c`) o **gerador de shader**:
  - hook `_ZN8RQShader11BuildSourceEjPPKcS2_` (Cg→GLSL ES na CPU)
  - PShaders substituídos por símbolo: `shadowResolvePShader`, `blurPShader`,
    `gradingPShader`, `contrastPShader`, `contrastVShader`
  - `GetMobileEffectSetting`, `RQCaps`, `RQMaxBones` (por símbolo)
- Texturas: motor tem `dxtSwizzler` (decodifica DXT→cru quando GPU não tem S3TC);
  ETC1 passa direto. Mali só-ETC1 → coberto pelo próprio motor.

## 🎮 GAMEPAD — API War Drum, hookável por nome
Símbolos exportados (arm64, resolvem por nome):
- `_Z28WarGamepad_GetGamepadButtonsi`, `_Z25WarGamepad_GetGamepadAxisii`,
  `_Z25WarGamepad_GetGamepadTypei`, `_Z26WarGamepad_GetGamepadTrackiiPiS_`
- `_Z14OS_GamepadAxisjj`, `_Z16OS_GamepadButtonjj`, `_Z21OS_GamepadIsConnected...`
- `_Z17AND_GamepadUpdatev`, `_Z21AND_GamepadInitializev`, `WarGamepadInit(JNIEnv*)`
- `CPad::UpdatePads`, `CPad::Initialise`, `CPad::StartShake` (força/rumble)
Fonte de estado = **SDL2** (device já tem SDL2 + `nextos-joymap` + `gptokeyb`; launcher
`gtasa-nextos.sh` já monta o event device). Vita mapeia via `CPad::GetPad` + stubs
(`CPad::GetCarGunUpDown`, `GetSteeringLeftRight`, `GetTurretLeft/Right`) — portar.

## 🔊 ÁUDIO — OpenSL ES (engine) + OpenAL (mixer)
- Engine cria mixer via **OpenSL ES**: `slCreateEngine`, `SL_IID_ENGINE/PLAY/BUFFERQUEUE/
  ANDROIDSIMPLEBUFFERQUEUE`.
- Família **`al*`** (OpenAL) toda substituída pelo Vita em `openal_patch.c` por nome
  (`alBufferData`, `alSource*`, `alAuxiliaryEffectSlot*`, `alBufferSamplesSOFT`…) → portar
  direto pro OpenAL do device.
- Device tem OpenAL; mapear OpenSL→OpenAL (o Vita já faz esse bridge).

## Resumo do esforço (por subsistema)
| Subsistema | Dificuldade | Por quê |
|---|---|---|
| Gráficos | **Baixa** | ES2 nativo; só portar shader-gen do Vita (sem shim) |
| Gamepad | **Baixa** | API War Drum por nome; SDL2 já no device |
| Áudio | **Média** | OpenSL→OpenAL bridge + al* do Vita |

Todos os hooks são **por nome** (portáveis armv7→aarch64). Ver tabelas em `re/`.
