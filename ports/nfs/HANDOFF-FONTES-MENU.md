# HANDOFF — BUG DAS FONTES DO MENU (NFS Most Wanted, Mali-450)

Device **<device-ip>**. Port em `ports/nfs/` (neste repo).
Build `./build.sh`. Deploy `scp -O nfs root@<device-ip>:/storage/roms/nfs/nfs` (matar nfs antes;
`mount -o remount,rw /storage/roms` se o vfat ficar RO). Decoder local: `~/nfs-diag/decode.py`.

## 🎯 O BUG
No menu, depois de **abrir vários menus/diálogos em sequência**, TODO o texto fica garbled
(glyphs trocados/faltando). Ex.: "CONFIRM/BACK/MENU" vira "MCCEHT/EMI/TECT". Acontece quando um
**pop-up/diálogo** aparece (sair sim/não, loja online). Persiste depois (não pisca).

## ✅✅ DIAGNÓSTICO DEFINITIVO (sessão 2026-06-16 — com dados, refuta a teoria anterior)
**É EVICTION/RE-PACK DO CACHE DE GLYPH DA ENGINE.** O `im::GlyphBuffer` mantém UMA página-atlas
512×512 (tex=7). O menu usa MUITOS pares (caractere × tamanho) — tamanhos 18, 20.25, 22.5, 24.75,
27, 40.5, 49.5… cada (char,size) = uma célula. **>256 combinações enchem a página de 512** → a
engine **DESPEJA glyphs antigos e re-adiciona em células NOVAS**. O texto já posicionado na tela
mantém as UVs ANTIGAS (a célula onde o glyph ESTAVA) → agora aquela célula tem OUTRO glyph →
**garble**. O conteúdo do atlas está SEMPRE CORRETO; o erro é a UV apontar pra célula realocada.

### Provas (medidas, não chute):
1. **Conteúdo do atlas = nosso raster, EXATO.** `atlas_7 (512²) == abm[canto 512²] flipado-V`,
   MAD **0.0** (pixel-exact), inclusive no momento garbled. ⇒ rasterização e upload OK.
2. **`NFS_SKIPPAGE=1`** (suprime draws que amostram tex=7) → TODO o texto some (título+corpo) ⇒
   tudo vem da MESMA página tex=7 (conteúdo certo).
3. **UVs reais (via `NFS_UVLOG`, captura glBufferSubData)**: os quads de texto amostram células
   VÁLIDAS do atlas (dist 5–16px de glyphs reais) mas os glyphs ERRADOS p/ o texto pretendido.
   Escala da UV = célula/512 (correta, sem erro 2×); coords ≤ 512 (não estoura).
4. **RE-PACK CONFIRMADO**: o MESMO (glyph,size) é desenhado (`drawString`/`NFS_STRLOG`) em
   células DIFERENTES ao longo do tempo (ex.: 'E' size 27 em (217,23) E (55,350); size 24.75 em
   (28,158) E (55,446)). `drawString` só ocorre em cache-MISS ⇒ re-adição ⇒ eviction/repack.
5. **A página de glyph é SÓ tex=7 (512²).** As "páginas 2/3…" que minha heurística marcou (tex=28,
   30, 35…) são **atlases de SPRITE** (botão, spinner, vidro rachado), não glyph. A "página 2" que
   o porter vê é o **diálogo pop-up**, não uma 2ª página de glyph.

### ❌ REFUTADO (teorias antigas, com dados):
- "Página 2 do glyph amostra a página 1 errada" (handoff anterior): **NÃO** — só existe 1 página de
  glyph; o garble ocorre com tex=7 sozinho.
- "Stride/getInfo errado corrompe o upload": **NÃO** — atlas == abm pixel-exact.
- "Engine empacota em 1024 (getInfo) mas sobe 512": **NÃO** — coords de cache ≤512, UV escala /512.
- "Tamanho da página = getInfo/2": **NÃO** — `NFS_ABMBIG` (getInfo=2048) NÃO mudou a página (segue
  512). O 512 é fixo na engine, independente do getInfo. (Boot OK com 2048.)
- "Buffer compartilhado entre páginas": `getBitmap` retorna sempre o MESMO handle (0x47850,
  gfx=nil), mas como só há 1 página de glyph, não há colisão de buffer aqui.

## ➡️ O FIX (direção, alta confiança)
**Aumentar a página de glyph (512→1024) p/ ela NÃO encher → sem eviction → sem garble.** (A
intuição do porter "manter o bitmap grande, não reduzir" e "é cache" bate 100%.) O tamanho 512 é
**fixo no código da engine** (`im::GlyphBuffer::AddTexturePage`), NÃO derivado do nosso getInfo →
precisa de **PATCH BINÁRIO** na libapp.so OU achar como a engine escolhe 512.
- O upload da página passa pelo **uploader genérico em libapp `+0x565f60`** (achado via
  `NFS_GLYPHRA` = return-address do glTexImage2D 512²; ra=`+0x56607c`). Ele lê W/H dos campos do
  objeto-página (`[r7,#44]`=H, etc.) → o 512 é setado na CRIAÇÃO da página (AddTexturePage), ainda
  NÃO localizada. PRÓXIMO: achar `AddTexturePage` (xref da string `GlyphBuffer::AddTexturePage(`
  @vaddr 0x9e3b22, ou tracear quem cria o objeto-página com width=512 e chama o uploader) e patchar
  512→1024 (e a área de empacotamento/UV-denominador correspondente).
- Alternativa: investigar se a engine DEVERIA criar uma 2ª página de glyph quando a 1ª enche (em vez
  de evictar), e se algo no port impede (aí o fix seria habilitar a 2ª página com buffer próprio).
- Mitigação possível sem patch: reduzir o nº de (char,size) distintos — ex. SNAPAR tamanhos de
  fonte a poucos buckets no font_shim — mas muda layout (a engine usa nosso measureText) e é frágil.

## 🧪 INSTRUMENTAÇÃO (envs, tudo gated default-OFF — não afeta jogo normal)
- `NFS_PAGEHIST` — por janela de frames, histograma de qual textura os quads pequenos amostram
  (id, WxH, nº uploads, índice de página) + log `[gpage]` de páginas descobertas.
- `NFS_SKIPPAGE=<n>` — suprime draws que amostram a página de glyph idx≥n (teste visual decisivo).
- `NFS_UVLOG` — captura VBOs (hooks glBufferData/**glBufferSubData**/glMapBufferRange/Unmap) e
  dumpa pos+uv+texel por vértice dos batches de texto. Trigger por arquivo: `touch
  /storage/roms/nfs/uvon` libera ~12 dumps no frame. (A engine preenche os buffers via
  **glBufferSubData**, NÃO glBufferData nem glMapBuffer — maps=0.)
- `NFS_GLYPHRA` — return-address do upload da página de glyph (acha o código da engine).
- `NFS_GFXLOG` — loga o objeto BitmapGraphics em getBitmap/drawString (sempre gfx=nil, handle único).
- `NFS_ABMBIG` — getInfo reporta 2048 (teste do tamanho da página; NÃO muda a página = 512).
- `NFS_ATLASDUMP`/`NFS_ABMDUMP`/`NFS_STRLOG`/`NFS_TEXLOG` — dumps do atlas/scratch + drawStrings.

## 🔬 ARQUITETURA DO TEXTO (confirmada)
- A engine rasteriza glyph-a-glyph via `BitmapGraphics.drawString(paint, "X", x, y)` → nosso
  `font_draw` (stb_truetype) escreve no scratch `abm` (1024² compartilhado) na célula (x,y). A
  engine lê o canto 512² (via lockPixels), flipa-V e sobe como tex=7. Depois desenha 1 quad por
  glyph amostrando a célula. Vértice: a0=pos(vec3@0), a1=**UV(vec2@16)**, a2=cor(ubyte4@12),
  stride 32; via glDrawArrays (ebuf=0) com `first`.
- Strings da engine: `im::GlyphBuffer::{AddTexturePage,prepareGlyph,clearBufferedGlyphPage}`,
  `BufferedGlyph(Page)`, boost shared_ptr. Classe Java `com/ea/ironmonkey/BitmapGraphics`.

## 🔁 REPRODUÇÃO AUTÔNOMA
Injetor `moga.txt`: `echo <keycode> > /storage/roms/nfs/moga.txt` (1 por frame, ~0.6s entre).
Keycodes: 96=A 4=BACK 105=R2 103=R1 22=RIGHT 21=LEFT 20=DOWN 19=UP 108=START.
Launcher de debug (criar no device): exporta os NFS_* + `NFS_AUTOSHOT=1`, roda em background,
log em /tmp/live.log, screenshot em /tmp/auto.raw (decodificar com `~/nfs-diag/decode.py`).
Sequência que reproduz o garble: `for k in 96 20 96 4 20 96 4 105 96 20 96 4 105 96 22 96 4`.
⚠️ MASCARAR emustation ao testar (`systemctl mask`), restaurar ao fim (`unmask`+`start`).
⚠️ Screenshots/dumps vão p/ /tmp (tmpfs); NÃO martelar o vfat /storage/roms.

## 🛠️ TENTATIVA DE FIX (patch do tamanho da página) — RE feito, patch NÃO localizado ainda
Objetivo: 512→1024 na criação da página (`GlyphBuffer::AddTexturePage`). Mapa do código (via
`NFS_GLYPHRA` = scan da pilha no upload da página de glyph):
- **Uploader genérico libapp `0x565f60`** (sobe QUALQUER textura): lê **W=[pageobj+44], H=[pageobj+48]**,
  data=[pageobj? r9] e chama glTexImage2D. Logo o objeto-página guarda W/H em +44/+48 (512/512 p/ a
  página de glyph). ⇒ patchar o que SETA +44/+48 = 512 no construtor da página resolveria a alloc;
  packing/UV provavelmente leem os mesmos campos (a confirmar).
- **Cadeia de chamada do upload** (do mais baixo): `0x565e28`(chama uploader) ← `0x473a5c`(função
  0x473a3c = libera/clear glyphs bufferizados, refcount boost shared_ptr) ← `0x4684b4` ← `0x467ad8`
  (gerenciador do cache: null-checks + refcount de shared_ptr) ← `0x3eb720` ← `0x3d5984` ← `0x3f8688`.
- **NÃO localizado**: a instrução que cria a página com 512 (AddTexturePage). Becos: `#512` aparece
  **142×** no binário (ranges de rand, tamanhos de snprintf, particle math — todos falso-positivo);
  xref PIC da string `GlyphBuffer::AddTexturePage(` (@0x9e3b22) **não resolve** (ldr+add e movw/movt
  não batem — tabela de strings via registrador-base do GOT); par `str[+44]`/`str[+48]` tem **469
  ocorrências** (offsets genéricos). `getRefreshPageSize` existe mas parece paginação de REDE
  (string vizinha `/userlist?pageSize`), não glyph.
- **PRÓXIMO (caminho seguro p/ achar e patchar)**:
  1. **Hook/detour do uploader `0x565f60`** (trampolim via so_util, como os detours do dysmantle):
     capturar **r7 (ponteiro do objeto-página)** no upload de tex=7 512² → ter o endereço do objeto.
  2. Com o objeto em mãos: dump dos campos; achar quem ESCREVE 512 em [obj+44] (watchpoint gdb no
     device, OU varrer a engine por `str #512 → [obj+44]`); confirmar se packing/UV leem [obj+44].
  3. **Patch reversível**: ou (a) runtime-patch de [obj+44]/[+48] = 1024 logo após criação +
     forçar a textura GL 1024 (e abm≥1024, já é); ou (b) patch binário do construtor 512→1024 numa
     CÓPIA da libapp.so no device (backup → testar boot/render/garble → reverter se quebrar).
  ⚠️ Risco: se o 512 da página for usado como LITERAL separado no packing E na UV (não só no campo),
  precisa patchar os 3. Testar incrementalmente (boot → menu → forçar garble) com backup.
- Alternativa sem patch (frágil, NÃO recomendada): reduzir nº de (char,size) distintos snapando
  tamanhos no font_shim — muda layout (engine usa nosso measureText) e a engine chaveia pelo
  PRÓPRIO size, então não reduz a pressão do cache dela. Descartada.

## 🛠️ PROGRESSO DO PATCH (sessão 2026-06-16, parte 2) — AddTexturePage LOCALIZADO, detour OK
- **`GlyphBuffer::AddTexturePage` = libapp `+0x565b78`** (ARM). Achado via `NFS_GENRA` (hook de
  glGenTextures grava o RA de criação por id; tex=7 → genRA=`+0x565bd8`, dentro dessa função).
- **Detour funcionando** (trampolim `hook_arm`, igual ao dysmantle/getfspath): `my_addtexpage` em
  imports.c, instalado por `nfs_install_addtexpage_hook` (gated `NFS_ATPLOG`/`NFS_BIGPAGE`).
  **Boot OK, sem crash, reversível** (é no shim).
- **A página de glyph é distinguível**: descriptor `a0` (r0) tem **+52==3** (TODOS os outros atlases
  — boot 510×1003, sprites, 256² — têm +52==2). Objeto descriptor é ÚNICO (reusado; +4 = contador
  de geração que sobe a cada re-add).
- **MAS não achei o lever do TAMANHO**: patchei `a0[+24]/[+28]` (W/H do descriptor) → a engine
  SOBRESCREVE (são saída, textura seguiu 512). Patchei `a1[+8]/[+12]` (=512/512) → a textura TAMBÉM
  seguiu 512: `a1` é um **rect de origem de blit** {x=0,y=0,w=512,h=512}, não o tamanho de
  alocação. O tamanho real da página (textura + buffer CPU 1MB[+60] + packing) é criado via uma
  **chamada VIRTUAL** dentro de AddTexturePage (`ldr r1,[r0,#28]; blx r1` em 0x565c8c), a partir de
  um campo que ainda não pinei — provavelmente do **GlyphBuffer MANAGER (o caller)**, não do a0/a1.
- **PRÓXIMO**: (a) detour do **caller** de AddTexturePage (achar quem passa a0/a1) p/ pegar o campo
  "page size" do manager e patchar 512→1024 ANTES de criar a 1ª página; OU (b) capturar o objeto-
  textura (r7 no uploader 0x565f60, [r7+44]=W=512) e rastrear quem escreve 512 ali. ⚠️ resize tem
  que ser COORDENADO (textura+buffer CPU+packing juntos) senão glyphs além de 512 se perdem.
- Hooks novos (gated): `NFS_GENRA` (glGenTextures RA), `NFS_ATPLOG` (loga campos de AddTexturePage),
  `NFS_BIGPAGE`+`NFS_PAGEDIM` (patcha o tamanho — ainda não eficaz, pendente achar o campo certo).

## RESUMO DE 1 LINHA
Fonte do menu quebra = **eviction/re-pack do cache de glyph** (página tex=7 512² enche com muitos
char×tamanho → a engine realoca glyphs em células novas → o texto na tela mantém UVs antigas → glyph
errado). Atlas SEMPRE correto. Fix = aumentar a página de glyph (patch do 512 em GlyphBuffer::
AddTexturePage) p/ não encher/evictar.
