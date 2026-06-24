# nextos_ports_android

Framework pra **portar jogos Android (ARM64, NativeActivity) pra Linux ARM64 / NextOS** — alvo principal os devices **Mali** (Amlogic-old/ng/nxtos, Utgard/Bifrost/Valhall).

Não recompila o jogo: **carrega o `.so` nativo do Android e roda direto** no Linux, com uma camada de shim que finge ser Android (fake JNI, OpenSL ES→SDL2, EGL→SDL2, bionic→glibc). Mesma linhagem dos so-loaders de PSVita (TheFloW), adaptada pra Linux ARM64 + SDL2.

> **Licença / créditos:** qualquer um pode compilar e usar este framework e os ports — **desde que dê os devidos créditos** ao projeto (**NextOS**). Use, modifique e redistribua à vontade, só mantenha a atribuição. Os jogos em si continuam sendo dos seus donos: tudo é **BYO-data** (você fornece o `.so`/dados do APK que **possui legalmente**).

> **Isto NÃO são ports PortMaster.** Cada jogo aqui roda a **versão ANDROID** (o `.so` do APK) via **so-loader** — não um build Linux/PC. O empacotamento aproveita o framework do PortMaster **só pra lançar** (control.txt + gptokeyb pra controle/sair), mas o que executa por dentro é o binário Android. Ports PortMaster "de verdade" (de builds Linux) desses jogos, quando existem, são projetos separados.

> **Provado no Mali-450 (Utgard):** os ports de referência **Syberia** (GLES1) e **LEGO Star Wars: TCS** (GLES2) rodam perfeitos — o caminho de render (so-loader + EGL→SDL2 + GLES) está validado no Utgard.

## Destaques

- **Bully: Anniversary Edition** — primeiro port em aarch64 / Linux / PortMaster (**inédito mundial**). O jogo completo da Rockstar via so-loader do `libGame.so` no Mali-450 (GLES2, fbdev): mundo aberto, escola, personagem, controle e áudio, **100% jogável**. Destraves: `hook_arm64` com pool de trampolins (colisão NvAPK), EGL via SDL2-mali, fixes GLES2 do Utgard (`highp→mediump`, `GL_LUMINANCE→RGBA`), o fix do `glClear` da roupa do Jimmy, e o limite de VRAM que travava a escola (`BULLY_TEX_*` + `asset_archive` O(log n)). Veja [`ports/bully`](ports/bully/).

- **Sonic Mania Plus** (RSDKv5 / Retro Engine, build Netflix) — **inédito mundial** no Mali-450 (GLES2, fbdev). Fluxo completo **com som**: logos → título → menu → save select → personagem → cutscene → Green Hill jogável. Destraves: título preso em cloud-save (`GetCloudSaveConflictState→0`), menu preto, save de jogo novo via `CloudSave` async, crash de fase na telemetria, e a receita de som (mixer puro `RSDK::Audio::MixToBuffer` na thread do SDL, bypass do Oboe). Veja [`ports/sonicmania`](ports/sonicmania/).

- **Castlevania: Symphony of the Night** (DotEmu) — port do zero via so-loader do `libsotn.so` (SDL2 estático, ES2 nativo) no Mali-450. Boot → título → menu → novo jogo → abertura "1792" + castelo, **com controle e som**; saves persistem. Destraves: relocação `R_AARCH64_ABS64` de imports indefinidos, canário bionic no TLS (`tpidr_el0+0x28`), stdio `__sF` bionic→glibc, assets case-insensitive, áudio AudioTrack→PulseAudio. Veja [`ports/sotn`](ports/sotn/).

- **GTA: Vice City** (reVC) — primeiro port feito **do zero** com o framework, 100% jogável no Mali-450. Documenta a arquitetura so-loader 2-módulos (libc++_shared + engine) e as receitas Mali-450/GLES2 reutilizáveis. Boa base ao portar o próximo jogo. Veja [`ports/revc`](ports/revc/).

- **Streets of Rage 4** (MonoGame/.NET 9) — roda **nativo** (não so-loader): o runtime .NET 9 CoreCLR + MonoGame em GLES2 executam o código gerenciado direto, com host próprio no lugar da `MainActivity`. Jogável com áudio, música original (Wwise) por um reimpl OpenAL leve; texturas ASTC→ETC1 na 1ª execução. Veja [`ports/sor4`](ports/sor4/).

E mais jogáveis no Mali-450: **Chrono Trigger** (Cocos2d-x, controle físico + áudio + inglês), **Terraria** (Unity IL2CPP), **DYSMANTLE** (Mali-450 + X5M) e **Crazy Taxi Classic**. Tabela completa abaixo.

## Jogos portados
| Jogo | Engine / método | Estado | Pasta |
|---|---|---|---|
| **Bully: Anniversary Edition** | so-loader (`libGame.so`) | **Jogável** (Mali-450, GLES2) — mundo, escola, personagem, controle, áudio | [`ports/bully`](ports/bully/) |
| **Castlevania: Symphony of the Night** (DotEmu) | so-loader (SDL2 nativo ES2) | **Jogável** — boot→título→menu→gameplay, áudio, controle, save persiste | [`ports/sotn`](ports/sotn/) |
| **GTA: Vice City** (reVC) | so-loader 2-módulos | **Jogável** (Mali-450) — mundo, controle, áudio, menu, NPCs | [`ports/revc`](ports/revc/) |
| **Sonic Mania Plus** (RSDKv5) | so-loader | **Jogável com som** — título→menu→save→cutscene→fase | [`ports/sonicmania`](ports/sonicmania/) |
| **Streets of Rage 4** | MonoGame/.NET 9 nativo (não so-loader) | **Jogável** (Mali-450 GLES2) — música/SFX, texturas ETC1 | [`ports/sor4`](ports/sor4/) |
| **DYSMANTLE** | so-loader (GameActivity) | **Jogável** (Mali-450 + X5M) — mundo com cor, áudio | [`ports/dysmantle`](ports/dysmantle/) |
| **Terraria** (Unity IL2CPP) | so-loader | **Jogável** — controle + áudio + player/mundo | [`ports/terraria`](ports/terraria/) |
| **Chrono Trigger** (Cocos2d-x 3.14.1) | so-loader (ES2 nativo) | **Jogável** (Mali-450) — render, controle físico (padrão Xbox), áudio, inglês | [`ports/chrono`](ports/chrono/) |
| **Crazy Taxi Classic** | so-loader (loader de **initdream** sobre o framework) | **Jogável** (Mali-450) — render + áudio + gptokeyb | [`ports/crazytaxi`](ports/crazytaxi/) |
| **NFS Most Wanted (2012)** | so-loader (armhf) | **Parcial** — gameplay 3D + áudio OK; fontes do menu pendentes | [`ports/nfs`](ports/nfs/) |
| **Resident Evil 4** (Unity) | so-loader | **Demo** — menu + entrada Cap.1 OK; andar congela (deadlock job-system) | [`ports/re4`](ports/re4/) |
| **Dusklight** (Zelda: Twilight Princess recomp) | recomp + backend Aurora GLES2 | **Em progresso** — cena reconhecível (castelo de Hyrule) | [`ports/dusklight`](ports/dusklight/) |
| **Cuphead** (Unity IL2CPP) | so-loader | **WIP** | [`ports/cuphead`](ports/cuphead/) |
| **Hollow Knight** (Unity IL2CPP) | so-loader | **Pesquisa** — renderiza em GLES3 (X5M, Mali-G310); muro = input | [`experiments/hollow-recon`](experiments/hollow-recon/) |
| **Syberia** (GLES1) · **LEGO Star Wars: TCS** (GLES2) | so-loader (ref. **mtojek**) | **Referência** (base do framework) — totalmente jogáveis no Mali-450 | [`docs/reference/syberia-src`](docs/reference/syberia-src/) · [`lswtcs-src`](docs/reference/lswtcs-src/) |

> **Dois caminhos:** a maioria é **so-loader** (carrega o `.so` Android e roda direto); alguns são **nativos** — Streets of Rage 4 roda o runtime .NET 9 + MonoGame em GLES2, e Dusklight é um recomp. O empacotamento PortMaster (launcher + BYO-data) é o mesmo nos dois.

> Todos os ports são **BYO-data**: o repo traz só o código/loader; você fornece o `.so`/dados do APK que **possui legalmente**.

## Por que funciona tão bem
Android é Linux. O código do jogo é **ARM nativo** rodando no ARM do device — zero emulação de CPU. GLES é GLES (mesma API). Nos TV boxes, é praticamente o hardware nativo do jogo (mesmo SoC/GPU classe Android). Só a "casca" Android é trocada por SDL2/glibc.

## Estrutura
```
core/            # REUTILIZÁVEL entre todos os ports (não-editar por jogo)
  so_util.*      #   loader ELF arm64 (relocs, GOT, init_array, hook_arm64)  <- coração
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
docs/            # arquitetura + receita + referência (syberia + lswtcs + crazytaxi)
facilitando_o_trabalho/  # base de conhecimento: receitas + troubleshooting + Matriz de Ports
```

## Quer portar um jogo novo?
**Esse é o convite.** O framework existe pra que mais gente porte mais jogos — e o trabalho mais chato já está resolvido. Pegue um APK que você possui, rode o bootstrap e siga as receitas.

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

**Onde aprender:** a pasta [`facilitando_o_trabalho/`](facilitando_o_trabalho/) tem 11 receitas reutilizáveis (ponte pthread/ABI, Mali-450/GLES2, fake JNI, áudio, controle/gptokeyb, memória/VRAM, texturas ETC1/ETC2, display, empacotamento, ponteiros/hooks) + troubleshooting + a **Matriz de Ports** (cada jogo já feito → a lição que ele ensinou). Cada port é um exemplo vivo; o próximo é mais fácil que o anterior.

**Portou algo novo?** Mande o port e documente o destrave — vira receita pro próximo. Só mantenha o crédito ao projeto (NextOS) e a regra BYO-data (nunca distribua dados de jogo).

## GLES1 vs GLES2
Cada jogo usa uma versão. O build linka `GLES_CM` (GLES1, ex. Syberia) **ou** `GLESv2` (GLES2, ex. LEGO SW) — configurável por port. (O `new-port.sh` detecta pela presença de símbolos GLES1-only como `glMatrixMode`/`glOrthof`.)

## Legal — BYO game files
Este repo é **só a ferramenta/loader** (como o PortMaster). Ele **não** distribui jogo nenhum. Você fornece o `.so` + assets de um APK **que você possui legalmente**. Uso não-comercial/hobbyista.

## Créditos
Núcleo derivado dos ports **[syberia_arm64](https://github.com/mtojek/syberia_arm64)** e **[lswtcs_arm64](https://github.com/mtojek/lswtcs_arm64)** de **mtojek** (licença **Apache-2.0**). Este framework generaliza aquele approach. Veja `NOTICE` para atribuição.

O port **Crazy Taxi Classic** usa o loader **[crazytaxi-aarch64](https://github.com/initdream/crazytaxi-aarch64)** de **[initdream](https://github.com/initdream)**, que o construiu **em cima deste framework**. Nós o **adaptamos para o Mali-450 (Utgard)**: recompilação no toolchain NextOS, mapeamento teclado→keycode Android para **gptokeyb**, e ajustes de áudio (PulseAudio). Versionamos **só o código/loader** — nenhum dado de jogo (copyright Sega) vai pro repo.
