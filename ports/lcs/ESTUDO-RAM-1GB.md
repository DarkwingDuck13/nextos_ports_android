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

## Consolidação dos 3 estudos (agentes, 2026-07-02)
- **Bully (mesma família)**: paginação LRU v11 JÁ está no lcs/imports.c (BULLY_PAGE, off);
  perfil low-mem por símbolo JÁ existe (bloco LCS_GFX_MEMLOW jni_shim 197-253); becos:
  TEX_BUDGET_HOOK falha (getters fixos), trilinear <1.2GB afunda RAM, evict nativo agressivo
  crasha, mlock OOM. Thread-pin de streaming = ganho real. `GetTotalGraphicsMemoryOfSystem`
  hardcoded 512MB no Bully (engine nunca despeja sozinho).
- **Dysmantle**: DYS_PAGE id-keyed (name-keyed dá textura errada), floor MemAvailable + guarda
  zram + cold-guard min_age + cooldown (senão piscada preta), stack-shrink madvise, fadvise no
  swap I/O, malloc env, launcher por MemTotal, ES-off (~76MB). LIÇÃO-MESTRA: **RAM é dominada
  pelo pool de objetos do MUNDO, não por textura** — cortar mundo > comprimir textura.
- **GTASA Vita/NextOS**: Vita dá heap grande e deixa o CStreaming decidir; gNoDetailTextures
  default; RQCaps->isSlowGPU (NÃO existe no LCS — Leeds mais velho). Nosso arsenal real já
  está no lcs shim (gStreamingMemSize, LOD/pop/dist, TEX_HALF, ETC1 bake). Downscale de render
  = inútil comprovado (RAM-bound, não GPU).

## ✅ APLICADO (commits 748fc2a + 1f54643, 2026-07-02)
1. **Higiene RAM** (lcs_ram_hygiene 900f): malloc_trim + stack-shrink madvise. + MALLOC_ARENA_MAX=2/
   TRIM/MMAP no launcher + swappiness 60. A/B headless: **pico 594→558MB, RSS 437→373MB**.
2. **Escada por MemTotal no run30.sh**: <700MB → MEMLOW completo (R36S); 700-1200 → só
   LCS_STREAM_PHASE (throttle de upload por fase, sem cortes visuais); ≥1200 → nada.
3. **ES parado durante o jogo** (desenhava por cima do fbdev — visto em screenshot; era o
   "personagem invisível/faixa no rodapé": fb sujo do ES + jogo desenhando só a área ativa).
   Usuário confirmou: "bugs de câmera/tela resolvidos".
4. **Perf**: SDL_Delay(16) fixo REMOVIDO do loop (capava <30fps em cena pesada); thread-pin
   SÓ das threads do engine → cores 0/1/3 (main/render livre — scheduler usa o core 2 vago).
   ⚠️ BECO NOVO (bisect 2026-07-02): **pinar o MAIN no core 2 crasha o Mali** (sig11 libMali no
   swap na fase de load ~f=500 e pós-respawn): as workers do driver nascem do main e HERDAM a
   afinidade → driver sufoca num core. Baseline leve: 30.0fps cravado.
5. **Câmera pós-cena presa (bug "começa atrás do carro/parede")**: a engine só restaura a câmera
   (modo 15/18 → 4) no primeiro INPUT; parado ela fica onde a cena largou (shotB: 80% preto).
   Fix: [camfix] armado em cutscene/flyby, dispara restore completo (finish+jumpcut+behind)
   quando modo 15/18 + ped imóvel ~3s sem cutscene ativa. LCS_CAM_BEHIND_AFTER_CUTSCENE=0 off.
6. **Respawn/restart (Wasted) → crash Mali** descoberto na run A (autowalk morreu): fila item
   novo — repro = morrer no gameplay; stack igual ao do pin?? re-testar SEM pin (pode ter sido
   o mesmo bug do pin — run A rodou binário com pin de main!).
5. Diags permanentes: [bigalloc] (nenhuma alocação >20MB única → arena de 254MB = milhares de
   allocs pequenas do mundo em arena glibc ✓ tese Dysmantle), [viewport]/[scissor].

## Mapa do frontend p/ automação (MENUDIAG screen/item)
- screen=2: menu principal (item 0 default = Start Game; A confirma).
- screen=14: submenu Start Game — **com SAVE presente a seleção default = item 1 (Load Game!)**;
  item 0 = New Game. Menu tem WRAP (UP no topo vai pro fim — UP×2 cego re-cai no Load).
  rkgate kind: ShowGate=New Game / ShowGateBeforeLoad=Load.
- screen=69: pós-gate (loading). Autopilot v4 navega lendo screen/item do [menu] loop.
- ⚠️ Load Game: às vezes funciona, às vezes morre em "Could not open Models/Generic/WHEELS.TXD"
  (arquivo vive dentro do gta3.img, não como entrada do WAD — fila).

## Sessão noturna 2026-07-02 (madrugada) — resultados
- **Camfix pós-cena (v5, commit d642f0d)**: rede armada por cenas 15/17/18, dispara restore só
  com ped imóvel 3s; dormant quando a engine restaura sozinha (intermitência confirmada: runs
  J/K restauraram nativas; shotB tinha ficado presa). shotG: início pós-escadas PERFEITO.
- **Perfil MEMLOW (1GB) exercitado no 832**: prefs aplicadas ([gfx] pref ...=0), visual
  aceitável (shotG: fog mais próximo, mundo limpo). HWM em runs curtas é dominado pelo load
  (586 vs 558 fase1 = ruído) — medição definitiva só em sessão longa/R36S.
- **runNG.sh**: wrapper de teste que ESTACIONA o save do usuário (backup triplo: .bak-seguro +
  PC scratchpad + ~/lcs-build/save-backup-nextos-20260702) e restaura por trap — New Game
  determinístico p/ automação. Save do usuário intacto e restaurado ✓.
- Lição harness: "run30 done" vai pro nohup.out (não run.log) — vigias devem grepar
  "Mali teardown".

## Fila restante (prioridade do usuário: DESEMPENHO > RAM > vidro; depois R36S 1GB)
1. **Vidro (VALIDAR COM USUÁRIO ao acordar)**: binário atual tem CGlass::RenderReflectionPolys
   NO-OP default — piscada do vidro dia/noite sumiu? (LCS_GLASS_REFLECT=1 religa). Se persistir:
   T2 = drenar uploads pós-swap; suspeito 3 = matfx env do vidro (DisableMatFx cirúrgico).
2. fps na CIDADE dirigindo (usuário sente; headless dirigir é arriscado — autowalk morreu e
   expôs o crash de respawn... que era o pin; re-testar morte/respawn no binário atual).
3. ✅ Load Game: RESOLVIDO pelo fix do pin — a única morte foi em binário com pin de main (crash Mali); pós-fix, todos os loads OK. "WHEELS.TXD" não existe no WAD (probe 9 nomes) = mensagem benigna de era-console (o celular também loga). Observar em sessão longa.
4. Bug câmera girando/tudo pisca em ângulo (provável morto com ES-fix — confirmar com usuário).
5. BULLY_PAGE modernizar (floor/cold-guard Dysmantle) SE textura virar pressão real (hoje não é).
6. R36S 1GB: escada liga MEMLOW sozinha; tunar caps/validar lá. Perfil "igual GTASA" =
   run-gtasa-perf.sh tiers já existem.
