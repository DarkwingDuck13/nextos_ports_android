# nextos_ports_android

Framework pra **portar jogos Android (ARM64, NativeActivity) pra Linux ARM64 / NextOS** — alvo principal os devices **Mali** (Amlogic-old/ng/nxtos, Utgard/Bifrost/Valhall).

Não recompila o jogo: **carrega o `.so` nativo do Android e roda direto** no Linux, com uma camada de shim que finge ser Android (fake JNI, OpenSL ES→SDL2, EGL→SDL2, bionic→glibc). Mesma linhagem dos so-loaders de PSVita (TheFloW), adaptada pra Linux ARM64 + SDL2.

> **Licença: [GPL-3.0](LICENSE)** — use, modifique e redistribua à vontade, **desde que** mantenha os créditos ao projeto (**NextOS**), mantenha esta mesma licença e **disponibilize o código-fonte das suas modificações** ao redistribuir (copyleft). Os jogos em si continuam sendo dos seus donos: tudo é **BYO-data** (você fornece o `.so`/dados do APK que **possui legalmente**).

> **Isto NÃO são ports PortMaster.** Cada jogo aqui roda a **versão ANDROID** (o `.so` do APK) via **so-loader** — não um build Linux/PC. O empacotamento aproveita o framework do PortMaster **só pra lançar** (control.txt + gptokeyb pra controle/sair), mas o que executa por dentro é o binário Android. Ports PortMaster "de verdade" (de builds Linux) desses jogos, quando existem, são projetos separados.

> **Provado no Mali-450 (Utgard):** os ports de referência **Syberia** (GLES1) e **LEGO Star Wars: TCS** (GLES2) rodam perfeitos — o caminho de render (so-loader + EGL→SDL2 + GLES) está validado no Utgard.

## Destaques

- **Bully: Anniversary Edition** — primeiro port em aarch64 / Linux / PortMaster (**inédito mundial**). O jogo completo da Rockstar via so-loader do `libGame.so` no Mali-450 (GLES2, fbdev): mundo aberto, escola, personagem, controle e áudio, **100% jogável**. Destraves: `hook_arm64` com pool de trampolins (colisão NvAPK), EGL via SDL2-mali, fixes GLES2 do Utgard (`highp→mediump`, `GL_LUMINANCE→RGBA`), o fix do `glClear` da roupa do Jimmy, e o limite de VRAM que travava a escola (`BULLY_TEX_*` + `asset_archive` O(log n)). Veja [`ports/bully`](ports/bully/).

- **Sonic Mania Plus** (RSDKv5 / Retro Engine, build Netflix) — **inédito mundial** no Mali-450 (GLES2, fbdev). Fluxo completo **com som**: logos → título → menu → save select → personagem → cutscene → Green Hill jogável. Destraves: título preso em cloud-save (`GetCloudSaveConflictState→0`), menu preto, save de jogo novo via `CloudSave` async, crash de fase na telemetria, e a receita de som (mixer puro `RSDK::Audio::MixToBuffer` na thread do SDL, bypass do Oboe). Veja [`ports/sonicmania`](ports/sonicmania/).

- **Castlevania: Symphony of the Night** (DotEmu) — port do zero via so-loader do `libsotn.so` (SDL2 estático, ES2 nativo) no Mali-450. Boot → título → menu → novo jogo → abertura "1792" + castelo, **com controle e som**; saves persistem. Destraves: relocação `R_AARCH64_ABS64` de imports indefinidos, canário bionic no TLS (`tpidr_el0+0x28`), stdio `__sF` bionic→glibc, assets case-insensitive, áudio AudioTrack→PulseAudio. Veja [`ports/sotn`](ports/sotn/).

- **GTA: Vice City** (reVC) — primeiro port feito **do zero** com o framework, 100% jogável no Mali-450. Documenta a arquitetura so-loader 2-módulos (libc++_shared + engine) e as receitas Mali-450/GLES2 reutilizáveis. Boa base ao portar o próximo jogo. Veja [`ports/revc`](ports/revc/).

- **Streets of Rage 4** (MonoGame/.NET 9) — roda **nativo** (não so-loader): o runtime .NET 9 CoreCLR + MonoGame em GLES2 executam o código gerenciado direto, com host próprio no lugar da `MainActivity`. Jogável com áudio, música original (Wwise) por um reimpl OpenAL leve; texturas ASTC→ETC1 na 1ª execução. Veja [`ports/sor4`](ports/sor4/).

- **Carrion** (MonoGame 3.8 / .NET 9) — o monstro-horror invertido roda **nativo** (mesma linhagem do SOR4, engine **nova**, não Unity): .NET 9 CoreCLR self-contained + MonoGame DesktopGL patchado + **gl4es** (GL desktop→GLES2 no Mali). **100% jogável** no Mali-450 **e no R36S**: render, controles, **som (FMOD real)** e jogo completo desbloqueado. Destraves: `SDL_NO_SIGNAL_HANDLERS` (SDL pisava no GC do .NET), stubs Mono.Android/Maui/InAppBilling, FMOD nativo via PulseAudio. Veja [`ports/carrion`](ports/carrion/).

- **Katana ZERO** (GameMaker Studio 2 / YYC, edição Netflix) — so-loader do `libyoyo.so` no Mali-450 **e no R36S**, com **binário único universal** (glibc 2.27, roda em qualquer device). Jogável com **ataque/controle** (fix do `buttonMask` do gamepad nativo), áudio (música/SFX por OGG streamed), inglês e resolução automática. Destraves: bypass do Netflix SDK via async event, áudio da thread OGG (`getJNIEnv`/`GetJavaVM`), e o `buttonMask` que filtrava todos os botões menos A. Veja [`ports/katanazero`](ports/katanazero/).

- **Final Fantasy VII** (Square Enix mobile) — o port Android é o **executável do FF7 PC (1998) recompilado pra ARM** sob a camada win32 própria da Square; o so-loader dirige esse engine duplo no Mali-450. **Fluxo completo nativo**: New Game → FMV de abertura (decode VP8 próprio + overlay GL com save/restore total de estado, áudio PCM sincronizado) → campo com Cloud, **sem tela preta**. Áudio 100% (SFX + música de campo/batalha + som dos vídeos), **saves funcionando**, inglês. Destraves: fim de filme = `MyDecoder.FRAME→0` (o -1 deixava o filme "tocando" pra sempre = tela preta E fundo do campo preto), `RenderMix` produz **float32** (alimentado como s16 = chiado), e o `__open` do engine cria pseudo-fd fantasma pra arquivo inexistente (save "invalid"). Veja [`ports/ff7`](ports/ff7/).

- **Sonic the Hedgehog 4: Episode II** (Sega NN/"fox") — so-loader do `libfox.so`, **validado multi-device** (Mali-450, X5M, R36S, muOS, ROCKNIX/Panfrost): vídeo, áudio, controle, bonus stages e resolução nativa; release arm64 via Docker buster (piso GLIBC 2.30). Veja [`ports/sonic4`](ports/sonic4/).

- **Mega Man Mobile 1–6** (Capcom, Cocos2d-x 3.9 + Cricket, armhf) — a série clássica inteira **rodando lisa** no Mali-450: vídeo, **controle dpad-touch** (diagonais reais no d-pad), áudio (Cricket→SDL com o áudio como clock mestre, corrige aceleração), save e **Select+Start pra sair**. Receita reusável (`cp -r megamanN megamanM`; `MAX_JARRAYS=8192`, ~15fps). Veja [`ports/megaman1`](ports/megaman1/) … [`ports/megaman6`](ports/megaman6/).

- **Secret of Mana** (remake 2018, plandroid/MCF, **GLES1 puro**) — so-loader no Mali-450, **jogável completo**: inglês, controle Xbox, fontes, save, BGM + SFX. Destraves: reconstruir o *fontcache* ao trocar de rasterizador, e drenar o ring do OpenSL ES pros SFX. Veja [`ports/secretofmana`](ports/secretofmana/).

- **Call of Duty: Black Ops Zombies** (Marmalade *s3e*) — so-loader no Mali-450: boot, New Game, gameplay, **fullscreen** (free_scale do OSD Amlogic), áudio e controle. Adaptação NextOS do loader de **[Producdevity](https://github.com/Producdevity/cod-boz-port)** (MIT — todo o crédito da arquitetura s3e é dele): **d-pad = movimento**, correções de áudio (callback `END_SAMPLE` da engine → tiros/SFX voltam), SDL do sistema e Select+Start. Veja [`ports/codboz`](ports/codboz/).

E mais jogáveis no Mali-450: **Hotline Miami 2** (GameMaker/YYC, música + gameplay), **DuckTales Remastered** (WayForward), **Minecraft Bedrock/MCPE** (fullscreen via mcpelauncher + SDL3), **Shantae and the Pirate's Curse** (WayForward, controles completos + áudio + 60fps + inglês), **Chrono Trigger** (Cocos2d-x, controle físico + áudio + inglês), **Terraria** (Unity IL2CPP) e **Crazy Taxi Classic**. Tabela completa abaixo.

## Jogos portados

Todos rodam a **versão Android** (o `.so` do APK) via so-loader, salvo onde indicado como **nativo**/**recomp**. Alvo principal: **Mali-450 (Utgard)**; vários validados também em R36S/X5M/muOS/ROCKNIX. **BYO-data**: o repo traz só o loader.

### ✅ Concluídos — jogáveis
| Jogo | Engine / método | Estado | Pasta |
|---|---|---|---|
| **Final Fantasy VII** (SQEX = FF7 PC recompilado p/ ARM) | so-loader + FMV VP8 próprio | Fluxo completo New Game→FMV→campo, áudio 100% (SFX+BGM+vídeos), saves, inglês | [`ports/ff7`](ports/ff7/) |
| **Sonic 4: Episode II** (Sega NN/"fox") | so-loader (armv7 + arm64) | Multi-device (Mali-450, X5M, R36S, muOS, ROCKNIX) — vídeo, áudio, controle, bonus stages | [`ports/sonic4`](ports/sonic4/) |
| **Sonic 4: Episode I** (Sega Hedgehog Engine) | so-loader | Boot completo, logos→título→fases, áudio e vídeo | [`ports/sonic4ep1`](ports/sonic4ep1/) |
| **Bully: Anniversary Edition** | so-loader (`libGame.so`) | Mundo aberto, escola, controle, áudio (Mali-450 GLES2; + R36S 1GB via streaming) | [`ports/bully`](ports/bully/) |
| **Castlevania: Symphony of the Night** (DotEmu) | so-loader (SDL2 nativo ES2) | Boot→título→menu→gameplay, áudio, controle, save persiste | [`ports/sotn`](ports/sotn/) |
| **GTA: Vice City** (reVC) | so-loader 2-módulos | 100% — mundo 3D, controle, áudio, menu, NPCs | [`ports/revc`](ports/revc/) |
| **Sonic Mania Plus** (RSDKv5, ed. Netflix) | so-loader | 100% com som — logos→título→menu→save→cutscene→fase | [`ports/sonicmania`](ports/sonicmania/) |
| **Streets of Rage 4** | **nativo** MonoGame/.NET 9 (GLES2) | Jogável — música/SFX (Wwise reimpl), texturas ETC1 | [`ports/sor4`](ports/sor4/) |
| **Carrion** | **nativo** MonoGame 3.8 / .NET 9 + gl4es | 100% (Mali-450 + R36S) — render, controle, **som (FMOD)** | [`ports/carrion`](ports/carrion/) |
| **Katana ZERO** (GameMaker/YYC, ed. Netflix) | so-loader — **binário único universal** glibc 2.27 | Jogável (Mali-450 + R36S) — ataque/controle nativo, áudio, inglês | [`ports/katanazero`](ports/katanazero/) |
| **DYSMANTLE** | so-loader + streaming nativo de texturas | Multi-device (Mali-450, X5M, R36S 1GB) — textura full-res paginada, áudio | [`ports/dysmantle`](ports/dysmantle/) |
| **Dead Space** (EA, so-loader armeabi) | so-loader (`libDeadSpace.so`) | Jogável (Mali-450) — gameplay, câmera/controle nativo (hooks InputRegions), som, save, HUD, saída blindada | [`ports/deadspace`](ports/deadspace/) |
| **Don't Starve** (Klei) | so-loader | Jogável — imagem, chão, áudio e controles funcionando; texturas ETC2→ETC1 dupla-camada | [`ports/dontstarve`](ports/dontstarve/) |
| **Hotline Miami 2** (GameMaker/YYC) | so-loader (`libyoyo.so`) | Jogável (Mali-450) — gameplay + música + SFX | [`ports/hlm2`](ports/hlm2/) |
| **DuckTales Remastered** (WayForward) | so-loader (armv7, FMOD) | Jogável (Mali-450) — menu, controle, gameplay | [`ports/ducktales`](ports/ducktales/) |
| **Minecraft Bedrock (MCPE 1.16)** | mcpelauncher (armhf) + SDL3 | Jogável fullscreen (Mali-450) | [`ports/mcpe`](ports/mcpe/) |
| **Chrono Trigger** (Cocos2d-x 3.14.1) | so-loader (ES2 nativo) | Jogável — render, controle físico (Xbox), áudio, inglês | [`ports/chrono`](ports/chrono/) |
| **Shantae and the Pirate's Curse** (WayForward) | so-loader (NativeActivity armv7, ES2) | Jogável — render + áudio + 60fps + inglês, controles completos | [`ports/shantae`](ports/shantae/) |
| **Crazy Taxi Classic** | so-loader (loader de **initdream** sobre o framework) | Jogável (Mali-450) — render + áudio + gptokeyb | [`ports/crazytaxi`](ports/crazytaxi/) |
| **Mega Man Mobile 1–6** (Capcom, Cocos2d-x 3.9) | so-loader (armhf) | Jogável — vídeo, controle dpad-touch, áudio, save, Select+Start | [`ports/megaman1`](ports/megaman1/) … [`megaman6`](ports/megaman6/) |
| **Secret of Mana** (remake 2018, MCF) | so-loader (**GLES1 puro**) | Jogável completo — inglês, controle, fontes, save, BGM+SFX | [`ports/secretofmana`](ports/secretofmana/) |
| **Call of Duty: Black Ops Zombies** (Marmalade s3e) | so-loader (loader de **Producdevity**, MIT) | Jogável — New Game, gameplay, fullscreen, áudio, controle (d-pad) | [`ports/codboz`](ports/codboz/) |
| **Terraria** (Unity IL2CPP) | so-loader | Jogável — controle + áudio + player/mundo | [`ports/terraria`](ports/terraria/) |
| **BADLAND** (Cocos2d-x, FMOD) | so-loader | Jogável (Mali-450) — render GLES2, áudio FMOD, texturas ETC2→ETC1 | [`ports/badland`](ports/badland/) |
| **Magic Rampage** (FMOD) | so-loader | Jogável (Mali-450) | [`ports/magicrampage`](ports/magicrampage/) |
| **Elderand** (Unity IL2CPP / URP 2D) | so-loader | Jogável (Mali-450) | [`ports/elderand`](ports/elderand/) |

### 🚧 Em andamento
| Jogo | Engine / método | Estado | Pasta |
|---|---|---|---|
| **GTA: Liberty City Stories** | so-loader | Carrega 100%, frame loop estável, gameplay 3D visível; ajustes finais | [`ports/lcs`](ports/lcs/) |
| **LEGO Batman 3: Beyond Gotham** (Fusion) | so-loader | Render limpo/estável (FBCOPY), título+menu, controle Xbox; falta gameplay | [`ports/legobatman`](ports/legobatman/) |
| **NFS Most Wanted (2012)** | so-loader (armhf) | Gameplay 3D + áudio OK; fontes do menu pendentes | [`ports/nfs`](ports/nfs/) |
| **Resident Evil 4** (Unity/Mono ARM32) | so-loader | Menu + entrada Cap.1; andar congela (deadlock job-system) | [`ports/re4`](ports/re4/) |
| **Final Fantasy IX** (Unity IL2CPP) | so-loader | Renderiza claro no fb0; caminho nativo destravado (Time.time) | [`ports/ff9`](ports/ff9/) |
| **Mega Man X** (Unity IL2CPP) | so-loader | Controles/menu por cursor completos; falta jogo novo nativo | [`ports/megamanx`](ports/megamanx/) |
| **Castlevania: Grimoire of Souls** (Unity 2018.4 IL2CPP) | so-loader | Deserializa a cena do Título; muro = skew asset↔metadata do APK mod | [`ports/cvgos`](ports/cvgos/) |
| **Graveyard Keeper** (Unity 2018.2 IL2CPP) | so-loader | Renderiza a tela de loading (caminho Terraria) | [`ports/graveyardkeeper`](ports/graveyardkeeper/) |
| **Cuphead** (Unity IL2CPP) | so-loader | WIP | [`ports/cuphead`](ports/cuphead/) |
| **Pixel Cup Soccer** (Unity IL2CPP) | so-loader | Loading renderiza; não passa dela (muro no 1º frame) | [`ports/pixelcup`](ports/pixelcup/) |
| **Rockman X DiVE Offline** (Unity) | so-loader | Boot avança; throttle/CRIWARE residual entre runs | [`ports/rockmanxdive`](ports/rockmanxdive/) |
| **NieR (reincarnation/automata)** (UE4) | so-loader | Loader OK, bootstrap do alocador UE4 resolvido | [`ports/nier`](ports/nier/) |
| **PES 2012** (Marmalade s3e) | so-loader | FSM de download mapeado, gate de 180MB bypassado; muro = OBB sound | [`ports/pes2012`](ports/pes2012/) |
| **The Amazing Spider-Man 2** (Gameloft) | so-loader | Boot estabiliza a 22fps, GL2JNI boot OK; muro = estado GAIA | [`ports/tasm2`](ports/tasm2/) |
| **Legend of Mana** (remake) | so-loader | Estudo de render (CLUT/pretos) | [`ports/legendofmana`](ports/legendofmana/) |
| **Limbo** | so-loader (NativeActivity) | Boot + render + tela inicial + controle; falta áudio Wwise | [`ports/limbo`](ports/limbo/) |
| **Left 4 Dead 2** (mobile) | so-loader (base infra RE4) | Scaffold compila (l4d2boot armhf) | [`ports/l4d2`](ports/l4d2/) |
| **Shantae: Seven Sirens** (WayForward "wf") | so-loader | Recon — engine renderiza; estudo do caminho | [`ports/sevensirens`](ports/sevensirens/) |
| **Summertime Saga** | so-loader | Trava das conversas resolvida (ETC1 dupla-camada runtime) | [`ports/summertimesaga`](ports/summertimesaga/) |
| **Dusklight** (Zelda: TP recomp) | recomp + backend Aurora GLES2 | Cena reconhecível (castelo de Hyrule) | [`ports/dusklight`](ports/dusklight/) |
| **Hollow Knight** (Unity IL2CPP) | so-loader | Pesquisa — renderiza em GLES3 (X5M, Mali-G310); muro = input | [`experiments/hollow-recon`](experiments/hollow-recon/) |

### 📚 Referência (base do framework)
| Jogo | Método | Pasta |
|---|---|---|
| **Syberia** (GLES1) · **LEGO Star Wars: TCS** (GLES2) | so-loader (ref. **mtojek**) — totalmente jogáveis no Mali-450 | [`docs/reference/syberia-src`](docs/reference/syberia-src/) · [`lswtcs-src`](docs/reference/lswtcs-src/) |

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

**Onde aprender:** a pasta [`facilitando_o_trabalho/`](facilitando_o_trabalho/) tem 13 receitas reutilizáveis (ponte pthread/ABI, Mali-450/GLES2, fake JNI, áudio, controle/gptokeyb, memória/VRAM, texturas ETC1/ETC2, display, empacotamento, ponteiros/hooks, Unity bootstrap/render/GC e o guia mestre de Unity ports) + troubleshooting (incl. **como pegar logs**: jnitrace pré-port + shim de log IL2CPP) + a **Matriz de Ports** (cada jogo já feito → a lição que ele ensinou). Cada port é um exemplo vivo; o próximo é mais fácil que o anterior.

**Portou algo novo?** Mande o port e documente o destrave — vira receita pro próximo. Só mantenha o crédito ao projeto (NextOS) e a regra BYO-data (nunca distribua dados de jogo).

## GLES1 vs GLES2
Cada jogo usa uma versão. O build linka `GLES_CM` (GLES1, ex. Syberia) **ou** `GLESv2` (GLES2, ex. LEGO SW) — configurável por port. (O `new-port.sh` detecta pela presença de símbolos GLES1-only como `glMatrixMode`/`glOrthof`.)

## Legal — BYO game files
Este repo é **só a ferramenta/loader** (como o PortMaster). Ele **não** distribui jogo nenhum. Você fornece o `.so` + assets de um APK **que você possui legalmente**. Uso não-comercial/hobbyista.

## Créditos
Núcleo derivado dos ports **[syberia_arm64](https://github.com/mtojek/syberia_arm64)** e **[lswtcs_arm64](https://github.com/mtojek/lswtcs_arm64)** de **mtojek** (licença **Apache-2.0**). Este framework generaliza aquele approach. Veja `NOTICE` para atribuição.

O port **Crazy Taxi Classic** usa o loader **[crazytaxi-aarch64](https://github.com/initdream/crazytaxi-aarch64)** de **[initdream](https://github.com/initdream)**, que o construiu **em cima deste framework**. Nós o **adaptamos para o Mali-450 (Utgard)**: recompilação no toolchain NextOS, mapeamento teclado→keycode Android para **gptokeyb**, e ajustes de áudio (PulseAudio). Versionamos **só o código/loader** — nenhum dado de jogo (copyright Sega) vai pro repo.

O port **Call of Duty: Black Ops Zombies** usa o loader Marmalade *s3e* **[cod-boz-port](https://github.com/Producdevity/cod-boz-port)** de **[Producdevity](https://github.com/Producdevity)** (licença **MIT** — toda a arquitetura do so-loader s3e é dele). Nós o **adaptamos para o Mali-450 (Utgard)**: SDL2 do sistema, **fullscreen por free_scale do OSD Amlogic**, **d-pad como movimento** para handhelds dpad-only, um conjunto de **correções de áudio** (assinatura real do `s3eSoundChannelRegister` + despacho do callback `END_SAMPLE` da engine, que traz de volta os SFX/tiros) e o hotkey Select+Start. Créditos e detalhes em [`ports/codboz`](ports/codboz/). Só o código/loader vai pro repo — dados do jogo (copyright Activision) são BYO.
