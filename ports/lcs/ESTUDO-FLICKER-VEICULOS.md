# GTA LCS — ESTUDO: flickers em veículos + rodar em 1GB (2026-07-03)

**Missão:** estudar o gtasa_vita (cap do mapa, sombras, desempenho), comparar com nosso código,
LISTAR as possíveis causas dos flickers (principalmente ao PEGAR veículo e DIRIGINDO na cidade),
e definir o que falta pra rodar em 1GB. Estado-base: binário validado 857d1ab9 (respawn ok,
vidros ok, minimapa ok — "melhor até agora").

## O que já sabemos do nosso pipeline (auditoria)
- Swap (imports.c my_eglSwapBuffers): bake-UI → shot(gated) → **ALPHA-fix** (clear alpha=1 todo
  frame; desliga SCISSOR+DITHER) → **vsync fbdev** (FBIO_WAITFORVSYNC ioctl antes do swap;
  comentário histórico: tearing "pior dirigindo/rápido") → swap real.
- Sombras: já OFF (LCS_SHADOWS_OFF=1) — descartadas como fonte.
- Envmap: OFF via gbEnvmapReady=0 por frame (validado como melhora no vidro em dado momento).
- Uploads: mundo sobe por glCompressedTexImage2D (ETC nativo do jogo) direto na render thread,
  SEM janela segura vs o tiler.

## 🎯 LISTA FINAL DE CAUSAS (ranqueada; a pé não pisca → causas de VEÍCULO no topo)
1. **★★★ Passe do envmap do veículo (GenerateEnvironmentMap → RTT 1024×1024 por frame)** —
   FBO-switch mid-frame no tiler Utgard = tiles piscando SÓ com veículo; roda MESMO com nosso
   NO_ENVMAP (que só corta o "ready"). Fix candidato: no-op no passe (gated). ← testável em
   1 mudança; também deve DEVOLVER FPS em veículo (1024×1024 de cena a menos por frame!).
2. **★★ Material do carro amostrando envmap semi-morto** (consequência do NO_ENVMAP atual):
   junto com o fix 1, decidir: passthru sem matfx (DisableMatFx nos materiais de veículo) OU
   religar envmap completo OU (futuro) specdot procedural estilo Vita/PS2.
3. **★ TEARING por vblank perdido** (fbdev WAITFORVSYNC) — em veículo o frame pesa mais →
   perde vblank → tear; explica "pior em certos lugares" (cenário pesado). A/B: LCS_NO_VSYNC=1
   e/ou vblank pós-swap.
4. **Faróis/coronas/luzes view-dependent** (noite) — A/B SUNREFLECT_OFF.
5. Uploads mid-frame (SECUNDÁRIO agora — a pé também haveria) — throttle de destroy já ficou.
6. Alpha do compositor (flash PRETO vs colorido distingue).
7. PVS pop-in em velocidade (não é flicker; separar).

## 🪟 VIDROS bugando (quadradinhos/transparência instável) — causas candidatas
1. **Sampler de envmap indefinido no shader do vidro** (mesmo mecanismo do item 2 acima):
   vidro usa dvGlassEnvMap* (fresnel/reflectivity) — com o envmap congelado (gbEnvmapReady=0),
   o vidro amostra textura nunca-atualizada/lixo → quadrados/manchas que mudam com a câmera.
2. **Ordem/sorting do passe alpha**: vidro é desenhado no passe alpha (cWorldStream::RenderAlpha
   + CGlass); se a ordem varia por frame (lista re-ordenada), a transparência "fervilha".
3. **Tile artifacts do Utgard**: quadrados = tiles; o passe do vidro (blend) intercalado com
   uploads mid-frame corrompe tiles translúcidos primeiro (blend lê o dst).
4. **Precision/half-float** no fragmento do vidro (fresnel em mediump no Utgard) → banding/
   quadriculado que pisca com o ângulo.
5. **Dither OFF** (nosso alpha-fix desliga GL_DITHER e não religa): em gradientes translúcidos
   o dither faz falta → banding em blocos. A/B fácil: religar dither após o clear do alpha.
6. **Blend com DST_ALPHA**: se o shader/blend do vidro usa o alpha do framebuffer como fator,
   nosso alpha-fix (clear alpha=1 no fim do frame) interage — verificar blendfunc do vidro.
→ Ver o que o GTASA Vita fez para vidro/janela (o agente está varrendo: skygfx/matfx patches;
  GTASA mobile tinha bug análogo de vidro em Adreno/Mali resolvido por patch de shader?).

## Protocolo de teste com o usuário (UMA variável por vez, TV)
- T1: LCS_NO_VSYNC=1 (tira a espera de vblank) → flicker muda? (se piorar tear = 1 confirmada)
- T2: NO_ENVMAP=0 (envmap vivo de novo) → flicker do carro muda? (2)
- T3: DisableMatFx nos materiais de veículo (patch gated novo) → (2)
- T4: LCS_SUNREFLECT_OFF=1 → (4)
- T5: throttle de CREATE só-em-carro? NÃO (lição peds); observar contadores upload×flicker (3).

## 🔑 DADO NOVO DO USUÁRIO (reordena tudo): a PÉ NUNCA pisca; só em CARRO/MOTO, e pior em
## certos lugares → causa específica de veículo, não streaming genérico.

## 🎯 CAUSA Nº1 LOCALIZADA (disasm nosso): o passe do envmap do veículo
`RenderEffects()` @0x522f0c chama **`CRenderer::GenerateEnvironmentMap()`** (plt 7cfa60) por
frame + `C_RenderTexture::SetTarget/Resolve/RenderPassThrough` → **renderiza a cena num RTT
1024×1024** (flagrado no [viewport] diag, caller `Display::SetRenderTarget` @0x59d598) = o
sphere-map do reflexo do carro (mesmo mecanismo do FLAG_SPHERE_XFORM do GTASA: "renders the
scene as a sphere map for vehicle reflections"). No Utgard, FBO-switch mid-frame + resolve
parcial = tiles piscando — SÓ quando há veículo (bate com o sintoma!).
⚠️ O nosso LCS_NO_ENVMAP (gbEnvmapReady=0) NÃO desliga esse passe (o RTT continua rodando) e
ainda deixa o material amostrando um mapa semi-morto → dupla fonte de flicker.
**Fix candidato cirúrgico (1 mudança, gated): no-op em `CRenderer::GenerateEnvironmentMap`**
(LCS_NO_ENVMAP_PASS=1) — mata o RTT inteiro; carro fica sem reflexo dinâmico (visual tipo
"Low/PS2"). Upgrade futuro estilo Vita: trocar o path do reflexo por specdot procedural em
view-space (gfx_patch.c:899-913/1051-1061 como referência).

## GTASA Vita (relatório do agente — resumo)
- **Cap do mapa/streaming: NÃO EXISTE** — o Vita roda o mundo cheio (fog/farclip 100% do
  timecyc.dat). Precedente: cap de mapa não é necessário nem no Vita 512MB.
- **Sombras**: sem toggle de desligar; shaders Cg de resolve/blur substituídos + NOP pontual
  p/ sombra clássica do CJ. (Nós já rodamos SHADOWS_OFF.)
- **Shaders/GPU**: precision lowering massivo (half/mediump); `disable_tex_bias=1` default;
  branches `isMaliChip` (pow 9 vs 10 no spec — anti-artefato FP16!), `isSlowGPU` (sem bias),
  `unk_08` (chip<=1: PULA spec+fog inteiros — caminho low-end forçável). MSAA/resolution são
  os knobs GPU diretos.
- **Reflexo/matfx veículo**: 3 modos (ENVMAP lerp / SPHERE_XFORM = cena→sphere map por frame /
  SPHERE_ENVMAP); skygfx PS2 troca ENVMAP por **specdot procedural** (sem RT!). É a rota de
  qualidade se quisermos reflexo bonito sem RTT.
- **RAM 512MB**: heap 192MB + **`GetDeviceType` reporta <256MB → engine entra em modo
  low-memory nativo** (pools/streaming menores sozinho!). disable_detail_textures=1.

## 💡 Aplicações-chave pro LCS 1GB (do Vita)
1. **GetDeviceType/low-memory nativo**: descobrir o análogo no LCS (o engine Leeds lê RAM do
   device via JNI? setDeviceInfo/OS hints — investigar; se existir branch <256MB, é o modo
   1GB "de graça", igual Bully fazia com device_ram_report).
2. `unk_08`-style: se o gerador de shader do LCS tiver caminho low-end (sem spec/fog), forçar
   no perfil 1GB.
3. Detail textures: já pinamos dv_renderDetailedDistance=0 (equivalente do gNoDetailTextures).

## Rodar em 1GB (consolidar)
- Base já pronta: escada MemTotal → MEMLOW (<700MB) com perfil visual aceitável (shotG);
  higiene opt-in; mundo=254MB é o alvo (budget/pools/população); destroy-throttle validado.
- Falta: medir MEMLOW em sessão LONGA; half-res ETC (LCS_TEX_HALF) como opção 1GB; paging
  (BULLY_PAGE modernizado) só se textura virar pressão; swap/zram do R36S (usar o do sistema).

---
# 📋 SESSÃO 2026-07-03 (tarde) — CASO DO MODO-DIREÇÃO: diagnóstico FECHADO, fix pendente

## O que foi PROVADO (cadeia completa)
1. Usuário: **rádio OFF no veículo = ZERO flicker** ao entrar/sair (prova de ouro).
2. Spike-diag: TODA entrada de veículo = frame de 2.1-2.6s, `tex+0 io+0`.
3. Stall-sampler (kernel stack): o main está DORMINDO (`nanosleep`) durante o tranco.
4. Sleep-diag: 40/40 sleeps vêm do caller `0x58eb00` = **`lglRenderQueue::
   flushCommandsAndResources`** — flush SÍNCRONO que trava o main até a fila de
   comandos+recursos drenar (loop: flushCommands→sleep(1ms); depois flushResources→sleep).
   No SD lento = 1-3s. A MESMA função aparece nos backtraces dos crashes sig11 do Mali.
5. Trocar estação de rádio = novo flush = "flicker atrás de flicker".

## O que foi tentado e o resultado
- ✅ #1 GenerateEnvironmentMap NO-OP (RTT 1024×1024) — VALIDADO ("coisas fixas", sem regressão).
- ❌ #2 RslMatFXMaterialSetupEnvMap no-op — crasha o load de material (revertida, opt-in).
- ❌ Vsync off — sem efeito no tranco.
- ❌ Create-throttle 8/frame — sem efeito no tranco (ficou, inofensivo).
- ❌ PlayerInCar no-op — rádio tocava mesmo assim (não é o gate; opt-in).
- ⚠️ Music PREWARM (aquece a cadeia FAT do data_music.wad no boot) — implementado, ficou
  default ON, mas NÃO bastou (o custo não era só a FAT chain).
- ⚠️ FLUSH BOUNDED (reimplementação do flushCommandsAndResources com deadline 120ms) —
  implementado MAS a validação headless mostrou spikes iguais (2.0-2.7s). **Verificar na
  próxima: o hook instalou? (procurar "[fix] flushCommandsAndResources BOUNDED" no log);
  o "[flush] bounded: cortado" nunca apareceu → ou não instalou, ou o wait está em OUTRO
  caller além do flush (rodar o sleep-diag de novo com o bound ativo).**
- ⚠️ ALPHA: provado por A/B que o clear-por-frame (fix do chão) causa a "matriz preta
  piscando" (câmera da morte); surface alpha=0 é mentira no mali fbdev; alpha-MASK total
  quebra a UI (tela preta com som). → v2 pendente (abaixo).

## ▶️ PRÓXIMAS ETAPAS (domingo)
1. **Fechar o flush-bounded**: conferir se o hook instalou (log "[fix] ... BOUNDED"); se
   sim e o spike persiste → rodar sleep-diag de novo (o caller pode ter mudado / haver um
   segundo wait); considerar hookar TAMBÉM o caller do flush (quem pede o flush no
   enter-car) e torná-lo assíncrono.
2. **Alpha v2 (matriz preta)**: clear de alpha a cada N frames (ex. 15) em vez de todo
   frame — 15× menos janela de flicker, mantém o chão limpo. Simples e seguro.
3. Rádio: se o flush-bounded fechar, re-testar troca de estação; se sobrar custo, avaliar
   por o load da estação numa thread (async) ou cache das cabeças de faixa.
4. Mapa/céu piscando em partes (relato novo) — colher print/observação com o [spike] ao lado.
5. Retomar a lista numerada (RAM/1GB): #6 low-memory nativo, #7 MEMLOW longa, #11 budget.
6. Becos do dia (NÃO repetir): alpha-mask total (UI preta), matfx-setup no-op (crash),
   PlayerInCar no-op (inócuo), prewarm sozinho (insuficiente).

---
# 📋 SESSÃO 2026-07-03 (madrugada) — CAUSA-RAIZ PROVADA NO KERNEL + comparação GTASA fechada

## Comparação GTASA×LCS (fechada, código conferido)
- **GTASA Vita** (`gtavc-build/refs/gtasa_vita`, TheOfficialFloW): áudio é só camada de
  REDIRECIONAMENTO (openal_patch.c ~150 símbolos, mpg123_patch.c ~90). A libGTASA.so roda o
  rádio numa THREAD DE ÁUDIO desacoplada (buffer-queue OpenAL `alSourceQueueBuffers` +
  `mpg123_open_feed/feed`). Render nunca bloqueia → sem flicker.
- **Nosso LCS** (Leeds "lgl" engine): `libGame.so` importa os MESMÍSSIMOS símbolos
  (`alSourceQueueBuffers`, `mpg123_open_feed/feed`) e precisa (NEEDED) de libopenal.so +
  libVendor_mpg123.so — MESMA arquitetura. E já usamos áudio DO SISTEMA: `main.c:142-146`
  dlopen GLOBAL de `libopenal.so.1`/`libmpg123.so.0`/SDL2/GLES — o "modo leve tipo GTASA" já
  está aplicado (as libs Android em lcs-src/lib NÃO vão pro runtime). Então "criar lib pra
  ficar leve" = JÁ FEITO; não é aí que mora o flicker.

## Prova de kernel (device .79, binário 3f58eed1, sampler de todas as threads ao vivo)
Ferramentas confirmadas instaladas no run: `[fix] flushCommandsAndResources BOUNDED (120ms)`,
`[diag] lglSleep hookado (mainsleep)`, `[stall] sampler ativo`, prewarm 458MB.
- Flush-bounded FUNCIONA: `[flush] bounded: cortado em 154ms (pend=2575/3044)` — o flush que
  travava 2–2.6s agora é capado em ~155ms. MAS só dispara ~1×/sessão → NÃO é mais a fonte
  principal do flicker do carro.
- Assinatura do tranco do carro MUDOU: main preso em **`futex_wait`** (sc=98), NÃO no
  nanosleep do flush. Sampler de threads: durante o freeze há threads em estado **D** em
  `sleep_on_page_killable` / `sleep_on_buffer` / `loop_make_request` (I/O de página do
  armazenamento), e o **próprio main entra em D** (lê o WAD) além de esperar futex.
- Storage: `data_music.wad` = **480MB no cartão SD vfat** (`/dev/mmcblk1p3`). Swap de 2GB
  (`swap2g.img`) **no MESMO SD**, 361MB em uso. RAM 654/832MB.
- **CAUSA-RAIZ**: entrar no carro / trocar estação → leitura SÍNCRONA do offset da estação no
  WAD de 480MB no SD lento, sob lock, com RAM estourada (page-fault + swap no mesmo SD) →
  main bloqueia no futex ~400ms–2.7s = flicker. Rádio OFF = sem leitura = sem flicker (prova
  de ouro confirmada no nível de I/O). É EXATAMENTE o que o GTASA evita com a thread de áudio.

## Respawn "geometria tremendo / coisas aparecendo" (relato novo, mesma doença)
Spikes medidos: `dur=5603ms tex+7 io+6`, `4095ms`, `2778ms` — reload do mundo puxando
geometria/textura do SD por 4–5s sob pressão de RAM. Envmap RTT JÁ está OFF
(`GenerateEnvironmentMap NO-OP` ativo) → NÃO é o envmap. É o streaming lento do SD.
LCS_STREAMER_MAX=80→3000 (commit 129a18a) resolveu a geometria-lixo EM MOVIMENTO (streamer
termina, `finished=1`), mas o RELOAD de respawn ainda estoura em segundos.

## 🎯 FIX DE VERDADE (precisa rebuild — a leitura do WAD passa pelo NOSSO wadfs/asset_archive.c)
1. **Rádio ASSÍNCRONO estilo GTASA**: a leitura do `data_music.wad` é servida pelo nosso
   código (imports.c wadfs / asset_archive.c). Fazer o read pesado da estação numa thread de
   fundo + read-ahead → o main (e o lock) não esperam o SD. Alvo nº1 do flicker do carro.
2. Suavizar reload de respawn: mesmo mecanismo (prefetch/async do stream de mundo) OU
   throttle mais alto de resource-drain só na janela de respawn.
3. Pressão de RAM é o multiplicador de tudo (swap no SD) — perfil MEMLOW longo pendente.
- ⚠️ Não é lib de áudio (já usamos a do sistema); não é envmap (já OFF); não é o flush
  (já capado). O que falta é tirar o SD do caminho síncrono do main.

## ⚠️⚠️ CORREÇÃO (mesma sessão, teste decisivo de I/O) — **NÃO É O SD**
Usuário garantiu "não é SD" e o argumento dele é definitivo: **o GTASA lê o rádio do MESMO
cartão SD e não pisca** → a velocidade do SD não pode ser o bloqueio. Medição confirmou:
- Watch dos contadores `/sys/block/mmcblk1/stat` (setores lidos) + `/proc/vmstat` (pswpin/out)
  DURANTE várias entradas de carro: no freeze, **SD_rd = +0 setores** na quase totalidade das
  amostras e **swpin = 0 o tempo TODO** (zero swap-in). O main dorme em `clock_nanosleep`
  (sc=115) / `futex` (sc=98) **sem ler nada do disco**.
- ⇒ O freeze é **CPU/LOCK, não I/O**. O main fica num LOOP DE ESPERA (poll + nanosleep 1ms)
  enquanto a estação do rádio é PREPARADA/decodificada numa outra thread (tid quente 247227,
  R w=0 = rodando em userspace; provável mpg123 decode / fill de buffer). Rádio OFF = ninguém
  pede a estação = main não espera = sem flicker. Mesma essência da comparação com o GTASA,
  mas o gargalo é **preparação síncrona da estação bloqueando o main**, NÃO leitura de SD.
- Correção de rota: **parar de perseguir SD/prefetch/read-ahead**. O alvo é **desacoplar a
  preparação da estação do main** (estilo thread de áudio do GTASA) OU **bound no loop de
  espera** (o main segue renderizando; o áudio entra 1 frame depois, sem freeze).

## ▶️ PRÓXIMO PASSO (quando retomar) — achar o loop de espera exato
O hook `my_lglSleep` (mainsleep) NÃO pegou esse sleep → é um `nanosleep`/`usleep` de OUTRA
primitiva (libc direto pelo libGame OU o `tramp_lglSleep` do próprio flush-bounded, que chama
o lglSleep ORIGINAL sem re-logar). Plano: **hookar `clock_nanosleep`/`nanosleep`/`usleep`
(libc, via imports.c) logando caller_off quando o main dorme em state 9** → identifica a
função do wait-loop (do rádio/estação) → aí sim bound/async cirúrgico nela. 1 build + 1
entrada de carro fecha. Reforço: **NÃO é SD; NÃO gastar tempo com prefetch de WAD.**
