# Bully R36S (GLES3/ETC2) — CAUSA-RAIZ do "personagens/mundo BRANCOS" achada + corrigida — 2026-06-21 (noite)

## ⭐ ESTADO FINAL 2026-06-22 (ponto de partida p/ amanha) — LER PRIMEIRO
- ✅ **Mundo branco no gameplay: RESOLVIDO + confirmado** (fix g_path_fresh; FBO INCOMPLETO=0). Commit 8f9f038 (master).
- ✅ **Meia-resolucao automatica p/ 1GB** funcionando (etc2_halve.c; GPU 129->27MB). Cache etc2cache_half no device.
- 🔴 **Stutter no R36S = MURO DE RAM, SEM fix viavel em 480MB.** PROVADO: motor RenderWare do Bully (~700MB) nao cabe
  nos 480MB; build mobile teve o gerenciamento de memoria de streaming GUTADO (despejo sem ref-count -> nao libera).
  Confirmacao: **roda LISO no Mali-450 (832MB)** -> e RAM, nao GPU. Tentativas que FALHARAM (todas no muro):
  cap256(mais pesado, OOM), mlock(OOM no load), swappiness 10/200(stutter/OOM), BULLY_STREAM_MULT(IplStreamingDist=0),
  BULLY_EVICT(despejo gutado libera zero). 
- 🎯 **VEREDITO: o Bully e jogo de Mali-450 832MB+ / 1GB+ usavel** (la roda liso, e o port esta pronto+melhor com os
  fixes de hoje). No R36S 480MB nao da sem RE massiva (reconstruir o ref-count do streaming = incerto).
- **AMANHA, 2 caminhos:** (A) EMPACOTAR o port pra Mali-450/1GB+ (PortMaster zip) — caminho recomendado, o trabalho
  ja rende; (B) se insistir no R36S: RE do CStreaming pra reconstruir deletable-flags/ref-count e religar o despejo
  (BULLY_EVICT ja tem a estrutura do hook em jni_shim:779, falta a maquinaria de baixo) — dificil/incerto.
- **Device** (R36S, link-local): rodar o `r36s-net.sh` (docs internas) ANTES; `sshpass -p <senha> ssh
  root@<device-ip>`. Estado deixado: 0 bully, binario 5725d864 (=repo), launcher meia-res sem flags experimentais,
  swappiness 60. Caches no device: etc2cache(full 873M), etc2cache_half(meia-res, ATIVO), etc2cache_lo(cap256 orfao,
  pode apagar). ⚠️SEMPRE matar bully por /proc/*/exe (pkill-x deixa zumbi); NUNCA swapfile no SD (trava).
- **Flags do binario** (todas opt-in, OFF no launcher): BULLY_STUTTERLOG, BULLY_MLOCK_CODE/CAP_MB, BULLY_STREAM_MULT,
  BULLY_TEX_MAXDIM (cap extra na meia-res), BULLY_EVICT, BULLY_NO_HALF (desliga meia-res).

## ATUALIZACAO 2026-06-22 — BRANCO RESOLVIDO+CONFIRMADO + MEIA-RESOLUCAO (1GB)
- **Branco do gameplay RESOLVIDO** ("imagem apareceu" + `[fbo] ATTACH status=0x8cd5 OK`, INCOMPLETO=0).
  RAIZ REAL (≠ a do filtro mipmap, que foi pista falsa — incompleta dá PRETO): o **render-target da cena 3D**
  herdava `bully_cur_tex_path` STALE (setado só ao abrir `.tex` em jni_shim.c:580, NUNCA limpo) → casava o cache →
  `bully_try_etc2` subia ETC2 DENTRO do RT → FBO INCOMPLETO → 3D não compunha → branco chapado (HUD ok por cima).
  ES2/Mali-450 imune (upload ETC2 gated em `bully_is_es3()`). FIX: flag `g_path_fresh` (imports.c) — path só vale
  p/ a 1ª textura após `.tex`; cada criação consome a frescura; RT (sem `.tex`) → pf=0 → nunca ETC2 → FBO completo.
- **MEIA-RESOLUCAO p/ 1GB (em teste 2026-06-22):** modo inteligente — em RAM<1.3GB o jogo inteiro roda a 1/2 res
  (ETC2 meia-res) p/ o load caber sem OOM de GPU. `src/etc2_halve.c` (modo BULLY_HALVEBAKE): decodifica cada ETC2
  cheio → box 2x → re-encoda → `etc2cache_half/` (grandes ≥64 mult-8 halvam; pequenas COPIADAS = nada escondido).
  Runtime BULLY_TEX_HALFALL (imports.c my_glTexStorage2D/my_glTexImage2D): pede chave meia-res + aloca metade.
  RTs ficam full-res (pf=0). Launcher liga auto em <1.3GB + roda o halve-bake 1x. `bully_halfdim` (etc2_halve.c)
  = regra compartilhada runtime+bake. Binário hash p/ conferir no commit.

## METODO QUE FUNCIONOU (chegar no gameplay e printar por SSH no R36S)
- **Matar SEMPRE por `/proc/*/exe`** (pkill -x/-f NÃO casa — engine renomeia thread → deixa ZUMBI vivo comendo
  120MB RAM+20MB GPU = falso-OOM/branco; foi por isso que a sessão SSH OOMava e o device físico não).
- **NUNCA swapfile no SD** (faz thrash infinito em SD lento = device TRAVA, sshd morre; o stock 512 zram OOM-mata
  LIMPO e o device se recupera sozinho). Rodar em 1GB stock mesmo (480MB usável + 512 zram).
- Reboot limpo antes de testar (zera zumbi+GPU). Captura: `/dev/shm/bully_shot` em loop enquanto vivo (alive por
  /proc/exe). Durante o load do New Game o sshd fica lento (esperar/retry, não é trava).

## TL;DR
- O **título e o menu renderizam PERFEITO** com ETC2 (logo, Jimmy, diretor, fundo de quadrinhos — cores certas).
- O **gameplay ficava BRANCO** (personagens, pessoas, fundo). NÃO era textura faltando no cache
  (cache = 13785/~14064 .etc2, 873M, e renderiza certo). Era um **bug específico do path GLES3**.

## CAUSA-RAIZ (bug do GLES3, `src/imports.c` `my_glTexParameteri`)
No R36S (contexto ES3, kmsdrm) o motor cria textura via `glTexStorage2D(levels=9)` (MIPMAPADA — confirmado
no log: `[tex] STORAGE levels=9 ifmt=0x8056`). Nossa **emulação** (`my_glTexStorage2D`) aloca **SÓ o nível 0**
(a emulação não gera mips; ETC2 comprimido nem pode gerar). Depois o motor seta
`MIN_FILTER = LINEAR_MIPMAP_LINEAR` nas superfícies **opacas 3D** (personagens/mundo).

O `my_glTexParameteri` antigo só forçava filtro LINEAR quando `!kmsdrm` (ou etc1_force, ou textura com alpha).
No R36S **é kmsdrm** → o filtro mipmapado **passava** → textura com filtro mip mas **sem níveis de mip =
INCOMPLETA → GLES amostra BRANCO**. O título/UI usa filtro LINEAR (sem mip) → completa → renderiza certo.
**Por isso o ES2/Mali-450 funcionava** (lá `!kmsdrm` forçava LINEAR). O comentário antigo "kmsdrm: mips
gerados no glTexImage2D" só vale pro path `glTexImage2D`, NÃO pra emulação `glTexStorage2D` do R36S.

## FIX (implementado + buildado + deployado)
`src/imports.c`, em `my_glTexParameteri`: força `MIN_FILTER` → LINEAR (0x2601) para QUALQUER textura
**emulada** (`g_tex_emul[id]`), que só tem o nível 0. (Forward-decl de `g_tex_emul` adicionada perto da
linha 672, padrão igual ao `g_tex_alpha`.) Custo: opacas usam bilinear em vez de trilinear — exatamente o
que o Mali-450 já fazia e ficava bom.

- Build: `docker run --rm -v "$PWD":/repo -v "$SR":/sysroot:ro debian:bullseye bash /repo/build_compat_gcc.sh`
  com `SR=~/NextOS-Elite-Edition/build.NextOS-Retro-Elite-Edition-Amlogic-old.aarch64-4/toolchain/aarch64-libreelec-linux-gnu/sysroot`
  → `bully.compat.gcc` (GLIBC_2.17, 159688B). Hash do fix = `9f3e61a68716...`.
- Deploy: copiado pro device como `/storage/roms/ports/bully/bully` (hash confere). Backup do anterior =
  `bully.bak-prefiltro` (device) e `bully.compat.gcc.bak-prefiltro` (host).
- Device pronto: `.use_etc2cache` presente (ETC2 ON), nada rodando.

## VERIFICAR (pela ES)
Abrir o Bully na ES e ENTRAR no gameplay. Esperado: personagens/pessoas/mundo agora com **textura**
(não branco). Menu já confirmado sem regressão (continua perfeito com o fix).

## MURO QUE SOBRA: OOM de GPU Mali (~150-172MB) no load do mundo
Medido no dmesg: `mali ff400000.gpu: OOM notifier: tsk bully ... 129548 kB` (dev mali0 ~143-172MB, dividido
com sway/ES). NÃO é RAM de sistema (textura ETC2 viva = só ~66MB; live set cabe). É **memória de GPU** —
não swappável (swapfile de 3GB no SD foi adicionado em runtime e NÃO ajudou; pode remover com
`swapoff /storage/swapfile; rm /storage/swapfile`).
- Lançando por SSH eu **não consegui** alcançar uma cena 3D estável: o load do attract/New-Game estoura a
  GPU e mata o bully no menu (frames ~2500-7700, variável). **Baixar o cap NÃO ajuda** (cap=44 morreu antes
  que cap=64) → o estouro é das alocações do **próprio motor** (RTT + texturas pequenas não-paginadas + VBOs),
  que nosso paging não controla. O gameplay É alcançado no device físico (vimos os brancos lá) — provável
  diferença: launch pela ES é mais enxuto que a sessão SSH.
- Toggles novos no launcher (`Bully.sh`): `BULLY_PAGE_CAP_MB` (default 64, override por env),
  `BULLY_SYNC_PAGE=1` (re-upload síncrono, sem pop-in branco do async; default = async).

## Achados de paging (contexto)
- O motor NUNCA deleta textura (`del=7`) → sem paging acumula → OOM. Paging é necessário.
- Despejo escreve **preto** (1x1 RGB 0,0,0), não branco → o "branco" NÃO era despejo (era o filtro mip).
- `cap=120` → `pf=0 ev=0` (sem thrash) e o personagem do menu rendeiza COMPLETO; `cap=64` cortava
  (despejava o personagem). Mais cap = render melhor MAS OOM de GPU mais cedo.

## Como reverter o fix (se preciso)
Device: `cp bully.bak-prefiltro bully`. Host: `cp bully.compat.gcc.bak-prefiltro bully.compat.gcc`.
Fonte: reverter o bloco `g_tex_emul → fl=1` em `my_glTexParameteri` + a forward-decl.
