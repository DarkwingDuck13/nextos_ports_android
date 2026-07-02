# GTA LCS — ESTUDO RAM/DESEMPENHO p/ 1GB (R36S-ready) + flicker de vidro/reflexo

**Missão (2026-07-02):** ganhar MUITA RAM e desempenho (sem flickers), fazer o LCS se adaptar
a device de 1GB como o Bully e o Dysmantle fazem, ajustar o streaming se preciso. Port será
levado ao R36S 1GB em breve. Estado bom preservado: commit `13ae6a6`, binário validado
`179c9afa` (backup `lcs-good-179c9afa.bak` no PC e no device).

## Baseline (2026-07-02, sessão real de gameplay no .79, 832MB)

| Métrica | Valor |
|---|---|
| VmRSS | **437 MB** |
| VmSwap | **190 MB** |
| VmHWM (pico) | **594 MB** |
| Device free | 74 MB (buff/cache 104) |
| swap2g usado | 236 MB |

⇒ Pico ~594MB de working set. Num R36S (~640MB utilizáveis) não cabe com folga; alvo:
**pico < 420MB** + streaming que não degrada (o RAM-wall da s12: streamer carrega mais rápido
que libera → swap SD → trava dirigindo).

Sabido da s12 (ESTUDO-PERFORMANCE-TIERS.md): texturas vivas são só ~65MB — o grosso da RAM NÃO
é textura GL; é heap do engine (gFixHeapSize=0 na época?, buffers, CdStream, pools, WADs).
⇒ Mapear o heap real antes de cortar às cegas (LCS_MEMDIAG + bully_resource_report + smaps).

## Flicker vidro/reflexo (estudo paralelo)

Sintoma (usuário): reflexo/vidro pisca QUADRADOS quando anda/dirige; carro PARADO não pisca.
Estado atual: `LCS_NO_ENVMAP=1` default (env-map off) melhorou ("parece até que corrigiu").

Infra na engine: `gpModelEnvMap` (RTT do carro) + `gpStaticEnvMap` + `gbEnvmapReady`;
vidro: `CGlass::RenderReflectionPolys` + knobs `dvGlassEnvMap*`; `GetDefaultReflectionsEnabled`
@0x54f82c; `DisableMatFx(material)` @0x4c8328 (desliga matfx POR MATERIAL — cirúrgico).

**Hipótese central (liga flicker ao streaming):** parado=sem uploads; andando=CStreaming sobe
texturas ETC (`glCompressedTexImage2D`) NO MEIO do frame → tiler Utgard resolve parcial →
blocos/tiles piscando. Testes: (T1) throttle `LCS_GFX_TEX_CREATE_PER_FRAME` baixo; (T2) drenar
uploads só logo APÓS o swap (janela segura); (T3) A/B parado vs andando com contagem de uploads
por frame no log. Se confirmar, o fix do streaming resolve o flicker de graça (e talvez até o
env-map possa voltar).

## Referências em estudo (agentes varrendo; consolidar aqui)
- Bully (MESMA libGame): streaming v11, perfil 1GB do bully2/R36S ("rodando bem demais" em 1GB).
- Dysmantle: DYS_PAGE (paging id-keyed, cap80/floor48, só-frias min_age+MIN_KB=24), launcher
  RAM-aware (MemTotal<1100→TEXSCALE 3.0), lição "piscada preta = floor alto".
- GTASA Vita (512MB!) + nosso GTASA NextOS: knobs de CStreaming/população/draw distance.

## Knobs já existentes no shim (herança, validar um a um)
`LCS_GFX_STREAM_MEM_MB`, `LCS_GFX_TEX_CREATE_PER_FRAME`, `LCS_GFX_TEX_DESTROY_PER_FRAME`,
`LCS_GFX_BUF_CREATE/DESTROY_PER_FRAME`, `LCS_GFX_MAX_CARS/MAX_PEDS/POP_MULT/CAR_MULT`,
`LCS_GFX_DRAW_DISTANCE/LOD_SCALE/VEHICLE_DIST/PED_DIST`, `LCS_GFX_FX_OFF`, `LCS_RAMGUARD`,
`dvStreamerAllowDestroy*` (eviction não dispara — investigar), `gFixHeapSize`,
`ImGonnaUseStreamingMemory` @0x4aebf0 (ptr [7f9000+2136]).

## Mapa anon da sessão real (o ouro)
| Mapeamento | Tamanho | RSS | Swap | Suspeita |
|---|---|---|---|---|
| 7f243c9000-7f34200000 | **~254MB** | **258MB** | 0 | **heap fixo do engine (gFixHeapSize/gMainHeap?) — JACKPOT se encolher** |
| 7f402cc000-7f44200000 | 63MB | 4MB | 58MB | arena secundária (quase toda em swap = fria) |
| 7f20000000-7f21e00000 | 30MB | 30MB | 0 | ? |
| [heap] malloc | — | 72MB | 62MB | glibc heap (MALLOC_ARENA_MAX/TRIM ajudam) |
| [stack] thread 340043 | — | 12MB | 0 | stack-shrink do Dysmantle aplica |

## 🐛 Bugs reportados pelo usuário (fila de tarefas)
1. **Câmera girando sobre o personagem → tudo pisca em certo ângulo** (descoberto após pegar o
   terno na casa, missão do terno). Log da sessão preservado em `run-sessao-nextos-terno.log`
   (sem diag — precisa repro com LCS_SUNREFLECT_DIAG/ALPHA_DIAG; suspeitos: sun-reflect/corona
   view-dependent, alpha do compositor em ângulo específico, envmap estático). PENDENTE.
2. Flicker vidro/reflexo em movimento (estudo acima — hipótese uploads mid-frame).
3. Câmera da cena das escadas "invertida/atrás do carro" (checar se persiste no fluxo novo).

## Plano (preencher pós-agentes)
1. Mapear consumo real (smaps por região + resource report) — saber ONDE estão os 594MB.
2. Portar do Bully o mecanismo de perfil por MemTotal (launcher) + o que couber do streaming v11.
3. Cortes de engine estilo Vita/SA (streaming budget, população, draw distance) num perfil
   `LCS_MEM_PROFILE=1gb` — SEM tocar o perfil atual do 832MB até validar.
4. Flicker: T1-T3 acima.
5. Testes headless A/B (RSS pico, fps médio do heartbeat, uploads/frame) antes de qualquer
   validação do usuário.
