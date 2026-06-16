# HANDOFF — BUG DAS FONTES DO MENU (NFS Most Wanted, Mali-450)

Device **192.168.31.164** (subnet .31, senha nextos). Port em `~/nextos_ports_android/ports/nfs/`.
Build `./build.sh`. Deploy `scp -O nfs root@192.168.31.164:/storage/roms/nfs/nfs` (matar nfs antes;
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
   o NextOS vê é o **diálogo pop-up**, não uma 2ª página de glyph.

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
intuição do NextOS "manter o bitmap grande, não reduzir" e "é cache" bate 100%.) O tamanho 512 é
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

## RESUMO DE 1 LINHA
Fonte do menu quebra = **eviction/re-pack do cache de glyph** (página tex=7 512² enche com muitos
char×tamanho → a engine realoca glyphs em células novas → o texto na tela mantém UVs antigas → glyph
errado). Atlas SEMPRE correto. Fix = aumentar a página de glyph (patch do 512 em GlyphBuffer::
AddTexturePage) p/ não encher/evictar.
