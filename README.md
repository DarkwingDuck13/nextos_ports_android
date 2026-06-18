# nextos_ports_android

Framework pra **portar jogos Android (ARM64, NativeActivity) pra Linux ARM64 / NextOS** — alvo principal os devices **Mali** (Amlogic-old/ng/nxtos, Utgard/Bifrost/Valhall).

Não recompila o jogo: **carrega o `.so` nativo do Android e roda direto** no Linux, com uma camada de shim que finge ser Android (fake JNI, OpenSL ES→SDL2, EGL→SDL2, bionic→glibc). Mesma linhagem dos so-loaders de PSVita (TheFloW), adaptada pra Linux ARM64 + SDL2.

> ℹ️ **Isto NÃO são ports PortMaster.** Cada jogo aqui roda a **versão ANDROID** (o `.so` do APK) via **so-loader** — não um build Linux/PC. O empacotamento aproveita o framework do PortMaster **só pra lançar** (control.txt + gptokeyb pra controle/sair), mas o que executa por dentro é o binário Android. Ports PortMaster "de verdade" (de builds Linux) desses jogos, quando existem, são projetos separados.

> ✅ **Provado no Mali-450 (Utgard):** os ports de referência **Syberia** (GLES1) e **LEGO Star Wars: TCS** (GLES2) rodam perfeitos. O caminho de render (so-loader + EGL→SDL2 + GLES) está validado no Utgard.

> 🏆 **DESTAQUE — primeiro port do BULLY: Anniversary Edition em aarch64 / Linux / PortMaster (INÉDITO MUNDIAL).** O jogo **completo** da Rockstar rodando via so-loader do `libGame.so` Android no **Mali-450 (OpenGL ES 2.0, fbdev)** — mundo aberto, escola, **personagem (vestido!)**, controle e áudio, **100% jogável**. Leia [`ports/bully/README.md`](ports/bully/README.md): documenta os destraves — `hook_arm64` com **pool de trampolins** (colisão NvAPK), **EGL via SDL2-mali**, fixes GLES2 do Utgard (`highp→mediump`, `GL_LUMINANCE→RGBA`), o **fix do `glClear`** que fazia a roupa do Jimmy "aparecer e sumir", e o **limite de memória de textura da GPU** que travava a escola (`BULLY_TEX_LIGHT`/`BULLY_TEX_HALF` + `asset_archive` **O(log n)**). Empacotado padrão PortMaster (BYO-data).

> 🥈 **Primeiro port feito DO ZERO com o framework: [`ports/revc`](ports/revc/README.md) — GTA: Vice City (reVC Android), 100% JOGÁVEL no Mali-450** (mundo, controle, áudio, menu, NPCs). Documenta a arquitetura **so-loader 2-módulos** (libc++_shared + engine) e as **receitas Mali-450/GLES2 reutilizáveis** (ABI pthread bionic→glibc, shaders highp/MAX_LIGHTS/im2d, texturas `GL_RGBA8`/`GL_TEXTURE_MAX_LEVEL`-mipmap, SDL resolução/input, redirect de `fopen`, patch de runtime). **Boa base ao portar o próximo jogo.**

> 🦔 **DESTAQUE — SONIC MANIA PLUS (engine RSDKv5/Retro Engine, build Netflix Android) rodando no Mali-450 (GLES2, fbdev) — INÉDITO MUNDIAL.** Fluxo **completo e jogável COM SOM**: logos → título → menu (Mania Mode) → save select → escolher personagem → cutscene de abertura (Angel Island) → **Green Hill Zone jogável**, sai com SELECT+START (padrão PortMaster). Leia [`ports/sonicmania/README.md`](ports/sonicmania/README.md): documenta os destraves — **título** travava em `WaitForConflictState` esperando um cloud-save (fix: forçar `PressButton` + patch `GetCloudSaveConflictState→0`); **menu** preto/spinner (fix: JNI `GetStringUTFChars` p/ jstring falso + `PrerollChecks` completar natural); **save de jogo novo** travava (cloud-only via `JniWrapper::CloudSave` async → entregar os callbacks via `CallCallback`); **crash de fase** ao destruir badnik (telemetria `Stats::TryTrackStat`→`std::wstring_convert` → no-op); e a **RECEITA DE SOM**: a callback do Oboe crasha em STL, então chamamos o mixer puro `RSDK::Audio::MixToBuffer` **direto na thread do SDL** (bypass do Oboe) + forçar `streamVolume`/`soundFXVolume`. Render via blit GLES2 próprio (shaders eram do lado Java). **BYO-data** (APK do Sonic Mania Plus que você possui).

> 🥊 **DESTAQUE — STREETS OF RAGE 4 (MonoGame/.NET 9) rodando NATIVO no Mali-450 (GLES2).** Diferente dos demais, este NÃO é so-loader: o runtime **.NET 9 CoreCLR** + **MonoGame compilado em GLES2** executam o código gerenciado do jogo direto, com um host próprio (`sor4host`) no lugar da `MainActivity`. Fluxo **jogável com áudio** — menu → seleção → fases, com a **música original (Wwise)** tocada por um reimpl OpenAL leve (troca limpa entre faixas) e SFX de combate. As texturas ASTC são convertidas pra **ETC1** na 1ª execução (BYO-data via APK). Leia [`ports/sor4/README.md`](ports/sor4/).

## Jogos portados
| Jogo | Engine / método | Estado | Pasta |
|---|---|---|---|
| **Bully: Anniversary Edition** | so-loader (`libGame.so`) | ✅ **100% jogável** (Mali-450, GLES2) — mundo, escola, personagem, controle, áudio | [`ports/bully`](ports/bully/) |
| **GTA: Vice City** (reVC) | so-loader 2-módulos | ✅ **100% jogável** (Mali-450) — mundo, controle, áudio, menu, NPCs | [`ports/revc`](ports/revc/) |
| **Sonic Mania Plus** (RSDKv5) | so-loader | ✅ **100% jogável COM SOM** — título→menu→save→cutscene→fase | [`ports/sonicmania`](ports/sonicmania/) |
| **Streets of Rage 4** | **MonoGame/.NET 9 NATIVO** (não so-loader) | ✅ **jogável + áudio validado** (Mali-450 GLES2) — música/SFX, texturas ETC1, BYO via APK | [`ports/sor4`](ports/sor4/) |
| **DYSMANTLE** | so-loader (GameActivity) | ✅ **jogável** (Mali-450 + X5M) — mundo com cor, áudio | [`ports/dysmantle`](ports/dysmantle/) |
| **Terraria** (Unity IL2CPP) | so-loader | ✅ **jogável** — controle + áudio + player/mundo | [`ports/terraria`](ports/terraria/) |
| **NFS Most Wanted (2012)** | so-loader (armhf) | 🟡 gameplay 3D + áudio OK; fontes do menu pendentes | [`ports/nfs`](ports/nfs/) |
| **Resident Evil 4** (Unity) | so-loader | 🔴 demo — menu + entrada Cap.1 OK; **andar congela** (deadlock job-system) | [`ports/re4`](ports/re4/) |
| **Dusklight** (Zelda: Twilight Princess recomp) | recomp + backend Aurora GLES2 | 🟢 cena reconhecível (castelo de Hyrule) | [`ports/dusklight`](ports/dusklight/) |
| **Cuphead** (Unity IL2CPP) | so-loader | 🔬 WIP | [`ports/cuphead`](ports/cuphead/) |
| **Hollow Knight** (Unity IL2CPP) | so-loader | 🔬 pesquisa — renderiza em GLES3 (X5M, Mali-G310); muro = input | [`experiments/hollow-recon`](experiments/hollow-recon/) |
| Syberia (GLES1) · LEGO Star Wars: TCS (GLES2) | so-loader (ref. **mtojek**) | 📦 referência — validam o framework no Utgard | — |

> ⚙️ **Dois caminhos**: a maioria é **so-loader** (carrega o `.so` Android e roda direto); alguns são **nativos** — Streets of Rage 4 roda o runtime **.NET 9 + MonoGame** compilado em GLES2, e Dusklight é um **recomp**. O empacotamento PortMaster (launcher + BYO-data) é o mesmo nos dois.

> Todos os ports são **BYO-data**: o repo traz só o código/loader; você fornece o `.so`/dados do APK que **possui legalmente**.

## Por que funciona tão bem
Android é Linux. O código do jogo é **ARM nativo** rodando no ARM do device — zero emulação de CPU. GLES é GLES (mesma API). Nos teus TV boxes, é praticamente o hardware nativo do jogo (mesmo SoC/GPU classe Android). Só a "casca" Android é trocada por SDL2/glibc.

## Estrutura
```
core/            # REUTILIZÁVEL entre todos os ports (não-editar por jogo)
  so_util.*      #   loader ELF arm64 (relocs, GOT, init_array, hook_arm64)  ← coração
  egl_shim.*     #   EGL -> SDL2 (genérico p/ qualquer jogo GLES)
  opensles_shim.*#   OpenSL ES -> SDL2 (ring buffer SPSC + resample)
  util.* error.* hashmap.h
template/src/    # BASE por-jogo (copiada e adaptada pra cada port)
  main.c         #   loader flow + GOT hooks + crash recovery
  android_shim.* #   fake android_native_app_glue (paths, input, resolução)
  jni_shim.*     #   fake JNI (package name, OBB path, feature flags)
tools/
  new-port.sh    # << gera um port novo a partir de um APK/.so >>
ports/<jogo>/    # cada port gerado vive aqui
docs/            # arquitetura + receita + referência (lswtcs)
```

## Fluxo de um port novo
```bash
# 1. bootstrap: extrai .so, classifica os símbolos, gera o esqueleto compilável
tools/new-port.sh ~/meujogo.apk meujogo

# 2. o tool reporta: X auto-resolvidos / Y a implementar (UNKNOWN)
#    edite ports/meujogo/src/imports.gen.c  (resolva os UNKNOWN)
#    edite jni_shim.c (package name + OBB path do jogo)

# 3. build (toolchain NextOS) e roda no device
make -C ports/meujogo
```
O `new-port.sh` mata o trabalho mais chato — a tabela de 200-370 símbolos — auto-mapeando libc/libm/GLES/pthread e listando só o que é específico do jogo.

## GLES1 vs GLES2
Cada jogo usa uma versão. O build linka `GLES_CM` (GLES1, ex. Syberia) **ou** `GLESv2` (GLES2, ex. LEGO SW) — configurável por port. (O `new-port.sh` detecta pela presença de símbolos GLES1-only como `glMatrixMode`/`glOrthof`.)

## Legal — BYO game files
Este repo é **só a ferramenta/loader** (como o PortMaster). Ele **não** distribui jogo nenhum. Você fornece o `.so` + assets de um APK **que você possui legalmente**. Uso não-comercial/hobbyista.

## Créditos
Núcleo derivado dos ports **[syberia_arm64](https://github.com/mtojek/syberia_arm64)** e **[lswtcs_arm64](https://github.com/mtojek/lswtcs_arm64)** de **mtojek** (licença **Apache-2.0**). Este framework generaliza aquele approach. Veja `NOTICE` para atribuição.
