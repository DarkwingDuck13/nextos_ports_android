# NFS MW — TELA do LOGO/DISCLAIMER: RESOLVIDA 2026-06-15 (white-default)

Device **192.168.31.164** (subnet .31, senha nextos). Port em `~/nextos_ports_android/ports/nfs/`.
Build `./build.sh`. Deploy `scp nfs root@192.168.31.164:/storage/roms/nfs/nfs` (matar nfs antes).

## ✅ RESULTADO (commit desta sessão)
O disclaimer ("All experiences portrayed in this game are for entertainment purposes only.
© 2018 Electronic Arts Inc.") agora renderiza **LIMPO**: fundo BRANCO + texto cinza legível.
Sumiram a **caixa preta** à esquerda, as **formas-fantasma** nos cantos e os **smears** cinza
("tudo fora de ordem"). Validado por screenshot (glReadPixels→PIL). O **menu/mapa** (Easydrive/
Downtown) também renderiza ótimo e legível — **sem regressão**. Resta só um **quadradinho escuro**
no canto inf. direito = o spinner de loading REAL (textura indisponível, ver abaixo) — cosmético.

## 🔬 CAUSA-RAIZ (achada por RE, não chute)
Instrumentei `my_glBindTexture`/`my_glDrawElements` com return-address (NFS_BINDLOG) no disclaimer
(frames ~285-375). Dados:
- 3 draws tex=0 por frame: `prog=53 c=6`, `prog=53 c=12`, `prog=50 c=6`, todos do MESMO draw-site
  da engine (libapp.so off **0x569db8**, base 0xefd00000).
- A fn de bind da engine (off **0x56960c**) faz: `r5=*r5` (objeto de textura do sprite); se
  `r5==0` → `glBindTexture(target, 0)`; senão → `glBindTexture(target, r5[56])` (id real).
  → **2 dos 3 sprites têm objeto de textura NULL → engine liga textura 0** (o link sprite→atlas
  do .sba resolve null no port; PARTE 11). É a MESMA raiz do texto que quebra no menu.
- O texto do disclaimer NÃO depende disso: renderiza via font_shim (textura própria, tex≠0). Com
  o hack OFF o disclaimer fica PRETO menos o texto → confirma que fundo/spinner/decorações são
  TODOS draws tex=0.

## 🔑 O FIX (egl_shim.c, atlas_rebind)
Para draws texturizados com **tex=0** (UI, c<64), o fallback PADRÃO passou a ser uma **textura 1×1
BRANCA** (em vez do antigo "atlas-rebind" que ligava o ÚLTIMO atlas grande → lixo). Branco faz o
quad mostrar a **cor de vértice**:
- fundo do disclaimer (vértice branco) → BRANCO ✓
- sprites/decorações sem textura → cor sólida limpa (em vez de regiões aleatórias de atlas = lixo) ✓
- **seguro game-wide**: conteúdo COM textura (logos/menu, tex≠0) NÃO passa por aqui.
Envs: `NFS_ATLASHACK=1` restaura o modo atlas antigo; `NFS_NOATLASHACK=1` desliga tudo (deixa tex=0
→ preto). `NFS_BINDLOG=1` loga bind/draw tex=0 no disclaimer (diagnóstico de fontes p/ depois).

## ⏳ O quadradinho do spinner (cosmético, NÃO é tex=0)
O spinner é um draw com **textura REAL tex=7** = uma **página dinâmica 512×512** (texs 2-8
compartilham o buffer-staging vazio 0xebde2008, preenchidas via glTexSubImage2D). A região do
spinner em tex=7 nunca é preenchida no port → amostra preto → quadrado escuro. Não dá p/ esconder
SÓ ele sem arriscar as fontes (mesmas páginas dinâmicas). O "O" que aparecia antes era coincidência
do atlas-hack (uma decoração caía num anel do atlas). Deixado como está (loading ~2s).

## ➡️ PRÓXIMO: fontes do menu que quebram após ~4-5 seções (MESMA raiz)
Hipótese: ao evicção/Remove texture, o objeto de textura do glyph vira null (r5==0) → o draw do
glyph vira tex=0 → com white-default vira BLOCO BRANCO (antes: lixo de atlas). Investigar com
NFS_BINDLOG já no menu (gatear por frame mais alto) p/ ver se o glyph quebrado é r5==0 e POR QUE
(eviction? re-upload da página?). O fix real é manter/re-bindar a textura do glyph daquele draw.

## FERRAMENTAS
- Screenshot: `NFS_AUTOSHOT=1 NFS_SEQSHOT=1` → `seq_NNNN.raw` (a cada 30 frames) em /storage/roms/nfs.
  Decodificar local: `~/nfs-diag/decode.py seq_*.raw` (RGBA 1280x720 + flip). Disclaimer ≈ frame 360.
- RE: objdump ARM `~/NextOS-Elite-Edition/build*Amlogic-old*/toolchain/bin/armv8a-emuelec-linux-gnueabihf-objdump`
  (libapp.so é ARM, NÃO Thumb). libapp base no log = `so_load: load base` (a do range que contém o ra).
- ⚠️ MASCARAR emustation; vfat /storage/roms vira RO se martelado → `mount -o remount,rw /storage/roms`.
  /tmp é tmpfs 416MB — NÃO acumular .raw lá (puxar de /storage/roms/nfs direto).
