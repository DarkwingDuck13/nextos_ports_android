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
