/*
 * text_render.c -- renderiza texto UTF-8 num buffer RGBA usando FreeType +
 * Roboto-Regular (a fonte default da plataforma Android que o Chrono Trigger
 * usa nas labels com fontName vazio). Serve o caminho nativo do jogo
 * (Cocos2dxBitmap.createTextBitmapShadowStroke -> nativeInitBitmapDC).
 */
#include <ft2build.h>
#include FT_FREETYPE_H
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include "util.h"

#define FONT_PATH "/storage/roms/ports/chrono/Roboto-Regular.ttf"

static FT_Library g_ft;
static FT_Face g_face;
static int g_ready = -1;

static int ensure_ft(void) {
  if (g_ready >= 0) return g_ready;
  g_ready = 0;
  if (FT_Init_FreeType(&g_ft)) { debugPrintf("text_render: FT_Init falhou\n"); return 0; }
  const char *p = getenv("CHRONO_FONT");
  if (!p) p = FONT_PATH;
  if (FT_New_Face(g_ft, p, 0, &g_face)) { debugPrintf("text_render: FT_New_Face('%s') falhou\n", p); return 0; }
  debugPrintf("text_render: fonte carregada '%s' glyphs=%ld\n", p, (long)g_face->num_glyphs);
  g_ready = 1;
  return 1;
}

/* decodifica 1 codepoint UTF-8; avanca *s */
static unsigned utf8_next(const char **s) {
  const unsigned char *p = (const unsigned char *)*s;
  unsigned c = *p++;
  if (c < 0x80) { }
  else if ((c >> 5) == 0x6) { c = ((c & 0x1F) << 6) | (*p++ & 0x3F); }
  else if ((c >> 4) == 0xE) { c = ((c & 0x0F) << 12); c |= (*p++ & 0x3F) << 6; c |= (*p++ & 0x3F); }
  else if ((c >> 3) == 0x1E) { c = ((c & 0x07) << 18); c |= (*p++ & 0x3F) << 12; c |= (*p++ & 0x3F) << 6; c |= (*p++ & 0x3F); }
  *s = (const char *)p;
  return c;
}

/* Renderiza 'utf8' tam 'px' cor (r,g,b). Devolve RGBA malloc'd (premult NAO),
 * preenche *outW/*outH. align: 0=left 1=center 2=right (afeta so layout no canvas).
 * Se reqW/reqH > 0, usa esse tamanho de canvas; senao calcula. Caller faz free. */
unsigned char *chrono_render_text(const char *utf8, int px, int r, int g, int b,
                                  int align, int reqW, int reqH,
                                  int *outW, int *outH) {
  if (!ensure_ft() || !utf8) return NULL;
  if (px < 6) px = 6; if (px > 200) px = 200;
  FT_Set_Pixel_Sizes(g_face, 0, px);
  int ascent = g_face->size->metrics.ascender >> 6;
  int descent = -(g_face->size->metrics.descender >> 6);
  int lineh = g_face->size->metrics.height >> 6;
  if (lineh < ascent + descent) lineh = ascent + descent;

  /* medir: largura por linha (\n quebra), numero de linhas */
  int maxw = 0, curw = 0, lines = 1;
  for (const char *s = utf8; *s;) {
    unsigned cp = utf8_next(&s);
    if (cp == '\n') { if (curw > maxw) maxw = curw; curw = 0; lines++; continue; }
    if (FT_Load_Char(g_face, cp, FT_LOAD_DEFAULT)) continue;
    curw += g_face->glyph->advance.x >> 6;
  }
  if (curw > maxw) maxw = curw;

  /* SEMPRE auto-dimensiona pelo conteudo: os reqW/reqH vindos do jogo sao
     pequenos/ruins (clipam o texto). O jogo posiciona o label pelo tamanho real. */
  (void)reqW; (void)reqH;
  int W = maxw + 2;
  int H = lines * lineh + 2;
  if (W < 1) W = 1; if (H < 1) H = 1;
  if (W > 2048) W = 2048; if (H > 2048) H = 2048;
  unsigned char *rgba = calloc((size_t)W * H * 4, 1);
  if (!rgba) return NULL;

  int line = 0;
  int pen_y = ascent;        /* baseline da 1a linha */
  int line_w0 = maxw;        /* largura da 1a linha p/ alinhar (simplif: usa maxw) */
  (void)line_w0;
  int pen_x = 0;
  /* alinhamento horizontal do bloco inteiro dentro do canvas */
  if (align == 1) pen_x = (W - maxw) / 2; else if (align == 2) pen_x = W - maxw;
  if (pen_x < 0) pen_x = 0;
  int start_x = pen_x;

  for (const char *s = utf8; *s;) {
    unsigned cp = utf8_next(&s);
    if (cp == '\n') { line++; pen_y = ascent + line * lineh; pen_x = start_x; continue; }
    if (FT_Load_Char(g_face, cp, FT_LOAD_RENDER)) continue;
    FT_GlyphSlot gl = g_face->glyph;
    FT_Bitmap *bm = &gl->bitmap;
    int gx = pen_x + gl->bitmap_left;
    int gy = pen_y - gl->bitmap_top;
    for (unsigned row = 0; row < bm->rows; row++) {
      int py = gy + (int)row;
      if (py < 0 || py >= H) continue;
      const unsigned char *src = bm->buffer + row * bm->pitch;
      for (unsigned col = 0; col < bm->width; col++) {
        int pxp = gx + (int)col;
        if (pxp < 0 || pxp >= W) continue;
        unsigned char a = src[col];
        if (!a) continue;
        unsigned char *dst = rgba + ((size_t)py * W + pxp) * 4;
        /* sobrepoe (max no alpha) */
        if (a >= dst[3]) { dst[0] = r; dst[1] = g; dst[2] = b; dst[3] = a; }
      }
    }
    pen_x += gl->advance.x >> 6;
  }

  if (getenv("CHRONO_TEXTLOG"))
    debugPrintf("render '%s' -> maxw=%d lines=%d W=%d H=%d px=%d\n", utf8, maxw, lines, W, H, px);
  if (outW) *outW = W;
  if (outH) *outH = H;
  return rgba;
}
