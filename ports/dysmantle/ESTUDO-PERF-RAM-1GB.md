# DYSMANTLE — Estudo de desempenho/RAM (devices ≤1GB) + flickers

> Estudo só (NÃO aplicado). Base medida 2026-06-15 com o jogo rodando no X5M.
> Objetivo: caber/rodar liso em **devices de ≤1GB de RAM** e matar os flickers.

## Definição do alvo: por RAM, NÃO por GPU  (decisão do porter 2026-06-15)
Gatear as economias de RAM por **capacidade de RAM (≤1GB)**, não pelo nome da GPU. Assim o
perfil cobre QUALQUER device de 1GB — **Mali-450/Amlogic (fbdev)**, **R26S (KMSDRM)**, R36S etc.
**Dois eixos independentes:**
- **RAM-tier (≤1GB)** → texturas/ring/heap (T3, T4, T5). Detecção no launcher:
  `MemTotal` de `/proc/meminfo` ≤ ~1310720 kB (1.25GB) → liga o "perfil 1GB".
- **Present/flicker** → depende do **backend de display** (KMSDRM vs fbdev), NÃO da RAM nem da
  GPU. R26S e X5M = KMSDRM; Mali-450/Amlogic = fbdev (T1).
- **Formato de textura (T3)** → depende da **capacidade da GPU** (amostra ETC2? só ETC1?),
  detectada em RUNTIME por extensão GL — independente de RAM e do nome do device.

## Princípio: GERAL vs gateado  (decisão do porter 2026-06-15)
**Se a melhoria é estritamente melhor (ou neutra) em todo lugar → aplica pra TODOS.** Só
gateia no perfil ≤1GB o que **custa algo** (nitidez, resolução, alpha) em device capaz.
Classificação das frentes por esse critério:

| Frente | Tipo | Aplica em |
|---|---|---|
| **T1** vsync/flicker | melhoria por-**backend** (sem downside) | **TODOS** do backend (fbdev; KMSDRM a validar) |
| **T2** resolução interna | **trade-off** (perde resolução) | **só ≤1GB** (capaz fica nativo) |
| **T3a** ETC2 passthrough | **GERAL** (menos RAM/CPU, mesma qualidade) | **TODOS** com GPU que amostra ETC2 |
| **T3b** ETC1 transcode | **trade-off** (perde qualidade/alpha) | **só ≤1GB** sem ETC2 (ex: Utgard) |
| **T4a** mipmaps | **GERAL** (menos shimmer, +cache) | **TODOS** |
| **T4b** TEXSCALE agressivo | **trade-off** (perde nitidez) | **só ≤1GB** |
| **T5** ring 512KB / MAX_PLAYERS / heap | **GERAL** se 0 underrun (só corta desperdício) | **TODOS** (validar com o medidor) |

Ou seja: **T1, T3a, T4a, T5 = melhorias gerais (todos os devices)**; **T2, T3b, T4b = só no
perfil ≤1GB** (trade-off de qualidade). Validação sempre com o medidor (`[PERFAUD]`/`[PERF]`/RSS).

## Baseline medido (X5M, jogo em combate, ~11min)
- **VmRSS ≈ 955 MB**, VmData ≈ 1159 MB, swap 0. → num device de 1GB (livre ~800MB) **estoura**.
- Render em **1080p nativo** (fullscreen, `SDL_GetDesktopDisplayMode`), **~30 fps GPU-bound** (852MHz).
- **vsync OFF** por padrão (`DYSMANTLE_SWAPINT=0` no launcher).
- Texturas sobem **RGBA8 NÃO-comprimido** na VRAM (TEXSCALE só reduz dimensão, bilinear).
- Audio ring agora 16MB (1MB×16) — já cortado de 64MB (v4).

## Onde a RAM vai — BREAKDOWN MEDIDO (smaps, X5M G310)
RSS 790MB no momento da medida, por mapeamento:
- **`[anon]` = 654 MB** ← DOMINANTE. No driver Mali a memória de textura/buffer da GPU é
  mapeada como anônima → **a maior parte do RSS é TEXTURA/GPU.**
- `[heap]` = 97 MB (heap da engine), `libMali.valhall.g310.so` = 29 MB, libs ~8 MB.
- Audio ring (16MB) e o resto são ruído perto da textura.
TEXSCALE confirmado ativo (512→393, 1024→787, fator 1.3) mas sobe **RGBA8 não-comprimido**.
Nesta instalação ETC2-decodes=0 → a engine carrega **JPEG→RGBA8** (o fixpak já converteu o pak).
⚠️ Medido no **X5M (G310, ES3, 3.6GB)**; o **Mali-450 (Utgard, fbdev, ~1GB)** aloca diferente,
mas a conclusão "textura domina a RAM" vale igual (e dói mais lá). → T2/T3 são as alavancas.

🔎 **Achado quick-win:** o shim **SEMPRE decodifica ETC2→RGBA** (`imports.c` ~390), mesmo no
X5M cuja G310 **amostra ETC2 nativo**. Passar o ETC2 direto (sem decode) onde a GPU suporta
= textura 8× menor SEM transcoder. Ver T3.

---

## T1 — Flicker: vsync/present (gate por BACKEND de display, não por GPU)  ⭐ (barato, ataca a QUEIXA)
**Causa provável:** `DYSMANTLE_SWAPINT=0` (vsync off) → sem sincronizar com o refresh →
**tearing/flicker**. O raciocínio "vsync+limiter=double-pacing 30fps" que motivou o `=0` era
do **X5M (KMSDRM)**. O flicker apareceu no **Mali-450 (fbdev)**. Como o present difere por
**backend** (KMSDRM vs fbdev), o gate de vsync deve ser por backend:
- **fbdev** (Mali-450/Amlogic): testar `SWAPINT=1` (vsync on) → provável fim do tearing.
- **KMSDRM** (X5M, **R26S**): pode precisar de valor diferente; o R26S (KMSDRM) é o caso a
  validar à parte — não assumir que é igual ao Mali-450. Pode ser que o `=0` sirva, ou que o
  KMSDRM queira `=1` sem o double-pacing (depende do compositor/limiter).
Confirmar também double-buffer real (`SDL_GL_DOUBLEBUFFER 1` em egl_shim.c:96; no fbdev checar
se o SDL3-mali-fbdev realmente faz flip duplo). **Proposta:** `SWAPINT` por-backend no launcher
(detecta `/dev/dri/card0` = KMSDRM, senão fbdev). Teste 100% config primeiro. **Custo: baixo.
Risco: baixo.** Arquivos: `DYSMANTLE.sh` (knob por-backend), `egl_shim.c` (SwapInterval já existe).

## T2 — Resolução interna real (render do MUNDO em FBO menor + upscale)  ⭐⭐ (maior alavanca perf)
**Estrutura medida ao vivo:** **89% dos draws vão pro FBO de cena** (DRAWSTATS fbo=11.3M vs
screen=1.4M, ~8:1). O FBO de cena tem attachment **cor `0x8CE0`** + **depth `0x8D00`** (há 2
jogos de targets: tex 20/21 e 197/198 = scene + pós/double). A tela (fbo 0, 11%) é só
composite + UI. → mexer na resolução do FBO ataca **~89% do custo de GPU** e **deixa a UI nativa**.
**Hoje:** o FBO de cena é alocado no tamanho da tela (1080p) → gargalo de fill no Utgard.
O `render_scale` da config da engine é **ZOOM de câmera, NÃO resolução** (já testado/descartado).
**Proposta:** interceptar a alocação dos attachments do FBO de cena (`my_glTexImage2D`/
`my_glRenderbufferStorage`/`my_glFramebufferTexture2D`, quando dim==tela) e criar a **0.6–0.7×**
(~720p), ajustar `glViewport` enquanto o FBO de cena está ligado, e o composite pra tela já
faz o **upscale** (amostra a textura de cor menor). Receita "InternalResolution 0.5" dos
ports-ship adaptada. Ganha fill-rate (mais fps / frames mais estáveis = menos flicker
percebido) **e** corta RAM do FBO. **Custo: médio-alto** (casar as dims dos 2 targets + o
viewport; cuidar pra não encolher a UI). Arquivos: `imports.c` (hooks FBO JÁ existem:
`g_cur_fbo`, `my_glBindFramebuffer`, `my_glFramebufferTexture2D`, `my_glRenderbufferStorage`).

## T3 — Texturas comprimidas na GPU em vez de RGBA8  ⭐⭐⭐ (maior alavanca RAM)
**Hoje:** todas as texturas do mundo viram **RGBA8** na VRAM (32 b/px) — confirmado pelo
breakdown (654MB anon). Divide em duas frentes por-device:

**T3a — X5M (G310/ES3): passar ETC2 DIRETO (quick-win, baixo risco).**
A G310 amostra ETC2 nativo, mas o shim decodifica pra RGBA assim mesmo. Proposta: gatear
`my_glCompressedTexImage2D` (imports.c ~390) — se a GPU suporta ETC2 (ES3/extensão), chamar
o `glCompressedTexImage2D` REAL com os blocos ETC2 (não decodificar) + reativar o KTX-redirect
(`DYSMANTLE_FORCE_ETC2`, já existe) pra engine carregar os .ktx. → **~8× menos RAM de textura
no X5M, sem transcoder**, e sem CPU de decode. Risco: confirmar que o "mundo branco" (que era
shader-alias, JÁ corrigido) não volta. **Custo: baixo-médio.**

**T3b — Mali-450 (Utgard/ES2): transcodar pra ETC1.**
O Utgard **suporta `GL_OES_compressed_ETC1_RGB8_texture`** mas NÃO ETC2. Proposta: pras
texturas **opacas RGB**, gerar ETC1 (transcode dos blocos ETC2→ETC1, ou encode rápido do
RGBA→ETC1 no upload) e `glCompressedTexImage2D(ETC1)` → 4 b/px. Texturas com **alpha**
(punchthrough/RGBA8) seguem RGBA reduzido. **Esse é o corte decisivo pra caber em 1GB.**
**Custo: alto** (encoder ETC1 + classificar opaco/alpha + fragilidade histórica — incremental,
começar por um subconjunto). Arquivos: `imports.c`, `etc2_decode.c` (+ um `etc1_encode.c`),
`fixpak.c` (poderia manter ETC e nem re-encodar JPEG).

## T4 — TEXSCALE por-device + mipmaps  ⭐ (quick win RAM/qualidade)
**Proposta:** em device de 1GB, subir o default `DYSMANTLE_TEXSCALE` de **1.3 → ~1.8–2.0**
(dimensão 55–50% → RAM de textura ~30–25%). Junto: **gerar mipmaps + LINEAR_MIPMAP**
(receita Bully: mipmap→LINEAR) pras texturas encolhidas não cintilarem ao longe — esse
shimmer de item distante **pode estar lendo como "flicker"**. Troca: leve perda de nitidez.
**Custo: baixo** (config + `glGenerateMipmap`/`glTexParameteri`). Arquivos: `imports.c`
(bloco TEXSCALE linha ~806), `DYSMANTLE.sh`. Bom de combinar com T1 pro teste de flicker.

## T5 — Orçamento do resto (audio ring + heap + data.pak)  (raspar os últimos MB)
Cortes aditivos pra fechar a conta em 1GB depois de T2/T3:
- **Audio ring 1MB→512KB/player** (16→8MB) e/ou **MAX_PLAYERS 16→12** — medir com o
  `[PERFAUD] underruns=/dead=` (já temos o medidor). Reversível por env.
- **fixpak**: hoje INFLA o data.pak (571→597MB em disco) re-encodando ETC2→JPEG/RGBA. Se T3
  (manter ETC) entrar, o fixpak fica mais leve / quase dispensável.
- **VmData 1.1GB**: investigar as maiores alocações anônimas (staging de textura, mapeamentos
  do so-loader) — achar o próximo bloco de 50–100MB.
**Custo: baixo-médio, diagnóstico.** Arquivos: `opensles_shim.c` (RING/MAX_PLAYERS), `fixpak.c`.

---

## Ordem sugerida (custo↑ / risco↑)
1. **T1** (vsync por-device) — testa o flicker já, config-only.
2. **T4** (TEXSCALE+mipmap por-device) — RAM rápida + reduz shimmer.
3. **T5** (ring/heap) — raspa MB com medidor na mão.
4. **T2** (resolução interna) — grande ganho de fps/estabilidade.
5. **T3** (ETC1) — maior corte de RAM, maior risco; incremental.

## Notas de risco / lições já conhecidas (não repetir)
- `render_scale` da engine = zoom, NÃO resolução (descartado).
- Texturas ETC2 direto no Mali-450 = **não amostra** → mundo branco (por isso decodifica hoje).
  ETC1 (T3) é diferente: É suportado no Utgard — esse é o caminho.
- Bilinear/stridefix/atlas: vários red-herrings já mapeados em imports.c (ver comentários).
- Medir SEM achismo: `[PERF]` (fps), `[PERFAUD]` (underruns/dead), `VmRSS` em /proc, `free -m`.

---
## MEDIDO IN-GAME no .127 (Mali-450 Utgard, 832MB, fbdev) — 2026-06-16
Device de prova = **.127** (senha <senha>): Mali-450/Utgard, **832MB RAM + 511MB swap**, fbdev.
- **Port do zero OK**: PSX apagada (20G), assets(1.4G fixados)+libs+binário deployados.
  ⚠️ ES lista de **`/storage/roms/ports_scripts/<Nome>.sh`** (NÃO `ports/`); dados em `ports/dysmantle/`.
- **🔊 ÁUDIO estava MUDO** (não era T5): launcher forçava `alsa`, mas .127 roda **PulseAudio**
  e o HDMI (AML-M8AUDIO) rejeita estéreo. FIX = `SDL_AUDIODRIVER=pulse` se `pgrep pulseaudio`
  (X5M=PipeWire segue alsa). Validado `underruns=0 dead=0`. (commit 2609f66)
- **T1 vsync** (fbdev→1) e **T5 ring 512KB** aplicados+validados (2609f66).
- **MEDIDAS in-game** (jogador no mundo): **RSS ~533MB + 32MB swap do jogo**; sistema:
  **64M livre, ~346MB de SWAP em uso**. fps ~25-30 **GPU-bound** (não RAM). TEXSCALE 1.3 vs 2.0
  ~igual na área inicial (textura grande não é o gargalo lá). → **roda mas APERTADO, vive no swap**.
- **Conclusão**: cortar os 533MB (T3) tira do swap = mais liso; T2 baixa FBO = +fps. Fazer os 2.

## PLANO T2+T3 (decisão do porter 2026-06-16: "fazer os 2", T3 "direito no extrator")
- **T3 (ETC1) NO EXTRATOR/fixpak, não no runtime**: o `fixpak.c` (roda 1x na instalação)
  passa a **transcodar as texturas opacas pra ETC1** e gravar no pak (em vez de decodificar
  ETC2→JPEG/RGBA). Runtime sobe `glCompressedTexImage2D(ETC1)` direto → ~8× menos RAM, zero
  CPU/frame. **Encoder ETC1 NOVO E PRÓPRIO** (NÃO usar o do Bully — é bugado; escrever
  `src/etc1_encode.c` do zero, correto). Texturas com alpha (punchthrough/RGBA8) seguem
  caminho RGBA. Utgard suporta `GL_OES_compressed_ETC1_RGB8` (0x8D64).
- **T2 (resolução interna)**: interceptar os attachments do FBO de cena (cor 0x8CE0 + depth
  0x8D00, 89% dos draws) p/ 0.65× + viewport; composite faz upscale. Ganha fps + corta FBO.
- Validar in-game medindo RSS/swap/fps (entrada: jogador entra; autônoma instável p/ attract).

---
## CONCLUSÃO FINAL (.127, medido 2026-06-16) — os "downscale" NÃO ajudam aqui
Investigação completa no Mali-450/832MB revelou os DOIS gargalos reais:
- **fps ~31 = DRAWCALL/CPU-bound, NÃO fill-rate.** A engine JÁ renderiza a cena em
  **768×432** (~0.6×) e dá upscale. Forçar mais baixo (T2: 538×302) = **mesmo fps**.
  → resolução interna (T2) e tamanho de textura (TEXSCALE) NÃO mudam fps.
- **RAM ~471MB = pools da engine** (`General Pool`/`StageObjectAllocatorPage`, ~344MB em
  regiões anon), NÃO texturas. → ETC1 (T3) e TEXSCALE (T4) NÃO reduzem a RAM. A/B
  controlado: ETC1 ON≈OFF. Subir TEXSCALE 1.3→2.0 só perde nitidez por ~nada.
- **Logo: T2/T3/T4 (texturas/resolução) são becos sem saída neste device.** Implementados
  e CORRETOS (T2 renderiza ok, ETC1 41-44dB sem bug), mas não movem a agulha. Ficam como
  knobs opt-in (DYSMANTLE_ISCALE, DYSMANTLE_NO_ETC1).
- **Ganhos REAIS da sessão**: 🔊 áudio (estava MUDO, fix pulse), T1 vsync (flicker), T5 ring.
- **O que REALMENTE ajudaria** (deep/arriscado, não feito): fps→reduzir drawcalls (batching
  da engine); RAM→encolher os pools da engine (RE do alocador). Ambos = trabalho de engine.
- Veredito: o jogo roda ~tão bem quanto dá nesse Mali-450 via esses levers (~31fps, cabe em
  832MB com ~160MB swap). O que faltava de verdade era o ÁUDIO (resolvido) e o flicker (T1).
