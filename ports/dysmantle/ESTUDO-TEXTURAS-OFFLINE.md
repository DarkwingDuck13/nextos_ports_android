# DYSMANTLE — ESTUDO: "tudo pronto antes do jogo, nada em runtime" (texturas offline + 1 binário)

> Estudo (NÃO aplicado ainda). Base: APK original `com.dysmantle53.soco.GP_1.4.1.12-APK_Award.apk`
> + repo atual `ports/dysmantle`. Pedido do porter (2026-06-18): replicar o que deu **super
> certo no SOR4** — TODA conversão de textura/qualquer fix acontece **offline, no progressor
> (1x na instalação)**, gerando as **melhores texturas pro device**. O runtime só **carrega e
> sobe**. **NADA em tempo real dentro do jogo.** Port mais **limpo**, menos RAM, **1 binário só**.

---

## 0. Princípio (palavras do porter)
- **TUDO pronto ANTES do jogo iniciar — qualquer textura, qualquer fix.**
- **NADA em tempo real dentro do jogo.**
- **1 binário só** (glibc velha ~2.17/2.27/2.30) → roda em **qualquer device**.
- **Mudar tudo** do Dysmantle nessa direção. Device de trabalho: **<device-ip>**.

---

## 1. O template que funcionou: SOR4
No SOR4 (`tools/sor4_extract.src`), o progressor faz numa passada, 1x:
1. extrai do APK; 2. patcha o código; 3. **`sor4texconv` converte ASTC→ETC1 (+downscale)**
direto pra `gameassets`; 4. apaga o APK. **O Mali lê ETC1 nativo → runtime sobe direto, zero
conversão em jogo.** Menu de qualidade (downscale 1/2/3) e modo (rápida/low-mem) no controle.

**Lição central:** o jogo nunca decodifica/transcoda textura em runtime — recebe o formato
final do device. É exatamente isso que queremos no Dysmantle.

---

## 2. Inventário REAL das texturas do Dysmantle (medido do APK original)

### data.pak — 544 MB, 24842 entradas
| tipo | total | com dados | VAZIOS (size=0) | bytes |
|---|---|---|---|---|
| `.jpg` | 8295 | 7826 | **469** | 63 MB |
| `.png` | 1584 | 1514 | **70** | 37 MB |
| `.ktx` (ETC2 zlib) | 539 | 539 | 0 | 46 MB |

**Os 539 `.ktx` (fonte ETC2):**
- **489× `0x9274` = ETC2 RGB8 (OPACO)** → irmãos: 469 `.jpg` vazios + 20 `.png`
- **50× `0x9278` = ETC2 RGBA8 (ALPHA)** → irmãos: 50 `.png` vazios
- dims: 256² (264), 512² (218), 1024² (50), 256×128 (5), 512×256 (1), **2048² (1)**

### data-gfx1200.pak — 145 MB, 8324 entradas
- 7225 `.jpg` + 994 `.png` + 104 `.ktx`. (1 jpg vazio.)

### data-localizations.pak — 79 MB
- só texto/fontes/png de UI (15 png). Sem `.ktx`.

### 🔑 Conclusões do inventário
1. **Há DOIS conjuntos de textura:**
   - **(A) JPEG/PNG com dados reais** (7826+1514 na data.pak, +7225+994 na gfx1200) — a MAIORIA.
     A engine carrega e **decodifica pra RGBA8 sozinha (libjpeg dentro do `.so`)** em runtime.
   - **(B) 539+104 `.ktx`** cujos irmãos `.jpg`/`.png` estão **VAZIOS** (mod "APK_Award"). Só o
     `.ktx` (ETC2) tem dado. Hoje o `fixpak` decodifica esses ETC2 e **re-encoda JPEG** pra
     preencher o slot vazio (e às vezes sobrescreve o `.ktx` in-place — bagunça: achei 466 slots
     `.ktx` virados JPEG `ffd8` no stage atual).
2. **Separação OPACO vs ALPHA é limpa e dá pra detectar:**
   - ETC2 RGB8 (`0x9274`) e a maioria dos `.jpg` = **OPACO** → vira **ETC1** (4 bpp).
   - ETC2 RGBA8 (`0x9278`) e os `.png` = **ALPHA** → precisa caminho RGBA (ETC1 não tem alpha).
3. **A re-codificação JPEG do fixpak é perda dupla** (ETC2→RGBA→JPEG→RGBA) e **infla o disco**.
   No fluxo limpo ela some.

---

## 3. O que roda em RUNTIME hoje (é isso que vamos ELIMINAR)
Em `src/imports.c`:
- **`my_glTexImage2D`** (linha ~862): para CADA textura que a engine sobe —
  - **TEXSCALE** (downscale RGBA por fator, ~884–930) — trabalho por-load.
  - **`try_upload_etc1`** (~830/968): **encoda RGBA→ETC1 NO DEVICE, no load** (`etc1_encode_image`).
    Encoder ETC1 rodando na CPU fraca a cada textura = **stutter/hitch de carregamento**.
- **`my_glCompressedTexImage2D`** (~395): com `FORCE_ETC2`, **decodifica ETC2→RGBA em runtime**.
- **Engine**: decodifica **JPEG/PNG→RGBA8 internamente** a cada load (libjpeg no `.so`).
- **`fixpak`** (offline, mas sujo): ETC2→RGBA→**re-JPEG**, infla/atropela slots.

➡️ **Resumo: hoje há até 3 conversões, 2 delas em RUNTIME** (JPEG-decode da engine + ETC1-encode
nosso). O objetivo é deixar **ZERO** em runtime.

---

## 4. Arquitetura-alvo (offline tudo, 1 binário, universal)

### 4.1 Formato universal de textura — a sacada
- **ETC1 (`GL_ETC1_RGB8_OES` 0x8D64) é amostrado por TODA GPU relevante:** Utgard/Mali-450 (ES2)
  **e** qualquer GPU ES3 (ETC2 é superset e inclui ETC1). → **ETC1 = UM formato opaco pra TODOS
  os devices.** Não precisa por-device pra opaco.
- **Alpha:** ETC1 não tem alpha. Como as texturas com alpha são minoria:
  - **universal simples:** subir alpha como **RGBA4444** (16 bpp, metade do RGBA8, roda ES2+ES3)
    ou RGBA8 puro. Sem compressão, mas são poucas.
  - (opção futura ES3-only: ETC2 RGBA — não vale a complexidade de detectar agora.)

➡️ **Plano de formato (offline):**
| classe | origem | vira | bpp |
|---|---|---|---|
| opaca | `.jpg` reais + ETC2 RGB8 (`0x9274`) + `.png` opacos | **ETC1** (0x8D64) | 4 |
| alpha | `.png` reais + ETC2 RGBA8 (`0x9278`) | **RGBA4444** (ou RGBA8) | 16 (ou 32) |

### 4.2 Onde a conversão acontece
Um **`texbake`** (sucessor limpo do `fixpak`) roda **no progressor, 1x**:
- lê cada textura do pak/APK (JPEG/PNG/KTX), decodifica pra RGBA,
- classifica opaca/alpha (alpha-scan dos pixels; já temos o sinal do formato KTX e da extensão),
- **opaca → ETC1**, **alpha → RGBA4444**, opcional downscale (menu estilo SOR4),
- grava o resultado **no formato que o runtime sobe DIRETO** (ver 4.3),
- **não re-encoda JPEG nunca mais.**

### 4.3 Como o runtime sobe SEM converter (o ponto crítico a validar) ⚠️
A engine escolhe `.jpg` vs `.ktx` por `NX_Graphics_IsTextureFormatSupported` (GL caps). Hoje no
ES2 ela pega o `.jpg`. Pra entregar textura pré-pronta, 2 estratégias (validar no .164):
- **(E1) Via `.ktx` + hook passthrough:** reescrever os slots como **KTX com ETC1 (0x8D64)** /
  KTX com RGBA4444, fazer `IsTextureFormatSupported(ETC1)`→true, e a engine chama
  `glCompressedTexImage2D`/`glTexImage2D` que **nossos hooks já interceptam** → **passam o bloco
  DIRETO** pro GL real (sem decode/encode). Prova de que a engine chama o caminho compressed: o
  hook `my_glCompressedTexImage2D` já existe e recebe os `.ktx`.
- **(E2) Container próprio + loader hook:** definir um header mínimo nosso (`magic+fmt+w+h+blocos`)
  e interceptar a leitura/loader de textura pra subir direto. Mais controle, mais trabalho.
- **Risco real:** casar a SELEÇÃO da engine (qual arquivo ela lê) com o formato que pré-bakeamos.
  É o **único item de pesquisa-no-device** deste plano. Resolver primeiro num punhado de texturas.

### 4.4 Um binário só (glibc velha) — FÁCIL
- O `build_compat_gcc.sh` já gera **GLIBC_2.17** (o símbolo máx é 2.17 porque o código só usa
  funções antigas). Ou seja **já roda em qualquer aarch64 glibc** (ArkOS/R36S 2.27, NextOS 2.38,
  X5M, …).
- **Ação:** adotar o compat como **O** binário (`dysmantle`), **apagar** o build nativo e **toda a
  lógica de detecção de glibc** no `DYSMANTLE.sh` (linhas ~76–85). Launcher encolhe.

---

## 5. Libs necessárias (o que já temos / o que falta)
| precisa | pra quê | status |
|---|---|---|
| **zlib** | inflar `.ktx`, deflar saída/pak | ✅ já (via dlopen no fixpak) |
| **decoder ETC2** | ler os `.ktx` fonte (0x9274/0x9278) → RGBA | ✅ `src/etc2_decode.c` |
| **encoder ETC1** | RGBA→ETC1 (opacas) | ✅ `src/etc1_encode.c` (próprio, correto) |
| **decoder JPEG** | ler os ~15 mil `.jpg` REAIS p/ converter | ❌ **FALTA** → `stb_image.h` |
| **decoder PNG** | ler os ~2,5 mil `.png` REAIS | ❌ **FALTA** → `stb_image.h` (cobre os 2) |
| **RGBA→RGBA4444** | empacotar alpha (16 bpp) | trivial (escrever, ~10 linhas) |
| ~~encoder JPEG~~ | — | ⛔ **SAI** do fluxo (`jpeg_enc.c` aposentado) |

- **`stb_image.h`** (single-header, domínio público) decodifica JPEG **e** PNG, **sem depender de
  lib do device** — encaixa no `texbake` (que roda no device, no progressor). Resolve o único furo.
- O `texbake` é **autossuficiente** (stb_image + etc2_decode + etc1_encode + zlib), igual ao espírito
  do `fixpak` (helper aarch64 no pacote).

---

## 6. Expectativa de RAM — HONESTO (não prometer demais)
O `ESTUDO-PERF-RAM-1GB.md` mediu no **.127 (Mali-450, 832 MB)**:
- **RSS ~471–533 MB dominado por POOLS DA ENGINE** (`General Pool`/`StageObjectAllocator`, ~344 MB),
  **não por textura.** A/B controlado: **ETC1 ON ≈ OFF** no RSS daquele device.
- ➡️ **Este trabalho NÃO vai derrubar muito o RSS no .127 específico.** Não vender isso.

**Mas os ganhos REAIS que ele entrega (e que o porter pediu):**
1. **Port limpo** — o princípio "nada em runtime" cumprido; fim do fixpak que infla/atropela.
2. **Load mais rápido + sem stutter** — some o JPEG-decode da engine **e** o ETC1-encode em runtime
   (esse encode na CPU fraca é hitch de verdade).
3. **VRAM ~8× menor nas opacas** (4 bpp vs 32) — ajuda devices **GPU/VRAM-bound** e onde textura
   DOMINA o RSS (ex.: X5M tinha **654 MB anon = textura**; lá derruba).
4. **Disco menor** — sem re-JPEG inflando o pak.
5. **1 binário** — manutenção e distribuição mais simples.
6. **Vale re-medir** o RSS com o caminho 100%-offline: o A/B antigo usava o encode-em-runtime, que
   aloca staging; o caminho limpo pode medir diferente.

---

## 7. Plano por fases (custo↑ / risco↑) — validar cada uma no .164
1. **F0 — 1 binário** (baixo) — **DECISÃO do porter 2026-06-18**: adotar o **compat como O binário
   principal** (`dysmantle`), apagar o nativo + toda a detecção de glibc no `.sh`. Requisito:
   o compat **tem que ter TUDO corrigido** (mesmo `src/` completo, todos os fixes) e a
   **compilação dele é SEMPRE no Docker** (`debian:bullseye`, `build_compat_gcc.sh` → GLIBC_2.17).
   Testar boot no .164. *(ganho imediato, risco ~zero.)*
2. **F1 — provar o passthrough** (médio, é o RISCO): pegar ~10 texturas, gerar KTX-ETC1 / RGBA4444
   offline, fazer a engine carregá-las e os hooks subirem **direto** (E1). Medir: aparece certo? sem
   decode/encode em runtime? Decide E1 vs E2. *(porta de entrada de tudo.)*
3. **F2 — `texbake`** (médio): tool offline autossuficiente (stb_image+etc2+etc1+zlib) que converte
   o pak INTEIRO: opacas→ETC1, alpha→RGBA4444, opcional downscale. Substitui o `fixpak`.
4. **F3 — limpar o runtime** (baixo): remover `try_upload_etc1`, `TEXSCALE`, `FORCE_ETC2` decode e o
   resto da conversão-em-runtime de `imports.c`. Runtime vira só passthrough.
5. **F4 — progressor + menu** (baixo): janela estilo SOR4 com menu de qualidade (downscale) e
   barra de %, chamando o `texbake`. Atualizar README/CHANGELOG. Empacotar **v5** (1 binário).
6. **F5 — medir** (baixo): RSS/VRAM/fps/load-time no .164 e (se der) num device VRAM-bound, com o
   medidor. Registrar honesto.

---

## 7.5 💡 OPÇÃO AVANÇADA GUARDADA (aprovada, 2026-06-18): ETC1 + plano de alpha separado
Pro "resto" (texturas com alpha), em vez de RGBA4444/5551 (16 bpp), dá pra chegar a **~8 bpp**:
- guardar o **RGB como ETC1** (4 bpp) + o **alpha como uma 2ª textura** (ETC1 do canal A, ou L8/EAC),
  e **combinar no shader** (amostra as 2 e faz `gl_FragColor.a = alphaTex.r`).
- Total ~8 bpp pro alpha (vs 16 do 4444) → mais RAM economizada.
- **Custo/risco:** exige **mexer no shader** do jogo (injetar uma variante que amostra 2 texturas só
  pras texturas com alpha) — invasivo nessa engine (10tons NX). O sistema de shader-alias que já
  temos (`hook_getshader`/`SHALIAS`) pode ser a porta de entrada, mas é trabalho + risco.
- **Decisão:** NÃO fazer agora. Guardado como alavanca extra **se a RAM ainda apertar** depois do
  ETC1 (opaca) + 4444/5551 (alpha). É a próxima fronteira se precisarmos raspar mais MB do alpha.

## 8. Riscos / lições já conhecidas (não repetir)
- **Seleção de textura da engine é o muro** (F1). Sem resolver isso, o pré-bake não chega na GPU.
- **ETC2 direto NÃO amostra no Utgard** (vira branco) — por isso opaco vai **ETC1**, não ETC2.
- **ETC1 não tem alpha** — alpha SEMPRE em RGBA (4444/8888). Classificar opaco/alpha certo
  (alpha-scan), senão sprite/decal fica com fundo preto.
- **RAM no .127 = pools da engine**, não textura — manter expectativa honesta (§6).
- **Encoder ETC1 é o nosso** (`etc1_encode.c`), NÃO o do Bully (bugado).
- **stb_image** precisa entrar no `texbake` (decoder que falta); manter o tool autossuficiente.
