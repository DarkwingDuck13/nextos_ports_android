/*
 * text_render.c -- rasterizador de texto do sistema via freetype (plandroid).
 *
 * O engine chama FontDrawStringToImageFunc/FontWidth/FontHeight esperando o
 * comportamento do Android Paint: setTextSize(px) mede/desenha a string com
 * anti-alias; a cor e' chapada (r,g,b) e o alpha vem da cobertura do glifo.
 * Reproduzimos com freetype + Roboto (sans padrao do Android).
 */
#include "text_render.h"
#include "util.h"
#include <stdlib.h>
#include <string.h>
#include <ft2build.h>
#include FT_FREETYPE_H

static FT_Library g_ft;
static FT_Face g_face;
static int g_ready = -1;

static const char *font_path(void) {
  const char *e = getenv("SOM_FONT");
  if (e) return e;
  return "./Roboto-Regular.ttf"; /* cwd = GAMEDIR */
}

static int ensure_font(void) {
  if (g_ready >= 0) return g_ready;
  g_ready = 0;
  if (FT_Init_FreeType(&g_ft)) { debugPrintf("freetype: init falhou\n"); return 0; }
  if (FT_New_Face(g_ft, font_path(), 0, &g_face)) {
    debugPrintf("freetype: New_Face('%s') falhou\n", font_path());
    return 0;
  }
  g_ready = 1;
  debugPrintf("freetype: fonte carregada '%s'\n", font_path());
  return 1;
}

/* decodifica um code point UTF-8; avanca *p. */
static unsigned utf8_next(const char **p) {
  const unsigned char *s = (const unsigned char *)*p;
  unsigned c = *s++;
  if (c >= 0xF0 && (s[0]&0xC0)==0x80 && (s[1]&0xC0)==0x80 && (s[2]&0xC0)==0x80) {
    c = ((c&7)<<18)|((s[0]&0x3F)<<12)|((s[1]&0x3F)<<6)|(s[2]&0x3F); s += 3;
  } else if (c >= 0xE0 && (s[0]&0xC0)==0x80 && (s[1]&0xC0)==0x80) {
    c = ((c&0xF)<<12)|((s[0]&0x3F)<<6)|(s[1]&0x3F); s += 2;
  } else if (c >= 0xC0 && (s[0]&0xC0)==0x80) {
    c = ((c&0x1F)<<6)|(s[0]&0x3F); s += 1;
  }
  *p = (const char *)s;
  return c;
}

static void set_size(int px) {
  if (px < 1) px = 1;
  FT_Set_Pixel_Sizes(g_face, 0, px);
}

int som_text_width(const char *utf8, int size) {
  if (!utf8 || !ensure_font()) return 0;
  set_size(size);
  int w = 0;
  const char *p = utf8;
  while (*p) {
    unsigned cp = utf8_next(&p);
    if (FT_Load_Char(g_face, cp, FT_LOAD_DEFAULT)) continue;
    w += (int)(g_face->glyph->advance.x >> 6);
  }
  return w;
}

int som_text_height(int size) {
  if (!ensure_font()) return size;
  set_size(size);
  /* -top+bottom ~= altura de linha (ascender - descender em px) */
  int asc = (int)(g_face->size->metrics.ascender >> 6);
  int desc = (int)(g_face->size->metrics.descender >> 6);
  int h = asc - desc;
  return h > 0 ? h : size;
}

void som_text_draw(const char *utf8, int size, unsigned int *buf, int bufW,
                   int bufH, int dstX, int dstY, int r, int g, int b) {
  if (!utf8 || !buf || !ensure_font()) return;
  set_size(size);
  unsigned int rgb = ((r & 0xFF) << 16) | ((g & 0xFF) << 8) | (b & 0xFF);
  int baseline = dstY + (int)(g_face->size->metrics.ascender >> 6);
  int pen_x = dstX;
  const char *p = utf8;
  while (*p) {
    unsigned cp = utf8_next(&p);
    if (FT_Load_Char(g_face, cp, FT_LOAD_RENDER)) continue;
    FT_GlyphSlot gs = g_face->glyph;
    FT_Bitmap *bm = &gs->bitmap;
    int gx = pen_x + gs->bitmap_left;
    int gy = baseline - gs->bitmap_top;
    for (unsigned row = 0; row < bm->rows; row++) {
      int py = gy + (int)row;
      if (py < 0 || py >= bufH) continue;
      const unsigned char *src = bm->buffer + row * bm->pitch;
      for (unsigned col = 0; col < bm->width; col++) {
        int px = gx + (int)col;
        if (px < 0 || px >= bufW) continue;
        unsigned a = src[col];
        if (!a) continue;
        unsigned int *dst = &buf[py * bufW + px];
        unsigned da = (*dst >> 24) & 0xFF;
        unsigned na = a > da ? a : da; /* mantem o mais opaco */
        *dst = (na << 24) | rgb;
      }
    }
    pen_x += (int)(gs->advance.x >> 6);
  }
}
