# LCS Mali-450 — ESTUDO DE PERFORMANCE & TIERS (2026-06-23)

## 1. GARGALO CONFIRMADO (dados reais do device .88, gameplay)
| Métrica | Valor | Veredito |
|---|---|---|
| RAM total | 832 MB | fixo (Mali-450) |
| RAM livre | **64 MB** | 🔴 crítico |
| Swap em uso | **252 MB** (no SD, VFAT) | 🔴 thrashing |
| lcs RSS | 443 MB | + **209 MB swapped** |
| fps | ~18 | baixo |

**RAIZ do "trava dirigindo" (diagnóstico do NextOS, confirmado):** a velocidade do
veículo faz o streamer alocar textura/modelo novo → RAM cheia → **swap pro SD (lento)
→ FREEZE**. A pé é devagar (pouca alocação) → não trava. Parado → não trava.
NÃO é GPU/vsync/env-map (já testados). É **RAM → swap thrashing**.
Referência: MESMA parede do Bully (Mali-450 832MB; eviction sem ref-count é frágil).

## 2. ALAVANCAS DE RAM (já existem no código, mas DESLIGADAS no run atual)
| Alavanca | Flag | Default | O que faz |
|---|---|---|---|
| Mip-skip ETC | LCS_KEEPMIP (p/ reativar) | **ON** | pula mips lvl>0 (-33% tex) |
| Paginação/despejo LRU | `BULLY_PAGE=1` + `BULLY_PAGE_CAP_MB` | **OFF** (cap 220) | despeja textura LRU acima do orçamento |
| Texture-light (stub) | `LCS_TEX_LIGHT=1` + `LCS_TEX_LIGHT_CAP` | **OFF** | troca textura por 4x4 acima do cap (=textura sumida) |
| Half-res textura | — | **NÃO IMPLEMENTADO** | (Bully tinha bake meia-res; LCS só MEDE o ganho) |
| Distância de LOD | `dvLodDistanceScale` | 0.75 | menos geometria de alto detalhe |
| Orçamento streamer | `LCS_STREAMER_MAX` / `LCS_RESOURCEDRAIN_MAX` | 80 / 6 | quanto pumpa/dreda por frame |

## 3. TIERS PROPOSTOS (a validar no device)
- **Tier 0 — QUALIDADE (atual):** sem despejo. RSS 443+209swap → trava dirigindo. ❌
- **Tier 1 — BALANCEADO:** `BULLY_PAGE=1 BULLY_PAGE_CAP_MB=180` + mip-skip. Meta: RAM
  sem swap, leve pop/recarga de textura ao virar. (TESTANDO AGORA)
- **Tier 2 — LISO:** `BULLY_PAGE=1 BULLY_PAGE_CAP_MB=140` + `LCS_TEX_LIGHT` cap +
  `dvLodDistanceScale=0.5` + `RESOURCEDRAIN_MAX` menor (espalha upload). Meta: dirigir
  sem trava, visual mais pobre (texturas menores/sumidas longe).
- **Tier 3 — POTATO:** tudo agressivo + half-res (se implementar). Meta: liso garantido.

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

Tiers preparados:

| Tier | Uso | Flags principais |
|---|---|---|
| 1 / balanced | primeiro teste seguro | `BULLY_PAGE_CAP_MB=180`, `LCS_STREAMER_MAX=64`, `LCS_RESOURCEDRAIN_MAX=4`, `LCS_GFX_LOD_SCALE=0.65` |
| 2 / smooth | se ainda swapar/travar | cap 140MB, streamer 48, drain 3, LOD 0.50, `LCS_RENDER_SCALE=0.90` |
| 3 / potato | teste agressivo de prova | cap 110MB, streamer 32, drain 2, LOD 0.40, render 0.75, `LCS_TEX_LIGHT=1` |

`run-final.sh` tambem foi preparado para usar esse perfil quando chamado com
`LCS_PROFILE=gtasa-perf`. Nada foi executado/testado ainda por pedido do NextOS.
