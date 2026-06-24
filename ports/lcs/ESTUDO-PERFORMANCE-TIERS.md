# LCS Mali-450 — ESTUDO DE PERFORMANCE & TIERS (2026-06-23)

## 1. GARGALO CONFIRMADO (dados reais do device, gameplay)
| Métrica | Valor | Veredito |
|---|---|---|
| RAM total | 832 MB | fixo (Mali-450) |
| RAM livre | **64 MB** | 🔴 crítico |
| Swap em uso | **252 MB** (no SD, VFAT) | 🔴 thrashing |
| lcs RSS | 443 MB | + **209 MB swapped** |
| fps | ~18 | baixo |

**RAIZ do "trava dirigindo" (diagnóstico do porter, confirmado):** a velocidade do
veículo faz o streamer alocar textura/modelo novo → RAM cheia → **swap pro SD (lento)
→ FREEZE**. A pé é devagar (pouca alocação) → não trava. Parado → não trava.
NÃO é GPU/vsync/env-map (já testados). É **RAM → swap thrashing**.
Referência: MESMA parede do Bully (Mali-450 832MB; eviction sem ref-count é frágil).

## 2. ALAVANCAS DE RAM (já existem no código, mas DESLIGADAS no run atual)
| Alavanca | Flag | Default | O que faz |
|---|---|---|---|
| Mip-skip ETC | LCS_KEEPMIP (p/ reativar) | **ON** | pula mips lvl>0 (-33% tex) |
| Paginação/despejo LRU | `BULLY_PAGE=1` + `BULLY_PAGE_CAP_MB` | **OFF** (cap 220) | despeja textura LRU acima do orçamento |
| Texture-light (stub) | `LCS_TEX_LIGHT=1` + `LCS_TEX_LIGHT_CAP` | **OFF** | melhora flicker por remover textura, mas quebra pele/peds/carros; usar so diag |
| Half-res textura comprimida | `LCS_TEX_HALF=1` + `LCS_TEX_HALF_MIN` | **ON nos tiers perf** | usa mip 1 como nivel 0 em ETC grande (-75% por textura afetada) |
| Distância de LOD | `dvLodDistanceScale` | 0.75 | menos geometria de alto detalhe |
| Orçamento streamer | `LCS_STREAMER_MAX` / `LCS_RESOURCEDRAIN_MAX` | 80 / 6 | quanto pumpa/dreda por frame |

## 3. TIERS PROPOSTOS (a validar no device)
- **Tier 0 — QUALIDADE (atual):** sem despejo. RSS 443+209swap → trava dirigindo. ❌
- **Tier 1 — BALANCEADO:** `BULLY_PAGE=1 BULLY_PAGE_CAP_MB=180` + mip-skip. Meta: RAM
  sem swap, leve pop/recarga de textura ao virar. (TESTANDO AGORA)
- **Tier 2 — LISO:** `BULLY_PAGE=1 BULLY_PAGE_CAP_MB=140`, `LCS_TEX_HALF_MIN=512`,
  `dvLodDistanceScale=0.5`, draw 0.35, populacao/carros reduzidos, streamer por fase.
  Meta: dirigir sem trava preservando visual.
- **Tier 3 — AGRESSIVO SAFE:** `LCS_TEX_HALF_MIN=256`, draw 0.25, densidade menor,
  `LCS_NO_ENVMAP=1`, streamer ainda mais contido. Meta: reduzir flicker/stutter sem textura preta.

## 4. SE EVICTION NÃO BASTAR → implementar HALF-RES (downscale no upload)
O `my_glCompressedTexImage2D` (imports.c) já intercepta o upload e MEDE o ganho de
halving (g_half512/g_half1024). Falta APLICAR: decodificar ETC1 → reduzir 2x → re-encodar
(ou usar nível de mip menor como base). ~-50% a -75% de RAM de textura. É o que o Bully
fez (etc2_halve) e o que dá fôlego de verdade no 832MB.

## 5. OUTRAS FRENTES (secundárias, já sabidas)
- Swap SD thrasha; zram seria menos pior (OOM-mata limpo) MAS o certo é caber sem swap.
- Vsync (FBIO_WAITFORVSYNC) tirou parte do tearing (mantido).
- Flicker residual no carro/vidro = output-path/tile (menor, deixado).
- ✅ Chão preto = RESOLVIDO (alpha do framebuffer, build a1b9a7e+).

## PRÓXIMO: validar Tier 1 (eviction) → se RAM bound + dirige sem trava, baixar cap até o
ponto bom; se eviction falhar (engine sem ref-count, lição Bully) → implementar half-res.

## 5.1 DOWNSCALE DE RENDER — TESTADO E DESCARTADO (2026-06-23, A/B no device)
Medição pareada de fps em gameplay assentado (state=9, janela de 20s, mesmo método):

| Render | fps | Pixels |
|---|---|---|
| **1280x720 (baseline)** | **21** | 921.600 |
| **1152x648 (LCS_RENDER_SCALE=0.9)** | **20** | 746.496 (-19%) |

**VEREDITO: downscale NÃO ajuda.** Renderizar 19% menos pixels deu fps IGUAL (até 1
a menos, dentro do ruído). Se fosse GPU-bound, menos pixels = mais fps; não aconteceu →
**o gargalo NÃO é a GPU/fill-rate, é RAM/streaming** (consistente com a seção 1). Além
disso o `LCS_RENDER_SCALE` no Mali fbdev não faz mode-set do /dev/fb0 (continua 1280x720)
→ o engine renderiza 1152x648 num canto da tela. Conclusão prática: **downscale não
resolve o "trava dirigindo" e ainda encolhe a imagem → mantido 1280x720 como default.**
A flag `LCS_RENDER_SCALE` fica no código (egl_shim) mas OFF; o caminho para o freeze é
RAM (eviction/half-res, seções 2-4), não resolução de render.

## 6. PERFIL OFFLINE PREPARADO (NAO TESTADO AINDA)

Criado `run-gtasa-perf.sh` para testar depois, sem alterar o perfil visual bom atual.
Ele chama o fluxo jogavel normal (`run-playable.sh`), mas aplica disciplina inspirada no
GTASA NextOS/Vita:

- diagnostico off por padrao (`LCS_GLSTATS=0`, `LCS_INPUTDIAG=0`, `LCS_RENDERDIAG=0`);
- CPU governor em `performance`, igual ao launcher do GTASA;
- `PULSE_LATENCY_MSEC=120`, `MALLOC_CHECK_=0`, drivers GL/EGL explicitos;
- sombras/detail/PVS mantidos no perfil bom atual (`LCS_GFX_PREFS=1`, `LCS_SHADOWS_OFF=1`,
  `LCS_PVS_CLEAN=1`);
- paginacao LRU ligada (`BULLY_PAGE=1`) com cap por tier;
- streamer/drain/LOD reduzidos por tier para tentar evitar swap.

Tiers atuais depois dos testes em device:

| Tier | Uso | Flags principais |
|---|---|---|
| 1 / balanced | primeiro teste seguro | `BULLY_PAGE_CAP_MB=180`, `LCS_STREAMER_MAX=64`, `LCS_RESOURCEDRAIN_MAX=4`, `LCS_GFX_LOD_SCALE=0.65`, `LCS_TEX_HALF=1`, `LCS_TEX_HALF_MIN=1024` |
| 2 / smooth | base jogavel full-native | cap 140MB, streamer 48, drain 3, LOD 0.50, draw 0.35, `LCS_TEX_HALF_MIN=512`, `render_scale=native` |
| 3 / potato | agressivo safe | cap 110MB, streamer 32, drain 2, LOD 0.40, draw 0.25, `LCS_TEX_HALF_MIN=256`, `LCS_NO_ENVMAP=1`, `render_scale=native`, `TEX_LIGHT=off` |

`run-final.sh` tambem foi preparado para usar esse perfil quando chamado com
`LCS_PROFILE=gtasa-perf`. Nada foi executado/testado ainda por pedido do porter.

Nota de estudo apos comparar com reVC/GTASA: `BULLY_PAGE` foi herdado do Bully e registra melhor o
caminho `glTexImage2D`/ETC1-cacheado. O LCS Android sobe a maior parte do mundo por
`glCompressedTexImage2D` ETC, entao a alavanca mais direta de RAM e `LCS_TEX_HALF`, ja existente no
hook comprimido: quando chega o mip 1, ele reenvia esse mip como nivel 0 e descarta a base cheia.
Isso segue o principio do GTASA (`disable_mipmaps`/perfil mobile) e deve ser medido antes de mexer
em hacks de texture-light.

## 7. RESULTADO DOS TESTES AUTOMATICOS (2026-06-23)

- `LCS_RENDER_SCALE` foi descartado como default: a imagem precisa ficar cheia/native, e downscale
  nao atacou o gargalo real (RAM/streaming).
- Tier 2 full-native chegou a gameplay e andou com frame avancando bem. Medicao andando ficou por
  volta de 410-447MB RSS e 165-181MB de swap do processo.
- Tier 3 com `LCS_TEX_LIGHT=1` melhorou bastante flicker/stutter percebido, mas quebrou texturas de
  pessoas/carros. Captura ruim: `~/lcs-build/shot-tier3-tex128-20260623.png`.
- `LCS_TEX_LIGHT_MIN_DIM` foi adicionado para experimento seletivo, mas ainda nao e seguro para
  default jogavel.
- Perfil candidato atual: Tier 3 safe, sem `TEX_LIGHT`, com `NO_ENVMAP=1` e streamer/populacao
  agressivos. Captura boa: `~/lcs-build/shot-tier3-safe-noenvmap-20260623.png`.

Proxima linha de trabalho: manter o Tier 3 safe, testar ao volante/dirigindo e, se ainda houver
flicker, criar seletor real por asset/classe para `TEX_LIGHT` em vez de cap global por quantidade.

## 8. TIER 3+ FX-OFF VALIDADO (2026-06-23)

Pergunta: com o jogo ja quase liso, o que ainda da para desligar sem quebrar visual?

Resposta validada no device: cortar reflexos/FX leves e reduzir um pouco mais populacao/veiculos.
Nao mexer em resolucao e nao usar `TEX_LIGHT` global.

Novo Tier 3 default:

| Item | Valor |
|---|---|
| Resolucao | native/full 1280x720 |
| FX/reflexos | `LCS_SUNREFLECT_OFF=1`, `LCS_GFX_FX_OFF=1`, `LCS_NO_ENVMAP=1` |
| Populacao | `peds=3`, `cars=2`, `pop=0.20`, `carpop=0.20` |
| Distancias | `ped_dist=14`, `veh_dist=20`, draw 0.25 |
| Textura | `LCS_TEX_HALF=1`, `LCS_TEX_HALF_MIN=256`, `TEX_LIGHT=off` |
| Streamer | load `2/16 tex`, `4/16 buf`; gameplay `1/16 tex`, `1/16 buf` |

Medicao automatica:

| Perfil | VmRSS | VmSwap | Observacao |
|---|---:|---:|---|
| Tier 3 safe anterior | 453580 kB | 165732 kB | visual bom |
| Tier 3+ FX-off | 443804 kB | 166696 kB | visual bom, full-native |

Ganho direto medido: ~9-10 MB de RSS. FPS curto medido na cena parada: `300 frames / 15s`,
aprox. 20 FPS. A captura `~/lcs-build/shot-tier3-plus-fxoff-20260623.png` mostrou HUD,
personagem e textura OK.

Conclusao pratica:
- Seguro desligar: sun reflections, coronas reflections, motion blur streaks, rain streaks,
  light bloom, env-map.
- Seguro reduzir um pouco: peds/cars/densidade/distancia.
- Nao usar como default: `LCS_TEX_LIGHT`; ele melhora flicker por remover textura, mas quebra
  peds/carros/pessoas.
- Nao usar como default: `LCS_RENDER_SCALE`; nao melhorou FPS e encolhe a imagem.
