<div align="center">

# 🎮 nextos_ports_android

### Rode jogos **Android** nativamente em **Linux ARM64** — sem recompilar, sem emular

<p>
  <img alt="Licença" src="https://img.shields.io/badge/licença-GPL--3.0-blue?style=for-the-badge">
  <img alt="Jogáveis" src="https://img.shields.io/badge/jogáveis-35-brightgreen?style=for-the-badge">
  <img alt="Em andamento" src="https://img.shields.io/badge/em%20andamento-21-orange?style=for-the-badge">
</p>
<p>
  <img alt="Alvo" src="https://img.shields.io/badge/alvo-Mali%20·%20ARM64%20·%20Linux-lightgrey?style=flat-square">
  <img alt="PC" src="https://img.shields.io/badge/PC%20x86__64-roadmap-yellow?style=flat-square">
  <img alt="Render" src="https://img.shields.io/badge/render-EGL%20→%20SDL2%20·%20GLES1%2FGLES2-8A2BE2?style=flat-square">
  <img alt="Devices" src="https://img.shields.io/badge/devices-Amlogic%20·%20R36S%20·%20X5M%20·%20muOS%20·%20ROCKNIX-informational?style=flat-square">
  <img alt="BYO-data" src="https://img.shields.io/badge/BYO--data-sem%20jogo%20no%20repo-critical?style=flat-square">
</p>

</div>

---

Carrega o `.so` nativo do APK e o executa **direto** no Linux, com uma camada de shim que finge ser Android (fake JNI, OpenSL ES→SDL2, EGL→SDL2, bionic→glibc). O código ARM do jogo roda no ARM do device — **zero emulação de CPU**, GLES é GLES. Alvo principal: handhelds e TV boxes **Mali** (Amlogic Utgard/Bifrost/Valhall, R36S, X5M, muOS, ROCKNIX). Mesma linhagem dos so-loaders de PSVita (TheFloW), adaptada para Linux ARM64 + SDL2.

> [!IMPORTANT]
> **BYO-data.** O repositório contém **apenas o loader/código** — nenhum dado de jogo. Você fornece o `.so` e os assets de um APK que **possui legalmente**. Uso não-comercial/hobbyista.

> [!NOTE]
> Estes **não** são ports PortMaster: cada jogo roda a versão **Android** via so-loader, não um build Linux/PC. O empacotamento PortMaster é usado apenas para lançar (control.txt + gptokeyb).

## ✨ Destaques

Vários **primeiros ports mundiais** em Linux/aarch64: **Bully: Anniversary Edition** (Rockstar — mundo aberto completo), **Sonic Mania Plus** (RSDKv5), **GTA: Vice City**, **Final Fantasy VII** (com FMV próprio) e **Dead Space**. **Streets of Rage 4** e **Carrion** rodam **nativos** (runtime .NET 9 + MonoGame em GLES2). Cada port documenta seus destraves técnicos na própria pasta.

## 🕹️ Jogos portados

Todos rodam a **versão Android** (o `.so` do APK) via so-loader, salvo onde indicado como **nativo**/**recomp**. Alvo principal: **Mali-450 (Utgard)**; muitos validados também em R36S/X5M/muOS/ROCKNIX.

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
| **Summertime Saga** (Ren'Py→Android) | so-loader | Jogável (Mali-450) — conversas destravadas (ETC1 dupla-camada runtime) | [`ports/summertimesaga`](ports/summertimesaga/) |
| **BADLAND** (Cocos2d-x, FMOD) | so-loader | Jogável (Mali-450) — render GLES2, áudio FMOD, texturas ETC2→ETC1 | [`ports/badland`](ports/badland/) |
| **Magic Rampage** (FMOD) | so-loader | Jogável (Mali-450) | [`ports/magicrampage`](ports/magicrampage/) |
| **Battlefield: Bad Company 2** (Karisma, GLES1) | so-loader (armeabi) | Jogável (Mali-450) — FPS 1ª pessoa, mundo 3D+NPC+HUD, gamepad nativo (AppOnKeyEvent/Joystick) | [`ports/bfbc2`](ports/bfbc2/) |
| **Castle of Illusion** (Sega "oz") | so-loader (NativeActivity+FMOD, arm64) | Jogável (Mali-450) — render+controle+áudio | [`ports/castleofillusion`](ports/castleofillusion/) |
| **LEGO Star Wars: A Força Desperta** (Fusion/WB) | so-loader (arm64, base lswtcs) | Jogável (Mali-450) — menu + mundo + **fases entram e jogam**, controle Xbox padrão, música MP3, cutscene inicial; dt-clamp render-only + fix do deadlock de compilação de shaders no load | [`ports/lswtfa`](ports/lswtfa/) |
| **LEGO Ninjago: Shadow of Ronin** (Fusion/WB) | so-loader (arm64, base lswtfa) | Jogável (Mali-450) — menu → New Game → fase, controle Xbox padrão, áudio MP3, inglês, save persistente, Select+Start | [`ports/ninjago`](ports/ninjago/) |
| **LEGO Batman 3: Beyond Gotham** (Fusion/WB) | so-loader (arm64, base ninjago) | Jogável (Mali-450) — menu com **fundo animado** → New Game → fase, controle Xbox padrão, áudio MP3, inglês, Select+Start | [`ports/legobatman`](ports/legobatman/) |
| **LEGO Batman 2: DC Super Heroes** (Fusion/WB, engine SH1) | so-loader (arm64, `libLEGO_SH1.so`, base lswtfa) | Jogável (Mali-450) — menu → New Game → fase, **gamepad NATIVO** (hook `fnaController_Poll`; JNI `nativeControllerSetData` é stub), áudio MP3, inglês, Select+Start | [`ports/legobatman2`](ports/legobatman2/) |
| **LEGO The Lord of the Rings** (Fusion/WB) | so-loader (**armeabi-v7a softfp** GLES2, `libLEGO_LOTR.so`) | Jogável (Mali-450) — **1º port ARM 32-bit da família**; menu → New Game → fase 3D do Prólogo, combate, movimento por analógico/dpad, áudio OpenSL | [`ports/lotr`](ports/lotr/) |
| **LEGO Jurassic World** (Fusion/WB) | so-loader (arm64, `libProject_Amber_Mobile.so`, base ninjago) | Jogável (Mali-450) — menu → New Game → fase, controle Xbox padrão, áudio, inglês | [`ports/ljw`](ports/ljw/) |
| **LEGO Marvel Super Heroes: Universe in Peril** (Fusion/WB) | so-loader (arm64, `libLEGO_M1.so`) | Jogável (Mali-450) — OBB de 1.9GB montado nativamente em C, gamepad (hook `fnaController_Poll`), stick direito = cursor de menu, som | [`ports/marvel`](ports/marvel/) |
| **LEGO Harry Potter: Years 5–7** (Fusion/WB) | so-loader (**armeabi-v7a / GLES1**, `libLEGO_HP2.so`, base lotr) | Jogável (Mali-450) — menu → gameplay, controle nativo (confirm nativo, mapa por contexto, pause no Start), inglês | [`ports/legohp2`](ports/legohp2/) |

### 🚧 Em andamento
| Jogo | Engine / método | Estado | Pasta |
|---|---|---|---|
| **GTA: Liberty City Stories** | so-loader | Carrega 100%, frame loop estável, gameplay 3D visível; ajustes finais | [`ports/lcs`](ports/lcs/) |
| **LEGO Harry Potter: Years 1–4** (Fusion/WB) | so-loader (**armv7/GLES1**, `libLEGOHarry.so`, base lotr) | Gameplay jogável com controle nativo (hub + HUD + studs) e música FLAC; falta polish: cenário preto (texturas), 16:9 real, SFX | [`ports/legohp1`](ports/legohp1/) |
| **NFS Most Wanted (2012)** | so-loader (armhf) | Gameplay 3D + áudio OK; fontes do menu pendentes | [`ports/nfs`](ports/nfs/) |
| **Resident Evil 4** (Unity/Mono ARM32) | so-loader | Menu + entrada Cap.1; andar congela (deadlock job-system) | [`ports/re4`](ports/re4/) |
| **Final Fantasy IX** (Unity IL2CPP) | so-loader | Renderiza claro no fb0; caminho nativo destravado (Time.time) | [`ports/ff9`](ports/ff9/) |
| **Mega Man X** (Unity IL2CPP) | so-loader | Controles/menu por cursor completos; falta jogo novo nativo | [`ports/megamanx`](ports/megamanx/) |
| **Elderand** (Unity IL2CPP / URP 2D) | so-loader | Investigação de render | [`ports/elderand`](ports/elderand/) |
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
| **Dusklight** (Zelda: TP recomp) | recomp + backend Aurora GLES2 | Cena reconhecível (castelo de Hyrule) | [`ports/dusklight`](ports/dusklight/) |
| **Hollow Knight** (Unity IL2CPP) | so-loader | Pesquisa — renderiza em GLES3 (X5M, Mali-G310); muro = input | [`experiments/hollow-recon`](experiments/hollow-recon/) |

### 📚 Referência (base do framework)
| Jogo | Método | Pasta |
|---|---|---|
| **Syberia** (GLES1) · **LEGO Star Wars: TCS** (GLES2) | so-loader (ref. **mtojek**) — totalmente jogáveis no Mali-450 | [`docs/reference/syberia-src`](docs/reference/syberia-src/) · [`lswtcs-src`](docs/reference/lswtcs-src/) |

## ⚙️ Como funciona

Android é Linux e o código do jogo é **ARM nativo** — GLES é GLES (mesma API), sem tradução de CPU nem de gráficos. Só a "casca" Android é substituída por SDL2/glibc. Nos TV boxes é praticamente o hardware nativo do jogo (mesmo SoC/GPU classe Android).

Dois caminhos: a maioria é **so-loader** (carrega o `.so` e roda direto); alguns são **nativos** (SOR4 e Carrion executam o runtime .NET 9 + MonoGame; Dusklight é um recomp). O build linka **GLES1** (`GLES_CM`, ex. Syberia) ou **GLES2** (`GLESv2`, ex. LEGO Star Wars) por port — o `new-port.sh` detecta pela presença de símbolos GLES1-only.

## 🗂️ Estrutura do projeto

```
core/                    # reutilizável entre todos os ports (não editar por jogo)
  so_util.*              #   loader ELF arm64 (relocs, GOT, init_array, hook_arm64) — o coração
  egl_shim.*             #   EGL  -> SDL2
  opensles_shim.*        #   OpenSL ES -> SDL2 (ring buffer SPSC + resample)
template/src/            # base por-jogo, copiada e adaptada a cada port
  main.c                 #   loader flow + GOT hooks + crash recovery
  android_shim.*         #   fake native_app_glue (paths, input, resolução)
  jni_shim.*             #   fake JNI (package name, OBB path, feature flags)
tools/new-port.sh        # gera um port novo a partir de um APK/.so
ports/<jogo>/            # cada port vive aqui
facilitando_o_trabalho/  # base de conhecimento: receitas + troubleshooting + Matriz de Ports
```

## 🚀 Portar um jogo novo

```bash
# 1. bootstrap: extrai o .so, classifica símbolos, gera o esqueleto compilável
tools/new-port.sh ~/meujogo.apk meujogo

# 2. resolva os símbolos UNKNOWN em ports/meujogo/src/imports.gen.c
#    e ajuste jni_shim.c (package name + OBB path)

# 3. build + roda no device
make -C ports/meujogo
```

O `new-port.sh` auto-mapeia libc/libm/GLES/pthread (a tabela de 200–370 símbolos) e lista só o que é específico do jogo. A pasta [`facilitando_o_trabalho/`](facilitando_o_trabalho/) reúne 16 receitas reutilizáveis (pthread/ABI, Mali-450/GLES2, fake JNI, áudio, controle/gptokeyb, VRAM, texturas ETC1/ETC2, hooks, Unity bootstrap/render/GC), troubleshooting e a Matriz de Ports (cada jogo → a lição que ensinou). Contribuições são bem-vindas: mantenha o crédito ao projeto (**NextOS**) e a regra BYO-data.

### 🧭 Direções do framework

- **JNI por tabela ([`nx_jni`](facilitando_o_trabalho/kit_essencial/core/nx_jni.h))** — declara os métodos JNI numa tabela em vez de escrever `switch` à mão. [Receita 14](facilitando_o_trabalho/receitas/14-jni-por-tabela.md).
- **Loaders genéricos por engine** — 1 binário roda N jogos de uma engine (GameMaker/Cocos2d-x) via `game.cfg`, sem port novo. [Receita 15](facilitando_o_trabalho/receitas/15-loaders-genericos-por-engine.md).
- **Alvo PC (x86_64) / multiarch** — a mesma técnica carrega o `.so` x86_64 de um APK e roda em desktop Linux; ótimo pra debugar (gdb/asan) antes do device. [Receita 16](facilitando_o_trabalho/receitas/16-alvo-pc-e-multiarch.md).

Agentes de IA (Codex/Claude) que forem trabalhar no repo: leiam o [`AGENTS.md`](AGENTS.md).

## 📜 Licença e créditos

**[GPL-3.0](LICENSE)** — use, modifique e redistribua mantendo a mesma licença, os créditos ao projeto (**NextOS**) e o código-fonte das suas modificações (copyleft). Os jogos continuam sendo dos seus donos (BYO-data).

- Núcleo derivado de **[syberia_arm64](https://github.com/mtojek/syberia_arm64)** e **[lswtcs_arm64](https://github.com/mtojek/lswtcs_arm64)** de **mtojek** (Apache-2.0). Veja `NOTICE`.
- **Crazy Taxi Classic** usa o loader **[crazytaxi-aarch64](https://github.com/initdream/crazytaxi-aarch64)** de **initdream** (construído sobre este framework), adaptado por nós para o Mali-450.
- **Call of Duty: Black Ops Zombies** usa o loader Marmalade s3e **[cod-boz-port](https://github.com/Producdevity/cod-boz-port)** de **Producdevity** (MIT), adaptado por nós para o Mali-450.
