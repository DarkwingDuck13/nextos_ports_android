# DYSMANTLE (NextOS Elite / PortMaster) — CHANGELOG

---

## 📋 MINI CHANGELOG — v3 → v5 (resumo do que mudou)

- **v3** — Janela de **extração com progresso** (BYO-DATA: põe seu APK, o jogo extrai/conserta
  sozinho na 1ª vez). Texturas garantidas (conserto ETC2→JPEG/PNG, anti-branco). 2 binários (ArkOS).
- **v4** — 🔊 **ÁUDIO CORRIGIDO (importante):** os SFX de combate **sumiam após ~2 min** — a causa
  era o `cb_dead` envenenando o **pool de 16 players** reusado pela engine (1 som morria → slot
  morto pra sempre). Fix: guarda no callback + des-envenenar no reuse + ring 1 MB + pump thread
  dedicada (4 ms) → `underruns=0 dead=0`. + frame pacing + ETC1 em runtime (T3) + render scale.
- **v5** — **Conversão de textura 100% OFFLINE** no progressor (nada convertido durante o jogo =
  sem stutter): cache **ETC1** (4 bpp, ~8× menos VRAM no Mali) + **downscale escolhível** (1.0/1.2/
  1.3/2.0, estilo SOR4) + **1 binário universal** (glibc 2.27, roda em qualquer aarch64).
- **v5 (final)** — 💗 **MAGENTA RESOLVIDO** com **verificação de conteúdo** + downscale como opção
  real (re-gera o cache se você mudar a escala).

---

## v5 (final) — MAGENTA RESOLVIDO (verificação de conteúdo) + downscale opção

### 💗 Anti-magenta: verificação de conteúdo no upload
- O cache é indexado por NOME, mas o nome da textura pode ficar **velho** entre uploads
  (mips/atlas/FBO). Quando uma textura ERRADA batia a mesma dimensão (512²/256²/128² são comuns)
  de uma entrada do cache, subia a **ETC1 errada → chão magenta**.
- **Fix:** antes de subir a ETC1 do cache, o binário **decodifica uma amostra dos blocos e compara
  com o RGBA real** que a engine vai desenhar. Bate (textura certa) → sobe ETC1; não bate (colisão
  de nome) → usa a textura normal (RGBA8) → **cor correta, zero magenta**. Sem encode em runtime.

### 🎚️ Downscale como OPÇÃO (estilo SOR4) — `DYSMANTLE_TEXSCALE`
- `1.0` qualidade total · `1.2` leve · **`1.3` recomendado** · `2.0` máx economia (1 GB).
- Aplicado **offline** (bakeado no cache na escala escolhida) **e** no runtime juntos → reduz
  TODAS as texturas (não só as do cache) = o maior ganho de RAM em 1 GB. Mudar a escala **re-gera
  o cache sozinho** na próxima abertura.

### 🧱 Por que NÃO dá "ETC1 puro no pak" (igual SOR4) — muro do motor (confirmado)
- O motor do DYSMANTLE é **FECHADO** e, neste device **ES2/Mali-450**, o carregador de `.ktx`
  (`KtxImageLoader`) **dá NULL-deref/SIGSEGV ao abrir QUALQUER `.ktx`** — porque no ES2 o motor só
  usa JPEG; o caminho `.ktx` só existe no ES3 e é quebrado aqui. Geramos o `.ktx` ETC1 **byte-idêntico
  ao original** (com mips) e mesmo assim crasha **antes** do upload GL. Comprovado no crash.
- Diferente do SOR4 (motor MonoGame **nosso**, mandamos carregar ETC1). Aqui a única via é o **hook
  no upload** (engine carrega JPEG → trocamos por ETC1 do cache) — equivalente ao texcache do SOR4.

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
