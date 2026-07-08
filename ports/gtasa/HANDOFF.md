# GTA San Andreas (aarch64 so-loader) — HANDOFF

## Objetivo
So-loader **próprio, aarch64**, com o MESMO desempenho do port que já roda liso no
Mali-450. Meta futura do NextOS: jogar **online** (por isso arm64, não armv7).

## Estado (provado nesta sessão)
- O port que roda liso **JÁ É aarch64** (não armv7). Binário: `~/gta-sa-deploy/gtasa/gtasa`
  (`ELF 64-bit aarch64 PIE`, **não-stripado, com debug_info**) + engine
  `~/gta-sa-deploy/gtasa/libGTASA.so` (49.8 MB, aarch64).
- Proveniência do binário (extraída do DWARF): projeto **"Gta san andreas wrapper"** de
  autor turco (build path `/mnt/c/Users/Yavuz/.../Gta san andreas wrapper/...togithub`),
  feito pra R36S. Repo público não indexado no web/GitHub API (privado ou nicho).
- **É o MESMO framework "NX"** que roda nosso Bully e LCS. Conjunto de fontes do binário
  GTASA vs `experiments/bully/ref-bully-NX/`: **13 de 14 arquivos idênticos** —
  `main, config, asset_archive, cpplib_loader, imports, jni_patch, movie_patch, openal,
  so_util, util, zip_fs, error, hooks/game, hooks/opengl`. Único que falta:
  **`setup_gui.c`** (recuperável do binário debug).

## O que já está feito
- `ports/gtasa/src/` — esqueleto do loader NX copiado de `ref-bully-NX` (casa com o
  conjunto de fontes do binário GTASA). ⚠️ **Está Bully-tuned**; precisa ser **SA-tuned**.

## O que falta (reconstruir a variante GTA SA)
Fontes de verdade pra cada peça (o binário é não-stripado → dá pra ler tudo):
1. `config.c` — parser do `config_gtasa.txt`. Opções SA (extraídas do binário):
   `screen_width/height, use_bloom, trilinear_filter, disable_mipmaps,
   disable_detail_textures, character_shadows, drop_highest_lod, decal_limit,
   debris_limit, show_weapon_menu, crouch_toggle, shadow, hud_detonator, language`.
2. `jni_patch.c` — tabela JNI específica do GTASA (métodos que `libGTASA.so` chama).
3. `hooks/game.c` + `hooks/opengl.c` — hooks SA (shims GLES2 pro Mali só-ETC1).
4. `imports.c` / `openal.c` — bindings libc/OpenAL do SA.
5. `setup_gui.c` — **falta no esqueleto**; recuperar do binário (`objdump`/rizin).

Método: `aarch64-linux-gnu-objdump -d` + rizin (host já tem rz-ghidra) no binário debug,
comparando função-a-função com o esqueleto NX pra portar as diferenças SA.

## Por que o desempenho sai igual "de fábrica"
Não é o loader — é (1) o **streaming nativo** do motor (`CStreaming` carrega/despeja por
área → working-set pequeno) e (2) os **cortes do `config_gtasa.txt`** (`drop_highest_lod 1`,
`disable_mipmaps 1`, sem sombra/decal). Mesmo loader + mesmo config = mesmo FPS.
Ver estudo completo: `~/Área de trabalho/ESTUDO - GTA SA vs Bully no Mali-450 ...md`.

## Runtime / deploy
- Engine+assets (2.5 GB) no device `/storage/roms/ports/gtasa/` (Mali-450 aarch64,
  NextOS-Retro-Elite). Launcher de referência: `gtasa-nextos.sh` (Wayland/KMSDRM
  fallback, SDL_VIDEODRIVER=mali, nextos-joymap).
- ⚠️ APK **v2.11.311** (Downloads) NÃO serve de base: é a re-release Flutter
  (`libGame.so` 8.9 MB), outra engine. Base correta = **clássico v2.00** (`libGTASA.so`).
- glibc ≤2.30 no release (Docker buster) antes de empacotar (regra do repo).

---
## 🔧 DRIVER (descoberto ao rodar nosso loader no device .79) — 2026-07-07

**Nosso loader JÁ:** compila aarch64, carrega libc++_shared (2358 sym) + libGTASA
(text 8.4MB), roda os 116 inicializadores C++. Só **32 imports não-resolvidos** (stubs
do jogo, lista em `re/`) + o driver.

**GTA SA usa framework NVIDIA NvEventQueueActivity, NÃO o modelo `impl*` do Bully.**
- Entrada real: `Java_com_nvidia_devtech_NvEventQueueActivity_jniNvAPKInit`,
  `NVEventAppInit`/`NVEventAppMain`, `NVEventEGLInit`.
- GTA SA registra natives via **RegisterNatives** (nosso jni_shim já captura em
  `natives`), não por símbolo `impl*`.

**Driver correto (blueprint do Vita `jni_patch.c:392`, adaptado p/ aarch64/Linux):**
1. `*(int*)so_symbol("IsAndroidPaused") = 0;` (default é 1)
2. fake_vm: `[0x00]=fake_vm`, `[0x18]=GetEnv` (idx 3 — offset difere do bully 0x30)
3. `JNI_OnLoad(fake_vm, NULL)` → dispara RegisterNatives → preenche `natives`
4. `init = ((struct JNM*)natives)[0].fn;` (struct 24B no aarch64: name@0,sig@8,fn@16)
   `init(fake_env, 0, /*init_graphics=*/1);`
5. `pthread_create` deve rotear `NVEventAppMain` p/ rodar (Vita: `pthread_create_fake`
   chama a func direto). Checar `pthread_bridge.c`.
6. Antes do jni_load: portar `patch_opengl` (shader-gen, sem shim — ES2 nativo),
   `patch_openal` (al* → OpenAL device), `patch_game`/`patch_gfx` do Vita.

**Correção pontual achada:** bully procura `_Z9NvAPKInitP8_jobjectP13_jobjectArrayS2_`
(3 args, Bully). GTA SA tem `_Z9NvAPKInitP8_jobject` (1 arg). Trocar no hook_nvapk.

**32 stubs a fornecer** (Vita `default_dynlib` tem todos): SL_IID_* (data), slCreateEngine,
cloud* (9), telemetry (2), Social Club (IsSCSignedIn/EnterSocialCLub/EnterGameFromSCFunc/
SigningOutfromApp/ProfileStatsSend/IsProfileStatsBusy), GetRockstarID, hasTouchScreen,
RTPrioLevel, scmainUpdate, SetJNEEnvFunc, AAsset_{getLength,getRemainingLength,seek}.

**PRÓXIMO PASSO:** reescrever `jni_load` (modelo NVEvent acima) + adicionar os 32 stubs
na tabela + `pthread_create`→NVEventAppMain. Rodar → primeira imagem do NOSSO loader.

---
## 🧭 ESTADO REAL + DESCOBERTA CRÍTICA (fim da sessão 2026-07-07)

### 🚨 O port de referência (Yavuz) é GLES3 → NÃO roda no Mali-450
Rodando o binário Yavuz no device .79 (Mali-450 Utgard, GLES2-only): falha em
`undefined symbol: glBindVertexArray`, depois `glDrawBuffers` — camada GL inteira é
**GLES3**. O "port que roda liso" roda num device **GLES3** (Mali-G31/H700 etc.), não
neste Mali-450. **CONSEQUÊNCIA: não existe GTA SA no Mali-450 ainda — o NOSSO loader
(GLES2-nativo) seria o 1º** (igual fomos no Bully). Nosso loader não é só "nosso código",
é o ÚNICO caminho pro Mali-450.
- (Shim `tools/libvao_alias.so` mapeia core VAO→OES; o device TEM `GL_OES_vertex_array_object`.
  Mas o Yavuz cascateia GLES3 — abandonado, device errado pra ele.)

### ✅ Nosso loader (base bully, GLES2-nativo) — o caminho certo, JÁ RODA a engine
- Compila aarch64 (`build.sh`), carrega libc++_shared+libGTASA, roda 116 init C++.
- Deployado no device como `gtasa-nosso`.

### 🔧 O que falta: DRIVER GTA-SA limpo (não remendar o do bully)
Check estático: o driver do bully pede **23 símbolos que o GTA SA NÃO tem** (fatais):
`BullySettings::*` (4), texture-bake (`RenderScene::CreateSpriteComponent`,
`SpriteComponent::Setup`, `MadNoRwTextureRead`, `TextureHeapHelper::GetCurrentTextureMemoryUsed`),
callbacks Rockstar (5), `StorageRootPath`, `OS_ZipAdd`, `OS_CanGameRender`,
`OS_IsGameSuspended`, `AND_{Create,Destroy}EglSurface`, `RemoveIslandsNotUsed`,
`implOnRockstarSetup`, `__cxa_guard_abort`, `Loading::IplStreamingDist`.

**PLANO (próxima fase, turnkey):**
1. Adicionar `so_symbol_opt` (não-fatal) em so_util.
2. Escrever `jni_load` GTA-SA MÍNIMO (modelo Vita `jni_patch.c:392`):
   `IsAndroidPaused=0` → fake_vm[0x18]=GetEnv → `JNI_OnLoad(vm)` →
   `init=((JNM*)natives)[0].fn; init(fake_env,0,1);` (struct 24B aarch64).
   DESLIGAR os hooks bully (bake/shadow/BullySettings/StorageRootPath-gates).
3. Rotear `pthread_create`→NVEventAppMain (Vita `pthread_create_fake`).
4. Adicionar os 32 stubs (Vita `default_dynlib` tem todos: SL_IID_* data, slCreateEngine,
   cloud*, telemetry, Social Club, GetRockstarID, hasTouchScreen, AAsset_*).
5. EGL/tela/input: reusar `bully_init_gl`/`bully_egl_objects`/`jni_init_input` (já Linux/Mali).
6. GL = passthrough ES2 (Mali-450) — SEM shim. Portar shader-gen do Vita se precisar.
7. Build (`build.sh`) → deploy `gtasa-nosso` → rodar → 1ª imagem do NOSSO loader no Mali-450.

Correção já aplicada: `hook_nvapk` usa `_Z9NvAPKInitP8_jobject` (1 arg) e removido NvAPKOpenFromPack.

---
## ✅✅ JOGÁVEL — GAMEPLAY NO MALI-450 (breakthrough)
CJ no mundo (bicicleta em Los Santos), menu com fontes, mundo 3D, zero crash.
**1º GTA SA do mundo em Mali-450** (o do Yavuz é GLES3, morre em glBindVertexArray).

### Driver NVEvent (jni_shim.c `jni_load`)
1. `IsAndroidPaused=0`; monta fake_vm/fake_env (offsets aarch64 = idx*8).
2. `JNI_OnLoad(fake_vm)` -> RegisterNatives (13 métodos). **COPIAR a tabela na hora**
   (RegisterNatives) — o array vem da STACK do JNI_OnLoad e some ao retornar.
3. `natives[0]`="init"(Z)Z = `NVEventJNIInit(env,thiz,bool)`; `init(fake_env,0,1)`.
4. Threads REAIS do glibc (bionic ABI compat). `gtasa_pthread_create` desvia o
   wrapper `NVThreadSpawnProc` (TLS de NVThread não populado -> pthread_kill).
   `OS_ThreadLaunch/Wait/SetValue`+`NVThreadSpawnJNIThread` hookados (handle
   +0x28=pthread_t, +0x69=running batem 1:1). `pthread_setname_np`->ret0.
5. Após init: injeta ciclo de vida `setWindowSize(w,h)`+`resumeEvent()` (sem Java
   o NVEventAppMain fica bloqueado esperando o surface). `start_async_file_worker`
   (AND_FileUpdated destrava OS_FileRead assíncrono).
6. main() vira keep-alive + pump SDL (input via poll GetGamepad*).

### Stubs (gtasa_stubs.c) — o que faltava resolver
__cxa_guard_*, __cxa_pure_virtual, cloud*(11), Social Club, telemetria, SL_IID*,
slCreateEngine(fail), AAsset_getLength/seek (root cria), sigaction->ret0,
pthread_setname_np->ret0, raise/abort (log do call-site).

### Assets — MUROS RESOLVIDOS
- **AAsset_getLength/getRemainingLength/seek** (nomes NÃO-64): mapear pras impls
  REAIS (aa_getLength64...) — estavam ret0 e travavam CText::Load (AMERICAN.GXT).
- **GTASA_NO_NVAPK=1**: NvAPKOpen->NULL -> o jogo cai no fopen NATIVO (FILE* em
  handle+8) -> OS_FileRead/GetPosition/Seek funcionam (ftell). Sem isso, ReadLine
  ->OS_FileGetPosition faz ftell num handle NvAPK (não-FILE*) -> SIGSEGV nos .met.
- **ci_fopen CASE-INSENSITIVE** (imports.c): "data/decision/PedEvent.txt" real =
  "data/Decision/PedEvent.txt". Busca em assets/ E na raiz (CINFO.BIN/MINFO.BIN).

### Gráficos/Áudio — desligados p/ Mali-450
- **Env-map de veículo** (RpMatFX): `SetEnvironmentMapCB`+`SetEnvMapCoeffCB`+
  `SetEnvMapCoeffAtomicCB` -> passthrough. RpMatFXMaterialGetEffects lê MatFX podre
  -> SIGSEGV ao carregar o 1º carro. (Opt-in de volta: GTASA_ENVMAP.)
- **Áudio**: `CAEAudioHardware::PlaySound`->ret0. OpenAL do device corrompe o heap
  (double free em SetAudioBuffer). TODO: bridge OpenAL correto. (Opt-in: GTASA_AUDIO.)

### Launcher (device)
`GTASA_NO_NVAPK=1`, symlinks dos subdirs de assets/ no CWD, SDL mali fbdev do
sistema (NÃO forçar SDL_VIDEODRIVER). glibc<=2.30 no release (Docker buster).

### TODO polish
- Áudio (bridge OpenAL sem corromper heap).
- Língua: menu sai em RUSSO -> forçar EN (locale/settings).
- MINFO.BIN/gta3.ini ausentes (não-fatais).
