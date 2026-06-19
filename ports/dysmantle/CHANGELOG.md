# DYSMANTLE (NextOS Elite / PortMaster) — CHANGELOG

---

## v5.1 — LEAN PAK (corta a duplicação de disco) + por que não dá "ETC1 puro sem cache"

### 🗜️ Disco: ~metade do que as texturas gastavam
- O progressor agora **enxuga o pak** depois de gerar o cache (`texbake --leanpak`): as
  texturas cobertas pelo cache viram um **placeholder mínimo** (o jogo lê os pixels REAIS
  do cache no hook — só importam as dimensões, que batem), as de alpha vêm reais, e os
  `.ktx` ETC2 (mortos no runtime, só serviam de fonte do bake) são **dropados**. Resultado:
  o pak deixa de **duplicar** as texturas que já estão no cache → ~200 MB a menos no device.
- **Atômico + seguro**: se faltar espaço no disco, PRESERVA o pak e cai pro modo `fixpak`
  (texturas reais, sem enxugar). Nunca deixa pak corrompido. O APK é apagado logo após a
  extração (libera espaço pro bake caber).
- Com o cache presente o launcher força `TEXSCALE=1.0` (reduzir as dims bypassaria a guarda
  de tamanho do cache → cache-miss → encode em runtime/stutter; o ETC1 já é 4bpp).

### 🧱 Por que NÃO existe "ETC1 puro sem cache" (igual SOR4)
- O motor do DYSMANTLE é FECHADO e **escolhe o decodificador de textura pelo formato do
  MANIFEST**, não pela extensão do arquivo. Forçar a engine a carregar `.ktx` (renomeando)
  faz ela tratar o KTX como JPEG → `LoadBitmapInternal` falha → crash. Mesmo gerando o KTX
  ETC1 com a cadeia de mips IDÊNTICA à original, o muro persiste (é antes do upload GL).
- Diferente do SOR4 (onde NÓS controlamos o motor MonoGame e mandamos carregar ETC1), aqui
  a única via é o **hook no upload** (engine carrega JPEG → trocamos por ETC1). O ETC1 PRECISA
  morar em algum lugar que o hook leia (o "cache" = o store ETC1, equivalente ao texcache do
  próprio SOR4). O lean pak elimina a DUPLICAÇÃO, que era o custo real de disco.

---

## v5 — CONVERSÃO OFFLINE NO PROGRESSOR + 1 BINÁRIO (sem conversão em jogo)

> Tudo é preparado UMA VEZ na 1ª execução (janela do progressor). O jogo abre LIMPO,
> **sem conversão de textura durante o gameplay** (sem stutter). Pacote BYO-DATA: você
> põe o seu APK do DYSMANTLE 1.4.1.12, abre, e o progressor faz tudo sozinho.

### 🧊 Cache ETC1 OFFLINE (o fim dos stutters) — estilo SOR4
- Na 1ª execução o **`texbake`** gera um **cache ETC1** (nome→ETC1, só texturas OPACAS) a
  partir dos paks. Em runtime o jogo **sobe a ETC1 já pronta** (lookup por nome no hook de
  upload) → **ZERO encode/decode de textura em jogo** = sem travadas. Antes o ETC1 era
  encodado em runtime (causava stutter ao entrar em áreas novas).
- **`texbake` é multi-thread (cores-1) + streaming** (RAM baixa, ~60MB): NÃO acumula o
  cache na RAM (seriam ~600MB → OOM no device de 1GB); grava em disco na hora. Roda com
  `nice` p/ o device não "congelar" durante o bake.
- 🌑 **Mapas de iluminação (normals/specular/heights) NÃO viram ETC1** (ficam RGBA8): ETC1
  é lossy e correlaciona RGB → destruía a luz → chão/personagem/árvores PRETOS. Agora a
  iluminação fica correta.
- 🛡️ **Guarda de tamanho** no lookup do cache (só substitui se as dims batem) → não pega
  textura errada por nome desatualizado (FBO/mip).

### 🎮 1 BINÁRIO universal
- Acabou o esquema de 2 binários. Agora **um só** `dysmantle` (GLIBC velha, compilado no
  Docker/`debian:bullseye`) que roda em **qualquer aarch64** (glibc ≥ 2.27: ArkOS/R36S até
  NextOS/X5M/2.30+). O launcher não detecta mais glibc.

### 🪟 Progressor (1ª execução) faz TUDO
- Extrai do APK → **fixpak** (preenche os .jpg/.png vazios = imagem completa, anti-branco)
  → **texbake** (cache ETC1) → apaga o APK. Com barra de % na tela.
- A geração do cache demora (é a parte pesada, ~minutos), mas é **uma vez só**; depois o
  jogo abre direto e limpo.

---

## 📋 RESUMO — o que mudou da v2 → v3 (pra quem já testou a v2)

- 🪟 **Instalação com JANELA de extração + barra de %** na tela (BYO-DATA): você põe o
  seu APK do DYSMANTLE 1.4.1.12 na pasta, abre, e uma janela mostra a extração e o
  **conserto das texturas** (`Dados principais… [###…] 32%`); no fim o jogo inicia
  sozinho. Antes era texto cru no terminal, sem feedback.
- 🎨 **Texturas garantidas (anti-branco/lavado)**: o conserto ETC2→JPEG/PNG (`fixpak`)
  agora roda **dentro da janela** na 1ª vez **e** tem uma **rede de segurança** no
  launcher (se os dados existem mas nunca foram consertados, conserta antes de abrir).
  Resolve os relatos de "personagens brancos / texturas lavadas".
- 🎮 **Dois binários** no pacote: `dysmantle` (glibc ≥ 2.38: NextOS/muOS/ROCKNIX/
  JELOS/X5M) e `dysmantle.compat` (GLIBC_2.27, GCC Debian 10: **ArkOS/dArkOS/R36S**).
  O launcher escolhe sozinho pela glibc do device (via `ldd`, com fallback).
- 📺 **Vídeo auto-detectado** (KMSDRM no Mali novo, mali/fbdev no Mali-450) — um
  pacote, todos os backends.
- 🧹 Launcher enxuto no padrão **Bully v9** (só chama a janela; lógica no
  `tools/dysmantle_extract.src`). Correção do **auto-kill** no X5M (o `pkill -f
  dysmantle` casava o próprio launcher; agora usa `-x`, comm exato).

Detalhes técnicos abaixo.

---

## v3 — 2026-06-14 — JANELA DE EXTRAÇÃO + 2 BINÁRIOS (ArkOS) + texturas garantidas

> Pacote no padrão do **Bully v9**: instalação BYO-DATA com **janela e barra de % na
> tela** (extração + conserto de texturas) e **dois binários** (glibc novo + GLIBC_2.27)
> pra cobrir do NextOS ao ArkOS/R36S. Validado no **X5M** (KMSDRM) e no **Mali-450**
> (fbdev).

### 🪟 Janela de extração (BYO-DATA com progresso)
- Coloque o seu **APK do DYSMANTLE 1.4.1.12** em `ports/dysmantle/` e abra o port. Na
  1ª vez abre uma **JANELA** (ferramenta `progressor`) que extrai a `libNativeGame.so`
  + a pasta `assets/` (~800 MB) e **conserta as texturas** mostrando **barra de %**.
  Ao terminar, libera o APK e o **jogo inicia sozinho**.
- Funciona no **KMSDRM** (Mali novo/X5M) e no **fbdev** (Mali-450/Utgard). A lógica
  está toda em `tools/dysmantle_extract.src`; o `DYSMANTLE.sh` só chama a janela.

### 🎨 Texturas garantidas (o conserto ETC2 → JPEG/PNG)
- Muitos APKs (mods "APK_Award"/"unlocked") deixam os **JPEG/PNG VAZIOS** dentro do
  `.pak` — só o irmão `.ktx` (ETC2 zlib) tem dados — → **personagens/chão/itens
  brancos ou lavados**. O `fixpak` (helper aarch64, decoder ETC2 puro em C) decodifica
  o ETC2 no próprio device e reencoda JPEG (libturbojpeg) / PNG (libz), preenchendo
  **todos os slots** de `data.pak` (539) + `data-gfx1200.pak` (104).
- **Roda na 1ª extração (dentro da janela)** e, como **rede de segurança**, o launcher
  também conserta se achar dados sem o marcador `.textures_fixed` (ex.: assets copiados
  na mão). Assim **nunca** abre branco/lavado, em qualquer caminho de instalação.
- `libc++_shared.so` **vem no pacote** (versão validada) → o loader não depende da
  versão de libc++ que vier no APK do usuário.

### 🎮 Dois binários (cobre ArkOS/dArkOS/R36S)
- `dysmantle` (nativo, glibc ≥ 2.38) e **`dysmantle.compat`** (GLIBC_2.27, GCC Debian
  10 — mesma técnica do `bully.compat`). O launcher detecta a glibc do device com
  `ldd --version` (fallback `getconf`; vazio → nativo) e escolhe sozinho.
- O `dysmantle.compat` cobre **ArkOS/dArkOS/R36S** (Ubuntu 18.04 = glibc 2.27).

### 🧹 Launcher (padrão Bully v9) + fix de auto-kill no X5M
- 🔑 **Auto-kill corrigido:** o guard de instância única do X5M usava `pkill -f
  dysmantle`, que casa a **própria linha de comando** do launcher (contém "dysmantle")
  → matava a si mesmo antes de extrair. Agora usa `pkill -x dysmantle` (comm exato); a
  engine renomeia a thread p/ "Main", então não casa o jogo em execução (o exit real é
  **SELECT+START** no binário).
- `controlfolder` robusto; vídeo auto-detect; `DYSMANTLE_TEXSCALE` (default 1.3)
  editável no topo do `.sh`.

---

## v2 / v1-test (2026-06-12/13) — primeiros pacotes pra comunidade

**Render**
- 🏆 Mundo COMPLETO renderizando (chão, pedras, lixeiras, árvores, água). Causa-raiz
  histórica: shaders `*Shadows` (feature_level=2) eram pulados no target GL tier-1 →
  vertex format 0 → geometria do mundo nunca era criada. Fix: fallback automático de
  shader (degrada `Shadows/Reflections/Heights/Specular/Normals/Glow/Fur→Diffuse/Lit`).
- Resolução dinâmica: segue o framebuffer do device. `DYSMANTLE_TEXSCALE` (default 1.3).

**Performance / Áudio**
- Áudio sem engasgo: pump thread dedicada de 4ms pros callbacks OpenSLES + buffer SDL
  4096→1024 frames (latência 93ms→23ms). Ring buffer 64→16 MB por player. Fade-out
  anti-click no stop (`DYSMANTLE_NO_FADEOUT_STOP` desliga).
- JNI de bateria implementado (a engine via "bateria 0%" e capava 30fps). vsync off por
  default (`DYSMANTLE_SWAPINT=0`). Medidores `[PERF]`/`[PERFAUD]`/`[PERFCPU]` no log.

**Controles**
- Padrão PortMaster: gptokeyb + `dysmantle.gptk`; sticks e gatilhos analógicos direto do
  pad; D-pad = quick slots. **SELECT+START sai** (check no binário, independe do nome do
  processo — a engine renomeia p/ "Main").

**Base**
- so-loader 2 módulos (libc++_shared + libNativeGame) com pad TLS anti-canary bionic.
- Oboe real via shim OpenSLES→SDL2; Paddleboat alimentado direto do C; GameActivity (AGDK)
  emulado; saves Android compatíveis (gamedata/).

**Limitações conhecidas**
- Dynamic Shadows: manter OFF (crash no load em GPU Utgard; sem efeito após o fallback).
- Device de 1 GB pode engasgar perto da fogueira (cena densa) — jogável, sem crash.
