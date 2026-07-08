# GTA Vice City (arm64) — so-loader próprio p/ Mali-450 — HANDOFF (começar do zero)

## Objetivo
Portar o **último GTA Vice City OFICIAL, arm64-v8a** (`libGame.so`, engine
RenderWare GLES2 sob casca Flutter) pro Mali-450 (Utgard, GLES2-only), com **loader
NOSSO aarch64**. Meta: gameplay completo (mundo, controle, áudio, cutscenes) — igual
fizemos no GTA SA. **NÃO usar o armhf** (attempt 1, arquivado em `_armhf_old_attempt1/`
— era o `libGTAVC.so` clássico armv7, feito sem referência = quebrado; o "OOM" era
sintoma do loader ruim, NUNCA foi RAM: o VC é MAIS LEVE que o SA).

## APK-fonte
`~/Downloads/23de350d-9a2f-4971-bc91-09a34e158e2f.apk` (1.5 GB, **PT-BR na UI Flutter**;
in-game GXT é INGLÊS — trocar depois). Também vale `com.rockstargames.gtavc-1.12.259`
(mesmo engine, build mais novo). Arch: **arm64-v8a**. Engine no APK:
`lib/arm64-v8a/{libGame.so, libflutter.so, libapp.so, libopenal.so, libVendor_mpg123.so}`.

## 🔑 DESCOBERTA-CHAVE (por que é gol cantado)
O `libGame.so` é a **MESMA engine RenderWare** dos outros ports Rockstar (tem os
`emu_gl*`), dirigida pelo **modelo `impl*`** (`Java_com_rockstargames_oswrapper_
GameNative_implOn*`) — **IDÊNTICO ao BULLY**. O Flutter (`libflutter`/`libapp`) é só
a casca/launcher; o JOGO é o `libGame.so`, que dirigimos **direto pelos `impl*`
por símbolo** (bypassando o Flutter, igual bypassamos o Java). É **arm64 + GLES2 +
impl\*** → tudo que já dominamos. `re/` tem os símbolos.

| Subsistema | libGame.so | Reuso do nosso `../gtasa/src` |
|---|---|---|
| Engine | RenderWare, **67 gl*, 0 ES3-only** = GLES2 | egl_shim.c (Mali fbdev) direto |
| Bootstrap | **impl\*** (38 símbolos, por so_symbol) | **jni_shim.c: usar o jni_load do BULLY** (não o NVEvent do SA) |
| C++ runtime | precisa **libc++_shared** (49 `std::__ndk1` undef) | main.c 2-módulos (A=libc++_shared, B=libGame) |
| Dados | NvAPK + AAsset + OS_FileOpen/Read | asset_archive.c + imports.c (aa_open/ci_fopen) + `GTASA_NO_NVAPK` |
| Áudio | **OpenAL-Soft** (`alcOpenDevice`, `alSourcePlay`) + `OS_SetOpenAlContext` | opensl_shim.c (bridge OpenSL→SDL) — CONFERIR se usa slCreateEngine |
| Input | `implOnGamepad{Down,Up,AxesChanged,Connected}` + War Drum | pump_gamepad (push) do jni_shim.c |
| Threads | `OS_ThreadLaunch`, `NVThreadGetCurrentJNIEnv` | my_OS_ThreadLaunch + hooks (handle offsets: RE-CONFIRMAR no VC) |
| Cutscenes | `assets/movies/` + `FindCutsceneAudioTrackId` | **NOVO** — investigar player de vídeo (ou pular) |
| Dados | loose em `assets/` (anim/audio/data/models/movies/texdb/text/textures/txd) | **sem OBB** — extraído p/ device (ver abaixo) |

## Entry points impl* (offsets do libGame.so deste APK — `re/libGame_impl_symbols.txt`)
- `implOnInitialSetup`   @ **0x440150**  (o 1º — setup inicial)
- `implOnActivityCreated`@ **0x440250**
- `implOnSurfaceCreated` @ **0x43cf58**
- `implOnSurfaceChanged` (RE achar)
- `implOnDrawFrame`      @ **0x4403c4**  (loop de frame)
- `implOnResume` / `implOnPause`
- `implOnRockstarSetup`  @ **0x441724**  (gate online, igual bully)
- `implOnGamepad{ButtonDown,ButtonUp,AxesChanged,Connected,Disconnected,Resume}`
- `implOnTouch{Start,Move,End}` (menu touch — sintetizar toque, igual LEGO)
- gate: `OS_OnRockstar{Setup,GateComplete,StateChanged,InitialComplete,SignInComplete,...}`
- `StorageRootPath`, `AND_CreateEglSurface`, `OS_ThreadMakeCurrent` — TODOS presentes = o
  `jni_load` do bully aplica quase direto (hook_egl/hook_threads/hook_screen/gate flags).

## Plano da próxima seção (passo a passo)

### 0. Dados (já enviados pro device em background)
`/storage/roms/ports/gtavc/` recebe: `libGame.so`, `libopenal.so`, `libVendor_mpg123.so`,
`libc++_shared.so` (do SA, aarch64 NDK) e `assets/` (anim, audio, data, models, movies,
texdb, text, textures, txd). Extraído de `~/.../gtavc-arm64-deploy/`. **Sem OBB** — tudo
loose em `assets/` (o loader acha via aa_open/ci_fopen com `GTASA_NO_NVAPK=1`).

### 1. Driver: trocar NVEvent → impl* (Bully)
O `src/jni_shim.c` veio do SA (jni_load = NVEvent). **Reescrever `jni_load` no modelo
impl\* do Bully**: `build_env` → hook_nvapk/egl/threads/screen/cxa → resolver os
`implOn*` por `so_symbol("Java_com_rockstargames_oswrapper_GameNative_implOn...")` →
`implOnInitialSetup` → `OS_ZipAdd` (se houver) → gate flags (StorageRootPath±offset)
→ `implOnActivityCreated` → bully_init_gl → `implOnSurfaceCreated/Changed` → seed
OS_EGL globals → `implOnResume` → loop `implOnDrawFrame` + disparar `OS_OnRockstar*`
(gate). **O git tem o jni_load bully original** (antes do commit 912290a que virou
NVEvent) — `git show 912290a^:ports/gtasa/src/jni_shim.c` OU pegar de `ports/bully`.
Assinatura impl* do bully: `implOnActivityCreated(env,thiz,ctx,int)`,
`implOnDrawFrame(env,thiz,float dt)`. RE-confirmar os args no VC (stripado).

### 2. Stubs (renomear gtasa_stubs.c → gtavc_stubs.c) — resolver os 327 undefs de `re/`
Igual SA: cxa_guard, cloud*, SL_IID*, social/telemetry, AAsset_*, `sigaction`/`setname`
→ ret0, pthread_create desvio (VC usa NVThreadSpawnProc? conferir), raise/abort log.
Diferenças VC: RE os undefs em `re/libGame_undefined_imports.txt` e cobrir os que não
resolvem por glibc/GL/libc++.

### 3. Assets: `GTASA_NO_NVAPK=1` + ci_fopen (case-insensitive) — igual SA
OS_FileGetPosition faz ftell → precisa FILE* real → forçar fopen nativo. A engine VC é
igual: NvAPK→NULL faz cair no fopen. Confirmar OS_FileRead/GetPosition offsets no VC.

### 4. Áudio: opensl_shim.c (bridge OpenSL→SDL) — CONFERIR
libGame usa OpenAL-Soft (alcOpenDevice). Se o backend for OpenSL (slCreateEngine),
o opensl_shim.c aplica direto. Se abrir device OpenAL direto (libopenal do APK),
avaliar usar a `libopenal.so` do device. `GTASA_NOAUDIO=1` p/ testar sem som 1º.

### 5. Gráficos / muros esperados (do SA):
- env-map de veículo (RpMatFX) pode crashar → `SetEnvironmentMapCB`→passthrough (RE no VC).
- resolução: OS_ScreenSetResolution por tier (GTASA_FULLRES-like). RE o ponto no VC.
- ETC1 (Mali só-ETC1) — o motor tem dxtSwizzler; passa.

### 6. Cutscenes/vídeos (`assets/movies/`) — NOVO
Investigar como libGame toca os movies (`FindCutsceneAudioTrackId`, OS_Movie*?). Se for
formato próprio (RW .mp4/.vp6), talvez precise shim ou pular a intro (hook o player→ret).

### 7. Input: mapear impl* gamepad + swap L/R (se o NextOS quiser, igual SA).

## Build & Run
```bash
cd ports/gtavc && ./build.sh          # dev (glibc device) -> ./gtavc
# release: docker debian:buster + build_buster.sh -> gtavc-buster (glibc 2.17, regra #12)
# deploy: gtavc[-buster] -> device:/storage/roms/ports/gtavc/gtavc-nosso
```
Envs (herdados do SA): `GTASA_NO_NVAPK=1` (renomear p/ GTAVC_* ou manter), `GTASA_FULLRES`,
`GTASA_NOAUDIO`, `GTASA_NODIAG`. Launcher: adaptar `../gtasa/gtasa-nextos.sh`.

## Diferenças do SA (cuidado)
1. **impl\*** (Bully) e NÃO NVEvent → jni_load diferente (item 1).
2. **STRIPADO** (sem DWARF) → RE por símbolo (nm) + offset, sem addr2line source-line.
3. **VC GXT** começa com `TABL` direto (sem `04 00 10 00` do SA) — formato GTA III/VC.
4. **Flutter** = ignorar (não carregar libflutter/libapp; dirigir libGame direto).
5. Cutscenes/movies = subsistema novo.

## Estado do device (atual)
Rodando **GTA SA** (nosso port, JOGÁVEL). O GTA VC vai em `/storage/roms/ports/gtavc/`
(dir separado). Device: 192.168.31.79 (Mali-450 NextOS-Retro-Elite, glibc 2.43).

## Referências
- **`../gtasa/`** = a referência viva (mesmo framework; HANDOFF/README de lá explicam o driver).
- `../bully/` = o jni_load impl* original (fonte do driver p/ o VC).
- `re/` = símbolos do libGame.so (impl*, defs, undefs).
- Fork lógica: `TheOfficialFloW` (Vita, mas é III/VC/SA família).

---
## ✅ PRIMEIRO BINÁRIO BUILDADO (esta seção)
- `src/jni_shim.c`: driver trocado **NVEvent → impl\* (bully)** — `jni_load` resolve
  `implOnInitialSetup/ActivityCreated/SurfaceCreated/Changed/DrawFrame/Resume` por
  símbolo + gate Rockstar + StorageRootPath. (`zip_fs_init` removido; helpers
  gtasa_raise/abort/pthread_create readicionados.)
- `main.c` → `libGame.so`. `build.sh`/`build_buster.sh` → `gtavc`.
- **`./build.sh` → `gtavc` (aarch64 PIE) COMPILA OK.** 245KB.
- `gtavc-nextos.sh` (launcher) criado (envs GTASA_* herdados que o binário lê).
- **NÃO testado no device** (device recebendo os 1.4GB de dados).

### PRÓXIMO PASSO IMEDIATO (quando os dados chegarem no device)
1. Deploy `gtavc` → `device:/storage/roms/ports/gtavc/gtavc-nosso`.
2. Rodar via launcher (ES parado) com `GTASA_NO_NVAPK=1 GTASA_NODIAG=1` e ler o log:
   - confirmar 2 módulos carregam + 0 UNRESOLVED (RE os undefs de `re/` que faltarem
     → adicionar em gtasa_stubs.c).
   - `implOnInitialSetup` acha? StorageRootPath gate offsets do VC (bully usa
     srp-0x174/-0x17c/-0x2e8 → RE-confirmar no libGame; se errado, achar por disasm).
   - `implOnDrawFrame` → primeira imagem (menu). Igual SA: OS_FileGetPosition/ftell
     (GTASA_NO_NVAPK), env-map crash (SetEnvironmentMapCB→passthrough), resolução.
3. Muros esperados (mesma família do SA/bully) — atacar pela ordem do log.

### RE ainda a fazer (libGame.so stripado — por símbolo/disasm)
- Offsets do gate (StorageRootPath ±X) — disasm implOnInitialSetup/OnResume.
- Assinaturas exatas dos impl* (args de implOnActivityCreated/OnDrawFrame).
- OS_ScreenSetResolution / tier (equivalente do GTASA_FULLRES).
- SetEnvironmentMapCB (env-map veículo) no VC.
- OS_ZipAdd: VC usa? (assets loose → talvez não).
- Cutscenes (assets/movies/): como libGame toca (OS_Movie*/FindCutsceneAudioTrackId).

---
## ✅✅ PRIMEIRO BOOT NO DEVICE (esta seção) — driver impl* funciona!
Rodado no device (dados ainda subindo — parcial). O binário passou por:
- libc++ (2358 sym) + libGame carregam ✓
- impl* TODOS resolvidos (setup/act/surfC/surfCh/draw/resume) ✓
- **JNI_OnLoad => 0x10004** ✓, **implOnInitialSetup OK** ✓
- **gates init=1 susp=0 render=1** ✓ (offsets StorageRootPath do BULLY -0x174/-0x17c/-0x2e8
  BATERAM no VC — não precisou re-derivar!)
- implOnActivityCreated ✓ → **GL Mali-450 GLES2 1280x720 ✓** → gamepad ✓
- implOnSurfaceCreated ✓ → implOnSurfaceChanged 1280x720 ✓
- **`[OSWrapper] HALT`** logo após surface-changed.

O driver bully portou ~1:1. O HALT é no init de recursos do surface-changed —
**provável DADO INCOMPLETO** (rsync em 1.0/1.4 GB na hora do teste). RE-TESTAR com
os assets completos. Se persistir: investigar o que o surface-changed carrega
(whitetexture/default resources) e o async file worker (start_async_file_worker
já é chamado no jni_load). Fixes já aplicados p/ o VC:
- `so_util_x64.h`: `so_symbol` → `so_find_addr_safe` (hooks NÃO-FATAIS; faltou símbolo = no-op).
- `hook_nvapk`: NvAPKInit **3-arg** (`_Z9NvAPKInitP8_jobjectP13_jobjectArrayS2_`) +
  `NvAPKOpenFromPack` (VC tem, igual bully).
