# GTA San Andreas — so-loader aarch64 próprio (Mali-450 / GLES2)

**1º GTA San Andreas do mundo rodando no Mali-450 (Utgard, GLES2-only).** Loader
so-loader **nosso**, GLES2-nativo, com driver **NVIDIA NvEventQueueActivity**.
Estado: **JOGÁVEL** — CJ no mundo, menu em inglês, **áudio**, controle, full-res
1280×720, ~165 MB de RAM, estável em device de ~1 GB.

> O port de referência "Yavuz" é **GLES3** (morre em `glBindVertexArray` no Utgard).
> A `libGTASA.so` é a MESMA (mesmo md5) — o engine é **100% GLES2**, sem caminho
> ES3 escondido. Logo, este loader é o ÚNICO caminho pro Mali-450, e iguala/supera
> o Yavuz em qualidade (full-res + trilinear).

---

## Como funciona (arquitetura)

O GTA SA Android NÃO usa o modelo `impl*` do Bully. Usa o framework **NVIDIA
NvEventQueueActivity**: o jogo se registra via `RegisterNatives` e roda o próprio
loop (`NVEventAppMain`), chamando de volta o "Java" via JNI. Nós fingimos esse Java.

### Driver (src/jni_shim.c `jni_load`)
1. `IsAndroidPaused = 0` (default 1 → loop pausado).
2. Monta `fake_vm`/`fake_env` (offsets aarch64 = idx×8; Vita usa idx×4).
3. `JNI_OnLoad(fake_vm)` → o jogo chama `RegisterNatives(env, cls, methods, 13)`.
   **A tabela vive na STACK do JNI_OnLoad** → copiar na hora (some ao retornar).
4. `natives[0]` = método `"init" (Z)Z` = `NVEventJNIInit(env, thiz, initGraphics)`;
   chamar `init(fake_env, 0, 1)` → spawna `NVEventAppMain` numa thread.
5. **Threads REAIS do glibc** (ABI pthread bionic≈glibc). O wrapper
   `NVThreadSpawnProc` depende de TLS de NVThread que não populamos → desviamos
   (roda `arg[8]` direto, igual `pthread_create_fake` do Vita). `OS_ThreadLaunch/
   Wait/SetValue` + `NVThreadSpawnJNIThread` hookados (handle +0x28=pthread_t,
   +0x69=running batem 1:1 com OS_ThreadIsRunning/Wait).
6. **Ciclo de vida da Activity** (sem Java): injetar `setWindowSize(w,h)` +
   `resumeEvent()` → destrava o surface/render (senão NVEventAppMain fica
   bloqueado esperando).
7. **Async file worker**: `AND_FileUpdated` drena `AndroidFile::firstAsyncFile`
   (senão `OS_FileRead` assíncrono bloqueia pra sempre).
8. `main()` vira keep-alive + pump SDL. Input = **poll** (`GetGamepad*`).

## Muros quebrados (o miolo — cada um foi um SIGSEGV/deadlock)

| Muro | Causa | Fix |
|---|---|---|
| natives some | tabela na stack do JNI_OnLoad | copiar em RegisterNatives |
| `pthread_kill(SIGSEGV)` | `NVThreadSpawnProc` usa TLS não-populado | bypass + hook `OS_ThreadLaunch` |
| trava em `AMERICAN.GXT` | `AAsset_getLength/seek` (nomes NÃO-64) stubados=0 | mapear pras impls reais (`aa_getLength64`...) |
| SIGSEGV nos `.met`/`.dat` | `OS_FileGetPosition` faz `ftell` num handle NvAPK | `GTASA_NO_NVAPK=1` → **fopen NATIVO** (FILE* real) |
| arquivo "não encontrado" | case-sensitive (`data/decision` vs `data/Decision`) | `ci_fopen` case-insensitive (imports.c) |
| crash no 1º carro | env-map de veículo (RpMatFX) lê MatFX podre | hook `SetEnvironmentMapCB`+coeff → passthrough |
| `double free` (áudio) | OpenAL-Soft meio-init (OpenSL falhava) | **bridge OpenSL ES → SDL_Audio** (opensl_shim.c) |
| menu em russo | `InitialiseLanguage` cai no fallback idx5 (flag de região) | patch `csel`→`mov` + força AMERICAN(0) |
| resolução baixa | tier do device < 2 → render escalado | `GTASA_FULLRES=1` (NOP no `b.lt` de OS_AppInit) |

## Som — bridge OpenSL ES → SDL_Audio (src/opensl_shim.c)
O jogo embute **OpenAL-Soft estático**; o backend de saída Android é **OpenSL ES**
(`slCreateEngine`). O Amlogic não tem OpenSL, mas tem **SDL2 + ALSA (I2S)**.
Implementamos a superfície OpenSL ES 1.0.1 (SLObjectItf/SLEngineItf/SLPlayItf/
SLAndroidSimpleBufferQueueItf) e mandamos o PCM pro `SDL_OpenAudioDevice`:
`Enqueue`→ring buffer; callback do SDL dreno + dispara o cb do OpenAL-Soft.

---

## Build & Run
```bash
cd ports/gtasa && ./build.sh          # cross aarch64 (sysroot NextOS-Retro-Elite) -> ./gtasa (PIE)
# deploy: gtasa -> device:/storage/roms/ports/gtasa/gtasa-nosso
# engine+assets no device: libGTASA.so (v2.00 CLÁSSICO, NÃO a re-release Flutter),
#   libc++_shared.so, assets/, config_gtasa.txt
```
Launcher device: `gtasa-nextos.sh` — foreground (ES resume), symlinks de assets/,
NÃO força SDL_VIDEODRIVER (mali fbdev vem do sistema).

### Envs (knobs)
| Env | Efeito |
|---|---|
| `GTASA_NO_NVAPK=1` | **obrigatório** — fopen nativo (OS_FileGetPosition/ftell OK) |
| `GTASA_FULLRES=1` | render full 1280×720 (~20fps) vs escalado liso (~30fps) |
| `GTASA_NOAUDIO=1` | mudo (senão áudio ON via bridge) |
| `GTASA_LR_NORMAL=1` | desfaz o swap L1↔L2 / R1↔R2 |
| `GTASA_ENVMAP=1` | religa env-map de veículo (**CRASHA** no Mali-450) |
| `GTASA_NODIAG=1` | sem dump de threads (diagnóstico) |

## Mapa dos arquivos (src/)
- `main.c` — carrega libc++_shared + libGTASA, crash/diag handlers, chama jni_load.
- `jni_shim.c` — **DRIVER NVEvent**, fake JNI, `patch_game_gtasa` (hooks por-nome:
  threads/screen/língua/env-map/áudio/resolução), input (poll + swap L/R + injeção).
- `gtasa_stubs.c` — imports que faltavam (cxa_guard, cloud*, SL_IID*, social,
  raise/abort log, desvio pthread_create, sigaction/setname, slCreateEngine).
- `opensl_shim.c` — **bridge OpenSL ES → SDL_Audio** (som).
- `imports.c` — libc/GL/AAsset bionic→glibc; `ci_fopen` case-insensitive.
- `so_util.c` — loader ELF aarch64 (relocate/resolve/hook), base reVC/max_arm64.

## Fontes / referência
- Lógica SA: `TheOfficialFloW/gtasa_vita` (**MIT**, ARMv7) — offsets crus não
  servem no aarch64; re-derivados por símbolo (libGTASA não-stripada, tem DWARF).
- `HANDOFF.md` / `RESOLUTION.md` / `SUBSYSTEMS.md` — log detalhado da engenharia.

## TODO / polish
- Testar `disable_detail_textures 0` + `drop_highest_lod 0` (RAM sobra ~335MB).
- Retarget armv7 (`armv8a-emuelec-linux-gnueabihf-gcc`) — trivial com o código.
- Online/SA-MP (por isso aarch64).
